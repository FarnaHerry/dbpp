module;

#include <arrow-adbc/adbc.h>
#include <toml++/toml.hpp>
#include <huxerui/datatable.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

#include <cstdio>

module dbpp.adbc;

import std;
import dbpp.types;
import dbpp.i18n;
import dbpp.redis;

namespace dbpp {

namespace {

bool DecodeUtf8Sequence(std::string_view text, std::size_t index, std::size_t& out_len) noexcept {
  out_len = 0;
  if (index >= text.size()) {
    return false;
  }
  const auto first = static_cast<std::uint8_t>(text[index]);
  if (first <= 0x7FU) {
    out_len = 1;
    return true;
  }
  std::size_t length = 0;
  std::uint32_t value = 0;
  std::uint32_t minimum = 0;
  if ((first & 0xE0U) == 0xC0U) {
    length = 2;
    value = first & 0x1FU;
    minimum = 0x80U;
  } else if ((first & 0xF0U) == 0xE0U) {
    length = 3;
    value = first & 0x0FU;
    minimum = 0x800U;
  } else if ((first & 0xF8U) == 0xF0U) {
    length = 4;
    value = first & 0x07U;
    minimum = 0x10000U;
  } else {
    return false;
  }
  if (length > text.size() - index) {
    return false;
  }
  for (std::size_t offset = 1; offset < length; ++offset) {
    const auto continuation = static_cast<std::uint8_t>(text[index + offset]);
    if ((continuation & 0xC0U) != 0x80U) {
      return false;
    }
    value = (value << 6U) | (continuation & 0x3FU);
  }
  if (value < minimum || value > 0x10FFFFU || (value >= 0xD800U && value <= 0xDFFFU)) {
    return false;
  }
  out_len = length;
  return true;
}

std::string DecodeSystemTextToUtf8(std::string_view raw) {
  if (raw.empty() || IsValidUtf8(raw)) {
    return std::string(raw);
  }
#if defined(_WIN32)
  if (raw.size() <= static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    const int src_len = static_cast<int>(raw.size());
    const int wide_len = MultiByteToWideChar(CP_ACP, 0, raw.data(), src_len, nullptr, 0);
    if (wide_len > 0) {
      std::wstring wide(static_cast<std::size_t>(wide_len), L'\0');
      if (MultiByteToWideChar(CP_ACP, 0, raw.data(), src_len, wide.data(), wide_len) ==
          wide_len) {
        const int utf8_len =
            WideCharToMultiByte(CP_UTF8, 0, wide.data(), wide_len, nullptr, 0, nullptr, nullptr);
        if (utf8_len > 0) {
          std::string utf8(static_cast<std::size_t>(utf8_len), '\0');
          if (WideCharToMultiByte(CP_UTF8, 0, wide.data(), wide_len, utf8.data(), utf8_len,
                                  nullptr, nullptr) == utf8_len) {
            return SanitizeUtf8(utf8);
          }
        }
      }
    }
  }
#endif
  return SanitizeUtf8(raw);
}

std::string LowerAscii(std::string_view text) {
  std::string result;
  result.reserve(text.size());
  for (const char c : text) {
    result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return result;
}

bool ContainsLower(std::string_view haystack, std::string_view needle) {
  return LowerAscii(haystack).find(LowerAscii(needle)) != std::string::npos;
}

#if defined(_WIN32)
constexpr char kSearchPathSeparator = ';';
#else
constexpr char kSearchPathSeparator = ':';
#endif

std::vector<std::string> SplitSearchPath(std::string_view value) {
  std::vector<std::string> entries;
  std::size_t start = 0;
  for (std::size_t index = 0; index <= value.size(); ++index) {
    if (index == value.size() || value[index] == kSearchPathSeparator) {
      if (index > start) {
        entries.emplace_back(value.substr(start, index - start));
      }
      start = index + 1;
    }
  }
  return entries;
}

std::string SystemAdbcDriversDir() {
#if defined(_WIN32)
  return {};
#elif defined(__APPLE__)
  return "/Library/Application Support/ADBC/Drivers";
#else
  return "/etc/adbc/drivers";
#endif
}

std::filesystem::path EnvAdbcDriversDir(const char* env_var) {
  const char* value = std::getenv(env_var);
  if (value == nullptr || *value == '\0') {
    return {};
  }
  return std::filesystem::path(value) / "etc" / "adbc" / "drivers";
}

bool HasTomlExtension(const std::filesystem::path& path) {
  return LowerAscii(path.extension().string()) == ".toml";
}

bool IsProfileManifest(const std::filesystem::path& path) {
  return LowerAscii(path.filename().string()) == "profile.toml";
}

std::string BuildAdditionalSearchPathList() {
  std::vector<std::string> dirs;
  std::set<std::string> seen;

  const auto add_dir = [&](const std::filesystem::path& dir) {
    if (dir.empty()) {
      return;
    }
    std::error_code error;
    if (!std::filesystem::is_directory(dir, error)) {
      return;
    }
    const std::string normalized = dir.lexically_normal().string();
    if (seen.insert(normalized).second) {
      dirs.push_back(dir.string());
    }
  };

  if (const char* dbpp_drivers = std::getenv("DBPP_ADBC_DRIVERS"); dbpp_drivers != nullptr) {
    for (const std::string& entry : SplitSearchPath(dbpp_drivers)) {
      const std::filesystem::path path(entry);
      std::error_code error;
      if (std::filesystem::is_directory(path, error)) {
        add_dir(path);
      } else if (std::filesystem::is_regular_file(path, error) && path.has_parent_path()) {
        add_dir(path.parent_path());
      }
    }
  }

  for (const char* env_var : {"VIRTUAL_ENV", "CONDA_PREFIX"}) {
    add_dir(EnvAdbcDriversDir(env_var));
  }

  std::string joined;
  for (std::size_t index = 0; index < dirs.size(); ++index) {
    if (index > 0) {
      joined.push_back(kSearchPathSeparator);
    }
    joined += dirs[index];
  }
  return joined;
}

std::optional<detail::ParsedDriverManifest> ParseDriverManifest(const std::filesystem::path& path) {
  if (!HasTomlExtension(path) || IsProfileManifest(path)) {
    return std::nullopt;
  }

  toml::table config;
  try {
    config = toml::parse_file(path.native());
  } catch (...) {
    return std::nullopt;
  }

  if (const toml::node* mv = config.get("manifest_version"); mv != nullptr) {
    if (!mv->is_integer() || mv->as_integer()->get() != 1) {
      return std::nullopt;
    }
  }

  const auto shared_node = config.at_path("Driver.shared");
  std::string shared_hint;
  if (const auto* single = shared_node.as_string()) {
    if (single->get().empty()) {
      return std::nullopt;
    }
    shared_hint = single->get();
  } else if (const auto* platforms = shared_node.as_table()) {
    for (const auto& [key, value] : *platforms) {
      (void)key;
      if (const auto* str = value.as_string(); str != nullptr && !str->get().empty()) {
        if (!shared_hint.empty()) {
          shared_hint.push_back(' ');
        }
        shared_hint += str->get();
      }
    }
    if (shared_hint.empty()) {
      return std::nullopt;
    }
  } else {
    return std::nullopt;
  }

  detail::ParsedDriverManifest parsed;
  parsed.shared_hint = std::move(shared_hint);
  if (const auto* name = config["name"].as_string()) {
    parsed.name = name->get();
  }
  if (const auto* version = config["version"].as_string()) {
    parsed.version = version->get();
  }
  return parsed;
}

std::optional<std::filesystem::path> ResolveManifestPath(std::string_view driver_spec) {
  if (driver_spec.empty()) {
    return std::nullopt;
  }

  const std::filesystem::path spec_path(driver_spec);
  if (HasTomlExtension(spec_path)) {
    std::error_code error;
    if (std::filesystem::is_regular_file(spec_path, error)) {
      return spec_path;
    }
    return std::nullopt;
  }

  if (driver_spec.find('/') != std::string_view::npos ||
      driver_spec.find('\\') != std::string_view::npos || spec_path.has_extension()) {
    return std::nullopt;
  }

  const std::string filename = std::string(driver_spec) + ".toml";
  const auto check_dir = [&](const std::filesystem::path& dir)
      -> std::optional<std::filesystem::path> {
    if (dir.empty()) {
      return std::nullopt;
    }
    std::error_code error;
    const std::filesystem::path candidate = dir / filename;
    if (std::filesystem::is_regular_file(candidate, error)) {
      return candidate;
    }
    return std::nullopt;
  };

  if (const char* env = std::getenv("ADBC_DRIVER_PATH"); env != nullptr) {
    for (const std::string& entry : SplitSearchPath(env)) {
      if (auto found = check_dir(entry)) {
        return found;
      }
    }
  }

  if (const char* dbpp_drivers = std::getenv("DBPP_ADBC_DRIVERS"); dbpp_drivers != nullptr) {
    for (const std::string& entry : SplitSearchPath(dbpp_drivers)) {
      const std::filesystem::path path(entry);
      std::error_code error;
      if (std::filesystem::is_directory(path, error)) {
        if (auto found = check_dir(path)) {
          return found;
        }
      } else if (std::filesystem::is_regular_file(path, error) && path.has_parent_path()) {
        if (auto found = check_dir(path.parent_path())) {
          return found;
        }
      }
    }
  }

  for (const char* env_var : {"VIRTUAL_ENV", "CONDA_PREFIX"}) {
    if (auto found = check_dir(EnvAdbcDriversDir(env_var))) {
      return found;
    }
  }

  if (auto found = check_dir(UserAdbcDriversDir())) {
    return found;
  }
  if (auto found = check_dir(SystemAdbcDriversDir())) {
    return found;
  }
  return std::nullopt;
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

Dialect DetectDialectFromScheme(std::string_view uri) {
  const std::string lower = LowerAscii(TrimAscii(uri));
  if (lower.starts_with("redis://") || lower.starts_with("rediss://")) {
    return Dialect::Redis;
  }
  if (lower.starts_with("postgresql://") || lower.starts_with("postgres://")) {
    return Dialect::Postgresql;
  }
  if (lower.starts_with("mysql://") || lower.starts_with("mariadb://")) {
    return Dialect::Mysql;
  }
  if (lower.starts_with("duckdb:")) {
    return Dialect::Duckdb;
  }
  return Dialect::Generic;
}

Dialect DetectDialectFromText(std::string_view text) {
  const std::string key = LowerAscii(text);
  if (key.find("sqlite") != std::string::npos) {
    return Dialect::Sqlite;
  }
  if (key.find("postgres") != std::string::npos) {
    return Dialect::Postgresql;
  }
  if (key.find("mysql") != std::string::npos || key.find("mariadb") != std::string::npos) {
    return Dialect::Mysql;
  }
  if (key.find("duckdb") != std::string::npos) {
    return Dialect::Duckdb;
  }
  if (key.find("redis") != std::string::npos) {
    return Dialect::Redis;
  }
  return Dialect::Generic;
}

Dialect DetectDialect(std::string_view driver_path, std::string_view uri) {
  const Dialect by_scheme = DetectDialectFromScheme(uri);
  if (by_scheme != Dialect::Generic) {
    return by_scheme;
  }
  const Dialect by_driver = DetectDialectFromText(driver_path);
  if (by_driver != Dialect::Generic) {
    return by_driver;
  }
  const Dialect by_uri = DetectDialectFromText(uri);
  if (by_uri != Dialect::Generic) {
    return by_uri;
  }
  if (const auto manifest_path = ResolveManifestPath(driver_path)) {
    if (const auto parsed = ParseDriverManifest(*manifest_path)) {
      return DetectDialectFromText(parsed->name + " " + parsed->shared_hint);
    }
  }
  return Dialect::Generic;
}


std::string FormatDouble(double value) {
  if (std::isnan(value)) {
    return "NaN";
  }
  if (std::isinf(value)) {
    return value < 0 ? "-Inf" : "Inf";
  }
  std::ostringstream stream;
  stream << std::setprecision(15) << value;
  return stream.str();
}

std::string FormatDate(std::chrono::sys_days days) {
  const std::chrono::year_month_day value(days);
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%04d-%02u-%02u",
                static_cast<int>(value.year()),
                static_cast<unsigned>(value.month()),
                static_cast<unsigned>(value.day()));
  return buffer;
}

std::string FormatTime(std::chrono::seconds seconds) {
  const auto day_seconds = static_cast<std::int64_t>(seconds.count());
  const std::int64_t normalized =
      ((day_seconds % 86400) + 86400) % 86400;
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "%02lld:%02lld:%02lld",
                static_cast<long long>(normalized / 3600),
                static_cast<long long>((normalized % 3600) / 60),
                static_cast<long long>(normalized % 60));
  return buffer;
}

std::filesystem::path ExecutableDirectory() {
#if defined(_WIN32)
  std::wstring buffer(512, L'\0');
  for (;;) {
    const DWORD len = GetModuleFileNameW(nullptr, buffer.data(),
                                         static_cast<DWORD>(buffer.size()));
    if (len == 0) {
      return {};
    }
    if (len < buffer.size()) {
      buffer.resize(len);
      return std::filesystem::path(buffer).parent_path();
    }
    if (buffer.size() >= 32768) {
      return {};
    }
    buffer.resize(buffer.size() * 2, L'\0');
  }
#elif defined(__APPLE__)
  std::uint32_t size = 0;
  (void)_NSGetExecutablePath(nullptr, &size);
  if (size == 0) {
    return {};
  }
  std::string buffer(size, '\0');
  if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
    return {};
  }
  const std::filesystem::path raw(buffer.c_str());
  std::error_code error;
  const std::filesystem::path resolved = std::filesystem::weakly_canonical(raw, error);
  return (error ? raw : resolved).parent_path();
#else
  std::string buffer(512, '\0');
  for (;;) {
    const ssize_t len = readlink("/proc/self/exe", buffer.data(), buffer.size());
    if (len < 0) {
      return {};
    }
    if (static_cast<std::size_t>(len) < buffer.size()) {
      buffer.resize(static_cast<std::size_t>(len));
      return std::filesystem::path(buffer).parent_path();
    }
    if (buffer.size() >= 32768) {
      return {};
    }
    buffer.resize(buffer.size() * 2, '\0');
  }
#endif
}

std::optional<std::filesystem::path> FindBundledSqliteDriverInDir(
    const std::filesystem::path& dir) {
  if (dir.empty()) {
    return std::nullopt;
  }
  std::error_code error;
  if (!std::filesystem::is_directory(dir, error)) {
    return std::nullopt;
  }

#if defined(_WIN32)
  constexpr std::string_view kExactName = "adbc_driver_sqlite.dll";
#elif defined(__APPLE__)
  constexpr std::string_view kExactName = "libadbc_driver_sqlite.dylib";
#else
  constexpr std::string_view kExactName = "libadbc_driver_sqlite.so";
#endif

  const std::filesystem::path exact = dir / kExactName;
  if (std::filesystem::is_regular_file(exact, error)) {
    return exact;
  }

  std::vector<std::filesystem::path> matches;
  for (std::filesystem::directory_iterator it(dir, error), end; it != end;
       it.increment(error)) {
    if (error) {
      break;
    }
    std::error_code file_error;
    if (!std::filesystem::is_regular_file(it->path(), file_error)) {
      continue;
    }
    const std::string lower = LowerAscii(it->path().filename().string());
#if defined(_WIN32)
    if (lower == kExactName) {
      matches.push_back(it->path());
    }
#elif defined(__APPLE__)
    if (lower.starts_with("libadbc_driver_sqlite.") && lower.ends_with(".dylib")) {
      matches.push_back(it->path());
    }
#else
    if (lower.starts_with("libadbc_driver_sqlite.so.")) {
      matches.push_back(it->path());
    }
#endif
  }

  if (matches.empty()) {
    return std::nullopt;
  }
  std::ranges::sort(matches, [](const auto& left, const auto& right) {
    return left.filename().string() < right.filename().string();
  });
  return matches.front();
}

std::filesystem::path BundledDriverDirectory() {
  const std::filesystem::path exe_dir = ExecutableDirectory();
  if (exe_dir.empty()) {
    return {};
  }
#if defined(_WIN32)
  return exe_dir;
#elif defined(__APPLE__)
  return (exe_dir / ".." / "Frameworks").lexically_normal();
#else
  return exe_dir / "lib";
#endif
}


} // namespace

