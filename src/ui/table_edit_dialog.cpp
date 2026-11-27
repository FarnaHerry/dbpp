#include "ui/ui_common.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace dbpp {

using namespace huxerui;
using namespace dbpp::ui;

namespace {

bool ShouldMultilineEdit(const ColumnMeta& col, std::string_view initial_val) {
  if (initial_val.find('\n') != std::string_view::npos || initial_val.size() > 80) {
    return true;
  }
  const std::string lower = [&col] {
    std::string s = col.type;
    std::ranges::transform(s, s.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    return s;
  }();
  return lower.find("json") != std::string::npos || lower.find("text") != std::string::npos ||
         lower.find("clob") != std::string::npos;
}

} // namespace

[[huxerui::composable]] View EditCellDialog(DialogContext dialog, State<AppModel> app,
                                             State<WorkspaceModel> workspace, std::uint64_t tab_id,
                                             std::size_t row_index, std::size_t column_index,
                                             TaskScope /*tasks*/, NotifyFn /*notify*/) {
  const LocalizedCatalog loc{UseService<Resources>(), UseService<SettingsModel>()};
  const SemanticColors& semantic = UseEnvironment<SemanticColors>();

  const WorkspaceTab* tab = workspace.Get().FindTab(tab_id);
  if (tab == nullptr || tab->kind != WorkspaceTabKind::Table || !tab->table.snapshot ||
      row_index >= tab->table.snapshot->rows.size() ||
      column_index >= tab->table.columns.size() ||
      column_index >= tab->table.snapshot->rows[row_index].values.size()) {
    return DialogCard(Column{
        Row{
            Text(app::strings::action_close, TextRole::Title),
            Spacer(),
            ActionButton(Button(app::strings::action_close).OnClick([dialog] { dialog.Dismiss(); })),
        },
        MutedText(loc.Get(app::strings::table_tab_closed)),
    }.With(Frame{.width = 420.0F}, Spacing{10.0F}));
  }

  const ColumnMeta& col = tab->table.columns[column_index];
  const CellValue& cell = tab->table.snapshot->rows[row_index].values[column_index];
  const PendingCellEdit* pending_edit = tab->table.FindPendingEdit(row_index, column_index);
  const bool eff_null = pending_edit != nullptr ? pending_edit->is_null : cell.is_null;
  const std::string eff_raw =
      pending_edit != nullptr ? (pending_edit->is_null ? std::string{} : pending_edit->raw_text)
                              : cell.text;
  const std::string eff_display =
      eff_null ? std::string{}
               : (pending_edit != nullptr ? pending_edit->display_text : cell.DisplayText());
  const bool ts_hint = IsTimestampColumnHint(col.name, col.type) ||
                       (!eff_display.empty() && eff_display != eff_raw);

  auto text_state = UseState(TextEditingValue::FromText(eff_display));
  auto set_null = UseState(eff_null);

  const auto save = [dialog, app, workspace, tab_id, row_index, col_name = col.name,
                     text_state, set_null] {
    const std::string out_val = set_null.Get() ? std::string{"NULL"} : text_state.Get().text;
    dialog.Dismiss();
    StageTableCellEdit(app, workspace, tab_id,
                       DataTableEditRequest{
                           .cell_id = {.row_id = std::to_string(row_index), .column_id = col_name},
                           .text = out_val,
                       });
  };

  const bool multiline = ShouldMultilineEdit(col, eff_display);
  TextField editor =
      TextField(text_state)
          .Label(col.name)
          .Variant(TextFieldVariant::Outlined)
          .LineLimits(multiline ? TextFieldLineLimits::MultiLine(3, 8)
                                : TextFieldLineLimits::SingleLine())
          .OnChanged([text_state, set_null](const TextEditingValue& val) {
            text_state = val;
            if (set_null.Get() && !val.text.empty()) {
              set_null = false;
            }
          });
  if (!multiline) {
    editor = std::move(editor).OnSubmitted(save);
  }

  Views body;
  body.Add(Row{
      Text(UseString(app::strings::edit_cell_title, col.name), TextRole::Title).With(Grow(1.0F)),
      ActionButton(Button(app::strings::action_cancel).OnClick([dialog] { dialog.Dismiss(); })),
      ActionButton(Button(app::strings::action_save).OnClick(save), ButtonTone::Primary),
  }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{8.0F}));
  body.Add(MutedText(TypeLabel(col)));
  if (ts_hint) {
    body.Add((!eff_null && !eff_display.empty() && eff_display != eff_raw)
                 ? MutedText(UseString(app::strings::edit_cell_raw_hint, eff_raw))
                 : MutedText(UseString(app::strings::edit_cell_ts_hint)));
  }
  body.Add(MonospaceField(std::move(editor).With(Enabled{!set_null.Get()})));
  body.Add(
      Row{
          Text(app::strings::edit_cell_set_null).With(Grow(1.0F)),
          Switch(set_null.Get()).OnChanged([set_null](bool next) { set_null = next; }),
      }
          .With(CrossAlign{CrossAxisAlignment::Center}, Spacing{10.0F},
                Padding{EdgeInsets::Symmetric(2.0F, 4.0F)}, CornerRadius{6.0F},
                Background{VisualFill{WithAlpha(semantic.border_subtle, 0.25F)}}));

