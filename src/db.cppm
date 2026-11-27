module;

#include <huxerui/datatable.h>
#include <huxerui/http.h>
#include <huxerui/huxerui.h>
#include <huxerui/task.h>

export module dbpp.db;

import std;
import dbpp.settings;
import dbpp.i18n;
import dbpp.types;
import dbpp.adbc;
import dbpp.redis;

export namespace dbpp {

struct DbSession {
  std::uint64_t id = 0;
  std::string name;
  std::string uri;
  std::string driver_path;
  std::string remark;
  Dialect dialect = Dialect::Generic;
  bool sqlite = false;
  bool read_only = false;
  bool production_mode = false;
  bool show_system_databases = false;
  AdbcConnectionInfo connection_info;
  std::shared_ptr<AdbcSession> session;
  std::vector<DbObject> objects;
  std::vector<std::string> databases;
  std::vector<TriggerMeta> triggers;
  std::vector<RoutineMeta> routines;
  std::vector<EventMeta> events;
};

struct AppModel {
  std::vector<DbSession> sessions;
  std::uint64_t active_session = 0;
  std::string active_object;
  bool active_is_view = false;
  std::string status;
  std::string error;
  std::unordered_map<std::string, std::string> failed_connections;
  std::uint64_t next_session_id = 1;
  std::int64_t default_page_size = kPageSize;
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
  LanguageMode language_mode = LanguageMode::System;
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
  LanguageMode language_mode = LanguageMode::System;
  bool system_dark = false;
  NotifyFn notify;
};

struct SqlLimitRewriteResult {
  std::string sql;
  bool applied = false;
  std::int64_t effective_limit = 0;
  std::int64_t effective_offset = 0;
};

[[nodiscard]] SqlLimitRewriteResult ApplyImplicitSqlLimit(Dialect dialect,
                                                          std::string_view sql,
                                                          std::int64_t page_size,
                                                          std::int64_t page = 0);
[[nodiscard]] std::string FormatExecutionDuration(std::int64_t elapsed_us);
[[nodiscard]] std::string CompactSqlPreview(std::string_view sql, std::size_t max_bytes = 240);
[[nodiscard]] std::string SingleLineCellPreview(std::string_view text);
[[nodiscard]] float ComputeAutoColumnWidth(std::string_view header, std::size_t col_index,
                                           const GridSnapshot* snapshot);
[[nodiscard]] std::vector<ConnectionProfile> LoadConnectionProfiles(const std::string& path);
bool SaveConnectionProfiles(const std::string& path,
                            const std::vector<ConnectionProfile>& profiles);
[[nodiscard]] std::vector<SavedQuery> LoadSavedQueries(const std::string& path);
bool SaveSavedQueries(const std::string& path, const std::vector<SavedQuery>& queries);
[[nodiscard]] std::string SavedQueryConnectionKey(std::string_view driver_path,
                                                  std::string_view uri);
[[nodiscard]] std::vector<std::string> OrderConnectionKeys(
    const std::vector<std::string>& natural_keys,
    const std::vector<std::string>& custom_order);
[[nodiscard]] std::vector<std::string> ReorderConnectionKeys(
    const std::vector<std::string>& natural_keys,
    const std::vector<std::string>& custom_order, std::string_view from_key,
    std::string_view to_key);
BatchInsertResult InsertRowsBlocking(AdbcSession& session, const TableModel& model,
                                     const std::vector<PendingInsertRow>& rows);
AdbcQueryResult UpdateCellBlocking(AdbcSession& session, const TableModel& model,
                                   std::size_t row_index, std::size_t column_index,
                                   const std::string& text);
AdbcQueryResult UpdateRowBlocking(
    AdbcSession& session, const TableModel& model, std::size_t row_index,
    const std::vector<std::pair<std::size_t, std::string>>& updates);
AdbcQueryResult DeleteRowBlocking(AdbcSession& session, const TableModel& model,
                                  std::size_t row_index);
TableModel LoadTableBlocking(AdbcSession& session, std::uint64_t session_id,
                             DbObject object, std::int64_t page, std::int64_t page_size,
                             std::optional<huxerui::DataTableSort> sort, std::string filter);

huxerui::Task<AdbcQueryResult> ExecuteQueryAsync(std::shared_ptr<AdbcSession> session,
                                                 std::string sql, bool want_rows,
                                                 std::size_t max_rows,
                                                 std::string target_database = {});
huxerui::Task<AdbcQueryResult> ExecutePreparedQueryAsync(
    std::shared_ptr<AdbcSession> session, std::string sql,
    std::vector<PreparedParamValue> params, bool want_rows, std::size_t max_rows,
    std::string target_database = {});
huxerui::Task<AdbcQueryResult> UpdateCellAsync(std::shared_ptr<AdbcSession> session,
                                               TableModel model, std::size_t row_index,
                                               std::size_t column_index, std::string text);
huxerui::Task<AdbcQueryResult> DeleteRowAsync(std::shared_ptr<AdbcSession> session,
                                              TableModel model, std::size_t row_index);

void RequestLoadTable(const huxerui::State<AppModel>& app,
                      const huxerui::State<WorkspaceModel>& workspace,
                      const huxerui::TaskScope& tasks, std::uint64_t tab_id,
                      std::int64_t page, std::optional<huxerui::DataTableSort> sort,
                      std::string filter, bool keep_selection, NotifyFn notify,
                      std::int64_t page_size = 0);
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
                         std::string custom_title = {}, std::string initial_database = {});