std::string DefaultSqliteDriverPath() {
  if (const char* environment = std::getenv("DBPP_SQLITE_DRIVER"); environment != nullptr &&
                                                                    *environment != '\0') {
    return environment;
  }

  const std::filesystem::path exe_dir = ExecutableDirectory();
  if (!exe_dir.empty()) {
#if defined(_WIN32)
    const std::filesystem::path candidates[] = {
        exe_dir,
        exe_dir / "drivers",
        exe_dir / "adbc" / "driver" / "sqlite",
    };
#elif defined(__APPLE__)
    const std::filesystem::path candidates[] = {
        (exe_dir / ".." / "Frameworks").lexically_normal(),
        exe_dir,
        exe_dir / "drivers",
        exe_dir / "adbc" / "driver" / "sqlite",
    };
#else
    const std::filesystem::path candidates[] = {
        exe_dir / "lib",
        exe_dir / "drivers",
        exe_dir,
        exe_dir / "adbc" / "driver" / "sqlite",
    };
#endif
    for (const auto& dir : candidates) {
      if (const auto found = FindBundledSqliteDriverInDir(dir)) {
        return found->string();
      }
    }
  }

#ifdef DBPP_SQLITE_DRIVER_PATH
  {
    std::error_code error;
    if (std::filesystem::is_regular_file(DBPP_SQLITE_DRIVER_PATH, error)) {
      return DBPP_SQLITE_DRIVER_PATH;
    }
  }
#endif
  return "adbc_driver_sqlite";
}

