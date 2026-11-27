#include "ui/ui_common.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dbpp {

using namespace huxerui;
using namespace dbpp::ui;

[[huxerui::composable]] View TableDesignPage(
    State<AppModel> app, State<WorkspaceModel> workspace, std::uint64_t tab_id,
    TaskScope tasks, NotifyFn notify, AppThemeConfig theme_config, LanguageMode language_mode,
    bool system_dark) {
  const ThemeSpec& spec = UseTheme();
  const SemanticColors& semantic = UseEnvironment<SemanticColors>();
  const LocalizedCatalog loc{UseService<Resources>(), UseService<SettingsModel>()};
  const auto dialogs = UseDialog();
  const auto menu = UseMenu();
  const auto clipboard = UseApplication().Clipboard();
  auto page_tasks = UseTaskScope();

  const WorkspaceTab* tab = workspace.Get().FindTab(tab_id);
  if (tab == nullptr || tab->kind != WorkspaceTabKind::TableDesign) {
    return TableEmptyState(app::strings::table_tab_closed);
  }
  const DbSession* session = nullptr;
  for (const DbSession& open : app.Get().sessions) {
    if (open.id == tab->session_id) {
      session = &open;
      break;
    }
  }
  if (session == nullptr || !session->session || session->session->IsClosed()) {
    return TableEmptyState(app::strings::table_conn_disconnected);
  }

  const std::shared_ptr<AdbcSession> adbc = session->session;
  const std::uint64_t session_id = session->id;
  const Dialect dialect = session->dialect;
  const bool read_only = session->read_only;
  const bool is_production = session->production_mode;
  const std::string prod_conn_name = session->name;

  // Sub-tab order: 0=DDL, 1=Columns, 2=Indexes, 3=Foreign Keys, 4=Triggers
  auto active_section = UseState<int>(1);
  auto loading = UseState(true);
  auto busy = UseState(false);
  auto current_object = UseState(tab->design_object);
  auto design_data = UseState(TableDesignSnapshot{.object = tab->design_object});
  auto error_text = UseState(std::string{});

  auto table_name_input = UseState(TextEditingValue::FromText(tab->design_object.name));
  auto selected_col_cell = UseState<std::optional<DataTableCellId>>(std::nullopt);
  auto selected_col_idx = UseState<int>(-1);
  auto edit_col_name = UseState(TextEditingValue{});
  auto edit_col_type = UseState(TextEditingValue{});
  auto edit_col_nullable = UseState(true);

  auto add_col_name = UseState(TextEditingValue{});
  auto add_col_type = UseState(TextEditingValue::FromText("TEXT"));
  auto add_col_default = UseState(TextEditingValue{});
  auto add_col_nullable = UseState(true);
  auto preview_override = UseState(std::string{});

  auto selected_trigger_cell = UseState<std::optional<DataTableCellId>>(std::nullopt);
  auto selected_trigger_idx = UseState<int>(-1);
  auto column_widths = UseState(std::unordered_map<std::string, float>{});

  const auto copy_text = [clipboard, notify, loc](const std::string& text,
                                                  StringResource label_res) {
    if (clipboard && clipboard->SupportsSynchronousText() && clipboard->WriteText(text)) {
      if (notify) {
        notify(loc.Fmt(app::strings::toast_copied, loc.Get(label_res)));
      }
    } else if (notify) {
      notify(loc.Get(app::strings::toast_clipboard_unsupported));
    }
  };

  const auto reload_design = [page_tasks, adbc, session_id, current_object, design_data,
                              selected_col_idx, selected_col_cell, edit_col_name, edit_col_type,
                              edit_col_nullable, selected_trigger_idx, selected_trigger_cell,
                              loading, error_text, app, workspace, tasks, notify,
                              loc](std::optional<DbObject> override_obj = std::nullopt) {
    if (!adbc || adbc->IsClosed()) {
      loading = false;
      error_text = loc.Get(app::strings::toast_connection_closed);
      return;
    }
    loading = true;
    const DbObject target = override_obj.has_value() ? *override_obj : current_object.Get();
    page_tasks.Launch([adbc, session_id, target, current_object, design_data, selected_col_idx,
                       selected_col_cell, edit_col_name, edit_col_type, edit_col_nullable,
                       selected_trigger_idx, selected_trigger_cell, loading, error_text, app,
                       workspace, tasks, notify]() -> Task<void> {
      TableDesignSnapshot loaded = co_await adbc->sequence.Run(
          [adbc, target](std::stop_token) { return LoadTableDesignBlocking(*adbc, target); });
      loading = false;
      if (loaded.connection_lost) {
        HandleSessionDisconnected(app, workspace, tasks, session_id, loaded.error, notify);
        co_return;
      }
      if (!loaded.error.empty() && loaded.object.columns.empty()) {
        error_text = loaded.error;
        co_return;
      }
      error_text = std::string{};
      current_object = loaded.object;
      const int cur_col = selected_col_idx.Get();
      if (cur_col >= 0 && static_cast<std::size_t>(cur_col) < loaded.object.columns.size()) {
        const ColumnMeta& c = loaded.object.columns[static_cast<std::size_t>(cur_col)];
        edit_col_name = TextEditingValue::FromText(c.name);
        edit_col_type = TextEditingValue::FromText(c.type.empty() ? "ANY" : c.type);
        edit_col_nullable = c.nullable;
      } else {
        selected_col_idx = -1;
        selected_col_cell = std::nullopt;
      }
      const int cur_trg = selected_trigger_idx.Get();
      if (cur_trg < 0 || static_cast<std::size_t>(cur_trg) >= loaded.triggers.size()) {
        selected_trigger_idx = -1;
        selected_trigger_cell = std::nullopt;
      }
      design_data = std::move(loaded);
    });
  };

  Lifecycle([reload_design] {
    reload_design(std::nullopt);
    return [] {};
  });

  const DbObject cur_obj = current_object.Get();

  const auto perform_ddl = [page_tasks, adbc, session_id, dialect, tab_id, cur_obj,
                            current_object, table_name_input, add_col_name, add_col_default, busy,
                            error_text, preview_override, reload_design, app, workspace, tasks,
                            notify, loc](std::string sql, std::optional<std::string> renamed_table) {
    if (sql.empty() || busy.Get()) {
      return;
    }
    if (adbc && adbc->read_only) {
      if (notify) {
        notify(loc.Get(app::strings::toast_readonly_blocked));
      }
      return;
    }
    if (!adbc || adbc->IsClosed()) {
      HandleSessionDisconnected(app, workspace, tasks, session_id, {}, notify);
      return;
    }
    busy = true;
    preview_override = sql;
    page_tasks.Launch([adbc, session_id, dialect, tab_id, cur_obj, current_object,
                       table_name_input, add_col_name, add_col_default, busy, error_text,
                       reload_design, sql = std::move(sql),
                       renamed_table = std::move(renamed_table), app, workspace, tasks, notify,
                       loc]() -> Task<void> {
      AdbcQueryResult res = co_await ExecuteQueryAsync(adbc, sql, false, 0);
      busy = false;
      if (res.connection_lost) {
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
      DbObject updated = cur_obj;
      if (renamed_table.has_value() && !renamed_table->empty()) {
        next_name = *renamed_table;
        updated.name = next_name;
        current_object = updated;
        table_name_input = TextEditingValue::FromText(next_name);
      } else {
        add_col_name = TextEditingValue{};
        add_col_default = TextEditingValue{};
      }

      const std::string cur_schema =
          cur_obj.db_schema.empty() ? ObjectDatabaseName(dialect, cur_obj) : cur_obj.db_schema;
      workspace.Update([&](WorkspaceModel& ws) {
        if (renamed_table.has_value() && !renamed_table->empty()) {
          for (WorkspaceTab& wtab : ws.tabs) {
            if (wtab.session_id == session_id && wtab.object_name == old_name &&
                (wtab.object_schema == cur_obj.db_schema || wtab.object_schema == cur_schema) &&
                !wtab.is_view) {
              if (wtab.kind == WorkspaceTabKind::Table) {
                wtab.object_name = next_name;
                wtab.table.object = next_name;
                wtab.title = cur_schema.empty() ? next_name : (cur_schema + "." + next_name);
              } else if (wtab.kind == WorkspaceTabKind::TableDesign) {
                const std::string disp =
                    cur_schema.empty() ? next_name : (cur_schema + "." + next_name);
                wtab.object_name = next_name;
                wtab.design_object.name = next_name;
                wtab.design_display_label = disp;
                wtab.title = TrFmt("Design · {0}", "表结构 · {0}", {disp});
              }
            }
          }
        }
      });

      RefreshSessionObjects(app, workspace, tasks, session_id, notify, false);
      if (!renamed_table.has_value()) {
        std::vector<std::tuple<std::uint64_t, std::int64_t, std::optional<DataTableSort>, std::string>>
            tabs_info;
        for (const WorkspaceTab& wtab : workspace.Get().tabs) {
          if (wtab.kind == WorkspaceTabKind::Table && wtab.session_id == session_id &&
              wtab.object_name == cur_obj.name &&
              (wtab.object_schema == cur_obj.db_schema || wtab.object_schema == cur_schema) &&
              !wtab.is_view && !wtab.table.HasPendingChanges()) {
            tabs_info.emplace_back(wtab.id, wtab.table.page, wtab.table.sort, wtab.table.filter);
          }
        }
        for (auto& [tid, pg, srt, flt] : tabs_info) {
          RequestLoadTable(app, workspace, tasks, tid, pg, srt, std::move(flt), false, notify);
        }
      }
      reload_design(updated);
      if (notify) {
        notify(loc.Get(app::strings::toast_edit_table_done));
      }
    });
  };

  const auto execute_ddl = [dialogs, is_production, prod_conn_name, read_only, busy, theme_config,
                            language_mode, system_dark, notify, loc,
                            perform_ddl](std::string sql, std::optional<std::string> renamed_table) {
    if (sql.empty() || busy.Get()) {
      return;
    }
    const std::string summary = sql;
    RunGuardedWrite(
        WriteGuardContext{
            .read_only = read_only,
            .production_mode = is_production,
            .connection_name = prod_conn_name,
            .dialogs = dialogs,
            .notify = notify,
            .loc = loc,
            .theme_config = theme_config,
            .language_mode = language_mode,
            .system_dark = system_dark,
        },
        summary,
        [perform_ddl, sql = std::move(sql), renamed_table = std::move(renamed_table)]() mutable {
          perform_ddl(std::move(sql), std::move(renamed_table));
        });
  };

  const TableDesignSnapshot& snap = design_data.Get();
  const std::vector<ColumnMeta>& cols = snap.object.columns;
  const std::vector<IndexMeta>& indexes = snap.object.indexes;
  const std::vector<ForeignKeyMeta>& fks = snap.object.foreign_keys;
  const std::vector<TriggerMeta>& triggers = snap.triggers;

  const int sel_idx = selected_col_idx.Get();
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

  std::string sql_preview = !add_col_sql.empty()
                                ? add_col_sql
                                : (!rename_table_sql.empty()
                                       ? rename_table_sql
                                       : (!rename_col_sql.empty()
                                              ? rename_col_sql
                                              : (!preview_override.Get().empty()
                                                     ? preview_override.Get()
                                                     : "--")));

  const std::string header_name = cur_obj.name.empty() ? tab->design_display_label
                                                       : QualifiedName(dialect, cur_obj);

  // Header toolbar
  Views header_items;
  header_items.Add(TintedIcon(app::images::edit, semantic.accent_table, 17.0F));
  header_items.Add(Text(header_name)
                       .Style(TextStyle{Font::System(font_size::kTitle).WithWeight(FontWeight::SemiBold),
                                        spec.colors.on_surface})
                       .LineLimit(1)
                       .Overflow(TextOverflow::Ellipsis));
  if (is_production) {
    header_items.Add(StatusBadge(UseString(app::strings::badge_prod), spec.colors.error,
                                 WithAlpha(spec.colors.error, 0.14F),
                                 WithAlpha(spec.colors.error, 0.40F)));
  }
  if (read_only) {
    header_items.Add(StatusBadge(UseString(app::strings::badge_ro), spec.colors.on_surface_variant,
                                 spec.colors.surface_container, semantic.border_subtle));
  }
  header_items.Add(Spacer());
  if (!read_only && !cur_obj.is_view) {
    header_items.Add(CompactTextField(
        TextField(table_name_input)
            .Placeholder(app::strings::edit_table_name_label)
            .Variant(TextFieldVariant::Outlined)
            .LineLimits(TextFieldLineLimits::SingleLine())
            .OnChanged([table_name_input](const TextEditingValue& v) { table_name_input = v; }),
        28.0F, 8.0F)
                         .With(Frame{.width = 210.0F}));
    header_items.Add(CompactActionButton(
        Button(app::strings::edit_table_rename_btn)
            .OnClick([execute_ddl, rename_table_sql, table_name_input] {
              execute_ddl(rename_table_sql, TrimmedSql(table_name_input.Get().text));
            }),
        ButtonTone::Default, 28.0F, 10.0F)
                         .With(Enabled{!busy.Get() && !rename_table_sql.empty()}));
  }
  header_items.Add(CompactActionButton(
      Button(app::strings::action_refresh).OnClick([reload_design] { reload_design(std::nullopt); }),
      ButtonTone::Default, 28.0F, 10.0F)
                       .With(Enabled{!loading.Get() && !busy.Get()}));

  View header_bar = Row{std::move(header_items)}
                        .With(Padding{EdgeInsets::Symmetric(10.0F, 6.0F)}, Spacing{8.0F},
                              CrossAlign{CrossAxisAlignment::Center});

  // 5-Section horizontal switcher strip: DDL, Columns, Indexes, Foreign Keys, Triggers
  const int cur_sec = active_section.Get();
  View section_bar = Row{
      SchemaSectionPill(loc.Get(app::strings::design_tab_ddl), cur_sec == 0,
                        [active_section] { active_section = 0; }, spec, semantic)
          .Key("design-pill-ddl"),
      SchemaSectionPill(loc.Fmt(app::strings::design_tab_columns, std::to_string(cols.size())),
                        cur_sec == 1, [active_section] { active_section = 1; }, spec, semantic)
          .Key("design-pill-cols"),
      SchemaSectionPill(loc.Fmt(app::strings::design_tab_indexes, std::to_string(indexes.size())),
                        cur_sec == 2, [active_section] { active_section = 2; }, spec, semantic)
          .Key("design-pill-idx"),
      SchemaSectionPill(loc.Fmt(app::strings::design_tab_fks, std::to_string(fks.size())),
                        cur_sec == 3, [active_section] { active_section = 3; }, spec, semantic)
          .Key("design-pill-fk"),
      SchemaSectionPill(
          loc.Fmt(app::strings::design_tab_triggers, std::to_string(triggers.size())),
          cur_sec == 4, [active_section] { active_section = 4; }, spec, semantic)
          .Key("design-pill-trg"),
      Spacer(),
  }.With(Padding{EdgeInsets::Symmetric(10.0F, 4.0F)}, Spacing{6.0F},
         CrossAlign{CrossAxisAlignment::Center});

  const std::unordered_map<std::string, float>& widths = column_widths.Get();
  const auto on_col_resized = [column_widths](const DataTableColumnResize& r) {
    column_widths.Update([&r](std::unordered_map<std::string, float>& m) {
      m[r.column_id] = r.width;
    });
  };

  View section_body;
  if (cur_sec == 0) {
    // DDL section
    const std::string ddl = snap.ddl;
    View ddl_actions = Row{
        CompactActionButton(
            Button(app::strings::design_copy_ddl).OnClick([copy_text, ddl] {
              copy_text(ddl, app::strings::design_ddl_copied_label);
            }),
            ButtonTone::Default, 28.0F, 10.0F)
            .With(Enabled{!ddl.empty()}),
        CompactActionButton(
            Button(app::strings::design_open_in_sql).OnClick(
                [app, workspace, session_id, ddl, header_name] {
                  (void)OpenSqlTab(app, workspace, session_id, ddl, "DDL · " + header_name);
                }),
            ButtonTone::Primary, 28.0F, 10.0F)
            .With(Enabled{!ddl.empty()}),
        Spacer(),
    }.With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Center});

    View ddl_card =
        ddl.empty()
            ? TableEmptyState(app::strings::design_ddl_empty).With(Grow(1.0F))
            : ScrollView(
                  Text(ddl)
                      .Style(TextStyle{Font::Monospace(13.0F), spec.colors.on_surface})
                      .With(Padding{EdgeInsets::Symmetric(14.0F, 12.0F)}))
                  .With(Grow(1.0F), CornerRadius{6.0F}, Background{VisualFill{spec.colors.surface}},
                        Border{semantic.border_subtle, 1.0F});

    section_body = Column{std::move(ddl_actions), std::move(ddl_card)}
                       .With(Grow(1.0F), Padding{EdgeInsets::Symmetric(10.0F, 8.0F)},
                             Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch});
  } else if (cur_sec == 1) {
    // Columns (Fields) horizontal DataTable + compact edit strip
    View table_area;
    if (cols.empty()) {
      table_area = TableEmptyState(app::strings::schema_dialog_empty).With(Grow(1.0F));
    } else {
      const auto col_ctx = [menu, loc, copy_text, execute_ddl, dialect, cur_obj, cols, read_only,
                            busy, danger = semantic.status_failed](
                               Point pos, std::size_t ridx, std::size_t) {
        if (ridx >= cols.size()) {
          return;
        }
        const ColumnMeta& cm = cols[ridx];
        std::vector<MenuEntry> entries;
        entries.emplace_back(MenuItem(app::images::copy, loc.Get(app::strings::design_ctx_copy_name),
                                      [copy_text, name = cm.name] {
                                        copy_text(name, app::strings::sidebar_copy_col_label);
                                      }));
        entries.emplace_back(MenuItem(app::images::copy, loc.Get(app::strings::design_ctx_copy_type),
                                      [copy_text, tp = cm.type] {
                                        copy_text(tp.empty() ? "ANY" : tp,
                                                  app::strings::design_ctx_copy_type);
                                      }));
        if (!read_only && !cur_obj.is_view) {
          const std::string dsql = BuildDropColumnSql(dialect, cur_obj, cm.name);
          entries.emplace_back(MenuSection{});
          entries.emplace_back(
              MenuItem(app::images::trash, loc.Get(app::strings::design_ctx_drop_col),
                       [execute_ddl, dsql] { execute_ddl(dsql, std::nullopt); })
                  .IconTint(danger)
                  .Enabled(!busy.Get() && cols.size() > 1 && !dsql.empty()));
        }
        menu.ShowAt(pos, std::move(entries));
      };

      DataTableOptions opts{
          .rows = MakeSequentialRows(cols.size()),
          .columns = BuildSchemaColumnsTable(cols, widths, spec, semantic, loc, col_ctx),
          .row_height = 28.0F,
          .header_height = 30.0F,
          .frozen_column_count = 0,
          .sort = std::nullopt,
          .selected_cell = selected_col_cell.Get(),
      };
      table_area =
          DataTable(std::move(opts))
              .On<DataTableEvents::SelectionRequested>(
                  [cols, selected_col_cell, selected_col_idx, edit_col_name, edit_col_type,
                   edit_col_nullable](const DataTableCellId& cell) {
                    selected_col_cell = cell;
                    const std::size_t ridx = RowIndexFromId(cell.row_id);
                    if (ridx != static_cast<std::size_t>(-1) && ridx < cols.size()) {
                      selected_col_idx = static_cast<int>(ridx);
                      const ColumnMeta& c = cols[ridx];
                      edit_col_name = TextEditingValue::FromText(c.name);
                      edit_col_type = TextEditingValue::FromText(c.type.empty() ? "ANY" : c.type);
                      edit_col_nullable = c.nullable;
                    }
                  })
              .On<DataTableEvents::ColumnResized>(on_col_resized)
              .With(Grow(1.0F), CornerRadius{6.0F}, Border{semantic.border_subtle, 1.0F})
              .Key("design-cols-table-" + std::to_string(tab_id));
    }

    Views col_items;
    col_items.Add(std::move(table_area).With(Grow(1.0F)).Key("design-cols-grid"));

    if (!read_only && !cur_obj.is_view) {
      Views selected_row_items;
      if (has_selected_col) {
        selected_row_items.Add(
            StatusBadge(selected_meta.name, spec.colors.on_primary_container,
                        spec.colors.primary_container, WithAlpha(spec.colors.primary, 0.35F))
                .Key("design-sel-col-badge"));
        selected_row_items.Add(
            CompactTextField(
                TextField(edit_col_name)
                    .Placeholder(app::strings::edit_table_rename_col_label)
                    .Variant(TextFieldVariant::Outlined)
                    .LineLimits(TextFieldLineLimits::SingleLine())
                    .OnChanged([edit_col_name](const TextEditingValue& v) { edit_col_name = v; }),
                28.0F, 8.0F)
                .With(Frame{.width = 170.0F})
                .Key("design-sel-rename-input"));
        selected_row_items.Add(
            CompactActionButton(
                Button(app::strings::edit_table_rename_col_btn).OnClick([execute_ddl,
                                                                         rename_col_sql] {
                  execute_ddl(rename_col_sql, std::nullopt);
                }),
                ButtonTone::Default, 28.0F, 10.0F)
                .With(Enabled{!busy.Get() && !rename_col_sql.empty()})
                .Key("design-sel-rename-btn"));
        if (dialect != Dialect::Sqlite) {
          selected_row_items.Add(
              CompactTextField(
                  TextField(edit_col_type)
                      .Placeholder(app::strings::edit_table_type_label)
                      .Variant(TextFieldVariant::Outlined)
                      .LineLimits(TextFieldLineLimits::SingleLine())
                      .OnChanged(
                          [edit_col_type](const TextEditingValue& v) { edit_col_type = v; }),
                  28.0F, 8.0F)
                  .With(Frame{.width = 140.0F})
                  .Key("design-sel-type-input"));
          selected_row_items.Add(
              CompactActionButton(
                  Button(app::strings::edit_table_alter_type_btn)
                      .OnClick([execute_ddl, alter_type_sql] {
                        execute_ddl(alter_type_sql, std::nullopt);
                      }),
                  ButtonTone::Default, 28.0F, 10.0F)
                  .With(Enabled{!busy.Get() && !alter_type_sql.empty()})
                  .Key("design-sel-type-btn"));
        } else {
          selected_row_items.Add(MutedText(UseString(app::strings::edit_table_sqlite_type_note))
                                     .With(Grow(1.0F))
                                     .Key("design-sel-sqlite-note"));
        }
        selected_row_items.Add(Spacer().Key("design-sel-spacer"));
        selected_row_items.Add(
            CompactActionButton(
                Button(app::strings::edit_table_drop_col_btn).OnClick([execute_ddl, drop_col_sql] {
                  execute_ddl(drop_col_sql, std::nullopt);
                }),
                ButtonTone::Danger, 28.0F, 10.0F)
                .With(Enabled{!busy.Get() && cols.size() > 1 && !drop_col_sql.empty()})
                .Key("design-sel-drop-btn"));
      } else {
        selected_row_items.Add(MutedText(UseString(app::strings::design_select_col_hint))
                                   .With(Grow(1.0F))
                                   .Key("design-sel-hint"));
      }

      View edit_strip = Column{
          Row{std::move(selected_row_items)}
              .With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Center}),
          Row{
              Caption(UseString(app::strings::edit_table_add_section)),
              CompactTextField(
                  TextField(add_col_name)
                      .Placeholder(app::strings::edit_table_add_name_ph)
                      .Variant(TextFieldVariant::Outlined)
                      .LineLimits(TextFieldLineLimits::SingleLine())
                      .OnChanged([add_col_name](const TextEditingValue& v) { add_col_name = v; }),
                  28.0F, 8.0F)
                  .With(Frame{.width = 160.0F}),
              CompactTextField(
                  TextField(add_col_type)
                      .Placeholder(app::strings::edit_table_add_type_ph)
                      .Variant(TextFieldVariant::Outlined)
                      .LineLimits(TextFieldLineLimits::SingleLine())
                      .OnChanged([add_col_type](const TextEditingValue& v) { add_col_type = v; }),
                  28.0F, 8.0F)
                  .With(Frame{.width = 140.0F}),
              CompactTextField(
                  TextField(add_col_default)
                      .Placeholder(app::strings::edit_table_add_default_ph)
                      .Variant(TextFieldVariant::Outlined)
                      .LineLimits(TextFieldLineLimits::SingleLine())
                      .OnChanged(
                          [add_col_default](const TextEditingValue& v) { add_col_default = v; }),
                  28.0F, 8.0F)
                  .With(Frame{.width = 160.0F}),
              Switch(app::strings::edit_table_add_nullable, add_col_nullable)
                  .OnChanged([add_col_nullable](bool v) { add_col_nullable = v; }),
              Spacer(),
              CompactActionButton(
                  Button(busy.Get() ? app::strings::edit_table_executing
                                    : app::strings::edit_table_add_btn)
                      .OnClick([execute_ddl, add_col_sql] {
                        execute_ddl(add_col_sql, std::nullopt);
                      }),
                  ButtonTone::Primary, 28.0F, 12.0F)
                  .With(Enabled{!busy.Get() && !add_col_sql.empty()}),
          }.With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Center}),
          Row{
              Caption(UseString(app::strings::edit_table_sql_preview)),
              Text(sql_preview)
                  .Style(TextStyle{Font::Monospace(12.0F), spec.colors.on_surface_variant})
                  .LineLimit(1)
                  .Overflow(TextOverflow::Ellipsis)
                  .With(Grow(1.0F)),
          }.With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Center}),
      }.With(Padding{EdgeInsets::Symmetric(10.0F, 8.0F)}, Spacing{8.0F},
             CrossAlign{CrossAxisAlignment::Stretch}, CornerRadius{6.0F},
             Background{VisualFill{spec.colors.surface}}, Border{semantic.border_subtle, 1.0F});

      col_items.Add(std::move(edit_strip).Key("design-cols-edit-strip"));
    }

    section_body = Column{std::move(col_items)}
                       .With(Grow(1.0F), Padding{EdgeInsets::Symmetric(10.0F, 8.0F)},
                             Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch});
  } else if (cur_sec == 2) {
    // Indexes horizontal DataTable
    if (indexes.empty()) {
      section_body = TableEmptyState(app::strings::design_indexes_empty).With(Grow(1.0F));
    } else {
      const auto idx_ctx = [menu, loc, copy_text, app, workspace, session_id, indexes](
                               Point pos, std::size_t ridx, std::size_t) {
        if (ridx >= indexes.size()) {
          return;
        }
        const IndexMeta& ix = indexes[ridx];
        std::vector<MenuEntry> entries;
        entries.emplace_back(MenuItem(app::images::copy, loc.Get(app::strings::design_ctx_copy_name),
                                      [copy_text, name = ix.name] {
                                        copy_text(name, app::strings::design_ctx_copy_name);
                                      }));
        if (!ix.definition.empty()) {
          entries.emplace_back(
              MenuItem(app::images::copy, loc.Get(app::strings::design_ctx_copy_def),
                       [copy_text, def = ix.definition] {
                         copy_text(def, app::strings::design_def_copied_label);
                       }));
          entries.emplace_back(
              MenuItem(app::images::sql_console, loc.Get(app::strings::design_ctx_open_sql),
                       [app, workspace, session_id, ix] {
                         (void)OpenSqlTab(app, workspace, session_id, ix.definition,
                                          "Index · " + ix.name);
                       }));
        }
        menu.ShowAt(pos, std::move(entries));
      };
      DataTableOptions opts{
          .rows = MakeSequentialRows(indexes.size()),
          .columns = BuildSchemaIndexesTable(indexes, widths, spec, loc, idx_ctx),
          .row_height = 28.0F,
          .header_height = 30.0F,
          .frozen_column_count = 0,
          .sort = std::nullopt,
          .selected_cell = std::nullopt,
      };
      section_body =
          Column{
              DataTable(std::move(opts))
                  .On<DataTableEvents::ColumnResized>(on_col_resized)
                  .With(Grow(1.0F), CornerRadius{6.0F}, Border{semantic.border_subtle, 1.0F})
                  .Key("design-idx-table-" + std::to_string(tab_id)),
          }
              .With(Grow(1.0F), Padding{EdgeInsets::Symmetric(10.0F, 8.0F)},
                    CrossAlign{CrossAxisAlignment::Stretch});
    }
  } else if (cur_sec == 3) {
    // Foreign Keys horizontal DataTable
    if (fks.empty()) {
      section_body = TableEmptyState(app::strings::design_fks_empty).With(Grow(1.0F));
    } else {
      const auto fk_ctx = [menu, loc, copy_text, fks](Point pos, std::size_t ridx) {
        if (ridx >= fks.size()) {
          return;
        }
        const ForeignKeyMeta& fk = fks[ridx];
        std::vector<MenuEntry> entries;
        if (!fk.constraint_name.empty()) {
          entries.emplace_back(
              MenuItem(app::images::copy, loc.Get(app::strings::design_ctx_copy_name),
                       [copy_text, name = fk.constraint_name] {
                         copy_text(name, app::strings::design_ctx_copy_name);
                       }));
        }
        if (!fk.fk_table.empty()) {
          entries.emplace_back(
              MenuItem(app::images::copy, loc.Get(app::strings::sidebar_ctx_copy_name),
                       [copy_text, tbl = fk.fk_table] {
                         copy_text(tbl, app::strings::sidebar_copy_name_label);
                       }));
        }
        if (!entries.empty()) {
          menu.ShowAt(pos, std::move(entries));
        }
      };
      DataTableOptions opts{
          .rows = MakeSequentialRows(fks.size()),
          .columns = BuildSchemaForeignKeysTable(fks, widths, spec, semantic, loc, fk_ctx),
          .row_height = 28.0F,
          .header_height = 30.0F,
          .frozen_column_count = 0,
          .sort = std::nullopt,
          .selected_cell = std::nullopt,
      };
      section_body =
          Column{
              DataTable(std::move(opts))
                  .On<DataTableEvents::ColumnResized>(on_col_resized)
                  .With(Grow(1.0F), CornerRadius{6.0F}, Border{semantic.border_subtle, 1.0F})
                  .Key("design-fk-table-" + std::to_string(tab_id)),
          }
              .With(Grow(1.0F), Padding{EdgeInsets::Symmetric(10.0F, 8.0F)},
                    CrossAlign{CrossAxisAlignment::Stretch});
    }
  } else {
    // Triggers horizontal DataTable + definition preview strip
    if (triggers.empty()) {
      section_body = TableEmptyState(app::strings::design_triggers_empty).With(Grow(1.0F));
    } else {
      const auto trg_ctx = [menu, loc, copy_text, app, workspace, session_id, triggers](
                               Point pos, std::size_t ridx, std::size_t) {
        if (ridx >= triggers.size()) {
          return;
        }
        const TriggerMeta& trg = triggers[ridx];
        std::vector<MenuEntry> entries;
        entries.emplace_back(MenuItem(app::images::copy, loc.Get(app::strings::design_ctx_copy_name),
                                      [copy_text, name = trg.name] {
                                        copy_text(name, app::strings::design_ctx_copy_name);
                                      }));
        if (!trg.definition.empty()) {
          entries.emplace_back(
              MenuItem(app::images::copy, loc.Get(app::strings::design_ctx_copy_def),
                       [copy_text, def = trg.definition] {
                         copy_text(def, app::strings::design_def_copied_label);
                       }));
          entries.emplace_back(
              MenuItem(app::images::sql_console, loc.Get(app::strings::design_ctx_open_sql),
                       [app, workspace, session_id, trg] {
                         (void)OpenSqlTab(app, workspace, session_id, trg.definition,
                                          "Trigger · " + trg.name);
                       }));
        }
        menu.ShowAt(pos, std::move(entries));
      };
      DataTableOptions opts{
          .rows = MakeSequentialRows(triggers.size()),
          .columns = BuildSchemaTriggersTable(triggers, widths, spec, loc, trg_ctx),
          .row_height = 28.0F,
          .header_height = 30.0F,
          .frozen_column_count = 0,
          .sort = std::nullopt,
          .selected_cell = selected_trigger_cell.Get(),
      };
      Views trg_views;
      trg_views.Add(
          DataTable(std::move(opts))
              .On<DataTableEvents::SelectionRequested>(
                  [triggers, selected_trigger_cell,
                   selected_trigger_idx](const DataTableCellId& cell) {
                    selected_trigger_cell = cell;
                    const std::size_t ridx = RowIndexFromId(cell.row_id);
                    if (ridx != static_cast<std::size_t>(-1) && ridx < triggers.size()) {
                      selected_trigger_idx = static_cast<int>(ridx);
                    }
                  })
              .On<DataTableEvents::ColumnResized>(on_col_resized)
              .With(Grow(1.0F), CornerRadius{6.0F}, Border{semantic.border_subtle, 1.0F})
              .Key("design-trg-table-" + std::to_string(tab_id)));

      const int sel_trg = selected_trigger_idx.Get();
      if (sel_trg >= 0 && static_cast<std::size_t>(sel_trg) < triggers.size()) {
        const TriggerMeta& trg = triggers[static_cast<std::size_t>(sel_trg)];
        if (!trg.definition.empty()) {
          trg_views.Add(
              Column{
                  Row{
                      Caption(UseString(app::strings::design_trigger_def_label) + " · " + trg.name),
                      Spacer(),
                      CompactActionButton(
                          Button(app::strings::design_copy_def).OnClick([copy_text,
                                                                         def = trg.definition] {
                            copy_text(def, app::strings::design_def_copied_label);
                          }),
                          ButtonTone::Default, 26.0F, 8.0F),
                      CompactActionButton(
                          Button(app::strings::design_open_in_sql)
                              .OnClick([app, workspace, session_id, trg] {
                                (void)OpenSqlTab(app, workspace, session_id, trg.definition,
                                                 "Trigger · " + trg.name);
                              }),
                          ButtonTone::Primary, 26.0F, 8.0F),
                  }.With(Spacing{6.0F}, CrossAlign{CrossAxisAlignment::Center}),
                  ScrollView(
                      Text(trg.definition)
                          .Style(TextStyle{Font::Monospace(12.0F), spec.colors.on_surface})
                          .With(Padding{EdgeInsets::Symmetric(10.0F, 8.0F)}))
                      .With(Frame{.max_height = 140.0F}, CornerRadius{5.0F},
                            Background{VisualFill{spec.colors.surface_container}}),
              }
                  .With(Padding{EdgeInsets::Symmetric(10.0F, 8.0F)}, Spacing{6.0F},
                        CrossAlign{CrossAxisAlignment::Stretch}, CornerRadius{6.0F},
                        Background{VisualFill{spec.colors.surface}},
                        Border{semantic.border_subtle, 1.0F})
                  .Key("design-trg-detail-" + std::to_string(tab_id)));
        }
      }
      section_body = Column{std::move(trg_views)}
                         .With(Grow(1.0F), Padding{EdgeInsets::Symmetric(10.0F, 8.0F)},
                               Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch});
    }
  }

  Views page_items;
  page_items.Add(std::move(header_bar).Key("design-header-" + std::to_string(tab_id)));
  page_items.Add(std::move(section_bar).Key("design-sections-" + std::to_string(tab_id)));
  page_items.Add(Divider().Key("design-div-" + std::to_string(tab_id)));
  if (!error_text.Get().empty()) {
    page_items.Add(ErrorText(error_text.Get())
                       .With(Padding{EdgeInsets::Symmetric(10.0F, 4.0F)})
                       .Key("design-err-" + std::to_string(tab_id)));
  }
  const bool is_busy_now = loading.Get() || busy.Get();
  const StringVariant busy_label =
      busy.Get() ? StringVariant(app::strings::edit_table_executing)
                 : StringVariant(app::strings::schema_dialog_loading);
  page_items.Add(LoadingOverlay(std::move(section_body), is_busy_now, busy_label)
                     .With(Grow(1.0F))
                     .Key("design-body-" + std::to_string(tab_id)));

  return Column{std::move(page_items)}.With(Grow(1.0F), Spacing{0.0F},
                                            CrossAlign{CrossAxisAlignment::Stretch});
}

} // namespace dbpp