  return DialogCard(Column{std::move(body)}.With(
      Frame{.width = 500.0F}, Spacing{10.0F}, CrossAlign{CrossAxisAlignment::Stretch}));
}

View NullValueBadge(Color null_color, bool strikethrough) {
  return Text("NULL")
      .Style(TextStyle{
          .font = Font::Monospace(11.0F).WithSlant(FontSlant::Italic).WithWeight(FontWeight::Medium),
          .foreground = null_color,
          .decoration = strikethrough ? TextDecoration::StrikeThrough : TextDecoration::None,
      })
      .LineLimit(1)
      .With(Padding{EdgeInsets::Symmetric(5.0F, 1.0F)}, CornerRadius{4.0F},
            Background{VisualFill{WithAlpha(null_color, 0.14F)}},
            Border{WithAlpha(null_color, 0.30F), 1.0F});
}

void RunGuardedWrite(const WriteGuardContext& guard, std::string action_summary,
                     std::function<void()> on_proceed) {
  if (guard.read_only) {
    if (guard.notify) {
      guard.notify(guard.loc.Get(app::strings::toast_readonly_blocked));
    }
    return;
  }
  if (guard.production_mode) {
    guard.dialogs.Show([guard, action_summary = std::move(action_summary),
                        on_proceed = std::move(on_proceed)](DialogContext dialog) {
      return WithDialogTheme(
          guard.theme_config, guard.language_mode, guard.system_dark,
          ProductionWriteConfirmDialog(dialog, guard.connection_name, action_summary, on_proceed));
    });
    return;
  }
  if (on_proceed) {
    on_proceed();
  }
}

DataTableColumn MakeRowNumberColumn(std::int64_t base_offset, std::size_t row_count,
                                    const std::unordered_map<std::string, float>& column_widths,
                                    Color muted_color, Color draft_color,
                                    std::vector<std::size_t> pending_deletes, Color delete_color) {
  DataTableColumn column;
  column.id = std::string(kRowNumberColumnId);
  column.header = "#";
  column.sortable = false;

  const std::string max_label =
      FormatGlobalRowNumber(base_offset, row_count > 0 ? row_count - 1 : 0);
  const float digits = static_cast<float>(std::max<std::size_t>(2, max_label.size()));
  float initial_width = std::clamp(26.0F + digits * 8.0F, 44.0F, 96.0F);
  if (const auto width_it = column_widths.find(column.id); width_it != column_widths.end()) {
    initial_width = std::clamp(width_it->second, 40.0F, 120.0F);
  }
  column.width = DataTableColumnWidth{
      .initial = initial_width,
      .min = 40.0F,
      .max = 120.0F,
  };
  column.render_cell = [base_offset, muted_color, draft_color,
                        pending_deletes = std::move(pending_deletes),
                        delete_color](const DataTableCellContext& cell) -> DataTableCellContent {
    if (IsDraftRowId(cell.id.row_id)) {
      return DataTableCellContent{
          .view = Text("+")
                      .Style(TextStyle{Font::Monospace(12.0F).WithWeight(FontWeight::SemiBold),
                                       draft_color})
                      .LineLimit(1),
          .label = "+",
      };
    }
    const std::size_t row_idx = RowIndexFromId(cell.id.row_id);
    if (row_idx == static_cast<std::size_t>(-1)) {
      return DataTableCellContent{.view = View{}, .label = {}};
    }
    const bool is_del =
        std::ranges::find(pending_deletes, row_idx) != pending_deletes.end();
    std::string num_text = FormatGlobalRowNumber(base_offset, row_idx);
    if (is_del) {
      return DataTableCellContent{
          .view = Row{Text(num_text)
                          .Style(TextStyle{
                              .font = Font::Monospace(12.0F).WithWeight(FontWeight::SemiBold),
                              .foreground = delete_color,
                              .decoration = TextDecoration::StrikeThrough,
                          })
                          .LineLimit(1)}
                      .With(Frame{.min_height = 26.0F}, CrossAlign{CrossAxisAlignment::Center},
                            Background{VisualFill{WithAlpha(delete_color, 0.14F)}}),
          .label = num_text,
      };
    }
    return DataTableCellContent{
        .view = Text(num_text)
                    .Style(TextStyle{Font::Monospace(12.0F), muted_color})
                    .LineLimit(1)
                    .Overflow(TextOverflow::Ellipsis),
        .label = std::move(num_text),
    };
  };
  return column;
}

