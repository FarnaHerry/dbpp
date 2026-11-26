module;

#include <arrow-adbc/adbc.h>
#include <arrow-adbc/adbc_driver_manager.h>
#include <nanoarrow/nanoarrow.h>
#include <toml++/toml.hpp>
#include <huxerui/datatable.h>
#include <huxerui/task.h>

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

#include <cerrno>
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

class AdbcErrorGuard {
public:
  AdbcErrorGuard() = default;
  AdbcErrorGuard(const AdbcErrorGuard&) = delete;
  AdbcErrorGuard& operator=(const AdbcErrorGuard&) = delete;
  ~AdbcErrorGuard() { Reset(); }

  AdbcError* Ptr() noexcept { return &error_; }

  void Reset() noexcept {
    if (error_.release != nullptr) {
      error_.release(&error_);
      error_.release = nullptr;
    }
  }

  [[nodiscard]] std::string Take() {
    std::string message =
        error_.message != nullptr ? DecodeSystemTextToUtf8(error_.message) : "unknown ADBC error";
    Reset();
    return message;
  }

private:
  AdbcError error_ = ADBC_ERROR_INIT;
};

struct AdbcStatementGuard {
  AdbcStatement statement{};
  bool created = false;
  ~AdbcStatementGuard() {
    if (created) {
      AdbcErrorGuard release_error;
      AdbcStatementRelease(&statement, release_error.Ptr());
    }
  }
};


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

struct ParsedDriverManifest {
  std::string name;
  std::string version;
  std::string shared_hint;
};

