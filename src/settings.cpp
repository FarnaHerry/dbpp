module;

#include <huxerui/state.h>
#include <sqlite3.h>

module dbpp.settings;

import std;

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

std::string TrimAsciiCopy(std::string_view text) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
    text.remove_prefix(1);
  }
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
    text.remove_suffix(1);
  }
  return std::string(text);
}

std::string_view FlagText(bool value) {
  return value ? "true" : "false";
}

bool ExecSql(sqlite3* db, const char* sql) {
  return sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

struct SqliteGuard {
  sqlite3* db = nullptr;
  ~SqliteGuard() {
    if (db != nullptr) {
      sqlite3_close_v2(db);
    }
  }
};

struct StmtGuard {
  sqlite3_stmt* stmt = nullptr;
  ~StmtGuard() {
    if (stmt != nullptr) {
      sqlite3_finalize(stmt);
    }
  }
};

struct LegacyConnectionRow {
  std::string name;
  std::string driver_path;
  std::string uri;
  std::string username;
  std::string password;
};

std::string DecodeLegacyTsvField(std::string_view text) {
  const auto hex_value = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  std::string result;
  result.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '%' && i + 2 < text.size()) {
      const int high = hex_value(text[i + 1]);
      const int low = hex_value(text[i + 2]);
      if (high >= 0 && low >= 0) {
        result.push_back(static_cast<char>((high << 4) | low));
        i += 2;
        continue;
      }
    }
    result.push_back(text[i]);
  }
  return result;
}

std::vector<LegacyConnectionRow> LoadLegacyConnectionTsvRows(const std::string& path) {
  std::vector<LegacyConnectionRow> rows;
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return rows;
  }
  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty() || line.front() == '#') {
      continue;
    }
    std::vector<std::string_view> fields;
    std::size_t start = 0;
    while (start <= line.size() && fields.size() < 5) {
      const std::size_t tab = line.find('\t', start);
      if (tab == std::string::npos) {
        fields.emplace_back(line.data() + start, line.size() - start);
        break;
      }
      fields.emplace_back(line.data() + start, tab - start);
      start = tab + 1;
    }
    if (fields.size() != 4 && fields.size() != 5) {
      continue;
    }
    LegacyConnectionRow row;
    row.name = DecodeLegacyTsvField(fields[0]);
    row.driver_path = DecodeLegacyTsvField(fields[1]);
    row.uri = DecodeLegacyTsvField(fields[2]);
    row.username = DecodeLegacyTsvField(fields[3]);
    if (fields.size() == 5) {
      row.password = DecodeLegacyTsvField(fields[4]);
    }
    if (!row.driver_path.empty() && !row.uri.empty()) {
      rows.push_back(std::move(row));
    }
  }
  return rows;
}

} // namespace

namespace detail {

bool IsLegacyTsvPath(std::string_view path) noexcept {
  return LowerAsciiCopy(path).ends_with(".tsv");
}

void RestrictConfigFilePermissions(std::string_view db_path) noexcept {
#if !defined(_WIN32)
  if (db_path.empty()) {
    return;
  }
  const std::string base(db_path);
  for (const std::string_view suffix : {"", "-wal", "-shm", "-journal"}) {
    const std::filesystem::path candidate(base + std::string(suffix));
    std::error_code ec;
    if (std::filesystem::is_regular_file(candidate, ec)) {
      std::filesystem::permissions(
          candidate,
          std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
          std::filesystem::perm_options::replace, ec);
    }
  }
#else
  (void)db_path;
#endif
}

std::vector<std::pair<std::string, std::string>>
LoadLegacySettingsTsvPairs(const std::string& path) {
  std::vector<std::pair<std::string, std::string>> pairs;
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return pairs;
  }
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    const auto tab = line.find('\t');
    if (tab == std::string::npos || tab == 0) {
      continue;
    }
    pairs.emplace_back(line.substr(0, tab), line.substr(tab + 1));
  }
  return pairs;
}