[[huxerui::composable]] View CompactTableCellEditor(huxerui::View field) {
  TextFieldStyle style = UseEnvironment<TextFieldStyle>();
  style.show_label = false;
  style.standard.minimum_height = 22.0F;
  style.outlined.minimum_height = 22.0F;
  style.text_style.font = Font::Monospace(13.0F);
  style.placeholder_style.font = Font::Monospace(13.0F);
  style.padding = EdgeInsets::Symmetric(6.0F, 1.0F);
  ThemeDefinition definition = OverrideTextFieldStyle(std::move(style));
  View owned = std::move(field);
  return Theme(std::move(definition), std::move(owned));
}

void ShowReadOnlyCellContextMenu(
    Point position, const MenuHandle& menu, const std::shared_ptr<Clipboard>& clipboard,
    const std::string& cell_display, const std::string& row_tsv, const NotifyFn& notify,
    const LocalizedCatalog& loc) {
  std::vector<MenuEntry> entries;
  entries.emplace_back(MenuItem(
      app::images::copy, loc.Get(app::strings::table_ctx_copy_value),
      [clipboard, notify, loc, cell_display] {
        if (clipboard && clipboard->SupportsSynchronousText() &&
            clipboard->WriteText(cell_display)) {
          if (notify) {
            notify(loc.Fmt(app::strings::toast_copied,
                           loc.Get(app::strings::table_copy_value_label)));
          }
        } else if (notify) {
          notify(loc.Get(app::strings::toast_clipboard_unsupported));
        }
      }));
  entries.emplace_back(MenuItem(
      app::images::copy, loc.Get(app::strings::table_ctx_copy_row),
      [clipboard, notify, loc, row_tsv] {
        if (clipboard && clipboard->SupportsSynchronousText() && clipboard->WriteText(row_tsv)) {
          if (notify) {
            notify(loc.Fmt(app::strings::toast_copied,
                           loc.Get(app::strings::table_copy_row_label)));
          }
        } else if (notify) {
          notify(loc.Get(app::strings::toast_clipboard_unsupported));
        }
      }));
  menu.ShowAt(position, std::move(entries));
}

