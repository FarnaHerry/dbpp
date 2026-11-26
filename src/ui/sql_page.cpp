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

namespace {

bool HasLikelySqlParameters(std::string_view sql) {
  bool in_single = false;
  bool in_double = false;
  for (std::size_t i = 0; i < sql.size(); ++i) {
    const char ch = sql[i];
    if (in_single) {
      if (ch == '\'') in_single = false;
      continue;
    }
    if (in_double) {
      if (ch == '"') in_double = false;
      continue;
    }
    if (ch == '-' && i + 1 < sql.size() && sql[i + 1] == '-') {
      while (i < sql.size() && sql[i] != '\n') ++i;
      continue;
    }
    if (ch == '/' && i + 1 < sql.size() && sql[i + 1] == '*') {
      i += 2;
      while (i + 1 < sql.size() && !(sql[i] == '*' && sql[i + 1] == '/')) ++i;
      continue;
    }
    if (ch == '\'') {
      in_single = true;
      continue;
    }
    if (ch == '"') {
      in_double = true;
      continue;
    }
    if (ch == '?') {
      return true;
    }
    if (ch == '$' && i + 1 < sql.size() &&
        std::isdigit(static_cast<unsigned char>(sql[i + 1]))) {
      return true;
    }
  }
  return false;
}

[[huxerui::composable]] View SqlParametersDialog(
    DialogContext dialog, std::vector<QueryParameterMeta> parameters,
    std::function<void(std::vector<PreparedParamValue>)> on_execute) {
  const ThemeSpec& spec = UseTheme();
  const SemanticColors& semantic = UseEnvironment<SemanticColors>();
  auto values = UseState(std::vector<TextEditingValue>(parameters.size()));

  Views fields;
  fields.Reserve(parameters.size());
  for (std::size_t i = 0; i < parameters.size(); ++i) {
    const QueryParameterMeta& meta = parameters[i];
    TextEditingValue cur = i < values.Get().size() ? values.Get()[i] : TextEditingValue{};
    fields.Add(Column{
        Row{
            Text(meta.name)
                .Style(TextStyle{Font::System(font_size::kChip).WithWeight(FontWeight::SemiBold),
                                 spec.colors.on_surface}),
            Spacer(),
            MutedText(meta.type),
        }.With(CrossAlign{CrossAxisAlignment::Center}),
        TextField(std::move(cur))
            .Placeholder("NULL")
            .Variant(TextFieldVariant::Outlined)
            .LineLimits(TextFieldLineLimits::SingleLine())
            .OnChanged([values, i](const TextEditingValue& val) {
              values.Update([i, val](std::vector<TextEditingValue>& list) {
                if (i < list.size()) {
                  list[i] = val;
                }
              });
            }),
    }.With(Spacing{4.0F}, Padding{EdgeInsets::Symmetric(8.0F, 6.0F)}, CornerRadius{6.0F},
           Background{VisualFill{spec.colors.surface}}, Border{semantic.border_subtle, 1.0F})
     .Key("sql-param-" + std::to_string(i)));
  }

  return DialogCard(Column{
      Text(app::strings::sql_params_title, TextRole::Title),
      MutedText(UseString(app::strings::sql_params_subtitle)),
      ScrollView(Column{std::move(fields)}
                     .With(Spacing{6.0F}, CrossAlign{CrossAxisAlignment::Stretch}))
          .With(Frame{.max_height = 320.0F}),
      Row{
          Spacer(),
          ActionButton(Button(app::strings::action_cancel).OnClick([dialog] { dialog.Dismiss(); })),
          ActionButton(Button(app::strings::sql_params_run).OnClick([dialog, values, on_execute] {
                         const auto& raw = values.Get();
                         std::vector<PreparedParamValue> params;
                         params.reserve(raw.size());
                         for (const TextEditingValue& v : raw) {
                           const bool is_null = v.text.empty() || v.text == "NULL" || v.text == "null";
                           params.push_back(PreparedParamValue{
                               .text = is_null ? std::string{} : v.text,
                               .is_null = is_null,
                           });
                         }
                         dialog.Dismiss();
                         if (on_execute) {
                           on_execute(std::move(params));
                         }
                       }),
                       ButtonTone::Primary),
      }.With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Center}),
  }.With(Spacing{10.0F}, CrossAlign{CrossAxisAlignment::Stretch})
                    .With(Frame{.width = 460.0F}));
}

} // namespace

