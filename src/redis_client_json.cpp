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
      error_out = Tr("Trailing characters after JSON", "JSON 尾部存在多余字符");
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
      error_out = Tr("Expected JSON string", "期望 JSON 字符串");
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
        error_out = Tr("Unescaped control character in JSON string",
                       "JSON 字符串包含未转义控制字符");
        return false;
      }
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (pos_ >= text_.size()) {
        error_out = Tr("Unterminated JSON escape sequence", "JSON 转义序列未结束");
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
            error_out = Tr("Truncated JSON \\u escape", "JSON \\u 转义长度不足");
            return false;
          }
          std::uint32_t cp = 0;
          for (int k = 0; k < 4; ++k) {
            const int d = detail::RedisHexDigit(text_[pos_++]);
            if (d < 0) {
              error_out = Tr("Invalid hex digit in JSON \\u escape",
                             "无效的 JSON \\u 十六进制字符");
              return false;
            }
            cp = (cp << 4) | static_cast<std::uint32_t>(d);
          }
          if (cp >= 0xD800U && cp <= 0xDBFFU && pos_ + 6 <= text_.size() &&
              text_[pos_] == '\\' && text_[pos_ + 1] == 'u') {
            std::uint32_t low = 0;
            bool ok = true;
            for (int k = 0; k < 4; ++k) {
              const int d = detail::RedisHexDigit(text_[pos_ + 2 + static_cast<std::size_t>(k)]);
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
          error_out = TrFmt("Unsupported JSON escape \\{0}", "不支持的 JSON 转义 \\{0}",
                            {std::string{esc}});
          return false;
      }
    }
    error_out = Tr("Unterminated JSON string", "JSON 字符串缺少结束引号");
    return false;
  }

  bool ParseValue(JsonValue& out, std::string& error_out, int depth) {
    if (depth > 32) {
      error_out = Tr("JSON nesting too deep", "JSON 嵌套层级过深");
      return false;
    }
    SkipWs();
    if (pos_ >= text_.size()) {
      error_out = Tr("Unexpected end of JSON input", "JSON 内容意外结束");
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
          error_out = Tr("Expected ':' in JSON object", "JSON 对象缺少 ':'");
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
          error_out = Tr("Expected '}' in JSON object", "JSON 对象缺少 '}'");
          return false;
        }
        if (text_[pos_] == '}') {
          ++pos_;
          out.str_val = std::string(text_.substr(obj_start, pos_ - obj_start));
          return true;
        }
        if (text_[pos_] != ',') {
          error_out = Tr("Expected ',' between JSON object members", "JSON 对象成员间缺少 ','");
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
          error_out = Tr("Expected ']' in JSON array", "JSON 数组缺少 ']'");
          return false;
        }
        if (text_[pos_] == ']') {
          ++pos_;
          out.str_val = std::string(text_.substr(arr_start, pos_ - arr_start));
          return true;
        }
        if (text_[pos_] != ',') {
          error_out = Tr("Expected ',' between JSON array elements", "JSON 数组元素间缺少 ','");
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
        error_out = Tr("Invalid JSON number", "无效的 JSON 数字");
        return false;
      }
      while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
        ++pos_;
      }
      if (pos_ < text_.size() && text_[pos_] == '.') {
        ++pos_;
        if (pos_ >= text_.size() || !std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
          error_out = Tr("Invalid JSON fraction", "无效的 JSON 小数");
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
          error_out = Tr("Invalid JSON exponent", "无效的 JSON 指数");
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
        error_out = TrFmt("JSON number out of range: {0}", "JSON 数字超出范围: {0}", {raw});
        return false;
      }
      out = JsonValue{};
      out.kind = JsonValue::Kind::Number;
      out.number_val = d;
      out.str_val = raw;
      return true;
    }

    error_out = TrFmt("Invalid JSON token '{0}'", "无效的 JSON token '{0}'", {std::string{c}});
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
      out += FormatJsonHashPairs(entry.elements[1].elements, detail::kRedisMaxCollectionPreview);
      out.push_back('}');
    } else {
      AppendJsonEscapedString(out, entry.ToDisplayString());
    }
  }
  out.push_back(']');
  return out;
}