bool EnsureConfigSqliteSync(const std::string& db_path) {
  if (db_path.empty()) {
    return false;
  }
  const std::filesystem::path target(db_path);
  if (target.has_parent_path()) {
    std::error_code dir_error;
    std::filesystem::create_directories(target.parent_path(), dir_error);
  }

  SqliteGuard conn;
  if (sqlite3_open_v2(db_path.c_str(), &conn.db,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK ||
      conn.db == nullptr) {
    return false;
  }
  sqlite3_busy_timeout(conn.db, 5000);

  if (!ExecSql(conn.db, "PRAGMA journal_mode = WAL;") ||
      !ExecSql(conn.db, "PRAGMA foreign_keys = ON;") ||
      !ExecSql(conn.db,
               "CREATE TABLE IF NOT EXISTS settings ("
               "key TEXT NOT NULL PRIMARY KEY, "
               "value TEXT NOT NULL DEFAULT ''"
               ");") ||
      !ExecSql(conn.db,
               "CREATE TABLE IF NOT EXISTS connections ("
               "id INTEGER PRIMARY KEY AUTOINCREMENT, "
               "sort_order INTEGER NOT NULL DEFAULT 0, "
               "name TEXT NOT NULL DEFAULT '', "
               "driver_path TEXT NOT NULL, "
               "uri TEXT NOT NULL, "
               "username TEXT NOT NULL DEFAULT '', "
               "password TEXT NOT NULL DEFAULT '', "
               "remark TEXT NOT NULL DEFAULT '', "
               "read_only INTEGER NOT NULL DEFAULT 0, "
               "production_mode INTEGER NOT NULL DEFAULT 0, "
               "show_system_databases INTEGER NOT NULL DEFAULT 0"
               ");") ||
      !ExecSql(conn.db,
               "CREATE INDEX IF NOT EXISTS idx_connections_sort_order "
               "ON connections (sort_order, id);") ||
      !ExecSql(conn.db,
               "CREATE TABLE IF NOT EXISTS saved_queries ("
               "id INTEGER PRIMARY KEY AUTOINCREMENT, "
               "sort_order INTEGER NOT NULL DEFAULT 0, "
               "connection_key TEXT NOT NULL DEFAULT '', "
               "database_name TEXT NOT NULL DEFAULT '', "
               "name TEXT NOT NULL DEFAULT '', "
               "sql_text TEXT NOT NULL DEFAULT ''"
               ");") ||
      !ExecSql(conn.db,
               "CREATE INDEX IF NOT EXISTS idx_saved_queries_sort_order "
               "ON saved_queries (sort_order, id);")) {
    return false;
  }

  {
    bool has_remark = false;
    bool has_read_only = false;
    bool has_prod = false;
    bool has_show_sys = false;
    StmtGuard cols_stmt;
    if (sqlite3_prepare_v2(conn.db, "PRAGMA table_info(connections);", -1, &cols_stmt.stmt,
                           nullptr) == SQLITE_OK) {
      while (sqlite3_step(cols_stmt.stmt) == SQLITE_ROW) {
        const unsigned char* ptr = sqlite3_column_text(cols_stmt.stmt, 1);
        const std::string_view col_name =
            ptr ? std::string_view(reinterpret_cast<const char*>(ptr)) : std::string_view{};
        if (col_name == "remark") has_remark = true;
        if (col_name == "read_only") has_read_only = true;
        if (col_name == "production_mode") has_prod = true;
        if (col_name == "show_system_databases") has_show_sys = true;
      }
    }
    if ((!has_remark &&
         !ExecSql(conn.db, "ALTER TABLE connections ADD COLUMN remark TEXT NOT NULL DEFAULT '';")) ||
        (!has_read_only &&
         !ExecSql(conn.db,
                  "ALTER TABLE connections ADD COLUMN read_only INTEGER NOT NULL DEFAULT 0;")) ||
        (!has_prod &&
         !ExecSql(conn.db,
                  "ALTER TABLE connections ADD COLUMN production_mode INTEGER NOT NULL DEFAULT 0;")) ||
        (!has_show_sys &&
         !ExecSql(conn.db,
                  "ALTER TABLE connections ADD COLUMN show_system_databases INTEGER NOT NULL DEFAULT 0;"))) {
      return false;
    }
  }

  {
    StmtGuard version_stmt;
    if (sqlite3_prepare_v2(conn.db, "PRAGMA user_version;", -1, &version_stmt.stmt, nullptr) !=
        SQLITE_OK) {
      return false;
    }
    int version = 0;
    if (sqlite3_step(version_stmt.stmt) == SQLITE_ROW) {
      version = sqlite3_column_int(version_stmt.stmt, 0);
    }
    if (version < 2 && !ExecSql(conn.db, "PRAGMA user_version = 2;")) {
      return false;
    }
  }

  bool already_migrated = false;
  {
    StmtGuard check_stmt;
    if (sqlite3_prepare_v2(conn.db,
                           "SELECT 1 FROM settings WHERE key = 'meta.tsv_migrated' LIMIT 1;",
                           -1, &check_stmt.stmt, nullptr) != SQLITE_OK) {
      return false;
    }
    already_migrated = sqlite3_step(check_stmt.stmt) == SQLITE_ROW;
  }

  if (!already_migrated) {
    if (!ExecSql(conn.db, "BEGIN IMMEDIATE;")) {
      return false;
    }
    bool migrated_in_tx = false;
    {
      StmtGuard check_stmt;
      if (sqlite3_prepare_v2(conn.db,
                             "SELECT 1 FROM settings WHERE key = 'meta.tsv_migrated' LIMIT 1;",
                             -1, &check_stmt.stmt, nullptr) == SQLITE_OK) {
        migrated_in_tx = sqlite3_step(check_stmt.stmt) == SQLITE_ROW;
      }
    }
    bool ok = true;
    if (!migrated_in_tx) {
      std::error_code ec;
      const std::filesystem::path legacy_settings = target.parent_path() / "settings.tsv";
      if (std::filesystem::is_regular_file(legacy_settings, ec)) {
        const auto pairs = LoadLegacySettingsTsvPairs(legacy_settings.string());
        StmtGuard ins_stmt;
        if (sqlite3_prepare_v2(
                conn.db, "INSERT OR IGNORE INTO settings (key, value) VALUES (?, ?);", -1,
                &ins_stmt.stmt, nullptr) != SQLITE_OK) {
          ok = false;
        } else {
          for (const auto& [k, v] : pairs) {
            sqlite3_reset(ins_stmt.stmt);
            sqlite3_clear_bindings(ins_stmt.stmt);
            sqlite3_bind_text(ins_stmt.stmt, 1, k.data(), static_cast<int>(k.size()),
                              SQLITE_TRANSIENT);
            sqlite3_bind_text(ins_stmt.stmt, 2, v.data(), static_cast<int>(v.size()),
                              SQLITE_TRANSIENT);
            if (sqlite3_step(ins_stmt.stmt) != SQLITE_DONE) {
              ok = false;
              break;
            }
          }
        }
      }

      const std::filesystem::path legacy_conns = target.parent_path() / "connections.tsv";
      if (ok && std::filesystem::is_regular_file(legacy_conns, ec)) {
        int existing_count = 0;
        StmtGuard cnt_stmt;
        if (sqlite3_prepare_v2(conn.db, "SELECT COUNT(*) FROM connections;", -1, &cnt_stmt.stmt,
                               nullptr) != SQLITE_OK) {
          ok = false;
        } else if (sqlite3_step(cnt_stmt.stmt) == SQLITE_ROW) {
          existing_count = sqlite3_column_int(cnt_stmt.stmt, 0);
        }
        if (ok && existing_count == 0) {
          const std::vector<LegacyConnectionRow> legacy =
              LoadLegacyConnectionTsvRows(legacy_conns.string());
          StmtGuard ins_stmt;
          if (!legacy.empty()) {
            if (sqlite3_prepare_v2(
                    conn.db,
                    "INSERT INTO connections "
                    "(sort_order, name, driver_path, uri, username, password) "
                    "VALUES (?, ?, ?, ?, ?, ?);",
                    -1, &ins_stmt.stmt, nullptr) != SQLITE_OK) {
              ok = false;
            } else {
              for (std::size_t i = 0; i < legacy.size(); ++i) {
                const auto& p = legacy[i];
                sqlite3_reset(ins_stmt.stmt);
                sqlite3_clear_bindings(ins_stmt.stmt);
                sqlite3_bind_int64(ins_stmt.stmt, 1, static_cast<sqlite3_int64>(i));
                sqlite3_bind_text(ins_stmt.stmt, 2, p.name.data(), static_cast<int>(p.name.size()),
                                  SQLITE_TRANSIENT);
                sqlite3_bind_text(ins_stmt.stmt, 3, p.driver_path.data(),
                                  static_cast<int>(p.driver_path.size()), SQLITE_TRANSIENT);
                sqlite3_bind_text(ins_stmt.stmt, 4, p.uri.data(), static_cast<int>(p.uri.size()),
                                  SQLITE_TRANSIENT);
                sqlite3_bind_text(ins_stmt.stmt, 5, p.username.data(),
                                  static_cast<int>(p.username.size()), SQLITE_TRANSIENT);
                sqlite3_bind_text(ins_stmt.stmt, 6, p.password.data(),
                                  static_cast<int>(p.password.size()), SQLITE_TRANSIENT);
                if (sqlite3_step(ins_stmt.stmt) != SQLITE_DONE) {
                  ok = false;
                  break;
                }
              }
            }
          }
        }
      }

      if (ok && !ExecSql(conn.db,
                         "INSERT OR REPLACE INTO settings (key, value) "
                         "VALUES ('meta.tsv_migrated', '1');")) {
        ok = false;
      }
    }

    if (!ok || !ExecSql(conn.db, "COMMIT;")) {
      ExecSql(conn.db, "ROLLBACK;");
      return false;
    }
  }

  RestrictConfigFilePermissions(db_path);
  return true;
}

} // namespace detail