void ExecuteSqlTabQuery(const State<AppModel>& app, const State<WorkspaceModel>& workspace,
                        const std::shared_ptr<SettingsModel>& settings, std::uint64_t tab_id,
                        const TaskScope& tasks, NotifyFn notify, const LocalizedCatalog& loc,
                        std::int64_t requested_page, bool from_editor,
                        std::int64_t override_page_size,
                        std::vector<PreparedParamValue> bound_params) {
  const WorkspaceTab* cur_tab = workspace.Get().FindTab(tab_id);
  if (cur_tab == nullptr || cur_tab->kind != WorkspaceTabKind::Sql) {
    return;
  }
  if (!from_editor && !cur_tab->sql.paginated) {
    return;
  }
  const std::string text =
      TrimmedSql(from_editor ? cur_tab->sql.editor.text : cur_tab->sql.last_sql);
  if (text.empty()) {
    return;
  }
  const std::int64_t clamped_page =
      from_editor ? 0 : std::max<std::int64_t>(0, requested_page);
  const std::uint64_t session_id = cur_tab->session_id;
  std::shared_ptr<AdbcSession> adbc;
  bool session_found = false;
  bool session_is_redis = false;
  Dialect session_dialect = Dialect::Generic;
  std::string target_database = TrimmedSql(
      from_editor
          ? cur_tab->sql.target_database
          : (cur_tab->sql.last_database.empty() ? cur_tab->sql.target_database
                                                : cur_tab->sql.last_database));
  for (const DbSession& open : app.Get().sessions) {
    if (open.id == session_id) {
      session_found = true;
      adbc = open.session;
      session_dialect = open.dialect;
      session_is_redis = (open.dialect == Dialect::Redis);
      if (target_database.empty()) {
        target_database = DefaultSessionDatabase(open);
      }
      break;
    }
  }
  if (!adbc || adbc->IsClosed()) {
    if (session_found) {
      HandleSessionDisconnected(app, workspace, tasks, session_id, {}, notify);
    } else if (notify) {
      notify(loc.Get(app::strings::toast_connection_closed));
    }
    return;
  }

  const std::int64_t configured_limit =
      settings ? settings->default_page_size.Get() : kDefaultPageSize;
  const std::int64_t active_limit =
      override_page_size > 0
          ? std::clamp(override_page_size, kMinPageSize, kMaxPageSize)
          : (cur_tab->sql.page_size > 0 ? cur_tab->sql.page_size : configured_limit);

  const std::string running_status = loc.Get(app::strings::sql_running);
  workspace.Update([tab_id, running_status, active_limit](WorkspaceModel& ws) {
    if (WorkspaceTab* target = ws.FindTab(tab_id)) {
      target->sql.page_size = active_limit;
      target->sql.running = true;
      target->sql.error.clear();
      target->sql.status = running_status;
    }
  });

  tasks.Launch([app, workspace, tab_id, tasks, notify, adbc, session_id, session_dialect,
                session_is_redis, text, target_database, active_limit, clamped_page,
                from_editor, loc, bound_params = std::move(bound_params)]() mutable -> Task<void> {
    if (session_is_redis || IsSqlReadStatement(text)) {
      const SqlLimitRewriteResult rewritten =
          (session_is_redis || !bound_params.empty())
              ? SqlLimitRewriteResult{}
              : ApplyImplicitSqlLimit(session_dialect, text, active_limit, clamped_page);
      const std::string exec_sql = rewritten.applied ? rewritten.sql : text;
      const std::size_t max_rows =
          session_is_redis
              ? kMaxQueryRows
              : (rewritten.applied
                     ? static_cast<std::size_t>(rewritten.effective_limit)
                     : std::max<std::size_t>(kMaxQueryRows,
                                             static_cast<std::size_t>(active_limit)));
      AdbcQueryResult result =
          !bound_params.empty()
              ? co_await ExecutePreparedQueryAsync(adbc, exec_sql, std::move(bound_params), true,
                                                   max_rows, target_database)
              : co_await ExecuteQueryAsync(adbc, exec_sql, true, max_rows, target_database);
      if (result.connection_lost) {
        const std::string message = result.error;
        workspace.Update([tab_id](WorkspaceModel& ws) {
          if (WorkspaceTab* target = ws.FindTab(tab_id)) {
            target->sql.running = false;
            target->sql.error.clear();
            target->sql.status.clear();
          }
        });
        HandleSessionDisconnected(app, workspace, tasks, session_id, message, notify);
        co_return;
      }
      if (!result.error.empty()) {
        const std::string message = result.error;
        const std::int64_t elapsed_us = result.elapsed_us;
        workspace.Update([tab_id, exec_sql, elapsed_us](WorkspaceModel& ws) {
          if (WorkspaceTab* target = ws.FindTab(tab_id)) {
            target->sql.last_executed_sql = exec_sql;
            target->sql.last_elapsed_us = elapsed_us;
            target->sql.running = false;
            target->sql.error.clear();
            target->sql.status.clear();
          }
        });
        if (notify) {
          if (result.cancelled) {
            notify(loc.Get(app::strings::sql_cancelled));
          } else {
            notify(loc.Fmt(session_is_redis ? app::strings::sql_cmd_failed
                                            : app::strings::sql_query_failed,
                           message));
          }
        }
        co_return;
      }
      auto query = std::make_shared<QueryResult>();
      query->columns = std::move(result.columns);
      query->snapshot =
          std::make_shared<const GridSnapshot>(GridSnapshot{.rows = std::move(result.rows)});
      query->truncated = result.truncated;
      const std::int64_t elapsed_us = result.elapsed_us;
      std::unordered_map<std::string, float> auto_widths;
      auto_widths.reserve(query->columns.size());
      for (std::size_t i = 0; i < query->columns.size(); ++i) {
        auto_widths.emplace("result-" + std::to_string(i),
                            ComputeAutoColumnWidth(query->columns[i], i, query->snapshot.get()));
      }
      const std::size_t rows = query->snapshot->rows.size();
      const std::string returned_status =
          rewritten.applied
              ? loc.Fmt(query->truncated ? app::strings::sql_returned_rows_page_more
                                         : app::strings::sql_returned_rows_page,
                        rows)
              : loc.Fmt(query->truncated ? app::strings::sql_returned_rows_truncated
                                         : app::strings::sql_returned_rows,
                        rows);
      const std::string redis_current_db =
          (session_is_redis && adbc && adbc->redis)
              ? ("db" + std::to_string(adbc->redis->CurrentDb()))
              : std::string{};
      workspace.Update([tab_id, query, returned_status, redis_current_db, rewritten, active_limit,
                        clamped_page, from_editor, text, exec_sql, elapsed_us, target_database,
                        auto_widths = std::move(auto_widths)](WorkspaceModel& ws) mutable {
        if (WorkspaceTab* target = ws.FindTab(tab_id)) {
          query->id = target->sql.next_result_id++;
          const bool keep_widths = !from_editor && target->sql.result &&
                                   target->sql.result->columns == query->columns;
          target->sql.result = query;
          if (!keep_widths) {
            target->sql.column_widths = std::move(auto_widths);
          }
          target->sql.page_size = active_limit;
          target->sql.page = rewritten.applied ? clamped_page : 0;
          target->sql.paginated = rewritten.applied;
          target->sql.has_next_page = rewritten.applied && query->truncated;
          target->sql.last_sql = rewritten.applied ? text : std::string{};
          target->sql.last_database = rewritten.applied ? target_database : std::string{};
          target->sql.last_executed_sql = exec_sql;
          target->sql.last_elapsed_us = elapsed_us;
          target->sql.running = false;
          target->sql.error.clear();
          target->sql.status = returned_status;
          if (!redis_current_db.empty()) {
            target->sql.target_database = redis_current_db;
          }
        }
      });
      if (session_is_redis && !IsReadStatement(Dialect::Redis, text)) {
        RefreshSessionObjects(app, workspace, tasks, session_id, notify, false);
      }
      co_return;
    }

    AdbcQueryResult executed =
        !bound_params.empty()
            ? co_await ExecutePreparedQueryAsync(adbc, text, std::move(bound_params), false, 0,
                                                 target_database)
            : co_await ExecuteQueryAsync(adbc, text, false, 0, target_database);
    if (executed.connection_lost) {
      const std::string message = executed.error;
      workspace.Update([tab_id](WorkspaceModel& ws) {
        if (WorkspaceTab* target = ws.FindTab(tab_id)) {
          target->sql.running = false;
          target->sql.error.clear();
          target->sql.status.clear();
        }
      });
      HandleSessionDisconnected(app, workspace, tasks, session_id, message, notify);
      co_return;
    }
    if (!executed.error.empty()) {
      const std::string message = executed.error;
      const std::int64_t elapsed_us = executed.elapsed_us;
      workspace.Update([tab_id, text, elapsed_us](WorkspaceModel& ws) {
        if (WorkspaceTab* target = ws.FindTab(tab_id)) {
          target->sql.last_executed_sql = text;
          target->sql.last_elapsed_us = elapsed_us;
          target->sql.running = false;
          target->sql.error.clear();
          target->sql.status.clear();
        }
      });
      if (notify) {
        notify(loc.Fmt(app::strings::sql_query_failed, message));
      }
      co_return;
    }

    const std::int64_t elapsed_us = executed.elapsed_us;
    const std::string done_status =
        executed.affected >= 0
            ? loc.Fmt(app::strings::sql_affected_rows, executed.affected)
            : loc.Get(app::strings::sql_exec_done);
    workspace.Update([tab_id, done_status, text, elapsed_us](WorkspaceModel& ws) {
      if (WorkspaceTab* target = ws.FindTab(tab_id)) {
        target->sql.result.reset();
        target->sql.column_widths.clear();
        target->sql.page = 0;
        target->sql.paginated = false;
        target->sql.has_next_page = false;
        target->sql.last_sql.clear();
        target->sql.last_database.clear();
        target->sql.last_executed_sql = text;
        target->sql.last_elapsed_us = elapsed_us;
        target->sql.running = false;
        target->sql.error.clear();
        target->sql.status = done_status;
      }
    });
    RefreshSessionObjects(app, workspace, tasks, session_id, notify, false);
    if (notify) {
      notify(loc.Fmt(app::strings::sql_toast_affected, executed.affected));
    }
  });
}

