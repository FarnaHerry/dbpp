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

struct ForeignKeyMeta {
  std::string constraint_name;
  std::vector<std::string> columns;
  std::string fk_catalog;
  std::string fk_db_schema;
  std::string fk_table;
  std::vector<std::string> fk_columns;

  bool operator==(const ForeignKeyMeta&) const = default;
};

struct TableConstraintMeta {
  std::string name;
  std::string type;
  std::vector<std::string> columns;

  bool operator==(const TableConstraintMeta&) const = default;
};

struct IndexMeta {
  std::string name;
  bool unique = false;
  bool primary = false;
  std::vector<std::string> columns;
  std::string definition;

  bool operator==(const IndexMeta&) const = default;
};

struct TriggerMeta {
  std::string name;
  std::string catalog;
  std::string db_schema;
  std::string table_name;
  std::string timing;
  std::string event;
  std::string definition;

  bool operator==(const TriggerMeta&) const = default;
};

struct RoutineMeta {
  std::string name;
  std::string catalog;
  std::string db_schema;
  bool is_function = false;
  std::string return_type;
  std::string signature;
  std::string definition;

  bool operator==(const RoutineMeta&) const = default;
};

struct EventMeta {
  std::string name;
  std::string catalog;
  std::string db_schema;
  std::string schedule;
  std::string status;
  std::string definition;

  bool operator==(const EventMeta&) const = default;
};

struct SavedQuery {
  std::int64_t id = 0;
  std::string connection_key;
  std::string database_name;
  std::string name;
  std::string sql_text;

  bool operator==(const SavedQuery&) const = default;
};

struct ColumnMeta {
  std::string name;
  std::string type;
  bool nullable = true;
  bool primary_key = false;
  int primary_key_order = 0;
  std::string default_expr;
  std::string remarks;
  std::optional<std::int32_t> column_size;
  std::optional<std::int16_t> decimal_digits;
  bool unique = false;
  bool autoincrement = false;
  bool generated = false;
  std::string extension_name;

  [[nodiscard]] bool NotNull() const { return !nullable; }
  bool operator==(const ColumnMeta&) const = default;
};

struct AddColumnSpec {
  std::string name;
  std::string type;
  bool nullable = true;
  std::string default_expr;
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
  std::vector<ForeignKeyMeta> foreign_keys;
  std::vector<TableConstraintMeta> constraints;
  std::vector<IndexMeta> indexes;
};

struct ConnectionRequest {
  std::string driver_path;
  std::string uri;
  std::string fallback_uri;
  std::string username;
  std::string password;
  std::string title;
  std::string remark;
  bool read_only = false;
  bool production_mode = false;
  bool show_system_databases = false;
};

/// User-saved ADBC/Redis connection (persisted in `dbpp.sqlite3`, including optional password).
struct ConnectionProfile {
  std::string name;
  std::string driver_path;
  std::string uri;
  std::string username;
  std::string password;
  std::string remark;
  bool read_only = false;
  bool production_mode = false;
  bool show_system_databases = false;

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
  std::string display_text;

  [[nodiscard]] const std::string& DisplayText() const noexcept {
    return display_text.empty() ? text : display_text;
  }
};

struct GridRow {
  std::vector<CellValue> values;
  std::vector<CellValue> key_values;
  std::optional<std::int64_t> rowid;
  std::string row_locator_token;
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
  bool connection_lost = false;
  std::string error;
};

enum class RowLocatorKind {
  None,
  PrimaryKey,
  UniqueKey,
  HiddenRowId,
  AllColumns,
};

struct TableModel {
  std::uint64_t session_id = 0;
  std::string object;
  std::string qualified_name;
  bool is_view = false;
  bool editable = false;
  bool read_only = false;
  bool uses_rowid = false;
  RowLocatorKind row_locator = RowLocatorKind::None;
  std::string hidden_rowid_expr;
  std::string hidden_rowid_where_col;
  std::string hidden_rowid_cast;
  bool hidden_rowid_is_numeric = true;
  std::vector<ColumnMeta> columns;
  std::vector<std::size_t> key_columns;
  std::shared_ptr<const GridSnapshot> snapshot;
  std::int64_t total = 0;
  std::int64_t page = 0;
  std::int64_t page_size = kPageSize;
  std::string filter;
  std::optional<huxerui::DataTableSort> sort;
  bool loading = false;
  bool connection_lost = false;
  std::string error;
  std::string message;
  std::string executed_sql;
  std::int64_t elapsed_us = -1;
  std::vector<PendingInsertRow> pending_inserts;
  std::uint64_t next_draft_id = 1;
  bool committing = false;
  std::unordered_map<std::string, float> column_widths;
  std::uint64_t snapshot_version = 0;
};

