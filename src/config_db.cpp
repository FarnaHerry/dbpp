module;

#include <huxerui/sqlite.h>
#include <huxerui/task.h>
#include <cstdio>

module dbpp.config_db;

import std;
import dbpp.settings;
import dbpp.types;
import dbpp.db;

namespace dbpp {

namespace sqlite = huxerui::sqlite;

namespace {

const sqlite::Table<SettingRow>& BuildSettingsTable() {
  static const sqlite::Table<SettingRow> table{
      "settings",
      sqlite::Column<&SettingRow::key>{"key", sqlite::PrimaryKey{}},
      sqlite::Column<&SettingRow::value>{"value"},
  };
  return table;
}

const sqlite::Table<ConnectionRow>& BuildConnectionsTable() {
  static const sqlite::Table<ConnectionRow> table{
      "connections",
      sqlite::Column<&ConnectionRow::id>{"id", sqlite::PrimaryKey{}, sqlite::AutoIncrement{}},
      sqlite::Column<&ConnectionRow::sort_order>{"sort_order"},
      sqlite::Column<&ConnectionRow::name>{"name"},
      sqlite::Column<&ConnectionRow::driver_path>{"driver_path"},
      sqlite::Column<&ConnectionRow::uri>{"uri"},
      sqlite::Column<&ConnectionRow::username>{"username"},
      sqlite::Column<&ConnectionRow::password>{"password"},
      sqlite::Column<&ConnectionRow::remark>{"remark"},
      sqlite::Column<&ConnectionRow::read_only>{"read_only"},
      sqlite::Column<&ConnectionRow::production_mode>{"production_mode"},
      sqlite::Column<&ConnectionRow::show_system_databases>{"show_system_databases"},
      sqlite::Index<&ConnectionRow::sort_order, &ConnectionRow::id>{"idx_connections_sort_order"},
  };
  return table;
}

const sqlite::Table<SavedQueryRow>& BuildSavedQueriesTable() {
  static const sqlite::Table<SavedQueryRow> table{
      "saved_queries",
      sqlite::Column<&SavedQueryRow::id>{"id", sqlite::PrimaryKey{}, sqlite::AutoIncrement{}},
      sqlite::Column<&SavedQueryRow::sort_order>{"sort_order"},
      sqlite::Column<&SavedQueryRow::connection_key>{"connection_key"},
      sqlite::Column<&SavedQueryRow::database_name>{"database_name"},
      sqlite::Column<&SavedQueryRow::name>{"name"},
      sqlite::Column<&SavedQueryRow::sql_text>{"sql_text"},
      sqlite::Index<&SavedQueryRow::sort_order, &SavedQueryRow::id>{
          "idx_saved_queries_sort_order"},
  };
  return table;
}

const sqlite::Schema& BuildConfigSchema() {
  static const sqlite::Schema schema{
      2, BuildSettingsTable(), BuildConnectionsTable(), BuildSavedQueriesTable()};
  return schema;
}

const sqlite::Migrations& BuildConfigMigrations() {
  static const sqlite::Migrations migrations{
      sqlite::Migration{
          1,
          2,
          [](sqlite::MigrationContext& migration) -> sqlite::Result<void> {
            for (const char* stmt : {
                     "ALTER TABLE connections ADD COLUMN remark TEXT NOT NULL DEFAULT '';",
                     "ALTER TABLE connections ADD COLUMN read_only INTEGER NOT NULL DEFAULT 0;",
                     "ALTER TABLE connections ADD COLUMN production_mode INTEGER NOT NULL DEFAULT 0;",
                     "ALTER TABLE connections ADD COLUMN show_system_databases INTEGER NOT NULL DEFAULT 0;",
                     "CREATE TABLE IF NOT EXISTS saved_queries ("
                     "id INTEGER PRIMARY KEY AUTOINCREMENT, "
                     "sort_order INTEGER NOT NULL DEFAULT 0, "
                     "connection_key TEXT NOT NULL DEFAULT '', "
                     "database_name TEXT NOT NULL DEFAULT '', "
                     "name TEXT NOT NULL DEFAULT '', "
                     "sql_text TEXT NOT NULL DEFAULT ''"
                     ");",
                     "CREATE INDEX IF NOT EXISTS idx_saved_queries_sort_order "
                     "ON saved_queries (sort_order, id);",
                 }) {
              auto res = migration.Execute(stmt);
              if (!res) {
                return res.Error();
              }
            }
            return {};
          }},
  };
  return migrations;
}

void ApplySettingPair(std::string_view key, std::string_view value, TraySettings& out) {
  if (key == kSettingTrayEnabled) {
    out.tray_enabled = (value == "true");
  } else if (key == kSettingLightweight) {
    out.lightweight = (value == "true");
  } else if (key == kSettingThemeMode) {
    out.theme_mode = ThemeModeFromKey(value);
  } else if (key == kSettingThemePreset) {
    out.theme_preset = ThemePresetFromKey(value);
  } else if (key == kSettingUiLanguage) {
    out.language = LanguageModeFromKey(value);
  } else if (key == kSettingMcpEnabled) {
    out.mcp_enabled = (value == "true");
  } else if (key == kSettingMcpHost) {
    out.mcp_host = NormalizeMcpHost(value);
  } else if (key == kSettingMcpPort) {
    out.mcp_port = ParseMcpPort(value);
  } else if (key == kSettingMcpReadOnly) {
    out.mcp_read_only = (value != "false");
  } else if (key == kSettingDefaultPageSize) {
    out.default_page_size = ParsePageSize(value);
  } else if (key == kSettingRedisViewMode) {
    out.redis_view_mode = RedisViewModeFromKey(value);
  }
}

} // namespace

const sqlite::Table<SettingRow>& SettingsTable() {
  return BuildSettingsTable();
}

const sqlite::Table<ConnectionRow>& ConnectionsTable() {
  return BuildConnectionsTable();
}

const sqlite::Table<SavedQueryRow>& SavedQueriesTable() {
  return BuildSavedQueriesTable();
}

const sqlite::Schema& ConfigSchema() {
  return BuildConfigSchema();
}

const sqlite::Migrations& ConfigMigrations() {
  return BuildConfigMigrations();
}

sqlite::OpenOptions ConfigOpenOptions() {
  return sqlite::OpenOptions{
      .mode = sqlite::OpenMode::ReadWriteCreate,
      .journal_mode = sqlite::JournalMode::Wal,
      .busy_timeout = std::chrono::seconds{5},
      .create_parent_directories = true,
  };
}

TraySettings ConfigDbService::ReadStartupSettingsSync(const std::string& db_path) {
  std::error_code ec;
  const std::filesystem::path target(db_path);
  if (!std::filesystem::exists(target, ec)) {
    const std::filesystem::path legacy_tsv = target.parent_path() / "settings.tsv";
    if (std::filesystem::is_regular_file(legacy_tsv, ec)) {
      return LoadTraySettings(legacy_tsv.string());
    }
    return {};
  }

  TraySettings result;
  bool has_settings_table = false;
  auto inspected = sqlite::Database::QueryReadOnlySync(
      huxerui::File{db_path},
      "SELECT name FROM sqlite_master WHERE type='table' AND name='settings'",
      [&has_settings_table](const sqlite::RowView&) -> sqlite::Result<void> {
        has_settings_table = true;
        return {};
      });
  if (!inspected) {
    std::fprintf(stderr, "db++: 读取配置表信息失败：%s\n", inspected.Error().Message().c_str());
    return result;
  }
  if (!has_settings_table) {
    const std::filesystem::path legacy_tsv = target.parent_path() / "settings.tsv";
    if (std::filesystem::is_regular_file(legacy_tsv, ec)) {
      return LoadTraySettings(legacy_tsv.string());
    }
    return result;
  }

  auto read = sqlite::Database::QueryReadOnlySync(
      huxerui::File{db_path},
      "SELECT key, value FROM settings WHERE key IN "
      "('tray.enabled', 'tray.lightweight', 'theme.mode', 'theme.preset', 'ui.language', "
      "'mcp.enabled', 'mcp.host', 'mcp.port', 'mcp.read_only', 'query.page_size', "
      "'redis.view_mode')",
      [&result](const sqlite::RowView& row) -> sqlite::Result<void> {
        auto key = row.Get<std::string>("key");
        if (!key) {
          return key.Error();
        }
        auto value = row.Get<std::string>("value");
        if (!value) {
          return value.Error();
        }
        ApplySettingPair(*key, *value, result);
        return {};
      });
  if (!read) {
    std::fprintf(stderr, "db++: 启动读取配置失败：%s\n", read.Error().Message().c_str());
  }
  return result;
}

huxerui::Task<bool> ConfigDbService::OpenAndMigrateAsync(std::string db_path,
                                                         huxerui::TaskScope tasks) {
  auto self = shared_from_this();
  db_path_ = std::move(db_path);
  tasks_ = std::move(tasks);
  has_tasks_ = true;
  if (database_.has_value()) {
    co_return true;
  }
  if (opening_) {
    co_return false;
  }
  opening_ = true;
  auto opened = co_await sqlite::Database::OpenAsync(
      huxerui::File{db_path_}, ConfigSchema(), ConfigMigrations(), ConfigOpenOptions());
  opening_ = false;
  if (!opened) {
    std::fprintf(stderr, "db++: 打开 SQLite 配置库失败：%s\n",
                 opened.Error().Message().c_str());
    co_return false;
  }
  database_ = *opened;
  ApplyRestrictedFilePermissions();

  const std::filesystem::path db_file(db_path_);
  const std::filesystem::path legacy_settings_path = db_file.parent_path() / "settings.tsv";
  const std::filesystem::path legacy_conns_path = db_file.parent_path() / "connections.tsv";
  const std::vector<std::pair<std::string, std::string>> legacy_settings =
      detail::LoadLegacySettingsTsvPairs(legacy_settings_path.string());
  std::vector<ConnectionProfile> legacy_conns;
  {
    std::error_code ec;
    if (std::filesystem::is_regular_file(legacy_conns_path, ec)) {
      legacy_conns = LoadConnectionProfiles(legacy_conns_path.string());
    }
  }

  auto migrated = co_await database_->TransactionAsync(
      [legacy_settings = std::move(legacy_settings),
       legacy_conns = std::move(legacy_conns)](sqlite::Transaction& tx) -> sqlite::Result<void> {
        const auto& settings_tbl = SettingsTable();
        const auto& conns_tbl = ConnectionsTable();

        auto marker = tx.Find(settings_tbl, std::string("meta.tsv_migrated"));
        if (!marker) {
          return marker.Error();
        }
        if (marker->has_value()) {
          return {};
        }

        for (const auto& [k, v] : legacy_settings) {
          auto existing = tx.Find(settings_tbl, k);
          if (!existing) {
            return existing.Error();
          }
          if (!existing->has_value()) {
            auto inserted = tx.Insert(settings_tbl, SettingRow{k, v});
            if (!inserted) {
              return inserted.Error();
            }
          }
        }

        if (!legacy_conns.empty()) {
          auto existing_count = tx.Select(conns_tbl).Count();
          if (!existing_count) {
            return existing_count.Error();
          }
          if (*existing_count == 0) {
            std::vector<ConnectionRow> rows;
            rows.reserve(legacy_conns.size());
            for (std::size_t i = 0; i < legacy_conns.size(); ++i) {
              rows.push_back(ConnectionRow{
                  .id = 0,
                  .sort_order = static_cast<std::int64_t>(i),
                  .name = legacy_conns[i].name,
                  .driver_path = legacy_conns[i].driver_path,
                  .uri = legacy_conns[i].uri,
                  .username = legacy_conns[i].username,
                  .password = legacy_conns[i].password,
              });
            }
            auto inserted = tx.InsertMany(conns_tbl, rows);
            if (!inserted) {
              return inserted.Error();
            }
          }
        }

        auto done_marker = tx.Insert(settings_tbl, SettingRow{"meta.tsv_migrated", "1"});
        if (!done_marker) {
          return done_marker.Error();
        }
        return {};
      });

  if (!migrated) {
    std::fprintf(stderr, "db++: 迁移旧版 TSV 配置失败：%s\n",
                 migrated.Error().Message().c_str());
  }
  ApplyRestrictedFilePermissions();

  while (!pending_settings_.empty()) {
    auto [key, value] = std::move(pending_settings_.front());
    pending_settings_.pop_front();
    ScheduleSettingWrite(std::move(key), std::move(value));
  }
  if (pending_connections_.has_value()) {
    std::vector<ConnectionProfile> queued = std::move(*pending_connections_);
    pending_connections_.reset();
    ScheduleConnectionsWrite(std::move(queued));
  }
  if (pending_saved_queries_.has_value()) {
    std::vector<SavedQuery> queued = std::move(*pending_saved_queries_);
    pending_saved_queries_.reset();
    ScheduleSavedQueriesWrite(std::move(queued));
  }

  (void)self;
  co_return true;
}

huxerui::Task<std::vector<ConnectionProfile>> ConfigDbService::LoadConnectionsAsync() const {
  if (!database_.has_value()) {
    co_return LoadConnectionProfiles(db_path_);
  }
  const auto& conns_tbl = ConnectionsTable();
  auto rows = co_await database_->Select(conns_tbl)
                  .OrderBy(conns_tbl.Column<&ConnectionRow::sort_order>(),
                           sqlite::SortDirection::Ascending)
                  .OrderBy(conns_tbl.Column<&ConnectionRow::id>(),
                           sqlite::SortDirection::Ascending)
                  .AllAsync();
  if (!rows) {
    std::fprintf(stderr, "db++: 读取 SQLite 连接列表失败：%s\n",
                 rows.Error().Message().c_str());
    co_return LoadConnectionProfiles(db_path_);
  }
  std::vector<ConnectionProfile> profiles;
  profiles.reserve(rows->size());
  for (ConnectionRow& row : *rows) {
    profiles.push_back(ConnectionProfile{
        .name = std::move(row.name),
        .driver_path = std::move(row.driver_path),
        .uri = std::move(row.uri),
        .username = std::move(row.username),
        .password = std::move(row.password),
        .remark = std::move(row.remark),
        .read_only = row.read_only,
        .production_mode = row.production_mode,
        .show_system_databases = row.show_system_databases,
    });
  }
  co_return profiles;
}

huxerui::Task<std::vector<SavedQuery>> ConfigDbService::LoadSavedQueriesAsync() const {
  if (!database_.has_value()) {
    co_return LoadSavedQueries(db_path_);
  }
  const auto& tbl = SavedQueriesTable();
  auto rows = co_await database_->Select(tbl)
                  .OrderBy(tbl.Column<&SavedQueryRow::sort_order>(),
                           sqlite::SortDirection::Ascending)
                  .OrderBy(tbl.Column<&SavedQueryRow::id>(), sqlite::SortDirection::Ascending)
                  .AllAsync();
  if (!rows) {
    std::fprintf(stderr, "db++: 读取已保存查询失败：%s\n", rows.Error().Message().c_str());
    co_return LoadSavedQueries(db_path_);
  }
  std::vector<SavedQuery> queries;
  queries.reserve(rows->size());
  for (SavedQueryRow& row : *rows) {
    queries.push_back(SavedQuery{
        .id = row.id,
        .connection_key = std::move(row.connection_key),
        .database_name = std::move(row.database_name),
        .name = std::move(row.name),
        .sql_text = std::move(row.sql_text),
    });
  }
  co_return queries;
}

void ConfigDbService::PersistSetting(std::string key, std::string value) {
  if (!database_.has_value()) {
    if (has_tasks_) {
      pending_settings_.emplace_back(std::move(key), std::move(value));
    }
    return;
  }
  ScheduleSettingWrite(std::move(key), std::move(value));
}

void ConfigDbService::PersistConnections(std::vector<ConnectionProfile> profiles) {
  if (!database_.has_value()) {
    if (has_tasks_) {
      pending_connections_ = std::move(profiles);
    } else if (!db_path_.empty()) {
      (void)SaveConnectionProfiles(db_path_, profiles);
    }
    return;
  }
  ScheduleConnectionsWrite(std::move(profiles));
}

void ConfigDbService::PersistSavedQueries(std::vector<SavedQuery> queries) {
  if (!database_.has_value()) {
    if (has_tasks_) {
      pending_saved_queries_ = std::move(queries);
    } else if (!db_path_.empty()) {
      (void)SaveSavedQueries(db_path_, queries);
    }
    return;
  }
  ScheduleSavedQueriesWrite(std::move(queries));
}

void ConfigDbService::ScheduleSettingWrite(std::string key, std::string value) {
  if (!database_.has_value() || !has_tasks_) {
    return;
  }
  auto db = *database_;
  auto weak = weak_from_this();
  tasks_.Launch([db, weak, key = std::move(key),
                 value = std::move(value)]() mutable -> huxerui::Task<void> {
    const auto& tbl = SettingsTable();
    SettingRow row{std::move(key), std::move(value)};
    auto res = co_await db.UpsertAsync(
        tbl, row, sqlite::OnConflict(tbl.Column<&SettingRow::key>()),
        sqlite::Set(tbl.Column<&SettingRow::value>(), row.value));
    if (!res) {
      std::fprintf(stderr, "db++: 保存设置失败：%s\n", res.Error().Message().c_str());
    }
    if (const auto locked = weak.lock()) {
      locked->ApplyRestrictedFilePermissions();
    }
  });
}

void ConfigDbService::ScheduleConnectionsWrite(std::vector<ConnectionProfile> profiles) {
  if (!database_.has_value() || !has_tasks_) {
    return;
  }
  auto db = *database_;
  auto weak = weak_from_this();
  tasks_.Launch([db, weak, profiles = std::move(profiles)]() mutable -> huxerui::Task<void> {
    auto res = co_await db.TransactionAsync(
        [&profiles](sqlite::Transaction& tx) -> sqlite::Result<void> {
          auto cleared = tx.Execute("DELETE FROM connections");
          if (!cleared) {
            return cleared.Error();
          }
          if (profiles.empty()) {
            return {};
          }
          std::vector<ConnectionRow> rows;
          rows.reserve(profiles.size());
          for (std::size_t i = 0; i < profiles.size(); ++i) {
            rows.push_back(ConnectionRow{
                .id = 0,
                .sort_order = static_cast<std::int64_t>(i),
                .name = profiles[i].name,
                .driver_path = profiles[i].driver_path,
                .uri = profiles[i].uri,
                .username = profiles[i].username,
                .password = profiles[i].password,
                .remark = profiles[i].remark,
                .read_only = profiles[i].read_only,
                .production_mode = profiles[i].production_mode,
                .show_system_databases = profiles[i].show_system_databases,
            });
          }
          auto inserted = tx.InsertMany(ConnectionsTable(), rows);
          if (!inserted) {
            return inserted.Error();
          }
          return {};
        });
    if (!res) {
      std::fprintf(stderr, "db++: 保存连接配置失败：%s\n", res.Error().Message().c_str());
    }
    if (const auto locked = weak.lock()) {
      locked->ApplyRestrictedFilePermissions();
    }
  });
}

void ConfigDbService::ScheduleSavedQueriesWrite(std::vector<SavedQuery> queries) {
  if (!database_.has_value() || !has_tasks_) {
    return;
  }
  auto db = *database_;
  auto weak = weak_from_this();
  tasks_.Launch([db, weak, queries = std::move(queries)]() mutable -> huxerui::Task<void> {
    auto res = co_await db.TransactionAsync(
        [&queries](sqlite::Transaction& tx) -> sqlite::Result<void> {
          auto cleared = tx.Execute("DELETE FROM saved_queries");
          if (!cleared) {
            return cleared.Error();
          }
          if (queries.empty()) {
            return {};
          }
          std::vector<SavedQueryRow> rows;
          rows.reserve(queries.size());
          for (std::size_t i = 0; i < queries.size(); ++i) {
            rows.push_back(SavedQueryRow{
                .id = 0,
                .sort_order = static_cast<std::int64_t>(i),
                .connection_key = queries[i].connection_key,
                .database_name = queries[i].database_name,
                .name = queries[i].name,
                .sql_text = queries[i].sql_text,
            });
          }
          auto inserted = tx.InsertMany(SavedQueriesTable(), rows);
          if (!inserted) {
            return inserted.Error();
          }
          return {};
        });
    if (!res) {
      std::fprintf(stderr, "db++: 保存查询列表失败：%s\n", res.Error().Message().c_str());
    }
    if (const auto locked = weak.lock()) {
      locked->ApplyRestrictedFilePermissions();
    }
  });
}

void ConfigDbService::ApplyRestrictedFilePermissions() const {
  if (!db_path_.empty()) {
    detail::RestrictConfigFilePermissions(db_path_);
  }
}

} // namespace dbpp