void ShowTableCellContextMenu(
    Point position, const MenuHandle& menu, const DialogHandle& dialogs,
    const std::shared_ptr<Clipboard>& clipboard, const State<AppModel>& app,
    const State<WorkspaceModel>& workspace, std::uint64_t tab_id, std::size_t row_index,
    std::size_t col_index, Dialect dialect, bool is_redis, bool can_edit_cell,
    bool can_modify_row, bool is_pending_delete, bool can_set_null, bool /*is_production*/,
    const std::string& /*prod_conn_name*/, const std::string& cell_display,
    const std::string& row_tsv, const ColumnMeta& col_meta, const CellValue& raw_cell,
    const TaskScope& tasks, const NotifyFn& notify, const AppThemeConfig& theme_config,
    LanguageMode language_mode, bool system_dark, const Color& danger_tint,
    const LocalizedCatalog& loc, const std::function<void()>& add_draft_row,
    const std::function<void(std::string)>& apply_filter_sql) {
  std::vector<MenuEntry> entries;
  if (can_edit_cell) {
    entries.emplace_back(MenuItem(
        app::images::edit, loc.Get(app::strings::table_ctx_edit_cell),
        [dialogs, app, workspace, tab_id, row_index, col_index, tasks, notify, theme_config,
         language_mode, system_dark] {
          dialogs.Show([app, workspace, tab_id, row_index, col_index, tasks, notify, theme_config,
                        language_mode, system_dark](DialogContext dialog) {
            return WithDialogTheme(
                theme_config, language_mode, system_dark,
                EditCellDialog(dialog, app, workspace, tab_id, row_index, col_index, tasks, notify));
          });
        }));
  }
  const bool show_set_null =
      can_modify_row && !is_pending_delete && !(is_redis && col_meta.name == "size");
  if (show_set_null) {
    entries.emplace_back(
        MenuItem(app::images::null_val, loc.Get(app::strings::table_ctx_set_null),
                 [app, workspace, tab_id, row_index, col_name = col_meta.name] {
                   StageTableCellEdit(app, workspace, tab_id,
                                      DataTableEditRequest{
                                          .cell_id = {.row_id = std::to_string(row_index),
                                                      .column_id = col_name},
                                          .text = "NULL",
                                      });
                 })
            .Enabled(can_set_null));
  }
  if (can_edit_cell || show_set_null) {
    entries.emplace_back(MenuSection{});
  }

  entries.emplace_back(MenuItem(
      app::images::copy, loc.Get(app::strings::table_ctx_copy_value),
      [clipboard, notify, loc, cell_display] {
        if (clipboard && clipboard->SupportsSynchronousText() &&
            clipboard->WriteText(cell_display)) {
          if (notify) {
            notify(loc.Fmt(app::strings::toast_copied,
                           loc.Get(app::strings::table_copy_value_label)));
          }
        } else if (notify) {
          notify(loc.Get(app::strings::toast_clipboard_unsupported));
        }
      }));
  entries.emplace_back(MenuItem(
      app::images::copy, loc.Get(app::strings::table_ctx_copy_row),
      [clipboard, notify, loc, row_tsv] {
        if (clipboard && clipboard->SupportsSynchronousText() && clipboard->WriteText(row_tsv)) {
          if (notify) {
            notify(loc.Fmt(app::strings::toast_copied,
                           loc.Get(app::strings::table_copy_row_label)));
          }
        } else if (notify) {
          notify(loc.Get(app::strings::toast_clipboard_unsupported));
        }
      }));
  entries.emplace_back(MenuItem(
      app::images::search, loc.Get(app::strings::table_ctx_filter_value),
      [apply_filter_sql, dialect, is_redis, col_meta, raw_cell] {
        if (!apply_filter_sql) {
          return;
        }
        if (is_redis) {
          apply_filter_sql(raw_cell.is_null ? std::string{} : raw_cell.text);
          return;
        }
        const std::string ident = QuoteIdentifier(dialect, col_meta.name);
        if (raw_cell.is_null) {
          apply_filter_sql(ident + " IS NULL");
        } else {
          apply_filter_sql(ident + " = " +
                           SqlLiteral(dialect, raw_cell.text, col_meta.type, false));
        }
      }));

  if (can_modify_row) {
    entries.emplace_back(MenuSection{});
    entries.emplace_back(MenuItem(
        app::images::add,
        loc.Get(is_redis ? app::strings::table_add_key : app::strings::table_add_row),
        add_draft_row));
    const std::string del_label =
        is_pending_delete
            ? loc.Get(app::strings::table_undo_delete_row)
            : loc.Get(is_redis ? app::strings::table_delete_key : app::strings::table_delete_row);
    MenuItem del_item(
        app::images::trash, del_label,
        [workspace, tab_id, row_index, col_name = col_meta.name] {
          workspace.Update([tab_id, row_index, col_name](WorkspaceModel& ws) {
            if (WorkspaceTab* target = ws.FindTab(tab_id)) {
              target->selected_cell =
                  DataTableCellId{.row_id = std::to_string(row_index), .column_id = col_name};
            }
          });
          (void)ToggleTableRowPendingDelete(workspace, tab_id, row_index);
        });
    if (!is_pending_delete) {
      del_item = std::move(del_item).IconTint(danger_tint);
    }
    entries.emplace_back(std::move(del_item));
  }

  menu.ShowAt(position, std::move(entries));
}