std::string_view ThemeModeKey(ThemeMode mode) {
  switch (mode) {
    case ThemeMode::Dark: return "dark";
    case ThemeMode::Light: return "light";
    case ThemeMode::System:
    default: return "system";
  }
}

ThemeMode ThemeModeFromKey(std::string_view key) {
  if (key == "dark" || key == "1") {
    return ThemeMode::Dark;
  }
  if (key == "light" || key == "2") {
    return ThemeMode::Light;
  }
  return ThemeMode::System;
}

std::string_view ThemePresetKey(ThemePreset preset) {
  switch (preset) {
    case ThemePreset::Emerald: return "emerald";
    case ThemePreset::WarmAmber: return "warm_amber";
    case ThemePreset::VioletDusk: return "violet_dusk";
    case ThemePreset::Monochrome: return "monochrome";
    case ThemePreset::GeminiSpark:
    default: return "gemini";
  }
}

ThemePreset ThemePresetFromKey(std::string_view key) {
  if (key == "emerald") {
    return ThemePreset::Emerald;
  }
  if (key == "warm_amber") {
    return ThemePreset::WarmAmber;
  }
  if (key == "violet_dusk") {
    return ThemePreset::VioletDusk;
  }
  if (key == "monochrome") {
    return ThemePreset::Monochrome;
  }
  return ThemePreset::GeminiSpark;
}

