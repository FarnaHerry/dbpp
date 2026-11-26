module;

#include <huxerui/datatable.h>
#include <huxerui/huxerui.h>

export module dbpp.types;

import std;
import dbpp.settings;

export namespace dbpp {

using NotifyFn = std::function<void(std::string)>;

inline constexpr std::int64_t kPageSize = 100;
inline constexpr std::size_t kMaxQueryRows = 5000;
inline constexpr float kMinColumnWidth = 56.0F;
inline constexpr float kMaxColumnWidth = 1600.0F;
inline constexpr float kAutoMinColumnWidth = 76.0F;
inline constexpr float kAutoMaxColumnWidth = 360.0F;

/// SQL dialect used only for identifier quoting and pagination syntax.
enum class Dialect {
  Sqlite,
  Postgresql,
  Mysql,
  Duckdb,
  Redis,
  Generic,
};

/// Database brand used to select per-database vector logos and branding.
enum class DatabaseBrand {
  Sqlite,
  Postgresql,
  Mysql,
  Mariadb,
  Duckdb,
  Redis,
  Mssql,
  Clickhouse,
  FlightSql,
  Snowflake,
  BigQuery,
  Trino,
  Databricks,
  Redshift,
  Custom,
};

struct ColumnMeta {
  std::string name;
  std::string type;
  bool nullable = true;
  bool primary_key = false;
  int primary_key_order = 0;

  [[nodiscard]] bool NotNull() const { return !nullable; }
  bool operator==(const ColumnMeta&) const = default;
};

struct DbObject {
  std::string name;
  std::string catalog;
  std::string db_schema;
  std::string table_type;
  bool is_view = false;
  std::optional<std::int64_t> item_count;
  std::vector<std::string> primary_key;
  std::vector<ColumnMeta> columns;
};

struct ConnectionRequest {
  std::string driver_path;
  std::string uri;
  std::string username;
  std::string password;
  std::string title;
};

/// User-saved ADBC/Redis connection (persisted in `dbpp.sqlite3`, including optional password).
struct ConnectionProfile {
  std::string name;
  std::string driver_path;
  std::string uri;
  std::string username;
  std::string password;

  bool operator==(const ConnectionProfile&) const = default;
};

struct AdbcDriverInfo {
  std::string name;
  std::string path;
  std::string detail;
  std::string version;
  bool is_manifest = false;

  bool operator==(const AdbcDriverInfo&) const = default;
};

enum class DatabaseCategory {
  LocalFile,
  Relational,
  KeyValue,
  AnalyticsCloud,
  Custom,
};

enum class ConnectionFormKind {
  File,
  HostPort,
  Uri,
  Custom,
};

/// Metadata and defaults for a supported database type presented in the
/// two-step "New Connection" wizard and unified edit dialog.
struct DatabasePreset {
  std::string id;
  std::string name;
  std::string subtitle;
  std::string driver_slug;
  DatabaseCategory category = DatabaseCategory::Relational;
  ConnectionFormKind form_kind = ConnectionFormKind::Uri;
  std::string uri_scheme;
  std::string default_host;
  std::string default_port;
  std::string default_database;
  std::string default_user;
  std::string uri_placeholder;
  std::vector<std::string> file_extensions;
  bool builtin = false;
};

struct DecomposedPresetUri {
  std::string host;
  std::string port;
  std::string database;
  std::string username;
  std::string password;
  std::string extra_query;
  bool parsed = false;
};

struct SanitizedUriResult {
  std::string uri;
  std::string username;
  std::string password;
  bool stripped_password = false;
};

struct CellValue {
  std::string text;
  bool is_null = false;
};

struct GridRow {
  std::vector<CellValue> values;
  std::vector<CellValue> key_values;
  std::optional<std::int64_t> rowid;
};

struct GridSnapshot {
  std::vector<GridRow> rows;
};

struct PendingInsertRow {
  std::uint64_t draft_id = 0;
  std::vector<std::string> values;