void LaunchTableFileImport(
    bool is_jsonl, const std::shared_ptr<FilePicker>& picker,
    const std::shared_ptr<AdbcSession>& adbc_ptr, std::string target_table_name,
    std::string target_catalog, std::string target_schema, bool is_production,
    std::string prod_conn_name, const DialogHandle& dialogs, const TaskScope& tasks,
    NotifyFn notify, LocalizedCatalog loc, std::function<void()> on_done,
    AppThemeConfig theme_config, LanguageMode language_mode, bool system_dark) {
  if (!picker || !picker->CanOpenFiles() || !adbc_ptr) {
    return;
  }
  const auto run_ingest = [picker, adbc_ptr, target_table_name, target_catalog, target_schema,
                           tasks, notify, loc, on_done, is_jsonl]() {
    tasks.Launch([picker, adbc_ptr, target_table_name, target_catalog, target_schema, notify, loc,
                  on_done, is_jsonl]() -> Task<void> {
      FilePickerFilter filter{
          .name = is_jsonl ? "JSONL" : "CSV",
          .extensions = is_jsonl ? std::vector<std::string>{"jsonl", "ndjson"}
                                 : std::vector<std::string>{"csv", "tsv"},
          .content_types = {is_jsonl ? "application/x-ndjson" : "text/csv"},
      };
      auto selected = co_await picker->OpenFileAsync(std::move(filter));
      if (!selected) co_return;
      auto file = selected->AsFile();
      if (!file) {
        if (notify) notify(loc.Get(app::strings::connect_picker_no_local_path));
        co_return;
      }
      const std::string p = file->Path();
      const BulkIngestResult res = co_await adbc_ptr->sequence.Run(
          [adbc_ptr, p, target_table_name, target_catalog, target_schema,
           is_jsonl](std::stop_token) {
            return is_jsonl
                       ? IngestJsonlPathBlocking(*adbc_ptr, p, target_table_name,
                                                 IngestMode::Append, target_catalog, target_schema)
                       : IngestCsvPathBlocking(*adbc_ptr, p, target_table_name,
                                               IngestMode::Append, target_catalog, target_schema);
          });
      if (!res.error.empty()) {
        if (notify) notify(loc.Fmt(app::strings::table_import_failed, res.error));
        co_return;
      }
      if (notify) {
        notify(loc.Fmt(app::strings::table_import_done,
                       res.rows_affected >= 0 ? std::to_string(res.rows_affected)
                                              : std::string("?")));
      }
      if (on_done) {
        on_done();
      }
    });
  };
  RunGuardedWrite(
      WriteGuardContext{
          .read_only = false,
          .production_mode = is_production,
          .connection_name = prod_conn_name,
          .dialogs = dialogs,
          .notify = notify,
          .loc = loc,
          .theme_config = theme_config,
          .language_mode = language_mode,
          .system_dark = system_dark,
      },
      std::string(is_jsonl ? "IMPORT JSONL -> " : "IMPORT CSV -> ") + target_table_name,
      run_ingest);
}

namespace {

std::string MakeSafeExportBaseName(std::string_view raw, std::string_view fallback = "export") {
  std::string out;
  out.reserve(raw.size());
  for (const unsigned char ch : raw) {
    if (ch == '/' || ch == '\\' || ch == ':' || ch == '*' || ch == '?' || ch == '"' ||
        ch == '<' || ch == '>' || ch == '|' || std::isspace(ch) != 0) {
      if (out.empty() || out.back() != '_') {
        out.push_back('_');
      }
    } else {
      out.push_back(static_cast<char>(ch));
    }
  }
  while (!out.empty() && out.back() == '_') {
    out.pop_back();
  }
  return out.empty() ? std::string(fallback) : out;
}

} // namespace

void LaunchDatabaseSqlExport(
    const std::shared_ptr<FilePicker>& picker, const std::shared_ptr<AdbcSession>& adbc_ptr,
    SqlDumpOptions options, std::string suggested_name, const TaskScope& tasks, NotifyFn notify,
    LocalizedCatalog loc) {
  if (!adbc_ptr || adbc_ptr->IsClosed()) {
    if (notify) {
      notify(loc.Get(app::strings::toast_connection_closed));
    }
    return;
  }
  if (!picker || !picker->CanSaveFiles()) {
    if (notify) {
      notify(loc.Fmt(app::strings::db_export_failed, "FilePicker unavailable"));
    }
    return;
  }

  tasks.Launch([adbc_ptr, options = std::move(options),
                suggested_name = std::move(suggested_name), picker, notify = std::move(notify),
                loc]() mutable -> Task<void> {
    const std::string temp_path = MakeTempSqlExportPath();
    const SqlDumpResult res = co_await adbc_ptr->sequence.Run(
        [adbc_ptr, options = std::move(options), temp_path](std::stop_token) mutable {
          return ExportDatabaseSqlToFilePathBlocking(*adbc_ptr, options, temp_path);
        });
    const auto cleanup = [&temp_path] { RemoveFileQuietly(temp_path); };
    if (!res.error.empty()) {
      cleanup();
      if (notify) {
        notify(loc.Fmt(app::strings::db_export_failed, res.error));
      }
      co_return;
    }

    SaveFileOptions save_opts{
        .suggested_name = suggested_name,
        .filter = FilePickerFilter{
            .name = "SQL",
            .extensions = {"sql"},
            .content_types = {"application/sql", "text/plain"},
        },
    };
    const bool saved = co_await picker->SaveFileAsync(File(temp_path), save_opts);
    cleanup();
    if (saved && notify) {
      notify(loc.Fmt(app::strings::db_export_done, std::to_string(res.tables_exported),
                     std::to_string(res.rows_written)));
    }
  });
}

