module;

#include <huxerui/state.h>

module dbpp.mcp;

import std;
import dbpp.settings;
import dbpp.types;
import dbpp.adbc;
import dbpp.db;

#ifndef DBPP_VERSION
#define DBPP_VERSION "0.4.0"
#endif

namespace dbpp {
namespace {

std::string LowerAsciiCopy(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const unsigned char c : text) {
    out.push_back(static_cast<char>(std::tolower(c)));
  }
  return out;
}

std::string_view TrimView(std::string_view text) {
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

// ---------------------------------------------------------------------------
// Minimal JSON parser & serializer for JSON-RPC 2.0 / MCP
// ---------------------------------------------------------------------------

struct JsonValue {
  using Array = std::vector<JsonValue>;
  using Object = std::vector<std::pair<std::string, JsonValue>>;

  std::variant<std::nullptr_t, bool, std::int64_t, double, std::string, Array, Object> data{
      nullptr};

  [[nodiscard]] bool IsNull() const noexcept { return std::holds_alternative<std::nullptr_t>(data); }
  [[nodiscard]] bool IsObject() const noexcept { return std::holds_alternative<Object>(data); }

  [[nodiscard]] const JsonValue* Find(std::string_view key) const noexcept {
    if (const auto* obj = std::get_if<Object>(&data)) {
      for (const auto& [k, v] : *obj) {
        if (k == key) {
          return &v;
        }
      }
    }
    return nullptr;
  }

  [[nodiscard]] std::string AsString(std::string_view fallback = {}) const {
    if (const auto* s = std::get_if<std::string>(&data)) {
      return *s;
    }
    return std::string(fallback);
  }

  [[nodiscard]] std::optional<std::int64_t> AsInt64() const noexcept {
    if (const auto* i = std::get_if<std::int64_t>(&data)) {
      return *i;
    }
    if (const auto* d = std::get_if<double>(&data);
        d != nullptr && std::isfinite(*d) &&
        *d >= static_cast<double>(std::numeric_limits<std::int64_t>::min()) &&
        *d <= static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
      return static_cast<std::int64_t>(*d);
    }
    return std::nullopt;
  }

  void Set(std::string key, JsonValue value) {
    if (!IsObject()) {
      data = Object{};
    }
    auto& obj = std::get<Object>(data);
    for (auto& [k, v] : obj) {
      if (k == key) {
        v = std::move(value);
        return;
      }
    }
    obj.emplace_back(std::move(key), std::move(value));
  }
};

JsonValue MakeObject(JsonValue::Object entries = {}) {
  return JsonValue{.data = std::move(entries)};
}

JsonValue MakeArray(JsonValue::Array items = {}) {
  return JsonValue{.data = std::move(items)};
}

void AppendUtf8CodePoint(std::string& out, std::uint32_t cp) {
  if (cp <= 0x7FU) {
    out.push_back(static_cast<char>(cp));
  } else if (cp <= 0x7FFU) {
    out.push_back(static_cast<char>(0xC0U | (cp >> 6)));
    out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
  } else if (cp <= 0xFFFFU) {
    out.push_back(static_cast<char>(0xE0U | (cp >> 12)));
    out.push_back(static_cast<char>(0x80U | ((cp >> 6) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
  } else if (cp <= 0x10FFFFU) {
    out.push_back(static_cast<char>(0xF0U | (cp >> 18)));
    out.push_back(static_cast<char>(0x80U | ((cp >> 12) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | ((cp >> 6) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
  }
}

class JsonParser {
public:
  explicit JsonParser(std::string_view input) : src_(input) {}

  bool Parse(JsonValue& out) {
    SkipWs();
    if (!ParseValue(out, 0)) {
      return false;
    }
    SkipWs();
    return pos_ == src_.size();
  }

private:
  void SkipWs() noexcept {
    while (pos_ < src_.size() &&
           (src_[pos_] == ' ' || src_[pos_] == '\t' || src_[pos_] == '\r' ||
            src_[pos_] == '\n')) {
      ++pos_;
    }
  }

  bool ParseValue(JsonValue& out, int depth) {
    if (depth > 64) {
      return false;
    }
    SkipWs();
    if (pos_ >= src_.size()) {
      return false;
    }
    const char ch = src_[pos_];
    if (ch == 'n') {
      if (src_.substr(pos_, 4) == "null") {
        pos_ += 4;
        out = JsonValue{nullptr};
        return true;
      }
      return false;
    }
    if (ch == 't') {
      if (src_.substr(pos_, 4) == "true") {
        pos_ += 4;
        out = JsonValue{true};
        return true;
      }
      return false;
    }
    if (ch == 'f') {
      if (src_.substr(pos_, 5) == "false") {
        pos_ += 5;
        out = JsonValue{false};
        return true;
      }
      return false;
    }
    if (ch == '"') {
      std::string s;
      if (!ParseString(s)) {
        return false;
      }
      out = JsonValue{std::move(s)};
      return true;
    }
    if (ch == '[') {
      return ParseArray(out, depth + 1);
    }
    if (ch == '{') {
      return ParseObject(out, depth + 1);
    }
    if (ch == '-' || (ch >= '0' && ch <= '9')) {
      return ParseNumber(out);
    }
    return false;
  }

  bool ParseHex4(std::uint32_t& code) {
    if (pos_ + 4 > src_.size()) {
      return false;
    }
    code = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = src_[pos_++];
      code <<= 4;
      if (c >= '0' && c <= '9') {
        code |= static_cast<std::uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        code |= static_cast<std::uint32_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        code |= static_cast<std::uint32_t>(c - 'A' + 10);
      } else {
        return false;
      }
    }
    return true;
  }

  bool ParseString(std::string& out) {
    if (pos_ >= src_.size() || src_[pos_] != '"') {
      return false;
    }
    ++pos_;
    out.clear();
    while (pos_ < src_.size()) {
      const unsigned char c = static_cast<unsigned char>(src_[pos_++]);
      if (c == '"') {
        return true;
      }
      if (c < 0x20U) {
        return false;
      }
      if (c == '\\') {
        if (pos_ >= src_.size()) {
          return false;
        }
        const char esc = src_[pos_++];
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
            std::uint32_t cp = 0;
            if (!ParseHex4(cp)) {
              return false;
            }
            if (cp >= 0xD800U && cp <= 0xDBFFU && pos_ + 6 <= src_.size() &&
                src_[pos_] == '\\' && src_[pos_ + 1] == 'u') {
              pos_ += 2;
              std::uint32_t low = 0;
              if (!ParseHex4(low)) {
                return false;
              }
              if (low >= 0xDC00U && low <= 0xDFFFU) {
                cp = 0x10000U + (((cp - 0xD800U) << 10) | (low - 0xDC00U));
              }
            }
            AppendUtf8CodePoint(out, cp);
            break;
          }
          default:
            return false;
        }
      } else {
        out.push_back(static_cast<char>(c));
      }
    }
    return false;
  }

  bool ParseNumber(JsonValue& out) {
    const std::size_t start = pos_;
    if (src_[pos_] == '-') {
      ++pos_;
    }
    if (pos_ >= src_.size() || !std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
      return false;
    }
    if (src_[pos_] == '0') {
      ++pos_;
    } else {
      while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
        ++pos_;
      }
    }
    bool is_float = false;
    if (pos_ < src_.size() && src_[pos_] == '.') {
      is_float = true;
      ++pos_;
      if (pos_ >= src_.size() || !std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
        return false;
      }
      while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
        ++pos_;
      }
    }
    if (pos_ < src_.size() && (src_[pos_] == 'e' || src_[pos_] == 'E')) {
      is_float = true;
      ++pos_;
      if (pos_ < src_.size() && (src_[pos_] == '+' || src_[pos_] == '-')) {
        ++pos_;
      }
      if (pos_ >= src_.size() || !std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
        return false;
      }
      while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
        ++pos_;
      }
    }
    const std::string_view token = src_.substr(start, pos_ - start);
    if (!is_float) {
      std::int64_t val = 0;
      const auto [end, ec] = std::from_chars(token.data(), token.data() + token.size(), val);
      if (ec == std::errc{} && end == token.data() + token.size()) {
        out = JsonValue{val};
        return true;
      }
    }
    const std::string copy(token);
    char* end = nullptr;
    const double d = std::strtod(copy.c_str(), &end);
    if (end != copy.c_str() + copy.size() || !std::isfinite(d)) {
      return false;
    }
    out = JsonValue{d};
    return true;
  }

  bool ParseArray(JsonValue& out, int depth) {
    ++pos_; // skip '['
    JsonValue::Array arr;
    SkipWs();
    if (pos_ < src_.size() && src_[pos_] == ']') {
      ++pos_;
      out = JsonValue{std::move(arr)};
      return true;
    }
    for (;;) {
      JsonValue elem;
      if (!ParseValue(elem, depth)) {
        return false;
      }
      arr.push_back(std::move(elem));
      SkipWs();
      if (pos_ >= src_.size()) {
        return false;
      }
      if (src_[pos_] == ']') {
        ++pos_;
        out = JsonValue{std::move(arr)};
        return true;
      }
      if (src_[pos_] != ',') {
        return false;
      }
      ++pos_;
    }
  }

  bool ParseObject(JsonValue& out, int depth) {
    ++pos_; // skip '{'
    JsonValue::Object obj;
    SkipWs();
    if (pos_ < src_.size() && src_[pos_] == '}') {
      ++pos_;
      out = JsonValue{std::move(obj)};
      return true;
    }
    for (;;) {
      SkipWs();
      std::string key;
      if (!ParseString(key)) {
        return false;
      }
      SkipWs();
      if (pos_ >= src_.size() || src_[pos_] != ':') {
        return false;
      }
      ++pos_;
      JsonValue val;
      if (!ParseValue(val, depth)) {
        return false;
      }
      bool updated = false;
      for (auto& [k, v] : obj) {
        if (k == key) {
          v = std::move(val);
          updated = true;
          break;
        }
      }
      if (!updated) {
        obj.emplace_back(std::move(key), std::move(val));
      }
      SkipWs();
      if (pos_ >= src_.size()) {
        return false;
      }
      if (src_[pos_] == '}') {
        ++pos_;
        out = JsonValue{std::move(obj)};
        return true;
      }
      if (src_[pos_] != ',') {
        return false;
      }
      ++pos_;
    }
  }

  std::string_view src_;
  std::size_t pos_ = 0;
};

void WriteJsonString(std::string& out, std::string_view text) {
  out.push_back('"');
  for (const unsigned char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20U) {
          constexpr char kHex[] = "0123456789abcdef";
          out += "\\u00";
          out.push_back(kHex[(c >> 4) & 0xFU]);
          out.push_back(kHex[c & 0xFU]);
        } else {
          out.push_back(static_cast<char>(c));
        }
        break;
    }
  }
  out.push_back('"');
}

void DumpJsonImpl(const JsonValue& value, std::string& out, int indent_step, int depth) {
  if (value.IsNull()) {
    out += "null";
  } else if (const auto* b = std::get_if<bool>(&value.data)) {
    out += (*b ? "true" : "false");
  } else if (const auto* i = std::get_if<std::int64_t>(&value.data)) {
    out += std::to_string(*i);
  } else if (const auto* d = std::get_if<double>(&value.data)) {
    std::ostringstream ss;
    ss << *d;
    out += ss.str();
  } else if (const auto* s = std::get_if<std::string>(&value.data)) {
    WriteJsonString(out, *s);
  } else if (const auto* arr = std::get_if<JsonValue::Array>(&value.data)) {
    if (arr->empty()) {
      out += "[]";
      return;
    }
    out.push_back('[');
    for (std::size_t idx = 0; idx < arr->size(); ++idx) {
      if (idx > 0) {
        out.push_back(',');
      }
      if (indent_step > 0) {
        out.push_back('\n');
        out.append(static_cast<std::size_t>((depth + 1) * indent_step), ' ');
      }
      DumpJsonImpl((*arr)[idx], out, indent_step, depth + 1);
    }
    if (indent_step > 0) {
      out.push_back('\n');
      out.append(static_cast<std::size_t>(depth * indent_step), ' ');
    }
    out.push_back(']');
  } else if (const auto* obj = std::get_if<JsonValue::Object>(&value.data)) {
    if (obj->empty()) {
      out += "{}";
      return;
    }
    out.push_back('{');
    for (std::size_t idx = 0; idx < obj->size(); ++idx) {
      if (idx > 0) {
        out.push_back(',');
      }
      if (indent_step > 0) {
        out.push_back('\n');
        out.append(static_cast<std::size_t>((depth + 1) * indent_step), ' ');
      }
      WriteJsonString(out, (*obj)[idx].first);
      out.push_back(':');
      if (indent_step > 0) {
        out.push_back(' ');
      }
      DumpJsonImpl((*obj)[idx].second, out, indent_step, depth + 1);
    }
    if (indent_step > 0) {
      out.push_back('\n');
      out.append(static_cast<std::size_t>(depth * indent_step), ' ');
    }
    out.push_back('}');
  }
}

std::string DumpJson(const JsonValue& value, int indent_step = 0) {
  std::string out;
  DumpJsonImpl(value, out, indent_step, 0);
  return out;
}

// ---------------------------------------------------------------------------
// MCP tool helpers
// ---------------------------------------------------------------------------

std::string EffectiveDriverPath(std::string_view driver_path, std::string_view uri) {
  if (!driver_path.empty()) {
    return std::string(driver_path);
  }
  const std::string inferred = InferDriverForUri(uri);
  if (inferred == "sqlite" || inferred.empty()) {
    return DefaultSqliteDriverPath();
  }
  return inferred;
}

bool ResolveConnectionFromArgs(const McpServerConfig& config, const JsonValue* args,
                               ConnectionRequest& req_out, std::string& error_out) {
  if (args == nullptr || !args->IsObject()) {
    error_out = "Missing tool arguments object";
    return false;
  }
  const std::string profile_key = args->Find("profile") ? args->Find("profile")->AsString() : "";
  const std::string direct_uri = args->Find("uri") ? args->Find("uri")->AsString() : "";
  const std::string direct_driver =
      args->Find("driver_path") ? args->Find("driver_path")->AsString() : "";
  const std::string direct_user = args->Find("username") ? args->Find("username")->AsString() : "";
  const std::string direct_pass = args->Find("password") ? args->Find("password")->AsString() : "";

  if (!profile_key.empty()) {
    if (config.profiles_db_path.empty()) {
      error_out = "No profile database path configured";
      return false;
    }
    const std::vector<ConnectionProfile> profiles = LoadConnectionProfiles(config.profiles_db_path);
    const ConnectionProfile* found = nullptr;
    for (const ConnectionProfile& p : profiles) {
      if (p.name == profile_key || LowerAsciiCopy(p.name) == LowerAsciiCopy(profile_key)) {
        found = &p;
        break;
      }
    }
    if (found == nullptr) {
      std::size_t idx = 0;
      const auto [end, ec] =
          std::from_chars(profile_key.data(), profile_key.data() + profile_key.size(), idx);
      if (ec == std::errc{} && end == profile_key.data() + profile_key.size() &&
          idx < profiles.size()) {
        found = &profiles[idx];
      }
    }
    if (found == nullptr) {
      error_out = "Saved connection profile not found: " + profile_key;
      return false;
    }
    const SanitizedUriResult sanitized = SanitizeUriCredentials(found->uri);
    req_out.driver_path = EffectiveDriverPath(found->driver_path, sanitized.uri);
    req_out.uri = sanitized.uri;
    req_out.username = found->username.empty() ? sanitized.username : found->username;
    req_out.password = found->password.empty() ? sanitized.password : found->password;
    req_out.title = found->name.empty() ? DisplayNameFromUri(sanitized.uri, "Database") : found->name;
    req_out.read_only = config.read_only;
    return true;
  }

  if (!direct_uri.empty()) {
    const SanitizedUriResult sanitized = SanitizeUriCredentials(direct_uri);
    req_out.driver_path = EffectiveDriverPath(direct_driver, sanitized.uri);
    req_out.uri = sanitized.uri;
    req_out.username = direct_user.empty() ? sanitized.username : direct_user;
    req_out.password = direct_pass.empty() ? sanitized.password : direct_pass;
    req_out.title = DisplayNameFromUri(sanitized.uri, "Database");
    req_out.read_only = config.read_only;
    return true;
  }

  error_out = "Provide either `profile` (saved connection name/index) or `uri`";
  return false;
}

struct ScopedAdbcSession {
  std::shared_ptr<AdbcSession> session;
  ~ScopedAdbcSession() {
    if (session) {
      session->MarkClosed();
      session->ReleaseResourcesBlocking();
    }
  }
};

JsonValue MakeToolTextResult(const JsonValue& payload, bool is_error = false) {
  JsonValue item = MakeObject({
      {"type", JsonValue{std::string("text")}},
      {"text", JsonValue{DumpJson(payload, 2)}},
  });
  return MakeObject({
      {"content", MakeArray({std::move(item)})},
      {"isError", JsonValue{is_error}},
  });
}

JsonValue MakeToolErrorText(std::string_view message) {
  JsonValue item = MakeObject({
      {"type", JsonValue{std::string("text")}},
      {"text", JsonValue{std::string(message)}},
  });
  return MakeObject({
      {"content", MakeArray({std::move(item)})},
      {"isError", JsonValue{true}},
  });
}

JsonValue BuildToolsListResult() {
  const auto conn_props = [] {
    return MakeObject({
        {"profile", MakeObject({{"type", JsonValue{std::string("string")}},
                                {"description", JsonValue{std::string(
                                    "Saved connection name or 0-based index from list_profiles")}}})},
        {"uri", MakeObject({{"type", JsonValue{std::string("string")}},
                            {"description", JsonValue{std::string(
                                "Direct database URI or SQLite file path (alternative to profile)")}}})},
        {"driver_path", MakeObject({{"type", JsonValue{std::string("string")}},
                                    {"description", JsonValue{std::string(
                                        "Optional ADBC driver name, manifest path, or shared library path")}}})},
        {"username", MakeObject({{"type", JsonValue{std::string("string")}},
                                 {"description", JsonValue{std::string("Optional username")}}})},
        {"password", MakeObject({{"type", JsonValue{std::string("string")}},
                                 {"description", JsonValue{std::string("Optional password")}}})},
    });
  };

  JsonValue t_profiles = MakeObject({
      {"name", JsonValue{std::string("list_profiles")}},
      {"description", JsonValue{std::string(
          "List saved db++ connection profiles (credentials omitted).")}},
      {"inputSchema", MakeObject({{"type", JsonValue{std::string("object")}},
                                  {"properties", MakeObject()}})},
  });

  JsonValue t_tables = MakeObject({
      {"name", JsonValue{std::string("list_tables")}},
      {"description", JsonValue{std::string(
          "List tables, views, or Redis databases for a connection profile or URI.")}},
      {"inputSchema", MakeObject({{"type", JsonValue{std::string("object")}},
                                  {"properties", conn_props()}})},
  });

  JsonValue describe_props = conn_props();
  describe_props.Set("table", MakeObject({{"type", JsonValue{std::string("string")}},
                                          {"description", JsonValue{std::string(
                                              "Table, view, or Redis DB name")}}}));
  describe_props.Set("schema", MakeObject({{"type", JsonValue{std::string("string")}},
                                           {"description", JsonValue{std::string(
                                               "Optional database/schema qualifier")}}}));
  JsonValue t_describe = MakeObject({
      {"name", JsonValue{std::string("describe_table")}},
      {"description", JsonValue{std::string(
          "Inspect column names, declared types, nullability, and primary keys.")}},
      {"inputSchema", MakeObject({{"type", JsonValue{std::string("object")}},
                                  {"properties", std::move(describe_props)},
                                  {"required", MakeArray({JsonValue{std::string("table")}})}})},
  });

  JsonValue query_props = conn_props();
  query_props.Set("sql", MakeObject({{"type", JsonValue{std::string("string")}},
                                     {"description", JsonValue{std::string(
                                         "SQL query (or Redis command) to execute")}}}));
  query_props.Set("max_rows", MakeObject({{"type", JsonValue{std::string("integer")}},
                                          {"description", JsonValue{std::string(
                                              "Maximum rows to return (default 200, max 1000)")}}}));
  JsonValue t_query = MakeObject({
      {"name", JsonValue{std::string("run_query")}},
      {"description", JsonValue{std::string(
          "Execute SQL or Redis statement against a connection (enforces read-only guard when enabled).")}},
      {"inputSchema", MakeObject({{"type", JsonValue{std::string("object")}},
                                  {"properties", std::move(query_props)},
                                  {"required", MakeArray({JsonValue{std::string("sql")}})}})},
  });

  JsonValue t_info = MakeObject({
      {"name", JsonValue{std::string("get_connection_info")}},
      {"description", JsonValue{std::string(
          "Get ADBC vendor, driver, Arrow version, and supported table types.")}},
      {"inputSchema", MakeObject({{"type", JsonValue{std::string("object")}},
                                  {"properties", conn_props()}})},
  });

  JsonValue stats_props = conn_props();
  stats_props.Set("table", MakeObject({{"type", JsonValue{std::string("string")}},
                                       {"description", JsonValue{std::string("Table name")}}}));
  stats_props.Set("schema", MakeObject({{"type", JsonValue{std::string("string")}},
                                        {"description", JsonValue{std::string("Optional schema")}}}));
  JsonValue t_stats = MakeObject({
      {"name", JsonValue{std::string("get_table_stats")}},
      {"description", JsonValue{std::string(
          "Retrieve ADBC table/column statistics (row count, null count, NDV, min/max).")}},
      {"inputSchema", MakeObject({{"type", JsonValue{std::string("object")}},
                                  {"properties", std::move(stats_props)},
                                  {"required", MakeArray({JsonValue{std::string("table")}})}})},
  });

  JsonValue schema_props = conn_props();
  schema_props.Set("sql", MakeObject({{"type", JsonValue{std::string("string")}},
                                      {"description", JsonValue{std::string("SQL query to inspect")}}}));
  JsonValue t_inspect_schema = MakeObject({
      {"name", JsonValue{std::string("inspect_query_schema")}},
      {"description", JsonValue{std::string(
          "Dry-run inspect the output column names and types of a SQL query via ADBC.")}},
      {"inputSchema", MakeObject({{"type", JsonValue{std::string("object")}},
                                  {"properties", std::move(schema_props)},
                                  {"required", MakeArray({JsonValue{std::string("sql")}})}})},
  });

  return MakeObject({
      {"tools", MakeArray({std::move(t_profiles), std::move(t_tables), std::move(t_describe),
                           std::move(t_query), std::move(t_info), std::move(t_stats),
                           std::move(t_inspect_schema)})},
  });
}

JsonValue HandleToolCall(const McpServerConfig& config, const JsonValue* params) {
  if (params == nullptr || !params->IsObject()) {
    return MakeToolErrorText("Missing params object");
  }
  const std::string name = params->Find("name") ? params->Find("name")->AsString() : "";
  const JsonValue* args = params->Find("arguments");

  if (name == "list_profiles") {
    if (config.profiles_db_path.empty()) {
      return MakeToolTextResult(MakeObject({{"profiles", MakeArray()}}));
    }
    const std::vector<ConnectionProfile> profiles = LoadConnectionProfiles(config.profiles_db_path);
    JsonValue::Array arr;
    arr.reserve(profiles.size());
    for (std::size_t i = 0; i < profiles.size(); ++i) {
      const ConnectionProfile& p = profiles[i];
      arr.push_back(MakeObject({
          {"index", JsonValue{static_cast<std::int64_t>(i)}},
          {"name", JsonValue{p.name.empty() ? DisplayNameFromUri(p.uri, "Database") : p.name}},
          {"driver_path", JsonValue{p.driver_path}},
          {"uri", JsonValue{p.uri}},
          {"username", JsonValue{p.username}},
      }));
    }
    return MakeToolTextResult(MakeObject({
        {"count", JsonValue{static_cast<std::int64_t>(arr.size())}},
        {"profiles", MakeArray(std::move(arr))},
    }));
  }

  ConnectionRequest req;
  std::string err;
  if (!ResolveConnectionFromArgs(config, args, req, err)) {
    return MakeToolErrorText(err);
  }
  AdbcOpenResult opened = OpenAdbcBlocking(req);
  if (!opened.session) {
    return MakeToolErrorText("Failed to open connection: " + opened.error);
  }
  ScopedAdbcSession guard{opened.session};

  if (name == "list_tables" || name == "run_query" || name == "describe_table" ||
      name == "get_connection_info" || name == "get_table_stats" ||
      name == "inspect_query_schema") {
    const std::string table_name =
        (args != nullptr && args->Find("table")) ? args->Find("table")->AsString() : "";
    const std::string schema_name =
        (args != nullptr && args->Find("schema")) ? args->Find("schema")->AsString() : "";
    const std::string sql =
        (args != nullptr && args->Find("sql")) ? args->Find("sql")->AsString() : "";
    std::int64_t max_rows = 200;
    if (args != nullptr && args->Find("max_rows")) {
      if (const auto requested = args->Find("max_rows")->AsInt64()) {
        max_rows = *requested;
      }
    }
    const detail::McpExtendedToolOutput ext = detail::McpHandleExtendedToolCall(
        name, *guard.session, table_name, schema_name, sql, true, config.read_only, max_rows);
    if (ext.is_error) {
      return MakeToolErrorText(ext.payload_json);
    }
    return MakeObject({
        {"content", MakeArray({MakeObject({
                        {"type", JsonValue{std::string("text")}},
                        {"text", JsonValue{ext.payload_json}},
                    })})},
        {"isError", JsonValue{false}},
    });
  }

  return MakeToolErrorText("Unknown tool: " + name);
}

JsonValue MakeRpcError(const JsonValue& id, std::int64_t code, std::string message) {
  return MakeObject({
      {"jsonrpc", JsonValue{std::string("2.0")}},
      {"id", id},
      {"error", MakeObject({
                    {"code", JsonValue{code}},
                    {"message", JsonValue{std::move(message)}},
                })},
  });
}

JsonValue MakeRpcResult(const JsonValue& id, JsonValue result) {
  return MakeObject({
      {"jsonrpc", JsonValue{std::string("2.0")}},
      {"id", id},
      {"result", std::move(result)},
  });
}

using HttpResponse = detail::McpHttpResponse;

HttpResponse HandleJsonRpcBody(const McpServerConfig& config, std::string_view body) {
  JsonValue req;
  JsonParser parser(body);
  if (!parser.Parse(req) || !req.IsObject()) {
    return HttpResponse{
        .status_code = 200,
        .status_text = "OK",
        .body = DumpJson(MakeRpcError(JsonValue{nullptr}, -32700, "Parse error")),
    };
  }

  const JsonValue* id_ptr = req.Find("id");
  const bool is_notification = (id_ptr == nullptr || id_ptr->IsNull());
  const std::string method = req.Find("method") ? req.Find("method")->AsString() : "";
  if (method.empty()) {
    if (is_notification) {
      return HttpResponse{.status_code = 202, .status_text = "Accepted", .body = {}};
    }
    return HttpResponse{
        .status_code = 200,
        .status_text = "OK",
        .body = DumpJson(MakeRpcError(*id_ptr, -32600, "Invalid Request: missing method")),
    };
  }

  if (method == "notifications/initialized" || method.starts_with("notifications/")) {
    return HttpResponse{.status_code = 202, .status_text = "Accepted", .body = {}};
  }

  if (is_notification) {
    return HttpResponse{.status_code = 202, .status_text = "Accepted", .body = {}};
  }

  const JsonValue& id = *id_ptr;
  if (method == "initialize") {
    return HttpResponse{
        .status_code = 200,
        .status_text = "OK",
        .body = DumpJson(MakeRpcResult(
            id,
            MakeObject({
                {"protocolVersion", JsonValue{std::string("2025-03-26")}},
                {"capabilities", MakeObject({{"tools", MakeObject({{"listChanged", JsonValue{false}}})}})},
                {"serverInfo", MakeObject({
                                   {"name", JsonValue{std::string("dbpp")}},
                                   {"version", JsonValue{std::string(DBPP_VERSION)}},
                               })},
            }))),
    };
  }

  if (method == "ping") {
    return HttpResponse{
        .status_code = 200,
        .status_text = "OK",
        .body = DumpJson(MakeRpcResult(id, MakeObject())),
    };
  }

  if (method == "tools/list") {
    return HttpResponse{
        .status_code = 200,
        .status_text = "OK",
        .body = DumpJson(MakeRpcResult(id, BuildToolsListResult())),
    };
  }

  if (method == "tools/call") {
    return HttpResponse{
        .status_code = 200,
        .status_text = "OK",
        .body = DumpJson(MakeRpcResult(id, HandleToolCall(config, req.Find("params")))),
    };
  }

  return HttpResponse{
      .status_code = 200,
      .status_text = "OK",
      .body = DumpJson(MakeRpcError(id, -32601, "Method not found: " + method)),
  };
}


} // namespace

namespace detail {

std::string McpLowerAscii(std::string_view text) {
  return LowerAsciiCopy(text);
}

std::string_view McpTrimView(std::string_view text) {
  return TrimView(text);
}

std::string McpBuildHealthJson(bool read_only) {
  return DumpJson(MakeObject({
      {"status", JsonValue{std::string("ok")}},
      {"service", JsonValue{std::string("dbpp-mcp")}},
      {"version", JsonValue{std::string(DBPP_VERSION)}},
      {"read_only", JsonValue{read_only}},
  }));
}

McpHttpResponse HandleMcpJsonRpcBody(const McpServerConfig& config, std::string_view body) {
  return HandleJsonRpcBody(config, body);
}

} // namespace detail

std::string BuildMcpClientConfigJson(std::string_view endpoint_url) {
  JsonValue root = MakeObject({
      {"mcpServers", MakeObject({
                         {"dbpp", MakeObject({
                                      {"type", JsonValue{std::string("http")}},
                                      {"url", JsonValue{std::string(endpoint_url)}},
                                  })},
                     })},
  });
  return DumpJson(root, 2);
}

} // namespace dbpp
