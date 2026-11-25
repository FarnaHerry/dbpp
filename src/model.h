#pragma once

#include "settings.h"

#include <huxerui/datatable.h>
#include <huxerui/http.h>
#include <huxerui/huxerui.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace dbpp {

using NotifyFn = std::function<void(std::string)>;

constexpr std::int64_t kPageSize = 100;
constexpr std::size_t kMaxQueryRows = 5000;
constexpr float kMinColumnWidth = 56.0F;
constexpr float kMaxColumnWidth = 1600.0F;
constexpr float kAutoMinColumnWidth = 76.0F;
constexpr float kAutoMaxColumnWidth = 360.0F;

/// SQL dialect used only for identifier quoting and pagination syntax.
enum class Dialect {
  Sqlite,
  Postgresql,
  Mysql,
  Duckdb,
  Redis,
  Generic,
};

struct AdbcSession;

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

/// User-saved ADBC connection. Passwords are intentionally not persisted.
struct ConnectionProfile {
  std::string name;
  std::string driver_path;
  std::string uri;
  std::string username;

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
/// two-step "新建连接" wizard.
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

struct DbSession {
  std::uint64_t id = 0;
  std::string name;
  std::string uri;
  std::string driver_path;
  Dialect dialect = Dialect::Generic;
  bool sqlite = false;
  bool read_only = false;
  std::shared_ptr<AdbcSession> session;
  std::vector<DbObject> objects;
};

struct AppModel {
  std::vector<DbSession> sessions;
  std::uint64_t active_session = 0;
  std::string active_object;
  bool active_is_view = false;
  std::string status;
  std::string error;
  std::uint64_t next_session_id = 1;
  int revision = 0;

