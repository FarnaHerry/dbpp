#include "redis_client.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace dbpp {

namespace {

constexpr int kConnectTimeoutMs = 5000;
constexpr int kSocketIoTimeoutMs = 15000;
constexpr std::size_t kMaxScanKeys = 10000;
constexpr std::size_t kMaxCollectionPreview = 200;
constexpr std::size_t kMaxListZsetPreview = 100;
constexpr std::size_t kMaxStreamPreview = 20;
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

// ---- Platform Socket Helpers ----------------------------------------------

#if defined(_WIN32)
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;

void EnsureWinSockInitialized() {
  struct WsaInit {
    WsaInit() {
      WSADATA data{};
      (void)WSAStartup(MAKEWORD(2, 2), &data);
    }
  };
  static WsaInit init;
}

void CloseSocketHandle(SocketHandle s) {
  if (s != kInvalidSocket) {
    closesocket(s);
  }
}

std::string WideToUtf8(const wchar_t* wide) {
  if (wide == nullptr || *wide == L'\0') {
    return {};
  }
  const int utf8_len =
      WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
  if (utf8_len <= 1) {
    return {};
  }
  std::string out(static_cast<std::size_t>(utf8_len - 1), '\0');
  if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), utf8_len, nullptr, nullptr) <= 0) {
    return {};
  }
  return out;
}

std::string LastSocketErrorText(int err_code = 0) {
  const int code = err_code != 0 ? err_code : WSAGetLastError();
  wchar_t* msg = nullptr;
  FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                     FORMAT_MESSAGE_IGNORE_INSERTS,
                 nullptr, static_cast<DWORD>(code), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                 reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
  std::string converted = WideToUtf8(msg);
  if (msg != nullptr) {
    LocalFree(msg);
  }
  const std::string_view trimmed = TrimAscii(converted);
  return trimmed.empty() ? ("WSA error " + std::to_string(code)) : std::string(trimmed);
}

std::string GaiErrorText(int err_code) {
  std::string converted = WideToUtf8(gai_strerrorW(err_code));
  const std::string_view trimmed = TrimAscii(converted);
  return trimmed.empty() ? ("getaddrinfo error " + std::to_string(err_code)) : std::string(trimmed);
}

bool SetNonBlocking(SocketHandle s, bool non_blocking) {
  u_long mode = non_blocking ? 1UL : 0UL;
  return ioctlsocket(s, FIONBIO, &mode) == 0;
}

void ApplySocketTimeouts(SocketHandle s, int timeout_ms) {
  const DWORD ms = static_cast<DWORD>(timeout_ms);
  (void)setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
  (void)setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
}

bool WaitConnectReady(SocketHandle s, int timeout_ms, std::string& error_out) {
  WSAPOLLFD pfd{};
  pfd.fd = s;
  pfd.events = POLLWRNORM;
  const int rc = WSAPoll(&pfd, 1, timeout_ms);
  if (rc == 0) {
    error_out = "连接超时";
    return false;
  }
  if (rc < 0) {
    error_out = LastSocketErrorText();
    return false;
  }
  int so_err = 0;
  int len = sizeof(so_err);
  if (getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_err), &len) != 0) {
    error_out = LastSocketErrorText();
    return false;
  }
  if (so_err != 0) {
    error_out = LastSocketErrorText(so_err);
    return false;
  }
  return true;
}

bool IsWouldBlockError(int code) {
  return code == WSAEWOULDBLOCK || code == WSAEINPROGRESS;
}

bool IsTimedOutError(int code) {
  return code == WSAETIMEDOUT;
}

int CurrentSocketError() {
  return WSAGetLastError();
}

#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;

void EnsureWinSockInitialized() {}

void CloseSocketHandle(SocketHandle s) {
  if (s >= 0) {
    close(s);
  }
}

std::string LastSocketErrorText(int err_code = 0) {
  const int code = err_code != 0 ? err_code : errno;
  return std::strerror(code);
}

std::string GaiErrorText(int err_code) {
  const char* msg = gai_strerror(err_code);
  return (msg != nullptr && *msg != '\0') ? std::string(msg)
                                          : ("getaddrinfo error " + std::to_string(err_code));
}

bool SetNonBlocking(SocketHandle s, bool non_blocking) {
  const int flags = fcntl(s, F_GETFL, 0);
  if (flags < 0) {
    return false;
  }
  const int next = non_blocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
  return fcntl(s, F_SETFL, next) == 0;
}

void ApplySocketTimeouts(SocketHandle s, int timeout_ms) {
  struct timeval tv {};
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = static_cast<suseconds_t>((timeout_ms % 1000) * 1000);
  (void)setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  (void)setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#if defined(SO_NOSIGPIPE)
  const int on = 1;
  (void)setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
}

bool WaitConnectReady(SocketHandle s, int timeout_ms, std::string& error_out) {
  struct pollfd pfd {};
  pfd.fd = s;
  pfd.events = POLLOUT;
  int rc = 0;
  do {
    rc = poll(&pfd, 1, timeout_ms);
  } while (rc < 0 && errno == EINTR);
  if (rc == 0) {
    error_out = "连接超时";
    return false;
  }
  if (rc < 0) {
    error_out = LastSocketErrorText();
    return false;
  }
  int so_err = 0;
  socklen_t len = sizeof(so_err);
  if (getsockopt(s, SOL_SOCKET, SO_ERROR, &so_err, &len) != 0) {
    error_out = LastSocketErrorText();
    return false;
  }
  if (so_err != 0) {
    error_out = LastSocketErrorText(so_err);
    return false;
  }
  return true;
}

bool IsWouldBlockError(int code) {
  return code == EINPROGRESS || code == EWOULDBLOCK || code == EAGAIN;
}

bool IsTimedOutError(int code) {
  return code == EAGAIN || code == EWOULDBLOCK || code == ETIMEDOUT;
}

int CurrentSocketError() {
  return errno;
}
#endif

// ---- RESP2 / RESP3 Wire Helpers -------------------------------------------

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
    error = "RESP 嵌套层级过深";
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
        error = "无效的 RESP 整数: " + std::string(line);
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
      error = "无效的 RESP3 布尔值: " + std::string(line);
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
          error = "无效的 RESP3 浮点数: " + std::string(line);
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
        error = "无效的 RESP 字符串长度: " + std::string(line);
        return false;
      }
      if (*parsed_len < 0) {
        if (*parsed_len != -1) {
          error = "非法的 RESP 字符串长度: " + std::string(line);
          return false;
        }
        out = RedisValue{};
        out.kind = RedisValueKind::Null;
        cursor = line_end + 2;
        return true;
      }
      if (*parsed_len > kMaxRespBulkBytes) {
        error = "RESP Bulk 数据超过 64MB 限制";
        return false;
      }
      const std::size_t payload_len = static_cast<std::size_t>(*parsed_len);
      const std::size_t payload_start = line_end + 2;
      if (buffer.size() - payload_start < payload_len + 2) {
        return false;
      }
      if (buffer[payload_start + payload_len] != '\r' ||
          buffer[payload_start + payload_len + 1] != '\n') {
        error = "RESP Bulk 缺少 CRLF 结束符";
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
        error = "无效的 RESP 数组长度: " + std::string(line);
        return false;
      }
      if (*parsed_count < 0) {
        if (*parsed_count != -1) {
          error = "非法的 RESP 数组长度: " + std::string(line);
          return false;
        }
        out = RedisValue{};
        out.kind = RedisValueKind::Null;
        cursor = line_end + 2;
        return true;
      }
      if (*parsed_count > kMaxRespArrayElements) {
        error = "RESP 数组元素数量过多";
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
      error = std::string("不支持的 RESP 响应前缀 '") + prefix + "'";
      return false;
    }
  }
}

// ---- Minimal Self-Contained JSON Formatter & Parser -----------------------

void AppendJsonEscapedString(std::string& out, std::string_view text) {
  static constexpr char kHex[] = "0123456789abcdef";
  out.push_back('"');
  for (const unsigned char uc : text) {
    switch (uc) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (uc < 0x20U) {
          out += "\\u00";
          out.push_back(kHex[(uc >> 4) & 0x0FU]);
          out.push_back(kHex[uc & 0x0FU]);
        } else {
          out.push_back(static_cast<char>(uc));
        }
        break;
    }
  }
  out.push_back('"');
}

struct JsonValue {
  enum class Kind { Null, Bool, Number, String, Array, Object };
  Kind kind = Kind::Null;
  bool bool_val = false;
  double number_val = 0.0;
  std::string str_val;
  std::vector<JsonValue> array_val;
  std::vector<std::pair<std::string, JsonValue>> object_val;

  [[nodiscard]] std::string AsScalarString() const {
    switch (kind) {
      case Kind::Null: return "null";
      case Kind::Bool: return bool_val ? "true" : "false";
      case Kind::Number:
      case Kind::String: return str_val;
      case Kind::Array:
      case Kind::Object: return str_val;
    }
    return {};
  }
};