std::optional<ParsedDriverManifest> ParseDriverManifest(const std::filesystem::path& path) {
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

  ParsedDriverManifest parsed;
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

struct StreamHolder {
  ArrowArrayStream stream{};
  StreamHolder() = default;
  StreamHolder(const StreamHolder&) = delete;
  StreamHolder& operator=(const StreamHolder&) = delete;
  ~StreamHolder() {
    if (stream.release != nullptr) {
      stream.release(&stream);
      stream = ArrowArrayStream{};
    }
  }
};

struct SchemaHolder {
  ArrowSchema schema{};
  SchemaHolder() = default;
  SchemaHolder(const SchemaHolder&) = delete;
  SchemaHolder& operator=(const SchemaHolder&) = delete;
  ~SchemaHolder() {
    if (schema.release != nullptr) {
      schema.release(&schema);
      schema = ArrowSchema{};
    }
  }
};

struct ArrayHolder {
  ArrowArray array{};
  ArrayHolder() = default;
  ArrayHolder(const ArrayHolder&) = delete;
  ArrayHolder& operator=(const ArrayHolder&) = delete;
  ~ArrayHolder() { Reset(); }

  void Reset() noexcept {
    if (array.release != nullptr) {
      array.release(&array);
      array = ArrowArray{};
    }
  }
};

struct ArrayViewHolder {
  ArrowArrayView view{};
  bool initialized = false;
  ArrayViewHolder() = default;
  ArrayViewHolder(const ArrayViewHolder&) = delete;
  ArrayViewHolder& operator=(const ArrayViewHolder&) = delete;
  ~ArrayViewHolder() { Reset(); }

  void Reset() noexcept {
    if (initialized) {
      ArrowArrayViewReset(&view);
      view = ArrowArrayView{};
      initialized = false;
    }
  }
};

struct FieldInfo {
  std::string name;
  ArrowType type = NANOARROW_TYPE_UNINITIALIZED;
  ArrowTimeUnit time_unit = NANOARROW_TIME_UNIT_SECOND;
  std::int32_t decimal_bitwidth = 0;
  std::int32_t decimal_precision = 0;
  std::int32_t decimal_scale = 0;
};

std::string StreamError(ArrowArrayStream& stream) {
  const char* message = stream.get_last_error != nullptr ? stream.get_last_error(&stream) : nullptr;
  return message != nullptr ? DecodeSystemTextToUtf8(message) : "Arrow stream error";
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

std::string FormatTimestamp(std::int64_t value, ArrowTimeUnit unit) {
  std::int64_t seconds = value;
  switch (unit) {
    case NANOARROW_TIME_UNIT_MILLI: seconds = value / 1000; break;
    case NANOARROW_TIME_UNIT_MICRO: seconds = value / 1'000'000; break;
    case NANOARROW_TIME_UNIT_NANO: seconds = value / 1'000'000'000; break;
    case NANOARROW_TIME_UNIT_SECOND: break;
  }
  const auto time = std::chrono::sys_seconds{std::chrono::seconds{seconds}};
  const auto days = std::chrono::floor<std::chrono::days>(time);
  return FormatDate(days) + " " + FormatTime(std::chrono::floor<std::chrono::seconds>(time) - days);
}

std::string TimeUnitLabel(ArrowTimeUnit unit) {
  switch (unit) {
    case NANOARROW_TIME_UNIT_MILLI: return "ms";
    case NANOARROW_TIME_UNIT_MICRO: return "us";
    case NANOARROW_TIME_UNIT_NANO: return "ns";
    case NANOARROW_TIME_UNIT_SECOND: return "s";
  }
  return "s";
}

CellValue ExtractCell(const ArrowArrayView& view, int64_t row, const FieldInfo& field) {
  CellValue cell;
  if (ArrowArrayViewIsNull(&view, row) != 0 || view.storage_type == NANOARROW_TYPE_NA) {
    cell.text = "NULL";
    cell.is_null = true;
    return cell;
  }

  const ArrowType storage = view.storage_type;
  switch (storage) {
    case NANOARROW_TYPE_BOOL:
      cell.text = ArrowArrayViewGetIntUnsafe(&view, row) != 0 ? "true" : "false";
      return cell;
    case NANOARROW_TYPE_INT8:
    case NANOARROW_TYPE_INT16:
    case NANOARROW_TYPE_INT32:
    case NANOARROW_TYPE_INT64:
      cell.text = std::to_string(ArrowArrayViewGetIntUnsafe(&view, row));
      return cell;
    case NANOARROW_TYPE_UINT8:
    case NANOARROW_TYPE_UINT16:
    case NANOARROW_TYPE_UINT32:
    case NANOARROW_TYPE_UINT64:
      cell.text = std::to_string(ArrowArrayViewGetUIntUnsafe(&view, row));
      return cell;
    case NANOARROW_TYPE_FLOAT:
    case NANOARROW_TYPE_DOUBLE:
    case NANOARROW_TYPE_HALF_FLOAT:
      cell.text = FormatDouble(ArrowArrayViewGetDoubleUnsafe(&view, row));
      return cell;
    case NANOARROW_TYPE_STRING:
    case NANOARROW_TYPE_LARGE_STRING:
    case NANOARROW_TYPE_STRING_VIEW: {
      const ArrowStringView value = ArrowArrayViewGetStringUnsafe(&view, row);
      if (value.data != nullptr) {
        const std::string_view sv(value.data, static_cast<std::size_t>(value.size_bytes));
        cell.text = IsValidUtf8(sv) ? std::string(sv) : SanitizeUtf8(sv);
      }
      return cell;
    }
    case NANOARROW_TYPE_BINARY:
    case NANOARROW_TYPE_LARGE_BINARY:
    case NANOARROW_TYPE_FIXED_SIZE_BINARY:
    case NANOARROW_TYPE_BINARY_VIEW: {
      const ArrowBufferView value = ArrowArrayViewGetBytesUnsafe(&view, row);
      cell.text = "<blob " + std::to_string(value.size_bytes) + " bytes>";
      return cell;
    }
    default:
      break;
  }

  switch (field.type) {
    case NANOARROW_TYPE_DATE32: {
      const auto days = std::chrono::sys_days{
          std::chrono::days{ArrowArrayViewGetIntUnsafe(&view, row)}};
      cell.text = FormatDate(days);
      return cell;
    }
    case NANOARROW_TYPE_DATE64: {
      const auto milliseconds = std::chrono::milliseconds{
          ArrowArrayViewGetIntUnsafe(&view, row)};
      cell.text = FormatDate(std::chrono::sys_days{
          std::chrono::duration_cast<std::chrono::days>(milliseconds)});
      return cell;
    }
    case NANOARROW_TYPE_TIMESTAMP:
      cell.text = FormatTimestamp(ArrowArrayViewGetIntUnsafe(&view, row), field.time_unit);
      return cell;
    case NANOARROW_TYPE_TIME32:
    case NANOARROW_TYPE_TIME64: {
      std::int64_t value = ArrowArrayViewGetIntUnsafe(&view, row);
      switch (field.time_unit) {
        case NANOARROW_TIME_UNIT_MILLI: value /= 1000; break;
        case NANOARROW_TIME_UNIT_MICRO: value /= 1'000'000; break;
        case NANOARROW_TIME_UNIT_NANO: value /= 1'000'000'000; break;
        case NANOARROW_TIME_UNIT_SECOND: break;
      }
      cell.text = FormatTime(std::chrono::seconds{value});
      return cell;
    }
    case NANOARROW_TYPE_DURATION:
      cell.text = std::to_string(ArrowArrayViewGetIntUnsafe(&view, row)) + " " +
                  TimeUnitLabel(field.time_unit);
      return cell;
    case NANOARROW_TYPE_DECIMAL32:
    case NANOARROW_TYPE_DECIMAL64:
    case NANOARROW_TYPE_DECIMAL128:
    case NANOARROW_TYPE_DECIMAL256: {
      ArrowDecimal decimal;
      ArrowDecimalInit(&decimal, field.decimal_bitwidth, field.decimal_precision,
                       field.decimal_scale);
      ArrowArrayViewGetDecimalUnsafe(&view, row, &decimal);
      ArrowBuffer buffer;
      ArrowBufferInit(&buffer);
      if (ArrowDecimalAppendStringToBuffer(&decimal, &buffer) == NANOARROW_OK) {
        cell.text.assign(reinterpret_cast<const char*>(buffer.data), buffer.size_bytes);
      } else {
        cell.text = "<decimal>";
      }
      ArrowBufferReset(&buffer);
      return cell;
    }
    default:
      break;
  }

  cell.text = std::string("<") + ArrowTypeString(field.type) + ">";
  return cell;
}

std::string StringAt(const ArrowArrayView* view, int64_t index) {
  if (view == nullptr || index < 0 || index >= view->length ||
      ArrowArrayViewIsNull(view, index) != 0) {
    return {};
  }
  const ArrowStringView value = ArrowArrayViewGetStringUnsafe(view, index);
  if (value.data == nullptr) {
    return {};
  }
  return std::string(value.data, static_cast<std::size_t>(value.size_bytes));
}

int64_t ListEnd(const ArrowArrayView* list, int64_t index) {
  if (list == nullptr || list->n_children == 0) {
    return 0;
  }
  if (index + 1 < list->length) {
    return ArrowArrayViewListChildOffset(list, index + 1);
  }
  return list->children[0]->length;
}

int64_t ListStart(const ArrowArrayView* list, int64_t index) {
  if (list == nullptr || list->n_children == 0 || index < 0 || index >= list->length) {
    return 0;
  }
  return ArrowArrayViewListChildOffset(list, index);
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

std::string InjectSchemeUriCredentials(std::string_view uri, std::string_view username,
                                       std::string_view password);

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

namespace {

void ReleaseNativeAdbcHandles(AdbcSession& session) noexcept {
  if (session.connection_created) {
    AdbcErrorGuard error;
    AdbcConnectionRelease(&session.connection, error.Ptr());
    session.connection_created = false;
    session.connection = {};
  }
  if (session.database_created) {
    AdbcErrorGuard error;
    AdbcDatabaseRelease(&session.database, error.Ptr());
    session.database_created = false;
    session.database = {};
  }
  session.initialized = false;
}

AdbcObjectsResult LoadObjectsBlockingOnce(AdbcSession& session, AdbcStatusCode& status_out) {
  AdbcObjectsResult result;
  if (!session.connection_created) {
    status_out = ADBC_STATUS_INVALID_STATE;
    result.error = Tr("Connection is closed", "连接已关闭");
    return result;
  }

  AdbcErrorGuard error;
  StreamHolder stream;
  status_out = AdbcConnectionGetObjects(&session.connection, ADBC_OBJECT_DEPTH_ALL, nullptr, nullptr,
                                        nullptr, nullptr, nullptr, &stream.stream, error.Ptr());
  if (status_out != ADBC_STATUS_OK) {
    result.error = error.Take();
    return result;
  }

  SchemaHolder schema;
  if (stream.stream.get_schema(&stream.stream, &schema.schema) != 0) {
    result.error = StreamError(stream.stream);
    return result;
  }

  while (true) {
    ArrayHolder array;
    if (stream.stream.get_next(&stream.stream, &array.array) != 0) {
      result.error = StreamError(stream.stream);
      return result;
    }
    if (array.array.release == nullptr) {
      break;
    }

    ArrayViewHolder root;
    ArrowError arrow_error;
    ArrowErrorInit(&arrow_error);
    if (ArrowArrayViewInitFromSchema(&root.view, &schema.schema, &arrow_error) != NANOARROW_OK) {
      result.error = DecodeSystemTextToUtf8(arrow_error.message);
      return result;
    }
    root.initialized = true;
    ArrowErrorInit(&arrow_error);
    if (ArrowArrayViewSetArray(&root.view, &array.array, &arrow_error) != NANOARROW_OK) {
      result.error = DecodeSystemTextToUtf8(arrow_error.message);
      return result;
    }

    const ArrowArrayView* catalogs = root.view.n_children > 1 ? root.view.children[1] : nullptr;
    for (int64_t catalog_index = 0; catalog_index < root.view.array->length; ++catalog_index) {
      const std::string catalog_name =
          root.view.n_children > 0 ? StringAt(root.view.children[0], catalog_index) : std::string{};
      if (catalogs == nullptr || ArrowArrayViewIsNull(catalogs, catalog_index) != 0) {
        continue;
      }
      const int64_t schema_start = ListStart(catalogs, catalog_index);
      const int64_t schema_end = ListEnd(catalogs, catalog_index);
      const ArrowArrayView* schema_struct = catalogs->children[0];
      for (int64_t schema_index = schema_start; schema_index < schema_end; ++schema_index) {
        const std::string db_schema = StringAt(schema_struct->children[0], schema_index);
        const ArrowArrayView* tables = schema_struct->children[1];
        if (ArrowArrayViewIsNull(tables, schema_index) != 0) {
          continue;
        }
        const int64_t table_start = ListStart(tables, schema_index);
        const int64_t table_end = ListEnd(tables, schema_index);
        const ArrowArrayView* table_struct = tables->children[0];
        for (int64_t table_index = table_start; table_index < table_end; ++table_index) {
          DbObject object;
          object.catalog = catalog_name;
          object.db_schema = db_schema;
          object.name = StringAt(table_struct->children[0], table_index);
          object.table_type = StringAt(table_struct->children[1], table_index);
          object.is_view = ContainsLower(object.table_type, "view");

          const ArrowArrayView* columns_list = table_struct->children[2];
          if (ArrowArrayViewIsNull(columns_list, table_index) == 0) {
            const int64_t column_start = ListStart(columns_list, table_index);
            const int64_t column_end = ListEnd(columns_list, table_index);
            const ArrowArrayView* column_struct = columns_list->children[0];
            for (int64_t column_index = column_start; column_index < column_end;
                 ++column_index) {
              ColumnMeta column;
              column.name = StringAt(column_struct->children[0], column_index);
              column.type = StringAt(column_struct->children[4], column_index);
              object.columns.push_back(std::move(column));
            }
          }

          const ArrowArrayView* constraints_list = table_struct->children[3];
          if (ArrowArrayViewIsNull(constraints_list, table_index) == 0) {
            const int64_t constraint_start = ListStart(constraints_list, table_index);
            const int64_t constraint_end = ListEnd(constraints_list, table_index);
            const ArrowArrayView* constraint_struct = constraints_list->children[0];
            for (int64_t constraint_index = constraint_start;
                 constraint_index < constraint_end; ++constraint_index) {
              const std::string constraint_type =
                  StringAt(constraint_struct->children[1], constraint_index);
              if (LowerAscii(constraint_type) != "primary key") {
                continue;
              }
              const ArrowArrayView* names = constraint_struct->children[2];
              if (names == nullptr || ArrowArrayViewIsNull(names, constraint_index) != 0) {
                continue;
              }
              const int64_t name_start = ListStart(names, constraint_index);
              const int64_t name_end = ListEnd(names, constraint_index);
              for (int64_t name_index = name_start; name_index < name_end; ++name_index) {
                object.primary_key.push_back(StringAt(names->children[0], name_index));
              }
            }
          }

          if (!object.name.empty()) {
            result.objects.push_back(std::move(object));
          }
        }
      }
    }
  }

  std::ranges::sort(result.objects, [](const DbObject& left, const DbObject& right) {
    if (left.is_view != right.is_view) {
      return !left.is_view;
    }
    if (left.db_schema != right.db_schema) {
      return left.db_schema < right.db_schema;
    }
    return left.name < right.name;
  });
  return result;
}

AdbcSchemaResult LoadSchemaBlockingOnce(AdbcSession& session, const DbObject& object,
                                      AdbcStatusCode& status_out) {
  AdbcSchemaResult result;
  if (!session.connection_created) {
    status_out = ADBC_STATUS_INVALID_STATE;
    result.error = Tr("Connection is closed", "连接已关闭");
    return result;
  }

  AdbcErrorGuard error;
  SchemaHolder schema;
  const char* catalog = object.catalog.empty() ? nullptr : object.catalog.c_str();
  const char* db_schema = object.db_schema.empty() ? nullptr : object.db_schema.c_str();

  status_out = AdbcConnectionGetTableSchema(&session.connection, catalog, db_schema,
                                            object.name.c_str(), &schema.schema, error.Ptr());
  if (status_out != ADBC_STATUS_OK) {
    result.error = error.Take();
    result.columns = object.columns;
    return result;
  }

  for (int64_t index = 0; index < schema.schema.n_children; ++index) {
    const ArrowSchema* field = schema.schema.children[index];
    ColumnMeta column;
    column.name = field->name != nullptr ? field->name : "column " + std::to_string(index + 1);
    ArrowSchemaView view;
    ArrowError arrow_error;
    ArrowErrorInit(&arrow_error);
    if (ArrowSchemaViewInit(&view, field, &arrow_error) == NANOARROW_OK) {
      column.type = ArrowTypeString(view.type);
    } else {
      column.type = "ANY";
    }
    for (const ColumnMeta& known : object.columns) {
      if (known.name == column.name && !known.type.empty()) {
        column.type = known.type;
        break;
      }
    }
    column.nullable = (field->flags & ARROW_FLAG_NULLABLE) != 0;
    for (std::size_t key_index = 0; key_index < object.primary_key.size(); ++key_index) {
      if (object.primary_key[key_index] == column.name) {
        column.primary_key = true;
        column.primary_key_order = static_cast<int>(key_index + 1);
        break;
      }
    }
    result.columns.push_back(std::move(column));
  }

  if (result.columns.empty()) {
    result.columns = object.columns;
  }
  return result;
}

AdbcQueryResult ExecuteBlockingOnce(AdbcSession& session, const std::string& sql, bool want_rows,
                                    std::size_t max_rows, AdbcStatusCode& status_out,
                                    bool& before_execute_out) {
  AdbcQueryResult result;
  before_execute_out = true;
  if (!session.connection_created) {
    status_out = ADBC_STATUS_INVALID_STATE;
    result.error = Tr("Connection is closed", "连接已关闭");
    return result;
  }

  AdbcErrorGuard error;
  // Declared before StreamHolder/SchemaHolder/ArrayViewHolder so C++ destroys
  // Arrow views/streams before calling AdbcStatementRelease.
  AdbcStatementGuard statement_guard;
  status_out = AdbcStatementNew(&session.connection, &statement_guard.statement, error.Ptr());
  if (status_out != ADBC_STATUS_OK) {
    result.error = error.Take();
    return result;
  }
  statement_guard.created = true;

  status_out =
      AdbcStatementSetSqlQuery(&statement_guard.statement, sql.c_str(), error.Ptr());
  if (status_out != ADBC_STATUS_OK) {
    result.error = error.Take();
    return result;
  }

  StreamHolder stream;
  std::int64_t affected = -1;
  before_execute_out = false;
  status_out = AdbcStatementExecuteQuery(&statement_guard.statement,
                                         want_rows ? &stream.stream : nullptr, &affected,
                                         error.Ptr());
  if (status_out != ADBC_STATUS_OK) {
    result.error = error.Take();
    return result;
  }
  result.affected = affected;

  if (!want_rows) {
    return result;
  }

  SchemaHolder schema;
  if (stream.stream.get_schema(&stream.stream, &schema.schema) != 0) {
    result.error = StreamError(stream.stream);
    return result;
  }

  const int64_t field_count = schema.schema.n_children;
  std::vector<FieldInfo> fields;
  fields.reserve(static_cast<std::size_t>(field_count));
  for (int64_t index = 0; index < field_count; ++index) {
    const ArrowSchema* field = schema.schema.children[index];
    FieldInfo info;
    info.name = field->name != nullptr ? field->name : "column " + std::to_string(index + 1);
    ArrowSchemaView view;
    ArrowError arrow_error;
    ArrowErrorInit(&arrow_error);
    if (ArrowSchemaViewInit(&view, field, &arrow_error) == NANOARROW_OK) {
      info.type = view.type;
      info.time_unit = view.time_unit;
      info.decimal_bitwidth = view.decimal_bitwidth;
      info.decimal_precision = view.decimal_precision;
      info.decimal_scale = view.decimal_scale;
    }
    fields.push_back(std::move(info));
    result.columns.push_back(fields.back().name);
  }

  {
    ArrayViewHolder schema_check;
    ArrowError arrow_error;
    ArrowErrorInit(&arrow_error);
    if (ArrowArrayViewInitFromSchema(&schema_check.view, &schema.schema, &arrow_error) !=
        NANOARROW_OK) {
      result.error = DecodeSystemTextToUtf8(arrow_error.message);
      return result;
    }
    schema_check.initialized = true;
  }

  bool stop = false;
  while (!stop) {
    ArrayHolder array;
    if (stream.stream.get_next(&stream.stream, &array.array) != 0) {
      result.error = StreamError(stream.stream);
      break;
    }
    if (array.array.release == nullptr) {
      break;
    }

    ArrayViewHolder root;
    ArrowError arrow_error;
    ArrowErrorInit(&arrow_error);
    if (ArrowArrayViewInitFromSchema(&root.view, &schema.schema, &arrow_error) != NANOARROW_OK) {
      result.error = DecodeSystemTextToUtf8(arrow_error.message);
      break;
    }
    root.initialized = true;
    ArrowErrorInit(&arrow_error);
    if (ArrowArrayViewSetArray(&root.view, &array.array, &arrow_error) != NANOARROW_OK) {
      result.error = DecodeSystemTextToUtf8(arrow_error.message);
      break;
    }

    const int64_t rows = array.array.length;
    for (int64_t row = 0; row < rows; ++row) {
      if (result.rows.size() >= max_rows) {
        result.truncated = true;
        stop = true;
        break;
      }
      GridRow grid_row;
      grid_row.values.reserve(fields.size());
      for (int64_t column = 0; column < field_count; ++column) {
        grid_row.values.push_back(ExtractCell(*root.view.children[column], row, fields[column]));
      }
      result.rows.push_back(std::move(grid_row));
    }
  }

  return result;
}

} // namespace

void AdbcSession::ReleaseResourcesBlocking() noexcept {
  ReleaseNativeAdbcHandles(*this);
  redis.reset();
  request = ConnectionRequest{};
  alive.store(false, std::memory_order_release);
}

AdbcSession::~AdbcSession() {
  MarkClosed();
  ReleaseResourcesBlocking();
}

bool IsConnectionLostError(int adbc_status, std::string_view message) {
  if (adbc_status == ADBC_STATUS_IO) {
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

AdbcOpenResult OpenAdbcBlocking(const ConnectionRequest& request) {
  AdbcOpenResult result;
  auto session = std::make_shared<AdbcSession>();
  session->request = request;
  const Dialect dialect = DetectDialect(request.driver_path, request.uri);
  if (dialect == Dialect::Redis) {
    const RedisParseUriResult parsed =
        ParseRedisUri(request.uri, request.username, request.password);
    if (!parsed.error.empty()) {
      result.error = parsed.error;
      return result;
    }
    auto client = std::make_unique<RedisClient>();
    std::string conn_err;
    if (!client->Connect(parsed.endpoint, conn_err)) {
      result.error = conn_err;
      return result;
    }
    session->dialect = Dialect::Redis;
    session->redis = std::move(client);
    session->initialized = true;
    session->alive.store(true, std::memory_order_release);
    result.session = std::move(session);
    return result;
  }

  AdbcErrorGuard error;

  const auto fail = [&](std::string_view operation) -> AdbcOpenResult {
    result.error = std::string(operation) + ": " + error.Take();
    result.session.reset();
    return result;
  };

  if (AdbcDatabaseNew(&session->database, error.Ptr()) != ADBC_STATUS_OK) {
    return fail(Tr("create ADBC database", "创建 ADBC database"));
  }
  session->database_created = true;

  if (AdbcDriverManagerDatabaseSetLoadFlags(&session->database, ADBC_LOAD_FLAG_DEFAULT,
                                            error.Ptr()) != ADBC_STATUS_OK) {
    return fail(Tr("set ADBC load flags", "设置 ADBC 加载标志"));
  }
  const std::string extra_paths = BuildAdditionalSearchPathList();
  if (!extra_paths.empty() &&
      AdbcDriverManagerDatabaseSetAdditionalSearchPathList(
          &session->database, extra_paths.c_str(), error.Ptr()) != ADBC_STATUS_OK) {
    return fail(Tr("set ADBC search paths", "设置 ADBC 搜索路径"));
  }

  if (AdbcDatabaseSetOption(&session->database, "driver",
                            request.driver_path.c_str(), error.Ptr()) != ADBC_STATUS_OK) {
    return fail(Tr("set ADBC driver", "设置 ADBC driver"));
  }

  const std::string lower_uri = LowerAscii(request.uri);
  const std::string lower_driver = LowerAscii(request.driver_path);
  bool credentials_in_uri = false;
  std::string effective_uri = request.uri;
  if (dialect == Dialect::Postgresql) {
    effective_uri =
        BuildPostgresqlUriWithCredentials(request.uri, request.username, request.password);
    credentials_in_uri = true;
  } else if ((!request.username.empty() || !request.password.empty()) &&
             (lower_uri.starts_with("mysql://") || lower_uri.starts_with("mariadb://") ||
              lower_uri.starts_with("mssql://") || lower_uri.starts_with("sqlserver://") ||
              (lower_driver.find("clickhouse") != std::string::npos &&
               (lower_uri.starts_with("http://") || lower_uri.starts_with("https://"))))) {
    effective_uri = InjectSchemeUriCredentials(request.uri, request.username, request.password);
    credentials_in_uri = true;
  }
  if (!effective_uri.empty() &&
      AdbcDatabaseSetOption(&session->database, ADBC_OPTION_URI, effective_uri.c_str(),
                            error.Ptr()) != ADBC_STATUS_OK) {
    return fail(Tr("set URI", "设置 URI"));
  }

  if (dialect == Dialect::Sqlite && !request.uri.empty() &&
      request.uri != ":memory:" &&
      request.uri.rfind("file:", 0) != 0 && request.uri.find("://") == std::string::npos) {
    std::error_code filesystem_error;
    const std::filesystem::path database_path(request.uri);
    if (database_path.has_parent_path()) {
      std::filesystem::create_directories(database_path.parent_path(), filesystem_error);
    }
  }
  if (!credentials_in_uri && dialect != Dialect::Sqlite) {
    if (!request.username.empty() &&
        AdbcDatabaseSetOption(&session->database, "username", request.username.c_str(),
                              error.Ptr()) != ADBC_STATUS_OK) {
      return fail(Tr("set username", "设置用户名"));
    }
    if (!request.password.empty() &&
        AdbcDatabaseSetOption(&session->database, "password", request.password.c_str(),
                              error.Ptr()) != ADBC_STATUS_OK) {
      return fail(Tr("set password", "设置密码"));
    }
  }
  if (AdbcDatabaseInit(&session->database, error.Ptr()) != ADBC_STATUS_OK) {
    return fail(Tr("initialize database", "初始化数据库"));
  }

  if (AdbcConnectionNew(&session->connection, error.Ptr()) != ADBC_STATUS_OK) {
    return fail(Tr("create connection", "创建连接"));
  }
  session->connection_created = true;
  if (AdbcConnectionInit(&session->connection, &session->database, error.Ptr()) !=
      ADBC_STATUS_OK) {
    return fail(Tr("initialize connection", "初始化连接"));
  }

  session->dialect = dialect;
  session->initialized = true;
  session->alive.store(true, std::memory_order_release);
  result.session = std::move(session);
  return result;
}

bool ReconnectBlocking(AdbcSession& session, std::string& error_out) {
  error_out.clear();
  if (session.IsClosed()) {
    error_out = Tr("Connection is closed", "连接已关闭");
    return false;
  }
  if (session.dialect == Dialect::Redis && session.redis) {
    const bool ok = session.redis->Connect(session.redis->Endpoint(), error_out);
    session.alive.store(ok, std::memory_order_release);
    return ok;
  }
  if (session.dialect == Dialect::Sqlite || session.request.uri == ":memory:" ||
      (session.request.driver_path.empty() && session.request.uri.empty())) {
    return false;
  }

  AdbcOpenResult fresh = OpenAdbcBlocking(session.request);
  if (!fresh.session) {
    ReleaseNativeAdbcHandles(session);
    session.alive.store(false, std::memory_order_release);
    error_out = std::move(fresh.error);
    return false;
  }

  ReleaseNativeAdbcHandles(session);
  session.database = fresh.session->database;
  session.connection = fresh.session->connection;
  session.database_created = fresh.session->database_created;
  session.connection_created = fresh.session->connection_created;
  session.initialized = fresh.session->initialized;
  session.dialect = fresh.session->dialect;
  fresh.session->database = {};
  fresh.session->connection = {};
  fresh.session->database_created = false;
  fresh.session->connection_created = false;
  fresh.session->initialized = false;
  session.alive.store(true, std::memory_order_release);
  return true;
}

AdbcObjectsResult LoadObjectsBlocking(AdbcSession& session) {
  if (session.IsClosed()) {
    AdbcObjectsResult result;
    result.connection_lost = true;
    result.error = Tr("Connection is closed", "连接已关闭");
    return result;
  }
  if (session.dialect == Dialect::Redis && session.redis) {
    AdbcObjectsResult result = LoadRedisObjectsBlocking(*session.redis);
    if (!result.error.empty() &&
        (!session.redis->IsConnected() || IsConnectionLostError(0, result.error))) {
      result.connection_lost = true;
      session.alive.store(false, std::memory_order_release);
    } else if (result.error.empty()) {
      session.alive.store(true, std::memory_order_release);
    }
    return result;
  }

  AdbcStatusCode status = ADBC_STATUS_OK;
  AdbcObjectsResult result = LoadObjectsBlockingOnce(session, status);
  if (!result.error.empty() &&
      (!session.connection_created || IsConnectionLostError(status, result.error))) {
    std::string reconnect_err;
    if (ReconnectBlocking(session, reconnect_err)) {
      status = ADBC_STATUS_OK;
      result = LoadObjectsBlockingOnce(session, status);
    } else if (!reconnect_err.empty()) {
      result.error = std::move(reconnect_err);
    }
    if (!result.error.empty() &&
        (!session.connection_created || IsConnectionLostError(status, result.error) ||
         !session.IsAlive())) {
      result.connection_lost = true;
      session.alive.store(false, std::memory_order_release);
      return result;
    }
  }
  if (result.error.empty()) {
    session.alive.store(true, std::memory_order_release);
  }
  return result;
}

AdbcSchemaResult LoadSchemaBlocking(AdbcSession& session, const DbObject& object) {
  if (session.IsClosed()) {
    AdbcSchemaResult result;
    result.connection_lost = true;
    result.error = Tr("Connection is closed", "连接已关闭");
    return result;
  }
  if (session.dialect == Dialect::Redis) {
    AdbcSchemaResult result;
    result.columns = RedisKeySchema();
    return result;
  }

  AdbcStatusCode status = ADBC_STATUS_OK;
  AdbcSchemaResult result = LoadSchemaBlockingOnce(session, object, status);
  if (!result.error.empty() &&
      (!session.connection_created || IsConnectionLostError(status, result.error))) {
    std::string reconnect_err;
    if (ReconnectBlocking(session, reconnect_err)) {
      status = ADBC_STATUS_OK;
      result = LoadSchemaBlockingOnce(session, object, status);
    } else if (!reconnect_err.empty()) {
      result.error = std::move(reconnect_err);
    }
    if (!result.error.empty() &&
        (!session.connection_created || IsConnectionLostError(status, result.error) ||
         !session.IsAlive())) {
      result.connection_lost = true;
      session.alive.store(false, std::memory_order_release);
      return result;
    }
  }
  if (result.error.empty()) {
    session.alive.store(true, std::memory_order_release);
  }
  return result;
}

AdbcQueryResult ExecuteBlocking(AdbcSession& session, std::string sql, bool want_rows,
                                std::size_t max_rows) {
  if (session.IsClosed()) {
    AdbcQueryResult result;
    result.connection_lost = true;
    result.error = Tr("Connection is closed", "连接已关闭");
    return result;
  }
  const auto started = std::chrono::steady_clock::now();
  const auto elapsed_micros = [started]() -> std::int64_t {
    return std::max<std::int64_t>(
        0, std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now() - started)
               .count());
  };
  if (session.dialect == Dialect::Redis && session.redis) {
    AdbcQueryResult result = ExecuteRedisCommandBlocking(*session.redis, sql, max_rows);
    result.elapsed_us = elapsed_micros();
    if (!result.error.empty() &&
        (!session.redis->IsConnected() || IsConnectionLostError(0, result.error))) {
      result.connection_lost = true;
      session.alive.store(false, std::memory_order_release);
    } else if (result.error.empty()) {
      session.alive.store(true, std::memory_order_release);
    }
    return result;
  }

  AdbcStatusCode status = ADBC_STATUS_OK;
  bool before_execute = false;
  AdbcQueryResult result =
      ExecuteBlockingOnce(session, sql, want_rows, max_rows, status, before_execute);
  if (!result.error.empty() &&
      (!session.connection_created || IsConnectionLostError(status, result.error))) {
    if (before_execute || IsSqlReadStatement(sql)) {
      std::string reconnect_err;
      if (ReconnectBlocking(session, reconnect_err)) {
        status = ADBC_STATUS_OK;
        result = ExecuteBlockingOnce(session, sql, want_rows, max_rows, status, before_execute);
      } else if (!reconnect_err.empty()) {
        result.error = std::move(reconnect_err);
      }
    }
    if (!result.error.empty() &&
        (!session.connection_created || IsConnectionLostError(status, result.error) ||
         !session.IsAlive())) {
      result.connection_lost = true;
      result.elapsed_us = elapsed_micros();
      session.alive.store(false, std::memory_order_release);
      return result;
    }
  }
  if (result.error.empty()) {
    session.alive.store(true, std::memory_order_release);
  }
  result.elapsed_us = elapsed_micros();
  return result;
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

bool IsValidUtf8(std::string_view text) noexcept {
  std::size_t index = 0;
  while (index < text.size()) {
    std::size_t len = 0;
    if (!DecodeUtf8Sequence(text, index, len)) {
      return false;
    }
    index += len;
  }
  return true;
}

std::string SanitizeUtf8(std::string_view text) {
  if (IsValidUtf8(text)) {
    return std::string(text);
  }
  std::string out;
  out.reserve(text.size() + 8);
  std::size_t index = 0;
  while (index < text.size()) {
    std::size_t len = 0;
    if (DecodeUtf8Sequence(text, index, len)) {
      out.append(text.substr(index, len));
      index += len;
    } else {
      out += "\xEF\xBF\xBD";
      ++index;
    }
  }
  return out;
}

std::string QuoteIdentifier(Dialect dialect, std::string_view identifier) {
  const char quote = dialect == Dialect::Mysql ? '`' : '"';
  std::string result;
  result.reserve(identifier.size() + 2);
  result.push_back(quote);
  for (const char c : identifier) {
    if (c == quote) {
      result.push_back(quote);
    }
    result.push_back(c);
  }
  result.push_back(quote);
  return result;
}

std::string ObjectDatabaseName(Dialect dialect, const DbObject& object) {
  if (dialect == Dialect::Redis) {
    return {};
  }
  if (dialect == Dialect::Mysql) {
    if (!object.db_schema.empty()) {
      return object.db_schema;
    }
    if (!object.catalog.empty() && LowerAscii(object.catalog) != "def") {
      return object.catalog;
    }
    return {};
  }
  if (!object.db_schema.empty()) {
    return object.db_schema;
  }
  if (!object.catalog.empty()) {
    const std::string lower = LowerAscii(object.catalog);
    if (lower != "def" && lower != "main") {
      return object.catalog;
    }
  }
  return {};
}

bool IsSystemDatabaseOrSchema(std::string_view name) {
  const std::string lower = LowerAscii(TrimAscii(name));
  if (lower.empty()) {
    return false;
  }
  return lower == "information_schema" || lower == "performance_schema" || lower == "mysql" ||
         lower == "sys" || lower == "pg_catalog" || lower == "pg_toast" ||
         lower == "sqlite_temp_master" || lower.starts_with("pg_temp_") ||
         lower.starts_with("pg_toast_temp_");
}

std::string QualifiedName(Dialect dialect, const DbObject& object) {
  if (object.db_schema.empty()) {
    if (dialect == Dialect::Mysql && !object.catalog.empty() &&
        LowerAscii(object.catalog) != "def") {
      return QuoteIdentifier(dialect, object.catalog) + "." + QuoteIdentifier(dialect, object.name);
    }
    return QuoteIdentifier(dialect, object.name);
  }
  return QuoteIdentifier(dialect, object.db_schema) + "." +
         QuoteIdentifier(dialect, object.name);
}

std::string NormalizeSqlFilter(std::string_view raw) {
  std::string_view text = TrimAscii(raw);
  while (!text.empty() && text.back() == ';') {
    text.remove_suffix(1);
    text = TrimAscii(text);
  }
  if (text.size() > 5 && LowerAscii(text.substr(0, 5)) == "where") {
    const unsigned char next = static_cast<unsigned char>(text[5]);
    if (next == ' ' || next == '\t' || next == '\r' || next == '\n' || next == '(') {
      text.remove_prefix(5);
      text = TrimAscii(text);
    }
  }
  while (!text.empty() && text.back() == ';') {
    text.remove_suffix(1);
    text = TrimAscii(text);
  }
  return std::string(text);
}

std::string BuildDropObjectSql(Dialect dialect, const DbObject& object) {
  const std::string qualified = QualifiedName(dialect, object);
  if (object.is_view) {
    if (ContainsLower(object.table_type, "materialized")) {
      return "DROP MATERIALIZED VIEW " + qualified;
    }
    return "DROP VIEW " + qualified;
  }
  if (ContainsLower(object.table_type, "foreign")) {
    return "DROP FOREIGN TABLE " + qualified;
  }
  return "DROP TABLE " + qualified;
}

std::string BuildRenameTableSql(Dialect dialect, const DbObject& object,
                                std::string_view new_name) {
  const std::string_view trimmed = TrimAscii(new_name);
  if (trimmed.empty() || dialect == Dialect::Redis) {
    return {};
  }
  return "ALTER TABLE " + QualifiedName(dialect, object) + " RENAME TO " +
         QuoteIdentifier(dialect, trimmed);
}

std::string BuildAddColumnSql(Dialect dialect, const DbObject& object,
                              std::string_view column_name, std::string_view column_type,
                              bool nullable, std::string_view default_expr) {
  const std::string_view name = TrimAscii(column_name);
  const std::string_view type = TrimAscii(column_type);
  const std::string_view def = TrimAscii(default_expr);
  if (name.empty() || type.empty() || dialect == Dialect::Redis) {
    return {};
  }
  std::string sql = "ALTER TABLE " + QualifiedName(dialect, object) + " ADD COLUMN " +
                    QuoteIdentifier(dialect, name) + " " + std::string(type);
  if (!nullable) {
    sql += " NOT NULL";
  }
  if (!def.empty()) {
    sql += " DEFAULT ";
    sql.append(def);
  }
  return sql;
}

std::string BuildRenameColumnSql(Dialect dialect, const DbObject& object,
                                 std::string_view old_name, std::string_view new_name) {
  const std::string_view old_trimmed = TrimAscii(old_name);
  const std::string_view new_trimmed = TrimAscii(new_name);
  if (old_trimmed.empty() || new_trimmed.empty() || old_trimmed == new_trimmed ||
      dialect == Dialect::Redis) {
    return {};
  }
  return "ALTER TABLE " + QualifiedName(dialect, object) + " RENAME COLUMN " +
         QuoteIdentifier(dialect, old_trimmed) + " TO " +
         QuoteIdentifier(dialect, new_trimmed);
}

std::string BuildDropColumnSql(Dialect dialect, const DbObject& object,
                               std::string_view column_name) {
  const std::string_view trimmed = TrimAscii(column_name);
  if (trimmed.empty() || dialect == Dialect::Redis) {
    return {};
  }
  return "ALTER TABLE " + QualifiedName(dialect, object) + " DROP COLUMN " +
         QuoteIdentifier(dialect, trimmed);
}

std::string BuildAlterColumnTypeSql(Dialect dialect, const DbObject& object,
                                    std::string_view column_name, std::string_view new_type,
                                    bool nullable) {
  const std::string_view col = TrimAscii(column_name);
  const std::string_view type = TrimAscii(new_type);
  if (col.empty() || type.empty() || dialect == Dialect::Sqlite || dialect == Dialect::Redis) {
    return {};
  }
  if (dialect == Dialect::Mysql) {
    return "ALTER TABLE " + QualifiedName(dialect, object) + " MODIFY COLUMN " +
           QuoteIdentifier(dialect, col) + " " + std::string(type) +
           (nullable ? " NULL" : " NOT NULL");
  }
  return "ALTER TABLE " + QualifiedName(dialect, object) + " ALTER COLUMN " +
         QuoteIdentifier(dialect, col) + " TYPE " + std::string(type);
}

namespace {

bool IsNumericType(std::string_view type) {
  const std::string lower = LowerAscii(type);
  return lower.find("int") != std::string::npos || lower.find("real") != std::string::npos ||
         lower.find("double") != std::string::npos || lower.find("float") != std::string::npos ||
         lower.find("numeric") != std::string::npos ||
         lower.find("decimal") != std::string::npos;
}

bool IsBoolType(std::string_view type) {
  const std::string lower = LowerAscii(type);
  return lower.find("bool") != std::string::npos;
}

bool IsValidNumber(std::string_view text) {
  if (text.empty()) {
    return false;
  }
  if (std::isspace(static_cast<unsigned char>(text.front())) ||
      std::isspace(static_cast<unsigned char>(text.back()))) {
    return false;
  }
  const std::string copy(text);
  errno = 0;
  char* end = nullptr;
  const double value = std::strtod(copy.c_str(), &end);
  return errno != ERANGE && end == copy.c_str() + copy.size() && std::isfinite(value);
}

} // namespace

std::string SqlLiteral(Dialect dialect, std::string_view text, std::string_view declared_type,
                       bool is_null) {
  if (is_null) {
    return "NULL";
  }
  if (IsBoolType(declared_type)) {
    const std::string lower = LowerAscii(text);
    if (lower == "1" || lower == "true") {
      return "TRUE";
    }
    if (lower == "0" || lower == "false") {
      return "FALSE";
    }
  }
  if (IsNumericType(declared_type) && IsValidNumber(text)) {
    return std::string(text);
  }
  std::string result;
  result.reserve(text.size() + 2);
  result.push_back('\'');
  for (const char c : text) {
    if (dialect == Dialect::Mysql && c == '\\') {
      result.push_back('\\');
    }
    if (c == '\'') {
      result.push_back('\'');
    }
    result.push_back(c);
  }
  result.push_back('\'');
  return result;
}

bool IsSqlReadStatement(std::string_view sql) {
  std::string_view text = sql;
  for (;;) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
      return false;
    }
    text.remove_prefix(first);
    if (text.starts_with("--")) {
      const auto newline = text.find('\n');
      text = newline == std::string_view::npos ? std::string_view{} : text.substr(newline + 1);
      continue;
    }
    if (text.starts_with("/*")) {
      const auto close = text.find("*/");
      text = close == std::string_view::npos ? std::string_view{} : text.substr(close + 2);
      continue;
    }
    break;
  }
  if (text.empty()) {
    return false;
  }
  const auto end = text.find_first_of(" \t\r\n(");
  const std::string keyword = LowerAscii(text.substr(0, end));
  if (keyword == "select" || keyword == "pragma" || keyword == "explain" ||
      keyword == "values" || keyword == "table" || keyword == "with" ||
      keyword == "show" || keyword == "describe" || keyword == "desc") {
    return true;
  }
  return LowerAscii(text).find(" returning ") != std::string::npos;
}

bool IsReadStatement(Dialect dialect, std::string_view text) {
  if (dialect == Dialect::Redis) {
    return IsRedisReadCommand(text);
  }
  return IsSqlReadStatement(text);
}

std::string InferDriverForUri(std::string_view uri) {
  const std::string lower = LowerAscii(uri);
  if (lower.starts_with("redis://") || lower.starts_with("rediss://")) {
    return "redis";
  }
  if (lower.starts_with("postgresql://") || lower.starts_with("postgres://")) {
    return "postgresql";
  }
  if (lower.starts_with("mysql://") || lower.starts_with("mariadb://")) {
    return "mysql";
  }
  if (lower.starts_with("duckdb:") || lower.ends_with(".duckdb")) {
    return "duckdb";
  }
  return DefaultSqliteDriverPath();
}

std::string DisplayNameFromUri(std::string_view uri, std::string_view fallback) {
  if (uri.empty()) {
    return std::string(fallback);
  }
  const std::string lower = LowerAscii(uri);
  if (lower.starts_with("redis://") || lower.starts_with("rediss://")) {
    return FormatRedisDisplayName(uri);
  }
  const auto slash = uri.find_last_of("/\\");
  std::string name = slash == std::string_view::npos ? std::string(uri)
                                                     : std::string(uri.substr(slash + 1));
  if (name.empty()) {
    return std::string(fallback);
  }
  return name;
}

namespace {

bool IsUnreservedUriChar(unsigned char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
         c == '-' || c == '.' || c == '_' || c == '~';
}

std::string PercentEncodeUriComponent(std::string_view text) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(text.size());
  for (const unsigned char c : text) {
    if (IsUnreservedUriChar(c)) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[(c >> 4) & 0x0F]);
      out.push_back(kHex[c & 0x0F]);
    }
  }
  return out;
}

std::string LibpqSingleQuote(std::string_view value) {
  std::string out;
  out.reserve(value.size() + 2);
  out.push_back('\'');
  for (const char c : value) {
    if (c == '\'' || c == '\\') {
      out.push_back('\\');
    }
    out.push_back(c);
  }
  out.push_back('\'');
  return out;
}

int HexDigitValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

std::string PercentDecodeUriComponent(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '%' && i + 2 < text.size()) {
      const int hi = HexDigitValue(text[i + 1]);
      const int lo = HexDigitValue(text[i + 2]);
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

std::string InjectCredentialsIntoSchemeUri(std::string_view uri, std::size_t scheme_end,
                                           std::string_view username,
                                           std::string_view password) {
  const std::size_t auth_end_pos = uri.find_first_of("/?#", scheme_end);
  const std::size_t auth_end = auth_end_pos == std::string_view::npos ? uri.size() : auth_end_pos;
  const std::string_view authority = uri.substr(scheme_end, auth_end - scheme_end);
  const std::size_t at_rel = authority.rfind('@');

  const std::string enc_user = PercentEncodeUriComponent(username);
  const std::string enc_pass = PercentEncodeUriComponent(password);

  if (at_rel == std::string_view::npos) {
    std::string out(uri.substr(0, scheme_end));
    out += enc_user;
    if (!enc_pass.empty()) {
      out.push_back(':');
      out += enc_pass;
    }
    out.push_back('@');
    out += uri.substr(scheme_end);
    return out;
  }

  const std::string_view existing_userinfo = authority.substr(0, at_rel);
  const std::size_t colon = existing_userinfo.find(':');
  const std::string_view existing_user =
      colon == std::string_view::npos ? existing_userinfo : existing_userinfo.substr(0, colon);
  const std::string_view existing_pass =
      colon == std::string_view::npos ? std::string_view{} : existing_userinfo.substr(colon + 1);

  const std::string final_user = !username.empty() ? enc_user : std::string(existing_user);
  const std::string final_pass = !password.empty() ? enc_pass : std::string(existing_pass);

  std::string out(uri.substr(0, scheme_end));
  out += final_user;
  if (!final_pass.empty()) {
    out.push_back(':');
    out += final_pass;
  }
  out.push_back('@');
  out += uri.substr(scheme_end + at_rel + 1);
  return out;
}

std::string InjectSchemeUriCredentials(std::string_view uri, std::string_view username,
                                       std::string_view password) {
  if (username.empty() && password.empty()) {
    return std::string(uri);
  }
  const std::size_t scheme_pos = uri.find("://");
  if (scheme_pos == std::string_view::npos) {
    return std::string(uri);
  }
  return InjectCredentialsIntoSchemeUri(uri, scheme_pos + 3, username, password);
}

} // namespace

SanitizedUriResult SanitizeUriCredentials(std::string_view uri) {
  const std::string_view trimmed = TrimAscii(uri);
  SanitizedUriResult res;
  res.uri = std::string(trimmed);

  const std::size_t scheme_pos = trimmed.find("://");
  if (scheme_pos == std::string_view::npos) {
    return res;
  }
  const std::string scheme = LowerAscii(trimmed.substr(0, scheme_pos));
  const std::size_t auth_start = scheme_pos + 3;
  const std::size_t auth_end_pos = trimmed.find_first_of("/?#", auth_start);
  const std::size_t auth_end =
      auth_end_pos == std::string_view::npos ? trimmed.size() : auth_end_pos;
  const std::string_view authority = trimmed.substr(auth_start, auth_end - auth_start);
  const std::size_t at = authority.rfind('@');
  if (at == std::string_view::npos) {
    return res;
  }

  const std::string_view userinfo = authority.substr(0, at);
  const std::string_view host_and_rest = trimmed.substr(auth_start + at + 1);
  const std::size_t colon = userinfo.find(':');
  if (colon != std::string_view::npos) {
    res.username = PercentDecodeUriComponent(userinfo.substr(0, colon));
    res.password = PercentDecodeUriComponent(userinfo.substr(colon + 1));
    res.stripped_password = !res.password.empty();
    res.uri = std::string(trimmed.substr(0, auth_start)) + std::string(host_and_rest);
    return res;
  }

  if (scheme == "redis" || scheme == "rediss") {
    res.password = PercentDecodeUriComponent(userinfo);
    res.stripped_password = !res.password.empty();
    res.uri = std::string(trimmed.substr(0, auth_start)) + std::string(host_and_rest);
    return res;
  }

  res.username = PercentDecodeUriComponent(userinfo);
  return res;
}

std::string DefaultAppDataDir() {
  if (const char* override_dir = std::getenv("DBPP_DATA_DIR");
      override_dir != nullptr && *override_dir != '\0') {
    return override_dir;
  }
#if defined(_WIN32)
  if (const char* local_app_data = std::getenv("LOCALAPPDATA");
      local_app_data != nullptr && *local_app_data != '\0') {
    return (std::filesystem::path(local_app_data) / "dbpp" / "data").string();
  }
  return {};
#elif defined(__APPLE__)
  const char* home = std::getenv("HOME");
  if (home == nullptr || *home == '\0') {
    return {};
  }
  std::string identity = "dbpp";
  const std::filesystem::path exe_dir = ExecutableDirectory();
  if (!exe_dir.empty() && exe_dir.filename() == "MacOS") {
    const std::filesystem::path contents_dir = exe_dir.parent_path();
    std::error_code ec;
    if (contents_dir.filename() == "Contents" &&
        std::filesystem::is_regular_file(contents_dir / "Info.plist", ec)) {
      identity = "dev.farna.dbpp";
    }
  }
  return (std::filesystem::path(home) / "Library" / "Application Support" / identity).string();
#else
  std::filesystem::path data_base;
  if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg != nullptr && *xdg != '\0') {
    data_base = std::filesystem::path(xdg);
  } else if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
    data_base = std::filesystem::path(home) / ".local" / "share";
  }
  if (data_base.empty()) {
    return {};
  }
  return (data_base / "dbpp").string();
