module;

#include <huxerui/datatable.h>

#ifndef DBPP_VERSION
#define DBPP_VERSION "0.4.5"
#endif

module dbpp.cli;

import std;
import dbpp.types;
import dbpp.adbc;
import dbpp.redis;
import dbpp.driver_installer;
import dbpp.db;

namespace dbpp {

namespace detail {

void CliJsonWriter::Raw(std::string_view text) { buf.append(text); }
void CliJsonWriter::BeginObject() { buf.push_back('{'); }
void CliJsonWriter::EndObject() { buf.push_back('}'); }
void CliJsonWriter::BeginArray() { buf.push_back('['); }
void CliJsonWriter::EndArray() { buf.push_back(']'); }
void CliJsonWriter::Comma(bool& first) {
  if (!first) {
    buf.push_back(',');
  }
  first = false;
}
void CliJsonWriter::Null() { buf.append("null"); }
void CliJsonWriter::Bool(bool value) { buf.append(value ? "true" : "false"); }
void CliJsonWriter::Int64(std::int64_t value) { buf.append(std::to_string(value)); }
void CliJsonWriter::Size(std::size_t value) { buf.append(std::to_string(value)); }
void CliJsonWriter::String(std::string_view text) {
  static constexpr char kHex[] = "0123456789abcdef";
  buf.push_back('"');
  for (const unsigned char uc : text) {
    switch (uc) {
      case '"': buf.append("\\\""); break;
      case '\\': buf.append("\\\\"); break;
      case '\b': buf.append("\\b"); break;
      case '\f': buf.append("\\f"); break;
      case '\n': buf.append("\\n"); break;
      case '\r': buf.append("\\r"); break;
      case '\t': buf.append("\\t"); break;
      default:
        if (uc < 0x20U) {
          buf.append("\\u00");
          buf.push_back(kHex[(uc >> 4) & 0x0FU]);
          buf.push_back(kHex[uc & 0x0FU]);
        } else {
          buf.push_back(static_cast<char>(uc));
        }
        break;
    }
  }
  buf.push_back('"');
}
void CliJsonWriter::Key(std::string_view key, bool& first) {
  Comma(first);
  String(key);
  buf.push_back(':');
}
void CliJsonWriter::KeyString(std::string_view key, std::string_view value, bool& first) {
  Key(key, first);
  String(value);
}
void CliJsonWriter::KeyBool(std::string_view key, bool value, bool& first) {
  Key(key, first);
  Bool(value);
}
void CliJsonWriter::KeyInt64(std::string_view key, std::int64_t value, bool& first) {
  Key(key, first);
  Int64(value);
}
void CliJsonWriter::KeySize(std::string_view key, std::size_t value, bool& first) {
  Key(key, first);
  Size(value);
}

std::string CliLowerAscii(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const unsigned char c : text) {
    out.push_back(static_cast<char>(std::tolower(c)));
  }
  return out;
}

std::string_view CliTrimAscii(std::string_view text) {
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

bool CliContainsIgnoreCase(std::string_view haystack, std::string_view needle) {
  if (needle.empty()) {
    return true;
  }
  return CliLowerAscii(haystack).find(CliLowerAscii(needle)) != std::string::npos;
}

bool CliEqualsIgnoreCase(std::string_view left, std::string_view right) {
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

std::optional<std::int64_t> CliParseStrictInt64(std::string_view text) {
  text = CliTrimAscii(text);
  if (text.empty()) {
    return std::nullopt;
  }
  std::int64_t value = 0;
  const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc{} || ptr != text.data() + text.size()) {
    return std::nullopt;
  }
  return value;
}

void CliSetEnvVar(const char* name, const char* value) {
#if defined(_WIN32)
  _putenv_s(name, value != nullptr ? value : "");
#else
  if (value != nullptr && *value != '\0') {
    setenv(name, value, 1);
  } else {
    unsetenv(name);
  }
#endif
}

bool CliIsKnownCommand(std::string_view cmd) {
  return cmd == "help" || cmd == "version" || cmd == "doctor" || cmd == "driver" ||
         cmd == "drivers" || cmd == "profile" || cmd == "profiles" || cmd == "tables" ||
         cmd == "objects" || cmd == "schema" || cmd == "describe" || cmd == "desc" ||
         cmd == "rows" || cmd == "query" || cmd == "exec" || cmd == "sql" ||
         cmd == "info" || cmd == "stats" || cmd == "import" || cmd == "export" ||
         cmd == "copy";
}

bool CliContainsHelpFlag(const std::vector<std::string>& args) {
  return std::ranges::any_of(args, [](const std::string& arg) {
    return arg == "-h" || arg == "--help";
  });
}

bool CliDetectJsonRequestedEarly(const std::vector<std::string>& args) {
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    if (a == "--") {
      break;
    }
    if (a == "--json" || a == "--format=json") {
      return true;
    }
    if (a == "--format" && i + 1 < args.size() && args[i + 1] == "json") {
      return true;
    }
  }
  return false;
}

std::string_view CliDialectName(Dialect dialect) {
  switch (dialect) {
    case Dialect::Sqlite: return "sqlite";
    case Dialect::Postgresql: return "postgresql";
    case Dialect::Mysql: return "mysql";
    case Dialect::Duckdb: return "duckdb";
    case Dialect::Redis: return "redis";
    case Dialect::Generic: return "generic";
  }
  return "generic";
}

int CliEmitError(std::ostream& err, bool json_mode, std::string_view message, int code) {
  if (json_mode) {
    CliJsonWriter w;
    w.BeginObject();
    bool first = true;
    w.KeyBool("ok", false, first);
    w.KeyString("error", message, first);
    w.EndObject();
    w.Raw("\n");
    err << w.buf;
  } else {
    err << "error: " << message << '\n';
  }
  return code;
}

bool ParseFormatString(std::string_view value, CliOutputFormat& out_format) {
  const std::string lower = CliLowerAscii(value);
  if (lower == "table") {
    out_format = CliOutputFormat::Table;
    return true;
  }
  if (lower == "tsv") {
    out_format = CliOutputFormat::Tsv;
    return true;
  }
  if (lower == "csv") {
    out_format = CliOutputFormat::Csv;
    return true;
  }
  if (lower == "json") {
    out_format = CliOutputFormat::Json;
    return true;
  }
  return false;
}

void CliSplitFlag(std::string_view arg, std::string_view& flag,
               std::optional<std::string_view>& inline_val) {
  inline_val.reset();
  if (arg.starts_with("--")) {
    const std::size_t eq = arg.find('=');
    if (eq != std::string_view::npos) {
      flag = arg.substr(0, eq);
      inline_val = arg.substr(eq + 1);
      return;
    }
  }
  flag = arg;
}

bool CliConsumeFlagValue(const std::vector<std::string>& args, std::size_t& index,
                      std::optional<std::string_view> inline_val, std::string_view flag_name,
                      std::string& value_out, std::string& error_out) {
  if (inline_val.has_value()) {
    value_out = std::string(*inline_val);
    return true;
  }
  if (index + 1 >= args.size()) {
    error_out = "参数 " + std::string(flag_name) + " 缺少值";
    return false;
  }
  value_out = args[++index];
  return true;
}

bool CliTryConsumeCommonFormatFlag(const std::vector<std::string>& args, std::size_t& index,
                                CliOutputFormat& format, bool& help_requested,
                                std::string& error_out, bool& handled) {
  handled = false;
  std::string_view flag;
  std::optional<std::string_view> inline_val;
  CliSplitFlag(args[index], flag, inline_val);

  if (flag == "-h" || flag == "--help") {
    if (inline_val.has_value()) {
      error_out = "参数 " + std::string(flag) + " 不接受值";
      return false;
    }
    help_requested = true;
    handled = true;
    return true;
  }
  if (flag == "--json") {
    if (inline_val.has_value()) {
      error_out = "参数 --json 不接受值";
      return false;
    }
    format = CliOutputFormat::Json;
    handled = true;
    return true;
  }
  if (flag == "--format" || flag == "-f") {
    std::string val;
    if (!CliConsumeFlagValue(args, index, inline_val, flag, val, error_out)) {
      return false;
    }
    if (!ParseFormatString(val, format)) {
      error_out = "无效的输出格式: " + val + " (可选: table, tsv, csv, json)";
      return false;
    }
    handled = true;
    return true;
  }
  return true;
}

bool CliTryConsumeConnFlag(const std::vector<std::string>& args, std::size_t& index,
                        CliConnOptions& conn, std::string& error_out, bool& handled) {
  handled = false;
  std::string_view flag;
  std::optional<std::string_view> inline_val;
  CliSplitFlag(args[index], flag, inline_val);

  std::string* target = nullptr;
  if (flag == "--profile" || flag == "-p") {
    target = &conn.profile_name;
  } else if (flag == "--profiles") {
    target = &conn.profiles_path;
  } else if (flag == "--sqlite") {
    conn.has_sqlite = true;
    target = &conn.sqlite_path;
  } else if (flag == "--redis") {
    conn.has_redis = true;
    target = &conn.redis_uri;
  } else if (flag == "--driver" || flag == "-d") {
    target = &conn.driver;
  } else if (flag == "--uri" || flag == "-u") {
    target = &conn.uri;
  } else if (flag == "--user") {
    conn.has_user = true;
    target = &conn.user;
  } else if (flag == "--password") {
    conn.has_password = true;
    target = &conn.password;
  } else if (flag == "--password-env") {
    conn.has_password_env = true;
    target = &conn.password_env;
  } else {
    return true;
  }

  handled = true;
  return CliConsumeFlagValue(args, index, inline_val, flag, *target, error_out);
}

std::string CliDefaultProfilesPath() {
  const std::string data_dir = DefaultAppDataDir();
  if (data_dir.empty()) {
    return "dbpp.sqlite3";
  }
  return (std::filesystem::path(data_dir) / "dbpp.sqlite3").string();
}

int CliResolveConnectionRequest(const CliConnOptions& conn, ConnectionRequest& request_out,
                             std::string& error_out) {
  if (conn.has_sqlite && conn.has_redis) {
    error_out = "--sqlite 与 --redis 不能同时使用";
    return kCliExitUsage;
  }
  if (conn.has_sqlite) {
    if (!conn.profile_name.empty()) {
      error_out = "--sqlite 与 --profile 不能同时使用";
      return kCliExitUsage;
    }
    if (!conn.uri.empty() || !conn.driver.empty()) {
      error_out = "--sqlite 不能与 --driver 或 --uri 同时使用";
      return kCliExitUsage;
    }
    if (conn.sqlite_path.empty()) {
      error_out = "--sqlite 路径不能为空";
      return kCliExitUsage;
    }
    request_out.driver_path = DefaultSqliteDriverPath();
    request_out.uri = conn.sqlite_path;
  } else if (conn.has_redis) {
    if (!conn.profile_name.empty()) {
      error_out = "--redis 与 --profile 不能同时使用";
      return kCliExitUsage;
    }
    if (!conn.uri.empty() || !conn.driver.empty()) {
      error_out = "--redis 不能与 --driver 或 --uri 同时使用";
      return kCliExitUsage;
    }
    if (conn.redis_uri.empty()) {
      error_out = "--redis URI 不能为空";
      return kCliExitUsage;
    }
    request_out.driver_path = "redis";
    request_out.uri = conn.redis_uri;
  } else if (!conn.profile_name.empty()) {
    const std::string path =
        conn.profiles_path.empty() ? CliDefaultProfilesPath() : conn.profiles_path;
    const std::vector<ConnectionProfile> profiles = LoadConnectionProfiles(path);
    const auto it = std::ranges::find_if(profiles, [&conn](const ConnectionProfile& p) {
      return p.name == conn.profile_name;
    });
    if (it == profiles.end()) {
      error_out = "未找到保存的连接配置 '" + conn.profile_name + "' (" + path + ")";
      return kCliExitError;
    }
    request_out.title = it->name;
    request_out.driver_path = conn.driver.empty() ? it->driver_path : conn.driver;
    request_out.uri = conn.uri.empty() ? it->uri : conn.uri;
    request_out.username = conn.has_user ? conn.user : it->username;
    request_out.password = it->password;
  } else {
    if (conn.uri.empty()) {
      error_out =
          "缺少连接目标: 请指定 --profile <name>、--sqlite <path>、--redis <uri> 或 --uri <uri>";
      return kCliExitUsage;
    }
    request_out.uri = conn.uri;
    request_out.driver_path = conn.driver.empty() ? InferDriverForUri(conn.uri) : conn.driver;
    request_out.username = conn.user;
  }

  if (conn.has_user) {
    request_out.username = conn.user;
  }

  if (conn.has_password_env) {
    const char* env_val = std::getenv(conn.password_env.c_str());
    if (env_val == nullptr || *env_val == '\0') {
      error_out = "环境变量 '" + conn.password_env + "' 未设置或为空";
      return kCliExitError;
    }
    request_out.password = env_val;
  } else if (conn.has_password) {
    request_out.password = conn.password;
  } else if (const char* fallback_pw = std::getenv("DBPP_PASSWORD");
             fallback_pw != nullptr && *fallback_pw != '\0') {
    request_out.password = fallback_pw;
  }

  return kCliExitOk;
}

std::string EscapeSingleLineCell(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    switch (c) {
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out.push_back(c); break;
    }
  }
  return out;
}

