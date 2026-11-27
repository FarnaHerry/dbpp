#include "ui/ui_common.h"

#include <algorithm>
#include <cctype>

namespace dbpp {

using namespace huxerui;
using namespace dbpp::ui;

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
  bool session_read_only = false;
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
      session_read_only = open.read_only || (adbc && adbc->read_only);
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
  if (session_read_only && !IsPureReadQuery(session_dialect, text)) {
    if (notify) {
      notify(loc.Get(app::strings::toast_readonly_blocked));
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

[[huxerui::composable]] View SaveQueryDialog(
    DialogContext dialog, std::optional<SavedQuery> existing, std::string connection_key,
    std::string initial_database, std::string initial_name, std::string initial_sql,
    State<std::vector<SavedQuery>> saved_queries, std::string db_path, NotifyFn notify) {
  const auto config_db = UseService<ConfigDbService>();
  const LocalizedCatalog loc{UseService<Resources>(), UseService<SettingsModel>()};
  auto name_input = UseState(TextEditingValue::FromText(std::move(initial_name)));
  auto db_input = UseState(TextEditingValue::FromText(std::move(initial_database)));
  auto sql_input = UseState(TextEditingValue::FromText(std::move(initial_sql)));

  const auto save = [dialog, existing, connection_key, name_input, db_input, sql_input,
                     saved_queries, db_path, config_db, notify, loc] {
    const std::string name = TrimmedSql(name_input.Get().text);
    const std::string sql = TrimmedSql(sql_input.Get().text);
    if (name.empty() || sql.empty()) {
      if (notify) {
        notify(loc.Get(app::strings::toast_query_name_required));
      }
      return;
    }
    SavedQuery next{
        .id = existing ? existing->id : 0,
        .connection_key = connection_key,
        .database_name = TrimmedSql(db_input.Get().text),
        .name = name,
        .sql_text = sql,
    };
    std::vector<SavedQuery> list = saved_queries.Get();
    bool replaced = false;
    if (existing.has_value()) {
      if (auto it = std::ranges::find(list, *existing); it != list.end()) {
        *it = next;
        replaced = true;
      }
    }
    if (!replaced) {
      for (SavedQuery& item : list) {
        if (item.connection_key == next.connection_key &&
            item.database_name == next.database_name && item.name == next.name) {
          item = next;
          replaced = true;
          break;
        }
      }
    }
    if (!replaced) {
      list.push_back(next);
    }
    if (!PersistSavedQueryList(config_db, db_path, list)) {
      return;
    }
    saved_queries = std::move(list);
    dialog.Dismiss();
    if (notify) {
      notify(loc.Fmt(app::strings::toast_query_saved, name));
    }
  };

  return DialogCard(Column{
      Row{
          TintedIcon(app::images::query, UseTheme().colors.primary, 18.0F),
          Text(app::strings::save_query_dialog_title, TextRole::Title),
          Spacer(),
          ActionButton(Button(app::strings::action_cancel).OnClick([dialog] { dialog.Dismiss(); })),
          ActionButton(Button(app::strings::action_save).OnClick(save), ButtonTone::Primary),
      }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{8.0F}),
      TextField(name_input)
          .Label(app::strings::save_query_name_label)
          .Placeholder(app::strings::save_query_name_ph)
          .Variant(TextFieldVariant::Outlined)
          .LineLimits(TextFieldLineLimits::SingleLine())
          .OnChanged([name_input](const TextEditingValue& v) { name_input = v; }),
      TextField(db_input)
          .Label(app::strings::save_query_db_label)
          .Variant(TextFieldVariant::Outlined)
          .LineLimits(TextFieldLineLimits::SingleLine())
          .OnChanged([db_input](const TextEditingValue& v) { db_input = v; }),
      MonospaceField(
          TextField(sql_input)
              .Label(app::strings::save_query_sql_label)
              .Variant(TextFieldVariant::Outlined)
              .LineLimits(TextFieldLineLimits::MultiLine(5, 10))
              .OnChanged([sql_input](const TextEditingValue& v) { sql_input = v; })
              .With(Frame{.min_height = 120.0F})),
  }.With(Spacing{10.0F}, CrossAlign{CrossAxisAlignment::Stretch})
                    .With(Frame{.width = 540.0F}));
}

} // namespace dbpp