#endif
}

std::string BuildPostgresqlUriWithCredentials(std::string_view uri, std::string_view username,
                                              std::string_view password) {
  if (username.empty() && password.empty()) {
    return std::string(uri);
  }

  const std::string lower = LowerAscii(uri);
  if (lower.starts_with("postgresql://")) {
    return InjectCredentialsIntoSchemeUri(uri, 13, username, password);
  }
  if (lower.starts_with("postgres://")) {
    return InjectCredentialsIntoSchemeUri(uri, 11, username, password);
  }

  std::string out(uri);
  const auto append_kv = [&](std::string_view key, std::string_view val) {
    if (val.empty()) {
      return;
    }
    if (!out.empty() && out.back() != ' ' && out.back() != '\t') {
      out.push_back(' ');
    }
    out += key;
    out.push_back('=');
    out += LibpqSingleQuote(val);
  };
  append_kv("user", username);
  append_kv("password", password);
  return out;
}

const std::vector<DatabasePreset>& SupportedDatabasePresets() {
  const auto make_file = [](std::string id, std::string name, std::string subtitle,
                            std::string slug, std::string placeholder,
                            std::vector<std::string> extensions, bool builtin) {
    DatabasePreset p;
    p.id = std::move(id);
    p.name = std::move(name);
    p.subtitle = std::move(subtitle);
    p.driver_slug = std::move(slug);
    p.category = DatabaseCategory::LocalFile;
    p.form_kind = ConnectionFormKind::File;
    p.uri_placeholder = std::move(placeholder);
    p.file_extensions = std::move(extensions);
    p.builtin = builtin;
    return p;
  };
  const auto make_host = [](std::string id, std::string name, std::string subtitle,
                            std::string slug, DatabaseCategory category, std::string scheme,
                            std::string host, std::string port, std::string db,
                            std::string user, std::string placeholder) {
    DatabasePreset p;
    p.id = std::move(id);
    p.name = std::move(name);
    p.subtitle = std::move(subtitle);
    p.driver_slug = std::move(slug);
    p.category = category;
    p.form_kind = ConnectionFormKind::HostPort;
    p.uri_scheme = std::move(scheme);
    p.default_host = std::move(host);
    p.default_port = std::move(port);
    p.default_database = std::move(db);
    p.default_user = std::move(user);
    p.uri_placeholder = std::move(placeholder);
    return p;
  };
  const auto make_uri = [](std::string id, std::string name, std::string subtitle,
                           std::string slug, DatabaseCategory category, ConnectionFormKind kind,
                           std::string placeholder) {
    DatabasePreset p;
    p.id = std::move(id);
    p.name = std::move(name);
    p.subtitle = std::move(subtitle);
    p.driver_slug = std::move(slug);
    p.category = category;
    p.form_kind = kind;
    p.uri_placeholder = std::move(placeholder);
    return p;
  };

  static const std::vector<DatabasePreset> presets = {
      make_file("sqlite", "SQLite", "本地单文件数据库 · 内置驱动", "",
                "/path/to/app.sqlite 或 :memory:", {"sqlite", "sqlite3", "db", "db3", "s3db"},
                true),
      make_host("postgresql", "PostgreSQL", "开源关系型数据库 · 端口 5432", "postgresql",
                DatabaseCategory::Relational, "postgresql", "127.0.0.1", "5432", "postgres",
                "postgres", "postgresql://127.0.0.1:5432/postgres"),
      make_host("mysql", "MySQL", "关系型数据库 · 端口 3306", "mysql",
                DatabaseCategory::Relational, "mysql", "127.0.0.1", "3306", "", "root",
                "mysql://127.0.0.1:3306/mydb"),
      make_host("mariadb", "MariaDB", "MySQL 兼容关系型数据库 · 端口 3306", "mariadb",
                DatabaseCategory::Relational, "mysql", "127.0.0.1", "3306", "", "root",
                "mysql://127.0.0.1:3306/mydb"),
      make_file("duckdb", "DuckDB", "嵌入式列式分析数据库", "duckdb",
                "/path/to/app.duckdb 或 :memory:", {"duckdb", "ddb", "db"}, false),
      [&] {
        DatabasePreset p = make_host(
            "redis", "Redis", "内存键值数据库 · 内置 RESP 客户端 · 端口 6379", "redis",
            DatabaseCategory::KeyValue, "redis", "127.0.0.1", "6379", "0", "",
            "redis://127.0.0.1:6379/0");
        p.builtin = true;
        return p;
      }(),
      make_host("mssql", "SQL Server", "Microsoft SQL Server · 端口 1433", "mssql",
                DatabaseCategory::Relational, "mssql", "127.0.0.1", "1433", "master", "sa",
                "mssql://127.0.0.1:1433?database=master"),
      make_host("clickhouse", "ClickHouse", "实时 OLAP 分析数据库 · HTTP 8123", "clickhouse",
                DatabaseCategory::AnalyticsCloud, "http", "127.0.0.1", "8123", "default",
                "default", "http://127.0.0.1:8123/?database=default"),
      make_uri("flightsql", "Arrow Flight SQL", "高性能 Arrow 原生 SQL 协议", "flightsql",
               DatabaseCategory::AnalyticsCloud, ConnectionFormKind::Uri,
               "grpc+tls://host:31337"),
      make_uri("snowflake", "Snowflake", "云数据仓库 · ADBC 驱动", "snowflake",
               DatabaseCategory::AnalyticsCloud, ConnectionFormKind::Uri,
               "user:password@account/database/schema"),
      make_uri("bigquery", "BigQuery", "Google Cloud 分析型数据仓库", "bigquery",
               DatabaseCategory::AnalyticsCloud, ConnectionFormKind::Uri,
               "bigquery://project/dataset"),
      make_uri("trino", "Trino", "分布式联邦 SQL 查询引擎", "trino",
               DatabaseCategory::AnalyticsCloud, ConnectionFormKind::Uri,
               "http://host:8080/catalog/schema"),
      make_uri("databricks", "Databricks", "Lakehouse SQL Warehouse", "databricks",
               DatabaseCategory::AnalyticsCloud, ConnectionFormKind::Uri,
               "databricks://token:...@host:443/sql/1.0/warehouses/..."),
      make_uri("redshift", "Amazon Redshift", "AWS 云数据仓库 · 端口 5439", "redshift",
               DatabaseCategory::AnalyticsCloud, ConnectionFormKind::Uri,
               "postgresql://host:5439/dev"),
      make_uri("custom", "自定义 ADBC 驱动", "任意驱动短名、.toml 或共享库路径", "",
               DatabaseCategory::Custom, ConnectionFormKind::Custom,
               "/path/to/db.sqlite · postgresql://host/db"),
  };
  return presets;
}