  bool operator==(const PendingInsertRow&) const = default;
};

struct BatchInsertResult {
  std::size_t inserted = 0;
  bool rolled_back = false;
  std::string error;
};

struct TableModel {
  std::uint64_t session_id = 0;
  std::string object;
  std::string qualified_name;
  bool is_view = false;
  bool editable = false;
  bool read_only = false;
  bool uses_rowid = false;
  std::vector<ColumnMeta> columns;
  std::vector<std::size_t> key_columns;
  std::shared_ptr<const GridSnapshot> snapshot;
  std::int64_t total = 0;
  std::int64_t page = 0;
  std::int64_t page_size = kPageSize;
  std::string filter;
  std::optional<huxerui::DataTableSort> sort;
  bool loading = false;
  std::string error;
  std::string message;
  std::vector<PendingInsertRow> pending_inserts;
  std::uint64_t next_draft_id = 1;
  bool committing = false;
  std::unordered_map<std::string, float> column_widths;
};

struct QueryResult {
  std::uint64_t id = 0;
  std::vector<std::string> columns;
  std::shared_ptr<const GridSnapshot> snapshot;
  bool truncated = false;
};

struct SqlModel {
  huxerui::TextEditingValue editor;
  std::shared_ptr<const QueryResult> result;
  std::unordered_map<std::string, float> column_widths;
  std::uint64_t next_result_id = 1;
  bool running = false;
  std::string status;
  std::string error;
};

enum class WorkspaceTabKind {
  Table,
  Sql,
};

struct WorkspaceTab {
  std::uint64_t id = 0;
  WorkspaceTabKind kind = WorkspaceTabKind::Table;
  std::uint64_t session_id = 0;
  std::string session_name;
  std::string session_uri;
  std::string title;

  std::string object_name;
  std::string object_schema;
  bool is_view = false;
  TableModel table;
  std::optional<huxerui::DataTableCellId> selected_cell;
  huxerui::TextEditingValue filter_input;

  SqlModel sql;
};

struct WorkspaceModel {
  std::vector<WorkspaceTab> tabs;
  std::uint64_t active_tab_id = 0;
  std::uint64_t next_tab_id = 1;
  std::uint64_t next_sql_index = 1;

  [[nodiscard]] const WorkspaceTab* FindTab(std::uint64_t id) const {
    for (const auto& tab : tabs) {
      if (tab.id == id) {
        return &tab;
      }
    }
    return nullptr;
  }

  [[nodiscard]] WorkspaceTab* FindTab(std::uint64_t id) {
    for (auto& tab : tabs) {
      if (tab.id == id) {
        return &tab;
      }
    }
    return nullptr;
  }

  [[nodiscard]] const WorkspaceTab* ActiveTab() const { return FindTab(active_tab_id); }
  [[nodiscard]] WorkspaceTab* ActiveTab() { return FindTab(active_tab_id); }

  [[nodiscard]] const WorkspaceTab* FindTableTab(std::uint64_t session_id,
                                                 std::string_view object_name,
                                                 std::string_view object_schema,
                                                 bool is_view) const {
    for (const auto& tab : tabs) {
      if (tab.kind == WorkspaceTabKind::Table && tab.session_id == session_id &&
          tab.object_name == object_name && tab.object_schema == object_schema &&
          tab.is_view == is_view) {
        return &tab;
      }
    }
    return nullptr;
  }

  [[nodiscard]] WorkspaceTab* FindTableTab(std::uint64_t session_id,
                                           std::string_view object_name,
                                           std::string_view object_schema, bool is_view) {
    for (auto& tab : tabs) {
      if (tab.kind == WorkspaceTabKind::Table && tab.session_id == session_id &&
          tab.object_name == object_name && tab.object_schema == object_schema &&
          tab.is_view == is_view) {
        return &tab;
      }
    }
    return nullptr;
  }
};

struct AdbcObjectsResult {
  std::vector<DbObject> objects;
  std::string error;
};

struct AdbcSchemaResult {
  std::vector<ColumnMeta> columns;
  std::string error;
};

struct AdbcQueryResult {
  std::vector<std::string> columns;
  std::vector<GridRow> rows;
  std::int64_t affected = -1;
  bool truncated = false;
  std::string error;
};

} // namespace dbpp
