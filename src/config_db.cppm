module;

#include <huxerui/sqlite.h>
#include <huxerui/task.h>

export module dbpp.config_db;

import std;
import dbpp.settings;
import dbpp.types;
import dbpp.db;

export namespace dbpp {

struct SettingRow {
  std::string key;
  std::string value;
};

struct ConnectionRow {
  std::int64_t id = 0;
  std::int64_t sort_order = 0;
  std::string name;
  std::string driver_path;
  std::string uri;
  std::string username;
  std::string password;
  std::string remark;
  bool read_only = false;
  bool production_mode = false;
  bool show_system_databases = false;
};

struct SavedQueryRow {
  std::int64_t id = 0;
  std::int64_t sort_order = 0;
  std::string connection_key;
  std::string database_name;
  std::string name;
  std::string sql_text;
};

[[nodiscard]] const huxerui::sqlite::Table<SettingRow>& SettingsTable();
[[nodiscard]] const huxerui::sqlite::Table<ConnectionRow>& ConnectionsTable();
[[nodiscard]] const huxerui::sqlite::Table<SavedQueryRow>& SavedQueriesTable();
[[nodiscard]] const huxerui::sqlite::Schema& ConfigSchema();
[[nodiscard]] const huxerui::sqlite::Migrations& ConfigMigrations();
[[nodiscard]] huxerui::sqlite::OpenOptions ConfigOpenOptions();

/// Manages asynchronous HuxerUI::SQLite persistence for `<data_dir>/dbpp.sqlite3`
/// in the desktop GUI runtime, while sharing the exact schema and WAL settings
/// with synchronous CLI helpers in db.cpp / settings.cpp.
class ConfigDbService final : public std::enable_shared_from_this<ConfigDbService> {
public:
  [[nodiscard]] static TraySettings ReadStartupSettingsSync(const std::string& db_path);

  huxerui::Task<bool> OpenAndMigrateAsync(std::string db_path, huxerui::TaskScope tasks);
  [[nodiscard]] huxerui::Task<std::vector<ConnectionProfile>> LoadConnectionsAsync() const;
  [[nodiscard]] huxerui::Task<std::vector<SavedQuery>> LoadSavedQueriesAsync() const;

  void PersistSetting(std::string key, std::string value);
  void PersistConnections(std::vector<ConnectionProfile> profiles);
  void PersistSavedQueries(std::vector<SavedQuery> queries);

private:
  void ScheduleSettingWrite(std::string key, std::string value);
  void ScheduleConnectionsWrite(std::vector<ConnectionProfile> profiles);
  void ScheduleSavedQueriesWrite(std::vector<SavedQuery> queries);
  void ApplyRestrictedFilePermissions() const;

  std::optional<huxerui::sqlite::Database> database_;
  std::string db_path_;
  huxerui::TaskScope tasks_;
  bool has_tasks_ = false;
  bool opening_ = false;
  std::deque<std::pair<std::string, std::string>> pending_settings_;
  std::optional<std::vector<ConnectionProfile>> pending_connections_;
  std::optional<std::vector<SavedQuery>> pending_saved_queries_;
};

} // namespace dbpp