std::string ComposePresetUri(const DatabasePreset& preset, std::string_view host,
                             std::string_view port, std::string_view database,
                             std::string_view extra_query) {
  std::string_view h = TrimAscii(host);
  if (h.find("://") != std::string_view::npos) {
    return std::string(h);
  }
  if (h.empty()) {
    h = preset.default_host.empty() ? std::string_view("127.0.0.1")
                                    : std::string_view(preset.default_host);
  }
  const std::string_view p = TrimAscii(port);
  const std::string_view db = TrimAscii(database);
  std::string_view q = TrimAscii(extra_query);
  if (!q.empty() && q.front() == '?') {
    q.remove_prefix(1);
  }

  std::string formatted_host(h);
  const std::size_t first_colon = formatted_host.find(':');
  const std::size_t last_colon = formatted_host.rfind(':');
  const bool already_bracketed = !formatted_host.empty() && formatted_host.front() == '[';
  const bool has_multiple_colons =
      first_colon != std::string::npos && first_colon != last_colon;
  if (!already_bracketed && has_multiple_colons) {
    formatted_host = "[" + formatted_host + "]";
  }

  bool host_includes_port = false;
  if (already_bracketed) {
    const std::size_t close = formatted_host.find(']');
    host_includes_port = close != std::string::npos && close + 1 < formatted_host.size() &&
                         formatted_host[close + 1] == ':';
  } else if (first_colon != std::string::npos && first_colon == last_colon) {
    host_includes_port = true;
  }

  std::string authority = formatted_host;
  if (!host_includes_port && !p.empty()) {
    authority.push_back(':');
    authority.append(p);
  }

  const std::string enc_db = PercentEncodeUriComponent(db);
  if (preset.id == "mssql") {
    std::string uri = "mssql://" + authority;
    std::string query;
    if (!enc_db.empty()) {
      query = "database=" + enc_db;
    }
    if (!q.empty()) {
      if (!query.empty()) {
        query.push_back('&');
      }
      query.append(q);
    }
    if (!query.empty()) {
      uri.push_back('?');
      uri += query;
    }
    return uri;
  }
  if (preset.id == "clickhouse") {
    std::string uri = "http://" + authority + "/";
    std::string query;
    if (!enc_db.empty()) {
      query = "database=" + enc_db;
    }
    if (!q.empty()) {
      if (!query.empty()) {
        query.push_back('&');
      }
      query.append(q);
    }
    if (!query.empty()) {
      uri.push_back('?');
      uri += query;
    }
    return uri;
  }

  const std::string_view scheme =
      preset.uri_scheme.empty() ? std::string_view("postgresql")
                                : std::string_view(preset.uri_scheme);
  std::string uri = std::string(scheme) + "://" + authority;
  if (!enc_db.empty()) {
    uri.push_back('/');
    uri += enc_db;
  }
  if (!q.empty()) {
    uri.push_back('?');
    uri.append(q);
  }
  return uri;
}