std::uint64_t OpenTableDesignTab(const huxerui::State<AppModel>& app,
                                 const huxerui::State<WorkspaceModel>& workspace,
                                 std::uint64_t session_id, DbObject object,
                                 std::string display_label);
[[nodiscard]] std::vector<std::string> ListSessionDatabases(const DbSession& session);
[[nodiscard]] std::string DefaultSessionDatabase(const DbSession& session);
[[nodiscard]] std::string BuildSwitchDatabaseSql(Dialect dialect,
                                                 std::string_view target_database);
bool SwitchSqlTabSession(const huxerui::State<AppModel>& app,
                         const huxerui::State<WorkspaceModel>& workspace,
                         std::uint64_t tab_id, std::uint64_t session_id);
bool SwitchSqlTabDatabase(const huxerui::State<AppModel>& app,
                          const huxerui::State<WorkspaceModel>& workspace,
                          std::uint64_t tab_id, std::string database);
std::uint64_t OpenSettingsTab(const huxerui::State<AppModel>& app,
                              const huxerui::State<WorkspaceModel>& workspace);
void ActivateWorkspaceTab(const huxerui::State<AppModel>& app,
                          const huxerui::State<WorkspaceModel>& workspace,
                          std::uint64_t tab_id);
void CloseWorkspaceTabs(const huxerui::State<AppModel>& app,
                        const huxerui::State<WorkspaceModel>& workspace,
                        const std::vector<std::uint64_t>& tab_ids);
[[nodiscard]] std::string ConnectionFailureKey(std::string_view driver_path,
                                               std::string_view uri);
void OpenConnection(const huxerui::State<AppModel>& app,
                    const huxerui::State<WorkspaceModel>& workspace,
                    const huxerui::TaskScope& tasks, ConnectionRequest request,
                    NotifyFn notify,
                    std::function<void(std::uint64_t)> on_opened = {});
void CloseSession(const huxerui::State<AppModel>& app,
                  const huxerui::State<WorkspaceModel>& workspace,
                  const huxerui::TaskScope& tasks, std::uint64_t session_id,
                  NotifyFn notify);
void RefreshSessionObjects(const huxerui::State<AppModel>& app,
                           const huxerui::State<WorkspaceModel>& workspace,
                           const huxerui::TaskScope& tasks, std::uint64_t session_id,
                           NotifyFn notify, bool notify_success = true);
void HandleSessionDisconnected(const huxerui::State<AppModel>& app,
                                const huxerui::State<WorkspaceModel>& workspace,
                                const huxerui::TaskScope& tasks, std::uint64_t session_id,
                                std::string reason, NotifyFn notify);
void ScheduleSessionHealthCheck(const huxerui::State<AppModel>& app,
                                const huxerui::State<WorkspaceModel>& workspace,
                                const huxerui::TaskScope& tasks, NotifyFn notify);
void CloseAllSessions(const huxerui::State<AppModel>& app, const huxerui::TaskScope& tasks);

} // namespace dbpp

namespace dbpp::detail {

std::string DbTrimAscii(std::string_view text);
std::string DbLowerAscii(std::string_view text);
std::string DbUpperAscii(std::string_view text);
std::optional<int> DbParseRedisTargetDbIndex(std::string_view text);
bool DbSessionHasMultipleCatalogs(const DbSession& session);
std::string DbSessionObjectBucket(const DbSession& session, const DbObject& object,
                                  bool multi_catalog);
const DbSession* DbFindSession(const AppModel& model, std::uint64_t id);
const DbObject* DbFindObject(const DbSession& session, std::string_view name,
                             std::string_view db_schema);
std::string DbFormatTabTitle(const DbSession& session, std::string_view object,
                             std::string_view db_schema);
void DbSyncAppFromActiveTab(AppModel& model, const WorkspaceModel& ws);
std::size_t DbParseRowIndex(std::string_view id);
bool DbEqualsIgnoreCaseAscii(std::string_view left, std::string_view right);
void DbCloseSessionAsync(std::shared_ptr<AdbcSession> adbc, const huxerui::TaskScope& tasks);
AdbcQueryResult DbUpdateCellBlocking(AdbcSession& session, const TableModel& model,
                                     std::size_t row_index, std::size_t column_index,
                                     const std::string& text);
AdbcQueryResult DbUpdateRowBlocking(
    AdbcSession& session, const TableModel& model, std::size_t row_index,
    const std::vector<std::pair<std::size_t, std::string>>& updates);
AdbcQueryResult DbDeleteRowBlocking(AdbcSession& session, const TableModel& model,
                                    std::size_t row_index);
std::string DbBuildWhere(Dialect dialect, const TableModel& model, const GridRow& row);
void DbApplyOpenedSession(const huxerui::State<AppModel>& app,
                          const huxerui::State<WorkspaceModel>& workspace,
                          const huxerui::TaskScope& tasks, AdbcOpenResult opened,
                          ConnectionRequest request, AdbcObjectsResult objects,
                          NotifyFn notify,
                          std::function<void(std::uint64_t)> on_opened = {});

} // namespace dbpp::detail
