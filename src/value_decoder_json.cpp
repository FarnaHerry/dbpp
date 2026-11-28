module dbpp.decoder;

import std;
import dbpp.i18n;

namespace dbpp {

namespace {

constexpr int kMaxJsonDepth = 128;

void AppendJsonEscapedByte(unsigned char b, std::string& out) {
  switch (b) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\b':
      out += "\\b";
      break;
    case '\f':
      out += "\\f";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (b < 0x20u) {
        static constexpr char kHex[] = "0123456789abcdef";
        out += "\\u00";
        out.push_back(kHex[(b >> 4) & 0x0F]);
        out.push_back(kHex[b & 0x0F]);
      } else {
        out.push_back(static_cast<char>(b));
      }
      break;
  }
}

struct JsonFormatter {
  std::string_view src;
  std::size_t pos = 0;
  bool pretty = true;
  std::string out{};
  std::string error{};

  void SkipWs() noexcept {
    while (pos < src.size() &&
           (src[pos] == ' ' || src[pos] == '\t' || src[pos] == '\r' || src[pos] == '\n')) {
      ++pos;
    }
  }

  void Indent(int depth) {
    out.push_back('\n');
    out.append(static_cast<std::size_t>(depth) * 2, ' ');
  }

  bool ParseString() {
    if (pos >= src.size() || src[pos] != '"') {
      error = Tr("Expected JSON string", "缺少 JSON 字符串起始引号");
      return false;
    }
    ++pos;
    out.push_back('"');
    while (pos < src.size()) {
      const unsigned char ch = static_cast<unsigned char>(src[pos++]);
      if (ch == '"') {
        out.push_back('"');
        return true;
      }
      if (ch < 0x20u) {
        error = Tr("Unescaped control character in JSON string", "JSON 字符串包含未转义控制字符");
        return false;
      }
      if (ch != '\\') {
        out.push_back(static_cast<char>(ch));
        continue;
      }
      if (pos >= src.size()) {
        error = Tr("Unterminated escape in JSON string", "JSON 字符串转义未闭合");
        return false;
      }
      const char esc = src[pos++];
      switch (esc) {
        case '"':
          out += "\\\"";
          break;
        case '\\':
          out += "\\\\";
          break;
        case '/':
          out.push_back('/');
          break;
        case 'b':
          out += "\\b";
          break;
        case 'f':
          out += "\\f";
          break;
        case 'n':
          out += "\\n";
          break;
        case 'r':
          out += "\\r";
          break;
        case 't':
          out += "\\t";
          break;
        case 'u': {
          std::uint16_t u1 = 0;
          if (!detail::DecoderParseHex4(src, pos, u1)) {
            error = Tr("Invalid \\uXXXX escape in JSON", "无效的 JSON \\uXXXX 转义");
            return false;
          }
          pos += 4;
          if (u1 >= 0xD800u && u1 <= 0xDBFFu) {
            if (pos + 6 <= src.size() && src[pos] == '\\' && src[pos + 1] == 'u') {
              std::uint16_t u2 = 0;
              if (detail::DecoderParseHex4(src, pos + 2, u2) && u2 >= 0xDC00u && u2 <= 0xDFFFu) {
                pos += 6;
                const std::uint32_t cp =
                    0x10000u + (((static_cast<std::uint32_t>(u1 - 0xD800u)) << 10) |
                                static_cast<std::uint32_t>(u2 - 0xDC00u));
                detail::DecoderAppendCodepointUtf8(cp, out);
                break;
              }
            }
          }
          if (u1 < 0x20u) {
            AppendJsonEscapedByte(static_cast<unsigned char>(u1), out);
          } else if (u1 == '"' || u1 == '\\') {
            AppendJsonEscapedByte(static_cast<unsigned char>(u1), out);
          } else {
            detail::DecoderAppendCodepointUtf8(u1, out);
          }
          break;
        }
        default:
          error = Tr("Unknown escape sequence in JSON", "未知的 JSON 转义字符");
          return false;
      }
    }
    error = Tr("Unterminated JSON string", "JSON 字符串未闭合");
    return false;
  }

  bool ParseNumber() {
    const std::size_t start = pos;
    if (pos < src.size() && src[pos] == '-') ++pos;
    if (pos >= src.size()) {
      error = Tr("Invalid JSON number", "无效的 JSON 数字");
      return false;
    }
    if (src[pos] == '0') {
      ++pos;
    } else if (src[pos] >= '1' && src[pos] <= '9') {
      while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') ++pos;
    } else {
      error = Tr("Invalid JSON number", "无效的 JSON 数字");
      return false;
    }
    if (pos < src.size() && src[pos] == '.') {
      ++pos;
      if (pos >= src.size() || src[pos] < '0' || src[pos] > '9') {
        error = Tr("Invalid JSON fraction", "无效的 JSON 小数部分");
        return false;
      }
      while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') ++pos;
    }
    if (pos < src.size() && (src[pos] == 'e' || src[pos] == 'E')) {
      ++pos;
      if (pos < src.size() && (src[pos] == '+' || src[pos] == '-')) ++pos;
      if (pos >= src.size() || src[pos] < '0' || src[pos] > '9') {
        error = Tr("Invalid JSON exponent", "无效的 JSON 指数部分");
        return false;
      }
      while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') ++pos;
    }
    out.append(src.substr(start, pos - start));
    return true;
  }