std::size_t Utf8DisplayWidth(std::string_view text) {
  std::size_t width = 0;
  std::size_t i = 0;
  while (i < text.size()) {
    const unsigned char b = static_cast<unsigned char>(text[i]);
    if (b < 0x80U) {
      if (b >= 0x20U && b != 0x7FU) {
        ++width;
      }
      ++i;
      continue;
    }
    std::uint32_t cp = 0;
    std::size_t len = 1;
    if ((b & 0xE0U) == 0xC0U && i + 1 < text.size()) {
      cp = b & 0x1FU;
      len = 2;
    } else if ((b & 0xF0U) == 0xE0U && i + 2 < text.size()) {
      cp = b & 0x0FU;
      len = 3;
    } else if ((b & 0xF8U) == 0xF0U && i + 3 < text.size()) {
      cp = b & 0x07U;
      len = 4;
    } else {
      ++width;
      ++i;
      continue;
    }
    for (std::size_t k = 1; k < len; ++k) {
      cp = (cp << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3FU);
    }
    i += len;
    const bool wide =
        (cp >= 0x1100U && cp <= 0x115FU) || (cp >= 0x2E80U && cp <= 0x9FFFU) ||
        (cp >= 0xAC00U && cp <= 0xD7AFU) || (cp >= 0xF900U && cp <= 0xFAFFU) ||
        (cp >= 0xFE10U && cp <= 0xFE6FU) || (cp >= 0xFF00U && cp <= 0xFF60U) ||
        (cp >= 0x20000U && cp <= 0x2FFFFU);
    width += wide ? 2 : 1;
  }
  return width;
}