struct QueryResult {
  std::uint64_t id = 0;
  std::vector<std::string> columns;
  std::shared_ptr<const GridSnapshot> snapshot;
  bool truncated = false;
};

struct SqlModel {
  huxerui::TextEditingValue editor;
  std::string target_database;
  std::int64_t page_size = 0;
  std::int64_t page = 0;
  bool paginated = false;
  bool has_next_page = false;
  std::string last_sql;
  std::string last_database;
  std::string last_executed_sql;
  std::int64_t last_elapsed_us = -1;
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
  Settings,
  TableDesign,
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

  DbObject design_object;
  std::string design_display_label;

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

  [[nodiscard]] const WorkspaceTab* FindTableDesignTab(std::uint64_t session_id,
                                                       std::string_view object_name,
                                                       std::string_view object_schema) const {
    for (const auto& tab : tabs) {
      if (tab.kind == WorkspaceTabKind::TableDesign && tab.session_id == session_id &&
          tab.object_name == object_name && tab.object_schema == object_schema) {
        return &tab;
      }
    }
    return nullptr;
  }

  [[nodiscard]] WorkspaceTab* FindTableDesignTab(std::uint64_t session_id,
                                                 std::string_view object_name,
                                                 std::string_view object_schema) {
    for (auto& tab : tabs) {
      if (tab.kind == WorkspaceTabKind::TableDesign && tab.session_id == session_id &&
          tab.object_name == object_name && tab.object_schema == object_schema) {
        return &tab;
      }
    }
    return nullptr;
  }

  [[nodiscard]] const WorkspaceTab* FindSettingsTab() const {
    for (const auto& tab : tabs) {
      if (tab.kind == WorkspaceTabKind::Settings) {
        return &tab;
      }
    }
    return nullptr;
  }

  [[nodiscard]] WorkspaceTab* FindSettingsTab() {
    for (auto& tab : tabs) {
      if (tab.kind == WorkspaceTabKind::Settings) {
        return &tab;
      }
    }
    return nullptr;
  }
};

struct AdbcConnectionInfo {
  std::string vendor_name;
  std::string vendor_version;
  std::string vendor_arrow_version;
  std::optional<bool> vendor_sql;
  std::optional<bool> vendor_substrait;
  std::string driver_name;
  std::string driver_version;
  std::string driver_arrow_version;
  std::optional<std::int64_t> driver_adbc_version;
  bool supported = false;
};

struct ColumnStatistic {
  std::string column_name;
  std::string statistic_name;
  std::int16_t statistic_key = 0;
  std::string value_text;
  bool approximate = false;
};

struct TableStatisticsResult {
  std::optional<std::int64_t> row_count;
  bool row_count_approximate = false;
  std::vector<ColumnStatistic> statistics;
  bool supported = false;
  bool connection_lost = false;
  std::string error;
};

struct QueryParameterMeta {
  std::string name;
  std::string type;
  bool nullable = true;
  std::size_t ordinal = 0;
};

struct PreparedParamValue {
  std::string text;
  bool is_null = false;
};

enum class ExportFormat {
  Csv,
  Jsonl,
};

enum class IngestMode {
  Create,
  Append,
  Replace,
  CreateAppend,
};

struct BulkIngestResult {
  std::int64_t rows_affected = -1;
  bool connection_lost = false;
  std::string error;
};

struct AdbcObjectsResult {
  std::vector<DbObject> objects;
  std::vector<std::string> databases;
  std::vector<TriggerMeta> triggers;
  std::vector<RoutineMeta> routines;
  std::vector<EventMeta> events;
  bool connection_lost = false;
  std::string error;
  std::string sqlstate;
};

struct AdbcSchemaResult {
  std::vector<ColumnMeta> columns;
  bool connection_lost = false;
  std::string error;
  std::string sqlstate;
  std::vector<std::pair<std::string, std::string>> error_details;
};

struct AdbcQueryResult {
  std::vector<std::string> columns;
  std::vector<GridRow> rows;
  std::int64_t affected = -1;
  std::int64_t elapsed_us = -1;
  bool truncated = false;
  bool connection_lost = false;
  std::string error;
  std::string sqlstate;
  std::int32_t vendor_code = 0;
  bool cancelled = false;
  std::vector<std::pair<std::string, std::string>> error_details;
};

} // namespace dbpp