class JsonParser {
public:
  explicit JsonParser(std::string_view input) : text_(input) {}

  bool Parse(JsonValue& out, std::string& error_out) {
    SkipWs();
    if (!ParseValue(out, error_out, 0)) {
      return false;
    }
    SkipWs();
    if (pos_ != text_.size()) {
      error_out = "JSON 尾部存在多余字符";
      return false;
    }
    return true;
  }

private:
  void SkipWs() {
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
        ++pos_;
      } else {
        break;
      }
    }
  }

  static void AppendUtf8Codepoint(std::string& out, std::uint32_t cp) {
    if (cp <= 0x7FU) {
      out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FFU) {
      out.push_back(static_cast<char>(0xC0U | ((cp >> 6) & 0x1FU)));
      out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    } else if (cp <= 0xFFFFU) {
      out.push_back(static_cast<char>(0xE0U | ((cp >> 12) & 0x0FU)));
      out.push_back(static_cast<char>(0x80U | ((cp >> 6) & 0x3FU)));
      out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    } else {
      out.push_back(static_cast<char>(0xF0U | ((cp >> 18) & 0x07U)));
      out.push_back(static_cast<char>(0x80U | ((cp >> 12) & 0x3FU)));
      out.push_back(static_cast<char>(0x80U | ((cp >> 6) & 0x3FU)));
      out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    }
  }

  bool ParseString(std::string& out, std::string& error_out) {
    if (pos_ >= text_.size() || text_[pos_] != '"') {
      error_out = "期望 JSON 字符串";
      return false;
    }
    ++pos_;
    out.clear();
    while (pos_ < text_.size()) {
      const char c = text_[pos_++];
      if (c == '"') {
        return true;
      }
      if (static_cast<unsigned char>(c) < 0x20U) {
        error_out = "JSON 字符串包含未转义控制字符";
        return false;
      }
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (pos_ >= text_.size()) {
        error_out = "JSON 转义序列未结束";
        return false;
      }
      const char esc = text_[pos_++];
      switch (esc) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          if (pos_ + 4 > text_.size()) {
            error_out = "JSON \\u 转义长度不足";
            return false;
          }
          std::uint32_t cp = 0;
          for (int k = 0; k < 4; ++k) {
            const int d = HexDigit(text_[pos_++]);
            if (d < 0) {
              error_out = "无效的 JSON \\u 十六进制字符";
              return false;
            }
            cp = (cp << 4) | static_cast<std::uint32_t>(d);
          }
          if (cp >= 0xD800U && cp <= 0xDBFFU && pos_ + 6 <= text_.size() &&
              text_[pos_] == '\\' && text_[pos_ + 1] == 'u') {
            std::uint32_t low = 0;
            bool ok = true;
            for (int k = 0; k < 4; ++k) {
              const int d = HexDigit(text_[pos_ + 2 + static_cast<std::size_t>(k)]);
              if (d < 0) {
                ok = false;
                break;
              }
              low = (low << 4) | static_cast<std::uint32_t>(d);
            }
            if (ok && low >= 0xDC00U && low <= 0xDFFFU) {
              pos_ += 6;
              cp = 0x10000U + (((cp - 0xD800U) << 10U) | (low - 0xDC00U));
            }
          }
          AppendUtf8Codepoint(out, cp);
          break;
        }
        default:
          error_out = std::string("不支持的 JSON 转义 \\") + esc;
          return false;
      }
    }
    error_out = "JSON 字符串缺少结束引号";
    return false;
  }

  bool ParseValue(JsonValue& out, std::string& error_out, int depth) {
    if (depth > 32) {
      error_out = "JSON 嵌套层级过深";
      return false;
    }
    SkipWs();
    if (pos_ >= text_.size()) {
      error_out = "JSON 内容意外结束";
      return false;
    }
    const char c = text_[pos_];
    if (c == '"') {
      out = JsonValue{};
      out.kind = JsonValue::Kind::String;
      return ParseString(out.str_val, error_out);
    }
    if (c == '{') {
      ++pos_;
      out = JsonValue{};
      out.kind = JsonValue::Kind::Object;
      const std::size_t obj_start = pos_ - 1;
      SkipWs();
      if (pos_ < text_.size() && text_[pos_] == '}') {
        ++pos_;
        out.str_val = std::string(text_.substr(obj_start, pos_ - obj_start));
        return true;
      }
      for (;;) {
        SkipWs();
        std::string key;
        if (!ParseString(key, error_out)) {
          return false;
        }
        SkipWs();
        if (pos_ >= text_.size() || text_[pos_] != ':') {
          error_out = "JSON 对象缺少 ':'";
          return false;
        }
        ++pos_;
        JsonValue val;
        if (!ParseValue(val, error_out, depth + 1)) {
          return false;
        }
        out.object_val.emplace_back(std::move(key), std::move(val));
        SkipWs();
        if (pos_ >= text_.size()) {
          error_out = "JSON 对象缺少 '}'";
          return false;
        }
        if (text_[pos_] == '}') {
          ++pos_;
          out.str_val = std::string(text_.substr(obj_start, pos_ - obj_start));
          return true;
        }
        if (text_[pos_] != ',') {
          error_out = "JSON 对象成员间缺少 ','";
          return false;
        }
        ++pos_;
      }
    }
    if (c == '[') {
      ++pos_;
      out = JsonValue{};
      out.kind = JsonValue::Kind::Array;
      const std::size_t arr_start = pos_ - 1;
      SkipWs();
      if (pos_ < text_.size() && text_[pos_] == ']') {
        ++pos_;
        out.str_val = std::string(text_.substr(arr_start, pos_ - arr_start));
        return true;
      }
      for (;;) {
        JsonValue elem;
        if (!ParseValue(elem, error_out, depth + 1)) {
          return false;
        }
        out.array_val.push_back(std::move(elem));
        SkipWs();
        if (pos_ >= text_.size()) {
          error_out = "JSON 数组缺少 ']'";
          return false;
        }
        if (text_[pos_] == ']') {
          ++pos_;
          out.str_val = std::string(text_.substr(arr_start, pos_ - arr_start));
          return true;
        }
        if (text_[pos_] != ',') {
          error_out = "JSON 数组元素间缺少 ','";
          return false;
        }
        ++pos_;
      }
    }
    if (text_.substr(pos_).starts_with("true")) {
      pos_ += 4;
      out = JsonValue{};
      out.kind = JsonValue::Kind::Bool;
      out.bool_val = true;
      out.str_val = "true";
      return true;
    }
    if (text_.substr(pos_).starts_with("false")) {
      pos_ += 5;
      out = JsonValue{};
      out.kind = JsonValue::Kind::Bool;
      out.bool_val = false;
      out.str_val = "false";
      return true;
    }
    if (text_.substr(pos_).starts_with("null")) {
      pos_ += 4;
      out = JsonValue{};
      out.kind = JsonValue::Kind::Null;
      out.str_val = "null";
      return true;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
      const std::size_t start = pos_;
      if (text_[pos_] == '-') {
        ++pos_;
      }
      if (pos_ >= text_.size() || !std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
        error_out = "无效的 JSON 数字";
        return false;
      }
      while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
        ++pos_;
      }
      if (pos_ < text_.size() && text_[pos_] == '.') {
        ++pos_;
        if (pos_ >= text_.size() || !std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
          error_out = "无效的 JSON 小数";
          return false;
        }
        while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
          ++pos_;
        }
      }
      if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
        ++pos_;
        if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) {
          ++pos_;
        }
        if (pos_ >= text_.size() || !std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
          error_out = "无效的 JSON 指数";
          return false;
        }
        while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
          ++pos_;
        }
      }
      const std::string raw(text_.substr(start, pos_ - start));
      errno = 0;
      char* end = nullptr;
      const double d = std::strtod(raw.c_str(), &end);
      if (errno == ERANGE || end != raw.c_str() + raw.size() || !std::isfinite(d)) {
        error_out = "JSON 数字超出范围: " + raw;
        return false;
      }
      out = JsonValue{};
      out.kind = JsonValue::Kind::Number;
      out.number_val = d;
      out.str_val = raw;
      return true;
    }

    error_out = std::string("无效的 JSON token '") + c + "'";
    return false;
  }

  std::string_view text_;
  std::size_t pos_ = 0;
};

std::string FormatJsonArrayOfStrings(const std::vector<RedisValue>& elements, std::size_t cap) {
  std::string out = "[";
  const std::size_t count = std::min(elements.size(), cap);
  for (std::size_t i = 0; i < count; ++i) {
    if (i > 0) {
      out.push_back(',');
    }
    if (elements[i].IsNull()) {
      out += "null";
    } else {
      AppendJsonEscapedString(out, elements[i].ToDisplayString());
    }
  }
  out.push_back(']');
  if (elements.size() > cap) {
    out += " /* +" + std::to_string(elements.size() - cap) + " */";
  }
  return out;
}