void WritePadded(std::ostream& out, std::string_view text, std::size_t target_width) {
  out << text;
  const std::size_t w = Utf8DisplayWidth(text);
  for (std::size_t i = w; i < target_width; ++i) {
    out.put(' ');
  }
}

void WriteTableText(std::ostream& out, const std::vector<std::string>& columns,
                    const std::vector<GridRow>& rows) {
  if (columns.empty()) {
    return;
  }
  std::vector<std::size_t> widths(columns.size(), 0);
  std::vector<std::string> headers(columns.size());
  for (std::size_t c = 0; c < columns.size(); ++c) {
    headers[c] = EscapeSingleLineCell(columns[c]);
    widths[c] = std::max(widths[c], Utf8DisplayWidth(headers[c]));
  }

  std::vector<std::vector<std::string>> rendered(rows.size());
  for (std::size_t r = 0; r < rows.size(); ++r) {
    rendered[r].resize(columns.size());
    for (std::size_t c = 0; c < columns.size(); ++c) {
      if (c >= rows[r].values.size() || rows[r].values[c].is_null) {
        rendered[r][c] = "NULL";
      } else {
        rendered[r][c] = EscapeSingleLineCell(rows[r].values[c].text);
      }
      widths[c] = std::max(widths[c], Utf8DisplayWidth(rendered[r][c]));
    }
  }

  for (std::size_t c = 0; c < columns.size(); ++c) {
    if (c > 0) {
      out << " | ";
    }
    WritePadded(out, headers[c], widths[c]);
  }
  out << '\n';

  for (std::size_t c = 0; c < columns.size(); ++c) {
    if (c > 0) {
      out << "-+-";
    }
    for (std::size_t i = 0; i < widths[c]; ++i) {
      out.put('-');
    }
  }
  out << '\n';

  for (const auto& row : rendered) {
    for (std::size_t c = 0; c < columns.size(); ++c) {
      if (c > 0) {
        out << " | ";
      }
      WritePadded(out, row[c], widths[c]);
    }
    out << '\n';
  }
}