[[huxerui::composable]] View SqlPage(State<AppModel> app, State<WorkspaceModel> workspace,
                                     State<std::vector<ConnectionProfile>> profiles,
                                     std::uint64_t tab_id, TaskScope tasks, NotifyFn notify,
                                     AppThemeConfig theme_config, LanguageMode language_mode,
                                     bool system_dark) {
  const ThemeSpec& spec = UseTheme();
  const SemanticColors& semantic = UseEnvironment<SemanticColors>();
  const LocalizedCatalog loc{UseService<Resources>(), UseService<SettingsModel>()};
  const auto settings = UseService<SettingsModel>();
  const auto picker = UseService<FilePicker>();
  const auto dialogs = UseDialog();
  const auto menu = UseMenu();
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
  const auto run = [execute_query, workspace, tab_id, session_connected, is_redis,
                    adbc_ptr = session_connected ? session->session : std::shared_ptr<AdbcSession>{},
                    tasks, dialogs, theme_config, language_mode, system_dark] {
    const WorkspaceTab* cur = workspace.Get().FindTab(tab_id);
    if (cur == nullptr || !session_connected || !adbc_ptr) {
      return;
    }
    const std::string sql_text = TrimmedSql(cur->sql.editor.text);
    if (sql_text.empty()) {
      return;
    }
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
                spec.colors.on_surface_variant),
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
            bool inserted_sys_sep = false;
            for (const std::string& db : dbs) {
              if (!is_redis_sess && has_non_sys && !inserted_sys_sep &&
                  IsSystemDatabaseOrSchema(db)) {
                entries.emplace_back(MenuSection{});
                inserted_sys_sep = true;
              }
              entries.emplace_back(
                  MenuItem(db,
                           [app, workspace, tab_id, db] {
                             (void)SwitchSqlTabDatabase(app, workspace, tab_id, db);
                           })
                      .Checked(db == current_db));
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

  return Column{
      Row{
          DatabaseIcon(current_brand, 18.0F),
          Text(is_redis ? app::strings::sql_title_redis : app::strings::sql_title_sql,
               TextRole::Title),
          Spacer(),
          std::move(connection_selector),
          std::move(database_selector),
          (!is_redis && session_connected)
              ? CompactActionButton(
                    Button(in_tx_now ? app::strings::sql_tx_manual : app::strings::sql_tx_auto)
                        .OnClick([menu, tx_anchor, live_adbc, tasks, notify, loc, in_tx_now,
                                  tx_active_state] {
                          std::vector<MenuEntry> entries;
                          if (!in_tx_now) {
                            entries.emplace_back(
                                MenuItem(loc.Get(app::strings::sql_tx_begin),
                                         [live_adbc, tasks, notify, loc, tx_active_state] {
                                           tasks.Launch([live_adbc, notify, loc,
                                                         tx_active_state]() -> Task<void> {
                                             std::string err;
                                             const bool ok = co_await live_adbc->sequence.Run(
                                                 [live_adbc, &err](std::stop_token) {
                                                   return SetAutocommitBlocking(*live_adbc, false,
                                                                                err);
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
                                MenuItem(loc.Get(app::strings::sql_tx_commit),
                                         [live_adbc, tasks, notify, loc, tx_active_state] {
                                           tasks.Launch([live_adbc, notify, loc,
                                                         tx_active_state]() -> Task<void> {
                                             std::string err;
                                             const bool ok = co_await live_adbc->sequence.Run(
                                                 [live_adbc, &err](std::stop_token) {
                                                   if (!CommitTransactionBlocking(*live_adbc, err)) {
                                                     return false;
                                                   }
                                                   return SetAutocommitBlocking(*live_adbc, true,
                                                                                err);
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
                                MenuItem(loc.Get(app::strings::sql_tx_rollback),
                                         [live_adbc, tasks, notify, loc, tx_active_state] {
                                           tasks.Launch([live_adbc, notify, loc,
                                                         tx_active_state]() -> Task<void> {
                                             std::string err;
                                             const bool ok = co_await live_adbc->sequence.Run(
                                                 [live_adbc, &err](std::stop_token) {
                                                   if (!RollbackTransactionBlocking(*live_adbc,
                                                                                  err)) {
                                                     return false;
                                                   }
                                                   return SetAutocommitBlocking(*live_adbc, true,
                                                                                err);
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
              : View{},
          (!is_redis && session_connected)
              ? CompactActionButton(
                    Button(app::strings::sql_export)
                        .OnClick([menu, export_anchor, live_adbc, workspace, tab_id, picker, tasks,
                                  notify, loc] {
                          const WorkspaceTab* cur = workspace.Get().FindTab(tab_id);
                          const std::string sql_text =
                              cur != nullptr ? TrimmedSql(cur->sql.editor.text) : std::string{};
                          if (sql_text.empty()) {
                            return;
                          }
                          std::vector<MenuEntry> entries;
                          entries.emplace_back(MenuItem(
                              loc.Get(app::strings::sql_export_csv),
                              [live_adbc, sql_text, picker, tasks, notify, loc] {
                                ExportSqlWithPicker(live_adbc, sql_text, ExportFormat::Csv,
                                                    "query.csv", picker, tasks, notify, loc);
                              }));
                          entries.emplace_back(MenuItem(
                              loc.Get(app::strings::sql_export_jsonl),
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
              : View{},
          (sql_model.running && !is_redis)
              ? CompactActionButton(Button(app::strings::sql_cancel).OnClick(cancel_running),
                                    ButtonTone::Danger, 28.0F, 10.0F)
              : View{},
          CompactActionButton(
              Button(sql_model.running ? app::strings::sql_running : app::strings::sql_run)
                  .OnClick(run)
                  .With(Enabled{!sql_model.running && session_connected}),
              ButtonTone::Primary, 28.0F, 12.0F),
      }.With(Padding{EdgeInsets::Symmetric(6.0F, 0.0F)},
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