DecomposedPresetUri DecomposePresetUri(const DatabasePreset& preset, std::string_view uri) {
  DecomposedPresetUri out;
  const std::string_view trimmed = TrimAscii(uri);
  const std::size_t scheme_pos = trimmed.find("://");
  if (scheme_pos == std::string_view::npos) {
    return out;
  }

  const std::string_view scheme = trimmed.substr(0, scheme_pos);
  const std::size_t pos = scheme_pos + 3;
  const std::size_t auth_end_pos = trimmed.find_first_of("/?#", pos);
  const std::size_t auth_end =
      auth_end_pos == std::string_view::npos ? trimmed.size() : auth_end_pos;
  std::string_view authority = trimmed.substr(pos, auth_end - pos);

  if (const std::size_t at = authority.rfind('@'); at != std::string_view::npos) {
    const std::string_view userinfo = authority.substr(0, at);
    authority.remove_prefix(at + 1);
    if (const std::size_t colon = userinfo.find(':'); colon != std::string_view::npos) {
      out.username = PercentDecodeUriComponent(userinfo.substr(0, colon));
      out.password = PercentDecodeUriComponent(userinfo.substr(colon + 1));
    } else if (EqualsIgnoreCase(scheme, "redis") || EqualsIgnoreCase(scheme, "rediss")) {
      out.password = PercentDecodeUriComponent(userinfo);
    } else {
      out.username = PercentDecodeUriComponent(userinfo);
    }
  }

  if (authority.empty()) {
    return out;
  }
  if (authority.front() == '[') {
    const std::size_t close = authority.find(']');
    if (close == std::string_view::npos) {
      return out;
    }
    out.host = std::string(authority.substr(1, close - 1));
    if (close + 1 < authority.size()) {
      if (authority[close + 1] != ':') {
        return out;
      }
      out.port = std::string(authority.substr(close + 2));
    }
  } else {
    const std::size_t colon = authority.rfind(':');
    if (colon != std::string_view::npos && authority.find(':') == colon) {
      out.host = std::string(authority.substr(0, colon));
      out.port = std::string(authority.substr(colon + 1));
    } else {
      out.host = std::string(authority);
    }
  }

  std::string_view rest = trimmed.substr(auth_end);
  if (const std::size_t hash = rest.find('#'); hash != std::string_view::npos) {
    rest = rest.substr(0, hash);
  }
  std::string_view path;
  std::string_view query;
  if (const std::size_t qmark = rest.find('?'); qmark != std::string_view::npos) {
    path = rest.substr(0, qmark);
    query = rest.substr(qmark + 1);
  } else {
    path = rest;
  }

  if (!path.empty() && path.front() == '/') {
    path.remove_prefix(1);
  }

  if (preset.id == "mssql" || preset.id == "clickhouse") {
    std::string remaining_query;
    std::size_t qpos = 0;
    while (qpos <= query.size() && !query.empty()) {
      const std::size_t amp = query.find('&', qpos);
      const std::string_view param =
          amp == std::string_view::npos ? query.substr(qpos) : query.substr(qpos, amp - qpos);
      const std::size_t eq = param.find('=');
      const std::string_view key = eq == std::string_view::npos ? param : param.substr(0, eq);
      const std::string_view val =
          eq == std::string_view::npos ? std::string_view{} : param.substr(eq + 1);
      if (EqualsIgnoreCase(key, "database") && out.database.empty()) {
        out.database = PercentDecodeUriComponent(val);
      } else if (!param.empty()) {
        if (!remaining_query.empty()) {
          remaining_query.push_back('&');
        }
        remaining_query.append(param);
      }
      if (amp == std::string_view::npos) {
        break;
      }
      qpos = amp + 1;
    }
    if (out.database.empty() && !path.empty()) {
      out.database = PercentDecodeUriComponent(path);
    }
    out.extra_query = std::move(remaining_query);
  } else {
    if (!path.empty()) {
      out.database = PercentDecodeUriComponent(path);
    }
    out.extra_query = std::string(query);
  }

  out.parsed = !out.host.empty();
  return out;
}