void WriteTsvRows(std::ostream& out, const std::vector<std::string>& columns,
                  const std::vector<GridRow>& rows) {
  if (columns.empty()) {
    return;
  }
  for (std::size_t c = 0; c < columns.size(); ++c) {
    if (c > 0) {
      out.put('\t');
    }
    out << EscapeSingleLineCell(columns[c]);
  }
  out.put('\n');
  for (const GridRow& row : rows) {
    for (std::size_t c = 0; c < columns.size(); ++c) {
      if (c > 0) {
        out.put('\t');
      }
      if (c >= row.values.size() || row.values[c].is_null) {
        out << "\\N";
      } else {
        out << EscapeSingleLineCell(row.values[c].text);
      }
    }
    out.put('\n');
  }
}

void WriteCsvField(std::ostream& out, std::string_view value) {
  const bool needs_quotes = value.find_first_of(",\"\n\r") != std::string_view::npos;
  if (!needs_quotes) {
    out << value;
    return;
  }
  out.put('"');
  for (const char c : value) {
    if (c == '"') {
      out.put('"');
    }
    out.put(c);
  }
  out.put('"');
}

void WriteCsvRows(std::ostream& out, const std::vector<std::string>& columns,
                  const std::vector<GridRow>& rows) {
  if (columns.empty()) {
    return;
  }
  for (std::size_t c = 0; c < columns.size(); ++c) {
    if (c > 0) {
      out.put(',');
    }
    WriteCsvField(out, columns[c]);
  }
  out.put('\n');
  for (const GridRow& row : rows) {
    for (std::size_t c = 0; c < columns.size(); ++c) {
      if (c > 0) {
        out.put(',');
      }
      if (c < row.values.size() && !row.values[c].is_null) {
        WriteCsvField(out, row.values[c].text);
      }
    }
    out.put('\n');
  }
}