void LaunchDatabaseSqlImport(
    const std::shared_ptr<FilePicker>& picker, const std::shared_ptr<AdbcSession>& adbc_ptr,
    std::string target_database, bool is_production, std::string prod_conn_name,
    const DialogHandle& dialogs, const TaskScope& tasks, NotifyFn notify, LocalizedCatalog loc,
    std::function<void()> on_done, AppThemeConfig theme_config, LanguageMode language_mode,
    bool system_dark) {
  if (!adbc_ptr || adbc_ptr->IsClosed()) {
    if (notify) {
      notify(loc.Get(app::strings::toast_connection_closed));
    }
    return;
  }
  if (adbc_ptr->read_only) {
    if (notify) {
      notify(loc.Get(app::strings::toast_readonly_blocked));
    }
    return;
  }
  if (!picker || !picker->CanOpenFiles()) {
    return;
  }

  tasks.Launch([picker, adbc_ptr, target_database = std::move(target_database), is_production,
                prod_conn_name = std::move(prod_conn_name), dialogs, tasks,
                notify = std::move(notify), loc, on_done = std::move(on_done), theme_config,
                language_mode, system_dark]() mutable -> Task<void> {
    FilePickerFilter filter{
        .name = "SQL",
        .extensions = {"sql"},
        .content_types = {"application/sql", "text/plain"},
    };
    auto selected = co_await picker->OpenFileAsync(std::move(filter));
    if (!selected) {
      co_return;
    }
    auto file = selected->AsFile();
    if (!file) {
      if (notify) {
        notify(loc.Get(app::strings::connect_picker_no_local_path));
      }
      co_return;
    }
    const std::string path = file->Path();
    const auto run_import = [adbc_ptr, path, target_database, tasks, notify, loc, on_done] {
      tasks.Launch([adbc_ptr, path, target_database, notify, loc, on_done]() -> Task<void> {
        const SqlScriptImportResult res = co_await adbc_ptr->sequence.Run(
            [adbc_ptr, path, target_database](std::stop_token) {
              return ImportSqlScriptPathBlocking(*adbc_ptr, path, target_database);
            });
        if (!res.error.empty()) {
          if (notify) {
            notify(loc.Fmt(app::strings::db_import_failed, res.error));
          }
          co_return;
        }
        if (notify) {
          notify(loc.Fmt(app::strings::db_import_done, std::to_string(res.statements_executed)));
        }
        if (on_done) {
          on_done();
        }
      });
    };

    const std::size_t slash = path.find_last_of("/\\");
    const std::string file_name = slash == std::string::npos ? path : path.substr(slash + 1);
    const std::string target_label =
        target_database.empty() ? prod_conn_name : target_database;
    RunGuardedWrite(
        WriteGuardContext{
            .read_only = false,
            .production_mode = is_production,
            .connection_name = prod_conn_name,
            .dialogs = dialogs,
            .notify = notify,
            .loc = loc,
            .theme_config = theme_config,
            .language_mode = language_mode,
            .system_dark = system_dark,
        },
        "IMPORT SQL -> " + target_label + " (" + file_name + ")", run_import);
  });
}

void AppendDatabaseIoMenuEntries(
    std::vector<MenuEntry>& entries, const std::shared_ptr<FilePicker>& picker,
    const std::shared_ptr<AdbcSession>& adbc_ptr, std::string target_database,
    std::string suggested_base, bool read_only, bool is_production, std::string prod_conn_name,
    const DialogHandle& dialogs, const TaskScope& tasks, NotifyFn notify, LocalizedCatalog loc,
    std::function<void()> on_done, AppThemeConfig theme_config, LanguageMode language_mode,
    bool system_dark) {
  const std::string base = MakeSafeExportBaseName(suggested_base, "database");
  std::vector<MenuEntry> export_children;
  export_children.emplace_back(MenuItem(
      app::images::external_link, loc.Get(app::strings::sidebar_ctx_export_sql_all),
      [picker, adbc_ptr, target_database, base, tasks, notify, loc] {
        LaunchDatabaseSqlExport(
            picker, adbc_ptr,
            SqlDumpOptions{.target_database = target_database,
                           .single_object = std::nullopt,
                           .include_schema = true,
                           .include_data = true},
            base + ".sql", tasks, notify, loc);
      }));
  export_children.emplace_back(MenuItem(
      app::images::external_link, loc.Get(app::strings::sidebar_ctx_export_sql_schema),
      [picker, adbc_ptr, target_database, base, tasks, notify, loc] {
        LaunchDatabaseSqlExport(
            picker, adbc_ptr,
            SqlDumpOptions{.target_database = target_database,
                           .single_object = std::nullopt,
                           .include_schema = true,
                           .include_data = false},
            base + "_schema.sql", tasks, notify, loc);
      }));

  entries.emplace_back(MenuSection{});
  entries.emplace_back(MenuItem(app::images::external_link,
                                loc.Get(app::strings::sidebar_ctx_export_db),
                                std::move(export_children)));
  if (!read_only) {
    entries.emplace_back(MenuItem(
        app::images::add, loc.Get(app::strings::sidebar_ctx_import_sql),
        [picker, adbc_ptr, target_database, is_production, prod_conn_name, dialogs, tasks, notify,
         loc, on_done, theme_config, language_mode, system_dark] {
          LaunchDatabaseSqlImport(picker, adbc_ptr, target_database, is_production, prod_conn_name,
                                  dialogs, tasks, notify, loc, on_done, theme_config, language_mode,
                                  system_dark);
        }));
  }
}

