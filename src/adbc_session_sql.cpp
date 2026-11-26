module;

#include <huxerui/datatable.h>

#include <cerrno>

module dbpp.adbc;

import std;
import dbpp.types;
import dbpp.i18n;
import dbpp.redis;

namespace dbpp {

bool IsValidUtf8(std::string_view text) noexcept {
  std::size_t index = 0;
  while (index < text.size()) {
    std::size_t len = 0;
    if (!detail::AdbcDecodeUtf8Sequence(text, index, len)) {
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
    if (detail::AdbcDecodeUtf8Sequence(text, index, len)) {
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
    if (!object.catalog.empty() && detail::AdbcLowerAscii(object.catalog) != "def") {
      return object.catalog;
    }
    return {};
  }
  if (!object.db_schema.empty()) {
    return object.db_schema;
  }
  if (!object.catalog.empty()) {
    const std::string lower = detail::AdbcLowerAscii(object.catalog);
    if (lower != "def" && lower != "main") {
      return object.catalog;
    }
  }
  return {};
}

bool IsSystemDatabaseOrSchema(std::string_view name) {
  const std::string lower = detail::AdbcLowerAscii(detail::AdbcTrimAscii(name));
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
        detail::AdbcLowerAscii(object.catalog) != "def") {
      return QuoteIdentifier(dialect, object.catalog) + "." + QuoteIdentifier(dialect, object.name);
    }
    return QuoteIdentifier(dialect, object.name);
  }
  return QuoteIdentifier(dialect, object.db_schema) + "." +
         QuoteIdentifier(dialect, object.name);
}

std::string NormalizeSqlFilter(std::string_view raw) {
  std::string_view text = detail::AdbcTrimAscii(raw);
  while (!text.empty() && text.back() == ';') {
    text.remove_suffix(1);
    text = detail::AdbcTrimAscii(text);
  }
  if (text.size() > 5 && detail::AdbcLowerAscii(text.substr(0, 5)) == "where") {
    const unsigned char next = static_cast<unsigned char>(text[5]);
    if (next == ' ' || next == '\t' || next == '\r' || next == '\n' || next == '(') {
      text.remove_prefix(5);
      text = detail::AdbcTrimAscii(text);
    }
  }
  while (!text.empty() && text.back() == ';') {
    text.remove_suffix(1);
    text = detail::AdbcTrimAscii(text);
  }
  return std::string(text);
}

std::string BuildDropObjectSql(Dialect dialect, const DbObject& object) {
  const std::string qualified = QualifiedName(dialect, object);
  if (object.is_view) {
    if (detail::AdbcContainsLower(object.table_type, "materialized")) {
      return "DROP MATERIALIZED VIEW " + qualified;
    }
    return "DROP VIEW " + qualified;
  }
  if (detail::AdbcContainsLower(object.table_type, "foreign")) {
    return "DROP FOREIGN TABLE " + qualified;
  }
  return "DROP TABLE " + qualified;
}

std::string BuildRenameTableSql(Dialect dialect, const DbObject& object,
                                std::string_view new_name) {
  const std::string_view trimmed = detail::AdbcTrimAscii(new_name);
  if (trimmed.empty() || dialect == Dialect::Redis) {
    return {};
  }
  return "ALTER TABLE " + QualifiedName(dialect, object) + " RENAME TO " +
         QuoteIdentifier(dialect, trimmed);
}

std::string BuildAddColumnSql(Dialect dialect, const DbObject& object,
                              std::string_view column_name, std::string_view column_type,
                              bool nullable, std::string_view default_expr) {
  const std::string_view name = detail::AdbcTrimAscii(column_name);
  const std::string_view type = detail::AdbcTrimAscii(column_type);
  const std::string_view def = detail::AdbcTrimAscii(default_expr);
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
  const std::string_view old_trimmed = detail::AdbcTrimAscii(old_name);
  const std::string_view new_trimmed = detail::AdbcTrimAscii(new_name);
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
  const std::string_view trimmed = detail::AdbcTrimAscii(column_name);
  if (trimmed.empty() || dialect == Dialect::Redis) {
    return {};
  }
  return "ALTER TABLE " + QualifiedName(dialect, object) + " DROP COLUMN " +
         QuoteIdentifier(dialect, trimmed);
}

std::string BuildAlterColumnTypeSql(Dialect dialect, const DbObject& object,
                                    std::string_view column_name, std::string_view new_type,
                                    bool nullable) {
  const std::string_view col = detail::AdbcTrimAscii(column_name);
  const std::string_view type = detail::AdbcTrimAscii(new_type);
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
  const std::string lower = detail::AdbcLowerAscii(type);
  return lower.find("int") != std::string::npos || lower.find("real") != std::string::npos ||
         lower.find("double") != std::string::npos || lower.find("float") != std::string::npos ||
         lower.find("numeric") != std::string::npos ||
         lower.find("decimal") != std::string::npos;
}

bool IsBoolType(std::string_view type) {
  const std::string lower = detail::AdbcLowerAscii(type);
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
    const std::string lower = detail::AdbcLowerAscii(text);
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
  const std::string keyword = detail::AdbcLowerAscii(text.substr(0, end));
  if (keyword == "select" || keyword == "pragma" || keyword == "explain" ||
      keyword == "values" || keyword == "table" || keyword == "with" ||
      keyword == "show" || keyword == "describe" || keyword == "desc") {
    return true;
  }
  return detail::AdbcLowerAscii(text).find(" returning ") != std::string::npos;
}

bool IsReadStatement(Dialect dialect, std::string_view text) {
  if (dialect == Dialect::Redis) {
    return IsRedisReadCommand(text);
  }
  return IsSqlReadStatement(text);
}

std::string InferDriverForUri(std::string_view uri) {
  const std::string lower = detail::AdbcLowerAscii(uri);
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
  const std::string lower = detail::AdbcLowerAscii(uri);
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
  const std::string_view trimmed = detail::AdbcTrimAscii(uri);
  SanitizedUriResult res;
  res.uri = std::string(trimmed);

  const std::size_t scheme_pos = trimmed.find("://");
  if (scheme_pos == std::string_view::npos) {
    return res;
  }
  const std::string scheme = detail::AdbcLowerAscii(trimmed.substr(0, scheme_pos));
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
  const std::filesystem::path exe_dir = detail::AdbcExecutableDirectory();
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

  const std::string lower = detail::AdbcLowerAscii(uri);
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


namespace detail {

std::string AdbcPercentEncodeUriComponent(std::string_view text) {
  return PercentEncodeUriComponent(text);
}
std::string AdbcPercentDecodeUriComponent(std::string_view text) {
  return PercentDecodeUriComponent(text);
}
bool AdbcEqualsIgnoreCase(std::string_view left, std::string_view right) {
  return EqualsIgnoreCase(left, right);
}
std::string AdbcInjectSchemeUriCredentials(std::string_view uri, std::string_view username,
                                           std::string_view password) {
  return InjectSchemeUriCredentials(uri, username, password);
}

} // namespace detail

} // namespace dbpp
