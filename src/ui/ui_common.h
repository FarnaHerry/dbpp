#pragma once

#include <huxerui/datatable.h>
#include <huxerui/huxerui.h>
#include "app_resources.h"

import dbpp.settings;
import dbpp.i18n;
import dbpp.types;
import dbpp.adbc;
import dbpp.redis;
import dbpp.driver_installer;
import dbpp.db;
import dbpp.mcp;
import dbpp.config_db;
import dbpp.instance;

#ifndef DBPP_VERSION
#define DBPP_VERSION "0.4.0"
#endif

#include "ui/theme_palette.h"
#include "ui/theme.h"
#include "ui/root.h"
#include "ui/tray.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dbpp {

inline constexpr std::string_view kRowNumberColumnId = "__dbpp_rownum";

struct LocalizedCatalog {
  std::shared_ptr<huxerui::Resources> resources;
  std::shared_ptr<SettingsModel> settings;

  [[nodiscard]] huxerui::ResourceConfiguration Config() const {
    const LanguageMode mode = settings ? settings->language.Get() : LanguageMode::System;
    const huxerui::Locale platform =
        resources ? resources->Configuration().locale : huxerui::Locale::Default();
    return huxerui::ResourceConfiguration{.locale = EffectiveLocale(mode, platform)};
  }

  [[nodiscard]] std::string Get(const huxerui::StringVariant& value) const {
    if (!resources) {
      return {};
    }
    return resources->GetString(value, Config());
  }

  template <class... Args>
  [[nodiscard]] std::string Fmt(huxerui::StringResource resource, Args&&... args) const {
    return Get(huxerui::StringVariant::Format(std::move(resource), std::forward<Args>(args)...));
  }
};

struct LocalizedPresetStrings {
  huxerui::StringResource name;
  huxerui::StringResource subtitle;
  std::optional<huxerui::StringResource> uri_placeholder;
};

struct ConnectionFormStates {
  huxerui::State<std::string> driver_slug;
  huxerui::State<huxerui::TextEditingValue> driver;
  huxerui::State<huxerui::TextEditingValue> uri;
  huxerui::State<huxerui::TextEditingValue> host;
  huxerui::State<huxerui::TextEditingValue> port;
  huxerui::State<huxerui::TextEditingValue> database;
  huxerui::State<huxerui::TextEditingValue> username;
  huxerui::State<huxerui::TextEditingValue> password;
  huxerui::State<huxerui::TextEditingValue> title;
  huxerui::State<std::string> extra_query;
  huxerui::State<std::size_t> input_mode;
};

struct SidebarObjectGroup {
  std::string key;
  huxerui::StringResource label;
  std::vector<const DbObject*> items;
};

struct SidebarDatabaseGroup {
  std::string name;
  bool is_system = false;
  bool is_target = false;
  bool default_open = true;
  std::size_t object_count = 0;
  std::vector<SidebarObjectGroup> groups;
};

// Shared non-composable helpers (src/ui/ui_common.cpp)
std::size_t RowIndexFromId(std::string_view id);
bool IsDraftRowId(std::string_view id);
std::optional<std::uint64_t> ParseDraftRowId(std::string_view id);
const PendingInsertRow* FindDraftRow(const std::vector<PendingInsertRow>& drafts,
                                     std::uint64_t draft_id);
PendingInsertRow* FindDraftRowMutable(std::vector<PendingInsertRow>& drafts,
                                      std::uint64_t draft_id);
std::string DraftCellBufferKey(std::string_view row_id, std::string_view col_id);
const GridRow* RowAt(const std::shared_ptr<const GridSnapshot>& snapshot, std::size_t index);
std::string CellText(const std::shared_ptr<const GridSnapshot>& snapshot, std::string_view row_id,
                     std::size_t column);
bool CellIsNull(const std::shared_ptr<const GridSnapshot>& snapshot, std::string_view row_id,
                std::size_t column);
huxerui::DataTableRowSource MakeRows(const std::shared_ptr<const GridSnapshot>& snapshot);
huxerui::DataTableRowSource MakeTableRows(const std::shared_ptr<const GridSnapshot>& snapshot,
                                          const std::vector<PendingInsertRow>& drafts);
bool OpenExternalUrl(std::string_view url);
const huxerui::ImageResource& ObjectGroupIconResource(std::string_view key);
huxerui::StringResource ObjectKindResource(const DbObject& object);
std::string TypeLabel(const ColumnMeta& column);
bool IsBlobColumn(const ColumnMeta& column);
bool ContainsIgnoreCaseAscii(std::string_view haystack, std::string_view needle);
std::string_view StripLeadingV(std::string_view text);
std::string UriDisplayName(std::string_view uri);
std::string TrimmedSql(std::string_view text);
std::string NormalizeToastMessage(std::string_view text);
bool IsLikelyErrorMessage(std::string_view text);
NotifyFn MakeNotify(huxerui::ToastHandle toast,
                    std::shared_ptr<std::optional<huxerui::LayerId>> last_toast_id);
