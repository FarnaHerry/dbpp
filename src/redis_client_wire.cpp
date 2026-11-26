module;

#include <huxerui/datatable.h>

#include <cerrno>
#include <cstring>

module dbpp.redis;

import std;
import dbpp.types;
import dbpp.i18n;

namespace dbpp {

namespace {

constexpr int kMaxRespDepth = 64;
constexpr std::int64_t kMaxRespBulkBytes = 64 * 1024 * 1024;
constexpr std::int64_t kMaxRespArrayElements = 1024 * 1024;

std::string LowerAscii(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const unsigned char c : text) {
    out.push_back(static_cast<char>(std::tolower(c)));
  }
  return out;
}

std::string UpperAscii(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const unsigned char c : text) {
    out.push_back(static_cast<char>(std::toupper(c)));
  }
  return out;
}

std::string_view TrimAscii(std::string_view text) {
  while (!text.empty() &&
         (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' ||
          text.front() == '\n')) {
    text.remove_prefix(1);
  }
  while (!text.empty() &&
         (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' ||
          text.back() == '\n')) {
    text.remove_suffix(1);
  }
  return text;
}

bool EqualsIgnoreCase(std::string_view left, std::string_view right) {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t i = 0; i < left.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(left[i])) !=
        std::tolower(static_cast<unsigned char>(right[i]))) {
      return false;
    }
  }
  return true;
}

std::optional<std::int64_t> ParseStrictInt64(std::string_view text) {
  text = TrimAscii(text);
  if (text.empty()) {
    return std::nullopt;
  }
  std::int64_t val = 0;
  const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), val);
  if (ec != std::errc{} || ptr != text.data() + text.size()) {
    return std::nullopt;
  }
  return val;
}

int HexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

