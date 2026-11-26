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

[[huxerui::composable]] View ObjectSchemaDialog(DialogContext dialog,
                                                 std::shared_ptr<AdbcSession> adbc,
                                                 std::uint64_t session_id, DbObject object,
                                                 std::string display_label,
                                                 std::string qualified_name, State<AppModel> app,
                                                 State<WorkspaceModel> workspace, TaskScope tasks,
                                                 NotifyFn notify) {
  const ThemeSpec& spec = UseTheme();
  const SemanticColors& semantic = UseEnvironment<SemanticColors>();
  const LocalizedCatalog loc{UseService<Resources>(), UseService<SettingsModel>()};
  auto loading = UseState(true);
  auto columns = UseState(std::vector<ColumnMeta>(object.columns));
  auto stats = UseState(TableStatisticsResult{});
  auto error = UseState(std::string{});
  auto dialog_tasks = UseTaskScope();

  Lifecycle([dialog, dialog_tasks, adbc, session_id, object, app, workspace, tasks, loading,
             columns, stats, error, notify, loc] {
    if (!adbc || adbc->IsClosed()) {
      loading = false;
      error = loc.Get(app::strings::toast_connection_closed);
    } else {
      dialog_tasks.Launch([dialog, adbc, session_id, object, app, workspace, tasks, loading,
                           columns, stats, error, notify, loc]() -> Task<void> {
        struct SchemaAndStats {
          AdbcSchemaResult schema;
          TableStatisticsResult stats;
        };
        DbObject object_copy = object;
        SchemaAndStats loaded = co_await adbc->sequence.Run(
            [adbc, object_copy = std::move(object_copy)](std::stop_token) {
              SchemaAndStats out;
              out.schema = LoadSchemaBlocking(*adbc, object_copy);
              if (!out.schema.connection_lost) {
                out.stats = LoadTableStatisticsBlocking(*adbc, object_copy, true);
              }
              return out;
            });
        loading = false;
        if (loaded.schema.connection_lost) {
          error = loaded.schema.error;
          HandleSessionDisconnected(app, workspace, tasks, session_id, loaded.schema.error, notify);
          dialog.Dismiss();
          co_return;
        }
        if (!loaded.schema.error.empty()) {
          error = loaded.schema.error;
          if (notify) {
            notify(loc.Fmt(app::strings::toast_load_schema_failed, loaded.schema.error));
          }
          if (!loaded.schema.columns.empty()) {
            columns = std::move(loaded.schema.columns);
          }
        } else {
          columns = std::move(loaded.schema.columns);
        }
        stats = std::move(loaded.stats);
      });
    }
    return [] {};
  });

  const std::vector<ColumnMeta>& cols = columns.Get();
  const TableStatisticsResult& stats_val = stats.Get();
  View body_content;
  if (loading.Get()) {
    body_content = LoadingState(app::strings::schema_dialog_loading).With(Frame{.height = 200.0F});
  } else if (cols.empty()) {
    body_content = TableEmptyState(error.Get().empty() ? app::strings::schema_dialog_empty
                                                       : app::strings::schema_dialog_load_failed)
                       .With(Frame{.height = 140.0F});
  } else {
    Views col_rows;
    col_rows.Reserve(cols.size() + object.foreign_keys.size() + 8);
    for (std::size_t idx = 0; idx < cols.size(); ++idx) {
      const ColumnMeta& col = cols[idx];
      View row_content = Row{
          Text(col.name)
              .Style(TextStyle{Font::System(font_size::kChip).WithWeight(FontWeight::SemiBold),
                               spec.colors.on_surface})
              .LineLimit(1)
              .Overflow(TextOverflow::Ellipsis)
              .With(Grow(1.0F)),
          MutedText(TypeLabel(col)),
      }.With(Spacing{12.0F}, CrossAlign{CrossAxisAlignment::Center});

      View wrapped = col.remarks.empty()
                         ? std::move(row_content)
                         : Column{std::move(row_content), MutedText(col.remarks)}
                               .With(Spacing{2.0F}, CrossAlign{CrossAxisAlignment::Stretch});
      col_rows.Add(std::move(wrapped)
                       .With(Padding{EdgeInsets::Symmetric(8.0F, 5.0F)}, CornerRadius{5.0F},
                             Background{VisualFill{spec.colors.surface}},
                             Border{semantic.border_subtle, 1.0F})
                       .Key("schema-col-" + std::to_string(idx)));
    }

    if (!object.foreign_keys.empty()) {
      col_rows.Add(Caption(UseString(app::strings::schema_dialog_fk_section,
                                     object.foreign_keys.size()))
                       .With(Padding{EdgeInsets{.top = 6.0F}}));
      for (std::size_t fi = 0; fi < object.foreign_keys.size(); ++fi) {
        const ForeignKeyMeta& fk = object.foreign_keys[fi];
        std::string local_cols;
        for (std::size_t k = 0; k < fk.columns.size(); ++k) {
          if (k > 0) local_cols += ", ";
          local_cols += fk.columns[k];
        }
        std::string ref_cols;
        for (std::size_t k = 0; k < fk.fk_columns.size(); ++k) {
          if (k > 0) ref_cols += ", ";
          ref_cols += fk.fk_columns[k];
        }
        col_rows.Add(MutedText(UseString(app::strings::schema_dialog_fk_ref, local_cols,
                                         fk.fk_table, ref_cols))
                         .With(Padding{EdgeInsets::Symmetric(8.0F, 4.0F)}, CornerRadius{5.0F},
                               Background{VisualFill{spec.colors.surface}},
                               Border{semantic.border_subtle, 1.0F})
                         .Key("schema-fk-" + std::to_string(fi)));
      }
    }

    col_rows.Add(Caption(UseString(app::strings::schema_dialog_stats_section))
                     .With(Padding{EdgeInsets{.top = 6.0F}}));
    if (!stats_val.supported) {
      col_rows.Add(MutedText(UseString(app::strings::schema_dialog_stats_unsupported)));
    } else {
      if (stats_val.row_count.has_value()) {
        std::string rc_text = UseString(app::strings::schema_dialog_stats_row_count,
                                        std::to_string(*stats_val.row_count));
        if (stats_val.row_count_approximate) {
          rc_text += " ";
          rc_text += UseString(app::strings::schema_dialog_stats_approx);
        }
        col_rows.Add(MutedText(std::move(rc_text)));
      }
      for (std::size_t si = 0; si < stats_val.statistics.size() && si < 24; ++si) {
        const ColumnStatistic& st = stats_val.statistics[si];
        if (st.column_name.empty()) {
          continue;
        }
        col_rows.Add(Row{
            Text(st.column_name + " · " + st.statistic_name)
                .Style(TextStyle{Font::System(font_size::kCaption), spec.colors.on_surface})
                .With(Grow(1.0F)),
            MutedText(st.value_text),
        }.With(Padding{EdgeInsets::Symmetric(8.0F, 3.0F)}, CornerRadius{4.0F},
               Background{VisualFill{spec.colors.surface}},
               Border{semantic.border_subtle, 1.0F}));
      }
    }

    body_content = ScrollView(Column{std::move(col_rows)}
                                  .With(Spacing{4.0F}, CrossAlign{CrossAxisAlignment::Stretch}))
                       .With(Frame{.max_height = 380.0F});
  }

  View conn_info_banner;
  if (adbc && adbc->connection_info.supported) {
    conn_info_banner = MutedText(UseString(
        app::strings::schema_dialog_conn_info, adbc->connection_info.vendor_name,
        adbc->connection_info.vendor_version, adbc->connection_info.driver_name,
        adbc->connection_info.driver_version));
  }

  return DialogCard(Column{
      Row{
          Text::Format(TextRole::Title, app::strings::schema_dialog_title, display_label),
          Spacer(),
          ActionButton(Button(app::strings::action_close).OnClick([dialog] { dialog.Dismiss(); })),
      }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{8.0F}),
      MutedText(UseString(app::strings::schema_dialog_summary,
                          UseString(ObjectKindResource(object)), qualified_name,
                          cols.size())),
      std::move(conn_info_banner),
      std::move(body_content),
  }.With(Spacing{10.0F}, CrossAlign{CrossAxisAlignment::Stretch})
                    .With(Frame{.width = 580.0F}));
}