std::string_view LanguageModeKey(LanguageMode mode) {
  switch (mode) {
    case LanguageMode::Zh: return "zh";
    case LanguageMode::En: return "en";
    case LanguageMode::System:
    default: return "system";
  }
}

LanguageMode LanguageModeFromKey(std::string_view key) {
  const std::string lower = LowerAsciiCopy(key);
  if (lower == "zh" || lower == "zh-cn" || lower == "zh_cn" || lower == "zh-hans" ||
      lower == "zh-hans-cn") {
    return LanguageMode::Zh;
  }
  if (lower == "en" || lower == "en-us" || lower == "en_us" || lower == "en-gb" ||
      lower == "en_gb") {
    return LanguageMode::En;
  }
  return LanguageMode::System;
}

std::string_view RedisViewModeKey(RedisBrowserViewMode mode) {
  switch (mode) {
    case RedisBrowserViewMode::Tree: return "tree";
    case RedisBrowserViewMode::Table:
    default: return "table";
  }
}

RedisBrowserViewMode RedisViewModeFromKey(std::string_view key) {
  const std::string lower = LowerAsciiCopy(TrimAsciiCopy(key));
  if (lower == "tree") {
    return RedisBrowserViewMode::Tree;
  }
  return RedisBrowserViewMode::Table;
}