  [[nodiscard]] const DbSession* ActiveSession() const {
    for (const auto& session : sessions) {
      if (session.id == active_session) {
        return &session;
      }
    }
    return nullptr;
  }
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
};

struct ConnectContext {
  huxerui::State<AppModel> app;
  huxerui::State<WorkspaceModel> workspace;
  huxerui::State<std::vector<ConnectionProfile>> profiles;
  huxerui::TaskScope tasks;
  std::shared_ptr<huxerui::FilePicker> picker;
  std::shared_ptr<huxerui::HttpClient> http;
  std::string profiles_path;
  std::string default_driver;
  std::string default_uri;
  AppThemeConfig theme_config;
  bool system_dark = false;
  NotifyFn notify;
  std::function<void(std::size_t initial_tab, std::string filter_slug)> open_manager;
};

struct AdbcManagerContext {
  huxerui::State<AppModel> app;
  huxerui::State<WorkspaceModel> workspace;
  huxerui::State<std::vector<ConnectionProfile>> profiles;
  huxerui::TaskScope tasks;
  std::shared_ptr<huxerui::HttpClient> http;
  std::string profiles_path;
  std::string default_driver;
  std::string default_uri;
  std::size_t initial_tab = 0;
  std::string initial_filter;
  AppThemeConfig theme_config;
  bool system_dark = false;
  NotifyFn notify;
};

// ---- value and identifier helpers -----------------------------------------
[[nodiscard]] std::string QuoteIdentifier(Dialect dialect, std::string_view identifier);
[[nodiscard]] std::string SqlLiteral(Dialect dialect, std::string_view text,
                                     std::string_view declared_type, bool is_null);
[[nodiscard]] std::string QualifiedName(Dialect dialect, const DbObject& object);
[[nodiscard]] std::string BuildDropObjectSql(Dialect dialect, const DbObject& object);
[[nodiscard]] bool IsSqlReadStatement(std::string_view sql);
[[nodiscard]] bool IsReadStatement(Dialect dialect, std::string_view text);
[[nodiscard]] std::string InferDriverForUri(std::string_view uri);
[[nodiscard]] std::string DisplayNameFromUri(std::string_view uri, std::string_view fallback);
[[nodiscard]] std::string SingleLineCellPreview(std::string_view text);
[[nodiscard]] float ComputeAutoColumnWidth(std::string_view header, std::size_t col_index,
                                           const GridSnapshot* snapshot);
[[nodiscard]] std::string DefaultAppDataDir();
[[nodiscard]] std::string BuildPostgresqlUriWithCredentials(std::string_view uri,
                                                            std::string_view username,
                                                            std::string_view password);
[[nodiscard]] const std::vector<DatabasePreset>& SupportedDatabasePresets();
[[nodiscard]] std::string ComposePresetUri(const DatabasePreset& preset, std::string_view host,
                                           std::string_view port, std::string_view database);
[[nodiscard]] std::optional<AdbcDriverInfo> FindInstalledDriverForPreset(
    const DatabasePreset& preset, const std::vector<AdbcDriverInfo>& discovered);
[[nodiscard]] std::vector<ConnectionProfile> LoadConnectionProfiles(const std::string& path);
bool SaveConnectionProfiles(const std::string& path,
                            const std::vector<ConnectionProfile>& profiles);
[[nodiscard]] std::vector<std::string> OrderConnectionKeys(
    const std::vector<std::string>& natural_keys,
    const std::vector<std::string>& custom_order);
[[nodiscard]] std::vector<std::string> ReorderConnectionKeys(
    const std::vector<std::string>& natural_keys,
    const std::vector<std::string>& custom_order, std::string_view from_key,
    std::string_view to_key);
[[nodiscard]] std::string UserAdbcDriversDir();
[[nodiscard]] std::vector<AdbcDriverInfo> DiscoverAdbcDrivers();

// ---- ADBC operations (implemented in adbc_session.cpp) --------------------
struct AdbcOpenResult {
  std::shared_ptr<AdbcSession> session;
  std::string error;
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

AdbcOpenResult OpenAdbcBlocking(const ConnectionRequest& request);
AdbcObjectsResult LoadObjectsBlocking(AdbcSession& session);
AdbcSchemaResult LoadSchemaBlocking(AdbcSession& session, const DbObject& object);
AdbcQueryResult ExecuteBlocking(AdbcSession& session, std::string sql, bool want_rows,
                                std::size_t max_rows);
BatchInsertResult InsertRowsBlocking(AdbcSession& session, const TableModel& model,
                                     const std::vector<PendingInsertRow>& rows);
TableModel LoadTableBlocking(AdbcSession& session, std::uint64_t session_id,
                             DbObject object, std::int64_t page, std::int64_t page_size,
                             std::optional<huxerui::DataTableSort> sort, std::string filter);

// Async wrappers. Each one serializes its work on the owning session's
// WorkerSequence, so ADBC connection handles are never used concurrently.
huxerui::Task<AdbcQueryResult> ExecuteQueryAsync(std::shared_ptr<AdbcSession> session,
                                                 std::string sql, bool want_rows,
                                                 std::size_t max_rows);
huxerui::Task<AdbcQueryResult> UpdateCellAsync(std::shared_ptr<AdbcSession> session,
                                               TableModel model, std::size_t row_index,
                                               std::size_t column_index, std::string text);
huxerui::Task<AdbcQueryResult> DeleteRowAsync(std::shared_ptr<AdbcSession> session,
                                              TableModel model, std::size_t row_index);

// ---- UI coordination -------------------------------------------------------
void RequestLoadTable(const huxerui::State<AppModel>& app,
                      const huxerui::State<WorkspaceModel>& workspace,
                      const huxerui::TaskScope& tasks, std::uint64_t tab_id,
                      std::int64_t page, std::optional<huxerui::DataTableSort> sort,
                      std::string filter, bool keep_selection, NotifyFn notify);

void RequestSaveCell(const huxerui::State<AppModel>& app,
                     const huxerui::State<WorkspaceModel>& workspace,
                     const huxerui::TaskScope& tasks, std::uint64_t tab_id,
                     const huxerui::DataTableEditRequest& edit, NotifyFn notify);

void RequestDeleteSelected(const huxerui::State<AppModel>& app,
                           const huxerui::State<WorkspaceModel>& workspace,
                           const huxerui::TaskScope& tasks, std::uint64_t tab_id,
                           NotifyFn notify);

void RequestCommitPendingInserts(const huxerui::State<AppModel>& app,
                                 const huxerui::State<WorkspaceModel>& workspace,
                                 const huxerui::TaskScope& tasks, std::uint64_t tab_id,
                                 NotifyFn notify);

void SelectObject(const huxerui::State<AppModel>& app,
                  const huxerui::State<WorkspaceModel>& workspace,
                  const huxerui::TaskScope& tasks, std::uint64_t session_id,
                  std::string object, std::string db_schema, bool is_view, NotifyFn notify);

std::uint64_t OpenSqlTab(const huxerui::State<AppModel>& app,
                         const huxerui::State<WorkspaceModel>& workspace,
                         std::uint64_t session_id, std::string initial_sql = {},
                         std::string custom_title = {});

void ActivateWorkspaceTab(const huxerui::State<AppModel>& app,
                          const huxerui::State<WorkspaceModel>& workspace,
                          std::uint64_t tab_id);

void CloseWorkspaceTabs(const huxerui::State<AppModel>& app,
                        const huxerui::State<WorkspaceModel>& workspace,
                        const std::vector<std::uint64_t>& tab_ids);

void OpenConnection(const huxerui::State<AppModel>& app,
                    const huxerui::State<WorkspaceModel>& workspace,
                    const huxerui::TaskScope& tasks, ConnectionRequest request,
                    NotifyFn notify);

void CloseSession(const huxerui::State<AppModel>& app,
                  const huxerui::State<WorkspaceModel>& workspace,
                  const huxerui::TaskScope& tasks, std::uint64_t session_id,
                  NotifyFn notify);

void RefreshSessionObjects(const huxerui::State<AppModel>& app,
                           const huxerui::State<WorkspaceModel>& workspace,
                           const huxerui::TaskScope& tasks, std::uint64_t session_id,
                           NotifyFn notify, bool notify_success = true);

} // namespace dbpp