std::string FormatJsonHashPairs(const std::vector<RedisValue>& elements, std::size_t max_pairs) {
  std::string out = "{";
  std::size_t pairs = 0;
  for (std::size_t i = 0; i + 1 < elements.size(); i += 2) {
    if (pairs >= max_pairs) {
      break;
    }
    if (pairs > 0) {
      out.push_back(',');
    }
    AppendJsonEscapedString(out, elements[i].ToDisplayString());
    out.push_back(':');
    if (elements[i + 1].IsNull()) {
      out += "null";
    } else {
      AppendJsonEscapedString(out, elements[i + 1].ToDisplayString());
    }
    ++pairs;
  }
  out.push_back('}');
  return out;
}

std::string FormatJsonZsetPairs(const std::vector<RedisValue>& elements, std::size_t max_pairs) {
  std::string out = "[";
  std::size_t pairs = 0;
  for (std::size_t i = 0; i + 1 < elements.size(); i += 2) {
    if (pairs >= max_pairs) {
      break;
    }
    if (pairs > 0) {
      out.push_back(',');
    }
    out += "{\"member\":";
    AppendJsonEscapedString(out, elements[i].ToDisplayString());
    out += ",\"score\":";
    const std::string score_text = elements[i + 1].ToDisplayString();
    // Emit numeric score unquoted when valid JSON number; fall back to quoted string.
    std::string copy(score_text);
    char* end = nullptr;
    errno = 0;
    const double d = std::strtod(copy.c_str(), &end);
    if (!score_text.empty() && errno != ERANGE && end == copy.c_str() + copy.size() &&
        std::isfinite(d)) {
      out += score_text;
    } else {
      AppendJsonEscapedString(out, score_text);
    }
    out.push_back('}');
    ++pairs;
  }
  out.push_back(']');
  return out;
}

std::string FormatJsonStreamEntries(const std::vector<RedisValue>& elements) {
  std::string out = "[";
  for (std::size_t i = 0; i < elements.size(); ++i) {
    if (i > 0) {
      out.push_back(',');
    }
    const RedisValue& entry = elements[i];
    if (entry.kind == RedisValueKind::Array && entry.elements.size() >= 2) {
      out += "{\"id\":";
      AppendJsonEscapedString(out, entry.elements[0].ToDisplayString());
      out += ",\"fields\":";
      out += FormatJsonHashPairs(entry.elements[1].elements, kMaxCollectionPreview);
      out.push_back('}');
    } else {
      AppendJsonEscapedString(out, entry.ToDisplayString());
    }
  }
  out.push_back(']');
  return out;
}

// ---- Key / Value / Info Helpers -------------------------------------------

std::map<int, std::int64_t> ParseInfoKeyspace(std::string_view info_text) {
  std::map<int, std::int64_t> counts;
  std::size_t pos = 0;
  while (pos < info_text.size()) {
    const std::size_t nl = info_text.find('\n', pos);
    const std::string_view raw_line =
        nl == std::string_view::npos ? info_text.substr(pos) : info_text.substr(pos, nl - pos);
    pos = nl == std::string_view::npos ? info_text.size() : nl + 1;

    const std::string_view line = TrimAscii(raw_line);
    if (line.empty() || line.front() == '#') {
      continue;
    }
    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos) {
      continue;
    }
    const std::optional<int> db_idx = ParseRedisDbName(line.substr(0, colon));
    if (!db_idx.has_value()) {
      continue;
    }
    const std::string_view metrics = line.substr(colon + 1);
    const std::size_t keys_pos = metrics.find("keys=");
    if (keys_pos == std::string_view::npos) {
      continue;
    }
    const std::size_t val_start = keys_pos + 5;
    const std::size_t comma = metrics.find(',', val_start);
    const std::string_view count_sv = comma == std::string_view::npos
                                          ? metrics.substr(val_start)
                                          : metrics.substr(val_start, comma - val_start);
    if (const auto parsed = ParseStrictInt64(count_sv)) {
      counts[*db_idx] = *parsed;
    }
  }
  return counts;
}

CellValue TextCell(std::string text) {
  return CellValue{.text = std::move(text), .is_null = false};
}

CellValue NullCell() {
  return CellValue{.text = "NULL", .is_null = true};
}

CellValue ReplyToCell(const RedisValue& val) {
  if (val.IsNull()) {
    return NullCell();
  }
  return TextCell(val.ToDisplayString());
}

bool ValidateWritableKeyType(std::string_view type_text, std::string& normalized_out,
                             std::string& error_out) {
  const std::string lower = LowerAscii(TrimAscii(type_text));
  if (lower.empty() || lower == "string") {
    normalized_out = "string";
    return true;
  }
  if (lower == "hash" || lower == "list" || lower == "set" || lower == "zset") {
    normalized_out = lower;
    return true;
  }
  error_out = "不支持的 Redis 类型 '" + std::string(type_text) +
              "'（可选: string, hash, list, set, zset）";
  return false;
}

struct ParsedWriteSpec {
  std::string key;
  std::string type;
  std::optional<std::int64_t> ttl;
  std::vector<std::string> args;
};

bool BuildWriteCommandArgs(std::string_view type, std::string_view value_text,
                           std::vector<std::string>& args_out, std::string& error_out) {
  args_out.clear();
  if (type == "string") {
    args_out.push_back(std::string(value_text));
    return true;
  }
  JsonValue parsed;
  if (!JsonParser(value_text).Parse(parsed, error_out)) {
    error_out = std::string(type) + " 类型的值必须为合法 JSON（" + error_out + "）";
    return false;
  }
  if (type == "hash") {
    if (parsed.kind != JsonValue::Kind::Object || parsed.object_val.empty()) {
      error_out = "hash 值必须为非空 JSON 对象，例如 {\"field\":\"value\"}";
      return false;
    }
    for (const auto& [field, val] : parsed.object_val) {
      args_out.push_back(field);
      args_out.push_back(val.AsScalarString());
    }
    return true;
  }
  if (type == "list" || type == "set") {
    if (parsed.kind != JsonValue::Kind::Array || parsed.array_val.empty()) {
      error_out = std::string(type) + " 值必须为非空 JSON 数组，例如 [\"a\",\"b\"]";
      return false;
    }
    for (const JsonValue& elem : parsed.array_val) {
      args_out.push_back(elem.AsScalarString());
    }
    return true;
  }
  if (type == "zset") {
    if (parsed.kind != JsonValue::Kind::Array || parsed.array_val.empty()) {
      error_out = "zset 值必须为非空 JSON 数组，例如 [{\"member\":\"a\",\"score\":1}]";
      return false;
    }
    for (const JsonValue& elem : parsed.array_val) {
      if (elem.kind != JsonValue::Kind::Object) {
        error_out = "zset 数组元素必须为包含 member 与 score 的对象";
        return false;
      }
      const JsonValue* member = nullptr;
      const JsonValue* score = nullptr;
      for (const auto& [k, v] : elem.object_val) {
        if (k == "member") member = &v;
        if (k == "score") score = &v;
      }
      if (member == nullptr || score == nullptr) {
        error_out = "zset 每个元素都需要 member 和 score 字段";
        return false;
      }
      if (score->kind != JsonValue::Kind::Number && score->kind != JsonValue::Kind::String) {
        error_out = "zset 的 score 必须为数字";
        return false;
      }
      std::string score_str = score->AsScalarString();
      char* end = nullptr;
      errno = 0;
      const double d = std::strtod(score_str.c_str(), &end);
      if (score_str.empty() || errno == ERANGE || end != score_str.c_str() + score_str.size() ||
          !std::isfinite(d)) {
        error_out = "zset 的 score 不是有效数字: " + score_str;
        return false;
      }
      args_out.push_back(std::move(score_str));
      args_out.push_back(member->AsScalarString());
    }
    return true;
  }
  error_out = "暂不支持写入类型: " + std::string(type);
  return false;
}