void CliWriteTabularText(std::ostream& out, CliOutputFormat format,
                      const std::vector<std::string>& columns,
                      const std::vector<GridRow>& rows) {
  switch (format) {
    case CliOutputFormat::Table: WriteTableText(out, columns, rows); break;
    case CliOutputFormat::Tsv: WriteTsvRows(out, columns, rows); break;
    case CliOutputFormat::Csv: WriteCsvRows(out, columns, rows); break;
    case CliOutputFormat::Json: break;
  }
}

void CliAppendJsonRowsColumns(CliJsonWriter& w, const std::vector<std::string>& columns,
                           const std::vector<GridRow>& rows, bool& first_key) {
  w.Key("columns", first_key);
  w.BeginArray();
  bool first_col = true;
  for (const std::string& col : columns) {
    w.Comma(first_col);
    w.String(col);
  }
  w.EndArray();

  w.Key("rows", first_key);
  w.BeginArray();
  bool first_row = true;
  for (const GridRow& row : rows) {
    w.Comma(first_row);
    w.BeginArray();
    bool first_cell = true;
    for (std::size_t c = 0; c < columns.size(); ++c) {
      w.Comma(first_cell);
      if (c >= row.values.size() || row.values[c].is_null) {
        w.Null();
      } else {
        w.String(row.values[c].text);
      }
    }
    w.EndArray();
  }
  w.EndArray();
}