std::int64_t SafePageOffset(std::int64_t page, std::int64_t page_size);
std::string FormatGlobalRowNumber(std::int64_t base_offset, std::size_t row_index);
huxerui::DataTableColumn MakeRowNumberColumn(
    std::int64_t base_offset, std::size_t row_count,
    const std::unordered_map<std::string, float>& column_widths, huxerui::Color muted_color,
    huxerui::Color draft_color);
std::vector<huxerui::DataTableColumn> BuildReadOnlyColumns(
    const std::vector<std::string>& names, const std::shared_ptr<const GridSnapshot>& snapshot,
    const std::unordered_map<std::string, float>& column_widths, huxerui::Color text_color,
    huxerui::Color null_color, std::int64_t base_offset, huxerui::Color muted_color);
LocalizedPresetStrings DatabasePresetStrings(const DatabasePreset& preset);
std::pair<huxerui::StringResource, huxerui::StringResource> ThemePresetStrings(ThemePreset preset);
huxerui::View WithDialogTheme(const AppThemeConfig& config, LanguageMode language_mode,
                              bool system_dark, huxerui::View content);
std::optional<std::size_t> FindMatchingSessionIndex(const ConnectionProfile& profile,
                                                    const std::vector<DbSession>& sessions,
                                                    const std::vector<bool>& claimed);
bool PersistConnectionProfileList(const std::shared_ptr<ConfigDbService>& config_db,
                                  const std::string& profiles_path,
                                  const std::vector<ConnectionProfile>& list);
void ExportSqlWithPicker(std::shared_ptr<AdbcSession> adbc, std::string sql,
                         ExportFormat format, std::string suggested_name,
                         std::shared_ptr<huxerui::FilePicker> picker,
                         huxerui::TaskScope tasks, NotifyFn notify,
                         LocalizedCatalog loc);

// Shared UI composables (src/ui/ui_common.cpp)
huxerui::View TableEmptyState(huxerui::StringVariant text);
huxerui::View BigEmptyState(std::function<void()> open_new);
huxerui::View PageSizePopup(huxerui::PopupContext popup, std::int64_t current_size,
                            std::function<void(std::int64_t)> on_apply);
huxerui::View AuditDurationBadge(std::int64_t elapsed_us);
huxerui::View AuditSqlBadge(std::string executed_sql, NotifyFn notify);
huxerui::View PageNumberInput(std::int64_t current_page_one_based,
                              std::optional<std::int64_t> max_page_one_based, bool enabled,
                              std::function<void(std::int64_t)> on_go_zero_based, NotifyFn notify);
huxerui::View CompactPagerControls(std::int64_t current_page,
                                   std::optional<std::int64_t> total_pages, bool has_next,
                                   bool enabled, std::uint64_t sync_version,
                                   std::function<void(std::int64_t)> on_go_page, NotifyFn notify);
huxerui::View BottomStatusBar(huxerui::Views items);

// Sidebar grouping helpers (src/ui/sidebar_groups.cpp)
std::vector<SidebarObjectGroup> GroupObjectItems(
    Dialect dialect, const std::vector<const DbObject*>& items);
std::vector<SidebarObjectGroup> GroupSessionObjects(const DbSession& session);
bool SessionHasMultipleSchemas(const DbSession& session);
bool SessionUsesDatabaseHierarchy(const DbSession& session);
std::vector<SidebarDatabaseGroup> GroupSessionDatabases(const DbSession& session);

// Connection & confirmation dialogs (src/ui/connection_dialogs.cpp)
huxerui::View ConnectionFormFields(
    const DatabasePreset& preset, const std::string& active_preset_placeholder,
    const ConnectionFormStates& form, const std::vector<AdbcDriverInfo>& driver_list,
    bool can_pick_file, std::function<void()> pick_local_file,
    std::function<void()> on_password_submit, huxerui::StringVariant name_placeholder);
huxerui::View EditConnectionProfileDialog(
    huxerui::DialogContext dialog, std::optional<ConnectionProfile> original_profile,
    ConnectionProfile initial_values, std::optional<std::uint64_t> active_session_id,
    huxerui::State<AppModel> app, huxerui::State<WorkspaceModel> workspace,
    huxerui::State<std::vector<ConnectionProfile>> profiles, std::string profiles_path,
    huxerui::TaskScope tasks, NotifyFn notify);