[[huxerui::composable]] View EditTableDialog(
    DialogContext dialog, std::shared_ptr<AdbcSession> adbc, std::uint64_t session_id,
    Dialect dialect, DbObject initial_object, std::string display_label,
    State<AppModel> app, State<WorkspaceModel> workspace, TaskScope tasks, NotifyFn notify) {
  const ThemeSpec& spec = UseTheme();
  const SemanticColors& semantic = UseEnvironment<SemanticColors>();
  const LocalizedCatalog loc{UseService<Resources>(), UseService<SettingsModel>()};
  auto dialog_tasks = UseTaskScope();
  auto current_object = UseState(initial_object);
  auto loading = UseState(true);
  auto busy = UseState(false);
  auto columns = UseState(std::vector<ColumnMeta>(initial_object.columns));
  auto error_text = UseState(std::string{});

  auto table_name_input = UseState(TextEditingValue::FromText(initial_object.name));
  auto selected_col = UseState<int>(-1);
  auto edit_col_name = UseState(TextEditingValue{});
  auto edit_col_type = UseState(TextEditingValue{});
  auto edit_col_nullable = UseState(true);

  auto add_col_name = UseState(TextEditingValue{});
  auto add_col_type = UseState(TextEditingValue::FromText("TEXT"));
  auto add_col_default = UseState(TextEditingValue{});
  auto add_col_nullable = UseState(true);
  auto preview_override = UseState(std::string{});

  const auto reload_columns = [dialog, dialog_tasks, adbc, session_id, current_object, columns,
                               selected_col, edit_col_name, edit_col_type, edit_col_nullable,
                               loading, error_text, app, workspace, tasks, notify, loc] {
    if (!adbc || adbc->IsClosed()) {
      loading = false;
      error_text = loc.Get(app::strings::toast_connection_closed);
      return;
    }
    loading = true;
    const DbObject obj = current_object.Get();
    dialog_tasks.Launch([dialog, adbc, session_id, obj, columns, selected_col, edit_col_name,
                         edit_col_type, edit_col_nullable, loading, error_text, app, workspace,
                         tasks, notify]() -> Task<void> {
      AdbcSchemaResult res = co_await adbc->sequence.Run(
          [adbc, obj](std::stop_token) { return LoadSchemaBlocking(*adbc, obj); });
      loading = false;
      if (res.connection_lost) {
        HandleSessionDisconnected(app, workspace, tasks, session_id, res.error, notify);
        dialog.Dismiss();
        co_return;
      }
      if (!res.error.empty() && res.columns.empty()) {
        error_text = res.error;
        co_return;
      }
      error_text = std::string{};
      columns = res.columns;
      const int sel = selected_col.Get();
      if (sel >= 0 && static_cast<std::size_t>(sel) < res.columns.size()) {
        const ColumnMeta& c = res.columns[static_cast<std::size_t>(sel)];
        edit_col_name = TextEditingValue::FromText(c.name);
        edit_col_type = TextEditingValue::FromText(c.type.empty() ? "ANY" : c.type);
        edit_col_nullable = c.nullable;
      } else {
        selected_col = -1;
      }
    });
  };

  Lifecycle([reload_columns] {
    reload_columns();
    return [] {};
  });

  const DbObject cur_obj = current_object.Get();
  const auto execute_ddl = [dialog, dialog_tasks, adbc, session_id, dialect, cur_obj,
                            current_object, table_name_input, add_col_name, add_col_default, busy,
                            error_text, preview_override, reload_columns, app, workspace, tasks,
                            notify, loc](std::string sql, std::optional<std::string> renamed_table) {
    if (sql.empty() || busy.Get()) {
      return;
    }
    if (!adbc || adbc->IsClosed()) {
      HandleSessionDisconnected(app, workspace, tasks, session_id, {}, notify);
      dialog.Dismiss();
      return;
    }
    busy = true;
    preview_override = sql;
    dialog_tasks.Launch([dialog, adbc, session_id, dialect, cur_obj, current_object,
                         table_name_input, add_col_name, add_col_default, busy, error_text,
                         reload_columns, sql = std::move(sql),
                         renamed_table = std::move(renamed_table), app, workspace, tasks, notify,
                         loc]() -> Task<void> {
      AdbcQueryResult res = co_await ExecuteQueryAsync(adbc, sql, false, 0);
      busy = false;
      if (res.connection_lost) {
        dialog.Dismiss();
        HandleSessionDisconnected(app, workspace, tasks, session_id, res.error, notify);
        co_return;
      }
      if (!res.error.empty()) {
        error_text = res.error;
        if (notify) {
          notify(loc.Fmt(app::strings::toast_edit_table_failed, res.error));
        }
        co_return;
      }

      error_text = std::string{};
      std::string old_name = cur_obj.name;
      std::string next_name = old_name;
      if (renamed_table.has_value() && !renamed_table->empty()) {
        next_name = *renamed_table;
        DbObject updated = cur_obj;
        updated.name = next_name;
        current_object = std::move(updated);
        table_name_input = TextEditingValue::FromText(next_name);
      } else {
        add_col_name = TextEditingValue{};
        add_col_default = TextEditingValue{};
      }

      const std::string cur_schema =
          cur_obj.db_schema.empty() ? ObjectDatabaseName(dialect, cur_obj) : cur_obj.db_schema;
      if (renamed_table.has_value()) {
        workspace.Update([&](WorkspaceModel& ws) {
          for (WorkspaceTab& tab : ws.tabs) {
            if (tab.kind == WorkspaceTabKind::Table && tab.session_id == session_id &&
                tab.object_name == old_name &&
                (tab.object_schema == cur_obj.db_schema || tab.object_schema == cur_schema) &&
                !tab.is_view) {
              tab.object_name = next_name;
              tab.table.object = next_name;
              tab.title = cur_schema.empty() ? next_name : (cur_schema + "." + next_name);
            }
          }
        });
      }

      RefreshSessionObjects(app, workspace, tasks, session_id, notify, false);
      if (!renamed_table.has_value()) {
        std::vector<std::tuple<std::uint64_t, std::int64_t, std::optional<DataTableSort>, std::string>>
            tabs_info;
        for (const WorkspaceTab& tab : workspace.Get().tabs) {
          if (tab.kind == WorkspaceTabKind::Table && tab.session_id == session_id &&
              tab.object_name == cur_obj.name &&
              (tab.object_schema == cur_obj.db_schema || tab.object_schema == cur_schema) &&
              !tab.is_view && tab.table.pending_inserts.empty()) {
            tabs_info.emplace_back(tab.id, tab.table.page, tab.table.sort, tab.table.filter);
          }
        }
        for (auto& [tid, pg, srt, flt] : tabs_info) {
          RequestLoadTable(app, workspace, tasks, tid, pg, srt, std::move(flt), false, notify);
        }
      }
      reload_columns();
      if (notify) {
        notify(loc.Get(app::strings::toast_edit_table_done));
      }
    });
  };

  const std::vector<ColumnMeta>& cols = columns.Get();
  const int sel_idx = selected_col.Get();
  const bool has_selected_col =
      sel_idx >= 0 && static_cast<std::size_t>(sel_idx) < cols.size();
  const ColumnMeta selected_meta =
      has_selected_col ? cols[static_cast<std::size_t>(sel_idx)] : ColumnMeta{};

  const std::string rename_table_sql =
      TrimmedSql(table_name_input.Get().text) != cur_obj.name
          ? BuildRenameTableSql(dialect, cur_obj, table_name_input.Get().text)
          : std::string{};
  const std::string add_col_sql =
      BuildAddColumnSql(dialect, cur_obj, add_col_name.Get().text, add_col_type.Get().text,
                        add_col_nullable.Get(), add_col_default.Get().text);
  const std::string rename_col_sql =
      has_selected_col
          ? BuildRenameColumnSql(dialect, cur_obj, selected_meta.name, edit_col_name.Get().text)
          : std::string{};
  const std::string alter_type_sql =
      has_selected_col
          ? BuildAlterColumnTypeSql(dialect, cur_obj, selected_meta.name, edit_col_type.Get().text,
                                    edit_col_nullable.Get())
          : std::string{};
  const std::string drop_col_sql =
      has_selected_col ? BuildDropColumnSql(dialect, cur_obj, selected_meta.name) : std::string{};

  std::string sql_preview = add_col_sql;
  if (sql_preview.empty()) {
    sql_preview = !rename_table_sql.empty() ? rename_table_sql
                  : (!rename_col_sql.empty() ? rename_col_sql
                     : (!preview_override.Get().empty() ? preview_override.Get() : "--"));
  }

  Views col_rows;
  col_rows.Reserve(cols.size());
  for (std::size_t idx = 0; idx < cols.size(); ++idx) {
    const ColumnMeta& col = cols[idx];
    const bool is_sel = sel_idx == static_cast<int>(idx);
    Views badges;
    if (col.primary_key) {
      badges.Add(StatusBadge(UseString(app::strings::edit_table_col_pk), spec.colors.primary,
                             spec.colors.primary_container, WithAlpha(spec.colors.primary, 0.35F)));
    }
    if (!col.nullable) {
      badges.Add(StatusBadge(UseString(app::strings::edit_table_col_not_null),
                             spec.colors.on_surface_variant, spec.colors.surface_container,
                             semantic.border_subtle));
    }
    badges.Add(MutedText(col.type.empty() ? "ANY" : col.type));

    col_rows.Add(
        NavItem(col.name, {}, is_sel,
                [selected_col, edit_col_name, edit_col_type, edit_col_nullable, col, idx] {
                  selected_col = static_cast<int>(idx);
                  edit_col_name = TextEditingValue::FromText(col.name);
                  edit_col_type = TextEditingValue::FromText(col.type.empty() ? "ANY" : col.type);
                  edit_col_nullable = col.nullable;
                },
                Row{std::move(badges)}.With(Spacing{4.0F}, CrossAlign{CrossAxisAlignment::Center}),
                std::nullopt,
                TintedIcon(app::images::table,
                           is_sel ? semantic.accent_table : spec.colors.on_surface_variant, 14.0F))
            .Key("edit-col-" + std::to_string(idx) + "-" + col.name));
  }

  View selected_panel;
  if (has_selected_col) {
    View type_row;
    if (dialect == Dialect::Sqlite) {
      type_row = MutedText(UseString(app::strings::edit_table_sqlite_type_note));
    } else {
      type_row = Row{
          TextField(edit_col_type)
              .Label(app::strings::edit_table_type_label)
              .Variant(TextFieldVariant::Outlined)
              .LineLimits(TextFieldLineLimits::SingleLine())
              .OnChanged([edit_col_type](const TextEditingValue& val) { edit_col_type = val; })
              .With(Grow(1.0F)),
          ActionButton(Button(app::strings::edit_table_alter_type_btn)
                           .OnClick([execute_ddl, alter_type_sql] {
                             execute_ddl(alter_type_sql, std::nullopt);
                           }))
              .With(Enabled{!busy.Get() && !alter_type_sql.empty()}),
      }.With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Center});
    }

    selected_panel = Column{
        Row{
            TextField(edit_col_name)
                .Label(app::strings::edit_table_rename_col_label)
                .Variant(TextFieldVariant::Outlined)
                .LineLimits(TextFieldLineLimits::SingleLine())
                .OnChanged([edit_col_name](const TextEditingValue& val) { edit_col_name = val; })
                .With(Grow(1.0F)),
            ActionButton(Button(app::strings::edit_table_rename_col_btn)
                             .OnClick([execute_ddl, rename_col_sql] {
                               execute_ddl(rename_col_sql, std::nullopt);
                             }))
                .With(Enabled{!busy.Get() && !rename_col_sql.empty()}),
            ActionButton(Button(app::strings::edit_table_drop_col_btn)
                             .OnClick([execute_ddl, drop_col_sql] {
                               execute_ddl(drop_col_sql, std::nullopt);
                             }),
                         ButtonTone::Danger)
                .With(Enabled{!busy.Get() && cols.size() > 1 && !drop_col_sql.empty()}),
        }.With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Center}),
        std::move(type_row),
        cols.size() <= 1 ? MutedText(UseString(app::strings::edit_table_drop_last_hint)) : View{},
    }.With(Padding{EdgeInsets::Symmetric(10.0F, 8.0F)}, Spacing{8.0F},
           CrossAlign{CrossAxisAlignment::Stretch}, CornerRadius{7.0F},
           Background{VisualFill{spec.colors.surface}}, Border{semantic.border_subtle, 1.0F});
  }

  View columns_body =
      loading.Get() && cols.empty()
          ? LoadingState(app::strings::schema_dialog_loading).With(Frame{.height = 170.0F})
          : LoadingOverlay(
                ScrollView(Column{std::move(col_rows)}
                               .With(Spacing{3.0F}, CrossAlign{CrossAxisAlignment::Stretch})),
                loading.Get(), app::strings::schema_dialog_loading)
                .With(Frame{.height = 170.0F});

  const std::string header_label =
      cur_obj.name.empty() ? display_label : QualifiedName(dialect, cur_obj);

  return DialogCard(LoadingOverlay(
      Column{
      Row{
          TintedIcon(app::images::table, semantic.accent_table, 18.0F),
          Text::Format(TextRole::Title, app::strings::edit_table_title, header_label),
          Spacer(),
          ActionButton(Button(app::strings::action_close).OnClick([dialog] { dialog.Dismiss(); })),
      }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{8.0F}),
      ScrollView(
          Column{
              Caption(UseString(app::strings::edit_table_rename_section)),
              Row{
                  TextField(table_name_input)
                      .Label(app::strings::edit_table_name_label)
                      .Variant(TextFieldVariant::Outlined)
                      .LineLimits(TextFieldLineLimits::SingleLine())
                      .OnChanged(
                          [table_name_input](const TextEditingValue& v) { table_name_input = v; })
                      .With(Grow(1.0F)),
                  ActionButton(Button(app::strings::edit_table_rename_btn)
                                   .OnClick([execute_ddl, rename_table_sql, table_name_input] {
                                     execute_ddl(rename_table_sql, TrimmedSql(table_name_input.Get().text));
                                   }))
                      .With(Enabled{!busy.Get() && !rename_table_sql.empty()}),
              }.With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Center}),
              Divider(),
              Caption(UseString(app::strings::edit_table_columns_section, cols.size())),
              std::move(columns_body),
              std::move(selected_panel),
              Divider(),
              Caption(UseString(app::strings::edit_table_add_section)),
              Row{
                  TextField(add_col_name)
                      .Label(app::strings::edit_table_add_name_label)
                      .Placeholder(app::strings::edit_table_add_name_ph)
                      .Variant(TextFieldVariant::Outlined)
                      .LineLimits(TextFieldLineLimits::SingleLine())
                      .OnChanged([add_col_name](const TextEditingValue& v) { add_col_name = v; })
                      .With(Grow(1.0F)),
                  TextField(add_col_type)
                      .Label(app::strings::edit_table_add_type_label)
                      .Placeholder(app::strings::edit_table_add_type_ph)
                      .Variant(TextFieldVariant::Outlined)
                      .LineLimits(TextFieldLineLimits::SingleLine())
                      .OnChanged([add_col_type](const TextEditingValue& v) { add_col_type = v; })
                      .With(Frame{.width = 160.0F}),
                  TextField(add_col_default)
                      .Label(app::strings::edit_table_add_default_label)
                      .Placeholder(app::strings::edit_table_add_default_ph)
                      .Variant(TextFieldVariant::Outlined)
                      .LineLimits(TextFieldLineLimits::SingleLine())
                      .OnChanged(
                          [add_col_default](const TextEditingValue& v) { add_col_default = v; })
                      .With(Frame{.width = 160.0F}),
              }.With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Center}),
              Row{
                  Switch(app::strings::edit_table_add_nullable, add_col_nullable)
                      .OnChanged([add_col_nullable](bool v) { add_col_nullable = v; }),
                  Spacer(),
                  ActionButton(Button(busy.Get() ? app::strings::edit_table_executing
                                                 : app::strings::edit_table_add_btn)
                                   .OnClick([execute_ddl, add_col_sql] {
                                     execute_ddl(add_col_sql, std::nullopt);
                                   }),
                               ButtonTone::Primary)
                      .With(Enabled{!busy.Get() && !add_col_sql.empty()}),
              }.With(CrossAlign{CrossAxisAlignment::Center}),
              Caption(UseString(app::strings::edit_table_sql_preview)),
              Text(sql_preview)
                  .Style(TextStyle{Font::Monospace(12.0F), spec.colors.on_surface_variant})
                  .With(Padding{EdgeInsets::Symmetric(10.0F, 8.0F)}, CornerRadius{6.0F},
                        Background{VisualFill{spec.colors.surface}},
                        Border{semantic.border_subtle, 1.0F}),
              error_text.Get().empty() ? View{} : ErrorText(error_text.Get()),
          }.With(Spacing{10.0F}, CrossAlign{CrossAxisAlignment::Stretch}))
          .With(Grow(1.0F)),
  }.With(Spacing{10.0F}, CrossAlign{CrossAxisAlignment::Stretch}),
      busy.Get(), app::strings::edit_table_executing)
                        .With(Frame{.width = 660.0F, .height = 590.0F}));
}