std::string PercentDecode(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '%' && i + 2 < text.size()) {
      const int hi = HexDigit(text[i + 1]);
      const int lo = HexDigit(text[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    out.push_back(text[i]);
  }
  return out;
}

bool FindCrLf(std::string_view buffer, std::size_t pos, std::size_t& line_end) {
  const std::size_t found = buffer.find("\r\n", pos);
  if (found == std::string_view::npos) {
    return false;
  }
  line_end = found;
  return true;
}

bool ParseRespValueImpl(std::string_view buffer, std::size_t& cursor, int depth,
                        RedisValue& out, std::string& error) {
  if (depth > kMaxRespDepth) {
    error = Tr("RESP nesting too deep", "RESP 嵌套层级过深");
    return false;
  }
  if (cursor >= buffer.size()) {
    return false;
  }

  const char prefix = buffer[cursor];
  std::size_t line_end = 0;
  if (!FindCrLf(buffer, cursor + 1, line_end)) {
    return false;
  }
  const std::string_view line = buffer.substr(cursor + 1, line_end - (cursor + 1));

  switch (prefix) {
    case '+': {
      out = RedisValue{};
      out.kind = RedisValueKind::SimpleString;
      out.str = std::string(line);
      cursor = line_end + 2;
      return true;
    }
    case '-': {
      out = RedisValue{};
      out.kind = RedisValueKind::Error;
      out.str = std::string(line);
      cursor = line_end + 2;
      return true;
    }
    case ':': {
      const auto parsed = ParseStrictInt64(line);
      if (!parsed.has_value()) {
        error = TrFmt("Invalid RESP integer: {0}", "无效的 RESP 整数: {0}", {line});
        return false;
      }
      out = RedisValue{};
      out.kind = RedisValueKind::Integer;
      out.integer = *parsed;
      cursor = line_end + 2;
      return true;
    }
    case '(': {
      out = RedisValue{};
      out.kind = RedisValueKind::SimpleString;
      out.str = std::string(line);
      cursor = line_end + 2;
      return true;
    }
    case '_': {
      out = RedisValue{};
      out.kind = RedisValueKind::Null;
      cursor = line_end + 2;
      return true;
    }
    case '#': {
      if (line == "t" || line == "T") {
        out = RedisValue{};
        out.kind = RedisValueKind::Bool;
        out.boolean = true;
        cursor = line_end + 2;
        return true;
      }
      if (line == "f" || line == "F") {
        out = RedisValue{};
        out.kind = RedisValueKind::Bool;
        out.boolean = false;
        cursor = line_end + 2;
        return true;
      }
      error = TrFmt("Invalid RESP3 boolean: {0}", "无效的 RESP3 布尔值: {0}", {line});
      return false;
    }
    case ',': {
      out = RedisValue{};
      out.kind = RedisValueKind::Double;
      out.str = std::string(line);
      const std::string lower = LowerAscii(line);
      if (lower == "inf" || lower == "+inf") {
        out.number = INFINITY;
      } else if (lower == "-inf") {
        out.number = -INFINITY;
      } else if (lower == "nan") {
        out.number = NAN;
      } else {
        std::string copy(line);
        errno = 0;
        char* end = nullptr;
        const double val = std::strtod(copy.c_str(), &end);
        if (errno == ERANGE || end != copy.c_str() + copy.size()) {
          error = TrFmt("Invalid RESP3 double: {0}", "无效的 RESP3 浮点数: {0}", {line});
          return false;
        }
        out.number = val;
      }
      cursor = line_end + 2;
      return true;
    }
    case '$':
    case '!':
    case '=': {
      const auto parsed_len = ParseStrictInt64(line);
      if (!parsed_len.has_value()) {
        error = TrFmt("Invalid RESP string length: {0}", "无效的 RESP 字符串长度: {0}", {line});
        return false;
      }
      if (*parsed_len < 0) {
        if (*parsed_len != -1) {
          error = TrFmt("Negative RESP string length: {0}", "非法的 RESP 字符串长度: {0}", {line});
          return false;
        }
        out = RedisValue{};
        out.kind = RedisValueKind::Null;
        cursor = line_end + 2;
        return true;
      }
      if (*parsed_len > kMaxRespBulkBytes) {
        error = Tr("RESP bulk payload exceeds 64MB limit", "RESP Bulk 数据超过 64MB 限制");
        return false;
      }
      const std::size_t payload_len = static_cast<std::size_t>(*parsed_len);
      const std::size_t payload_start = line_end + 2;
      if (buffer.size() - payload_start < payload_len + 2) {
        return false;
      }
      if (buffer[payload_start + payload_len] != '\r' ||
          buffer[payload_start + payload_len + 1] != '\n') {
        error = Tr("RESP bulk payload missing CRLF terminator", "RESP Bulk 缺少 CRLF 结束符");
        return false;
      }
      std::string_view payload = buffer.substr(payload_start, payload_len);
      if (prefix == '=' && payload.size() >= 4 && payload[3] == ':') {
        payload.remove_prefix(4);
      }
      out = RedisValue{};
      out.kind = prefix == '!' ? RedisValueKind::Error : RedisValueKind::BulkString;
      out.str = std::string(payload);
      cursor = payload_start + payload_len + 2;
      return true;
    }
    case '*':
    case '~':
    case '>':
    case '%': {
      const auto parsed_count = ParseStrictInt64(line);
      if (!parsed_count.has_value()) {
        error = TrFmt("Invalid RESP array length: {0}", "无效的 RESP 数组长度: {0}", {line});
        return false;
      }
      if (*parsed_count < 0) {
        if (*parsed_count != -1) {
          error = TrFmt("Negative RESP array length: {0}", "非法的 RESP 数组长度: {0}", {line});
          return false;
        }
        out = RedisValue{};
        out.kind = RedisValueKind::Null;
        cursor = line_end + 2;
        return true;
      }
      if (*parsed_count > kMaxRespArrayElements) {
        error = Tr("Too many elements in RESP array", "RESP 数组元素数量过多");
        return false;
      }
      std::size_t next = line_end + 2;
      const std::size_t item_count =
          static_cast<std::size_t>(*parsed_count) * (prefix == '%' ? 2U : 1U);
      out = RedisValue{};
      out.kind = RedisValueKind::Array;
      out.is_map = (prefix == '%');
      out.elements.reserve(item_count);
      for (std::size_t i = 0; i < item_count; ++i) {
        RedisValue elem;
        if (!ParseRespValueImpl(buffer, next, depth + 1, elem, error)) {
          return false;
        }
        out.elements.push_back(std::move(elem));
      }
      cursor = next;
      return true;
    }
    default: {
      // Handle inline reply fallback if a server/proxy sends unexpected text.
      error = TrFmt("Unsupported RESP reply prefix '{0}'", "不支持的 RESP 响应前缀 '{0}'",
                    {std::string{prefix}});
      return false;
    }
  }
}

} // namespace