const DatabasePreset& ResolvePresetForProfile(std::string_view driver_path, std::string_view uri) {
  const auto& presets = SupportedDatabasePresets();
  const DatabaseBrand brand = DetectDatabaseBrand(driver_path, uri);
  std::string_view target_id = "custom";
  switch (brand) {
    case DatabaseBrand::Sqlite: target_id = "sqlite"; break;
    case DatabaseBrand::Postgresql: target_id = "postgresql"; break;
    case DatabaseBrand::Mysql: target_id = "mysql"; break;
    case DatabaseBrand::Mariadb: target_id = "mariadb"; break;
    case DatabaseBrand::Duckdb: target_id = "duckdb"; break;
    case DatabaseBrand::Redis: target_id = "redis"; break;
    case DatabaseBrand::Mssql: target_id = "mssql"; break;
    case DatabaseBrand::Clickhouse: target_id = "clickhouse"; break;
    case DatabaseBrand::FlightSql: target_id = "flightsql"; break;
    case DatabaseBrand::Snowflake: target_id = "snowflake"; break;
    case DatabaseBrand::BigQuery: target_id = "bigquery"; break;
    case DatabaseBrand::Trino: target_id = "trino"; break;
    case DatabaseBrand::Databricks: target_id = "databricks"; break;
    case DatabaseBrand::Redshift: target_id = "redshift"; break;
    case DatabaseBrand::Custom: target_id = "custom"; break;
  }
  for (const DatabasePreset& preset : presets) {
    if (preset.id == target_id) {
      return preset;
    }
  }
  return presets.back();
}

