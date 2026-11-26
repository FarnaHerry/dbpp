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

[[nodiscard]] std::string SingleLineCellPreview(std::string_view text);
[[nodiscard]] float ComputeAutoColumnWidth(std::string_view header, std::size_t col_index,
                                           const GridSnapshot* snapshot);
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
BatchInsertResult InsertRowsBlocking(AdbcSession& session, const TableModel& model,
                                     const std::vector<PendingInsertRow>& rows);
TableModel LoadTableBlocking(AdbcSession& session, std::uint64_t session_id,
                             DbObject object, std::int64_t page, std::int64_t page_size,
                             std::optional<huxerui::DataTableSort> sort, std::string filter);

huxerui::Task<AdbcQueryResult> ExecuteQueryAsync(std::shared_ptr<AdbcSession> session,
                                                 std::string sql, bool want_rows,
                                                 std::size_t max_rows);
huxerui::Task<AdbcQueryResult> UpdateCellAsync(std::shared_ptr<AdbcSession> session,
                                               TableModel model, std::size_t row_index,
                                               std::size_t column_index, std::string text);
huxerui::Task<AdbcQueryResult> DeleteRowAsync(std::shared_ptr<AdbcSession> session,
                                              TableModel model, std::size_t row_index);

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
std::uint64_t OpenSettingsTab(const huxerui::State<AppModel>& app,
                              const huxerui::State<WorkspaceModel>& workspace);
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
void HandleSessionDisconnected(const huxerui::State<AppModel>& app,
                                const huxerui::State<WorkspaceModel>& workspace,
                                const huxerui::TaskScope& tasks, std::uint64_t session_id,
                                std::string reason, NotifyFn notify);
void ScheduleSessionHealthCheck(const huxerui::State<AppModel>& app,
                                const huxerui::State<WorkspaceModel>& workspace,
                                const huxerui::TaskScope& tasks, NotifyFn notify);
void CloseAllSessions(const huxerui::State<AppModel>& app, const huxerui::TaskScope& tasks);

} // namespace dbpp