std::uint16_t ParseMcpPort(std::string_view text, std::uint16_t fallback) noexcept {
  const std::string trimmed = TrimAsciiCopy(text);
  if (trimmed.empty()) {
    return fallback;
  }
  unsigned int value = 0;
  const char* begin = trimmed.data();
  const char* end_ptr = trimmed.data() + trimmed.size();
  const auto [end, ec] = std::from_chars(begin, end_ptr, value);
  if (ec != std::errc{} || end != end_ptr || value == 0 || value > 65535U) {
    return fallback;
  }
  return static_cast<std::uint16_t>(value);
}

std::string NormalizeMcpHost(std::string_view host) {
  const std::string trimmed = TrimAsciiCopy(host);
  return trimmed.empty() ? std::string("127.0.0.1") : trimmed;
}

std::optional<std::int64_t> TryParsePageSize(std::string_view text) noexcept {
  const std::string trimmed = TrimAsciiCopy(text);
  if (trimmed.empty()) {
    return std::nullopt;
  }
  std::int64_t value = 0;
  const char* begin = trimmed.data();
  const char* end_ptr = trimmed.data() + trimmed.size();
  const auto [end, ec] = std::from_chars(begin, end_ptr, value);
  if (ec != std::errc{} || end != end_ptr || value < kMinPageSize || value > kMaxPageSize) {
    return std::nullopt;
  }
  return value;
}

std::int64_t ParsePageSize(std::string_view text, std::int64_t fallback) noexcept {
  const std::optional<std::int64_t> parsed = TryParsePageSize(text);
  if (parsed.has_value()) {
    return *parsed;
  }
  return std::clamp(fallback, kMinPageSize, kMaxPageSize);
}