std::optional<AdbcDriverInfo> FindInstalledDriverForPreset(
    const DatabasePreset& preset, const std::vector<AdbcDriverInfo>& discovered) {
  if (preset.id == "redis") {
    return AdbcDriverInfo{
        .name = "Redis",
        .path = "redis",
        .detail = std::string(Tr("Built-in RESP2/RESP3 client", "内置 RESP2/RESP3 客户端")),
        .version = {},
        .is_manifest = false,
    };
  }
  if (preset.builtin) {
    const std::string default_path = DefaultSqliteDriverPath();
    for (const AdbcDriverInfo& info : discovered) {
      if (info.path == default_path ||
          (info.is_manifest && EqualsIgnoreCase(info.path, "sqlite")) ||
          ContainsLower(info.path, "adbc_driver_sqlite")) {
        return info;
      }
    }
    return AdbcDriverInfo{
        .name = "SQLite",
        .path = default_path,
        .detail = default_path,
        .version = {},
        .is_manifest = false,
    };
  }
  if (preset.driver_slug.empty()) {
    return std::nullopt;
  }
  for (const AdbcDriverInfo& info : discovered) {
    if (info.is_manifest && EqualsIgnoreCase(info.path, preset.driver_slug)) {
      return info;
    }
  }
  const std::string lib_token = "adbc_driver_" + LowerAscii(preset.driver_slug);
  for (const AdbcDriverInfo& info : discovered) {
    if (ContainsLower(info.path, lib_token) || EqualsIgnoreCase(info.name, preset.name)) {
      return info;
    }
  }
  return std::nullopt;
}

DatabaseBrand DetectDatabaseBrand(std::string_view driver_or_id, std::string_view uri,
                                  Dialect dialect) {
  if (!driver_or_id.empty()) {
    const std::string lower = LowerAscii(driver_or_id);
    if (lower.find("redis") != std::string::npos) return DatabaseBrand::Redis;
    if (lower.find("mariadb") != std::string::npos) return DatabaseBrand::Mariadb;
    if (lower.find("mysql") != std::string::npos) return DatabaseBrand::Mysql;
    // Check redshift before postgresql because Redshift uses postgresql:// URIs.
    if (lower.find("redshift") != std::string::npos) return DatabaseBrand::Redshift;
    if (lower.find("postgres") != std::string::npos ||
        lower.find("pgsql") != std::string::npos ||
        lower.find("libpq") != std::string::npos) {
      return DatabaseBrand::Postgresql;
    }
    if (lower.find("sqlite") != std::string::npos) return DatabaseBrand::Sqlite;
    if (lower.find("duckdb") != std::string::npos) return DatabaseBrand::Duckdb;
    if (lower.find("mssql") != std::string::npos ||
        lower.find("sqlserver") != std::string::npos ||
        lower.find("sql server") != std::string::npos) {
      return DatabaseBrand::Mssql;
    }
    if (lower.find("clickhouse") != std::string::npos) return DatabaseBrand::Clickhouse;
    if (lower.find("flightsql") != std::string::npos ||
        lower.find("flight_sql") != std::string::npos ||
        lower.find("flight sql") != std::string::npos ||
        lower.find("arrow") != std::string::npos) {
      return DatabaseBrand::FlightSql;
    }
    if (lower.find("snowflake") != std::string::npos) return DatabaseBrand::Snowflake;
    if (lower.find("bigquery") != std::string::npos) return DatabaseBrand::BigQuery;
    if (lower.find("trino") != std::string::npos ||
        lower.find("presto") != std::string::npos) {
      return DatabaseBrand::Trino;
    }
    if (lower.find("databricks") != std::string::npos) return DatabaseBrand::Databricks;
  }

  if (!uri.empty()) {
    const std::string lower = LowerAscii(uri);
    if (lower.starts_with("redis://") || lower.starts_with("rediss://")) {
      return DatabaseBrand::Redis;
    }
    if (lower.starts_with("mariadb://")) return DatabaseBrand::Mariadb;
    if (lower.starts_with("mysql://")) return DatabaseBrand::Mysql;
    if (lower.find("redshift") != std::string::npos ||
        lower.find(":5439") != std::string::npos) {
      return DatabaseBrand::Redshift;
    }
    if (lower.starts_with("postgres://") || lower.starts_with("postgresql://")) {
      return DatabaseBrand::Postgresql;
    }
    if (lower.find("duckdb") != std::string::npos ||
        lower.find(".ddb") != std::string::npos) {
      return DatabaseBrand::Duckdb;
    }
    if (lower.starts_with("mssql://") || lower.starts_with("sqlserver://")) {
      return DatabaseBrand::Mssql;
    }
    if (lower.find("clickhouse") != std::string::npos ||
        lower.find(":8123") != std::string::npos) {
      return DatabaseBrand::Clickhouse;
    }
    if (lower.find("flightsql") != std::string::npos ||
        lower.find(":31337") != std::string::npos) {
      return DatabaseBrand::FlightSql;
    }
    if (lower.find("snowflake") != std::string::npos) return DatabaseBrand::Snowflake;
    if (lower.starts_with("bigquery://")) return DatabaseBrand::BigQuery;
    if (lower.starts_with("trino://")) return DatabaseBrand::Trino;
    if (lower.starts_with("databricks://")) return DatabaseBrand::Databricks;
    if (lower.find(".sqlite3") != std::string::npos ||
        lower.find(".sqlite") != std::string::npos ||
        lower.find(".db3") != std::string::npos ||
        lower.find(".s3db") != std::string::npos) {
      return DatabaseBrand::Sqlite;
    }
  }

  switch (dialect) {
    case Dialect::Sqlite: return DatabaseBrand::Sqlite;
    case Dialect::Postgresql: return DatabaseBrand::Postgresql;
    case Dialect::Mysql: return DatabaseBrand::Mysql;
    case Dialect::Duckdb: return DatabaseBrand::Duckdb;
    case Dialect::Redis: return DatabaseBrand::Redis;
    case Dialect::Generic:
    default:
      return DatabaseBrand::Custom;
  }
}

