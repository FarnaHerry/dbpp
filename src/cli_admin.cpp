module;

#include <huxerui/datatable.h>

#ifndef DBPP_VERSION
#define DBPP_VERSION "0.4.8"
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

// ---------------------------------------------------------------------------
// Help output
// ---------------------------------------------------------------------------

void CliPrintTopLevelHelp(std::ostream& out) {
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
      << "                               Manage saved connection profiles (dbpp.sqlite3)\n"
      << "  tables   <conn>              List tables and views\n"
      << "  schema   <conn> [table]      Inspect columns and primary keys\n"
      << "  rows     <conn> <table>      Paginated table rows (--limit/--offset/--where/--order)\n"
      << "  query    <conn> <sql|-|--file <path>>\n"
      << "                               Execute SQL query and return result rows\n"
      << "  exec     <conn> <sql|-|--file <path>> [--script]\n"
      << "                               Execute DDL/DML statement(s) and return affected rows\n"
      << "  sql      <conn> <sql|-|--file <path>>\n"
      << "                               Auto-detect query vs exec from SQL statement\n"
      << "  info     <conn>              Show ADBC driver/vendor metadata (GetInfo)\n"
      << "  stats    <conn> <table>      Show ADBC table/column statistics (GetStatistics)\n"
      << "  import   <conn> <table> <file>\n"
      << "                               Bulk ingest CSV/TSV/JSON/JSONL/SQL/MD/HTML/XML/XLSX into table\n"
      << "  export   <conn> <sql|-|--file <path>> --out <file>\n"
      << "                               Stream Arrow results to CSV/TSV/JSON/JSONL/SQL/MD/HTML/XML/XLSX\n"
      << "  copy     <src_conn> <dst_conn> --sql <sql> --table <table>\n"
      << "                               Stream Arrow query results between connections\n"
      << "  version                      Print version\n"
      << "  help [command]               Show help for top-level or subcommand\n"
      << "\n"
      << "Connection options (<conn>):\n"
      << "  --profile, -p <name>         Use saved profile from dbpp.sqlite3\n"
      << "  --sqlite <path>              Open local SQLite file (or :memory:)\n"
      << "  --redis <uri>                Connect to Redis (redis://[user:pass@]host[:port][/db])\n"
      << "  --uri, -u <uri>              Connection URI (or libpq conninfo)\n"
      << "  --driver, -d <slug|path>     ADBC driver slug, .toml path, or shared lib path\n"
      << "  --user <name>                Override username\n"
      << "  --password <pw>              Provide password directly\n"
      << "  --password-env <VAR>         Read password from environment variable (fallback: DBPP_PASSWORD)\n"
      << "  --profiles <path>            Override dbpp.sqlite3 path (env DBPP_DATA_DIR)\n"
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

bool CliPrintCommandHelp(std::string_view cmd, std::ostream& out) {
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
        << "  dbpp profile save <name> --driver <drv> --uri <uri> [--user <u>] [--password <pw>|--password-env <VAR>] [--profiles <path>] [--json]\n"
        << "  dbpp profile remove <name> [--profiles <path>] [--json]\n"
        << "Note: Passwords saved in dbpp.sqlite3 can still be overridden at query time via --password-env <VAR>.\n";
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
        << "                 [--where <expr>] [--type <redis-type>] [--order <col[:asc|desc]>]\n"
        << "                 [--format ...] [--json]\n"
        << "Defaults: --limit 50, --offset 0.\n";
    return true;
  }
  if (cmd == "query") {
    out << "Usage: dbpp query <conn> [<sql> | --sql <sql> | --file <path> | -]\n"
        << "                  [--param <val>]... [--param-null]... [--schema-only]\n"
        << "                  [--max-rows N] [--read-only] [--format ...] [--json]\n"
        << "Defaults: --max-rows 500.\n";
    return true;
  }
  if (cmd == "info") {
    out << "Usage: dbpp info <conn> [--format ...] [--json]\n"
        << "Inspects ADBC driver/vendor metadata via AdbcConnectionGetInfo and GetTableTypes.\n";
    return true;
  }
  if (cmd == "stats") {
    out << "Usage: dbpp stats <conn> <table> [--schema <name>] [--exact] [--format ...] [--json]\n"
        << "Fetches ADBC table/column statistics via AdbcConnectionGetStatistics.\n";
    return true;
  }
  if (cmd == "import") {
    out << "Usage: dbpp import <conn> <table> <file> [--type csv|tsv|json|jsonl|sql|markdown|html|xml|xlsx]\n"
        << "                   [--mode create|append|replace|create_append] [--delimiter <char>]\n"
        << "                   [--no-header] [--catalog <c>] [--schema <s>] [--json]\n";
    return true;
  }
  if (cmd == "export") {
    out << "Usage: dbpp export <conn> [<sql> | --sql <sql> | --file <path> | -] [--out <file>]\n"
        << "                   [--type csv|tsv|json|jsonl|sql|markdown|html|xml|xlsx]\n"
        << "                   [--table <name>] [--max-rows N] [--json]\n";
    return true;
  }
  if (cmd == "copy") {
    out << "Usage: dbpp copy (--src-sqlite <p> | --src-uri <u> [--src-driver <d>])\n"
        << "                 (--dst-sqlite <p> | --dst-uri <u> [--dst-driver <d>])\n"
        << "                 (--table <table> | --sql <sql> --table <table>)\n"
        << "                 [--mode create|append|replace|create_append] [--json]\n";
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

int CliHandleVersion(const std::vector<std::string>& args, std::ostream& out, std::ostream& err,
                  bool early_json) {
  CliOutputFormat format = CliOutputFormat::Table;
  bool help = false;
  std::string error;

  for (std::size_t i = 1; i < args.size(); ++i) {
    bool handled = false;
    if (!CliTryConsumeCommonFormatFlag(args, i, format, help, error, handled)) {
      return CliEmitError(err, early_json, error, kCliExitUsage);
    }
    if (handled) {
      continue;
    }
    return CliEmitError(err, early_json, "未知参数: " + args[i], kCliExitUsage);
  }
  if (help) {
    CliPrintCommandHelp("version", out);
    return kCliExitOk;
  }
  if (format == CliOutputFormat::Json) {
    CliJsonWriter w;
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

int CliHandleDoctor(const std::vector<std::string>& args, std::ostream& out, std::ostream& err,
                 bool early_json) {
  CliOutputFormat format = CliOutputFormat::Table;
  bool help = false;
  std::string profiles_override;
  std::string error;

  for (std::size_t i = 1; i < args.size(); ++i) {
    bool handled = false;
    if (!CliTryConsumeCommonFormatFlag(args, i, format, help, error, handled)) {
      return CliEmitError(err, early_json, error, kCliExitUsage);
    }
    if (handled) {
      continue;
    }
    std::string_view flag;
    std::optional<std::string_view> inline_val;
    CliSplitFlag(args[i], flag, inline_val);
    if (flag == "--profiles") {
      if (!CliConsumeFlagValue(args, i, inline_val, flag, profiles_override, error)) {
        return CliEmitError(err, early_json, error, kCliExitUsage);
      }
      continue;
    }
    return CliEmitError(err, early_json, "未知参数: " + args[i], kCliExitUsage);
  }
  if (help) {
    CliPrintCommandHelp("doctor", out);
    return kCliExitOk;
  }

  const std::string data_dir = DefaultAppDataDir();
  const std::string profiles_path =
      profiles_override.empty() ? CliDefaultProfilesPath() : profiles_override;
  std::error_code ec;
  const bool profiles_exists = std::filesystem::is_regular_file(profiles_path, ec);
  const std::vector<ConnectionProfile> profiles = LoadConnectionProfiles(profiles_path);

  const std::string sqlite_driver = DefaultSqliteDriverPath();
  ec.clear();
  const bool sqlite_exists = std::filesystem::is_regular_file(sqlite_driver, ec);
  const std::string user_drivers_dir = UserAdbcDriversDir();
  const std::string registry_url = AdbcRegistryBaseUrl();
  const std::vector<AdbcDriverInfo> discovered = DiscoverAdbcDrivers();

  if (format == CliOutputFormat::Json) {
    CliJsonWriter w;
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
      CliMakeTextRow({"version", DBPP_VERSION}),
      CliMakeTextRow({"platform", CurrentPlatformTuple()}),
      CliMakeTextRow({"data_dir", data_dir}),
      CliMakeTextRow({"profiles_path", profiles_path}),
      CliMakeTextRow({"profiles_count", std::to_string(profiles.size())}),
      CliMakeTextRow({"sqlite_driver", sqlite_driver}),
      CliMakeTextRow({"sqlite_driver_exists", sqlite_exists ? "true" : "false"}),
      CliMakeTextRow({"user_drivers_dir", user_drivers_dir}),
      CliMakeTextRow({"registry_url", registry_url}),
      CliMakeTextRow({"discovered_drivers", std::to_string(discovered.size())}),
  };
  CliWriteTabularText(out, format, {"key", "value"}, rows);
  return kCliExitOk;
}

int CliHandleDriver(const std::vector<std::string>& args, std::ostream& out, std::ostream& err,
                 bool early_json) {
  if (args.size() < 2 || args[1] == "-h" || args[1] == "--help") {
    CliPrintCommandHelp("driver", out);
    return kCliExitOk;
  }

  const std::string& sub = args[1];
  CliOutputFormat format = CliOutputFormat::Table;
  bool help = false;
  std::string filter;
  std::string registry_url;
  std::string drivers_dir;
  std::optional<std::string> slug_arg;
  std::string error;

  for (std::size_t i = 2; i < args.size(); ++i) {
    bool handled = false;
    if (!CliTryConsumeCommonFormatFlag(args, i, format, help, error, handled)) {
      return CliEmitError(err, early_json, error, kCliExitUsage);
    }
    if (handled) {
      continue;
    }
    std::string_view flag;
    std::optional<std::string_view> inline_val;
    CliSplitFlag(args[i], flag, inline_val);

    if (flag == "--filter" && sub == "catalog") {
      if (!CliConsumeFlagValue(args, i, inline_val, flag, filter, error)) {
        return CliEmitError(err, early_json, error, kCliExitUsage);
      }
      continue;
    }
    if (flag == "--registry" && (sub == "catalog" || sub == "install")) {
      if (!CliConsumeFlagValue(args, i, inline_val, flag, registry_url, error)) {
        return CliEmitError(err, early_json, error, kCliExitUsage);
      }
      continue;
    }
    if (flag == "--drivers-dir" &&
        (sub == "catalog" || sub == "install" || sub == "uninstall" || sub == "list")) {
      if (!CliConsumeFlagValue(args, i, inline_val, flag, drivers_dir, error)) {
        return CliEmitError(err, early_json, error, kCliExitUsage);
      }
      continue;
    }
    if (!args[i].empty() && args[i].front() == '-') {
      return CliEmitError(err, early_json, "未知参数: " + args[i], kCliExitUsage);
    }
    if ((sub == "install" || sub == "uninstall") && !slug_arg.has_value()) {
      slug_arg = args[i];
      continue;
    }
    return CliEmitError(err, early_json, "多余的位置参数: " + args[i], kCliExitUsage);
  }

  if (help) {
    CliPrintCommandHelp("driver", out);
    return kCliExitOk;
  }

  const bool json_mode = (format == CliOutputFormat::Json);
  const std::filesystem::path drivers_dir_path(drivers_dir);

  if (sub == "list") {
    std::optional<std::string> prev_env;
    bool restore_env = false;
    if (!drivers_dir.empty()) {
      if (const char* cur = std::getenv("DBPP_ADBC_DRIVERS")) {
        prev_env = cur;
      }
      restore_env = true;
      CliSetEnvVar("DBPP_ADBC_DRIVERS", drivers_dir.c_str());
    }
    const std::vector<AdbcDriverInfo> discovered = DiscoverAdbcDrivers();
    if (restore_env) {
      CliSetEnvVar("DBPP_ADBC_DRIVERS", prev_env ? prev_env->c_str() : nullptr);
    }

    if (json_mode) {
      CliJsonWriter w;
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
      rows.push_back(CliMakeTextRow(
          {drv.name, drv.path, drv.version, drv.is_manifest ? "manifest" : "library", drv.detail}));
    }
    CliWriteTabularText(out, format, {"name", "path", "version", "kind", "detail"}, rows);
    return kCliExitOk;
  }

  if (sub == "catalog") {
    const RegistryCatalogResult catalog = FetchRegistryCatalogBlocking(registry_url);
    if (!catalog.error.empty()) {
      return CliEmitError(err, json_mode, catalog.error, kCliExitError);
    }

    std::vector<RegistryDriverInfo> filtered;
    for (const RegistryDriverInfo& drv : catalog.drivers) {
      if (filter.empty() || detail::CliContainsIgnoreCase(drv.slug, filter) ||
          detail::CliContainsIgnoreCase(drv.name, filter) ||
          detail::CliContainsIgnoreCase(drv.description, filter)) {
        filtered.push_back(drv);
      }
    }

    if (json_mode) {
      CliJsonWriter w;
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
      rows.push_back(CliMakeTextRow(
          {drv.slug, drv.name, drv.version, installed ? "yes" : "no", drv.license, drv.description}));
    }
    CliWriteTabularText(out, format, {"slug", "name", "version", "installed", "license", "description"},
                     rows);
    return kCliExitOk;
  }

  if (sub == "install") {
    if (!slug_arg.has_value() || slug_arg->empty()) {
      return CliEmitError(err, json_mode, "缺少驱动 slug: dbpp driver install <slug>",
                       kCliExitUsage);
    }
    const RegistryCatalogResult catalog = FetchRegistryCatalogBlocking(registry_url);
    if (!catalog.error.empty()) {
      return CliEmitError(err, json_mode, catalog.error, kCliExitError);
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
        if (CliEqualsIgnoreCase(drv.slug, *slug_arg)) {
          matched = &drv;
          break;
        }
      }
    }
    if (matched == nullptr) {
      return CliEmitError(err, json_mode,
                       "当前平台 (" + CurrentPlatformTuple() + ") 未找到驱动: " + *slug_arg,
                       kCliExitError);
    }

    const DriverInstallResult res = InstallRegistryDriverBlocking(*matched, drivers_dir_path);
    if (!res.ok) {
      return CliEmitError(err, json_mode, res.message, kCliExitError);
    }
    if (json_mode) {
      CliJsonWriter w;
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
      return CliEmitError(err, json_mode, "缺少驱动 slug: dbpp driver uninstall <slug>",
                       kCliExitUsage);
    }
    const DriverInstallResult res =
        UninstallUserAdbcDriverBlocking(*slug_arg, drivers_dir_path);
    if (!res.ok) {
      return CliEmitError(err, json_mode, res.message, kCliExitError);
    }
    if (json_mode) {
      CliJsonWriter w;
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

  return CliEmitError(err, early_json,
                   "未知 driver 子命令 '" + sub + "' (可选: list, catalog, install, uninstall)",
                   kCliExitUsage);
}

int CliHandleProfile(const std::vector<std::string>& args, std::ostream& out, std::ostream& err,
                  bool early_json) {
  if (args.size() < 2 || args[1] == "-h" || args[1] == "--help") {
    CliPrintCommandHelp("profile", out);
    return kCliExitOk;
  }

  const std::string& raw_sub = args[1];
  const std::string sub =
      (raw_sub == "delete" || raw_sub == "rm") ? std::string("remove") : raw_sub;

  CliOutputFormat format = CliOutputFormat::Table;
  bool help = false;
  std::string profiles_override;
  std::string driver;
  std::string uri;
  std::string user;
  std::string password;
  std::string password_env;
  bool has_driver = false;
  bool has_uri = false;
  bool has_user = false;
  bool has_password = false;
  bool has_password_env = false;
  std::optional<std::string> name_arg;
  std::string error;

  for (std::size_t i = 2; i < args.size(); ++i) {
    bool handled = false;
    if (!CliTryConsumeCommonFormatFlag(args, i, format, help, error, handled)) {
      return CliEmitError(err, early_json, error, kCliExitUsage);
    }
    if (handled) {
      continue;
    }
    std::string_view flag;
    std::optional<std::string_view> inline_val;
    CliSplitFlag(args[i], flag, inline_val);

    if (flag == "--profiles") {
      if (!CliConsumeFlagValue(args, i, inline_val, flag, profiles_override, error)) {
        return CliEmitError(err, early_json, error, kCliExitUsage);
      }
      continue;
    }
    if (sub == "save" && (flag == "--driver" || flag == "-d")) {
      has_driver = true;
      if (!CliConsumeFlagValue(args, i, inline_val, flag, driver, error)) {
        return CliEmitError(err, early_json, error, kCliExitUsage);
      }
      continue;
    }
    if (sub == "save" && (flag == "--uri" || flag == "-u")) {
      has_uri = true;
      if (!CliConsumeFlagValue(args, i, inline_val, flag, uri, error)) {
        return CliEmitError(err, early_json, error, kCliExitUsage);
      }
      continue;
    }
    if (sub == "save" && flag == "--user") {
      has_user = true;
      if (!CliConsumeFlagValue(args, i, inline_val, flag, user, error)) {
        return CliEmitError(err, early_json, error, kCliExitUsage);
      }
      continue;
    }
    if (sub == "save" && flag == "--password") {
      has_password = true;
      if (!CliConsumeFlagValue(args, i, inline_val, flag, password, error)) {
        return CliEmitError(err, early_json, error, kCliExitUsage);
      }
      continue;
    }
    if (sub == "save" && flag == "--password-env") {
      has_password_env = true;
      if (!CliConsumeFlagValue(args, i, inline_val, flag, password_env, error)) {
        return CliEmitError(err, early_json, error, kCliExitUsage);
      }
      continue;
    }
    if (!args[i].empty() && args[i].front() == '-') {
      return CliEmitError(err, early_json, "未知参数: " + args[i], kCliExitUsage);
    }
    if ((sub == "show" || sub == "save" || sub == "remove") && !name_arg.has_value()) {
      name_arg = args[i];
      continue;
    }
    return CliEmitError(err, early_json, "多余的位置参数: " + args[i], kCliExitUsage);
  }

  if (help) {
    CliPrintCommandHelp("profile", out);
    return kCliExitOk;
  }

  const bool json_mode = (format == CliOutputFormat::Json);
  const std::string profiles_path =
      profiles_override.empty() ? CliDefaultProfilesPath() : profiles_override;
  std::vector<ConnectionProfile> profiles = LoadConnectionProfiles(profiles_path);

  if (sub == "list") {
    if (json_mode) {
      CliJsonWriter w;
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
        w.KeyBool("has_password", !p.password.empty(), fp);
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
      rows.push_back(CliMakeTextRow(
          {p.name, p.driver_path, p.uri, p.username, p.password.empty() ? "false" : "true"}));
    }
    CliWriteTabularText(out, format, {"name", "driver", "uri", "user", "has_password"}, rows);
    return kCliExitOk;
  }

  if (sub == "show") {
    if (!name_arg.has_value() || name_arg->empty()) {
      return CliEmitError(err, json_mode, "缺少配置名: dbpp profile show <name>", kCliExitUsage);
    }
    const auto it = std::ranges::find_if(profiles, [&name_arg](const ConnectionProfile& p) {
      return p.name == *name_arg;
    });
    if (it == profiles.end()) {
      return CliEmitError(err, json_mode, "未找到连接配置: " + *name_arg, kCliExitError);
    }
    if (json_mode) {
      CliJsonWriter w;
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
      w.KeyBool("has_password", !it->password.empty(), fp);
      w.EndObject();
      w.EndObject();
      w.Raw("\n");
      out << w.buf;
      return kCliExitOk;
    }
    std::vector<GridRow> rows = {
        CliMakeTextRow({"name", it->name}),
        CliMakeTextRow({"driver", it->driver_path}),
        CliMakeTextRow({"uri", it->uri}),
        CliMakeTextRow({"user", it->username}),
        CliMakeTextRow({"has_password", it->password.empty() ? "false" : "true"}),
        CliMakeTextRow({"profiles_path", profiles_path}),
    };
    CliWriteTabularText(out, format, {"key", "value"}, rows);
    return kCliExitOk;
  }

  if (sub == "save") {
    if (!name_arg.has_value() || name_arg->empty()) {
      return CliEmitError(err, json_mode,
                       "缺少配置名: dbpp profile save <name> --driver <drv> --uri <uri>",
                       kCliExitUsage);
    }
    if (!has_driver || driver.empty()) {
      return CliEmitError(err, json_mode, "缺少 --driver <drv>", kCliExitUsage);
    }
    if (!has_uri || uri.empty()) {
      return CliEmitError(err, json_mode, "缺少 --uri <uri>", kCliExitUsage);
    }
    std::string effective_password;
    if (has_password_env) {
      const char* env_val = std::getenv(password_env.c_str());
      if (env_val == nullptr || *env_val == '\0') {
        return CliEmitError(err, json_mode, "环境变量 '" + password_env + "' 未设置或为空",
                         kCliExitError);
      }
      effective_password = env_val;
    } else if (has_password) {
      effective_password = password;
    }
    ConnectionProfile entry{
        .name = *name_arg,
        .driver_path = driver,
        .uri = uri,
        .username = has_user ? user : std::string{},
        .password = std::move(effective_password),
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
      return CliEmitError(err, json_mode, "保存 dbpp.sqlite3 失败: " + profiles_path,
                       kCliExitError);
    }
    if (json_mode) {
      CliJsonWriter w;
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
      w.KeyBool("has_password", !entry.password.empty(), fp);
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
      return CliEmitError(err, json_mode, "缺少配置名: dbpp profile remove <name>",
                       kCliExitUsage);
    }
    const std::size_t before = profiles.size();
    std::erase_if(profiles,
                  [&name_arg](const ConnectionProfile& p) { return p.name == *name_arg; });
    if (profiles.size() == before) {
      return CliEmitError(err, json_mode, "未找到连接配置: " + *name_arg, kCliExitError);
    }
    if (!SaveConnectionProfiles(profiles_path, profiles)) {
      return CliEmitError(err, json_mode, "保存 dbpp.sqlite3 失败: " + profiles_path,
                       kCliExitError);
    }
    if (json_mode) {
      CliJsonWriter w;
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

  return CliEmitError(err, early_json,
                   "未知 profile 子命令 '" + raw_sub + "' (可选: list, show, save, remove)",
                   kCliExitUsage);
}


} // namespace detail

} // namespace dbpp
