module;

#include <huxerui/state.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

module dbpp.mcp;

import std;
import dbpp.settings;
import dbpp.types;
import dbpp.adbc;
import dbpp.db;

#ifndef DBPP_VERSION
#define DBPP_VERSION "0.3.4"
#endif

namespace dbpp {

namespace {

#ifdef _WIN32
using NativeSocket = SOCKET;
inline constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;
int PollSockets(pollfd* fds, unsigned long count, int timeout_ms) {
  return ::WSAPoll(fds, count, timeout_ms);
}
void CloseNativeSocket(NativeSocket fd) noexcept {
  if (fd != INVALID_SOCKET) {
    ::closesocket(fd);
  }
}
bool EnsureWsaInitialized() {
  static const bool initialized = [] {
    WSADATA data{};
    return ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  return initialized;
}
std::string LastSocketErrorString() {
  return "WSA error " + std::to_string(::WSAGetLastError());
}
void SetSocketTimeouts(NativeSocket fd) noexcept {
  const DWORD timeout_ms = 5000;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms),
               sizeof(timeout_ms));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout_ms),
               sizeof(timeout_ms));
}
#else
using NativeSocket = int;
inline constexpr NativeSocket kInvalidSocket = -1;
int PollSockets(pollfd* fds, nfds_t count, int timeout_ms) {
  return ::poll(fds, count, timeout_ms);
}
void CloseNativeSocket(NativeSocket fd) noexcept {
  if (fd >= 0) {
    ::close(fd);
  }
}
bool EnsureWsaInitialized() {
  return true;
}
std::string LastSocketErrorString() {
  return std::strerror(errno);
}
void SetSocketTimeouts(NativeSocket fd) noexcept {
  struct timeval tv {};
  tv.tv_sec = 5;
  tv.tv_usec = 0;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#ifdef SO_NOSIGPIPE
  const int on = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
}
#endif

struct UniqueSocket {
  NativeSocket fd = kInvalidSocket;

  UniqueSocket() = default;
  explicit UniqueSocket(NativeSocket handle) noexcept : fd(handle) {}
  UniqueSocket(const UniqueSocket&) = delete;
  UniqueSocket& operator=(const UniqueSocket&) = delete;
  UniqueSocket(UniqueSocket&& other) noexcept : fd(std::exchange(other.fd, kInvalidSocket)) {}
  UniqueSocket& operator=(UniqueSocket&& other) noexcept {
    if (this != &other) {
      Reset();
      fd = std::exchange(other.fd, kInvalidSocket);
    }
    return *this;
  }
  ~UniqueSocket() { Reset(); }

  void Reset(NativeSocket next = kInvalidSocket) noexcept {
    if (fd != kInvalidSocket) {
      CloseNativeSocket(fd);
    }
    fd = next;
  }

  [[nodiscard]] bool Valid() const noexcept { return fd != kInvalidSocket; }
};

