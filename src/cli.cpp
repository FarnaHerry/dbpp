#include "cli.h"

#include "adbc_session.h"
#include "driver_installer.h"
#include "model.h"
#include "redis_client.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#ifndef DBPP_VERSION
#define DBPP_VERSION "0.2.0"
#endif

namespace dbpp {

namespace {

constexpr std::int64_t kDefaultRowsLimit = 50;
constexpr std::int64_t kMaxRowsLimit = 100000;
constexpr std::size_t kDefaultQueryMaxRows = 500;
constexpr std::size_t kMaxQueryMaxRows = 100000;

enum class OutputFormat {
  Table,
  Tsv,
  Csv,
  Json,
};

enum class ObjectTypeFilter {
  All,
  Table,
  View,
};

struct ConnOptions {
  std::string profile_name;
  std::string profiles_path;
  std::string sqlite_path;
  std::string redis_uri;
  std::string driver;
  std::string uri;
  std::string user;
  std::string password;
  std::string password_env;
  bool has_sqlite = false;
  bool has_redis = false;
  bool has_user = false;
  bool has_password = false;
  bool has_password_env = false;
};

struct JsonWriter {
  std::string buf;

  void Raw(std::string_view text) { buf.append(text); }
  void BeginObject() { buf.push_back('{'); }
  void EndObject() { buf.push_back('}'); }
  void BeginArray() { buf.push_back('['); }
  void EndArray() { buf.push_back(']'); }
  void Comma(bool& first) {
    if (!first) {
      buf.push_back(',');
    }
    first = false;
  }

  void Null() { buf.append("null"); }
  void Bool(bool value) { buf.append(value ? "true" : "false"); }
  void Int64(std::int64_t value) { buf.append(std::to_string(value)); }
  void Size(std::size_t value) { buf.append(std::to_string(value)); }