std::string RedisValue::ToDisplayString() const {
  switch (kind) {
    case RedisValueKind::SimpleString:
    case RedisValueKind::Error:
    case RedisValueKind::BulkString: return str;
    case RedisValueKind::Integer: return std::to_string(integer);
    case RedisValueKind::Bool: return boolean ? "true" : "false";
    case RedisValueKind::Double: return str;
    case RedisValueKind::Null: return "NULL";
    case RedisValueKind::Array: {
      if (is_map) {
        return detail::RedisFormatJsonHashPairs(elements, elements.size() / 2);
      }
      return detail::RedisFormatJsonArrayOfStrings(elements, elements.size());
    }
  }
  return {};
}

std::optional<int> ParseRedisDbName(std::string_view name) {
  name = TrimAscii(name);
  if (name.empty()) {
    return std::nullopt;
  }
  const std::string lower = LowerAscii(name);
  std::string_view digits = lower;
  if (digits.starts_with("db")) {
    digits.remove_prefix(2);
  }
  const auto parsed = ParseStrictInt64(digits);
  if (!parsed.has_value() || *parsed < 0 || *parsed > 1000000) {
    return std::nullopt;
  }
  return static_cast<int>(*parsed);
}

RedisParseUriResult ParseRedisUri(std::string_view uri, std::string_view username_override,
                                  std::string_view password_override) {
  RedisParseUriResult res;
  std::string_view work = TrimAscii(uri);
  if (work.empty()) {
    res.error = Tr("Redis connection URI cannot be empty", "Redis 连接地址不能为空");
    return res;
  }

  const std::size_t scheme_pos = work.find("://");
  if (scheme_pos != std::string_view::npos) {
    const std::string scheme = LowerAscii(work.substr(0, scheme_pos));
    if (scheme == "rediss") {
      res.endpoint.tls = true;
      res.error = Tr(
          "TLS rediss:// connections are not supported yet; use redis:// or tunnel to a local port via SSH",
          "暂不支持 TLS 加密的 rediss:// 连接；请使用 redis:// 或通过 SSH 隧道映射到本地端口");
      return res;
    }
    if (scheme != "redis") {
      res.error = TrFmt("Invalid Redis URI scheme '{0}://' (only redis:// is supported)",
                        "无效的 Redis URI 协议 '{0}://'（仅支持 redis://）", {scheme});
      return res;
    }
    work.remove_prefix(scheme_pos + 3);
  }

  std::string_view authority = work;
  std::string_view path_and_query;
  if (const std::size_t slash_or_q = work.find_first_of("/?");
      slash_or_q != std::string_view::npos) {
    authority = work.substr(0, slash_or_q);
    path_and_query = work.substr(slash_or_q);
  }

  if (const std::size_t at = authority.rfind('@'); at != std::string_view::npos) {
    const std::string_view userinfo = authority.substr(0, at);
    authority = authority.substr(at + 1);
    const std::size_t colon = userinfo.find(':');
    if (colon == std::string_view::npos) {
      res.endpoint.password = PercentDecode(userinfo);
    } else {
      res.endpoint.username = PercentDecode(userinfo.substr(0, colon));
      res.endpoint.password = PercentDecode(userinfo.substr(colon + 1));
    }
  }

  if (!username_override.empty()) {
    res.endpoint.username = std::string(username_override);
  }
  if (!password_override.empty()) {
    res.endpoint.password = std::string(password_override);
  }

  authority = TrimAscii(authority);
  if (authority.empty()) {
    res.error = Tr("Redis URI missing host", "Redis URI 缺少主机名");
    return res;
  }

  std::string_view host_part = authority;
  std::string_view port_part;
  if (authority.front() == '[') {
    const std::size_t close = authority.find(']');
    if (close == std::string_view::npos) {
      res.error = Tr("Invalid IPv6 address", "无效的 IPv6 地址");
      return res;
    }
    host_part = authority.substr(1, close - 1);
    const std::string_view rest = authority.substr(close + 1);
    if (!rest.empty()) {
      if (rest.front() != ':') {
        res.error = Tr("Invalid Redis host:port format", "无效的 Redis 主机端口格式");
        return res;
      }
      port_part = rest.substr(1);
    }
  } else {
    const std::size_t colon = authority.rfind(':');
    if (colon != std::string_view::npos) {
      host_part = authority.substr(0, colon);
      port_part = authority.substr(colon + 1);
    }
  }

  if (host_part.empty()) {
    res.error = Tr("Redis URI missing host", "Redis URI 缺少主机名");
    return res;
  }
  res.endpoint.host = std::string(host_part);

  if (!port_part.empty()) {
    const auto port_num = ParseStrictInt64(port_part);
    if (!port_num.has_value() || *port_num < 1 || *port_num > 65535) {
      res.error = TrFmt("Invalid Redis port: {0}", "无效的 Redis 端口: {0}", {port_part});
      return res;
    }
    res.endpoint.port = static_cast<std::uint16_t>(*port_num);
  }

  std::string_view path = path_and_query;
  std::string_view query;
  if (const std::size_t q = path_and_query.find('?'); q != std::string_view::npos) {
    path = path_and_query.substr(0, q);
    query = path_and_query.substr(q + 1);
  }

  if (path.starts_with('/')) {
    path.remove_prefix(1);
  }
  path = TrimAscii(path);
  if (!path.empty()) {
    if (const std::size_t extra_slash = path.find('/'); extra_slash != std::string_view::npos) {
      path = path.substr(0, extra_slash);
    }
    const auto db_num = ParseRedisDbName(path);
    if (!db_num.has_value()) {
      res.error = TrFmt("Invalid Redis database index: {0}", "无效的 Redis 数据库编号: {0}",
                        {path});
      return res;
    }
    res.endpoint.db = *db_num;
  }

  std::size_t qpos = 0;
  while (qpos < query.size()) {
    const std::size_t amp = query.find('&', qpos);
    const std::string_view kv =
        amp == std::string_view::npos ? query.substr(qpos) : query.substr(qpos, amp - qpos);
    qpos = amp == std::string_view::npos ? query.size() : amp + 1;
    const std::size_t eq = kv.find('=');
    if (eq == std::string_view::npos) {
      continue;
    }
    const std::string k = LowerAscii(kv.substr(0, eq));
    const std::string v = PercentDecode(kv.substr(eq + 1));
    if (k == "db" && !v.empty()) {
      const auto db_num = ParseRedisDbName(v);
      if (!db_num.has_value()) {
        res.error = TrFmt("Invalid Redis db parameter: {0}", "无效的 Redis db 参数: {0}", {v});
        return res;
      }
      res.endpoint.db = *db_num;
    }
  }

  return res;
}