void AppendTableIoMenuEntries(
    std::vector<MenuEntry>& entries, const std::shared_ptr<FilePicker>& picker,
    const std::shared_ptr<AdbcSession>& adbc_ptr, DbObject object, std::string display_label,
    bool read_only, bool is_production, std::string prod_conn_name, const DialogHandle& dialogs,
    const TaskScope& tasks, NotifyFn notify, LocalizedCatalog loc, std::function<void()> on_done,
    AppThemeConfig theme_config, LanguageMode language_mode, bool system_dark) {
  const std::string base = MakeSafeExportBaseName(display_label, "table");
  const Dialect dialect = adbc_ptr ? adbc_ptr->dialect : Dialect::Generic;
  const std::string export_sql = "SELECT * FROM " + QualifiedName(dialect, object);

  std::vector<MenuEntry> export_children;
  export_children.emplace_back(MenuItem(
      app::images::external_link, loc.Get(app::strings::sidebar_ctx_export_sql_all),
      [picker, adbc_ptr, object, base, tasks, notify, loc] {
        LaunchDatabaseSqlExport(
            picker, adbc_ptr,
            SqlDumpOptions{.target_database = {},
                           .single_object = object,
                           .include_schema = true,
                           .include_data = true},
            base + ".sql", tasks, notify, loc);
      }));
  if (!object.is_view) {
    export_children.emplace_back(MenuItem(
        app::images::external_link, loc.Get(app::strings::sidebar_ctx_export_sql_schema),
        [picker, adbc_ptr, object, base, tasks, notify, loc] {
          LaunchDatabaseSqlExport(
              picker, adbc_ptr,
              SqlDumpOptions{.target_database = {},
                             .single_object = object,
                             .include_schema = true,
                             .include_data = false},
              base + "_schema.sql", tasks, notify, loc);
        }));
  }
  export_children.emplace_back(MenuSection{});
  export_children.emplace_back(MenuItem(
      app::images::external_link, loc.Get(app::strings::sql_export_csv),
      [adbc_ptr, export_sql, base, picker, tasks, notify, loc] {
        ExportSqlWithPicker(adbc_ptr, export_sql, ExportFormat::Csv, base + ".csv", picker, tasks,
                            notify, loc);
      }));
  export_children.emplace_back(MenuItem(
      app::images::external_link, loc.Get(app::strings::sql_export_jsonl),
      [adbc_ptr, export_sql, base, picker, tasks, notify, loc] {
        ExportSqlWithPicker(adbc_ptr, export_sql, ExportFormat::Jsonl, base + ".jsonl", picker,
                            tasks, notify, loc);
      }));

  entries.emplace_back(MenuSection{});
  entries.emplace_back(MenuItem(app::images::external_link,
                                loc.Get(app::strings::sidebar_ctx_export_table),
                                std::move(export_children)));
  if (!read_only && !object.is_view) {
    std::vector<MenuEntry> import_children;
    import_children.emplace_back(MenuItem(
        app::images::add, loc.Get(app::strings::table_import_csv),
        [picker, adbc_ptr, object, is_production, prod_conn_name, dialogs, tasks, notify, loc,
         on_done, theme_config, language_mode, system_dark] {
          LaunchTableFileImport(false, picker, adbc_ptr, object.name, object.catalog,
                                object.db_schema, is_production, prod_conn_name, dialogs, tasks,
                                notify, loc, on_done, theme_config, language_mode, system_dark);
        }));
    import_children.emplace_back(MenuItem(
        app::images::add, loc.Get(app::strings::table_import_jsonl),
        [picker, adbc_ptr, object, is_production, prod_conn_name, dialogs, tasks, notify, loc,
         on_done, theme_config, language_mode, system_dark] {
          LaunchTableFileImport(true, picker, adbc_ptr, object.name, object.catalog,
                                object.db_schema, is_production, prod_conn_name, dialogs, tasks,
                                notify, loc, on_done, theme_config, language_mode, system_dark);
        }));
    entries.emplace_back(MenuItem(app::images::add,
                                  loc.Get(app::strings::sidebar_ctx_import_table),
                                  std::move(import_children)));
  }
}