[[huxerui::composable]] View DropObjectDialog(
    DialogContext dialog, std::shared_ptr<AdbcSession> adbc, std::uint64_t session_id,
    DbObject object, std::string display_label, std::string drop_sql, State<AppModel> app,
    State<WorkspaceModel> workspace, TaskScope tasks, NotifyFn notify) {
  auto busy = UseState(false);
  const LocalizedCatalog loc{UseService<Resources>(), UseService<SettingsModel>()};
  const StringResource kind_res = ObjectKindResource(object);
  const std::string kind = UseString(kind_res);

  const auto confirm = [dialog, adbc, session_id, display_label, kind_res, drop_sql, app,
                        workspace, tasks, busy, notify, loc] {
    if (busy.Get()) {
      return;
    }
    if (!adbc || adbc->IsClosed()) {
      HandleSessionDisconnected(app, workspace, tasks, session_id, {}, notify);
      dialog.Dismiss();
      return;
    }
    busy = true;
    tasks.Launch([dialog, adbc, session_id, display_label, kind_res, drop_sql, app, workspace,
                  tasks, busy, notify, loc]() -> Task<void> {
      AdbcQueryResult res = co_await ExecuteQueryAsync(adbc, drop_sql, false, 0);
      if (res.connection_lost) {
        busy = false;
        dialog.Dismiss();
        HandleSessionDisconnected(app, workspace, tasks, session_id, res.error, notify);
        co_return;
      }
      if (!res.error.empty()) {
        busy = false;
        if (notify) {
          notify(loc.Fmt(app::strings::toast_drop_failed, res.error));
        }
        co_return;
      }
      dialog.Dismiss();
      RefreshSessionObjects(app, workspace, tasks, session_id, notify, false);
      if (notify) {
        notify(loc.Fmt(app::strings::toast_drop_done, loc.Get(kind_res), display_label));
      }
    });
  };

  return DialogCard(LoadingOverlay(
      Column{
          Row{
              Text::Format(TextRole::Title, app::strings::drop_dialog_title, kind),
              Spacer(),
              ActionButton(Button(app::strings::action_cancel)
                               .OnClick([dialog] { dialog.Dismiss(); })
                               .With(Enabled{!busy.Get()})),
              ActionButton(Button(busy.Get() ? app::strings::drop_dialog_confirming
                                             : app::strings::drop_dialog_confirm)
                               .OnClick(confirm)
                               .With(Enabled{!busy.Get()}),
                           ButtonTone::DangerSolid),
          }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{8.0F}),
          MutedText(UseString(app::strings::drop_dialog_warning, display_label)),
          MutedText(UseString(app::strings::drop_dialog_sql_preview, drop_sql)),
      }.With(Spacing{10.0F}, CrossAlign{CrossAxisAlignment::Stretch}),
      busy.Get(), app::strings::drop_dialog_confirming)
                        .With(Frame{.width = 520.0F}));
}

} // namespace dbpp