std::string FormatRedisDisplayName(std::string_view uri) {
  const RedisParseUriResult parsed = ParseRedisUri(uri);
  if (!parsed.error.empty() && !parsed.endpoint.tls) {
    return std::string(TrimAscii(uri));
  }
  std::string host = parsed.endpoint.host;
  if (host.empty()) {
    return "Redis";
  }
  if (host.find(':') != std::string::npos) {
    host = "[" + host + "]";
  }
  return host + ":" + std::to_string(parsed.endpoint.port) + "/" +
         std::to_string(parsed.endpoint.db);
}

std::string EncodeRespCommand(const std::vector<std::string>& args) {
  std::string out = "*" + std::to_string(args.size()) + "\r\n";
  for (const std::string& arg : args) {
    out += "$" + std::to_string(arg.size()) + "\r\n";
    out.append(arg);
    out += "\r\n";
  }
  return out;
}

bool TryParseRespValue(std::string_view buffer, std::size_t& consumed, RedisValue& out,
                       std::string& error) {
  consumed = 0;
  error.clear();
  std::size_t cursor = 0;
  if (!ParseRespValueImpl(buffer, cursor, 0, out, error)) {
    return false;
  }
  consumed = cursor;
  return true;
}

RedisCommandParseResult ParseRedisCommandLine(std::string_view line) {
  RedisCommandParseResult res;
  std::size_t i = 0;
  const std::size_t n = line.size();

  auto skip_ws = [&] {
    while (i < n && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r' || line[i] == '\n')) {
      ++i;
    }
  };

  skip_ws();
  if (i < n && line[i] == '#') {
    return res;
  }

  while (i < n) {
    skip_ws();
    if (i >= n) {
      break;
    }
    if (line[i] == '#') {
      break;
    }
    std::string token;
    const char quote = line[i];
    if (quote == '"' || quote == '\'') {
      ++i;
      bool closed = false;
      while (i < n) {
        const char c = line[i++];
        if (c == quote) {
          closed = true;
          break;
        }
        if (c == '\\' && quote == '"' && i < n) {
          const char esc = line[i++];
          switch (esc) {
            case 'n': token.push_back('\n'); break;
            case 'r': token.push_back('\r'); break;
            case 't': token.push_back('\t'); break;
            case '\\': token.push_back('\\'); break;
            case '"': token.push_back('"'); break;
            case '\'': token.push_back('\''); break;
            case 'x': {
              if (i + 1 < n) {
                const int hi = HexDigit(line[i]);
                const int lo = HexDigit(line[i + 1]);
                if (hi >= 0 && lo >= 0) {
                  token.push_back(static_cast<char>((hi << 4) | lo));
                  i += 2;
                  break;
                }
              }
              token.push_back('x');
              break;
            }
            default: token.push_back(esc); break;
          }
          continue;
        }
        if (c == '\\' && quote == '\'' && i < n && (line[i] == '\'' || line[i] == '\\')) {
          token.push_back(line[i++]);
          continue;
        }
        token.push_back(c);
      }
      if (!closed) {
        res.error = Tr("Command string missing closing quote", "命令字符串缺少结束引号");
        res.args.clear();
        return res;
      }
      res.args.push_back(std::move(token));
      continue;
    }

    while (i < n && line[i] != ' ' && line[i] != '\t' && line[i] != '\r' && line[i] != '\n') {
      if (line[i] == '"' || line[i] == '\'') {
        const char inner_quote = line[i++];
        bool closed = false;
        while (i < n) {
          const char c = line[i++];
          if (c == inner_quote) {
            closed = true;
            break;
          }
          if (c == '\\' && inner_quote == '"' && i < n) {
            const char esc = line[i++];
            switch (esc) {
              case 'n': token.push_back('\n'); break;
              case 'r': token.push_back('\r'); break;
              case 't': token.push_back('\t'); break;
              case '\\': token.push_back('\\'); break;
              case '"': token.push_back('"'); break;
              default: token.push_back(esc); break;
            }
            continue;
          }
          token.push_back(c);
        }
        if (!closed) {
          res.error = Tr("Command string missing closing quote", "命令字符串缺少结束引号");
          res.args.clear();
          return res;
        }
        continue;
      }
      token.push_back(line[i++]);
    }
    res.args.push_back(std::move(token));
  }

  return res;
}