  bool ParseValue(int depth) {
    if (depth > kMaxJsonDepth) {
      error = Tr("JSON nesting exceeds limit (128)", "JSON 嵌套深度超过上限 (128)");
      return false;
    }
    SkipWs();
    if (pos >= src.size()) {
      error = Tr("Unexpected end of JSON", "JSON 意外结束");
      return false;
    }
    const char c = src[pos];
    if (c == '"') return ParseString();
    if (c == '-' || (c >= '0' && c <= '9')) return ParseNumber();
    if (c == 't' && src.substr(pos).starts_with("true")) {
      pos += 4;
      out += "true";
      return true;
    }
    if (c == 'f' && src.substr(pos).starts_with("false")) {
      pos += 5;
      out += "false";
      return true;
    }
    if (c == 'n' && src.substr(pos).starts_with("null")) {
      pos += 4;
      out += "null";
      return true;
    }
    if (c == '[') {
      ++pos;
      out.push_back('[');
      SkipWs();
      if (pos < src.size() && src[pos] == ']') {
        ++pos;
        out.push_back(']');
        return true;
      }
      for (;;) {
        if (pretty) Indent(depth + 1);
        if (!ParseValue(depth + 1)) return false;
        SkipWs();
        if (pos < src.size() && src[pos] == ',') {
          ++pos;
          out.push_back(',');
          continue;
        }
        if (pos < src.size() && src[pos] == ']') {
          ++pos;
          if (pretty) Indent(depth);
          out.push_back(']');
          return true;
        }
        error = Tr("Expected ',' or ']' in JSON array", "JSON 数组缺少 ',' 或 ']'");
        return false;
      }
    }
    if (c == '{') {
      ++pos;
      out.push_back('{');
      SkipWs();
      if (pos < src.size() && src[pos] == '}') {
        ++pos;
        out.push_back('}');
        return true;
      }
      for (;;) {
        if (pretty) Indent(depth + 1);
        SkipWs();
        if (!ParseString()) return false;
        SkipWs();
        if (pos >= src.size() || src[pos] != ':') {
          error = Tr("Expected ':' in JSON object", "JSON 对象缺少 ':'");
          return false;
        }
        ++pos;
        out += pretty ? ": " : ":";
        if (!ParseValue(depth + 1)) return false;
        SkipWs();
        if (pos < src.size() && src[pos] == ',') {
          ++pos;
          out.push_back(',');
          continue;
        }
        if (pos < src.size() && src[pos] == '}') {
          ++pos;
          if (pretty) Indent(depth);
          out.push_back('}');
          return true;
        }
        error = Tr("Expected ',' or '}' in JSON object", "JSON 对象缺少 ',' 或 '}'");
        return false;
      }
    }
    error = Tr("Invalid JSON syntax", "无效的 JSON 语法");
    return false;
  }
};

} // namespace

namespace detail {

std::string_view DecoderTrimAscii(std::string_view text) noexcept {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
    text.remove_prefix(1);
  }
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
    text.remove_suffix(1);
  }
  return text;
}

int DecoderHexDigitValue(char c) noexcept {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

void DecoderAppendCodepointUtf8(std::uint32_t cp, std::string& out) {
  if (cp <= 0x7Fu) {
    out.push_back(static_cast<char>(cp));
  } else if (cp <= 0x7FFu) {
    out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  } else if (cp <= 0xFFFFu) {
    out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
    out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  } else if (cp <= 0x10FFFFu) {
    out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
    out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  }
}

bool DecoderParseHex4(std::string_view s, std::size_t pos, std::uint16_t& out) noexcept {
  if (pos + 4 > s.size()) return false;
  std::uint16_t v = 0;
  for (std::size_t i = 0; i < 4; ++i) {
    const int d = DecoderHexDigitValue(s[pos + i]);
    if (d < 0) return false;
    v = static_cast<std::uint16_t>((v << 4) | static_cast<std::uint16_t>(d));
  }
  out = v;
  return true;
}

bool DecoderFormatJsonString(std::string_view raw, bool pretty, std::string& out,
                             std::string& error) {
  const std::string_view trimmed = DecoderTrimAscii(raw);
  if (trimmed.empty()) {
    error = Tr("Empty JSON input", "输入内容为空");
    return false;
  }
  JsonFormatter fmt{.src = trimmed, .pos = 0, .pretty = pretty, .out = {}, .error = {}};
  fmt.out.reserve(trimmed.size() + (pretty ? trimmed.size() / 2 : 0));
  if (!fmt.ParseValue(0)) {
    error = fmt.error;
    return false;
  }
  fmt.SkipWs();
  if (fmt.pos != trimmed.size()) {
    error = Tr("Unexpected trailing characters after JSON", "JSON 末尾有多余字符");
    return false;
  }
  out = std::move(fmt.out);
  return true;
}

} // namespace detail

} // namespace dbpp