  void String(std::string_view text) {
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

  void Key(std::string_view key, bool& first) {
    Comma(first);
    String(key);
    buf.push_back(':');
  }

  void KeyString(std::string_view key, std::string_view value, bool& first) {
    Key(key, first);
    String(value);
  }

  void KeyBool(std::string_view key, bool value, bool& first) {
    Key(key, first);
    Bool(value);
  }

  void KeyInt64(std::string_view key, std::int64_t value, bool& first) {
    Key(key, first);
    Int64(value);
  }

  void KeySize(std::string_view key, std::size_t value, bool& first) {
    Key(key, first);
    Size(value);
  }
};

std::string LowerAscii(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const unsigned char c : text) {
    out.push_back(static_cast<char>(std::tolower(c)));
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

bool ContainsIgnoreCase(std::string_view haystack, std::string_view needle) {
  if (needle.empty()) {
    return true;
  }
  return LowerAscii(haystack).find(LowerAscii(needle)) != std::string::npos;
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
  std::int64_t value = 0;
  const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc{} || ptr != text.data() + text.size()) {
    return std::nullopt;
  }
  return value;
}

void SetEnvVar(const char* name, const char* value) {
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

bool IsKnownCliCommand(std::string_view cmd) {
  return cmd == "help" || cmd == "version" || cmd == "doctor" || cmd == "driver" ||
         cmd == "drivers" || cmd == "profile" || cmd == "profiles" || cmd == "tables" ||
         cmd == "objects" || cmd == "schema" || cmd == "describe" || cmd == "desc" ||
         cmd == "rows" || cmd == "query" || cmd == "exec" || cmd == "sql";
}

bool ContainsHelpFlag(const std::vector<std::string>& args) {
  return std::ranges::any_of(args, [](const std::string& arg) {
    return arg == "-h" || arg == "--help";
  });
}

bool DetectJsonRequestedEarly(const std::vector<std::string>& args) {
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

std::string_view DialectName(Dialect dialect) {
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

int EmitError(std::ostream& err, bool json_mode, std::string_view message, int code) {
  if (json_mode) {
    JsonWriter w;
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

bool ParseFormatString(std::string_view value, OutputFormat& out_format) {
  const std::string lower = LowerAscii(value);
  if (lower == "table") {
    out_format = OutputFormat::Table;
    return true;
  }
  if (lower == "tsv") {
    out_format = OutputFormat::Tsv;
    return true;
  }
  if (lower == "csv") {
    out_format = OutputFormat::Csv;
    return true;
  }
  if (lower == "json") {
    out_format = OutputFormat::Json;
    return true;
  }
  return false;
}

void SplitFlag(std::string_view arg, std::string_view& flag,
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

bool ConsumeFlagValue(const std::vector<std::string>& args, std::size_t& index,
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

bool TryConsumeCommonFormatFlag(const std::vector<std::string>& args, std::size_t& index,
                                OutputFormat& format, bool& help_requested,
                                std::string& error_out, bool& handled) {
  handled = false;
  std::string_view flag;
  std::optional<std::string_view> inline_val;
  SplitFlag(args[index], flag, inline_val);

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
    format = OutputFormat::Json;
    handled = true;
    return true;
  }
  if (flag == "--format" || flag == "-f") {
    std::string val;
    if (!ConsumeFlagValue(args, index, inline_val, flag, val, error_out)) {
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

bool TryConsumeConnFlag(const std::vector<std::string>& args, std::size_t& index,
                        ConnOptions& conn, std::string& error_out, bool& handled) {
  handled = false;
  std::string_view flag;
  std::optional<std::string_view> inline_val;
  SplitFlag(args[index], flag, inline_val);

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
  return ConsumeFlagValue(args, index, inline_val, flag, *target, error_out);
}

std::string DefaultProfilesPath() {
  const std::string data_dir = DefaultAppDataDir();
  if (data_dir.empty()) {
    return "connections.tsv";
  }
  return (std::filesystem::path(data_dir) / "connections.tsv").string();
}

int ResolveConnectionRequest(const ConnOptions& conn, ConnectionRequest& request_out,
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
        conn.profiles_path.empty() ? DefaultProfilesPath() : conn.profiles_path;
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

void WriteTabularText(std::ostream& out, OutputFormat format,
                      const std::vector<std::string>& columns,
                      const std::vector<GridRow>& rows) {
  switch (format) {
    case OutputFormat::Table: WriteTableText(out, columns, rows); break;
    case OutputFormat::Tsv: WriteTsvRows(out, columns, rows); break;
    case OutputFormat::Csv: WriteCsvRows(out, columns, rows); break;
    case OutputFormat::Json: break;
  }
}

void AppendJsonRowsColumns(JsonWriter& w, const std::vector<std::string>& columns,
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

GridRow MakeTextRow(std::initializer_list<std::string_view> cells) {
  GridRow row;
  row.values.reserve(cells.size());
  for (std::string_view cell : cells) {
    row.values.push_back(CellValue{.text = std::string(cell), .is_null = false});
  }
  return row;
}

bool ReadSqlInput(std::ostream& err, bool json_mode, std::istream& in,
                  const std::optional<std::string>& sql_arg,
                  const std::optional<std::string>& file_arg, std::string& sql_out,
                  int& exit_code_out) {
  if (sql_arg.has_value() && file_arg.has_value()) {
    exit_code_out = EmitError(err, json_mode, "不能同时指定 SQL 文本和 --file", kCliExitUsage);
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
        exit_code_out = EmitError(err, json_mode, "无法读取 SQL 文件: " + *file_arg,
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
    exit_code_out = EmitError(
        err, json_mode, "缺少 SQL 语句: 请传入 <sql>、--sql <sql>、--file <path> 或 -",
        kCliExitUsage);
    return false;
  }

  sql_out = std::string(TrimAscii(sql_out));
  if (sql_out.empty()) {
    exit_code_out = EmitError(err, json_mode, "SQL 语句不能为空", kCliExitUsage);
    return false;
  }
  return true;
}

std::vector<std::string> SplitSqlStatements(std::string_view script, Dialect dialect) {
  std::vector<std::string> statements;
  std::size_t stmt_start = 0;
  std::size_t i = 0;

  while (i < script.size()) {
    const char c = script[i];
    if (c == '-' && i + 1 < script.size() && script[i + 1] == '-') {
      i += 2;
      while (i < script.size() && script[i] != '\n') {
        ++i;
      }
      continue;
    }
    if (c == '/' && i + 1 < script.size() && script[i + 1] == '*') {
      i += 2;
      while (i + 1 < script.size() && !(script[i] == '*' && script[i + 1] == '/')) {
        ++i;
      }
      i = (i + 1 < script.size()) ? i + 2 : script.size();
      continue;
    }
    if (c == '\'') {
      ++i;
      while (i < script.size()) {
        if (dialect == Dialect::Mysql && script[i] == '\\' && i + 1 < script.size()) {
          i += 2;
          continue;
        }
        if (script[i] == '\'') {
          if (i + 1 < script.size() && script[i + 1] == '\'') {
            i += 2;
            continue;
          }
          ++i;
          break;
        }
        ++i;
      }
      continue;
    }
    if (c == '"' || c == '`') {
      const char quote = c;
      ++i;
      while (i < script.size()) {
        if (script[i] == quote) {
          if (i + 1 < script.size() && script[i + 1] == quote) {
            i += 2;
            continue;
          }
          ++i;
          break;
        }
        ++i;
      }
      continue;
    }
    if (c == '$') {
      std::size_t j = i + 1;
      while (j < script.size() &&
             (std::isalnum(static_cast<unsigned char>(script[j])) || script[j] == '_')) {
        ++j;
      }
      if (j < script.size() && script[j] == '$') {
        const std::string_view tag = script.substr(i, j - i + 1);
        const std::size_t close = script.find(tag, j + 1);
        i = close == std::string_view::npos ? script.size() : close + tag.size();
        continue;
      }
    }
    if (c == ';') {
      const std::string_view piece = TrimAscii(script.substr(stmt_start, i - stmt_start));
      if (!piece.empty()) {
        statements.emplace_back(piece);
      }
      stmt_start = i + 1;
    }
    ++i;
  }

  const std::string_view tail = TrimAscii(script.substr(stmt_start));
  if (!tail.empty()) {
    statements.emplace_back(tail);
  }
  return statements;
}

std::optional<DbObject> FindObjectByName(const std::vector<DbObject>& objects,
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
    if (!schema_filter.empty() && !EqualsIgnoreCase(obj.db_schema, schema_filter)) {
      continue;
    }
    if (!explicit_schema.empty()) {
      if (obj.db_schema == explicit_schema && obj.name == explicit_name) {
        exact_matches.push_back(&obj);
      } else if (EqualsIgnoreCase(obj.db_schema, explicit_schema) &&
                 EqualsIgnoreCase(obj.name, explicit_name)) {
        ci_matches.push_back(&obj);
      }
    } else {
      if (obj.name == explicit_name) {
        exact_matches.push_back(&obj);
      } else if (EqualsIgnoreCase(obj.name, explicit_name)) {
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

// ---------------------------------------------------------------------------
// Help output
// ---------------------------------------------------------------------------

void PrintTopLevelHelp(std::ostream& out) {
  out << "db++ " << DBPP_VERSION << " - SQLite-first ADBC database manager & CLI\n"
      << "\n"
      << "Usage:\n"
      << "  dbpp                         Launch desktop GUI\n"
      << "  dbpp <file.sqlite>           Open file in desktop GUI (.desktop compatible)\n"
      << "  dbpp gui|open [uri]          Launch GUI (optional --driver <drv>)\n"
      << "  dbpp <command> [options]     Run headless CLI command (no display required)\n"
      << "\n"
      << "Commands:\n"
      << "  doctor                       Show local environment, paths, and drivers\n"
      << "  driver <list|catalog|install|uninstall>\n"
      << "                               Manage ADBC drivers\n"
      << "  profile <list|show|save|remove>\n"
      << "                               Manage saved connection profiles (connections.tsv)\n"
      << "  tables   <conn>              List tables and views\n"
      << "  schema   <conn> [table]      Inspect columns and primary keys\n"
      << "  rows     <conn> <table>      Paginated table rows (--limit/--offset/--where/--order)\n"
      << "  query    <conn> <sql|-|--file <path>>\n"
      << "                               Execute SQL query and return result rows\n"
      << "  exec     <conn> <sql|-|--file <path>> [--script]\n"
      << "                               Execute DDL/DML statement(s) and return affected rows\n"
      << "  sql      <conn> <sql|-|--file <path>>\n"
      << "                               Auto-detect query vs exec from SQL statement\n"
      << "  version                      Print version\n"
      << "  help [command]               Show help for top-level or subcommand\n"
      << "\n"
      << "Connection options (<conn>):\n"
      << "  --profile, -p <name>         Use saved profile from connections.tsv\n"
      << "  --sqlite <path>              Open local SQLite file (or :memory:)\n"
      << "  --redis <uri>                Connect to Redis (redis://[user:pass@]host[:port][/db])\n"
      << "  --uri, -u <uri>              Connection URI (or libpq conninfo)\n"
      << "  --driver, -d <slug|path>     ADBC driver slug, .toml path, or shared lib path\n"
      << "  --user <name>                Override username\n"
      << "  --password <pw>              Provide password directly\n"
      << "  --password-env <VAR>         Read password from environment variable (fallback: DBPP_PASSWORD)\n"
      << "  --profiles <path>            Override connections.tsv path (env DBPP_DATA_DIR)\n"
      << "\n"
      << "Output & exit codes:\n"
      << "  --json                       Shorthand for --format json ({\"ok\":true,...})\n"
      << "  --format, -f <table|tsv|csv|json>\n"
      << "  Exit codes: 0 = ok, 1 = runtime/DB error, 2 = usage error\n"
      << "\n"
      << "Examples:\n"
      << "  dbpp doctor --json\n"
      << "  dbpp tables --sqlite ./app.sqlite --json\n"
      << "  dbpp rows   --redis redis://127.0.0.1:6379/0 db0 --where \"user:*\" --json\n"
      << "  dbpp query  --redis redis://127.0.0.1:6379/0 \"INFO server\" --json\n"
      << "  dbpp query  --sqlite ./app.sqlite \"SELECT * FROM items LIMIT 10\" --json\n"
      << "  dbpp profile save pg --driver postgresql --uri postgresql://127.0.0.1:5432/app --user dev\n"
      << "  PGPASS=secret dbpp tables -p pg --password-env PGPASS --json\n";
}

bool PrintCommandHelp(std::string_view cmd, std::ostream& out) {
  if (cmd == "doctor") {
    out << "Usage: dbpp doctor [--profiles <path>] [--json]\n"
        << "Inspects local db++ data directory, bundled SQLite driver, user ADBC drivers dir,\n"
        << "saved profile count, and all discovered ADBC drivers.\n";
    return true;
  }
  if (cmd == "version") {
    out << "Usage: dbpp version [--json]\n";
    return true;
  }
  if (cmd == "driver" || cmd == "drivers") {
    out << "Usage:\n"
        << "  dbpp driver list [--format table|tsv|csv|json] [--json]\n"
        << "  dbpp driver catalog [--filter <kw>] [--registry <url>] [--drivers-dir <dir>] [--format ...] [--json]\n"
        << "  dbpp driver install <slug> [--registry <url>] [--drivers-dir <dir>] [--json]\n"
        << "  dbpp driver uninstall <slug> [--drivers-dir <dir>] [--json]\n";
    return true;
  }
  if (cmd == "profile" || cmd == "profiles") {
    out << "Usage:\n"
        << "  dbpp profile list [--profiles <path>] [--format table|tsv|csv|json] [--json]\n"
        << "  dbpp profile show <name> [--profiles <path>] [--json]\n"
        << "  dbpp profile save <name> --driver <drv> --uri <uri> [--user <u>] [--profiles <path>] [--json]\n"
        << "  dbpp profile remove <name> [--profiles <path>] [--json]\n"
        << "Note: Passwords are never saved to connections.tsv; pass --password-env <VAR> at query time.\n";
    return true;
  }
  if (cmd == "tables" || cmd == "objects") {
    out << "Usage: dbpp tables <conn> [--schema <name>] [--type all|table|view] [--format ...] [--json]\n";
    return true;
  }
  if (cmd == "schema" || cmd == "describe" || cmd == "desc") {
    out << "Usage: dbpp schema <conn> [table] [--schema <name>] [--format ...] [--json]\n";
    return true;
  }
  if (cmd == "rows") {
    out << "Usage: dbpp rows <conn> <table> [--schema <name>] [--limit N] [--offset N]\n"
        << "                 [--where <expr>] [--order <col[:asc|desc]>] [--format ...] [--json]\n"
        << "Defaults: --limit 50, --offset 0.\n";
    return true;
  }
  if (cmd == "query") {
    out << "Usage: dbpp query <conn> [<sql> | --sql <sql> | --file <path> | -]\n"
        << "                  [--max-rows N] [--read-only] [--format ...] [--json]\n"
        << "Defaults: --max-rows 500.\n";
    return true;
  }
  if (cmd == "exec") {
    out << "Usage: dbpp exec <conn> [<sql> | --sql <sql> | --file <path> | -] [--script] [--json]\n"
        << "Use --script to split and execute multiple semicolon-delimited statements.\n";
    return true;
  }
  if (cmd == "sql") {
    out << "Usage: dbpp sql <conn> [<sql> | --sql <sql> | --file <path> | -] [--max-rows N] [--format ...] [--json]\n"
        << "Automatically dispatches read queries vs DDL/DML statements.\n";
    return true;
  }
  if (cmd == "gui" || cmd == "open") {
    out << "Usage: dbpp " << cmd << " [<uri>] [--driver <drv>]\n"
        << "Launches the desktop GUI optionally opening <uri> with <drv>.\n";
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Command handlers
// ---------------------------------------------------------------------------

int HandleVersion(const std::vector<std::string>& args, std::ostream& out, std::ostream& err,
                  bool early_json) {
  OutputFormat format = OutputFormat::Table;
  bool help = false;
  std::string error;

  for (std::size_t i = 1; i < args.size(); ++i) {
    bool handled = false;
    if (!TryConsumeCommonFormatFlag(args, i, format, help, error, handled)) {
      return EmitError(err, early_json, error, kCliExitUsage);
    }
    if (handled) {
      continue;
    }
    return EmitError(err, early_json, "未知参数: " + args[i], kCliExitUsage);
  }
  if (help) {
    PrintCommandHelp("version", out);
    return kCliExitOk;
  }
  if (format == OutputFormat::Json) {
    JsonWriter w;
    w.BeginObject();
    bool first = true;
    w.KeyBool("ok", true, first);
    w.KeyString("name", "dbpp", first);
    w.KeyString("version", DBPP_VERSION, first);
    w.KeyString("platform", CurrentPlatformTuple(), first);
    w.EndObject();
    w.Raw("\n");
    out << w.buf;
    return kCliExitOk;
  }
  out << "dbpp " << DBPP_VERSION << '\n';
  return kCliExitOk;
}

int HandleDoctor(const std::vector<std::string>& args, std::ostream& out, std::ostream& err,
                 bool early_json) {
  OutputFormat format = OutputFormat::Table;
  bool help = false;
  std::string profiles_override;
  std::string error;

  for (std::size_t i = 1; i < args.size(); ++i) {
    bool handled = false;
    if (!TryConsumeCommonFormatFlag(args, i, format, help, error, handled)) {
      return EmitError(err, early_json, error, kCliExitUsage);
    }
    if (handled) {
      continue;
    }
    std::string_view flag;
    std::optional<std::string_view> inline_val;
    SplitFlag(args[i], flag, inline_val);
    if (flag == "--profiles") {
      if (!ConsumeFlagValue(args, i, inline_val, flag, profiles_override, error)) {
        return EmitError(err, early_json, error, kCliExitUsage);
      }
      continue;
    }
    return EmitError(err, early_json, "未知参数: " + args[i], kCliExitUsage);
  }
  if (help) {
    PrintCommandHelp("doctor", out);
    return kCliExitOk;
  }

  const std::string data_dir = DefaultAppDataDir();
  const std::string profiles_path =
      profiles_override.empty() ? DefaultProfilesPath() : profiles_override;
  std::error_code ec;
  const bool profiles_exists = std::filesystem::is_regular_file(profiles_path, ec);
  const std::vector<ConnectionProfile> profiles = LoadConnectionProfiles(profiles_path);

  const std::string sqlite_driver = DefaultSqliteDriverPath();
  ec.clear();
  const bool sqlite_exists = std::filesystem::is_regular_file(sqlite_driver, ec);
  const std::string user_drivers_dir = UserAdbcDriversDir();
  const std::string registry_url = AdbcRegistryBaseUrl();
  const std::vector<AdbcDriverInfo> discovered = DiscoverAdbcDrivers();

  if (format == OutputFormat::Json) {
    JsonWriter w;
    w.BeginObject();
    bool first = true;
    w.KeyBool("ok", true, first);
    w.KeyString("version", DBPP_VERSION, first);
    w.KeyString("platform", CurrentPlatformTuple(), first);
    w.KeyString("data_dir", data_dir, first);
    w.KeyString("profiles_path", profiles_path, first);
    w.KeyBool("profiles_exists", profiles_exists, first);
    w.KeySize("profiles_count", profiles.size(), first);
    w.KeyString("sqlite_driver", sqlite_driver, first);
    w.KeyBool("sqlite_driver_exists", sqlite_exists, first);
    w.KeyString("user_drivers_dir", user_drivers_dir, first);
    w.KeyString("registry_url", registry_url, first);
    w.Key("discovered_drivers", first);
    w.BeginArray();
    bool first_drv = true;
    for (const AdbcDriverInfo& drv : discovered) {
      w.Comma(first_drv);
      w.BeginObject();
      bool fd = true;
      w.KeyString("name", drv.name, fd);
      w.KeyString("path", drv.path, fd);
      w.KeyString("version", drv.version, fd);
      w.KeyBool("is_manifest", drv.is_manifest, fd);
      w.KeyString("detail", drv.detail, fd);
      w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    w.Raw("\n");
    out << w.buf;
    return kCliExitOk;
  }

  std::vector<GridRow> rows = {
      MakeTextRow({"version", DBPP_VERSION}),
      MakeTextRow({"platform", CurrentPlatformTuple()}),
      MakeTextRow({"data_dir", data_dir}),
      MakeTextRow({"profiles_path", profiles_path}),
      MakeTextRow({"profiles_count", std::to_string(profiles.size())}),
      MakeTextRow({"sqlite_driver", sqlite_driver}),
      MakeTextRow({"sqlite_driver_exists", sqlite_exists ? "true" : "false"}),
      MakeTextRow({"user_drivers_dir", user_drivers_dir}),
      MakeTextRow({"registry_url", registry_url}),
      MakeTextRow({"discovered_drivers", std::to_string(discovered.size())}),
  };
  WriteTabularText(out, format, {"key", "value"}, rows);
  return kCliExitOk;
}

int HandleDriver(const std::vector<std::string>& args, std::ostream& out, std::ostream& err,
                 bool early_json) {
  if (args.size() < 2 || args[1] == "-h" || args[1] == "--help") {
    PrintCommandHelp("driver", out);
    return kCliExitOk;
  }

  const std::string& sub = args[1];
  OutputFormat format = OutputFormat::Table;
  bool help = false;
  std::string filter;
  std::string registry_url;
  std::string drivers_dir;
  std::optional<std::string> slug_arg;
  std::string error;

  for (std::size_t i = 2; i < args.size(); ++i) {
    bool handled = false;
    if (!TryConsumeCommonFormatFlag(args, i, format, help, error, handled)) {
      return EmitError(err, early_json, error, kCliExitUsage);
    }
    if (handled) {
      continue;
    }
    std::string_view flag;
    std::optional<std::string_view> inline_val;
    SplitFlag(args[i], flag, inline_val);

    if (flag == "--filter" && sub == "catalog") {
      if (!ConsumeFlagValue(args, i, inline_val, flag, filter, error)) {
        return EmitError(err, early_json, error, kCliExitUsage);
      }
      continue;
    }
    if (flag == "--registry" && (sub == "catalog" || sub == "install")) {
      if (!ConsumeFlagValue(args, i, inline_val, flag, registry_url, error)) {
        return EmitError(err, early_json, error, kCliExitUsage);
      }
      continue;
    }
    if (flag == "--drivers-dir" &&
        (sub == "catalog" || sub == "install" || sub == "uninstall" || sub == "list")) {
      if (!ConsumeFlagValue(args, i, inline_val, flag, drivers_dir, error)) {
        return EmitError(err, early_json, error, kCliExitUsage);
      }
      continue;
    }
    if (!args[i].empty() && args[i].front() == '-') {
      return EmitError(err, early_json, "未知参数: " + args[i], kCliExitUsage);
    }
    if ((sub == "install" || sub == "uninstall") && !slug_arg.has_value()) {
      slug_arg = args[i];
      continue;
    }
    return EmitError(err, early_json, "多余的位置参数: " + args[i], kCliExitUsage);
  }

  if (help) {
    PrintCommandHelp("driver", out);
    return kCliExitOk;
  }

  const bool json_mode = (format == OutputFormat::Json);
  const std::filesystem::path drivers_dir_path(drivers_dir);

  if (sub == "list") {
    std::optional<std::string> prev_env;
    bool restore_env = false;
    if (!drivers_dir.empty()) {
      if (const char* cur = std::getenv("DBPP_ADBC_DRIVERS")) {
        prev_env = cur;
      }
      restore_env = true;
      SetEnvVar("DBPP_ADBC_DRIVERS", drivers_dir.c_str());
    }
    const std::vector<AdbcDriverInfo> discovered = DiscoverAdbcDrivers();
    if (restore_env) {
      SetEnvVar("DBPP_ADBC_DRIVERS", prev_env ? prev_env->c_str() : nullptr);
    }

    if (json_mode) {
      JsonWriter w;
      w.BeginObject();
      bool first = true;
      w.KeyBool("ok", true, first);
      w.KeySize("count", discovered.size(), first);
      w.Key("drivers", first);
      w.BeginArray();
      bool first_item = true;
      for (const AdbcDriverInfo& drv : discovered) {
        w.Comma(first_item);
        w.BeginObject();
        bool fi = true;
        w.KeyString("name", drv.name, fi);
        w.KeyString("path", drv.path, fi);
        w.KeyString("version", drv.version, fi);
        w.KeyBool("is_manifest", drv.is_manifest, fi);
        w.KeyString("detail", drv.detail, fi);
        w.EndObject();
      }
      w.EndArray();
      w.EndObject();
      w.Raw("\n");
      out << w.buf;
      return kCliExitOk;
    }

    std::vector<GridRow> rows;
    rows.reserve(discovered.size());
    for (const AdbcDriverInfo& drv : discovered) {
      rows.push_back(MakeTextRow(
          {drv.name, drv.path, drv.version, drv.is_manifest ? "manifest" : "library", drv.detail}));
    }
    WriteTabularText(out, format, {"name", "path", "version", "kind", "detail"}, rows);
    return kCliExitOk;
  }

  if (sub == "catalog") {
    const RegistryCatalogResult catalog = FetchRegistryCatalogBlocking(registry_url);
    if (!catalog.error.empty()) {
      return EmitError(err, json_mode, catalog.error, kCliExitError);
    }

    std::vector<RegistryDriverInfo> filtered;
    for (const RegistryDriverInfo& drv : catalog.drivers) {
      if (filter.empty() || ContainsIgnoreCase(drv.slug, filter) ||
          ContainsIgnoreCase(drv.name, filter) ||
          ContainsIgnoreCase(drv.description, filter)) {
        filtered.push_back(drv);
      }
    }

    if (json_mode) {
      JsonWriter w;
      w.BeginObject();
      bool first = true;
      w.KeyBool("ok", true, first);
      w.KeyString("platform", CurrentPlatformTuple(), first);
      w.KeySize("count", filtered.size(), first);
      w.Key("drivers", first);
      w.BeginArray();
      bool first_item = true;
      for (const RegistryDriverInfo& drv : filtered) {
        const bool installed = IsUserInstalledAdbcDriver(drv.slug, drivers_dir_path);
        w.Comma(first_item);
        w.BeginObject();
        bool fi = true;
        w.KeyString("slug", drv.slug, fi);
        w.KeyString("name", drv.name, fi);
        w.KeyString("version", drv.version, fi);
        w.KeyString("license", drv.license, fi);
        w.KeyBool("installed", installed, fi);
        w.KeyString("description", drv.description, fi);
        w.KeyString("package_url", drv.package_url, fi);
        w.KeyString("sha256", drv.sha256, fi);
        w.EndObject();
      }
      w.EndArray();
      w.EndObject();
      w.Raw("\n");
      out << w.buf;
      return kCliExitOk;
    }

    std::vector<GridRow> rows;
    rows.reserve(filtered.size());
    for (const RegistryDriverInfo& drv : filtered) {
      const bool installed = IsUserInstalledAdbcDriver(drv.slug, drivers_dir_path);
      rows.push_back(MakeTextRow(
          {drv.slug, drv.name, drv.version, installed ? "yes" : "no", drv.license, drv.description}));
    }
    WriteTabularText(out, format, {"slug", "name", "version", "installed", "license", "description"},
                     rows);
    return kCliExitOk;
  }

  if (sub == "install") {
    if (!slug_arg.has_value() || slug_arg->empty()) {
      return EmitError(err, json_mode, "缺少驱动 slug: dbpp driver install <slug>",
                       kCliExitUsage);
    }
    const RegistryCatalogResult catalog = FetchRegistryCatalogBlocking(registry_url);
    if (!catalog.error.empty()) {
      return EmitError(err, json_mode, catalog.error, kCliExitError);
    }
    const RegistryDriverInfo* matched = nullptr;
    for (const RegistryDriverInfo& drv : catalog.drivers) {
      if (drv.slug == *slug_arg) {
        matched = &drv;
        break;
      }
    }
    if (matched == nullptr) {
      for (const RegistryDriverInfo& drv : catalog.drivers) {
        if (EqualsIgnoreCase(drv.slug, *slug_arg)) {
          matched = &drv;
          break;
        }
      }
    }
    if (matched == nullptr) {
      return EmitError(err, json_mode,
                       "当前平台 (" + CurrentPlatformTuple() + ") 未找到驱动: " + *slug_arg,
                       kCliExitError);
    }

    const DriverInstallResult res = InstallRegistryDriverBlocking(*matched, drivers_dir_path);
    if (!res.ok) {
      return EmitError(err, json_mode, res.message, kCliExitError);
    }
    if (json_mode) {
      JsonWriter w;
      w.BeginObject();
      bool first = true;
      w.KeyBool("ok", true, first);
      w.KeyString("slug", res.slug, first);
      w.KeyString("version", res.version, first);
      w.KeyString("manifest_path", res.manifest_path, first);
      w.KeyString("message", res.message, first);
      w.EndObject();
      w.Raw("\n");
      out << w.buf;
      return kCliExitOk;
    }
    out << res.message << '\n' << "manifest: " << res.manifest_path << '\n';
    return kCliExitOk;
  }

  if (sub == "uninstall") {
    if (!slug_arg.has_value() || slug_arg->empty()) {
      return EmitError(err, json_mode, "缺少驱动 slug: dbpp driver uninstall <slug>",
                       kCliExitUsage);
    }
    const DriverInstallResult res =
        UninstallUserAdbcDriverBlocking(*slug_arg, drivers_dir_path);
    if (!res.ok) {
      return EmitError(err, json_mode, res.message, kCliExitError);
    }
    if (json_mode) {
      JsonWriter w;
      w.BeginObject();
      bool first = true;
      w.KeyBool("ok", true, first);
      w.KeyString("slug", res.slug, first);
      w.KeyString("manifest_path", res.manifest_path, first);
      w.KeyString("message", res.message, first);
      w.EndObject();
      w.Raw("\n");
      out << w.buf;
      return kCliExitOk;
    }
    out << res.message << '\n';
    return kCliExitOk;
  }

  return EmitError(err, early_json,
                   "未知 driver 子命令 '" + sub + "' (可选: list, catalog, install, uninstall)",
                   kCliExitUsage);
}

int HandleProfile(const std::vector<std::string>& args, std::ostream& out, std::ostream& err,
                  bool early_json) {
  if (args.size() < 2 || args[1] == "-h" || args[1] == "--help") {
    PrintCommandHelp("profile", out);
    return kCliExitOk;
  }

  const std::string& raw_sub = args[1];
  const std::string sub =
      (raw_sub == "delete" || raw_sub == "rm") ? std::string("remove") : raw_sub;

  OutputFormat format = OutputFormat::Table;
  bool help = false;
  std::string profiles_override;
  std::string driver;
  std::string uri;
  std::string user;
  bool has_driver = false;
  bool has_uri = false;
  bool has_user = false;
  std::optional<std::string> name_arg;
  std::string error;

  for (std::size_t i = 2; i < args.size(); ++i) {
    bool handled = false;
    if (!TryConsumeCommonFormatFlag(args, i, format, help, error, handled)) {
      return EmitError(err, early_json, error, kCliExitUsage);
    }
    if (handled) {
      continue;
    }
    std::string_view flag;
    std::optional<std::string_view> inline_val;
    SplitFlag(args[i], flag, inline_val);

    if (flag == "--profiles") {
      if (!ConsumeFlagValue(args, i, inline_val, flag, profiles_override, error)) {
        return EmitError(err, early_json, error, kCliExitUsage);
      }
      continue;
    }
    if (sub == "save" && (flag == "--driver" || flag == "-d")) {
      has_driver = true;
      if (!ConsumeFlagValue(args, i, inline_val, flag, driver, error)) {
        return EmitError(err, early_json, error, kCliExitUsage);
      }
      continue;
    }
    if (sub == "save" && (flag == "--uri" || flag == "-u")) {
      has_uri = true;
      if (!ConsumeFlagValue(args, i, inline_val, flag, uri, error)) {
        return EmitError(err, early_json, error, kCliExitUsage);
      }
      continue;
    }
    if (sub == "save" && flag == "--user") {
      has_user = true;
      if (!ConsumeFlagValue(args, i, inline_val, flag, user, error)) {
        return EmitError(err, early_json, error, kCliExitUsage);
      }
      continue;
    }
    if (sub == "save" && flag == "--password") {
      return EmitError(
          err, early_json,
          "connections.tsv 不持久化保存密码；请在执行查询时通过 --password-env 或 --password 传入",
          kCliExitUsage);
    }
    if (!args[i].empty() && args[i].front() == '-') {
      return EmitError(err, early_json, "未知参数: " + args[i], kCliExitUsage);
    }
    if ((sub == "show" || sub == "save" || sub == "remove") && !name_arg.has_value()) {
      name_arg = args[i];
      continue;
    }
    return EmitError(err, early_json, "多余的位置参数: " + args[i], kCliExitUsage);
  }

  if (help) {
    PrintCommandHelp("profile", out);
    return kCliExitOk;
  }

  const bool json_mode = (format == OutputFormat::Json);
  const std::string profiles_path =
      profiles_override.empty() ? DefaultProfilesPath() : profiles_override;
  std::vector<ConnectionProfile> profiles = LoadConnectionProfiles(profiles_path);

  if (sub == "list") {
    if (json_mode) {
      JsonWriter w;
      w.BeginObject();
      bool first = true;
      w.KeyBool("ok", true, first);
      w.KeyString("profiles_path", profiles_path, first);
      w.KeySize("count", profiles.size(), first);
      w.Key("profiles", first);
      w.BeginArray();
      bool first_p = true;
      for (const ConnectionProfile& p : profiles) {
        w.Comma(first_p);
        w.BeginObject();
        bool fp = true;
        w.KeyString("name", p.name, fp);
        w.KeyString("driver", p.driver_path, fp);
        w.KeyString("uri", p.uri, fp);
        w.KeyString("user", p.username, fp);
        w.EndObject();
      }
      w.EndArray();
      w.EndObject();
      w.Raw("\n");
      out << w.buf;
      return kCliExitOk;
    }
    std::vector<GridRow> rows;
    rows.reserve(profiles.size());
    for (const ConnectionProfile& p : profiles) {
      rows.push_back(MakeTextRow({p.name, p.driver_path, p.uri, p.username}));
    }
    WriteTabularText(out, format, {"name", "driver", "uri", "user"}, rows);
    return kCliExitOk;
  }

  if (sub == "show") {
    if (!name_arg.has_value() || name_arg->empty()) {
      return EmitError(err, json_mode, "缺少配置名: dbpp profile show <name>", kCliExitUsage);
    }
    const auto it = std::ranges::find_if(profiles, [&name_arg](const ConnectionProfile& p) {
      return p.name == *name_arg;
    });
    if (it == profiles.end()) {
      return EmitError(err, json_mode, "未找到连接配置: " + *name_arg, kCliExitError);
    }
    if (json_mode) {
      JsonWriter w;
      w.BeginObject();
      bool first = true;
      w.KeyBool("ok", true, first);
      w.KeyString("profiles_path", profiles_path, first);
      w.Key("profile", first);
      w.BeginObject();
      bool fp = true;
      w.KeyString("name", it->name, fp);
      w.KeyString("driver", it->driver_path, fp);
      w.KeyString("uri", it->uri, fp);
      w.KeyString("user", it->username, fp);
      w.EndObject();
      w.EndObject();
      w.Raw("\n");
      out << w.buf;
      return kCliExitOk;
    }
    std::vector<GridRow> rows = {
        MakeTextRow({"name", it->name}),
        MakeTextRow({"driver", it->driver_path}),
        MakeTextRow({"uri", it->uri}),
        MakeTextRow({"user", it->username}),
        MakeTextRow({"profiles_path", profiles_path}),
    };
    WriteTabularText(out, format, {"key", "value"}, rows);
    return kCliExitOk;
  }

  if (sub == "save") {
    if (!name_arg.has_value() || name_arg->empty()) {
      return EmitError(err, json_mode,
                       "缺少配置名: dbpp profile save <name> --driver <drv> --uri <uri>",
                       kCliExitUsage);
    }
    if (!has_driver || driver.empty()) {
      return EmitError(err, json_mode, "缺少 --driver <drv>", kCliExitUsage);
    }
    if (!has_uri || uri.empty()) {
      return EmitError(err, json_mode, "缺少 --uri <uri>", kCliExitUsage);
    }
    ConnectionProfile entry{
        .name = *name_arg,
        .driver_path = driver,
        .uri = uri,
        .username = has_user ? user : std::string{},
    };
    bool updated = false;
    for (ConnectionProfile& existing : profiles) {
      if (existing.name == entry.name) {
        existing = entry;
        updated = true;
        break;
      }
    }
    if (!updated) {
      profiles.push_back(entry);
    }
    if (!SaveConnectionProfiles(profiles_path, profiles)) {
      return EmitError(err, json_mode, "保存 connections.tsv 失败: " + profiles_path,
                       kCliExitError);
    }
    if (json_mode) {
      JsonWriter w;
      w.BeginObject();
      bool first = true;
      w.KeyBool("ok", true, first);
      w.KeyBool("updated", updated, first);
      w.KeyString("profiles_path", profiles_path, first);
      w.Key("profile", first);
      w.BeginObject();
      bool fp = true;
      w.KeyString("name", entry.name, fp);
      w.KeyString("driver", entry.driver_path, fp);
      w.KeyString("uri", entry.uri, fp);
      w.KeyString("user", entry.username, fp);
      w.EndObject();
      w.EndObject();
      w.Raw("\n");
      out << w.buf;
      return kCliExitOk;
    }
    out << (updated ? "已更新连接配置 " : "已保存连接配置 ") << entry.name << " (" << profiles_path
        << ")\n";
    return kCliExitOk;
  }

  if (sub == "remove") {
    if (!name_arg.has_value() || name_arg->empty()) {
      return EmitError(err, json_mode, "缺少配置名: dbpp profile remove <name>",
                       kCliExitUsage);
    }
    const std::size_t before = profiles.size();
    std::erase_if(profiles,
                  [&name_arg](const ConnectionProfile& p) { return p.name == *name_arg; });
    if (profiles.size() == before) {
      return EmitError(err, json_mode, "未找到连接配置: " + *name_arg, kCliExitError);
    }
    if (!SaveConnectionProfiles(profiles_path, profiles)) {
      return EmitError(err, json_mode, "保存 connections.tsv 失败: " + profiles_path,
                       kCliExitError);
    }
    if (json_mode) {
      JsonWriter w;
      w.BeginObject();
      bool first = true;
      w.KeyBool("ok", true, first);
      w.KeyBool("removed", true, first);
      w.KeyString("name", *name_arg, first);
      w.KeyString("profiles_path", profiles_path, first);
      w.EndObject();
      w.Raw("\n");
      out << w.buf;
      return kCliExitOk;
    }
    out << "已删除连接配置 " << *name_arg << '\n';
    return kCliExitOk;
  }

  return EmitError(err, early_json,
                   "未知 profile 子命令 '" + raw_sub + "' (可选: list, show, save, remove)",
                   kCliExitUsage);
}

int HandleTables(const std::vector<std::string>& args, std::ostream& out, std::ostream& err,
                 bool early_json) {
  OutputFormat format = OutputFormat::Table;
  bool help = false;
  ConnOptions conn;
  std::string schema_filter;
  ObjectTypeFilter type_filter = ObjectTypeFilter::All;
  std::string error;

  for (std::size_t i = 1; i < args.size(); ++i) {
    bool handled = false;
    if (!TryConsumeCommonFormatFlag(args, i, format, help, error, handled)) {
      return EmitError(err, early_json, error, kCliExitUsage);
    }
    if (handled) {
      continue;
    }
    if (!TryConsumeConnFlag(args, i, conn, error, handled)) {
      return EmitError(err, early_json, error, kCliExitUsage);
    }
    if (handled) {
      continue;
    }
    std::string_view flag;
    std::optional<std::string_view> inline_val;
    SplitFlag(args[i], flag, inline_val);

    if (flag == "--schema") {
      if (!ConsumeFlagValue(args, i, inline_val, flag, schema_filter, error)) {
        return EmitError(err, early_json, error, kCliExitUsage);
      }
      continue;
    }
    if (flag == "--type") {
      std::string val;
      if (!ConsumeFlagValue(args, i, inline_val, flag, val, error)) {
        return EmitError(err, early_json, error, kCliExitUsage);
      }
      const std::string lower = LowerAscii(val);
      if (lower == "all") {
        type_filter = ObjectTypeFilter::All;
      } else if (lower == "table") {
        type_filter = ObjectTypeFilter::Table;
      } else if (lower == "view") {
        type_filter = ObjectTypeFilter::View;
      } else {
        return EmitError(err, early_json, "无效的 --type 值: " + val + " (可选: all, table, view)",
                         kCliExitUsage);
      }
      continue;
    }
    return EmitError(err, early_json, "未知参数: " + args[i], kCliExitUsage);
  }

  if (help) {
    PrintCommandHelp("tables", out);
    return kCliExitOk;
  }

  const bool json_mode = (format == OutputFormat::Json);
  ConnectionRequest req;
  if (const int code = ResolveConnectionRequest(conn, req, error); code != kCliExitOk) {
    return EmitError(err, json_mode, error, code);
  }

  const AdbcOpenResult opened = OpenAdbcBlocking(req);
  if (!opened.error.empty() || !opened.session) {
    return EmitError(err, json_mode, opened.error.empty() ? "连接失败" : opened.error,
                     kCliExitError);
  }

  const AdbcObjectsResult loaded = LoadObjectsBlocking(*opened.session);
  if (!loaded.error.empty()) {
    return EmitError(err, json_mode, loaded.error, kCliExitError);
  }

  std::vector<DbObject> filtered;
  for (const DbObject& obj : loaded.objects) {
    if (!schema_filter.empty() && !EqualsIgnoreCase(obj.db_schema, schema_filter)) {
      continue;
    }
    if (type_filter == ObjectTypeFilter::Table && obj.is_view) {
      continue;
    }
    if (type_filter == ObjectTypeFilter::View && !obj.is_view) {
      continue;
    }
    filtered.push_back(obj);
  }

  if (json_mode) {
    JsonWriter w;
    w.BeginObject();
    bool first = true;
    w.KeyBool("ok", true, first);
    w.KeyString("dialect", DialectName(opened.session->dialect), first);
    w.KeySize("count", filtered.size(), first);
    w.Key("objects", first);
    w.BeginArray();
    bool first_obj = true;
    for (const DbObject& obj : filtered) {
      w.Comma(first_obj);
      w.BeginObject();
      bool fo = true;
      w.KeyString("name", obj.name, fo);
      w.KeyString("qualified_name", QualifiedName(opened.session->dialect, obj), fo);
      w.KeyString("catalog", obj.catalog, fo);
      w.KeyString("schema", obj.db_schema, fo);
      w.KeyString("table_type", obj.table_type, fo);
      w.KeyBool("is_view", obj.is_view, fo);
      w.Key("primary_key", fo);
      w.BeginArray();
      bool first_pk = true;
      for (const std::string& pk : obj.primary_key) {
        w.Comma(first_pk);
        w.String(pk);
      }
      w.EndArray();
      w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    w.Raw("\n");
    out << w.buf;
    return kCliExitOk;
  }

  std::vector<GridRow> rows;
  rows.reserve(filtered.size());
  for (const DbObject& obj : filtered) {
    std::string pks;
    for (std::size_t k = 0; k < obj.primary_key.size(); ++k) {
      if (k > 0) {
        pks += ", ";
      }
      pks += obj.primary_key[k];
    }
    rows.push_back(
        MakeTextRow({obj.name, obj.db_schema, obj.table_type, obj.is_view ? "view" : "table", pks}));
  }
  WriteTabularText(out, format, {"name", "schema", "table_type", "kind", "primary_key"}, rows);
  return kCliExitOk;
}

std::vector<ColumnMeta> ResolveColumnsForObject(AdbcSession& session, const DbObject& obj,
                                                std::string& error_out) {
  AdbcSchemaResult schema = LoadSchemaBlocking(session, obj);
  std::vector<ColumnMeta> cols = std::move(schema.columns);
  if (cols.empty()) {
    const std::string qname = QualifiedName(session.dialect, obj);
    const AdbcQueryResult header =
        ExecuteBlocking(session, "SELECT * FROM " + qname + " LIMIT 0", true, 0);
    if (!header.error.empty()) {
      error_out = !schema.error.empty() ? schema.error : header.error;
      return {};
    }
    for (const std::string& col_name : header.columns) {
      cols.push_back(ColumnMeta{.name = col_name, .type = "ANY"});
    }
  }
  return cols;
}

void AppendJsonTableSchemaObject(JsonWriter& w, Dialect dialect, const DbObject& obj,
                                 const std::vector<ColumnMeta>& cols) {
  w.BeginObject();
  bool fo = true;
  w.KeyString("table", obj.name, fo);
  w.KeyString("qualified_name", QualifiedName(dialect, obj), fo);
  w.KeyString("schema", obj.db_schema, fo);
  w.KeyBool("is_view", obj.is_view, fo);
  w.Key("primary_key", fo);
  w.BeginArray();
  bool first_pk = true;
  for (const std::string& pk : obj.primary_key) {
    w.Comma(first_pk);
    w.String(pk);
  }
  w.EndArray();
  w.Key("columns", fo);
  w.BeginArray();
  bool first_col = true;
  for (const ColumnMeta& col : cols) {
    w.Comma(first_col);
    w.BeginObject();
    bool fc = true;
    w.KeyString("name", col.name, fc);
    w.KeyString("type", col.type, fc);
    w.KeyBool("nullable", col.nullable, fc);
    w.KeyBool("primary_key", col.primary_key, fc);
    w.KeyInt64("primary_key_order", col.primary_key_order, fc);
    w.EndObject();
  }
  w.EndArray();
  w.EndObject();
}

int HandleSchema(const std::vector<std::string>& args, std::ostream& out, std::ostream& err,
                 bool early_json) {
  OutputFormat format = OutputFormat::Table;
  bool help = false;
  ConnOptions conn;
  std::string schema_filter;
  std::optional<std::string> table_arg;
  std::string error;

  for (std::size_t i = 1; i < args.size(); ++i) {
    bool handled = false;
    if (!TryConsumeCommonFormatFlag(args, i, format, help, error, handled)) {
      return EmitError(err, early_json, error, kCliExitUsage);
    }
    if (handled) {
      continue;
    }
    if (!TryConsumeConnFlag(args, i, conn, error, handled)) {
      return EmitError(err, early_json, error, kCliExitUsage);
    }
    if (handled) {
      continue;
    }
    std::string_view flag;
    std::optional<std::string_view> inline_val;
    SplitFlag(args[i], flag, inline_val);

    if (flag == "--schema") {
      if (!ConsumeFlagValue(args, i, inline_val, flag, schema_filter, error)) {
        return EmitError(err, early_json, error, kCliExitUsage);
      }
      continue;
    }
    if (!args[i].empty() && args[i].front() == '-') {
      return EmitError(err, early_json, "未知参数: " + args[i], kCliExitUsage);
    }
    if (!table_arg.has_value()) {
      table_arg = args[i];
      continue;
    }
    return EmitError(err, early_json, "多余的位置参数: " + args[i], kCliExitUsage);
  }

  if (help) {
    PrintCommandHelp("schema", out);
    return kCliExitOk;
  }

  const bool json_mode = (format == OutputFormat::Json);
  ConnectionRequest req;
  if (const int code = ResolveConnectionRequest(conn, req, error); code != kCliExitOk) {
    return EmitError(err, json_mode, error, code);
  }

  const AdbcOpenResult opened = OpenAdbcBlocking(req);
  if (!opened.error.empty() || !opened.session) {
    return EmitError(err, json_mode, opened.error.empty() ? "连接失败" : opened.error,
                     kCliExitError);
  }

  const AdbcObjectsResult loaded = LoadObjectsBlocking(*opened.session);
  if (!loaded.error.empty()) {
    return EmitError(err, json_mode, loaded.error, kCliExitError);
  }

  if (table_arg.has_value()) {
    const auto matched = FindObjectByName(loaded.objects, *table_arg, schema_filter, error);
    if (!matched) {
      return EmitError(err, json_mode, error, kCliExitError);
    }
    const std::vector<ColumnMeta> cols =
        ResolveColumnsForObject(*opened.session, *matched, error);
    if (!error.empty() && cols.empty()) {
      return EmitError(err, json_mode, error, kCliExitError);
    }

    if (json_mode) {
      JsonWriter w;
      w.BeginObject();
      bool first = true;
      w.KeyBool("ok", true, first);
      w.KeyString("dialect", DialectName(opened.session->dialect), first);
      w.KeyString("table", matched->name, first);
      w.KeyString("qualified_name", QualifiedName(opened.session->dialect, *matched), first);
      w.KeyString("schema", matched->db_schema, first);
      w.KeyBool("is_view", matched->is_view, first);
      w.Key("primary_key", first);
      w.BeginArray();
      bool first_pk = true;
      for (const std::string& pk : matched->primary_key) {
        w.Comma(first_pk);
        w.String(pk);
      }
      w.EndArray();
      w.Key("columns", first);
      w.BeginArray();
      bool first_col = true;
      for (const ColumnMeta& col : cols) {
        w.Comma(first_col);
        w.BeginObject();
        bool fc = true;
        w.KeyString("name", col.name, fc);
        w.KeyString("type", col.type, fc);
        w.KeyBool("nullable", col.nullable, fc);
        w.KeyBool("primary_key", col.primary_key, fc);
        w.KeyInt64("primary_key_order", col.primary_key_order, fc);
        w.EndObject();
      }
      w.EndArray();
      w.EndObject();
      w.Raw("\n");
      out << w.buf;
      return kCliExitOk;
    }

    std::vector<GridRow> rows;
    rows.reserve(cols.size());
    for (const ColumnMeta& col : cols) {
      rows.push_back(MakeTextRow({
          col.name,
          col.type,
          col.nullable ? "true" : "false",
          col.primary_key ? "true" : "false",
          std::to_string(col.primary_key_order),
      }));
    }
    WriteTabularText(out, format, {"name", "type", "nullable", "primary_key", "pk_order"}, rows);
    return kCliExitOk;
  }

  std::vector<std::pair<DbObject, std::vector<ColumnMeta>>> all_schemas;
  for (const DbObject& obj : loaded.objects) {
    if (!schema_filter.empty() && !EqualsIgnoreCase(obj.db_schema, schema_filter)) {
      continue;
    }
    std::string col_error;
    std::vector<ColumnMeta> cols = ResolveColumnsForObject(*opened.session, obj, col_error);
    if (!col_error.empty() && cols.empty()) {
      return EmitError(err, json_mode, col_error, kCliExitError);
    }
    all_schemas.emplace_back(obj, std::move(cols));
  }

  if (json_mode) {
    JsonWriter w;
    w.BeginObject();
    bool first = true;
    w.KeyBool("ok", true, first);
    w.KeyString("dialect", DialectName(opened.session->dialect), first);
    w.KeySize("count", all_schemas.size(), first);
    w.Key("tables", first);
    w.BeginArray();
    bool first_tbl = true;
    for (const auto& [obj, cols] : all_schemas) {
      w.Comma(first_tbl);
      AppendJsonTableSchemaObject(w, opened.session->dialect, obj, cols);
    }
    w.EndArray();
    w.EndObject();
    w.Raw("\n");
    out << w.buf;
    return kCliExitOk;
  }

  std::vector<GridRow> rows;
  for (const auto& [obj, cols] : all_schemas) {
    for (const ColumnMeta& col : cols) {
      rows.push_back(MakeTextRow({
          obj.db_schema,
          obj.name,
          col.name,
          col.type,
          col.nullable ? "true" : "false",
          col.primary_key ? "true" : "false",
      }));
    }
  }
  WriteTabularText(out, format, {"schema", "table", "column", "type", "nullable", "primary_key"},
                   rows);
  return kCliExitOk;
}

int HandleRows(const std::vector<std::string>& args, std::ostream& out, std::ostream& err,
               bool early_json) {
  OutputFormat format = OutputFormat::Table;
  bool help = false;
  ConnOptions conn;
  std::string schema_filter;
  std::int64_t limit = kDefaultRowsLimit;
  std::int64_t offset = 0;
  std::string where_expr;
  std::string order_spec;
  std::optional<std::string> table_arg;
  std::string error;

  for (std::size_t i = 1; i < args.size(); ++i) {
    bool handled = false;
    if (!TryConsumeCommonFormatFlag(args, i, format, help, error, handled)) {
      return EmitError(err, early_json, error, kCliExitUsage);
    }
    if (handled) {
      continue;
    }
    if (!TryConsumeConnFlag(args, i, conn, error, handled)) {
      return EmitError(err, early_json, error, kCliExitUsage);
    }
    if (handled) {
      continue;
    }
    std::string_view flag;
    std::optional<std::string_view> inline_val;
    SplitFlag(args[i], flag, inline_val);

    if (flag == "--schema") {
      if (!ConsumeFlagValue(args, i, inline_val, flag, schema_filter, error)) {
        return EmitError(err, early_json, error, kCliExitUsage);
      }
      continue;
    }
    if (flag == "--limit") {
      std::string val;
      if (!ConsumeFlagValue(args, i, inline_val, flag, val, error)) {
        return EmitError(err, early_json, error, kCliExitUsage);
      }
      const auto parsed = ParseStrictInt64(val);
      if (!parsed.has_value() || *parsed < 1 || *parsed > kMaxRowsLimit) {
        return EmitError(err, early_json,
                         "--limit 必须为 1.." + std::to_string(kMaxRowsLimit) + " 之间的整数",
                         kCliExitUsage);
      }
      limit = *parsed;
      continue;
    }
    if (flag == "--offset") {
      std::string val;
      if (!ConsumeFlagValue(args, i, inline_val, flag, val, error)) {
        return EmitError(err, early_json, error, kCliExitUsage);
      }
      const auto parsed = ParseStrictInt64(val);
      if (!parsed.has_value() || *parsed < 0) {
        return EmitError(err, early_json, "--offset 必须为非负整数", kCliExitUsage);
      }
      offset = *parsed;
      continue;
    }
    if (flag == "--where") {
      if (!ConsumeFlagValue(args, i, inline_val, flag, where_expr, error)) {
        return EmitError(err, early_json, error, kCliExitUsage);
      }
      continue;
    }
    if (flag == "--order") {
      if (!ConsumeFlagValue(args, i, inline_val, flag, order_spec, error)) {
        return EmitError(err, early_json, error, kCliExitUsage);
      }
      continue;
    }
    if (!args[i].empty() && args[i].front() == '-') {
      return EmitError(err, early_json, "未知参数: " + args[i], kCliExitUsage);
    }
    if (!table_arg.has_value()) {
      table_arg = args[i];
      continue;
    }
    return EmitError(err, early_json, "多余的位置参数: " + args[i], kCliExitUsage);
  }

  if (help) {
    PrintCommandHelp("rows", out);
    return kCliExitOk;
  }

  const bool json_mode = (format == OutputFormat::Json);
  if (!table_arg.has_value() || table_arg->empty()) {
    return EmitError(err, json_mode, "缺少表名: dbpp rows <conn> <table>", kCliExitUsage);
  }

  ConnectionRequest req;
  if (const int code = ResolveConnectionRequest(conn, req, error); code != kCliExitOk) {
    return EmitError(err, json_mode, error, code);
  }

  const AdbcOpenResult opened = OpenAdbcBlocking(req);
  if (!opened.error.empty() || !opened.session) {
    return EmitError(err, json_mode, opened.error.empty() ? "连接失败" : opened.error,
                     kCliExitError);
  }

  const AdbcObjectsResult loaded = LoadObjectsBlocking(*opened.session);
  if (!loaded.error.empty()) {
    return EmitError(err, json_mode, loaded.error, kCliExitError);
  }

  const auto matched = FindObjectByName(loaded.objects, *table_arg, schema_filter, error);
  if (!matched) {
    return EmitError(err, json_mode, error, kCliExitError);
  }

  const Dialect dialect = opened.session->dialect;
  const std::string qname =
      dialect == Dialect::Redis ? matched->name : QualifiedName(dialect, *matched);
  const std::vector<ColumnMeta> cols = ResolveColumnsForObject(*opened.session, *matched, error);
  if (!error.empty() && cols.empty()) {
    return EmitError(err, json_mode, error, kCliExitError);
  }

  std::optional<huxerui::DataTableSort> redis_sort;
  std::string order_clause;
  if (!order_spec.empty()) {
    std::string_view col_part = order_spec;
    std::string dir = "ASC";
    if (const std::size_t colon = col_part.rfind(':'); colon != std::string_view::npos) {
      const std::string suffix = LowerAscii(col_part.substr(colon + 1));
      if (suffix == "asc" || suffix == "desc") {
        dir = (suffix == "desc") ? "DESC" : "ASC";
        col_part = col_part.substr(0, colon);
      } else {
        return EmitError(err, json_mode,
                         "无效的排序方向 ':" + suffix + "' (应为 :asc 或 :desc)",
                         kCliExitUsage);
      }
    }
    const ColumnMeta* matched_col = nullptr;
    for (const ColumnMeta& col : cols) {
      if (col.name == col_part) {
        matched_col = &col;
        break;
      }
    }
    if (matched_col == nullptr) {
      for (const ColumnMeta& col : cols) {
        if (EqualsIgnoreCase(col.name, col_part)) {
          matched_col = &col;
          break;
        }
      }
    }
    if (matched_col == nullptr) {
      return EmitError(err, json_mode, "未知排序列: " + std::string(col_part), kCliExitUsage);
    }
    order_clause = " ORDER BY " + QuoteIdentifier(dialect, matched_col->name) + " " + dir;
    redis_sort = huxerui::DataTableSort{
        .column_id = matched_col->name,
        .direction = dir == "DESC" ? huxerui::DataTableSortDirection::Descending
                                   : huxerui::DataTableSortDirection::Ascending,
    };
  }

  if (dialect == Dialect::Redis && opened.session->redis) {
    const RedisPageResult page_res = LoadRedisKeysPageBlocking(
        *opened.session->redis, *matched, offset, limit, redis_sort,
        std::string(TrimAscii(where_expr)));
    if (!page_res.error.empty()) {
      return EmitError(err, json_mode, page_res.error, kCliExitError);
    }
    std::vector<std::string> col_names;
    col_names.reserve(page_res.columns.size());
    for (const ColumnMeta& c : page_res.columns) {
      col_names.push_back(c.name);
    }
    const std::int64_t total = page_res.total;
    const std::int64_t returned = static_cast<std::int64_t>(page_res.rows.size());
    const bool truncated = (offset + returned < total);

    if (json_mode) {
      JsonWriter w;
      w.BeginObject();
      bool first = true;
      w.KeyBool("ok", true, first);
      w.KeyString("table", matched->name, first);
      w.KeyString("qualified_name", qname, first);
      w.KeyInt64("total", total, first);
      w.KeyInt64("limit", limit, first);
      w.KeyInt64("offset", offset, first);
      w.KeyInt64("returned", returned, first);
      w.KeyBool("truncated", truncated, first);
      AppendJsonRowsColumns(w, col_names, page_res.rows, first);
      w.EndObject();
      w.Raw("\n");
      out << w.buf;
      return kCliExitOk;
    }

    WriteTabularText(out, format, col_names, page_res.rows);
    if (format == OutputFormat::Table) {
      out << "(" << returned << " of " << total << " rows, offset " << offset << ")\n";
    }
    return kCliExitOk;
  }

  std::string where_clause;
  if (!TrimAscii(where_expr).empty()) {
    where_clause = " WHERE (" + std::string(TrimAscii(where_expr)) + ")";
  }

  const AdbcQueryResult count_res =
      ExecuteBlocking(*opened.session, "SELECT COUNT(*) FROM " + qname + where_clause, true, 1);
  if (!count_res.error.empty() || count_res.rows.empty() ||
      count_res.rows.front().values.empty()) {
    return EmitError(err, json_mode,
                     count_res.error.empty() ? "COUNT 查询未返回结果" : count_res.error,
                     kCliExitError);
  }
  const std::int64_t total = ParseStrictInt64(count_res.rows.front().values.front().text).value_or(0);

  std::string tie_breaker;
  std::vector<const ColumnMeta*> pk_cols;
  for (const ColumnMeta& col : cols) {
    if (col.primary_key) {
      pk_cols.push_back(&col);
    }
  }
  std::ranges::sort(pk_cols, [](const ColumnMeta* a, const ColumnMeta* b) {
    return a->primary_key_order < b->primary_key_order;
  });
  for (const ColumnMeta* col : pk_cols) {
    if (!tie_breaker.empty()) {
      tie_breaker += ", ";
    }
    tie_breaker += QuoteIdentifier(dialect, col->name);
  }
  if (tie_breaker.empty() && dialect == Dialect::Sqlite && !matched->is_view) {
    const AdbcQueryResult probe =
        ExecuteBlocking(*opened.session, "SELECT rowid FROM " + qname + " LIMIT 0", true, 0);
    if (probe.error.empty()) {
      tie_breaker = "rowid";
    }
  }
  if (!tie_breaker.empty()) {
    order_clause += order_clause.empty() ? " ORDER BY " : ", ";
    order_clause += tie_breaker;
  }

  std::string select_list;
  for (const ColumnMeta& col : cols) {
    if (!select_list.empty()) {
      select_list += ", ";
    }
    select_list += QuoteIdentifier(dialect, col.name);
  }
  if (select_list.empty()) {
    select_list = "*";
  }

  const std::string data_sql = "SELECT " + select_list + " FROM " + qname + where_clause +
                               order_clause + " LIMIT " + std::to_string(limit) + " OFFSET " +
                               std::to_string(offset);
  const AdbcQueryResult data =
      ExecuteBlocking(*opened.session, data_sql, true, static_cast<std::size_t>(limit));
  if (!data.error.empty() && data.rows.empty()) {
    return EmitError(err, json_mode, data.error, kCliExitError);
  }

  const std::int64_t returned = static_cast<std::int64_t>(data.rows.size());
  const bool truncated = (offset + returned < total) || data.truncated;

  if (json_mode) {
    JsonWriter w;
    w.BeginObject();
    bool first = true;
    w.KeyBool("ok", true, first);
    w.KeyString("table", matched->name, first);
    w.KeyString("qualified_name", qname, first);
    w.KeyInt64("total", total, first);
    w.KeyInt64("limit", limit, first);
    w.KeyInt64("offset", offset, first);
    w.KeyInt64("returned", returned, first);
    w.KeyBool("truncated", truncated, first);
    AppendJsonRowsColumns(w, data.columns, data.rows, first);
    w.EndObject();
    w.Raw("\n");
    out << w.buf;
    return kCliExitOk;
  }

  WriteTabularText(out, format, data.columns, data.rows);
  if (format == OutputFormat::Table) {
    out << "(" << returned << " of " << total << " rows, offset " << offset << ")\n";
  }
  return kCliExitOk;
}

int HandleQueryOrExecOrSql(const std::string& mode, const std::vector<std::string>& args,
                           std::ostream& out, std::ostream& err, std::istream& in,
                           bool early_json) {
  OutputFormat format = OutputFormat::Table;
  bool help = false;
  ConnOptions conn;
  std::size_t max_rows = kDefaultQueryMaxRows;
  bool read_only = false;
  bool script_mode = false;
  std::optional<std::string> sql_arg;
  std::optional<std::string> file_arg;
  std::string error;

  for (std::size_t i = 1; i < args.size(); ++i) {
    if (args[i] == "-") {
      if (sql_arg.has_value()) {
        return EmitError(err, early_json, "多余的位置参数: -", kCliExitUsage);
      }
      sql_arg = "-";
      continue;
    }

    bool handled = false;
    if (!TryConsumeCommonFormatFlag(args, i, format, help, error, handled)) {
      return EmitError(err, early_json, error, kCliExitUsage);
    }
    if (handled) {
      continue;
    }
    if (!TryConsumeConnFlag(args, i, conn, error, handled)) {
      return EmitError(err, early_json, error, kCliExitUsage);
    }
    if (handled) {
      continue;
    }

    std::string_view flag;
    std::optional<std::string_view> inline_val;
    SplitFlag(args[i], flag, inline_val);

    if (flag == "--sql") {
      std::string val;
      if (!ConsumeFlagValue(args, i, inline_val, flag, val, error)) {
        return EmitError(err, early_json, error, kCliExitUsage);
      }
      if (sql_arg.has_value()) {
        return EmitError(err, early_json, "不能多次指定 SQL 语句", kCliExitUsage);
      }
      sql_arg = std::move(val);
      continue;
    }
    if (flag == "--file" || flag == "-F") {
      std::string val;
      if (!ConsumeFlagValue(args, i, inline_val, flag, val, error)) {
        return EmitError(err, early_json, error, kCliExitUsage);
      }
      file_arg = std::move(val);
      continue;
    }
    if ((mode == "query" || mode == "sql") && flag == "--max-rows") {
      std::string val;
      if (!ConsumeFlagValue(args, i, inline_val, flag, val, error)) {
        return EmitError(err, early_json, error, kCliExitUsage);
      }
      const auto parsed = ParseStrictInt64(val);
      if (!parsed.has_value() || *parsed < 1 ||
          static_cast<std::size_t>(*parsed) > kMaxQueryMaxRows) {
        return EmitError(err, early_json,
                         "--max-rows 必须为 1.." + std::to_string(kMaxQueryMaxRows) + " 之间的整数",
                         kCliExitUsage);
      }
      max_rows = static_cast<std::size_t>(*parsed);
      continue;
    }
    if (mode == "query" && flag == "--read-only") {
      if (inline_val.has_value()) {
        return EmitError(err, early_json, "参数 --read-only 不接受值", kCliExitUsage);
      }
      read_only = true;
      continue;
    }
    if (mode == "exec" && flag == "--script") {
      if (inline_val.has_value()) {
        return EmitError(err, early_json, "参数 --script 不接受值", kCliExitUsage);
      }
      script_mode = true;
      continue;
    }
    if (!args[i].empty() && args[i].front() == '-') {
      return EmitError(err, early_json, "未知参数: " + args[i], kCliExitUsage);
    }
    if (!sql_arg.has_value()) {
      sql_arg = args[i];
      continue;
    }
    return EmitError(err, early_json, "多余的位置参数: " + args[i], kCliExitUsage);
  }

  if (help) {
    PrintCommandHelp(mode, out);
    return kCliExitOk;
  }

  const bool json_mode = (format == OutputFormat::Json);
  std::string sql_text;
  int read_code = kCliExitOk;
  if (!ReadSqlInput(err, json_mode, in, sql_arg, file_arg, sql_text, read_code)) {
    return read_code;
  }

  ConnectionRequest req;
  if (const int code = ResolveConnectionRequest(conn, req, error); code != kCliExitOk) {
    return EmitError(err, json_mode, error, code);
  }

  const bool anticipated_redis =
      ContainsIgnoreCase(req.driver_path, "redis") ||
      LowerAscii(req.uri).starts_with("redis://") ||
      LowerAscii(req.uri).starts_with("rediss://");
  if (read_only &&
      !IsReadStatement(anticipated_redis ? Dialect::Redis : Dialect::Generic, sql_text)) {
    return EmitError(err, json_mode, "使用 --read-only 时仅允许只读查询语句", kCliExitUsage);
  }

  const AdbcOpenResult opened = OpenAdbcBlocking(req);
  if (!opened.error.empty() || !opened.session) {
    return EmitError(err, json_mode, opened.error.empty() ? "连接失败" : opened.error,
                     kCliExitError);
  }

  if (mode == "exec" && script_mode) {
    const std::vector<std::string> stmts =
        opened.session->dialect == Dialect::Redis
            ? SplitRedisScript(sql_text)
            : SplitSqlStatements(sql_text, opened.session->dialect);
    if (stmts.empty()) {
      return EmitError(err, json_mode, "脚本中不包含可执行 SQL 语句", kCliExitUsage);
    }
    std::size_t executed = 0;
    std::int64_t total_affected = 0;
    bool any_affected_known = false;
    for (const std::string& stmt : stmts) {
      const AdbcQueryResult res = ExecuteBlocking(*opened.session, stmt, false, 0);
      if (!res.error.empty()) {
        return EmitError(err, json_mode,
                         "第 " + std::to_string(executed + 1) + " 条语句执行失败: " + res.error,
                         kCliExitError);
      }
      ++executed;
      if (res.affected >= 0) {
        total_affected += res.affected;
        any_affected_known = true;
      }
    }
    const std::int64_t reported_affected = any_affected_known ? total_affected : -1;
    if (json_mode) {
      JsonWriter w;
      w.BeginObject();
      bool first = true;
      w.KeyBool("ok", true, first);
      w.KeySize("statements_executed", executed, first);
      w.KeyInt64("affected_rows", reported_affected, first);
      w.EndObject();
      w.Raw("\n");
      out << w.buf;
      return kCliExitOk;
    }
    out << "已执行 " << executed << " 条语句";
    if (reported_affected >= 0) {
      out << " (影响 " << reported_affected << " 行)";
    }
    out << '\n';
    return kCliExitOk;
  }

  const bool want_rows =
      (mode == "query") ||
      (mode == "sql" && IsReadStatement(opened.session->dialect, sql_text));
  const AdbcQueryResult res =
      ExecuteBlocking(*opened.session, sql_text, want_rows, want_rows ? max_rows : 0);
  if (!res.error.empty() && (!want_rows || res.rows.empty())) {
    return EmitError(err, json_mode, res.error, kCliExitError);
  }

  if (!want_rows) {
    if (json_mode) {
      JsonWriter w;
      w.BeginObject();
      bool first = true;
      w.KeyBool("ok", true, first);
      if (mode == "sql") {
        w.KeyString("mode", "exec", first);
      }
      w.KeyInt64("affected_rows", res.affected, first);
      w.EndObject();
      w.Raw("\n");
      out << w.buf;
      return kCliExitOk;
    }
    if (res.affected >= 0) {
      out << "影响 " << res.affected << " 行\n";
    } else {
      out << "执行完成\n";
    }
    return kCliExitOk;
  }

  if (json_mode) {
    JsonWriter w;
    w.BeginObject();
    bool first = true;
    w.KeyBool("ok", true, first);
    if (mode == "sql") {
      w.KeyString("mode", "query", first);
    }
    w.KeySize("row_count", res.rows.size(), first);
    w.KeyBool("truncated", res.truncated, first);
    AppendJsonRowsColumns(w, res.columns, res.rows, first);
    w.EndObject();
    w.Raw("\n");
    out << w.buf;
    return kCliExitOk;
  }

  WriteTabularText(out, format, res.columns, res.rows);
  if (format == OutputFormat::Table && res.truncated) {
    out << "(truncated at " << res.rows.size() << " rows)\n";
  }
  return kCliExitOk;
}

#if defined(_WIN32)
void AttachWindowsParentConsoleIfInteractive() {
  HANDLE h_out = GetStdHandle(STD_OUTPUT_HANDLE);
  const DWORD out_type =
      (h_out != nullptr && h_out != INVALID_HANDLE_VALUE) ? GetFileType(h_out)
                                                          : FILE_TYPE_UNKNOWN;
  if (out_type != FILE_TYPE_UNKNOWN) {
    return;
  }
  if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
    return;
  }
  std::freopen("CONOUT$", "w", stdout);
  std::freopen("CONOUT$", "w", stderr);
  HANDLE h_in = GetStdHandle(STD_INPUT_HANDLE);
  const DWORD in_type =
      (h_in != nullptr && h_in != INVALID_HANDLE_VALUE) ? GetFileType(h_in)
                                                        : FILE_TYPE_UNKNOWN;
  if (in_type == FILE_TYPE_UNKNOWN) {
    std::freopen("CONIN$", "r", stdin);
  }
  SetConsoleOutputCP(CP_UTF8);
}
#endif

void ApplyGuiLaunchEnv(const std::vector<std::string>& args) {
  if (args.empty()) {
    return;
  }
  if (args[0] != "gui" && args[0] != "open") {
    if (!args[0].empty()) {
      SetEnvVar("DBPP_OPEN", args[0].c_str());
    }
    return;
  }
  std::string uri;
  std::string driver;
  for (std::size_t i = 1; i < args.size(); ++i) {
    std::string_view flag;
    std::optional<std::string_view> inline_val;
    SplitFlag(args[i], flag, inline_val);
    if (flag == "--driver" || flag == "-d") {
      if (inline_val.has_value()) {
        driver = std::string(*inline_val);
      } else if (i + 1 < args.size()) {
        driver = args[++i];
      }
      continue;
    }
    if (flag == "--uri" || flag == "-u") {
      if (inline_val.has_value()) {
        uri = std::string(*inline_val);
      } else if (i + 1 < args.size()) {
        uri = args[++i];
      }
      continue;
    }
    if (uri.empty() && !args[i].empty() && args[i].front() != '-') {
      uri = args[i];
    }
  }
  if (!uri.empty()) {
    SetEnvVar("DBPP_OPEN", uri.c_str());
  }
  if (!driver.empty()) {
    SetEnvVar("DBPP_DRIVER", driver.c_str());
  }
}

} // namespace

bool ShouldDispatchCli(const std::vector<std::string>& args) {
  if (args.empty()) {
    return false;
  }
  const std::string& first = args[0];
  if (first == "gui" || first == "open") {
    return ContainsHelpFlag(args);
  }
  if (args.size() == 1) {
    return IsKnownCliCommand(first) || (!first.empty() && first.front() == '-');
  }
  return true;
}

int RunCli(const std::vector<std::string>& args, std::ostream& out, std::ostream& err,
           std::istream& in) {
  if (args.empty()) {
    PrintTopLevelHelp(out);
    return kCliExitOk;
  }

  const bool early_json = DetectJsonRequestedEarly(args);
  const std::string& cmd = args[0];

  if (cmd == "-h" || cmd == "--help") {
    PrintTopLevelHelp(out);
    return kCliExitOk;
  }
  if (cmd == "help") {
    if (args.size() > 1 && args[1] != "-h" && args[1] != "--help") {
      if (!PrintCommandHelp(args[1], out)) {
        return EmitError(err, early_json, "未知命令: " + args[1], kCliExitUsage);
      }
      return kCliExitOk;
    }
    PrintTopLevelHelp(out);
    return kCliExitOk;
  }
  if (cmd == "-v" || cmd == "--version") {
    return HandleVersion({"version"}, out, err, early_json);
  }
  if (cmd == "version") {
    return HandleVersion(args, out, err, early_json);
  }
  if (cmd == "doctor") {
    return HandleDoctor(args, out, err, early_json);
  }
  if (cmd == "driver" || cmd == "drivers") {
    return HandleDriver(args, out, err, early_json);
  }
  if (cmd == "profile" || cmd == "profiles") {
    return HandleProfile(args, out, err, early_json);
  }
  if (cmd == "tables" || cmd == "objects") {
    return HandleTables(args, out, err, early_json);
  }
  if (cmd == "schema" || cmd == "describe" || cmd == "desc") {
    return HandleSchema(args, out, err, early_json);
  }
  if (cmd == "rows") {
    return HandleRows(args, out, err, early_json);
  }
  if (cmd == "query" || cmd == "exec" || cmd == "sql") {
    return HandleQueryOrExecOrSql(cmd, args, out, err, in, early_json);
  }
  if (cmd == "gui" || cmd == "open") {
    PrintCommandHelp(cmd, out);
    return kCliExitOk;
  }

  return EmitError(err, early_json, "未知命令: " + cmd + " (使用 --help 查看用法)",
                   kCliExitUsage);
}

std::optional<int> TryRunCli(int argc, char** argv) {
  std::vector<std::string> args;
  if (argc > 1 && argv != nullptr) {
    args.reserve(static_cast<std::size_t>(argc - 1));
    for (int i = 1; i < argc; ++i) {
      if (argv[i] != nullptr) {
        args.emplace_back(argv[i]);
      }
    }
  }

  if (!ShouldDispatchCli(args)) {
    ApplyGuiLaunchEnv(args);
    return std::nullopt;
  }

#if defined(_WIN32)
  AttachWindowsParentConsoleIfInteractive();
#endif
  return RunCli(args, std::cout, std::cerr, std::cin);
}

} // namespace dbpp