bool IsConnectionLostError(int adbc_status, std::string_view message) {
  return IsConnectionLostError(adbc_status, message, {});
}

bool IsConnectionLostError(int adbc_status, std::string_view message,
                           std::string_view sqlstate) {
  if (adbc_status == ADBC_STATUS_IO) {
    return true;
  }
  if (sqlstate.size() >= 2 && sqlstate[0] == '0' && sqlstate[1] == '8') {
    return true;
  }
  if (sqlstate == "57P01" || sqlstate == "57P02" || sqlstate == "57P03") {
    return true;
  }
  const std::string lower = LowerAscii(TrimAscii(message));
  if (lower.empty()) {
    return false;
  }
  if (lower == "eof" || lower.ends_with(": eof") ||
      lower.find(": eof\n") != std::string::npos ||
      lower.find(": eof\r") != std::string::npos) {
    return true;
  }
  static constexpr std::string_view kPatterns[] = {
      "connection is already closed",
      "connection already closed",
      "connection has been closed",
      "connection closed",
      "closed network connection",
      "use of closed network connection",
      "invalid connection",
      "bad connection",
      "broken pipe",
      "connection reset",
      "connection aborted",
      "connection refused",
      "server has gone away",
      "lost connection",
      "server closed the connection",
      "no connection to the server",
      "terminating connection",
      "unexpected eof",
      "eof detected",
      "socket is not connected",
      "network is unreachable",
      "software caused connection abort",
      "connection timed out",
      "i/o timeout",
  };
  for (const std::string_view pattern : kPatterns) {
    if (lower.find(pattern) != std::string_view::npos) {
      return true;
    }
  }
  return message.find("服务器已关闭连接") != std::string_view::npos ||
         message.find("连接已关闭") != std::string_view::npos ||
         message.find("连接未建立") != std::string_view::npos ||
         message.find("连接超时") != std::string_view::npos;
}