std::vector<std::string> SplitRedisScript(std::string_view script) {
  std::vector<std::string> cmds;
  std::size_t line_start = 0;
  char in_quote = '\0';
  for (std::size_t i = 0; i <= script.size(); ++i) {
    const bool at_end = (i == script.size());
    const char c = at_end ? '\n' : script[i];
    if (!at_end) {
      if (in_quote != '\0') {
        if (c == '\\' && in_quote == '"' && i + 1 < script.size()) {
          ++i;
          continue;
        }
        if (c == in_quote) {
          in_quote = '\0';
        }
        continue;
      }
      if (c == '"' || c == '\'') {
        in_quote = c;
        continue;
      }
    }
    if (c == '\n' || at_end) {
      const std::string_view piece = TrimAscii(script.substr(line_start, i - line_start));
      if (!piece.empty() && !piece.starts_with('#')) {
        cmds.emplace_back(piece);
      }
      line_start = i + 1;
    }
  }
  return cmds;
}

bool IsRedisReadCommand(std::string_view command_text) {
  const std::vector<std::string> lines = SplitRedisScript(command_text);
  if (lines.empty()) {
    return false;
  }

  static const std::unordered_set<std::string_view> kReadCommands = {
      "PING",       "ECHO",       "GET",       "MGET",       "STRLEN",     "GETRANGE",
      "SUBSTR",     "GETBIT",     "BITCOUNT",  "BITPOS",     "EXISTS",     "TYPE",
      "TTL",        "PTTL",       "EXPIRETIME","PEXPIRETIME","KEYS",       "SCAN",
      "DBSIZE",     "INFO",       "TIME",      "RANDOMKEY",  "DUMP",       "HGET",
      "HGETALL",    "HMGET",      "HKEYS",     "HVALS",      "HLEN",       "HEXISTS",
      "HSTRLEN",    "HSCAN",      "HRANDFIELD","LRANGE",     "LLEN",       "LINDEX",
      "LPOS",       "SMEMBERS",   "SCARD",     "SISMEMBER",  "SMISMEMBER", "SRANDMEMBER",
      "SSCAN",      "SINTER",     "SUNION",    "SDIFF",      "ZRANGE",     "ZREVRANGE",
      "ZRANGEBYSCORE", "ZREVRANGEBYSCORE", "ZRANGEBYLEX", "ZREVRANGEBYLEX",
      "ZCARD",      "ZSCORE",     "ZMSCORE",   "ZRANK",      "ZREVRANK",   "ZCOUNT",
      "ZLEXCOUNT",  "ZSCAN",      "ZRANDMEMBER","XLEN",      "XRANGE",     "XREVRANGE",
      "PFCOUNT",    "GEOPOS",     "GEODIST",   "GEOHASH",    "GEOSEARCH",  "GEORADIUS_RO",
      "COMMAND",    "ROLE",       "LASTSAVE",  "HELLO",      "SELECT",
  };
  for (const std::string& line : lines) {
    const RedisCommandParseResult parsed = ParseRedisCommandLine(line);
    if (!parsed.error.empty() || parsed.args.empty()) {
      return false;
    }
    const std::string cmd = UpperAscii(parsed.args[0]);
    const std::string sub = parsed.args.size() > 1 ? UpperAscii(parsed.args[1]) : std::string{};
    bool line_is_read = kReadCommands.contains(cmd);
    if (!line_is_read) {
      if (cmd == "CONFIG" && sub == "GET") line_is_read = true;
      else if (cmd == "CLIENT" &&
               (sub == "LIST" || sub == "INFO" || sub == "ID" || sub == "GETNAME")) {
        line_is_read = true;
      } else if (cmd == "MEMORY" && (sub == "USAGE" || sub == "STATS" || sub == "DOCTOR")) {
        line_is_read = true;
      } else if (cmd == "OBJECT" &&
                 (sub == "ENCODING" || sub == "IDLETIME" || sub == "FREQ" || sub == "REFCOUNT")) {
        line_is_read = true;
      } else if (cmd == "ACL" &&
                 (sub == "LIST" || sub == "USERS" || sub == "WHOAMI" || sub == "CAT")) {
        line_is_read = true;
      } else if (cmd == "CLUSTER" && (sub == "INFO" || sub == "NODES" || sub == "SLOTS")) {
        line_is_read = true;
      } else if (cmd == "XINFO" && (sub == "STREAM" || sub == "GROUPS" || sub == "CONSUMERS")) {
        line_is_read = true;
      }
    }
    if (!line_is_read) {
      return false;
    }
  }
  return true;
}

namespace detail {

std::string RedisLowerAscii(std::string_view text) { return LowerAscii(text); }
std::string RedisUpperAscii(std::string_view text) { return UpperAscii(text); }
std::string_view RedisTrimAscii(std::string_view text) { return TrimAscii(text); }
bool RedisEqualsIgnoreCase(std::string_view left, std::string_view right) {
  return EqualsIgnoreCase(left, right);
}
std::optional<std::int64_t> RedisParseStrictInt64(std::string_view text) {
  return ParseStrictInt64(text);
}
int RedisHexDigit(char c) { return HexDigit(c); }
std::string RedisPercentDecode(std::string_view text) { return PercentDecode(text); }

} // namespace detail

} // namespace dbpp