bool BuildWriteCommandArgs(std::string_view type, std::string_view value_text,
                           std::vector<std::string>& args_out, std::string& error_out) {
  args_out.clear();
  if (type == "string") {
    args_out.push_back(std::string(value_text));
    return true;
  }
  JsonValue parsed;
  if (!JsonParser(value_text).Parse(parsed, error_out)) {
    error_out = TrFmt("{0} value must be valid JSON ({1})",
                      "{0} 类型的值必须为合法 JSON（{1}）", {type, error_out});
    return false;
  }
  if (type == "hash") {
    if (parsed.kind != JsonValue::Kind::Object || parsed.object_val.empty()) {
      error_out = Tr("hash value must be a non-empty JSON object, e.g. {\"field\":\"value\"}",
                     "hash 值必须为非空 JSON 对象，例如 {\"field\":\"value\"}");
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
      error_out = TrFmt("{0} value must be a non-empty JSON array, e.g. [\"a\",\"b\"]",
                        "{0} 值必须为非空 JSON 数组，例如 [\"a\",\"b\"]", {type});
      return false;
    }
    for (const JsonValue& elem : parsed.array_val) {
      args_out.push_back(elem.AsScalarString());
    }
    return true;
  }
  if (type == "zset") {
    if (parsed.kind != JsonValue::Kind::Array || parsed.array_val.empty()) {
      error_out = Tr("zset value must be a non-empty JSON array, e.g. [{\"member\":\"a\",\"score\":1}]",
                     "zset 值必须为非空 JSON 数组，例如 [{\"member\":\"a\",\"score\":1}]");
      return false;
    }
    for (const JsonValue& elem : parsed.array_val) {
      if (elem.kind != JsonValue::Kind::Object) {
        error_out = Tr("zset array elements must be objects with member and score",
                       "zset 数组元素必须为包含 member 与 score 的对象");
        return false;
      }
      const JsonValue* member = nullptr;
      const JsonValue* score = nullptr;
      for (const auto& [k, v] : elem.object_val) {
        if (k == "member") member = &v;
        if (k == "score") score = &v;
      }
      if (member == nullptr || score == nullptr) {
        error_out = Tr("Every zset element requires member and score fields",
                       "zset 每个元素都需要 member 和 score 字段");
        return false;
      }
      if (score->kind != JsonValue::Kind::Number && score->kind != JsonValue::Kind::String) {
        error_out = Tr("zset score must be a number", "zset 的 score 必须为数字");
        return false;
      }
      std::string score_str = score->AsScalarString();
      char* end = nullptr;
      errno = 0;
      const double d = std::strtod(score_str.c_str(), &end);
      if (score_str.empty() || errno == ERANGE || end != score_str.c_str() + score_str.size() ||
          !std::isfinite(d)) {
        error_out = TrFmt("zset score is not a valid number: {0}",
                          "zset 的 score 不是有效数字: {0}", {score_str});
        return false;
      }
      args_out.push_back(std::move(score_str));
      args_out.push_back(member->AsScalarString());
    }
    return true;
  }
  error_out = TrFmt("Writing type {0} is not supported yet", "暂不支持写入类型: {0}", {type});
  return false;
}

} // namespace

namespace detail {

std::string RedisFormatJsonArrayOfStrings(const std::vector<RedisValue>& elements,
                                          std::size_t cap) {
  return FormatJsonArrayOfStrings(elements, cap);
}
std::string RedisFormatJsonHashPairs(const std::vector<RedisValue>& elements,
                                     std::size_t max_pairs) {
  return FormatJsonHashPairs(elements, max_pairs);
}
std::string RedisFormatJsonZsetPairs(const std::vector<RedisValue>& elements,
                                     std::size_t max_pairs) {
  return FormatJsonZsetPairs(elements, max_pairs);
}
std::string RedisFormatJsonStreamEntries(const std::vector<RedisValue>& elements) {
  return FormatJsonStreamEntries(elements);
}
bool RedisBuildWriteCommandArgs(std::string_view type, std::string_view value_text,
                                std::vector<std::string>& args_out, std::string& error_out) {
  return BuildWriteCommandArgs(type, value_text, args_out, error_out);
}

} // namespace detail

} // namespace dbpp