void ApplyDraftCellValue(const State<WorkspaceModel>& workspace, std::uint64_t tab_id,
                         std::string_view row_id, std::string_view col_id,
                         const std::string& value) {
  const auto draft_id = ParseDraftRowId(row_id);
  if (!draft_id) {
    return;
  }
  const WorkspaceTab* snapshot_tab = workspace.Get().FindTab(tab_id);
  if (snapshot_tab == nullptr || snapshot_tab->kind != WorkspaceTabKind::Table) {
    return;
  }
  std::size_t col_idx = snapshot_tab->table.columns.size();
  for (std::size_t i = 0; i < snapshot_tab->table.columns.size(); ++i) {
    if (snapshot_tab->table.columns[i].name == col_id) {
      col_idx = i;
      break;
    }
  }
  if (col_idx >= snapshot_tab->table.columns.size()) {
    return;
  }
  if (const PendingInsertRow* existing =
          FindDraftRow(snapshot_tab->table.pending_inserts, *draft_id)) {
    const std::string& cur_val =
        col_idx < existing->values.size() ? existing->values[col_idx] : std::string{};
    if (cur_val == value) {
      return;
    }
  } else {
    return;
  }

  workspace.Update([&](WorkspaceModel& ws) {
    WorkspaceTab* target = ws.FindTab(tab_id);
    if (target == nullptr || target->kind != WorkspaceTabKind::Table) {
      return;
    }
    PendingInsertRow* draft = FindDraftRowMutable(target->table.pending_inserts, *draft_id);
    if (draft == nullptr) {
      return;
    }
    if (draft->values.size() < target->table.columns.size()) {
      draft->values.resize(target->table.columns.size());
    }
    draft->values[col_idx] = value;
  });
}

void FlushLiveTableEdits(const State<AppModel>& app, const State<WorkspaceModel>& workspace,
                         std::uint64_t tab_id,
                         const std::shared_ptr<std::unordered_map<std::string, std::string>>& live) {
  if (!live || live->empty()) {
    return;
  }
  auto pending = std::move(*live);
  live->clear();
  bool has_draft = false;
  for (const auto& [key, val] : pending) {
    const std::size_t sep = key.find('\n');
    if (sep == std::string::npos) {
      continue;
    }
    const std::string_view row_id(key.data(), sep);
    const std::string_view col_id(key.data() + sep + 1, key.size() - sep - 1);
    if (IsDraftRowId(row_id)) {
      has_draft = true;
    } else {
      StageTableCellEdit(app, workspace, tab_id,
                         DataTableEditRequest{.cell_id = {.row_id = std::string(row_id),
                                                          .column_id = std::string(col_id)},
                                              .text = val});
    }
  }
  if (!has_draft) {
    return;
  }
  workspace.Update([&](WorkspaceModel& ws) {
    WorkspaceTab* target = ws.FindTab(tab_id);
    if (target == nullptr || target->kind != WorkspaceTabKind::Table) {
      return;
    }
    for (const auto& [key, val] : pending) {
      const std::size_t sep = key.find('\n');
      if (sep == std::string::npos) {
        continue;
      }
      const std::string_view row_id(key.data(), sep);
      const std::string_view col_id(key.data() + sep + 1, key.size() - sep - 1);
      const auto draft_id = ParseDraftRowId(row_id);
      if (!draft_id) {
        continue;
      }
      PendingInsertRow* draft = FindDraftRowMutable(target->table.pending_inserts, *draft_id);
      if (draft == nullptr) {
        continue;
      }
      if (draft->values.size() < target->table.columns.size()) {
        draft->values.resize(target->table.columns.size());
      }
      for (std::size_t i = 0; i < target->table.columns.size(); ++i) {
        if (target->table.columns[i].name == col_id) {
          draft->values[i] = val;
          break;
        }
      }
    }
  });
}

void HandleTableEditSubmitted(
    const DialogHandle& /*dialogs*/, const State<AppModel>& app,
    const State<WorkspaceModel>& workspace, const TaskScope& /*tasks*/, std::uint64_t tab_id,
    const DataTableEditRequest& edit, bool /*is_production*/, const std::string& /*prod_conn_name*/,
    AppThemeConfig /*theme_config*/, LanguageMode /*language_mode*/, bool /*system_dark*/,
    const NotifyFn& /*notify*/) {
  StageTableCellEdit(app, workspace, tab_id, edit);
}

} // namespace dbpp