namespace {

void StripVersionSuffixes(std::string& base) {
  for (;;) {
    const auto dot = base.rfind('.');
    if (dot == std::string::npos || dot + 1 >= base.size()) {
      break;
    }
    bool all_digits = true;
    for (std::size_t i = dot + 1; i < base.size(); ++i) {
      if (!std::isdigit(static_cast<unsigned char>(base[i]))) {
        all_digits = false;
        break;
      }
    }
    if (!all_digits) {
      break;
    }
    base.resize(dot);
  }
}

std::string DriverDisplayName(const std::filesystem::path& path) {
  std::string filename = path.filename().string();
  std::string base = filename;
  if (LowerAscii(base).ends_with(".toml")) {
    base.resize(base.size() - 5);
  } else {
    for (const std::string_view suffix : {".so", ".dylib", ".dll"}) {
      const auto position = LowerAscii(base).find(suffix);
      if (position != std::string::npos) {
        base.resize(position);
        if (suffix == ".dylib") {
          StripVersionSuffixes(base);
        }
        break;
      }
    }
  }
  if (base.starts_with("lib")) {
    base.erase(0, 3);
  }
  if (base.starts_with("adbc_driver_")) {
    base.erase(0, 12);
  }
  const std::string lower = LowerAscii(base);
  if (lower == "sqlite") return "SQLite";
  if (lower == "postgresql" || lower == "postgres") return "PostgreSQL";
  if (lower == "mysql" || lower == "mariadb") return "MySQL";
  if (lower == "duckdb") return "DuckDB";
  if (lower == "redis") return "Redis";
  if (lower == "flightsql") return "Flight SQL";
  if (lower == "snowflake") return "Snowflake";
  if (lower == "bigquery") return "BigQuery";
  if (lower == "clickhouse") return "ClickHouse";
  if (lower == "mssql") return "SQL Server";
  if (lower == "databricks") return "Databricks";
  if (lower == "redshift") return "Redshift";
  if (lower == "trino") return "Trino";
  if (lower == "presto") return "Presto";
  if (lower == "cassandra") return "Cassandra";
  if (lower == "datafusion") return "DataFusion";
  if (lower == "druid") return "Druid";
  if (lower == "exasol") return "Exasol";
  if (lower == "firebolt") return "Firebolt";
  if (lower == "quack") return "DuckDB Quack";
  std::string display = base;
  std::ranges::replace(display, '_', ' ');
  std::ranges::replace(display, '-', ' ');
  return display.empty() ? filename : display;
}

bool IsDriverFileName(std::string_view filename) {
  const std::string lower = LowerAscii(filename);
  if (lower.find("adbc_driver") == std::string::npos) {
    return false;
  }
  return lower.ends_with(".so") || lower.ends_with(".dylib") ||
         lower.ends_with(".dll") || lower.find(".so.") != std::string::npos;
}

std::string DriverBaseKey(std::string_view filename) {
  std::string base = LowerAscii(filename);
  const auto position = base.find(".so");
  if (position != std::string::npos) {
    base.resize(position);
  } else {
    for (const std::string_view suffix : {".dylib", ".dll"}) {
      const auto extension = base.rfind(suffix);
      if (extension != std::string::npos) {
        base.resize(extension);
        if (suffix == ".dylib") {
          StripVersionSuffixes(base);
        }
        break;
      }
    }
  }
  return base;
}

} // namespace

std::string UserAdbcDriversDir() {
#if defined(_WIN32)
  if (const char* local_app_data = std::getenv("LOCALAPPDATA");
      local_app_data != nullptr && *local_app_data != '\0') {
    return (std::filesystem::path(local_app_data) / "ADBC" / "Drivers").string();
  }
  return {};
#elif defined(__APPLE__)
  if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
    return (std::filesystem::path(home) / "Library" / "Application Support" / "ADBC" / "Drivers")
        .string();
  }
  return {};
#else
  std::filesystem::path config_dir;
  if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && *xdg != '\0') {
    config_dir = std::filesystem::path(xdg);
  } else if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
    config_dir = std::filesystem::path(home) / ".config";
  }
  if (config_dir.empty()) {
    return {};
  }
  return (config_dir / "adbc" / "drivers").string();
#endif
}

std::vector<AdbcDriverInfo> DiscoverAdbcDrivers() {
  std::map<std::string, AdbcDriverInfo> by_key;

  const auto add_manifest_file = [&](const std::filesystem::path& path, bool use_slug) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
      return;
    }
    if (!HasTomlExtension(path) || IsProfileManifest(path)) {
      return;
    }
    const std::string slug = path.stem().string();
    if (slug.empty()) {
      return;
    }
    const std::string key = "manifest:" + LowerAscii(slug);
    if (by_key.contains(key)) {
      return;
    }
    const auto parsed = ParseDriverManifest(path);
    if (!parsed) {
      return;
    }
    std::error_code absolute_error;
    std::filesystem::path absolute = std::filesystem::absolute(path, absolute_error);
    if (absolute_error) {
      absolute = path;
    }
    const std::string display_name =
        parsed->name.empty() ? DriverDisplayName(path) : parsed->name;
    const std::string driver_value = use_slug ? slug : absolute.string();
    const std::string detail =
        (parsed->version.empty() ? "manifest · " : "manifest · v" + parsed->version + " · ") +
        absolute.string();
    by_key[key] = AdbcDriverInfo{
        .name = display_name,
        .path = driver_value,
        .detail = detail,
        .version = parsed->version,
        .is_manifest = true,
    };
  };

  const auto add_manifest_dir = [&](const std::filesystem::path& dir) {
    if (dir.empty()) {
      return;
    }
    std::error_code error;
    if (!std::filesystem::is_directory(dir, error)) {
      return;
    }
    std::vector<std::filesystem::path> files;
    for (std::filesystem::directory_iterator it(dir, error), end; it != end;
         it.increment(error)) {
      if (error) {
        break;
      }
      files.push_back(it->path());
    }
    std::ranges::sort(files, [](const auto& left, const auto& right) {
      return left.filename().string() < right.filename().string();
    });
    for (const auto& file : files) {
      add_manifest_file(file, true);
    }
  };

  const auto add_manifest_env_entries = [&](const char* variable) {
    if (const char* value = std::getenv(variable); value != nullptr) {
      for (const std::string& entry : SplitSearchPath(value)) {
        if (entry.empty()) {
          continue;
        }
        const std::filesystem::path path(entry);
        std::error_code error;
        if (std::filesystem::is_directory(path, error)) {
          add_manifest_dir(path);
        } else {
          add_manifest_file(path, false);
        }
      }
    }
  };

  // Scan manifests in ADBC search priority order (first-wins per slug).
  add_manifest_env_entries("ADBC_DRIVER_PATH");
  add_manifest_env_entries("DBPP_ADBC_DRIVERS");
  for (const char* env_var : {"VIRTUAL_ENV", "CONDA_PREFIX"}) {
    add_manifest_dir(EnvAdbcDriversDir(env_var));
  }
  add_manifest_dir(UserAdbcDriversDir());
  add_manifest_dir(SystemAdbcDriversDir());

  const auto add_library_file = [&](const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
      return;
    }
    const std::string filename = path.filename().string();
    if (!IsDriverFileName(filename)) {
      return;
    }
    const std::string key = "lib:" + DriverBaseKey(filename);
    std::error_code absolute_error;
    std::filesystem::path absolute = std::filesystem::absolute(path, absolute_error);
    if (absolute_error) {
      absolute = path;
    }
    const auto existing = by_key.find(key);
    if (existing != by_key.end() &&
        std::filesystem::path(existing->second.path).filename().string().size() <=
            filename.size()) {
      return;
    }
    by_key[key] = AdbcDriverInfo{
        .name = DriverDisplayName(path),
        .path = absolute.string(),
        .detail = absolute.string(),
        .version = {},
        .is_manifest = false,
    };
  };

  const auto add_library_entry = [&](std::string entry) {
    if (entry.empty()) {
      return;
    }
    std::filesystem::path path(entry);
    std::error_code error;
    if (std::filesystem::is_directory(path, error)) {
      for (std::filesystem::directory_iterator it(path, error), end; it != end;
           it.increment(error)) {
        if (error) {
          break;
        }
        add_library_file(it->path());
      }
    } else {
      add_library_file(path);
    }
  };

  for (const char* variable : {"DBPP_ADBC_DRIVERS", "ADBC_DRIVER_PATH"}) {
    if (const char* value = std::getenv(variable); value != nullptr) {
      for (const std::string& entry : SplitSearchPath(value)) {
        add_library_entry(entry);
      }
    }
  }

  add_library_entry(BundledDriverDirectory().string());
  add_library_file(DefaultSqliteDriverPath());
  const std::filesystem::path default_driver(DefaultSqliteDriverPath());
  if (default_driver.has_parent_path()) {
    add_library_entry(default_driver.parent_path().string());
  }

#if !defined(_WIN32)
  for (const char* directory :
       {"/usr/lib64", "/usr/lib", "/usr/local/lib64", "/usr/local/lib",
        "/opt/homebrew/lib"}) {
    add_library_entry(directory);
  }
#endif

  std::vector<AdbcDriverInfo> drivers;
  drivers.reserve(by_key.size());
  for (auto& [key, driver] : by_key) {
    (void)key;
    drivers.push_back(std::move(driver));
  }
  std::ranges::sort(drivers, [](const AdbcDriverInfo& left, const AdbcDriverInfo& right) {
    if (left.name != right.name) {
      return left.name < right.name;
    }
    if (left.is_manifest != right.is_manifest) {
      return left.is_manifest > right.is_manifest;
    }
    return left.path < right.path;
  });
  return drivers;
}


} // namespace dbpp
