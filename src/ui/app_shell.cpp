#include "ui/ui_common.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <ranges>

namespace dbpp {

using namespace huxerui;
using namespace dbpp::ui;

[[huxerui::composable]] View AppContent() {
  constexpr float kSidebarDefaultWidth = 240.0F;
  constexpr float kSidebarMinWidth = 180.0F;
  constexpr float kSidebarMaxWidth = 420.0F;
  constexpr float kMainMinWidth = 420.0F;
  auto app = UseState(AppModel{});
  auto workspace = UseState(WorkspaceModel{});
  auto split_frames = UseState(std::array<Frame, 2>{
      Frame{.width = kSidebarDefaultWidth, .min_width = kSidebarMinWidth,
            .max_width = kSidebarMaxWidth},
      Frame{.min_width = kMainMinWidth},
  });
  auto split_grows = UseState(std::array<Grow, 2>{Grow(0.0F), Grow(1.0F)});
  auto toast = UseToast();
  auto dialogs = UseDialog();
  auto menu = UseMenu();
  const auto tab_scroll = UseScrollController();
  const auto all_tabs_anchor = UseViewHandle();
  const auto all_tabs_popup = UsePopup();
  auto tasks = UseTaskScope();
  auto picker = UseService<FilePicker>();
  auto http = UseService<HttpClient>();
  auto application = UseApplication();
  const auto config_db = UseService<ConfigDbService>();
  const File profile_dir = application.Directories().data_directory;
  const std::string profiles_path = profile_dir.Child("dbpp.sqlite3").Path();
  const File default_file = application.Directories().data_directory.Child("untitled.sqlite");
  auto profiles = UseState(std::vector<ConnectionProfile>{});
  Lifecycle([profiles, profiles_path, config_db, tasks] {
    if (config_db) {
      tasks.Launch([profiles, config_db]() -> Task<void> {
        profiles = co_await config_db->LoadConnectionsAsync();
      });
    } else {
      profiles = LoadConnectionProfiles(profiles_path);
    }
    return [] {};
  });

  const WindowHandle window = UseWindow();
  const auto tray_runtime = UseService<TrayRuntime>();
  const auto settings_model = UseService<SettingsModel>();
  const LocalizedCatalog loc{UseService<Resources>(), settings_model};
  const bool system_dark = UseSystemThemeBrightness() == ThemeBrightness::Dark;
  Lifecycle(
      [tray_runtime, system_dark] {
        if (tray_runtime) {
          tray_runtime->UpdateSystemDark(system_dark);
        }
        return [] {};
      },
      system_dark);
  const AppThemeConfig theme_config = settings_model->ThemeConfig();
  const LanguageMode language_mode = settings_model->language.Get();
  const std::int64_t default_page_size = settings_model->default_page_size.Get();
  Lifecycle(
      [app, default_page_size] {
        app.Update([default_page_size](AppModel& model) {
          model.default_page_size = default_page_size;
        });
        return [] {};
      },
      default_page_size);
  const ResolvedTheme resolved = ResolveTheme(theme_config, system_dark);
  const ThemeSpec& spec = resolved.spec;
  const SemanticColors& semantic = resolved.semantic;

  auto last_toast_id = UseState(std::make_shared<std::optional<LayerId>>());
  const NotifyFn notify = MakeNotify(toast, last_toast_id.Get());

  Lifecycle([app, workspace, tasks, notify] {
    auto active = std::make_shared<std::atomic<bool>>(true);
    tasks.Launch([active, app, workspace, tasks, notify]() -> Task<void> {
      while (active->load(std::memory_order_acquire)) {
        co_await Delay(std::chrono::seconds{15});
        if (!active->load(std::memory_order_acquire)) {
          break;
        }
        ScheduleSessionHealthCheck(app, workspace, tasks, notify);
      }
    });
    return [active, app, tasks] {
      active->store(false, std::memory_order_release);
      CloseAllSessions(app, tasks);
    };
  });

  // 关闭策略：托盘可用时关闭=隐藏（轻量模式=销毁窗口省内存）；托盘不可用时
  // 弹确认对话框退出，避免窗口一关应用就消失且没有入口唤回。
  auto close_dialog_open = UseState(false);
  window.OnCloseRequest(
      [tray_runtime, settings_model, dialogs, close_dialog_open, system_dark]() -> bool {
        if (tray_runtime->exit_requested) {
          return true;
        }
        if (settings_model->tray_enabled.Get() && tray_runtime->tray.IsAvailable() &&
            !tray_runtime->tray_refresh_failed) {
          if (settings_model->lightweight.Get()) {
            // 轻量级模式走正常关闭路径销毁原生窗口与 UI；Runtime 由
            // quit_on_last_window_closed = false 保活，托盘负责下次唤出重建。
            tray_runtime->retired = true;
            return false;
          }
          tray_runtime->HideWindow();
          return true;
        }
        if (close_dialog_open.Get()) {
          return true;
        }
        close_dialog_open = true;
        const AppThemeConfig current_theme = settings_model->ThemeConfig();
        const LanguageMode current_language = settings_model->language.Get();
        dialogs.Show([tray_runtime, close_dialog_open, current_theme, current_language,
                      system_dark](DialogContext dialog) {
          return WithDialogTheme(
              current_theme, current_language, system_dark,
              CloseConfirmDialog(dialog, tray_runtime, close_dialog_open));
        });
        return true;
      });

  // Optional startup connection: `DBPP_OPEN=/path/to.db dbpp` or
  // `DBPP_DRIVER=/path/to/libadbc_driver_x.so DBPP_OPEN=uri dbpp`.
  // Also register the tray handler so secondary single-instance launches
  // can activate this window and forward additional open requests.
  Lifecycle([tray_runtime, app, workspace, tasks, notify] {
    const auto dispatch_open = [app, workspace, tasks, notify](InstanceOpenRequest req) {
      if (req.uri.empty()) {
        return;
      }
      ConnectionRequest request{
          .driver_path =
              req.driver_path.empty() ? InferDriverForUri(req.uri) : std::move(req.driver_path),
          .uri = std::move(req.uri),
          .username = {},
          .password = {},
          .title = {},
      };
      OpenConnection(app, workspace, tasks, std::move(request), notify);
    };

    if (const auto startup = TakeEnvOpenRequest()) {
      dispatch_open(*startup);
    }
    tray_runtime->SetOpenRequestHandler(std::move(dispatch_open));
    return [tray_runtime] { tray_runtime->ClearOpenRequestHandler(); };
  });

  const auto open_manager = [dialogs, app, workspace, profiles, tasks, http, profiles_path,
                             default_file, notify, theme_config, language_mode,
                             system_dark](std::size_t initial_tab, std::string initial_filter) {
    AdbcManagerContext context{
        .app = app,
        .workspace = workspace,
        .profiles = profiles,
        .tasks = tasks,
        .http = http,
        .profiles_path = profiles_path,
        .default_driver = DefaultSqliteDriverPath(),
        .default_uri = default_file.Path(),
        .initial_tab = initial_tab,
        .initial_filter = std::move(initial_filter),
        .theme_config = theme_config,
        .language_mode = language_mode,
        .system_dark = system_dark,
        .notify = notify,
    };
    dialogs.Show([context](DialogContext dialog) {
      return WithDialogTheme(context.theme_config, context.language_mode, context.system_dark,
                             AdbcManagerDialog(dialog, context));
    });
  };

  const auto open_new = [dialogs, app, workspace, profiles, tasks, picker, http, profiles_path,
                         notify, default_file, theme_config, language_mode, system_dark,
                         open_manager] {
    ConnectContext context{
        .app = app,
        .workspace = workspace,
        .profiles = profiles,
        .tasks = tasks,
        .picker = picker,
        .http = http,
        .profiles_path = profiles_path,
        .default_driver = DefaultSqliteDriverPath(),
        .default_uri = default_file.Path(),
        .theme_config = theme_config,
        .language_mode = language_mode,
        .system_dark = system_dark,
        .notify = notify,
        .open_manager = open_manager,
    };
    dialogs.Show([context](DialogContext dialog) {
      return WithDialogTheme(context.theme_config, context.language_mode, context.system_dark,
                             ConnectDialog(dialog, context));
    });
  };

  const auto open_new_query = [app, workspace, notify, loc] {
    const DbSession* active_session = app.Get().ActiveSession();
    if (active_session == nullptr) {
      notify(loc.Get(app::strings::toast_select_connection_first));
      return;
    }
    (void)OpenSqlTab(app, workspace, active_session->id);
  };

  const auto connect_profile = [app, workspace, tasks, notify](ConnectionProfile profile) {
    const SanitizedUriResult sanitized = SanitizeUriCredentials(profile.uri);
    ConnectionRequest request{
        .driver_path = profile.driver_path,
        .uri = sanitized.uri,
        .username = profile.username.empty() ? sanitized.username : profile.username,
        .password = profile.password.empty() ? sanitized.password : profile.password,
        .title = profile.name.empty() ? UriDisplayName(sanitized.uri) : profile.name,
    };
    OpenConnection(app, workspace, tasks, std::move(request), notify);
  };

  const auto request_close_tabs = [dialogs, app, workspace, theme_config, language_mode,
                                   system_dark](std::vector<std::uint64_t> ids) {
    if (ids.empty()) {
      return;
    }
    std::size_t draft_count = 0;
    const WorkspaceModel& ws_snapshot = workspace.Get();
    for (const std::uint64_t id : ids) {
      if (const WorkspaceTab* target = ws_snapshot.FindTab(id);
          target != nullptr && target->kind == WorkspaceTabKind::Table) {
        draft_count += target->table.pending_inserts.size();
      }
    }
    if (draft_count == 0) {
      CloseWorkspaceTabs(app, workspace, ids);
      return;
    }
    dialogs.Show([app, workspace, ids = std::move(ids), draft_count, theme_config, language_mode,
                  system_dark](DialogContext dialog) {
      return WithDialogTheme(theme_config, language_mode, system_dark,
                             ConfirmCloseTabsDialog(dialog, app, workspace, ids, draft_count));
    });
  };

  const AppModel& model = app.Get();
  const WorkspaceModel& ws = workspace.Get();
  const WorkspaceTab* active_tab = ws.ActiveTab();
  const bool settings_tab_active =
      active_tab != nullptr && active_tab->kind == WorkspaceTabKind::Settings;

  // 标题栏右上角的齿轮负责打开/激活工作区中的「设置」标签页。
  View gear_button = Image(app::images::settings)
                         .Fit(ImageFit::Contain)
                         .Tint(settings_tab_active ? spec.colors.primary : spec.colors.on_surface)
                         .With(Padding{EdgeInsets::Symmetric(6.0F, 6.0F)},
                               CornerRadius{6.0F})
                         .OnClick([app, workspace] {
                           (void)OpenSettingsTab(app, workspace);
                         });
  if (settings_tab_active) {
    gear_button = std::move(gear_button)
                      .With(Background{VisualFill{spec.colors.primary_container}},
                            Border{WithAlpha(spec.colors.primary, 0.45F), 1.0F});
  }

  View title_bar = WindowTitleBar{
      TitleBarButton(UseString(app::strings::nav_new_connection),
                     [open_new] {
                       open_new();
                     },
                     false, &app::images::add_connection),
      TitleBarButton(UseString(app::strings::nav_new_query), open_new_query,
                     active_tab != nullptr && active_tab->kind == WorkspaceTabKind::Sql,
                     &app::images::sql_console),
      TitleBarButton(UseString(app::strings::nav_driver_manager),
                     [open_manager] {
                       open_manager(1, {});
                     },
                     false, &app::images::drivers),
      Spacer(),
      std::move(gear_button),
  }.With(Padding{EdgeInsets::Symmetric(10.0F, 0.0F)}, Spacing{6.0F},
         CrossAlign{CrossAxisAlignment::Center},
         Background{VisualFill{spec.colors.surface_container_low}});

  View main_content;
  if (ws.tabs.empty() || active_tab == nullptr) {
    main_content = BigEmptyState(open_new);
  } else {
    Views tab_items;
    tab_items.Reserve(ws.tabs.size());
    for (const WorkspaceTab& t : ws.tabs) {
      const std::uint64_t tid = t.id;
      const bool dirty = t.kind == WorkspaceTabKind::Table && !t.table.pending_inserts.empty();
      const std::string sublabel =
          (t.kind != WorkspaceTabKind::Settings && model.sessions.size() > 1)
              ? t.session_name
              : std::string{};
      std::vector<std::uint64_t> all_ids;
      std::vector<std::uint64_t> other_ids;
      all_ids.reserve(ws.tabs.size());
      other_ids.reserve(ws.tabs.size());
      for (const WorkspaceTab& other : ws.tabs) {
        all_ids.push_back(other.id);
        if (other.id != tid) {
          other_ids.push_back(other.id);
        }
      }

      bool is_redis_tab = false;
      if (t.kind == WorkspaceTabKind::Table) {
        for (const DbSession& open : model.sessions) {
          if (open.id == t.session_id) {
            is_redis_tab = open.dialect == Dialect::Redis;
            break;
          }
        }
      }
      const Color tab_accent =
          t.kind == WorkspaceTabKind::Settings
              ? spec.colors.primary
              : (t.kind == WorkspaceTabKind::Sql
                     ? semantic.accent_sql
                     : (is_redis_tab
                            ? semantic.accent_database
                            : (t.is_view ? semantic.accent_view : semantic.accent_table)));
      const bool is_active = tid == ws.active_tab_id;
      View tab_icon;
      if (t.kind == WorkspaceTabKind::Settings) {
        tab_icon = TintedIcon(app::images::settings,
                              is_active ? spec.colors.primary : spec.colors.on_surface_variant,
                              14.0F);
      } else if (t.kind == WorkspaceTabKind::Sql) {
        tab_icon = TintedIcon(app::images::sql_console, semantic.accent_sql, 14.0F);
      } else {
        const ImageResource& obj_res =
            is_redis_tab ? app::images::database
                         : (t.is_view ? app::images::view : app::images::table);
        tab_icon = TintedIcon(obj_res, tab_accent, 14.0F);
      }
      const std::string tab_label =
          t.kind == WorkspaceTabKind::Settings ? UseString(app::strings::settings_title) : t.title;
      tab_items.Add(
          WorkspaceTabItem(
              WorkspaceTabItemOptions{
                  .label = tab_label,
                  .sublabel = sublabel,
                  .active = is_active,
                  .dirty = dirty,
                  .kind_accent = tab_accent,
                  .is_sql = t.kind == WorkspaceTabKind::Sql,
                  .leading_icon = std::move(tab_icon),
              },
              [app, workspace, tid] { ActivateWorkspaceTab(app, workspace, tid); },
              [request_close_tabs, tid] { request_close_tabs({tid}); })
              .On<ViewEvents::ContextMenuRequested>(
                  [menu, loc, request_close_tabs, tid, other_ids, all_ids](Point position) {
                    std::vector<MenuEntry> entries;
                    entries.emplace_back(
                        MenuItem(loc.Get(app::strings::tab_ctx_close),
                                 [request_close_tabs, tid] { request_close_tabs({tid}); }));
                    if (!other_ids.empty()) {
                      entries.emplace_back(
                          MenuItem(loc.Get(app::strings::tab_ctx_close_others),
                                   [request_close_tabs, other_ids] {
                                     request_close_tabs(other_ids);
                                   }));
                    }
                    entries.emplace_back(MenuSection{});
                    entries.emplace_back(
                        MenuItem(loc.Get(app::strings::tab_ctx_close_all),
                                 [request_close_tabs, all_ids] {
                                   request_close_tabs(all_ids);
                                 }));
                    menu.ShowAt(position, std::move(entries));
                  })
              .Key("ws-tab-" + std::to_string(tid)));
    }

    View all_tabs_button =
        TintedIcon(app::images::chevron_down, spec.colors.on_surface_variant, 14.0F)
            .With(Padding{EdgeInsets::Symmetric(7.0F, 6.0F)}, CornerRadius{6.0F},
                  Background{VisualFill{spec.colors.surface_container}},
                  Border{semantic.border_subtle, 1.0F}, all_tabs_anchor)
            .OnClick([all_tabs_popup, all_tabs_anchor, app, workspace, request_close_tabs,
                      theme_config, language_mode, system_dark] {
              all_tabs_popup.Show(
                  all_tabs_anchor,
                  [app, workspace, request_close_tabs, theme_config, language_mode,
                   system_dark](PopupContext popup) {
                    return WithDialogTheme(
                        theme_config, language_mode, system_dark,
                        AllTabsPopup(popup, app, workspace, request_close_tabs));
                  },
                  PopupOptions{
                      .placement = {.side = AnchorSide::Below, .alignment = AnchorAlignment::End},
                      .gap = 4.0F,
                      .viewport_margin = 8.0F,
                      .offset = {},
                      .dismiss_on_outside_press = true,
                      .dismiss_on_cancel = true,
                      .trap_focus = false,
                      .retain_anchor_focus = false,
                      .on_dismiss_request = {},
                  });
            });

    View tab_strip =
        Row{
            ScrollView(Row{std::move(tab_items)}
                           .With(Spacing{4.0F}, CrossAlign{CrossAxisAlignment::Center}))
                .ScrollAxis(Axis::Horizontal)
                .Controller(tab_scroll)
                .On<ViewEvents::ScrollInput>([tab_scroll](const ScrollInputEvent& event) -> bool {
                  if (std::abs(event.delta_x) < 0.01F && std::abs(event.delta_y) > 0.01F) {
                    return tab_scroll.ScrollBy(event.delta_y);
                  }
                  return false;
                })
                .With(Grow(1.0F), Frame{.height = 30.0F}),
            TitleBarButton("＋", open_new_query),
            std::move(all_tabs_button),
        }
            .With(Padding{EdgeInsets::Symmetric(6.0F, 3.0F)}, Spacing{4.0F},
                  CrossAlign{CrossAxisAlignment::Center});

    View active_body;
    if (active_tab->kind == WorkspaceTabKind::Settings) {
      const auto apply_global_page_size = [app, workspace, settings_model, tasks, notify,
                                           loc](std::int64_t next_size) {
        const std::int64_t clamped = std::clamp(next_size, kMinPageSize, kMaxPageSize);
        app.Update([clamped](AppModel& m) { m.default_page_size = clamped; });
        struct PendingTableReload {
          std::uint64_t tab_id = 0;
          std::optional<DataTableSort> sort;
          std::string filter;
        };
        std::vector<PendingTableReload> table_reloads;
        std::vector<std::uint64_t> sql_reloads;
        workspace.Update([clamped, &table_reloads, &sql_reloads](WorkspaceModel& w) {
          for (WorkspaceTab& tab : w.tabs) {
            if (tab.kind == WorkspaceTabKind::Table) {
              if (tab.table.pending_inserts.empty()) {
                table_reloads.push_back(PendingTableReload{
                    .tab_id = tab.id,
                    .sort = tab.table.sort,
                    .filter = tab.table.filter,
                });
              }
            } else if (tab.kind == WorkspaceTabKind::Sql) {
              tab.sql.page_size = clamped;
              if (!tab.sql.running && tab.sql.paginated && !tab.sql.last_sql.empty()) {
                sql_reloads.push_back(tab.id);
              }
            }
          }
        });
        for (const PendingTableReload& item : table_reloads) {
          RequestLoadTable(app, workspace, tasks, item.tab_id, 0, item.sort, item.filter, false,
                           notify, clamped);
        }
        for (const std::uint64_t sql_tab_id : sql_reloads) {
          ExecuteSqlTabQuery(app, workspace, settings_model, sql_tab_id, tasks, notify, loc, 0,
                             false, clamped);
        }
      };
      active_body = SettingsPage(notify, apply_global_page_size).Key("tab-body-settings");
    } else if (active_tab->kind == WorkspaceTabKind::Table) {
      active_body = TableBrowserPage(app, workspace, active_tab->id, tasks, notify,
                                     theme_config, language_mode, system_dark)
                        .Key("tab-body-" + std::to_string(active_tab->id));
    } else {
      active_body = SqlPage(app, workspace, profiles, active_tab->id, tasks, notify,
                            theme_config, language_mode, system_dark)
                        .Key("tab-body-" + std::to_string(active_tab->id));
    }

    main_content = Column{
        std::move(tab_strip),
        Divider(),
        std::move(active_body),
    }.With(Spacing{0.0F}, CrossAlign{CrossAxisAlignment::Stretch});
  }

  const std::array<Frame, 2> cur_split_frames = split_frames.Get();
  const std::array<Grow, 2> cur_split_grows = split_grows.Get();
  View page_content =
      SplitLayout{
          Sidebar(app, workspace, profiles, profiles_path, tasks, notify, theme_config,
                  language_mode, system_dark, connect_profile)
              .With(cur_split_frames[0], cur_split_grows[0])
              .Key("workspace-sidebar-pane"),
          LoadingOverlay(std::move(main_content), !model.status.empty(), model.status)
              .With(cur_split_frames[1], cur_split_grows[1])
              .Key("workspace-main-pane"),
      }
          .Orientation(Axis::Horizontal)
          .OnChanged([split_frames, split_grows, kSidebarMinWidth, kSidebarMaxWidth,
                      kMainMinWidth](const std::vector<SplitChange>& changes) {
            std::array<Frame, 2> next_frames = split_frames.Get();
            std::array<Grow, 2> next_grows = split_grows.Get();
            for (const SplitChange& change : changes) {
              if (change.index < next_frames.size()) {
                next_frames[change.index] = change.frame;
                next_grows[change.index] = change.grow;
              }
            }
            next_frames[0].min_width = kSidebarMinWidth;
            next_frames[0].max_width = kSidebarMaxWidth;
            if (next_frames[0].width.has_value()) {
              next_frames[0].width =
                  std::clamp(*next_frames[0].width, kSidebarMinWidth, kSidebarMaxWidth);
            }
            next_frames[1].min_width = kMainMinWidth;
            split_frames = std::move(next_frames);
            split_grows = std::move(next_grows);
          })
          .With(Grow(1.0F));

  const float strip_h = semantic.uses_gradient_chrome ? 2.0F : 1.0F;
  View top_strip = Spacer().With(
      Grow(0.0F), Frame{.height = strip_h, .min_height = strip_h, .max_height = strip_h},
      Background{semantic.accent_gradient});

  View shell = Column{
      std::move(title_bar),
      std::move(top_strip),
      std::move(page_content),
  }.With(Grow(1.0F), CrossAlign{CrossAxisAlignment::Stretch},
         Background{VisualFill{spec.colors.background}});

  return Theme(AppThemeDefinition(resolved), std::move(shell));
}

[[huxerui::composable]] View AppShell() {
  const auto settings = UseService<SettingsModel>();
  const auto resources = UseService<Resources>();
  const LanguageMode mode = settings ? settings->language.Get() : LanguageMode::System;
  const Locale platform_locale =
      resources ? resources->Configuration().locale : Locale::Default();
  const Locale effective = EffectiveLocale(mode, platform_locale);
  SetActiveUiLanguage(ResolveUiLanguage(mode, effective.LanguageTag()));
  return ProvideEnvironment(effective, AppContent());
}

} // namespace dbpp

huxerui::View AppRoot() {
  return dbpp::AppShell();
}

const huxerui::Application application{
    AppRoot,
    {
        .window = dbpp::MainWindowOptions(),
        // The tray owns application lifetime: startup opens the window from a
        // hook task, and lightweight mode may destroy the window while the
        // tray keeps the runtime alive for a later OpenWindow rebuild.
        .create_initial_window = false,
        .show_debug_overlay = false,
        .quit_on_last_window_closed = false,
        .application_hooks = {dbpp::InstallTrayRuntime},
    },
};
