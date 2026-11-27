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

[[huxerui::composable]] View SqlPage(State<AppModel> app, State<WorkspaceModel> workspace,
                                     State<std::vector<ConnectionProfile>> profiles,
                                     State<std::vector<SavedQuery>> saved_queries,
                                     std::string profiles_path, std::uint64_t tab_id,
                                     TaskScope tasks, NotifyFn notify, AppThemeConfig theme_config,
                                     LanguageMode language_mode, bool system_dark) {
  const ThemeSpec& spec = UseTheme();
  const SemanticColors& semantic = UseEnvironment<SemanticColors>();
  const LocalizedCatalog loc{UseService<Resources>(), UseService<SettingsModel>()};
  const auto settings = UseService<SettingsModel>();
  const auto picker = UseService<FilePicker>();
  const auto dialogs = UseDialog();
  const auto menu = UseMenu();
  const auto clipboard = UseApplication().Clipboard();
  const auto limit_popup = UsePopup();
  const auto conn_anchor = UseViewHandle();
  const auto db_anchor = UseViewHandle();
  const auto tx_anchor = UseViewHandle();
  const auto export_anchor = UseViewHandle();
  const auto limit_anchor = UseViewHandle();
  auto tx_active_state = UseState(false);
  const WorkspaceTab* tab = workspace.Get().FindTab(tab_id);
  if (tab == nullptr || tab->kind != WorkspaceTabKind::Sql) {
    return TableEmptyState(app::strings::sql_tab_closed);
  }
  const SqlModel& sql_model = tab->sql;
  const std::vector<DbSession>& all_sessions = app.Get().sessions;
  const DbSession* session = nullptr;
  bool has_live_session = false;
  for (const DbSession& open : all_sessions) {
    if (open.session && !open.session->IsClosed()) {
      has_live_session = true;
    }
    if (open.id == tab->session_id) {
      session = &open;
    }
  }

  const bool session_connected =
      session != nullptr && session->session && !session->session->IsClosed();
  if (!session_connected && !has_live_session && profiles.Get().empty()) {
    return TableEmptyState(app::strings::sql_conn_disconnected);
  }
  const bool is_redis = session != nullptr && session->dialect == Dialect::Redis;
  const DatabaseBrand current_brand =
      session != nullptr
          ? DetectDatabaseBrand(session->driver_path, session->uri, session->dialect)
          : (!tab->session_uri.empty()
                 ? DetectDatabaseBrand({}, tab->session_uri, Dialect::Generic)
                 : DatabaseBrand::Custom);
  const std::string offline_tag = loc.Get(app::strings::sql_conn_offline_tag);
  const std::string selector_label =
      session_connected
          ? session->name
          : (session != nullptr
                 ? session->name + " (" + offline_tag + ")"
                 : (!tab->session_name.empty()
                        ? tab->session_name + " (" + offline_tag + ")"
                        : loc.Get(app::strings::sql_select_connection)));

  const auto execute_query = [app, workspace, settings, tab_id, tasks, notify,
                              loc](std::int64_t requested_page, bool from_editor,
                                   std::int64_t override_page_size,
                                   std::vector<PreparedParamValue> bound_params = {}) {
    ExecuteSqlTabQuery(app, workspace, settings, tab_id, tasks, notify, loc, requested_page,
                       from_editor, override_page_size, std::move(bound_params));
  };
  const bool session_read_only = session != nullptr && session->read_only;
  const bool session_production = session != nullptr && session->production_mode;
  const Dialect active_dialect = session != nullptr ? session->dialect : Dialect::Generic;
  const std::string active_session_name =
      session != nullptr ? session->name : tab->session_name;
  const std::string conn_key =
      session != nullptr ? SavedQueryConnectionKey(session->driver_path, session->uri)
                         : std::string{};

  const auto run = [execute_query, workspace, tab_id, session_connected, is_redis,
                    session_read_only, session_production, active_dialect, active_session_name,
                    adbc_ptr = session_connected ? session->session : std::shared_ptr<AdbcSession>{},
                    tasks, dialogs, notify, loc, theme_config, language_mode, system_dark] {
    const WorkspaceTab* cur = workspace.Get().FindTab(tab_id);
    if (cur == nullptr || !session_connected || !adbc_ptr) {
      return;
    }
    const std::string sql_text = TrimmedSql(cur->sql.editor.text);
    if (sql_text.empty()) {
      return;
    }
    const bool is_write = !IsPureReadQuery(active_dialect, sql_text);
    const auto proceed_execution = [execute_query, is_redis, adbc_ptr, sql_text, tasks, dialogs,
                                    theme_config, language_mode, system_dark] {
      if (!is_redis && HasLikelySqlParameters(sql_text)) {
        tasks.Launch([adbc_ptr, sql_text, execute_query, dialogs, theme_config, language_mode,
                      system_dark]() -> Task<void> {
          const AdbcParamSchemaResult param_res = co_await adbc_ptr->sequence.Run(
              [adbc_ptr, sql_text](std::stop_token) {
                return InspectStatementParametersBlocking(*adbc_ptr, sql_text);
              });
          if (!param_res.supported || param_res.parameters.empty()) {
            execute_query(0, true, 0, {});
            co_return;
          }
          dialogs.Show([params = param_res.parameters, execute_query, theme_config, language_mode,
                        system_dark](DialogContext dialog) {
            return WithDialogTheme(
                theme_config, language_mode, system_dark,
                SqlParametersDialog(dialog, params,
                                    [execute_query](std::vector<PreparedParamValue> bound) {
                                      execute_query(0, true, 0, std::move(bound));
                                    }));
          });
        });
        return;
      }
      execute_query(0, true, 0, {});
    };

    if (is_write) {
      RunGuardedWrite(
          WriteGuardContext{
              .read_only = session_read_only,
              .production_mode = session_production,
              .connection_name = active_session_name,
              .dialogs = dialogs,
              .notify = notify,
              .loc = loc,
              .theme_config = theme_config,
              .language_mode = language_mode,
              .system_dark = system_dark,
          },
          CompactSqlPreview(sql_text, 240), proceed_execution);
      return;
    }
    proceed_execution();
  };
  const auto cancel_running = [adbc_ptr = session_connected ? session->session
                                                            : std::shared_ptr<AdbcSession>{}] {
    if (adbc_ptr) {
      (void)adbc_ptr->CancelActiveOperation();
    }
  };
  const std::shared_ptr<AdbcSession> live_adbc =
      session_connected ? session->session : std::shared_ptr<AdbcSession>{};
  const bool in_tx_now = live_adbc ? live_adbc->in_transaction : tx_active_state.Get();

  View result_view;
  if (sql_model.running && !sql_model.result) {
    result_view = LoadingState(app::strings::sql_running);
  } else {
    View base_result;
    if (sql_model.result) {
      const std::shared_ptr<const GridSnapshot> snapshot = sql_model.result->snapshot;
      if (snapshot && !sql_model.result->columns.empty()) {
        const std::uint64_t result_id = sql_model.result->id;
        DataTableOptions options{
            .rows = MakeRows(snapshot),
            .columns = BuildReadOnlyColumns(
                sql_model.result->columns, snapshot, sql_model.column_widths,
                spec.colors.on_surface, semantic.null_text,
                sql_model.paginated ? SafePageOffset(sql_model.page,
                                                     sql_model.page_size > 0
                                                         ? sql_model.page_size
                                                         : (settings ? settings->default_page_size.Get()
                                                                     : kDefaultPageSize))
                                    : 0,
                spec.colors.on_surface_variant,
                [snapshot, menu, clipboard, notify, loc](Point pos, std::size_t row_idx,
                                                         std::size_t col_idx) {
                  const GridRow* gr = RowAt(snapshot, row_idx);
                  if (gr == nullptr || col_idx >= gr->values.size()) {
                    return;
                  }
                  const CellValue& cv = gr->values[col_idx];
                  const std::string cell_display =
                      cv.is_null ? std::string{"NULL"} : cv.DisplayText();
                  std::string row_tsv;
                  for (std::size_t ci = 0; ci < gr->values.size(); ++ci) {
                    if (ci > 0) row_tsv.push_back('\t');
                    row_tsv +=
                        gr->values[ci].is_null ? std::string{"NULL"} : gr->values[ci].DisplayText();
                  }
                  ShowReadOnlyCellContextMenu(pos, menu, clipboard, cell_display, row_tsv, notify,
                                              loc);
                }),
            .row_height = 26.0F,
            .header_height = 28.0F,
            .frozen_column_count = 0,
            .sort = std::nullopt,
            .selected_cell = std::nullopt,
        };
        base_result = DataTable(std::move(options))
                          .On<DataTableEvents::ColumnResized>(
                              [workspace, tab_id, result_id](const DataTableColumnResize& resize) {
                                workspace.Update([&](WorkspaceModel& ws) {
                                  if (WorkspaceTab* target = ws.FindTab(tab_id);
                                      target != nullptr && target->sql.result &&
                                      target->sql.result->id == result_id) {
                                    target->sql.column_widths[resize.column_id] = resize.width;
                                  }
                                });
                              })
                          .With(Border{semantic.border_subtle, 1.0F})
                          .Key("sql-result-" + std::to_string(result_id));
      } else {
        base_result = TableEmptyState(sql_model.status);
      }
    } else {
      base_result = TableEmptyState(app::strings::sql_empty_prompt);
    }
    result_view = LoadingOverlay(std::move(base_result), sql_model.running, app::strings::sql_running)
                      .With(Grow(1.0F));
  }

  const std::uint64_t cur_session_id = tab->session_id;
  View connection_selector =
      Row{
          DatabaseIcon(current_brand, 15.0F),
          Text(selector_label)
              .Style(TextStyle{Font::System(font_size::kChip).WithWeight(FontWeight::Medium),
                               session_connected ? spec.colors.on_surface
                                                 : spec.colors.on_surface_variant})
              .LineLimit(1)
              .Overflow(TextOverflow::Ellipsis)
              .With(Frame{.max_width = 220.0F}),
          TintedIcon(app::images::chevron_down, spec.colors.on_surface_variant, 13.0F),
      }
          .With(Frame{.height = 28.0F, .min_height = 28.0F, .max_height = 28.0F},
                Padding{EdgeInsets::Symmetric(8.0F, 0.0F)},
                Spacing{5.0F}, CrossAlign{CrossAxisAlignment::Center}, CornerRadius{6.0F},
                Background{VisualFill{spec.colors.surface}}, Border{semantic.border_subtle, 1.0F},
                conn_anchor, Enabled{!sql_model.running})
          .OnClick([menu, conn_anchor, app, workspace, profiles, tasks, notify, loc, tab_id,
                    cur_session_id] {
            const std::vector<DbSession>& live_pool = app.Get().sessions;
            std::unordered_map<std::string, int> name_counts;
            std::vector<bool> claimed(live_pool.size(), false);
            bool any_live = false;
            for (std::size_t i = 0; i < live_pool.size(); ++i) {
              const DbSession& s = live_pool[i];
              if (!s.session || s.session->IsClosed()) {
                claimed[i] = true;
                continue;
              }
              any_live = true;
              ++name_counts[s.name];
            }

            std::vector<MenuEntry> entries;
            for (const DbSession& s : live_pool) {
              if (!s.session || s.session->IsClosed()) {
                continue;
              }
              std::string item_label = s.name;
              if (name_counts[s.name] > 1) {
                const std::string uri_name = UriDisplayName(s.uri);
                if (!uri_name.empty()) {
                  item_label += " (" + uri_name + ")";
                }
              }
              const std::uint64_t sid = s.id;
              entries.emplace_back(
                  MenuItem(std::move(item_label),
                           [app, workspace, tab_id, sid] {
                             (void)SwitchSqlTabSession(app, workspace, tab_id, sid);
                           })
                      .Checked(sid == cur_session_id));
            }

            const std::vector<ConnectionProfile>& saved_profiles = profiles.Get();
            const std::string offline = loc.Get(app::strings::sql_conn_offline_tag);
            bool added_section = false;
            for (const ConnectionProfile& profile : saved_profiles) {
              if (const auto match = FindMatchingSessionIndex(profile, live_pool, claimed)) {
                claimed[*match] = true;
                continue;
              }
              if (any_live && !added_section) {
                entries.emplace_back(MenuSection{});
                added_section = true;
              }
              const SanitizedUriResult sanitized = SanitizeUriCredentials(profile.uri);
              const std::string display =
                  profile.name.empty() ? UriDisplayName(sanitized.uri) : profile.name;
              entries.emplace_back(MenuItem(
                  display + " (" + offline + ")",
                  [app, workspace, tasks, notify, tab_id, profile, sanitized, display] {
                    ConnectionRequest request{
                        .driver_path = profile.driver_path,
                        .uri = sanitized.uri,
                        .username = profile.username.empty() ? sanitized.username : profile.username,
                        .password = profile.password.empty() ? sanitized.password : profile.password,
                        .title = display,
                        .remark = profile.remark,
                        .read_only = profile.read_only,
                        .production_mode = profile.production_mode,
                        .show_system_databases = profile.show_system_databases,
                    };
                    OpenConnection(
                        app, workspace, tasks, std::move(request), notify,
                        [app, workspace, tab_id](std::uint64_t sid) {
                          (void)SwitchSqlTabSession(app, workspace, tab_id, sid);
                        });
                  }));
            }

            if (entries.empty()) {
              return;
            }
            menu.Show(conn_anchor, std::move(entries),
                      MenuOptions{
                          .placement = {.side = AnchorSide::Below,
                                        .alignment = AnchorAlignment::End},
                          .gap = 4.0F,
                          .viewport_margin = 8.0F,
                          .offset = {},
                          .width = std::nullopt,
                          .dismiss_on_outside_press = true,
                          .dismiss_on_cancel = true,
                          .on_dismiss_request = {},
                      });
          });

  const std::vector<std::string> session_dbs =
      session_connected ? ListSessionDatabases(*session) : std::vector<std::string>{};
  const std::string active_db =
      !sql_model.target_database.empty()
          ? sql_model.target_database
          : (session_connected ? DefaultSessionDatabase(*session) : std::string{});
  const std::string db_selector_label =
      active_db.empty() ? loc.Get(app::strings::sql_select_database) : active_db;
  View database_selector =
      Row{
          TintedIcon(app::images::database,
                     session_connected ? semantic.accent_database : spec.colors.on_surface_variant,
                     14.0F),
          Text(db_selector_label)
              .Style(TextStyle{Font::System(font_size::kChip).WithWeight(FontWeight::Medium),
                               session_connected && !active_db.empty()
                                   ? spec.colors.on_surface
                                   : spec.colors.on_surface_variant})
              .LineLimit(1)
              .Overflow(TextOverflow::Ellipsis)
              .With(Frame{.max_width = 180.0F}),
          TintedIcon(app::images::chevron_down, spec.colors.on_surface_variant, 13.0F),
      }
          .With(Frame{.height = 28.0F, .min_height = 28.0F, .max_height = 28.0F},
                Padding{EdgeInsets::Symmetric(8.0F, 0.0F)},
                Spacing{5.0F}, CrossAlign{CrossAxisAlignment::Center}, CornerRadius{6.0F},
                Background{VisualFill{spec.colors.surface}}, Border{semantic.border_subtle, 1.0F},
                db_anchor, Enabled{!sql_model.running && session_connected && !session_dbs.empty()})
          .OnClick([menu, db_anchor, app, workspace, tab_id] {
            const WorkspaceTab* cur_tab = workspace.Get().FindTab(tab_id);
            if (cur_tab == nullptr || cur_tab->kind != WorkspaceTabKind::Sql) {
              return;
            }
            const DbSession* live_session = nullptr;
            for (const DbSession& open : app.Get().sessions) {
              if (open.id == cur_tab->session_id && open.session && !open.session->IsClosed()) {
                live_session = &open;
                break;
              }
            }
            if (live_session == nullptr) {
              return;
            }
            const std::vector<std::string> dbs = ListSessionDatabases(*live_session);
            if (dbs.empty()) {
              return;
            }
            const std::string current_db = !cur_tab->sql.target_database.empty()
                                               ? cur_tab->sql.target_database
                                               : DefaultSessionDatabase(*live_session);
            const bool is_redis_sess = live_session->dialect == Dialect::Redis;
            bool has_non_sys = false;
            if (!is_redis_sess) {
              for (const std::string& db : dbs) {
                if (!IsSystemDatabaseOrSchema(db)) {
                  has_non_sys = true;
                  break;
                }
              }
            }
            std::vector<MenuEntry> entries;
            const bool show_sys = live_session->show_system_databases;
            for (const std::string& db : dbs) {
              const bool is_sys = !is_redis_sess && IsSystemDatabaseOrSchema(db);
              if (is_sys && !show_sys && has_non_sys && db != current_db) {
                continue;
              }
              entries.emplace_back(
                  MenuItem(app::images::database, db,
                           [app, workspace, tab_id, db] {
                             (void)SwitchSqlTabDatabase(app, workspace, tab_id, db);
                           })
                      .Checked(db == current_db));
            }
            if (entries.empty()) {
              return;
            }
            menu.Show(db_anchor, std::move(entries),
                      MenuOptions{
                          .placement = {.side = AnchorSide::Below,
                                        .alignment = AnchorAlignment::End},
                          .gap = 4.0F,
                          .viewport_margin = 8.0F,
                          .offset = {},
                          .width = std::nullopt,
                          .dismiss_on_outside_press = true,
                          .dismiss_on_cancel = true,
                          .on_dismiss_request = {},
                      });
          });

  const std::string status_display =
      (!session_connected && sql_model.status.empty())
          ? UseString(app::strings::sql_conn_disconnected)
          : sql_model.status;

  const std::int64_t effective_page_size =
      sql_model.page_size > 0
          ? sql_model.page_size
          : (settings ? settings->default_page_size.Get() : kDefaultPageSize);
  View sql_limit_selector = View{};
  if (!is_redis) {
    sql_limit_selector =
        Row{
            Text(UseString(app::strings::table_page_size_label, effective_page_size))
                .Style(TextStyle{Font::System(font_size::kCaption).WithWeight(FontWeight::Medium),
                                 spec.colors.on_surface}),
            TintedIcon(app::images::chevron_down, spec.colors.on_surface_variant, 12.0F),
        }
            .With(Frame{.height = 24.0F, .min_height = 24.0F, .max_height = 24.0F},
                  Padding{EdgeInsets::Symmetric(6.0F, 0.0F)}, Spacing{4.0F},
                  CrossAlign{CrossAxisAlignment::Center}, CornerRadius{5.0F},
                  limit_anchor, Enabled{!sql_model.running})
            .OnClick([limit_popup, limit_anchor, workspace, tab_id, settings, execute_query,
                      effective_page_size, theme_config, language_mode, system_dark] {
              const auto apply_limit = [workspace, tab_id, settings,
                                        execute_query](std::int64_t next_size) {
                const std::int64_t clamped = std::clamp(next_size, kMinPageSize, kMaxPageSize);
                enum class ReloadKind { None, LastSql, EditorSql };
                ReloadKind reload_kind = ReloadKind::None;
                workspace.Update([tab_id, clamped, &reload_kind](WorkspaceModel& ws) {
                  if (WorkspaceTab* target = ws.FindTab(tab_id);
                      target != nullptr && target->kind == WorkspaceTabKind::Sql) {
                    target->sql.page_size = clamped;
                    if (!target->sql.running) {
                      if (target->sql.paginated && !target->sql.last_sql.empty()) {
                        reload_kind = ReloadKind::LastSql;
                      } else if (target->sql.result != nullptr) {
                        const std::string editor_sql = TrimmedSql(target->sql.editor.text);
                        if (!editor_sql.empty() && IsSqlReadStatement(editor_sql)) {
                          reload_kind = ReloadKind::EditorSql;
                        }
                      }
                    }
                  }
                });
                if (settings) {
                  (void)settings->SetDefaultPageSize(clamped);
                }
                if (reload_kind == ReloadKind::LastSql) {
                  execute_query(0, false, clamped);
                } else if (reload_kind == ReloadKind::EditorSql) {
                  execute_query(0, true, clamped);
                }
              };
              limit_popup.Show(
                  limit_anchor,
                  [apply_limit, effective_page_size, theme_config, language_mode,
                   system_dark](PopupContext popup) {
                    return WithDialogTheme(
                        theme_config, language_mode, system_dark,
                        PageSizePopup(popup, effective_page_size, apply_limit));
                  },
                  PopupOptions{
                      .placement = {.side = AnchorSide::Above, .alignment = AnchorAlignment::End},
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
  }

  Views sql_footer_items;
  sql_footer_items.Add(AuditDurationBadge(sql_model.last_elapsed_us));
  sql_footer_items.Add(AuditSqlBadge(sql_model.last_executed_sql, notify));
  sql_footer_items.Add(Spacer());
  sql_footer_items.Add(Text(status_display)
                           .Style(TextStyle{Font::System(font_size::kCaption),
                                            spec.colors.on_surface_variant})
                           .LineLimit(1)
                           .Overflow(TextOverflow::Ellipsis)
                           .With(LayoutPriority(1.0F)));
  if (!is_redis) {
    sql_footer_items.Add(std::move(sql_limit_selector).With(LayoutPriority(1.0F)));
    sql_footer_items.Add(CompactPagerControls(
        sql_model.page, std::nullopt, sql_model.has_next_page,
        !sql_model.running && session_connected && sql_model.paginated,
        sql_model.result ? sql_model.result->id : 0,
        [execute_query](std::int64_t target) { execute_query(target, false, 0); }, notify));
  }
  View sql_footer = BottomStatusBar(std::move(sql_footer_items));

  const auto open_save_query = [dialogs, conn_key, active_db, workspace, tab_id, saved_queries,
                                profiles_path, notify, theme_config, language_mode, system_dark] {
    const WorkspaceTab* cur = workspace.Get().FindTab(tab_id);
    const std::string sql_text = cur != nullptr ? cur->sql.editor.text : std::string{};
    const std::string init_name =
        (cur != nullptr && !cur->title.empty() && !cur->title.starts_with("SQL"))
            ? cur->title
            : std::string{};
    dialogs.Show([conn_key, active_db, init_name, sql_text, saved_queries, profiles_path, notify,
                  theme_config, language_mode, system_dark](DialogContext dialog) {
      return WithDialogTheme(
          theme_config, language_mode, system_dark,
          SaveQueryDialog(dialog, std::nullopt, conn_key, active_db, init_name, sql_text,
                          saved_queries, profiles_path, notify));
    });
  };

  Views header_items;
  header_items.Add(DatabaseIcon(current_brand, 18.0F));
  header_items.Add(Text(is_redis ? app::strings::sql_title_redis : app::strings::sql_title_sql,
                        TextRole::Title));
  if (session_production) {
    header_items.Add(StatusBadge(loc.Get(app::strings::badge_prod), semantic.danger,
                                 semantic.danger_container, WithAlpha(semantic.danger, 0.35F))
                         .Key("sql-badge-prod"));
  }
  if (session_read_only) {
    header_items.Add(StatusBadge(loc.Get(app::strings::badge_ro), semantic.warning,
                                 semantic.warning_container, WithAlpha(semantic.warning, 0.35F))
                         .Key("sql-badge-ro"));
  }
  header_items.Add(Spacer().Key("sql-header-spacer"));
  header_items.Add(std::move(connection_selector).Key("sql-conn-selector"));
  header_items.Add(std::move(database_selector).Key("sql-db-selector"));
  if (!is_redis && session_connected && !session_read_only) {
    header_items.Add(
        CompactActionButton(
            Button(in_tx_now ? app::strings::sql_tx_manual : app::strings::sql_tx_auto)
                .OnClick([menu, tx_anchor, live_adbc, tasks, notify, loc, in_tx_now,
                          tx_active_state] {
                  std::vector<MenuEntry> entries;
                  if (!in_tx_now) {
                    entries.emplace_back(
                        MenuItem(app::images::connect, loc.Get(app::strings::sql_tx_begin),
                                 [live_adbc, tasks, notify, loc, tx_active_state] {
                                   tasks.Launch([live_adbc, notify, loc,
                                                 tx_active_state]() -> Task<void> {
                                     std::string err;
                                     const bool ok = co_await live_adbc->sequence.Run(
                                         [live_adbc, &err](std::stop_token) {
                                           return SetAutocommitBlocking(*live_adbc, false, err);
                                         });
                                     if (ok) {
                                       tx_active_state = true;
                                       if (notify) notify(loc.Get(app::strings::sql_tx_started));
                                     } else if (notify) {
                                       notify(loc.Fmt(app::strings::sql_tx_failed, err));
                                     }
                                   });
                                 }));
                  } else {
                    entries.emplace_back(
                        MenuItem(app::images::add, loc.Get(app::strings::sql_tx_commit),
                                 [live_adbc, tasks, notify, loc, tx_active_state] {
                                   tasks.Launch([live_adbc, notify, loc,
                                                 tx_active_state]() -> Task<void> {
                                     std::string err;
                                     const bool ok = co_await live_adbc->sequence.Run(
                                         [live_adbc, &err](std::stop_token) {
                                           if (!CommitTransactionBlocking(*live_adbc, err)) {
                                             return false;
                                           }
                                           return SetAutocommitBlocking(*live_adbc, true, err);
                                         });
                                     if (ok) {
                                       tx_active_state = false;
                                       if (notify) notify(loc.Get(app::strings::sql_tx_committed));
                                     } else if (notify) {
                                       notify(loc.Fmt(app::strings::sql_tx_failed, err));
                                     }
                                   });
                                 }));
                    entries.emplace_back(
                        MenuItem(app::images::disconnect, loc.Get(app::strings::sql_tx_rollback),
                                 [live_adbc, tasks, notify, loc, tx_active_state] {
                                   tasks.Launch([live_adbc, notify, loc,
                                                 tx_active_state]() -> Task<void> {
                                     std::string err;
                                     const bool ok = co_await live_adbc->sequence.Run(
                                         [live_adbc, &err](std::stop_token) {
                                           if (!RollbackTransactionBlocking(*live_adbc, err)) {
                                             return false;
                                           }
                                           return SetAutocommitBlocking(*live_adbc, true, err);
                                         });
                                     if (ok) {
                                       tx_active_state = false;
                                       if (notify) notify(loc.Get(app::strings::sql_tx_rolled_back));
                                     } else if (notify) {
                                       notify(loc.Fmt(app::strings::sql_tx_failed, err));
                                     }
                                   });
                                 }));
                  }
                  menu.Show(tx_anchor, std::move(entries),
                            MenuOptions{.placement = {.side = AnchorSide::Below,
                                                      .alignment = AnchorAlignment::End},
                                        .gap = 4.0F,
                                        .viewport_margin = 8.0F,
                                        .offset = {},
                                        .width = std::nullopt,
                                        .dismiss_on_outside_press = true,
                                        .dismiss_on_cancel = true,
                                        .on_dismiss_request = {}});
                })
                .With(Enabled{!sql_model.running}),
            in_tx_now ? ButtonTone::Primary : ButtonTone::Default, 28.0F, 10.0F)
            .With(tx_anchor)
            .Key("sql-tx-btn"));
  }
  if (!is_redis && session_connected) {
    header_items.Add(
        CompactActionButton(Button(app::strings::sql_save_query_btn).OnClick(open_save_query),
                            ButtonTone::Default, 28.0F, 10.0F)
            .Key("sql-save-btn"));
    header_items.Add(
        CompactActionButton(
            Button(app::strings::sql_export)
                .OnClick([menu, export_anchor, live_adbc, workspace, tab_id, picker, tasks, notify,
                          loc] {
                  const WorkspaceTab* cur = workspace.Get().FindTab(tab_id);
                  const std::string sql_text =
                      cur != nullptr ? TrimmedSql(cur->sql.editor.text) : std::string{};
                  if (sql_text.empty()) {
                    return;
                  }
                  std::vector<MenuEntry> entries;
                  entries.emplace_back(MenuItem(
                      app::images::external_link, loc.Get(app::strings::sql_export_csv),
                      [live_adbc, sql_text, picker, tasks, notify, loc] {
                        ExportSqlWithPicker(live_adbc, sql_text, ExportFormat::Csv, "query.csv",
                                            picker, tasks, notify, loc);
                      }));
                  entries.emplace_back(MenuItem(
                      app::images::external_link, loc.Get(app::strings::sql_export_jsonl),
                      [live_adbc, sql_text, picker, tasks, notify, loc] {
                        ExportSqlWithPicker(live_adbc, sql_text, ExportFormat::Jsonl,
                                            "query.jsonl", picker, tasks, notify, loc);
                      }));
                  menu.Show(export_anchor, std::move(entries),
                            MenuOptions{.placement = {.side = AnchorSide::Below,
                                                      .alignment = AnchorAlignment::End},
                                        .gap = 4.0F,
                                        .viewport_margin = 8.0F,
                                        .offset = {},
                                        .width = std::nullopt,
                                        .dismiss_on_outside_press = true,
                                        .dismiss_on_cancel = true,
                                        .on_dismiss_request = {}});
                })
                .With(Enabled{!sql_model.running}),
            ButtonTone::Default, 28.0F, 10.0F)
            .With(export_anchor)
            .Key("sql-export-btn"));
  }
  if (sql_model.running && !is_redis) {
    header_items.Add(CompactActionButton(Button(app::strings::sql_cancel).OnClick(cancel_running),
                                         ButtonTone::Danger, 28.0F, 10.0F)
                         .Key("sql-cancel-btn"));
  }
  header_items.Add(
      CompactActionButton(
          Button(sql_model.running ? app::strings::sql_running : app::strings::sql_run)
              .OnClick(run)
              .With(Enabled{!sql_model.running && session_connected}),
          ButtonTone::Primary, 28.0F, 12.0F)
          .Key("sql-run-btn"));

  return Column{
      Row{std::move(header_items)}
          .With(Padding{EdgeInsets::Symmetric(6.0F, 0.0F)},
                CrossAlign{CrossAxisAlignment::Center}, Spacing{4.0F}),
      MonospaceField(
          TextField(sql_model.editor)
              .Label(is_redis ? app::strings::sql_label_redis : app::strings::sql_label_sql)
              .Placeholder(is_redis ? StringVariant(app::strings::sql_ph_redis)
                                    : StringVariant("SELECT * FROM table;"))
              .Variant(TextFieldVariant::Outlined)
              .LineLimits(TextFieldLineLimits::MultiLine(4, 9))
              .OnChanged([workspace, tab_id](const TextEditingValue& value) {
                workspace.Update([&](WorkspaceModel& ws) {
                  if (WorkspaceTab* target = ws.FindTab(tab_id)) {
                    target->sql.editor = value;
                  }
                });
              })
              .With(Frame{.min_height = 96.0F}))
          .With(Padding{EdgeInsets::Symmetric(6.0F, 0.0F)}),
      MutedText(UseString(is_redis ? app::strings::sql_hint_redis : app::strings::sql_hint_sql))
          .With(Padding{EdgeInsets::Symmetric(6.0F, 0.0F)}),
      std::move(result_view),
      std::move(sql_footer),
  }.With(Grow(1.0F), Padding{EdgeInsets{.top = 4.0F}}, Spacing{4.0F},
         CrossAlign{CrossAxisAlignment::Stretch});
}

} // namespace dbpp