bool WriteTypedRedisKey(RedisClient& client, std::string_view key, std::string_view type,
                        const std::vector<std::string>& args, std::optional<std::int64_t> ttl_sec,
                        bool replace_existing, std::optional<std::int64_t> preserve_pttl_ms,
                        std::string& error_out) {
  if (type == "string") {
    const std::string& val = args.empty() ? std::string{} : args[0];
    std::vector<std::string> cmd = {"SET", std::string(key), val};
    if (preserve_pttl_ms.has_value() && *preserve_pttl_ms > 0) {
      cmd.emplace_back("KEEPTTL");
    }
    RedisValue reply;
    if (!client.Command(cmd, reply, error_out)) {
      return false;
    }
    if (reply.IsError()) {
      if (cmd.size() > 3 && LowerAscii(reply.str).find("syntax") != std::string::npos) {
        // Fallback for very old Redis versions without KEEPTTL.
        RedisValue r2;
        if (!client.Command({"SET", std::string(key), val}, r2, error_out)) {
          return false;
        }
        if (r2.IsError()) {
          error_out = r2.str;
          return false;
        }
        RedisValue r3;
        (void)client.Command({"PEXPIRE", std::string(key), std::to_string(*preserve_pttl_ms)},
                             r3, error_out);
      } else {
        error_out = reply.str;
        return false;
      }
    }
    if (ttl_sec.has_value() && *ttl_sec > 0) {
      RedisValue exp_reply;
      if (!client.Command({"EXPIRE", std::string(key), std::to_string(*ttl_sec)}, exp_reply,
                          error_out)) {
        return false;
      }
      if (exp_reply.IsError()) {
        error_out = exp_reply.str;
        return false;
      }
    }
    return true;
  }

  std::vector<std::vector<std::string>> batch;
  if (replace_existing) {
    batch.push_back({"DEL", std::string(key)});
  }
  std::vector<std::string> write_cmd;
  if (type == "hash") {
    write_cmd.emplace_back("HSET");
  } else if (type == "list") {
    write_cmd.emplace_back("RPUSH");
  } else if (type == "set") {
    write_cmd.emplace_back("SADD");
  } else if (type == "zset") {
    write_cmd.emplace_back("ZADD");
  } else {
    error_out = "不支持的类型: " + std::string(type);
    return false;
  }
  write_cmd.emplace_back(key);
  write_cmd.insert(write_cmd.end(), args.begin(), args.end());
  batch.push_back(std::move(write_cmd));

  if (ttl_sec.has_value() && *ttl_sec > 0) {
    batch.push_back({"EXPIRE", std::string(key), std::to_string(*ttl_sec)});
  } else if (preserve_pttl_ms.has_value() && *preserve_pttl_ms > 0) {
    batch.push_back({"PEXPIRE", std::string(key), std::to_string(*preserve_pttl_ms)});
  }

  std::vector<RedisValue> replies;
  if (!client.Pipeline(batch, replies, error_out)) {
    return false;
  }
  for (const RedisValue& r : replies) {
    if (r.IsError()) {
      error_out = r.str;
      return false;
    }
  }
  return true;
}