huxerui::View DeleteSavedProfileDialog(
    huxerui::DialogContext dialog, ConnectionProfile profile, std::string display_name,
    huxerui::State<std::vector<ConnectionProfile>> profiles, std::string profiles_path,
    NotifyFn notify);
huxerui::View ConfirmCloseTabsDialog(huxerui::DialogContext dialog, huxerui::State<AppModel> app,
                                     huxerui::State<WorkspaceModel> workspace,
                                     std::vector<std::uint64_t> tab_ids, std::size_t draft_count);
huxerui::View CloseConfirmDialog(huxerui::DialogContext dialog,
                                 std::shared_ptr<TrayRuntime> tray_runtime,
                                 huxerui::State<bool> close_dialog_open);
huxerui::View DeleteRowDialog(huxerui::DialogContext dialog, huxerui::State<AppModel> app,
                              huxerui::State<WorkspaceModel> workspace, std::uint64_t tab_id,
                              huxerui::TaskScope tasks, NotifyFn notify, bool is_redis);

// Schema / DDL dialogs (src/ui/schema_dialogs.cpp)
huxerui::View ObjectSchemaDialog(huxerui::DialogContext dialog, std::shared_ptr<AdbcSession> adbc,
                                 std::uint64_t session_id, DbObject object,
                                 std::string display_label, std::string qualified_name,
                                 huxerui::State<AppModel> app,
                                 huxerui::State<WorkspaceModel> workspace,
                                 huxerui::TaskScope tasks, NotifyFn notify);
huxerui::View EditTableDialog(huxerui::DialogContext dialog, std::shared_ptr<AdbcSession> adbc,
                              std::uint64_t session_id, Dialect dialect, DbObject object,
                              std::string display_label, huxerui::State<AppModel> app,
                              huxerui::State<WorkspaceModel> workspace, huxerui::TaskScope tasks,
                              NotifyFn notify);
huxerui::View DropObjectDialog(huxerui::DialogContext dialog, std::shared_ptr<AdbcSession> adbc,
                               std::uint64_t session_id, DbObject object, std::string display_label,
                               std::string drop_sql, huxerui::State<AppModel> app,
                               huxerui::State<WorkspaceModel> workspace, huxerui::TaskScope tasks,
                               NotifyFn notify);

// Top-level dialogs & workspace views
huxerui::View AdbcManagerDialog(huxerui::DialogContext dialog, AdbcManagerContext context);
huxerui::View ConnectDialog(huxerui::DialogContext dialog, ConnectContext context);
huxerui::View Sidebar(
    huxerui::State<AppModel> app, huxerui::State<WorkspaceModel> workspace,
    huxerui::State<std::vector<ConnectionProfile>> profiles, std::string profiles_path,
    huxerui::TaskScope tasks, NotifyFn notify, AppThemeConfig theme_config,
    LanguageMode language_mode, bool system_dark,
    std::function<void(ConnectionProfile)> connect_profile);
huxerui::View TableBrowserPage(
    huxerui::State<AppModel> app, huxerui::State<WorkspaceModel> workspace, std::uint64_t tab_id,
    huxerui::TaskScope tasks, NotifyFn notify, AppThemeConfig theme_config,
    LanguageMode language_mode, bool system_dark);
void ExecuteSqlTabQuery(const huxerui::State<AppModel>& app,
                        const huxerui::State<WorkspaceModel>& workspace,
                        const std::shared_ptr<SettingsModel>& settings, std::uint64_t tab_id,
                        const huxerui::TaskScope& tasks, NotifyFn notify,
                        const LocalizedCatalog& loc, std::int64_t requested_page,
                        bool from_editor, std::int64_t override_page_size,
                        std::vector<PreparedParamValue> bound_params = {});
huxerui::View SqlPage(huxerui::State<AppModel> app, huxerui::State<WorkspaceModel> workspace,
                      huxerui::State<std::vector<ConnectionProfile>> profiles, std::uint64_t tab_id,
                      huxerui::TaskScope tasks, NotifyFn notify, AppThemeConfig theme_config,
                      LanguageMode language_mode, bool system_dark);
huxerui::View AllTabsPopup(
    huxerui::PopupContext popup, huxerui::State<AppModel> app,
    huxerui::State<WorkspaceModel> workspace,
    std::function<void(std::vector<std::uint64_t>)> request_close_tabs);
huxerui::View SettingsPage(NotifyFn notify,
                           std::function<void(std::int64_t)> on_page_size_applied = {});

} // namespace dbpp