struct AddrInfoGuard {
  struct addrinfo* ptr = nullptr;
  ~AddrInfoGuard() {
    if (ptr != nullptr) {
      ::freeaddrinfo(ptr);
    }
  }
};

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
  [[nodiscard]] bool IsBool() const noexcept { return std::holds_alternative<bool>(data); }
  [[nodiscard]] bool IsInt() const noexcept { return std::holds_alternative<std::int64_t>(data); }
  [[nodiscard]] bool IsDouble() const noexcept { return std::holds_alternative<double>(data); }
  [[nodiscard]] bool IsString() const noexcept { return std::holds_alternative<std::string>(data); }
  [[nodiscard]] bool IsArray() const noexcept { return std::holds_alternative<Array>(data); }
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
    return true;
  }

  if (!direct_uri.empty()) {
    const SanitizedUriResult sanitized = SanitizeUriCredentials(direct_uri);
    req_out.driver_path = EffectiveDriverPath(direct_driver, sanitized.uri);
    req_out.uri = sanitized.uri;
    req_out.username = direct_user.empty() ? sanitized.username : direct_user;
    req_out.password = direct_pass.empty() ? sanitized.password : direct_pass;
    req_out.title = DisplayNameFromUri(sanitized.uri, "Database");
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

  return MakeObject({
      {"tools", MakeArray({std::move(t_profiles), std::move(t_tables), std::move(t_describe),
                           std::move(t_query)})},
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

  if (name == "list_tables") {
    const AdbcObjectsResult objs = LoadObjectsBlocking(*guard.session);
    if (!objs.error.empty() && objs.objects.empty()) {
      return MakeToolErrorText("Failed to list tables: " + objs.error);
    }
    JsonValue::Array arr;
    arr.reserve(objs.objects.size());
    for (const DbObject& o : objs.objects) {
      arr.push_back(MakeObject({
          {"name", JsonValue{o.name}},
          {"schema", JsonValue{o.db_schema}},
          {"catalog", JsonValue{o.catalog}},
          {"table_type", JsonValue{o.table_type}},
          {"is_view", JsonValue{o.is_view}},
      }));
    }
    return MakeToolTextResult(MakeObject({
        {"count", JsonValue{static_cast<std::int64_t>(arr.size())}},
        {"objects", MakeArray(std::move(arr))},
    }));
  }

  if (name == "describe_table") {
    const std::string table_name =
        (args != nullptr && args->Find("table")) ? args->Find("table")->AsString() : "";
    const std::string schema_name =
        (args != nullptr && args->Find("schema")) ? args->Find("schema")->AsString() : "";
    if (table_name.empty()) {
      return MakeToolErrorText("Missing required argument `table`");
    }
    DbObject target;
    target.name = table_name;
    target.db_schema = schema_name;
    const AdbcSchemaResult schema = LoadSchemaBlocking(*guard.session, target);
    if (!schema.error.empty() && schema.columns.empty()) {
      return MakeToolErrorText("Failed to describe table: " + schema.error);
    }
    JsonValue::Array cols;
    cols.reserve(schema.columns.size());
    for (const ColumnMeta& col : schema.columns) {
      cols.push_back(MakeObject({
          {"name", JsonValue{col.name}},
          {"type", JsonValue{col.type}},
          {"nullable", JsonValue{col.nullable}},
          {"primary_key", JsonValue{col.primary_key}},
      }));
    }
    return MakeToolTextResult(MakeObject({
        {"table", JsonValue{table_name}},
        {"schema", JsonValue{schema_name}},
        {"columns", MakeArray(std::move(cols))},
    }));
  }

  if (name == "run_query") {
    const std::string sql =
        (args != nullptr && args->Find("sql")) ? args->Find("sql")->AsString() : "";
    if (TrimView(sql).empty()) {
      return MakeToolErrorText("Missing required argument `sql`");
    }
    const bool is_read = IsReadStatement(guard.session->dialect, sql);
    if (config.read_only && !is_read) {
      return MakeToolErrorText(
          "Read-only mode is enabled in db++ MCP settings; write/DDL statements are blocked.");
    }
    std::int64_t max_rows = 200;
    if (args != nullptr && args->Find("max_rows")) {
      if (const auto requested = args->Find("max_rows")->AsInt64()) {
        max_rows = std::clamp<std::int64_t>(*requested, 1, 1000);
      }
    }
    const AdbcQueryResult res =
        ExecuteBlocking(*guard.session, sql, is_read, static_cast<std::size_t>(max_rows));
    if (!res.error.empty() && res.rows.empty()) {
      return MakeToolErrorText("Query failed: " + res.error);
    }

    JsonValue::Array col_names;
    col_names.reserve(res.columns.size());
    for (const std::string& c : res.columns) {
      col_names.push_back(JsonValue{c});
    }
    JsonValue::Array rows_json;
    rows_json.reserve(res.rows.size());
    for (const GridRow& row : res.rows) {
      JsonValue::Array cells;
      cells.reserve(row.values.size());
      for (const CellValue& cell : row.values) {
        if (cell.is_null) {
          cells.push_back(JsonValue{nullptr});
        } else {
          cells.push_back(JsonValue{cell.text});
        }
      }
      rows_json.push_back(MakeArray(std::move(cells)));
    }
    return MakeToolTextResult(MakeObject({
        {"columns", MakeArray(std::move(col_names))},
        {"rows", MakeArray(std::move(rows_json))},
        {"row_count", JsonValue{static_cast<std::int64_t>(res.rows.size())}},
        {"truncated", JsonValue{res.truncated}},
        {"affected", JsonValue{res.affected}},
    }));
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

struct HttpResponse {
  int status_code = 200;
  std::string status_text = "OK";
  std::string content_type = "application/json; charset=utf-8";
  std::string body;
};

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

// ---------------------------------------------------------------------------
// HTTP/1.1 connection framing
// ---------------------------------------------------------------------------

constexpr std::size_t kMaxHttpHeaderBytes = 64 * 1024;
constexpr std::size_t kMaxHttpBodyBytes = 4 * 1024 * 1024;

bool SendAll(NativeSocket fd, std::string_view data) {
  std::size_t sent = 0;
  while (sent < data.size()) {
#ifdef MSG_NOSIGNAL
    const int flags = MSG_NOSIGNAL;
#else
    const int flags = 0;
#endif
    const auto chunk = static_cast<int>(
        std::min<std::size_t>(data.size() - sent, 65536));
    const auto n = ::send(fd, data.data() + sent, chunk, flags);
    if (n <= 0) {
      return false;
    }
    sent += static_cast<std::size_t>(n);
  }
  return true;
}

void WriteHttpResponse(NativeSocket fd, const HttpResponse& res) {
  std::string raw;
  raw.reserve(256 + res.body.size());
  raw += "HTTP/1.1 ";
  raw += std::to_string(res.status_code);
  raw.push_back(' ');
  raw += res.status_text;
  raw += "\r\nContent-Type: ";
  raw += res.content_type;
  raw += "\r\nContent-Length: ";
  raw += std::to_string(res.body.size());
  raw += "\r\nAccess-Control-Allow-Origin: *";
  raw += "\r\nAccess-Control-Allow-Methods: GET, POST, OPTIONS";
  raw += "\r\nAccess-Control-Allow-Headers: Content-Type, Accept, Mcp-Session-Id";
  raw += "\r\nCache-Control: no-store";
  raw += "\r\nConnection: close\r\n\r\n";
  raw += res.body;
  (void)SendAll(fd, raw);
}

void HandleHttpClient(NativeSocket client_fd, const McpServerConfig& config) {
  SetSocketTimeouts(client_fd);

  std::string buffer;
  buffer.reserve(4096);
  std::size_t header_end = std::string::npos;
  char chunk[4096];

  while (header_end == std::string::npos) {
    const auto n = ::recv(client_fd, chunk, sizeof(chunk), 0);
    if (n <= 0) {
      return;
    }
    buffer.append(chunk, static_cast<std::size_t>(n));
    header_end = buffer.find("\r\n\r\n");
    if (header_end == std::string::npos && buffer.size() > kMaxHttpHeaderBytes) {
      WriteHttpResponse(client_fd, HttpResponse{
                                       .status_code = 431,
                                       .status_text = "Request Header Fields Too Large",
                                       .body = R"({"error":"Headers too large"})",
                                   });
      return;
    }
  }

  const std::string_view header_block(buffer.data(), header_end);
  const std::size_t first_crlf = header_block.find("\r\n");
  const std::string_view request_line =
      first_crlf == std::string_view::npos ? header_block : header_block.substr(0, first_crlf);

  const std::size_t sp1 = request_line.find(' ');
  const std::size_t sp2 = sp1 == std::string_view::npos ? std::string_view::npos
                                                        : request_line.find(' ', sp1 + 1);
  if (sp1 == std::string_view::npos) {
    WriteHttpResponse(client_fd, HttpResponse{
                                     .status_code = 400,
                                     .status_text = "Bad Request",
                                     .body = R"({"error":"Malformed request line"})",
                                 });
    return;
  }

  const std::string method = LowerAsciiCopy(request_line.substr(0, sp1));
  std::string_view raw_target =
      sp2 == std::string_view::npos ? request_line.substr(sp1 + 1)
                                    : request_line.substr(sp1 + 1, sp2 - sp1 - 1);
  if (const auto q = raw_target.find('?'); q != std::string_view::npos) {
    raw_target = raw_target.substr(0, q);
  }
  const std::string path(raw_target);

  std::size_t content_length = 0;
  if (first_crlf != std::string_view::npos) {
    std::string_view rest = header_block.substr(first_crlf + 2);
    while (!rest.empty()) {
      const auto next_crlf = rest.find("\r\n");
      const std::string_view line =
          next_crlf == std::string_view::npos ? rest : rest.substr(0, next_crlf);
      if (const auto colon = line.find(':'); colon != std::string_view::npos) {
        const std::string hdr_name = LowerAsciiCopy(TrimView(line.substr(0, colon)));
        if (hdr_name == "content-length") {
          const std::string_view val = TrimView(line.substr(colon + 1));
          std::size_t parsed = 0;
          const auto [end, ec] = std::from_chars(val.data(), val.data() + val.size(), parsed);
          if (ec != std::errc{} || end != val.data() + val.size() || parsed > kMaxHttpBodyBytes) {
            WriteHttpResponse(client_fd, HttpResponse{
                                             .status_code = 413,
                                             .status_text = "Payload Too Large",
                                             .body = R"({"error":"Payload too large"})",
                                         });
            return;
          }
          content_length = parsed;
        }
      }
      if (next_crlf == std::string_view::npos) {
        break;
      }
      rest.remove_prefix(next_crlf + 2);
    }
  }

  const std::size_t body_start = header_end + 4;
  while (buffer.size() - body_start < content_length) {
    const auto n = ::recv(client_fd, chunk, sizeof(chunk), 0);
    if (n <= 0) {
      return;
    }
    buffer.append(chunk, static_cast<std::size_t>(n));
    if (buffer.size() - body_start > kMaxHttpBodyBytes) {
      WriteHttpResponse(client_fd, HttpResponse{
                                       .status_code = 413,
                                       .status_text = "Payload Too Large",
                                       .body = R"({"error":"Payload too large"})",
                                   });
      return;
    }
  }

  const std::string_view body(buffer.data() + body_start, content_length);

  if (method == "options") {
    WriteHttpResponse(client_fd, HttpResponse{
                                     .status_code = 204,
                                     .status_text = "No Content",
                                     .content_type = "text/plain; charset=utf-8",
                                     .body = {},
                                 });
    return;
  }

  if (method == "get" && path == "/health") {
    WriteHttpResponse(
        client_fd,
        HttpResponse{
            .status_code = 200,
            .status_text = "OK",
            .body = DumpJson(MakeObject({
                {"status", JsonValue{std::string("ok")}},
                {"service", JsonValue{std::string("dbpp-mcp")}},
                {"version", JsonValue{std::string(DBPP_VERSION)}},
                {"read_only", JsonValue{config.read_only}},
            })),
        });
    return;
  }

  if (method == "post" && (path == "/mcp" || path == "/")) {
    WriteHttpResponse(client_fd, HandleJsonRpcBody(config, body));
    return;
  }

  WriteHttpResponse(client_fd, HttpResponse{
                                   .status_code = 404,
                                   .status_text = "Not Found",
                                   .body = R"({"error":"Not found"})",
                               });
}

bool OpenListenSocket(const std::string& host, std::uint16_t port, UniqueSocket& socket_out,
                      std::uint16_t& bound_port_out, std::string& error_out) {
  if (!EnsureWsaInitialized()) {
    error_out = "Failed to initialize Winsock";
    return false;
  }

  const std::string effective_host = NormalizeMcpHost(host);
  const std::string port_str = std::to_string(port);

  struct addrinfo hints {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;

  AddrInfoGuard res;
  const int gai = ::getaddrinfo(effective_host.c_str(), port_str.c_str(), &hints, &res.ptr);
  if (gai != 0 || res.ptr == nullptr) {
    // Fallback to AF_UNSPEC (e.g. IPv6 literal)
    hints.ai_family = AF_UNSPEC;
    res.ptr = nullptr;
    const int gai2 = ::getaddrinfo(effective_host.c_str(), port_str.c_str(), &hints, &res.ptr);
    if (gai2 != 0 || res.ptr == nullptr) {
      error_out = "Failed to resolve host `" + effective_host + "`";
      return false;
    }
  }

  for (struct addrinfo* rp = res.ptr; rp != nullptr; rp = rp->ai_next) {
    UniqueSocket sock(::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol));
    if (!sock.Valid()) {
      continue;
    }
#ifndef _WIN32
    const int opt = 1;
    ::setsockopt(sock.fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif
    if (::bind(sock.fd, rp->ai_addr, static_cast<socklen_t>(rp->ai_addrlen)) != 0) {
      error_out = "Bind failed on " + effective_host + ":" + port_str + ": " + LastSocketErrorString();
      continue;
    }
    if (::listen(sock.fd, 16) != 0) {
      error_out = "Listen failed: " + LastSocketErrorString();
      continue;
    }

    struct sockaddr_storage bound {};
    socklen_t bound_len = sizeof(bound);
    if (::getsockname(sock.fd, reinterpret_cast<struct sockaddr*>(&bound), &bound_len) != 0) {
      error_out = "getsockname failed: " + LastSocketErrorString();
      continue;
    }
    if (bound.ss_family == AF_INET) {
      bound_port_out = ntohs(reinterpret_cast<const struct sockaddr_in*>(&bound)->sin_port);
    } else if (bound.ss_family == AF_INET6) {
      bound_port_out = ntohs(reinterpret_cast<const struct sockaddr_in6*>(&bound)->sin6_port);
    } else {
      bound_port_out = port;
    }
    socket_out = std::move(sock);
    error_out.clear();
    return true;
  }

  if (error_out.empty()) {
    error_out = "Unable to bind socket on " + effective_host + ":" + port_str;
  }
  return false;
}

} // namespace

struct McpServer::Impl {
  mutable std::mutex mutex;
  McpServerConfig config;
  UniqueSocket listen_socket;
  std::atomic<bool> running{false};
  std::atomic<bool> stop_requested{false};
  std::atomic<std::uint16_t> bound_port{0};
  std::string last_error;
  std::thread accept_thread;
};

McpServer::McpServer() : impl_(std::make_unique<Impl>()) {}

McpServer::~McpServer() {
  Stop();
}

bool McpServer::Start(const McpServerConfig& config, std::string& error_out) {
  Stop();

  McpServerConfig normalized = config;
  normalized.host = NormalizeMcpHost(normalized.host);

  UniqueSocket listen_sock;
  std::uint16_t actual_port = 0;
  std::string err;
  if (!OpenListenSocket(normalized.host, normalized.port, listen_sock, actual_port, err)) {
    std::scoped_lock lock(impl_->mutex);
    impl_->config = normalized;
    impl_->bound_port.store(0, std::memory_order_release);
    impl_->running.store(false, std::memory_order_release);
    impl_->last_error = err;
    error_out = std::move(err);
    return false;
  }

  {
    std::scoped_lock lock(impl_->mutex);
    impl_->config = normalized;
    impl_->listen_socket = std::move(listen_sock);
    impl_->bound_port.store(actual_port, std::memory_order_release);
    impl_->stop_requested.store(false, std::memory_order_release);
    impl_->running.store(true, std::memory_order_release);
    impl_->last_error.clear();
  }

  Impl* state = impl_.get();
  impl_->accept_thread = std::thread([state, normalized]() {
    for (;;) {
      if (state->stop_requested.load(std::memory_order_acquire)) {
        break;
      }
      NativeSocket fd = kInvalidSocket;
      {
        std::scoped_lock lock(state->mutex);
        fd = state->listen_socket.fd;
      }
      if (fd == kInvalidSocket) {
        break;
      }
      pollfd pfd{};
      pfd.fd = fd;
      pfd.events = POLLIN;
      const int ready = PollSockets(&pfd, 1, 100);
      if (ready <= 0) {
        continue;
      }
      if (state->stop_requested.load(std::memory_order_acquire)) {
        break;
      }
      UniqueSocket client(::accept(fd, nullptr, nullptr));
      if (!client.Valid()) {
        continue;
      }
      HandleHttpClient(client.fd, normalized);
    }
    state->running.store(false, std::memory_order_release);
  });

  error_out.clear();
  return true;
}

void McpServer::Stop() {
  if (!impl_) {
    return;
  }
  impl_->stop_requested.store(true, std::memory_order_release);
  std::thread to_join;
  {
    std::scoped_lock lock(impl_->mutex);
    if (impl_->accept_thread.joinable()) {
      to_join = std::move(impl_->accept_thread);
    }
  }
  if (to_join.joinable()) {
    to_join.join();
  }
  {
    std::scoped_lock lock(impl_->mutex);
    impl_->listen_socket.Reset();
    impl_->running.store(false, std::memory_order_release);
    impl_->bound_port.store(0, std::memory_order_release);
  }
}

bool McpServer::IsRunning() const noexcept {
  return impl_ && impl_->running.load(std::memory_order_acquire);
}

std::uint16_t McpServer::BoundPort() const noexcept {
  return impl_ ? impl_->bound_port.load(std::memory_order_acquire) : 0;
}

McpServerStatus McpServer::Status() const {
  if (!impl_) {
    return McpServerStatus{};
  }
  std::scoped_lock lock(impl_->mutex);
  return McpServerStatus{
      .running = impl_->running.load(std::memory_order_acquire),
      .host = impl_->config.host,
      .bound_port = impl_->bound_port.load(std::memory_order_acquire),
      .read_only = impl_->config.read_only,
      .error = impl_->last_error,
  };
}

std::string McpServer::EndpointUrl() const {
  const McpServerStatus st = Status();
  return BuildMcpEndpointUrl(st.host, st.bound_port != 0 ? st.bound_port : impl_->config.port);
}

bool McpServerService::ApplySettings(bool enabled, std::string host, std::uint16_t port,
                                     bool read_only) {
  const std::string norm_host = NormalizeMcpHost(host);
  const std::uint16_t effective_port = port == 0 ? kDefaultMcpPort : port;

  if (!enabled) {
    server.Stop();
    status = McpServerStatus{
        .running = false,
        .host = norm_host,
        .bound_port = 0,
        .read_only = read_only,
        .error = {},
    };
    return true;
  }

  const McpServerStatus current = server.Status();
  if (current.running && current.host == norm_host && current.bound_port == effective_port &&
      current.read_only == read_only) {
    status = current;
    return true;
  }

  std::string err;
  const bool ok = server.Start(
      McpServerConfig{
          .host = norm_host,
          .port = effective_port,
          .read_only = read_only,
          .profiles_db_path = profiles_db_path,
      },
      err);
  status = server.Status();
  return ok;
}

void McpServerService::Shutdown() {
  server.Stop();
  McpServerStatus st = status.Get();
  st.running = false;
  st.bound_port = 0;
  status = std::move(st);
}

std::string BuildMcpEndpointUrl(std::string_view host, std::uint16_t port) {
  const std::string h = NormalizeMcpHost(host);
  const std::uint16_t p = port == 0 ? kDefaultMcpPort : port;
  if (h.find(':') != std::string::npos && !h.starts_with('[')) {
    return "http://[" + h + "]:" + std::to_string(p) + "/mcp";
  }
  return "http://" + h + ":" + std::to_string(p) + "/mcp";
}

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