bool ParseInfoIntoRows(std::string_view info_text, std::vector<GridRow>& rows_out) {
  std::string section = "general";
  std::size_t pos = 0;
  while (pos < info_text.size()) {
    const std::size_t nl = info_text.find('\n', pos);
    const std::string_view raw_line =
        nl == std::string_view::npos ? info_text.substr(pos) : info_text.substr(pos, nl - pos);
    pos = nl == std::string_view::npos ? info_text.size() : nl + 1;

    const std::string_view line = TrimAscii(raw_line);
    if (line.empty()) {
      continue;
    }
    if (line.front() == '#') {
      section = std::string(TrimAscii(line.substr(1)));
      continue;
    }
    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos) {
      continue;
    }
    GridRow row;
    row.values = {
        TextCell(section),
        TextCell(std::string(line.substr(0, colon))),
        TextCell(std::string(line.substr(colon + 1))),
    };
    rows_out.push_back(std::move(row));
  }
  return !rows_out.empty();
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
        return FormatJsonHashPairs(elements, elements.size() / 2);
      }
      return FormatJsonArrayOfStrings(elements, elements.size());
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
    res.error = "Redis 连接地址不能为空";
    return res;
  }

  const std::size_t scheme_pos = work.find("://");
  if (scheme_pos != std::string_view::npos) {
    const std::string scheme = LowerAscii(work.substr(0, scheme_pos));
    if (scheme == "rediss") {
      res.endpoint.tls = true;
      res.error =
          "暂不支持 TLS 加密的 rediss:// 连接；请使用 redis:// 或通过 SSH 隧道映射到本地端口";
      return res;
    }
    if (scheme != "redis") {
      res.error = "无效的 Redis URI 协议 '" + scheme + "://'（仅支持 redis://）";
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
    res.error = "Redis URI 缺少主机名";
    return res;
  }

  std::string_view host_part = authority;
  std::string_view port_part;
  if (authority.front() == '[') {
    const std::size_t close = authority.find(']');
    if (close == std::string_view::npos) {
      res.error = "无效的 IPv6 地址";
      return res;
    }
    host_part = authority.substr(1, close - 1);
    const std::string_view rest = authority.substr(close + 1);
    if (!rest.empty()) {
      if (rest.front() != ':') {
        res.error = "无效的 Redis 主机端口格式";
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
    res.error = "Redis URI 缺少主机名";
    return res;
  }
  res.endpoint.host = std::string(host_part);

  if (!port_part.empty()) {
    const auto port_num = ParseStrictInt64(port_part);
    if (!port_num.has_value() || *port_num < 1 || *port_num > 65535) {
      res.error = "无效的 Redis 端口: " + std::string(port_part);
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
      res.error = "无效的 Redis 数据库编号: " + std::string(path);
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
        res.error = "无效的 Redis db 参数: " + v;
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
        res.error = "命令字符串缺少结束引号";
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
          res.error = "命令字符串缺少结束引号";
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

// ---- RedisClient Transport & Execution -------------------------------------

RedisClient::~RedisClient() {
  Disconnect();
}

bool RedisClient::IsConnected() const {
  return static_cast<SocketHandle>(sock_) != kInvalidSocket;
}

void RedisClient::Disconnect() {
  const SocketHandle handle = static_cast<SocketHandle>(sock_);
  if (handle != kInvalidSocket) {
    CloseSocketHandle(handle);
#if defined(_WIN32)
    sock_ = static_cast<std::uintptr_t>(kInvalidSocket);
#else
    sock_ = kInvalidSocket;
#endif
  }
  recv_buf_.clear();
}

bool RedisClient::Connect(const RedisEndpoint& endpoint, std::string& error_out) {
  Disconnect();
  endpoint_ = endpoint;
  current_db_ = std::max(0, endpoint.db);
  return OpenSocketAndHandshake(error_out);
}

bool RedisClient::OpenSocketAndHandshake(std::string& error_out) {
  Disconnect();
  EnsureWinSockInitialized();

  struct addrinfo hints {};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  const std::string port_str = std::to_string(endpoint_.port);
  struct addrinfo* res = nullptr;
  const int gai = getaddrinfo(endpoint_.host.c_str(), port_str.c_str(), &hints, &res);
  if (gai != 0 || res == nullptr) {
    error_out = "解析主机失败 (" + endpoint_.host + "): " + GaiErrorText(gai);
    return false;
  }

  SocketHandle connected = kInvalidSocket;
  std::string last_err = "无法连接到 Redis 服务器";
  for (struct addrinfo* p = res; p != nullptr; p = p->ai_next) {
    const SocketHandle fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (fd == kInvalidSocket) {
      last_err = LastSocketErrorText();
      continue;
    }

    (void)SetNonBlocking(fd, true);
    const int rc = ::connect(fd, p->ai_addr, static_cast<int>(p->ai_addrlen));
    if (rc != 0) {
      const int err_code = CurrentSocketError();
      if (!IsWouldBlockError(err_code)) {
        last_err = LastSocketErrorText(err_code);
        CloseSocketHandle(fd);
        continue;
      }
      std::string wait_err;
      if (!WaitConnectReady(fd, kConnectTimeoutMs, wait_err)) {
        last_err = wait_err;
        CloseSocketHandle(fd);
        continue;
      }
    }

    if (!SetNonBlocking(fd, false)) {
      last_err = LastSocketErrorText();
      CloseSocketHandle(fd);
      continue;
    }
    ApplySocketTimeouts(fd, kSocketIoTimeoutMs);
    connected = fd;
    break;
  }
  freeaddrinfo(res);

  if (connected == kInvalidSocket) {
    error_out = "连接 Redis 失败 (" + endpoint_.host + ":" + port_str + "): " + last_err;
    return false;
  }

#if defined(_WIN32)
  sock_ = static_cast<std::uintptr_t>(connected);
#else
  sock_ = connected;
#endif

  const auto run_direct = [&](const std::vector<std::string>& cmd, RedisValue& out) -> bool {
    std::vector<RedisValue> replies;
    bool received_any = false;
    if (!ExecuteBatchRaw({cmd}, replies, received_any, error_out) || replies.empty()) {
      return false;
    }
    out = std::move(replies.front());
    return true;
  };

  const bool has_custom_user =
      !endpoint_.username.empty() && !EqualsIgnoreCase(endpoint_.username, "default");
  if (!endpoint_.password.empty() || has_custom_user) {
    RedisValue auth_reply;
    if (!endpoint_.username.empty()) {
      if (!run_direct({"AUTH", endpoint_.username, endpoint_.password}, auth_reply)) {
        Disconnect();
        return false;
      }
      if (auth_reply.IsError() && !endpoint_.password.empty() &&
          LowerAscii(auth_reply.str).find("wrong number of arguments") != std::string::npos) {
        if (!run_direct({"AUTH", endpoint_.password}, auth_reply)) {
          Disconnect();
          return false;
        }
      }
    } else {
      if (!run_direct({"AUTH", endpoint_.password}, auth_reply)) {
        Disconnect();
        return false;
      }
    }
    if (auth_reply.IsError()) {
      error_out = "Redis 认证失败: " + auth_reply.str;
      Disconnect();
      return false;
    }
  }

  if (current_db_ > 0) {
    RedisValue sel_reply;
    if (!run_direct({"SELECT", std::to_string(current_db_)}, sel_reply)) {
      Disconnect();
      return false;
    }
    if (sel_reply.IsError()) {
      error_out = "切换数据库 db" + std::to_string(current_db_) + " 失败: " + sel_reply.str;
      Disconnect();
      return false;
    }
  }

  RedisValue ping_reply;
  if (!run_direct({"PING"}, ping_reply)) {
    Disconnect();
    return false;
  }
  if (ping_reply.IsError()) {
    error_out = "Redis PING 失败: " + ping_reply.str;
    Disconnect();
    return false;
  }

  return true;
}

bool RedisClient::SendAll(std::string_view payload, std::string& error_out) {
  const SocketHandle fd = static_cast<SocketHandle>(sock_);
  if (fd == kInvalidSocket) {
    error_out = "Redis 连接未建立";
    return false;
  }
  std::size_t sent_total = 0;
  while (sent_total < payload.size()) {
#if defined(_WIN32)
    const int n = ::send(fd, payload.data() + sent_total,
                         static_cast<int>(payload.size() - sent_total), 0);
#elif defined(MSG_NOSIGNAL)
    const ssize_t n = ::send(fd, payload.data() + sent_total,
                             payload.size() - sent_total, MSG_NOSIGNAL);
#else
    const ssize_t n = ::send(fd, payload.data() + sent_total, payload.size() - sent_total, 0);
#endif
    if (n > 0) {
      sent_total += static_cast<std::size_t>(n);
      continue;
    }
    const int code = CurrentSocketError();
#if !defined(_WIN32)
    if (n < 0 && code == EINTR) {
      continue;
    }
#endif
    error_out = IsTimedOutError(code) ? "发送 Redis 请求超时"
                                      : ("发送 Redis 请求失败: " + LastSocketErrorText(code));
    return false;
  }
  return true;
}

bool RedisClient::RecvSome(std::string& error_out) {
  const SocketHandle fd = static_cast<SocketHandle>(sock_);
  if (fd == kInvalidSocket) {
    error_out = "Redis 连接未建立";
    return false;
  }
  char buf[16384];
  for (;;) {
#if defined(_WIN32)
    const int n = ::recv(fd, buf, static_cast<int>(sizeof(buf)), 0);
#else
    const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
#endif
    if (n > 0) {
      recv_buf_.append(buf, static_cast<std::size_t>(n));
      return true;
    }
    if (n == 0) {
      error_out = "Redis 服务器已关闭连接";
      return false;
    }
    const int code = CurrentSocketError();
#if !defined(_WIN32)
    if (code == EINTR) {
      continue;
    }
#endif
    error_out = IsTimedOutError(code) ? "读取 Redis 响应超时"
                                      : ("读取 Redis 响应失败: " + LastSocketErrorText(code));
    return false;
  }
}

bool RedisClient::ExecuteBatchRaw(const std::vector<std::vector<std::string>>& commands,
                                  std::vector<RedisValue>& out, bool& received_any,
                                  std::string& error_out) {
  out.clear();
  received_any = false;
  if (commands.empty()) {
    return true;
  }
  std::string payload;
  for (const auto& cmd : commands) {
    payload += EncodeRespCommand(cmd);
  }
  if (!SendAll(payload, error_out)) {
    return false;
  }

  out.reserve(commands.size());
  for (std::size_t idx = 0; idx < commands.size(); ++idx) {
    for (;;) {
      std::size_t consumed = 0;
      std::string parse_err;
      RedisValue value;
      if (TryParseRespValue(recv_buf_, consumed, value, parse_err)) {
        received_any = true;
        recv_buf_.erase(0, consumed);
        out.push_back(std::move(value));
        break;
      }
      if (!parse_err.empty()) {
        error_out = parse_err;
        Disconnect();
        return false;
      }
      if (!RecvSome(error_out)) {
        return false;
      }
      received_any = true;
    }
  }
  return true;
}

bool RedisClient::Pipeline(const std::vector<std::vector<std::string>>& commands,
                           std::vector<RedisValue>& out, std::string& error_out) {
  out.clear();
  if (commands.empty()) {
    return true;
  }
  if (!IsConnected() && !OpenSocketAndHandshake(error_out)) {
    return false;
  }
  bool received_any = false;
  if (ExecuteBatchRaw(commands, out, received_any, error_out)) {
    return true;
  }
  if (!received_any) {
    std::string reconnect_err;
    if (OpenSocketAndHandshake(reconnect_err)) {
      bool retry_received = false;
      if (ExecuteBatchRaw(commands, out, retry_received, error_out)) {
        return true;
      }
    }
  }
  Disconnect();
  return false;
}

bool RedisClient::Command(const std::vector<std::string>& args, RedisValue& out,
                          std::string& error_out) {
  std::vector<RedisValue> replies;
  if (!Pipeline({args}, replies, error_out) || replies.empty()) {
    return false;
  }
  out = std::move(replies.front());
  return true;
}

bool RedisClient::EnsureDb(int db, std::string& error_out) {
  if (db < 0) {
    error_out = "无效的 Redis 数据库编号";
    return false;
  }
  if (IsConnected() && current_db_ == db) {
    return true;
  }
  const int prev_db = current_db_;
  current_db_ = db;
  if (!IsConnected()) {
    if (!OpenSocketAndHandshake(error_out)) {
      current_db_ = prev_db;
      return false;
    }
    return true;
  }
  RedisValue reply;
  if (!Command({"SELECT", std::to_string(db)}, reply, error_out)) {
    current_db_ = prev_db;
    return false;
  }
  if (reply.IsError()) {
    current_db_ = prev_db;
    error_out = "切换数据库 db" + std::to_string(db) + " 失败: " + reply.str;
    return false;
  }
  return true;
}

// ---- db++ Domain Adapters --------------------------------------------------

std::vector<ColumnMeta> RedisKeySchema() {
  return {
      ColumnMeta{.name = "key",
                 .type = "TEXT",
                 .nullable = false,
                 .primary_key = true,
                 .primary_key_order = 1},
      ColumnMeta{.name = "type", .type = "TEXT", .nullable = false},
      ColumnMeta{.name = "ttl", .type = "INTEGER", .nullable = false},
      ColumnMeta{.name = "size", .type = "INTEGER", .nullable = false},
      ColumnMeta{.name = "value", .type = "TEXT", .nullable = true},
  };
}

AdbcObjectsResult LoadRedisObjectsBlocking(RedisClient& client) {
  AdbcObjectsResult result;
  RedisValue info_reply;
  std::string info_err;
  const bool info_ok = client.Command({"INFO", "keyspace"}, info_reply, info_err);

  std::map<int, std::int64_t> counts;
  bool has_keyspace = false;
  if (info_ok && !info_reply.IsError() && !info_reply.IsNull()) {
    counts = ParseInfoKeyspace(info_reply.ToDisplayString());
    has_keyspace = true;
  }

  int db_limit = 16;
  RedisValue cfg_reply;
  std::string cfg_err;
  const bool cfg_ok = client.Command({"CONFIG", "GET", "databases"}, cfg_reply, cfg_err);
  if (cfg_ok && cfg_reply.kind == RedisValueKind::Array && cfg_reply.elements.size() >= 2) {
    if (const auto parsed = ParseStrictInt64(cfg_reply.elements[1].ToDisplayString())) {
      db_limit = std::clamp<int>(static_cast<int>(*parsed), 1, 16);
    }
  }

  bool dbsize_ok = false;
  if (!has_keyspace) {
    RedisValue dbsize_reply;
    std::string dbsize_err;
    dbsize_ok = client.Command({"DBSIZE"}, dbsize_reply, dbsize_err);
    if (dbsize_ok && dbsize_reply.kind == RedisValueKind::Integer) {
      counts[client.CurrentDb()] = dbsize_reply.integer;
      has_keyspace = true;
    }
  }

  const bool any_cmd_succeeded = info_ok || cfg_ok || dbsize_ok;
  if (!any_cmd_succeeded && !client.IsConnected()) {
    RedisValue ping_reply;
    std::string ping_err;
    if (!client.Command({"PING"}, ping_reply, ping_err) || ping_reply.IsError()) {
      result.error = !ping_err.empty()
                         ? ping_err
                         : (!ping_reply.str.empty() ? ping_reply.str : "无法连接 Redis 服务器");
      return result;
    }
  }

  std::set<int> db_indices;
  for (int i = 0; i < db_limit; ++i) {
    db_indices.insert(i);
  }
  for (const auto& [k, count] : counts) {
    (void)count;
    db_indices.insert(k);
  }
  db_indices.insert(client.CurrentDb());

  const std::vector<ColumnMeta> schema = RedisKeySchema();
  for (const int idx : db_indices) {
    DbObject obj;
    obj.name = "db" + std::to_string(idx);
    obj.table_type = "REDIS DB";
    obj.is_view = false;
    obj.primary_key = {"key"};
    obj.columns = schema;
    if (const auto it = counts.find(idx); it != counts.end()) {
      obj.item_count = it->second;
    } else if (has_keyspace) {
      obj.item_count = 0;
    }
    result.objects.push_back(std::move(obj));
  }
  return result;
}

RedisPageResult LoadRedisKeysPageBlocking(RedisClient& client, const DbObject& object,
                                          std::int64_t offset, std::int64_t limit,
                                          std::optional<huxerui::DataTableSort> sort,
                                          std::string filter) {
  RedisPageResult result;
  result.columns = RedisKeySchema();

  const std::optional<int> target_db = ParseRedisDbName(object.name);
  if (!target_db.has_value()) {
    result.error = "无效的 Redis 数据库对象名: " + object.name;
    return result;
  }
  if (!client.EnsureDb(*target_db, result.error)) {
    return result;
  }

  const std::string_view trimmed_filter = TrimAscii(filter);
  const std::string pattern = trimmed_filter.empty() ? "*" : std::string(trimmed_filter);

  std::vector<std::string> keys;
  std::unordered_set<std::string> seen;
  std::string cursor = "0";
  for (;;) {
    RedisValue scan_reply;
    if (!client.Command({"SCAN", cursor, "MATCH", pattern, "COUNT", "500"}, scan_reply,
                        result.error)) {
      return result;
    }
    if (scan_reply.IsError()) {
      result.error = scan_reply.str;
      return result;
    }
    if (scan_reply.kind != RedisValueKind::Array || scan_reply.elements.size() < 2) {
      result.error = "SCAN 返回格式无效";
      return result;
    }
    cursor = scan_reply.elements[0].ToDisplayString();
    const RedisValue& batch = scan_reply.elements[1];
    if (batch.kind == RedisValueKind::Array) {
      for (const RedisValue& item : batch.elements) {
        std::string k = item.ToDisplayString();
        if (seen.insert(k).second) {
          keys.push_back(std::move(k));
          if (keys.size() >= kMaxScanKeys) {
            result.scan_capped = (cursor != "0");
            break;
          }
        }
      }
    }
    if (cursor == "0" || keys.size() >= kMaxScanKeys) {
      break;
    }
  }

  result.total = static_cast<std::int64_t>(keys.size());
  const std::int64_t safe_offset = std::max<std::int64_t>(0, offset);
  const std::int64_t safe_limit = std::max<std::int64_t>(1, limit);

  const auto fetch_rows_for_keys = [&](const std::vector<std::string>& page_keys,
                                       std::vector<GridRow>& rows_out,
                                       std::string& err_out) -> bool {
    rows_out.clear();
    if (page_keys.empty()) {
      return true;
    }

    std::vector<std::vector<std::string>> meta_cmds;
    meta_cmds.reserve(page_keys.size() * 2);
    for (const std::string& k : page_keys) {
      meta_cmds.push_back({"TYPE", k});
      meta_cmds.push_back({"TTL", k});
    }
    std::vector<RedisValue> meta_replies;
    if (!client.Pipeline(meta_cmds, meta_replies, err_out)) {
      return false;
    }

    std::vector<std::string> types(page_keys.size(), "none");
    std::vector<std::int64_t> ttls(page_keys.size(), -1);
    for (std::size_t i = 0; i < page_keys.size(); ++i) {
      if (i * 2 < meta_replies.size()) {
        types[i] = LowerAscii(meta_replies[i * 2].ToDisplayString());
      }
      if (i * 2 + 1 < meta_replies.size() &&
          meta_replies[i * 2 + 1].kind == RedisValueKind::Integer) {
        ttls[i] = meta_replies[i * 2 + 1].integer;
      }
    }

    // Batch fetch size and value preview for each key according to its type.
    struct FetchMapping {
      std::size_t key_idx = 0;
      std::size_t size_cmd_idx = static_cast<std::size_t>(-1);
      std::size_t val_cmd_idx = static_cast<std::size_t>(-1);
    };
    std::vector<std::vector<std::string>> data_cmds;
    std::vector<FetchMapping> mappings;
    mappings.reserve(page_keys.size());

    for (std::size_t i = 0; i < page_keys.size(); ++i) {
      const std::string& k = page_keys[i];
      const std::string& t = types[i];
      FetchMapping map{.key_idx = i};
      if (t == "string") {
        map.size_cmd_idx = data_cmds.size();
        data_cmds.push_back({"STRLEN", k});
        map.val_cmd_idx = data_cmds.size();
        data_cmds.push_back({"GET", k});
      } else if (t == "hash") {
        map.size_cmd_idx = data_cmds.size();
        data_cmds.push_back({"HLEN", k});
        map.val_cmd_idx = data_cmds.size();
        data_cmds.push_back({"HGETALL", k});
      } else if (t == "list") {
        map.size_cmd_idx = data_cmds.size();
        data_cmds.push_back({"LLEN", k});
        map.val_cmd_idx = data_cmds.size();
        data_cmds.push_back(
            {"LRANGE", k, "0", std::to_string(kMaxListZsetPreview - 1)});
      } else if (t == "set") {
        map.size_cmd_idx = data_cmds.size();
        data_cmds.push_back({"SCARD", k});
        map.val_cmd_idx = data_cmds.size();
        data_cmds.push_back({"SMEMBERS", k});
      } else if (t == "zset") {
        map.size_cmd_idx = data_cmds.size();
        data_cmds.push_back({"ZCARD", k});
        map.val_cmd_idx = data_cmds.size();
        data_cmds.push_back({"ZRANGE", k, "0", std::to_string(kMaxListZsetPreview - 1),
                             "WITHSCORES"});
      } else if (t == "stream") {
        map.size_cmd_idx = data_cmds.size();
        data_cmds.push_back({"XLEN", k});
        map.val_cmd_idx = data_cmds.size();
        data_cmds.push_back({"XRANGE", k, "-", "+", "COUNT",
                             std::to_string(kMaxStreamPreview)});
      }
      mappings.push_back(map);
    }

    std::vector<RedisValue> data_replies;
    if (!data_cmds.empty() && !client.Pipeline(data_cmds, data_replies, err_out)) {
      return false;
    }

    rows_out.reserve(page_keys.size());
    for (const FetchMapping& m : mappings) {
      const std::string& k = page_keys[m.key_idx];
      const std::string& t = types[m.key_idx];
      if (t == "none") {
        continue;
      }
      std::string size_str = "0";
      if (m.size_cmd_idx < data_replies.size() &&
          data_replies[m.size_cmd_idx].kind == RedisValueKind::Integer) {
        size_str = std::to_string(data_replies[m.size_cmd_idx].integer);
      }

      CellValue val_cell = NullCell();
      if (m.val_cmd_idx < data_replies.size()) {
        const RedisValue& vr = data_replies[m.val_cmd_idx];
        if (!vr.IsNull() && !vr.IsError()) {
          if (t == "string") {
            val_cell = TextCell(vr.ToDisplayString());
          } else if (t == "hash") {
            val_cell = TextCell(FormatJsonHashPairs(vr.elements, kMaxCollectionPreview));
          } else if (t == "list" || t == "set") {
            val_cell = TextCell(FormatJsonArrayOfStrings(vr.elements, kMaxCollectionPreview));
          } else if (t == "zset") {
            val_cell = TextCell(FormatJsonZsetPairs(vr.elements, kMaxListZsetPreview));
          } else if (t == "stream") {
            val_cell = TextCell(FormatJsonStreamEntries(vr.elements));
          }
        }
      } else {
        val_cell = TextCell("<" + t + ">");
      }

      GridRow row;
      row.values = {
          TextCell(k),
          TextCell(t),
          TextCell(std::to_string(ttls[m.key_idx])),
          TextCell(size_str),
          std::move(val_cell),
      };
      row.key_values = {row.values[0]};
      rows_out.push_back(std::move(row));
    }
    return true;
  };

  const bool non_key_sort =
      sort.has_value() && !sort->column_id.empty() && sort->column_id != "key";
  const bool descending =
      sort.has_value() && sort->direction == huxerui::DataTableSortDirection::Descending;

  if (non_key_sort && keys.size() <= 1000) {
    std::vector<GridRow> all_rows;
    if (!fetch_rows_for_keys(keys, all_rows, result.error)) {
      return result;
    }
    std::size_t sort_col = 0;
    for (std::size_t c = 0; c < result.columns.size(); ++c) {
      if (result.columns[c].name == sort->column_id) {
        sort_col = c;
        break;
      }
    }
    const bool numeric_col = (sort_col == 2 || sort_col == 3);
    std::ranges::sort(all_rows, [&](const GridRow& a, const GridRow& b) {
      const CellValue& ca = a.values[sort_col];
      const CellValue& cb = b.values[sort_col];
      int cmp = 0;
      if (numeric_col) {
        const std::int64_t na = ParseStrictInt64(ca.text).value_or(0);
        const std::int64_t nb = ParseStrictInt64(cb.text).value_or(0);
        cmp = (na < nb) ? -1 : (na > nb ? 1 : 0);
      } else {
        cmp = (ca.text < cb.text) ? -1 : (ca.text > cb.text ? 1 : 0);
      }
      if (cmp == 0) {
        cmp = (a.values[0].text < b.values[0].text)
                  ? -1
                  : (a.values[0].text > b.values[0].text ? 1 : 0);
      }
      return descending ? (cmp > 0) : (cmp < 0);
    });
    result.total = static_cast<std::int64_t>(all_rows.size());
    const std::size_t start =
        std::min(static_cast<std::size_t>(safe_offset), all_rows.size());
    const std::size_t end =
        std::min(start + static_cast<std::size_t>(safe_limit), all_rows.size());
    result.rows.assign(std::make_move_iterator(all_rows.begin() + static_cast<std::ptrdiff_t>(start)),
                       std::make_move_iterator(all_rows.begin() + static_cast<std::ptrdiff_t>(end)));
    return result;
  }

  std::ranges::sort(keys, [&](const std::string& a, const std::string& b) {
    return (sort.has_value() && sort->column_id == "key" && descending) ? (a > b) : (a < b);
  });
  const std::size_t start = std::min(static_cast<std::size_t>(safe_offset), keys.size());
  const std::size_t end =
      std::min(start + static_cast<std::size_t>(safe_limit), keys.size());
  std::vector<std::string> slice(keys.begin() + static_cast<std::ptrdiff_t>(start),
                                 keys.begin() + static_cast<std::ptrdiff_t>(end));
  if (!fetch_rows_for_keys(slice, result.rows, result.error)) {
    return result;
  }
  return result;
}

TableModel LoadRedisTableBlocking(RedisClient& client, std::uint64_t session_id,
                                  const DbObject& object, std::int64_t page,
                                  std::int64_t page_size,
                                  std::optional<huxerui::DataTableSort> sort, std::string filter) {
  TableModel model;
  model.session_id = session_id;
  model.object = object.name;
  model.qualified_name = object.name;
  model.is_view = false;
  model.read_only = false;
  model.editable = true;
  model.columns = RedisKeySchema();
  model.key_columns = {0};
  model.page_size = std::max<std::int64_t>(1, page_size);
  model.sort = sort;
  model.filter = filter;

  const std::int64_t req_page = std::max<std::int64_t>(0, page);
  RedisPageResult page_res = LoadRedisKeysPageBlocking(
      client, object, req_page * model.page_size, model.page_size, sort, filter);
  if (!page_res.error.empty()) {
    model.error = page_res.error;
    return model;
  }

  model.total = page_res.total;
  const std::int64_t page_count =
      std::max<std::int64_t>(1, (model.total + model.page_size - 1) / model.page_size);
  const std::int64_t clamped_page = std::clamp<std::int64_t>(req_page, 0, page_count - 1);
  if (clamped_page != req_page) {
    page_res = LoadRedisKeysPageBlocking(client, object, clamped_page * model.page_size,
                                         model.page_size, sort, filter);
    if (!page_res.error.empty()) {
      model.error = page_res.error;
      return model;
    }
    model.total = page_res.total;
  }
  model.page = clamped_page;
  model.snapshot = std::make_shared<const GridSnapshot>(GridSnapshot{.rows = std::move(page_res.rows)});
  model.message = std::to_string(model.total) + " 个键 · 第 " + std::to_string(model.page + 1) +
                  " / " + std::to_string(page_count) + " 页";
  if (page_res.scan_capped) {
    model.message += "（最多扫描 " + std::to_string(kMaxScanKeys) + " 键）";
  }
  return model;
}

namespace {

AdbcQueryResult ExecuteSingleRedisCommandBlocking(RedisClient& client,
                                                  std::string_view command_text,
                                                  std::size_t max_rows) {
  AdbcQueryResult result;
  const RedisCommandParseResult parsed = ParseRedisCommandLine(command_text);
  if (!parsed.error.empty()) {
    result.error = parsed.error;
    return result;
  }
  if (parsed.args.empty()) {
    result.error = "Redis 命令不能为空";
    return result;
  }

  const std::string cmd = UpperAscii(parsed.args[0]);
  const std::string sub = parsed.args.size() > 1 ? UpperAscii(parsed.args[1]) : std::string{};
  const std::size_t row_limit = max_rows == 0 ? kMaxQueryRows : max_rows;

  if (cmd == "SELECT" && parsed.args.size() == 2) {
    const auto db_num = ParseStrictInt64(parsed.args[1]);
    if (!db_num.has_value() || *db_num < 0 || *db_num > 1000000) {
      result.error = "无效的数据库编号: " + parsed.args[1];
      return result;
    }
    if (!client.EnsureDb(static_cast<int>(*db_num), result.error)) {
      return result;
    }
    result.columns = {"result"};
    result.rows.push_back(GridRow{.values = {TextCell("OK (db" + std::to_string(*db_num) + ")")},
                                  .key_values = {},
                                  .rowid = std::nullopt});
    result.affected = 0;
    return result;
  }

  RedisValue reply;
  if (!client.Command(parsed.args, reply, result.error)) {
    return result;
  }
  if (reply.IsError()) {
    result.error = reply.str;
    return result;
  }

  if (cmd == "INFO" &&
      (reply.kind == RedisValueKind::BulkString || reply.kind == RedisValueKind::SimpleString)) {
    std::vector<GridRow> info_rows;
    if (ParseInfoIntoRows(reply.str, info_rows)) {
      result.columns = {"section", "property", "value"};
      if (info_rows.size() > row_limit) {
        result.truncated = true;
        info_rows.resize(row_limit);
      }
      result.rows = std::move(info_rows);
      result.affected = static_cast<std::int64_t>(result.rows.size());
      return result;
    }
  }

  if (reply.kind == RedisValueKind::Array) {
    const bool withscores = std::ranges::any_of(parsed.args, [](const std::string& a) {
      return EqualsIgnoreCase(a, "WITHSCORES");
    });
    const bool is_pair_cmd = reply.is_map || cmd == "HGETALL" ||
                             (cmd == "CONFIG" && sub == "GET") || withscores;

    if (is_pair_cmd) {
      result.columns = withscores ? std::vector<std::string>{"member", "score"}
                                  : std::vector<std::string>{"field", "value"};
      for (std::size_t i = 0; i + 1 < reply.elements.size(); i += 2) {
        if (result.rows.size() >= row_limit) {
          result.truncated = true;
          break;
        }
        result.rows.push_back(GridRow{
            .values = {ReplyToCell(reply.elements[i]), ReplyToCell(reply.elements[i + 1])},
            .key_values = {},
            .rowid = std::nullopt,
        });
      }
      result.affected = static_cast<std::int64_t>(result.rows.size());
      return result;
    }

    // Two-element cursor array from SCAN / HSCAN / SSCAN / ZSCAN -> unwrap or show elements.
    if ((cmd == "SCAN" || cmd == "HSCAN" || cmd == "SSCAN" || cmd == "ZSCAN") &&
        reply.elements.size() >= 2 && reply.elements[1].kind == RedisValueKind::Array) {
      const std::string cursor = reply.elements[0].ToDisplayString();
      result.columns = {"cursor", "item"};
      for (const RedisValue& elem : reply.elements[1].elements) {
        if (result.rows.size() >= row_limit) {
          result.truncated = true;
          break;
        }
        result.rows.push_back(GridRow{.values = {TextCell(cursor), ReplyToCell(elem)},
                                      .key_values = {},
                                      .rowid = std::nullopt});
      }
      if (result.rows.empty()) {
        result.rows.push_back(
            GridRow{.values = {TextCell(cursor), NullCell()}, .key_values = {}, .rowid = std::nullopt});
      }
      result.affected = static_cast<std::int64_t>(result.rows.size());
      return result;
    }

    result.columns = {"index", "value"};
    for (std::size_t i = 0; i < reply.elements.size(); ++i) {
      if (result.rows.size() >= row_limit) {
        result.truncated = true;
        break;
      }
      result.rows.push_back(GridRow{
          .values = {TextCell(std::to_string(i)), ReplyToCell(reply.elements[i])},
          .key_values = {},
          .rowid = std::nullopt,
      });
    }
    result.affected = static_cast<std::int64_t>(result.rows.size());
    return result;
  }

  result.columns = {"result"};
  result.rows.push_back(GridRow{.values = {ReplyToCell(reply)}, .key_values = {}, .rowid = std::nullopt});
  result.affected = reply.kind == RedisValueKind::Integer ? reply.integer : 1;
  return result;
}

} // namespace

AdbcQueryResult ExecuteRedisCommandBlocking(RedisClient& client, std::string_view command_text,
                                            std::size_t max_rows) {
  const std::vector<std::string> lines = SplitRedisScript(command_text);
  if (lines.empty()) {
    AdbcQueryResult result;
    const RedisCommandParseResult syntax_check = ParseRedisCommandLine(command_text);
    result.error = !syntax_check.error.empty() ? syntax_check.error : "Redis 命令不能为空";
    return result;
  }
  if (lines.size() == 1) {
    return ExecuteSingleRedisCommandBlocking(client, lines.front(), max_rows);
  }
  AdbcQueryResult last_result;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    AdbcQueryResult step = ExecuteSingleRedisCommandBlocking(client, lines[i], max_rows);
    if (!step.error.empty()) {
      step.error = "第 " + std::to_string(i + 1) + " 条命令失败: " + step.error;
      return step;
    }
    last_result = std::move(step);
  }
  return last_result;
}

AdbcQueryResult UpdateRedisCellBlocking(RedisClient& client, const TableModel& model,
                                        std::size_t row_index, std::size_t column_index,
                                        const std::string& text) {
  AdbcQueryResult result;
  if (!model.snapshot || row_index >= model.snapshot->rows.size() || column_index >= 5) {
    result.error = "目标行或列已失效";
    return result;
  }
  const std::optional<int> db = ParseRedisDbName(model.object);
  if (!db.has_value()) {
    result.error = "无效的 Redis 数据库: " + model.object;
    return result;
  }
  if (!client.EnsureDb(*db, result.error)) {
    return result;
  }

  const GridRow& row = model.snapshot->rows[row_index];
  if (row.values.size() < 5) {
    result.error = "行数据不完整";
    return result;
  }
  const std::string& key = row.values[0].text;
  const std::string type = LowerAscii(row.values[1].text);

  if (column_index == 0) {
    if (text.empty()) {
      result.error = "键名不能为空";
      return result;
    }
    if (text == key) {
      return result;
    }
    RedisValue reply;
    if (!client.Command({"RENAMENX", key, text}, reply, result.error)) {
      return result;
    }
    if (reply.IsError()) {
      result.error = reply.str;
      return result;
    }
    if (reply.kind == RedisValueKind::Integer && reply.integer == 0) {
      result.error = "目标键已存在，无法覆盖: " + text;
      return result;
    }
    result.affected = 1;
    return result;
  }

  if (column_index == 1) {
    result.error = "不支持直接修改已有键的类型，请删除后新增";
    return result;
  }

  if (column_index == 2) {
    const std::string_view trimmed = TrimAscii(text);
    if (trimmed.empty() || trimmed == "-1" || EqualsIgnoreCase(trimmed, "NULL")) {
      RedisValue reply;
      if (!client.Command({"PERSIST", key}, reply, result.error)) {
        return result;
      }
      if (reply.IsError()) {
        result.error = reply.str;
        return result;
      }
      result.affected = 1;
      return result;
    }
    const auto ttl_val = ParseStrictInt64(trimmed);
    if (!ttl_val.has_value() || *ttl_val <= 0) {
      result.error = "TTL 必须为正整数（秒）或 -1（永不过期）";
      return result;
    }
    RedisValue reply;
    if (!client.Command({"EXPIRE", key, std::to_string(*ttl_val)}, reply, result.error)) {
      return result;
    }
    if (reply.IsError()) {
      result.error = reply.str;
      return result;
    }
    result.affected = reply.integer;
    return result;
  }

  if (column_index == 3) {
    result.error = "size 为只读统计列";
    return result;
  }

  // column_index == 4: update value.
  std::vector<std::string> write_args;
  if (!BuildWriteCommandArgs(type, text, write_args, result.error)) {
    return result;
  }
  RedisValue pttl_reply;
  std::optional<std::int64_t> preserve_pttl;
  if (client.Command({"PTTL", key}, pttl_reply, result.error) &&
      pttl_reply.kind == RedisValueKind::Integer && pttl_reply.integer > 0) {
    preserve_pttl = pttl_reply.integer;
  } else {
    result.error.clear();
  }
  if (!WriteTypedRedisKey(client, key, type, write_args, std::nullopt, type != "string",
                          preserve_pttl, result.error)) {
    return result;
  }
  result.affected = 1;
  return result;
}

AdbcQueryResult DeleteRedisKeyBlocking(RedisClient& client, const TableModel& model,
                                       std::size_t row_index) {
  AdbcQueryResult result;
  if (!model.snapshot || row_index >= model.snapshot->rows.size() ||
      model.snapshot->rows[row_index].values.empty()) {
    result.error = "目标行已失效";
    return result;
  }
  const std::optional<int> db = ParseRedisDbName(model.object);
  if (!db.has_value()) {
    result.error = "无效的 Redis 数据库: " + model.object;
    return result;
  }
  if (!client.EnsureDb(*db, result.error)) {
    return result;
  }
  const std::string& key = model.snapshot->rows[row_index].values[0].text;
  RedisValue reply;
  if (!client.Command({"DEL", key}, reply, result.error)) {
    return result;
  }
  if (reply.IsError()) {
    result.error = reply.str;
    return result;
  }
  result.affected = reply.kind == RedisValueKind::Integer ? reply.integer : 1;
  return result;
}

BatchInsertResult InsertRedisRowsBlocking(RedisClient& client, const TableModel& model,
                                          const std::vector<PendingInsertRow>& rows) {
  if (rows.empty()) {
    return {};
  }
  const std::optional<int> db = ParseRedisDbName(model.object);
  if (!db.has_value()) {
    return BatchInsertResult{.inserted = 0, .rolled_back = true,
                             .error = "无效的 Redis 数据库: " + model.object};
  }

  std::vector<ParsedWriteSpec> specs;
  specs.reserve(rows.size());
  std::unordered_set<std::string> batch_keys;
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const auto cell_at = [&](std::size_t col) -> std::string_view {
      return col < rows[i].values.size() ? std::string_view(rows[i].values[col])
                                         : std::string_view{};
    };
    const std::string prefix = "第 " + std::to_string(i + 1) + " 行";
    const std::string key(cell_at(0));
    if (key.empty()) {
      return BatchInsertResult{.inserted = 0, .rolled_back = true, .error = prefix + " 键名不能为空"};
    }
    if (!batch_keys.insert(key).second) {
      return BatchInsertResult{.inserted = 0, .rolled_back = true,
                               .error = prefix + " 键名与前面重复: " + key};
    }

    std::string norm_type;
    std::string err;
    if (!ValidateWritableKeyType(cell_at(1), norm_type, err)) {
      return BatchInsertResult{.inserted = 0, .rolled_back = true, .error = prefix + ": " + err};
    }

    std::optional<std::int64_t> ttl;
    const std::string_view ttl_sv = TrimAscii(cell_at(2));
    if (!ttl_sv.empty() && ttl_sv != "-1" && !EqualsIgnoreCase(ttl_sv, "NULL")) {
      const auto parsed = ParseStrictInt64(ttl_sv);
      if (!parsed.has_value() || *parsed <= 0) {
        return BatchInsertResult{
            .inserted = 0, .rolled_back = true,
            .error = prefix + ": TTL 必须为正整数或 -1"};
      }
      ttl = *parsed;
    }

    std::vector<std::string> args;
    if (!BuildWriteCommandArgs(norm_type, cell_at(4), args, err)) {
      return BatchInsertResult{.inserted = 0, .rolled_back = true, .error = prefix + ": " + err};
    }

    specs.push_back(ParsedWriteSpec{
        .key = key,
        .type = std::move(norm_type),
        .ttl = ttl,
        .args = std::move(args),
    });
  }

  std::string err;
  if (!client.EnsureDb(*db, err)) {
    return BatchInsertResult{.inserted = 0, .rolled_back = true, .error = err};
  }

  std::vector<std::vector<std::string>> exists_cmds;
  exists_cmds.reserve(specs.size());
  for (const ParsedWriteSpec& spec : specs) {
    exists_cmds.push_back({"EXISTS", spec.key});
  }
  std::vector<RedisValue> exists_replies;
  if (!client.Pipeline(exists_cmds, exists_replies, err)) {
    return BatchInsertResult{.inserted = 0, .rolled_back = true, .error = err};
  }
  for (std::size_t i = 0; i < specs.size() && i < exists_replies.size(); ++i) {
    if (exists_replies[i].kind == RedisValueKind::Integer && exists_replies[i].integer > 0) {
      return BatchInsertResult{.inserted = 0, .rolled_back = true,
                               .error = "第 " + std::to_string(i + 1) + " 行键已存在: " +
                                        specs[i].key};
    }
  }

  std::size_t inserted = 0;
  for (const ParsedWriteSpec& spec : specs) {
    if (!WriteTypedRedisKey(client, spec.key, spec.type, spec.args, spec.ttl, false,
                            std::nullopt, err)) {
      return BatchInsertResult{.inserted = inserted,
                               .rolled_back = (inserted == 0),
                               .error = "写入 " + spec.key + " 失败: " + err};
    }
    ++inserted;
  }

  return BatchInsertResult{.inserted = inserted, .rolled_back = false, .error = {}};
}

} // namespace dbpp