bool ShouldHealthCheckSession(Dialect dialect, std::string_view uri) {
  if (dialect == Dialect::Sqlite || dialect == Dialect::Duckdb) {
    return false;
  }
  if (dialect == Dialect::Redis || dialect == Dialect::Mysql || dialect == Dialect::Postgresql) {
    return true;
  }
  return uri.find("://") != std::string_view::npos;
}

AdbcQueryResult PingSessionBlocking(AdbcSession& session) {
  if (session.IsClosed()) {
    AdbcQueryResult result;
    result.connection_lost = true;
    result.error = Tr("Connection is closed", "连接已关闭");
    return result;
  }
  if (session.dialect == Dialect::Redis) {
    AdbcQueryResult res = ExecuteBlocking(session, "PING", true, 1);
    if (res.connection_lost || !session.redis || !session.redis->IsConnected() ||
        (!res.error.empty() && IsConnectionLostError(0, res.error))) {
      res.connection_lost = true;
      session.alive.store(false, std::memory_order_release);
      return res;
    }
    res.error.clear();
    res.connection_lost = false;
    session.alive.store(true, std::memory_order_release);
    return res;
  }

  AdbcQueryResult res = ExecuteBlocking(session, "SELECT 1", true, 1);
  if (res.connection_lost) {
    session.alive.store(false, std::memory_order_release);
    return res;
  }
  res.error.clear();
  res.connection_lost = false;
  session.alive.store(true, std::memory_order_release);
  return res;
}

