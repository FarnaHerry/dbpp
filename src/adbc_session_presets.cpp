module;

#include <huxerui/datatable.h>

module dbpp.adbc;

import std;
import dbpp.types;
import dbpp.i18n;
import dbpp.redis;

namespace dbpp {

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
  std::string_view h = detail::AdbcTrimAscii(host);
  if (h.find("://") != std::string_view::npos) {
    return std::string(h);
  }
  if (h.empty()) {
    h = preset.default_host.empty() ? std::string_view("127.0.0.1")
                                    : std::string_view(preset.default_host);
  }
  const std::string_view p = detail::AdbcTrimAscii(port);
  const std::string_view db = detail::AdbcTrimAscii(database);
  std::string_view q = detail::AdbcTrimAscii(extra_query);
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

  const std::string enc_db = detail::AdbcPercentEncodeUriComponent(db);
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
  const std::string_view trimmed = detail::AdbcTrimAscii(uri);
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
      out.username = detail::AdbcPercentDecodeUriComponent(userinfo.substr(0, colon));
      out.password = detail::AdbcPercentDecodeUriComponent(userinfo.substr(colon + 1));
    } else if (detail::AdbcEqualsIgnoreCase(scheme, "redis") || detail::AdbcEqualsIgnoreCase(scheme, "rediss")) {
      out.password = detail::AdbcPercentDecodeUriComponent(userinfo);
    } else {
      out.username = detail::AdbcPercentDecodeUriComponent(userinfo);
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
      if (detail::AdbcEqualsIgnoreCase(key, "database") && out.database.empty()) {
        out.database = detail::AdbcPercentDecodeUriComponent(val);
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
      out.database = detail::AdbcPercentDecodeUriComponent(path);
    }
    out.extra_query = std::move(remaining_query);
  } else {
    if (!path.empty()) {
      out.database = detail::AdbcPercentDecodeUriComponent(path);
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
          (info.is_manifest && detail::AdbcEqualsIgnoreCase(info.path, "sqlite")) ||
          detail::AdbcContainsLower(info.path, "adbc_driver_sqlite")) {
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
    if (info.is_manifest && detail::AdbcEqualsIgnoreCase(info.path, preset.driver_slug)) {
      return info;
    }
  }
  const std::string lib_token = "adbc_driver_" + detail::AdbcLowerAscii(preset.driver_slug);
  for (const AdbcDriverInfo& info : discovered) {
    if (detail::AdbcContainsLower(info.path, lib_token) || detail::AdbcEqualsIgnoreCase(info.name, preset.name)) {
      return info;
    }
  }
  return std::nullopt;
}

DatabaseBrand DetectDatabaseBrand(std::string_view driver_or_id, std::string_view uri,
                                  Dialect dialect) {
  if (!driver_or_id.empty()) {
    const std::string lower = detail::AdbcLowerAscii(driver_or_id);
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
    const std::string lower = detail::AdbcLowerAscii(uri);
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
  if (detail::AdbcLowerAscii(base).ends_with(".toml")) {
    base.resize(base.size() - 5);
  } else {
    for (const std::string_view suffix : {".so", ".dylib", ".dll"}) {
      const auto position = detail::AdbcLowerAscii(base).find(suffix);
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
  const std::string lower = detail::AdbcLowerAscii(base);
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
  const std::string lower = detail::AdbcLowerAscii(filename);
  if (lower.find("adbc_driver") == std::string::npos) {
    return false;
  }
  return lower.ends_with(".so") || lower.ends_with(".dylib") ||
         lower.ends_with(".dll") || lower.find(".so.") != std::string::npos;
}

std::string DriverBaseKey(std::string_view filename) {
  std::string base = detail::AdbcLowerAscii(filename);
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
    if (!detail::AdbcHasTomlExtension(path) || detail::AdbcIsProfileManifest(path)) {
      return;
    }
    const std::string slug = path.stem().string();
    if (slug.empty()) {
      return;
    }
    const std::string key = "manifest:" + detail::AdbcLowerAscii(slug);
    if (by_key.contains(key)) {
      return;
    }
    const auto parsed = detail::AdbcParseDriverManifest(path);
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
      for (const std::string& entry : detail::AdbcSplitSearchPath(value)) {
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
    add_manifest_dir(detail::AdbcEnvAdbcDriversDir(env_var));
  }
  add_manifest_dir(UserAdbcDriversDir());
  add_manifest_dir(detail::AdbcSystemAdbcDriversDir());

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
      for (const std::string& entry : detail::AdbcSplitSearchPath(value)) {
        add_library_entry(entry);
      }
    }
  }

  add_library_entry(detail::AdbcBundledDriverDirectory().string());
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