TraySettings LoadTraySettings(const std::string& path) {
  TraySettings settings;
  if (path.empty()) {
    return settings;
  }

  const auto apply_pair = [&settings](std::string_view key, std::string_view value) {
    if (key == kSettingTrayEnabled) {
      settings.tray_enabled = value == "true";
    } else if (key == kSettingLightweight) {
      settings.lightweight = value == "true";
    } else if (key == kSettingThemeMode) {
      settings.theme_mode = ThemeModeFromKey(value);
    } else if (key == kSettingThemePreset) {
      settings.theme_preset = ThemePresetFromKey(value);
    } else if (key == kSettingUiLanguage) {
      settings.language = LanguageModeFromKey(value);
    } else if (key == kSettingMcpEnabled) {
      settings.mcp_enabled = value == "true";
    } else if (key == kSettingMcpHost) {
      settings.mcp_host = NormalizeMcpHost(value);
    } else if (key == kSettingMcpPort) {
      settings.mcp_port = ParseMcpPort(value);
    } else if (key == kSettingMcpReadOnly) {
      settings.mcp_read_only = value != "false";
    } else if (key == kSettingDefaultPageSize) {
      settings.default_page_size = ParsePageSize(value);
    } else if (key == kSettingRedisViewMode) {
      settings.redis_view_mode = RedisViewModeFromKey(value);
    }
  };

  if (detail::IsLegacyTsvPath(path)) {
    for (const auto& [k, v] : detail::LoadLegacySettingsTsvPairs(path)) {
      apply_pair(k, v);
    }
    return settings;
  }

  std::error_code ec;
  const std::filesystem::path target(path);
  const bool db_exists = std::filesystem::is_regular_file(target, ec);
  const std::filesystem::path legacy_tsv = target.parent_path() / "settings.tsv";
  if (!db_exists) {
    if (std::filesystem::is_regular_file(legacy_tsv, ec)) {
      return LoadTraySettings(legacy_tsv.string());
    }
    return settings;
  }

  SqliteGuard conn;
  if (sqlite3_open_v2(path.c_str(), &conn.db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK ||
      conn.db == nullptr) {
    return settings;
  }
  sqlite3_busy_timeout(conn.db, 5000);
  StmtGuard stmt;
  if (sqlite3_prepare_v2(
          conn.db,
          "SELECT key, value FROM settings "
          "WHERE key IN ('tray.enabled', 'tray.lightweight', 'theme.mode', 'theme.preset', "
          "'ui.language', 'mcp.enabled', 'mcp.host', 'mcp.port', 'mcp.read_only', "
          "'query.page_size', 'redis.view_mode');",
          -1, &stmt.stmt, nullptr) != SQLITE_OK) {
    return settings;
  }
  while (sqlite3_step(stmt.stmt) == SQLITE_ROW) {
    const unsigned char* k_ptr = sqlite3_column_text(stmt.stmt, 0);
    const unsigned char* v_ptr = sqlite3_column_text(stmt.stmt, 1);
    const int k_len = sqlite3_column_bytes(stmt.stmt, 0);
    const int v_len = sqlite3_column_bytes(stmt.stmt, 1);
    if (k_ptr != nullptr && v_ptr != nullptr) {
      apply_pair(std::string_view(reinterpret_cast<const char*>(k_ptr), static_cast<std::size_t>(k_len)),
                 std::string_view(reinterpret_cast<const char*>(v_ptr), static_cast<std::size_t>(v_len)));
    }
  }
  return settings;
}

bool SaveTraySettings(const std::string& path, const TraySettings& settings) {
  if (path.empty()) {
    return false;
  }

  if (detail::IsLegacyTsvPath(path)) {
    const std::filesystem::path target(path);
    if (target.has_parent_path()) {
      std::error_code dir_error;
      std::filesystem::create_directories(target.parent_path(), dir_error);
    }
    const std::filesystem::path temp =
        target.parent_path() / (target.filename().string() + ".tmp");
    {
      std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
      if (!stream) {
        return false;
      }
      stream << kSettingTrayEnabled << '\t' << FlagText(settings.tray_enabled) << '\n'
             << kSettingLightweight << '\t' << FlagText(settings.lightweight) << '\n'
             << kSettingThemeMode << '\t' << ThemeModeKey(settings.theme_mode) << '\n'
             << kSettingThemePreset << '\t' << ThemePresetKey(settings.theme_preset) << '\n'
             << kSettingUiLanguage << '\t' << LanguageModeKey(settings.language) << '\n'
             << kSettingMcpEnabled << '\t' << FlagText(settings.mcp_enabled) << '\n'
             << kSettingMcpHost << '\t' << NormalizeMcpHost(settings.mcp_host) << '\n'
             << kSettingMcpPort << '\t' << settings.mcp_port << '\n'
             << kSettingMcpReadOnly << '\t' << FlagText(settings.mcp_read_only) << '\n'
             << kSettingDefaultPageSize << '\t'
             << std::clamp(settings.default_page_size, kMinPageSize, kMaxPageSize) << '\n'
             << kSettingRedisViewMode << '\t' << RedisViewModeKey(settings.redis_view_mode) << '\n';
      stream.flush();
      if (!stream) {
        std::error_code cleanup_error;
        std::filesystem::remove(temp, cleanup_error);
        return false;
      }
    }
    std::error_code error;
    std::filesystem::rename(temp, target, error);
    if (error) {
      std::error_code remove_error;
      std::filesystem::remove(target, remove_error);
      error.clear();
      std::filesystem::rename(temp, target, error);
      if (error) {
        std::error_code cleanup_error;
        std::filesystem::remove(temp, cleanup_error);
        return false;
      }
    }
    return true;
  }

  if (!detail::EnsureConfigSqliteSync(path)) {
    return false;
  }
  SqliteGuard conn;
  if (sqlite3_open_v2(path.c_str(), &conn.db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK ||
      conn.db == nullptr) {
    return false;
  }
  sqlite3_busy_timeout(conn.db, 5000);
  if (!ExecSql(conn.db, "BEGIN IMMEDIATE;")) {
    return false;
  }

  const std::pair<std::string_view, std::string> entries[] = {
      {kSettingTrayEnabled, std::string(FlagText(settings.tray_enabled))},
      {kSettingLightweight, std::string(FlagText(settings.lightweight))},
      {kSettingThemeMode, std::string(ThemeModeKey(settings.theme_mode))},
      {kSettingThemePreset, std::string(ThemePresetKey(settings.theme_preset))},
      {kSettingUiLanguage, std::string(LanguageModeKey(settings.language))},
      {kSettingMcpEnabled, std::string(FlagText(settings.mcp_enabled))},
      {kSettingMcpHost, NormalizeMcpHost(settings.mcp_host)},
      {kSettingMcpPort, std::to_string(settings.mcp_port)},
      {kSettingMcpReadOnly, std::string(FlagText(settings.mcp_read_only))},
      {kSettingDefaultPageSize,
       std::to_string(std::clamp(settings.default_page_size, kMinPageSize, kMaxPageSize))},
      {kSettingRedisViewMode, std::string(RedisViewModeKey(settings.redis_view_mode))},
  };

  StmtGuard upsert;
  if (sqlite3_prepare_v2(
          conn.db,
          "INSERT INTO settings (key, value) VALUES (?, ?) "
          "ON CONFLICT(key) DO UPDATE SET value = excluded.value;",
          -1, &upsert.stmt, nullptr) != SQLITE_OK) {
    ExecSql(conn.db, "ROLLBACK;");
    return false;
  }
  for (const auto& [k, v] : entries) {
    sqlite3_reset(upsert.stmt);
    sqlite3_clear_bindings(upsert.stmt);
    sqlite3_bind_text(upsert.stmt, 1, k.data(), static_cast<int>(k.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(upsert.stmt, 2, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
    if (sqlite3_step(upsert.stmt) != SQLITE_DONE) {
      ExecSql(conn.db, "ROLLBACK;");
      return false;
    }
  }

  if (!ExecSql(conn.db, "COMMIT;")) {
    ExecSql(conn.db, "ROLLBACK;");
    return false;
  }
  detail::RestrictConfigFilePermissions(path);
  return true;
}

void SettingsModel::Load() {
  const TraySettings settings = LoadTraySettings(path);
  tray_enabled = settings.tray_enabled;
  lightweight = settings.lightweight;
  theme_mode = settings.theme_mode;
  theme_preset = settings.theme_preset;
  language = settings.language;
  mcp_enabled = settings.mcp_enabled;
  mcp_host = NormalizeMcpHost(settings.mcp_host);
  mcp_port = settings.mcp_port;
  mcp_read_only = settings.mcp_read_only;
  default_page_size = settings.default_page_size;
  redis_view_mode = settings.redis_view_mode;
}

TraySettings SettingsModel::Snapshot() const {
  return TraySettings{
      .tray_enabled = tray_enabled.Get(),
      .lightweight = lightweight.Get(),
      .theme_mode = theme_mode.Get(),
      .theme_preset = theme_preset.Get(),
      .language = language.Get(),
      .mcp_enabled = mcp_enabled.Get(),
      .mcp_host = NormalizeMcpHost(mcp_host.Get()),
      .mcp_port = mcp_port.Get(),
      .mcp_read_only = mcp_read_only.Get(),
      .default_page_size = default_page_size.Get(),
      .redis_view_mode = redis_view_mode.Get(),
  };
}

AppThemeConfig SettingsModel::ThemeConfig() const {
  return AppThemeConfig{
      .mode = theme_mode.Get(),
      .preset = theme_preset.Get(),
  };
}

bool SettingsModel::SetTrayEnabled(bool enabled) {
  tray_enabled = enabled;
  if (persist_setting) {
    persist_setting(std::string(kSettingTrayEnabled), std::string(FlagText(enabled)));
    return true;
  }
  return SaveTraySettings(path, Snapshot());
}

bool SettingsModel::SetLightweight(bool value) {
  lightweight = value;
  if (persist_setting) {
    persist_setting(std::string(kSettingLightweight), std::string(FlagText(value)));
    return true;
  }
  return SaveTraySettings(path, Snapshot());
}

bool SettingsModel::SetThemeMode(ThemeMode mode) {
  theme_mode = mode;
  if (persist_setting) {
    persist_setting(std::string(kSettingThemeMode), std::string(ThemeModeKey(mode)));
    return true;
  }
  return SaveTraySettings(path, Snapshot());
}

bool SettingsModel::SetThemePreset(ThemePreset preset) {
  theme_preset = preset;
  if (persist_setting) {
    persist_setting(std::string(kSettingThemePreset), std::string(ThemePresetKey(preset)));
    return true;
  }
  return SaveTraySettings(path, Snapshot());
}

bool SettingsModel::SetLanguage(LanguageMode mode) {
  language = mode;
  if (persist_setting) {
    persist_setting(std::string(kSettingUiLanguage), std::string(LanguageModeKey(mode)));
    return true;
  }
  return SaveTraySettings(path, Snapshot());
}

bool SettingsModel::SetMcpEnabled(bool enabled) {
  mcp_enabled = enabled;
  if (persist_setting) {
    persist_setting(std::string(kSettingMcpEnabled), std::string(FlagText(enabled)));
    return true;
  }
  return SaveTraySettings(path, Snapshot());
}

bool SettingsModel::SetMcpHost(std::string host) {
  const std::string normalized = NormalizeMcpHost(host);
  mcp_host = normalized;
  if (persist_setting) {
    persist_setting(std::string(kSettingMcpHost), normalized);
    return true;
  }
  return SaveTraySettings(path, Snapshot());
}

bool SettingsModel::SetMcpPort(std::uint16_t port) {
  const std::uint16_t effective = port == 0 ? kDefaultMcpPort : port;
  mcp_port = effective;
  if (persist_setting) {
    persist_setting(std::string(kSettingMcpPort), std::to_string(effective));
    return true;
  }
  return SaveTraySettings(path, Snapshot());
}

bool SettingsModel::SetMcpReadOnly(bool read_only) {
  mcp_read_only = read_only;
  if (persist_setting) {
    persist_setting(std::string(kSettingMcpReadOnly), std::string(FlagText(read_only)));
    return true;
  }
  return SaveTraySettings(path, Snapshot());
}

bool SettingsModel::SetDefaultPageSize(std::int64_t page_size) {
  const std::int64_t effective = std::clamp(page_size, kMinPageSize, kMaxPageSize);
  default_page_size = effective;
  if (persist_setting) {
    persist_setting(std::string(kSettingDefaultPageSize), std::to_string(effective));
    return true;
  }
  return SaveTraySettings(path, Snapshot());
}

bool SettingsModel::SetRedisViewMode(RedisBrowserViewMode mode) {
  redis_view_mode = mode;
  if (persist_setting) {
    persist_setting(std::string(kSettingRedisViewMode), std::string(RedisViewModeKey(mode)));
    return true;
  }
  return SaveTraySettings(path, Snapshot());
}

} // namespace dbpp