namespace detail {

bool AdbcDecodeUtf8Sequence(std::string_view text, std::size_t index,
                            std::size_t& out_len) noexcept {
  return DecodeUtf8Sequence(text, index, out_len);
}
std::string AdbcDecodeSystemTextToUtf8(std::string_view raw) {
  return DecodeSystemTextToUtf8(raw);
}
std::string AdbcLowerAscii(std::string_view text) { return LowerAscii(text); }
bool AdbcContainsLower(std::string_view haystack, std::string_view needle) {
  return ContainsLower(haystack, needle);
}
std::string_view AdbcTrimAscii(std::string_view text) { return TrimAscii(text); }
std::vector<std::string> AdbcSplitSearchPath(std::string_view value) {
  return SplitSearchPath(value);
}
std::string AdbcSystemAdbcDriversDir() { return SystemAdbcDriversDir(); }
std::filesystem::path AdbcEnvAdbcDriversDir(const char* env_var) {
  return EnvAdbcDriversDir(env_var);
}
bool AdbcHasTomlExtension(const std::filesystem::path& path) { return HasTomlExtension(path); }
bool AdbcIsProfileManifest(const std::filesystem::path& path) { return IsProfileManifest(path); }
std::string AdbcBuildAdditionalSearchPathList() { return BuildAdditionalSearchPathList(); }
std::optional<ParsedDriverManifest> AdbcParseDriverManifest(const std::filesystem::path& path) {
  return ParseDriverManifest(path);
}
Dialect AdbcDetectDialect(std::string_view driver_path, std::string_view uri) {
  return DetectDialect(driver_path, uri);
}
std::string AdbcFormatDouble(double value) { return FormatDouble(value); }
std::string AdbcFormatDate(std::chrono::sys_days days) { return FormatDate(days); }
std::string AdbcFormatTime(std::chrono::seconds seconds) { return FormatTime(seconds); }
std::filesystem::path AdbcExecutableDirectory() { return ExecutableDirectory(); }
std::filesystem::path AdbcBundledDriverDirectory() { return BundledDriverDirectory(); }

std::string AdbcFormatTimestampTz(std::int64_t value, int time_unit, std::string_view tz) {
  std::int64_t sec_val = value;
  std::int64_t sub_val = 0;
  int width = 0;
  switch (time_unit) {
    case 1: { // MILLI
      const auto tp = std::chrono::sys_time<std::chrono::milliseconds>{
          std::chrono::milliseconds{value}};
      const auto sec = std::chrono::floor<std::chrono::seconds>(tp);
      sec_val = sec.time_since_epoch().count();
      sub_val = (tp - sec).count();
      width = 3;
      break;
    }
    case 2: { // MICRO
      const auto tp = std::chrono::sys_time<std::chrono::microseconds>{
          std::chrono::microseconds{value}};
      const auto sec = std::chrono::floor<std::chrono::seconds>(tp);
      sec_val = sec.time_since_epoch().count();
      sub_val = (tp - sec).count();
      width = 6;
      break;
    }
    case 3: { // NANO
      const auto tp = std::chrono::sys_time<std::chrono::nanoseconds>{
          std::chrono::nanoseconds{value}};
      const auto sec = std::chrono::floor<std::chrono::seconds>(tp);
      sec_val = sec.time_since_epoch().count();
      sub_val = (tp - sec).count();
      width = 9;
      break;
    }
    default:
      break;
  }
  const auto time = std::chrono::sys_seconds{std::chrono::seconds{sec_val}};
  const auto days = std::chrono::floor<std::chrono::days>(time);
  const bool zoned = !tz.empty();
  std::string out = FormatDate(days) + (zoned ? "T" : " ") +
                    FormatTime(std::chrono::floor<std::chrono::seconds>(time) - days);
  if (width > 0 && sub_val > 0) {
    char frac_buf[32];
    std::snprintf(frac_buf, sizeof(frac_buf), "%0*lld", width, static_cast<long long>(sub_val));
    std::string_view frac(frac_buf);
    while (!frac.empty() && frac.back() == '0') {
      frac.remove_suffix(1);
    }
    if (!frac.empty()) {
      out.push_back('.');
      out.append(frac);
    }
  }
  if (zoned) {
    out.push_back('Z');
  }
  return out;
}

std::string AdbcFormatTimeUnit(std::int64_t value, int time_unit) {
  std::int64_t sec = value;
  std::int64_t sub = 0;
  int width = 0;
  auto split = [&](std::int64_t scale, int digits) {
    const std::int64_t mod = ((value % scale) + scale) % scale;
    sec = (value - mod) / scale;
    sub = mod;
    width = digits;
  };
  switch (time_unit) {
    case 1: split(1000, 3); break;
    case 2: split(1'000'000, 6); break;
    case 3: split(1'000'000'000, 9); break;
    default: break;
  }
  std::string out = FormatTime(std::chrono::seconds{sec});
  if (width > 0 && sub > 0) {
    char frac_buf[32];
    std::snprintf(frac_buf, sizeof(frac_buf), "%0*lld", width, static_cast<long long>(sub));
    std::string_view frac(frac_buf);
    while (!frac.empty() && frac.back() == '0') {
      frac.remove_suffix(1);
    }
    if (!frac.empty()) {
      out.push_back('.');
      out.append(frac);
    }
  }
  return out;
}

} // namespace detail

} // namespace dbpp