GridRow CliMakeTextRow(std::initializer_list<std::string_view> cells) {
  GridRow row;
  row.values.reserve(cells.size());
  for (std::string_view cell : cells) {
    row.values.push_back(
        CellValue{.text = std::string(cell), .is_null = false, .display_text = {}});
  }
  return row;
}

bool CliReadSqlInput(std::ostream& err, bool json_mode, std::istream& in,
                  const std::optional<std::string>& sql_arg,
                  const std::optional<std::string>& file_arg, std::string& sql_out,
                  int& exit_code_out) {
  if (sql_arg.has_value() && file_arg.has_value()) {
    exit_code_out = CliEmitError(err, json_mode, "不能同时指定 SQL 文本和 --file", kCliExitUsage);
    return false;
  }
  if (file_arg.has_value()) {
    if (*file_arg == "-") {
      std::ostringstream oss;
      oss << in.rdbuf();
      sql_out = oss.str();
    } else {
      std::ifstream file(*file_arg, std::ios::binary);
      if (!file) {
        exit_code_out = CliEmitError(err, json_mode, "无法读取 SQL 文件: " + *file_arg,
                                  kCliExitError);
        return false;
      }
      std::ostringstream oss;
      oss << file.rdbuf();
      sql_out = oss.str();
    }
  } else if (sql_arg.has_value()) {
    if (*sql_arg == "-") {
      std::ostringstream oss;
      oss << in.rdbuf();
      sql_out = oss.str();
    } else {
      sql_out = *sql_arg;
    }
  } else {
    exit_code_out = CliEmitError(
        err, json_mode, "缺少 SQL 语句: 请传入 <sql>、--sql <sql>、--file <path> 或 -",
        kCliExitUsage);
    return false;
  }

  sql_out = std::string(CliTrimAscii(sql_out));
  if (sql_out.empty()) {
    exit_code_out = CliEmitError(err, json_mode, "SQL 语句不能为空", kCliExitUsage);
    return false;
  }
  return true;
}

std::vector<std::string> CliSplitSqlStatements(std::string_view script, Dialect dialect) {
  return SplitSqlStatements(script, dialect);
}

std::optional<DbObject> CliFindObjectByName(const std::vector<DbObject>& objects,
                                         std::string_view target,
                                         std::string_view schema_filter,
                                         std::string& error_out) {
  std::string_view explicit_schema;
  std::string_view explicit_name = target;
  if (const std::size_t dot = target.find('.'); dot != std::string_view::npos) {
    explicit_schema = target.substr(0, dot);
    explicit_name = target.substr(dot + 1);
  }

  std::vector<const DbObject*> exact_matches;
  std::vector<const DbObject*> ci_matches;
  for (const DbObject& obj : objects) {
    if (!schema_filter.empty() && !CliEqualsIgnoreCase(obj.db_schema, schema_filter)) {
      continue;
    }
    if (!explicit_schema.empty()) {
      if (obj.db_schema == explicit_schema && obj.name == explicit_name) {
        exact_matches.push_back(&obj);
      } else if (CliEqualsIgnoreCase(obj.db_schema, explicit_schema) &&
                 CliEqualsIgnoreCase(obj.name, explicit_name)) {
        ci_matches.push_back(&obj);
      }
    } else {
      if (obj.name == explicit_name) {
        exact_matches.push_back(&obj);
      } else if (CliEqualsIgnoreCase(obj.name, explicit_name)) {
        ci_matches.push_back(&obj);
      }
    }
  }

  const auto& matches = !exact_matches.empty() ? exact_matches : ci_matches;
  if (matches.empty()) {
    error_out = "未找到表或视图: " + std::string(target);
    return std::nullopt;
  }
  if (matches.size() == 1) {
    return *matches.front();
  }
  for (const DbObject* obj : matches) {
    if (obj->db_schema.empty() || obj->db_schema == "main" || obj->db_schema == "public") {
      return *obj;
    }
  }
  error_out = "存在多个同名对象 '" + std::string(target) + "'，请通过 --schema 指定 schema";
  return std::nullopt;
}

} // namespace detail

} // namespace dbpp
