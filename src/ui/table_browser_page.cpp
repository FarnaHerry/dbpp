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

[[huxerui::composable]] View TableBrowserPage(
    State<AppModel> app, State<WorkspaceModel> workspace, std::uint64_t tab_id,
    TaskScope tasks, NotifyFn notify, AppThemeConfig theme_config, LanguageMode language_mode,
    bool system_dark) {
  const ThemeSpec& spec = UseTheme();
  const SemanticColors& semantic = UseEnvironment<SemanticColors>();
  const LocalizedCatalog loc{UseService<Resources>(), UseService<SettingsModel>()};
  const auto dialogs = UseDialog();
  const auto menu = UseMenu();
  const auto picker = UseService<FilePicker>();
  const auto sort_anchor = UseViewHandle();
  const auto import_anchor = UseViewHandle();
  const auto export_anchor = UseViewHandle();
  const auto page_size_anchor = UseViewHandle();
  const auto page_size_popup = UsePopup();
  const auto settings = UseService<SettingsModel>();
  auto live_drafts = UseState(std::make_shared<std::unordered_map<std::string, std::string>>());

  const WorkspaceTab* tab = workspace.Get().FindTab(tab_id);
  if (tab == nullptr || tab->kind != WorkspaceTabKind::Table) {
    return TableEmptyState(app::strings::table_tab_closed);
  }
  const TableModel& model = tab->table;
  const DbSession* session = nullptr;
  for (const DbSession& open : app.Get().sessions) {
    if (open.id == tab->session_id) {
      session = &open;
      break;
    }
  }
  if (session == nullptr) {
    return TableEmptyState(app::strings::table_conn_disconnected);
  }
  const bool is_redis = session->dialect == Dialect::Redis;

  const auto apply_draft_value = [workspace, tab_id](const std::string& row_id,
                                                     const std::string& col_id,
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
  };

  const auto flush_draft_cell = [live_drafts, apply_draft_value](const std::string& row_id,
                                                                 const std::string& col_id) {
    auto live = live_drafts.Get();
    const std::string key = DraftCellBufferKey(row_id, col_id);
    const auto it = live->find(key);
    if (it == live->end()) {
      return;
    }
    std::string text = std::move(it->second);
    live->erase(it);
    apply_draft_value(row_id, col_id, text);
  };

  const auto flush_live_drafts = [workspace, tab_id, live_drafts]() {
    auto live = live_drafts.Get();
    if (live->empty()) {
      return;
    }
    auto pending = std::move(*live);
    live->clear();
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
        PendingInsertRow* draft =
            FindDraftRowMutable(target->table.pending_inserts, *draft_id);
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
  };

  const auto block_if_dirty = [workspace, tab_id, notify, loc]() -> bool {
    const WorkspaceTab* cur = workspace.Get().FindTab(tab_id);
    if (cur != nullptr && !cur->table.pending_inserts.empty()) {
      if (notify) {
        notify(loc.Get(app::strings::table_dirty_block));
      }
      return true;
    }
    return false;
  };

  const auto reload = [app, workspace, tasks, tab_id, notify, block_if_dirty](
                          std::int64_t page, std::string filter) {
    if (block_if_dirty()) {
      return;
    }
    const WorkspaceTab* cur = workspace.Get().FindTab(tab_id);
    if (cur == nullptr) {
      return;
    }
    RequestLoadTable(app, workspace, tasks, tab_id, page, cur->table.sort, std::move(filter),
                     false, notify);
  };

  const auto apply_filter = [reload, workspace, tab_id] {
    const WorkspaceTab* cur = workspace.Get().FindTab(tab_id);
    if (cur == nullptr) {
      return;
    }
    reload(0, cur->filter_input.text);
  };

  const auto clear_filter = [reload, workspace, tab_id] {
    workspace.Update([tab_id](WorkspaceModel& ws) {
      if (WorkspaceTab* target = ws.FindTab(tab_id)) {
        target->filter_input = TextEditingValue{};
      }
    });
    reload(0, {});
  };

  const auto refresh = [reload, workspace, tab_id] {
    const WorkspaceTab* cur = workspace.Get().FindTab(tab_id);
    if (cur == nullptr) {
      return;
    }
    reload(cur->table.page, cur->table.filter);
  };

  const auto add_draft_row = [workspace, tab_id, flush_live_drafts, notify, loc] {
    flush_live_drafts();
    const WorkspaceTab* cur = workspace.Get().FindTab(tab_id);
    if (cur == nullptr || !cur->table.editable || cur->table.read_only || cur->table.committing ||
        cur->table.columns.empty()) {
      return;
    }
    const std::size_t committed_rows =
        cur->table.snapshot ? cur->table.snapshot->rows.size() : 0;
    workspace.Update([&](WorkspaceModel& ws) {
      WorkspaceTab* target = ws.FindTab(tab_id);
      if (target == nullptr || target->kind != WorkspaceTabKind::Table ||
          !target->table.editable || target->table.read_only || target->table.committing ||
          target->table.columns.empty()) {
        return;
      }
      PendingInsertRow row;
      row.draft_id = target->table.next_draft_id++;
      row.values.resize(target->table.columns.size());
      const std::string row_id = "draft:" + std::to_string(row.draft_id);
      std::string focus_col = target->table.columns.front().name;
      for (const ColumnMeta& col : target->table.columns) {
        if (!IsBlobColumn(col)) {
          focus_col = col.name;
          break;
        }
      }
      target->table.pending_inserts.push_back(std::move(row));
      target->selected_cell = DataTableCellId{.row_id = row_id, .column_id = focus_col};
    });
    if (committed_rows > 12 && notify) {
      notify(loc.Get(app::strings::table_draft_added_hint));
    }
  };

  const auto commit_inserts = [app, workspace, tasks, tab_id, flush_live_drafts, notify] {
    flush_live_drafts();
    RequestCommitPendingInserts(app, workspace, tasks, tab_id, notify);
  };

  const auto rollback_inserts = [workspace, tab_id, live_drafts, notify, loc] {
    live_drafts.Get()->clear();
    bool cleared = false;
    workspace.Update([&](WorkspaceModel& ws) {
      WorkspaceTab* target = ws.FindTab(tab_id);
      if (target == nullptr || target->kind != WorkspaceTabKind::Table ||
          target->table.pending_inserts.empty() || target->table.committing) {
        return;
      }
      target->table.pending_inserts.clear();
      if (target->selected_cell.has_value() && IsDraftRowId(target->selected_cell->row_id)) {
        target->selected_cell = std::nullopt;
      }
      cleared = true;
    });
    if (cleared && notify) {
      notify(loc.Get(app::strings::table_draft_rolled_back));
    }
  };

  const auto delete_selected_action = [dialogs, app, workspace, tab_id, live_drafts,
                                       flush_live_drafts, tasks, notify, theme_config,
                                       language_mode, system_dark, is_redis, loc] {
    const WorkspaceTab* cur = workspace.Get().FindTab(tab_id);
    if (cur == nullptr || !cur->selected_cell.has_value()) {
      return;
    }
    if (const auto draft_id = ParseDraftRowId(cur->selected_cell->row_id)) {
      flush_live_drafts();
      const std::string row_prefix = cur->selected_cell->row_id + "\n";
      auto live = live_drafts.Get();
      for (auto it = live->begin(); it != live->end();) {
        if (it->first.starts_with(row_prefix)) {
          it = live->erase(it);
        } else {
          ++it;
        }
      }
      workspace.Update([&](WorkspaceModel& ws) {
        WorkspaceTab* target = ws.FindTab(tab_id);
        if (target == nullptr || target->kind != WorkspaceTabKind::Table) {
          return;
        }
        std::erase_if(target->table.pending_inserts, [draft_id](const PendingInsertRow& row) {
          return row.draft_id == *draft_id;
        });
        target->selected_cell = std::nullopt;
      });
      if (notify) {
        notify(loc.Get(app::strings::table_draft_removed_one));
      }
      return;
    }
    dialogs.Show([app, workspace, tab_id, tasks, notify, theme_config, language_mode, system_dark,
                  is_redis](DialogContext dialog) {
      return WithDialogTheme(theme_config, language_mode, system_dark,
                             DeleteRowDialog(dialog, app, workspace, tab_id, tasks, notify, is_redis));
    });
  };

  const auto apply_sort = [app, workspace, tasks, tab_id, notify,
                           block_if_dirty](std::optional<DataTableSort> next_sort) {
    if (block_if_dirty()) {
      return;
    }
    const WorkspaceTab* cur = workspace.Get().FindTab(tab_id);
    if (cur == nullptr) {
      return;
    }
    RequestLoadTable(app, workspace, tasks, tab_id, 0, std::move(next_sort), cur->table.filter,
                     false, notify);
  };

  constexpr float kToolbarHeight = 28.0F;
  Views toolbar_items;
  TextField filter_field =
      TextField(tab->filter_input)
          .Placeholder(is_redis ? app::strings::table_filter_redis_ph
                                : app::strings::table_filter_sql_ph)
          .Variant(TextFieldVariant::Outlined)
          .LineLimits(TextFieldLineLimits::SingleLine())
          .VerticalAlign(TextVerticalAlign::Center)
          .LeadingIcon(app::images::search)
          .OnChanged([workspace, tab_id](const TextEditingValue& value) {
            workspace.Update([&](WorkspaceModel& ws) {
              if (WorkspaceTab* target = ws.FindTab(tab_id)) {
                target->filter_input = value;
              }
            });
          })
          .OnSubmitted([apply_filter] { apply_filter(); });
  toolbar_items.Add(CompactTextField(std::move(filter_field), kToolbarHeight, 8.0F, font_size::kChip)
                        .With(Frame{.width = 240.0F, .height = kToolbarHeight,
                                    .min_height = kToolbarHeight, .max_height = kToolbarHeight}));
  toolbar_items.Add(CompactActionButton(Button(app::strings::table_apply_filter).OnClick(apply_filter),
                                        ButtonTone::Default, kToolbarHeight, 10.0F));
  if (!model.filter.empty() || !TrimmedSql(tab->filter_input.text).empty()) {
    toolbar_items.Add(
        CompactActionButton(Button(app::strings::table_clear_filter).OnClick(clear_filter),
                            ButtonTone::Default, kToolbarHeight, 10.0F));
  }
  const bool sort_active = model.sort.has_value();
  const std::string sort_label =
      sort_active ? UseString(model.sort->direction == DataTableSortDirection::Descending
                                  ? app::strings::table_sort_active_desc
                                  : app::strings::table_sort_active_asc,
                              model.sort->column_id)
                  : UseString(app::strings::table_sort);
  const Color sort_fg = sort_active ? spec.colors.on_primary_container : spec.colors.on_surface;
  const Color sort_bg = sort_active ? spec.colors.primary_container : spec.colors.surface;
  const Color sort_border =
      sort_active ? WithAlpha(spec.colors.primary, 0.45F) : semantic.border_subtle;
  const Color sort_icon_color =
      sort_active ? spec.colors.primary : spec.colors.on_surface_variant;
  const std::vector<ColumnMeta> sort_cols = model.columns;
  const std::optional<DataTableSort> current_sort = model.sort;
  toolbar_items.Add(
      Row{
          TintedIcon(app::images::sort, sort_icon_color, 13.0F),
          Text(sort_label)
              .Style(TextStyle{Font::System(font_size::kChip).WithWeight(FontWeight::Medium),
                               sort_fg})
              .LineLimit(1),
          TintedIcon(app::images::chevron_down, sort_icon_color, 12.0F),
      }
          .With(Frame{.height = kToolbarHeight, .min_height = kToolbarHeight,
                      .max_height = kToolbarHeight},
                Padding{EdgeInsets::Symmetric(8.0F, 0.0F)}, Spacing{5.0F},
                CrossAlign{CrossAxisAlignment::Center}, CornerRadius{6.0F},
                Background{VisualFill{sort_bg}}, Border{sort_border, 1.0F}, sort_anchor,
                Enabled{!model.columns.empty() && model.pending_inserts.empty()})
          .OnClick([menu, sort_anchor, loc, apply_sort, sort_cols, current_sort] {
            std::vector<MenuEntry> entries;
            if (current_sort.has_value()) {
              const bool is_desc = current_sort->direction == DataTableSortDirection::Descending;
              entries.emplace_back(
                  MenuItem(loc.Get(is_desc ? app::strings::table_sort_toggle_asc
                                           : app::strings::table_sort_toggle_desc),
                           [apply_sort, col_id = current_sort->column_id, is_desc] {
                             apply_sort(DataTableSort{
                                 .column_id = col_id,
                                 .direction = is_desc ? DataTableSortDirection::Ascending
                                                      : DataTableSortDirection::Descending,
                             });
                           }));
              entries.emplace_back(
                  MenuItem(loc.Get(app::strings::table_sort_clear), [apply_sort] {
                    apply_sort(std::nullopt);
                  }));
              entries.emplace_back(MenuSection{});
            }
            for (const ColumnMeta& col : sort_cols) {
              const bool active_col =
                  current_sort.has_value() && current_sort->column_id == col.name;
              const bool active_desc =
                  active_col && current_sort->direction == DataTableSortDirection::Descending;
              std::string item_label = col.name;
              if (active_col) {
                item_label += active_desc ? " ↓" : " ↑";
              }
              entries.emplace_back(
                  MenuItem(std::move(item_label),
                           [apply_sort, col_name = col.name, active_col, active_desc] {
                             DataTableSortDirection next_dir =
                                 (active_col && !active_desc) ? DataTableSortDirection::Descending
                                                              : DataTableSortDirection::Ascending;
                             apply_sort(DataTableSort{.column_id = col_name, .direction = next_dir});
                           })
                      .Checked(active_col));
            }
            menu.Show(sort_anchor, std::move(entries),
                      MenuOptions{
                          .placement = {.side = AnchorSide::Below,
                                        .alignment = AnchorAlignment::Start},
                          .gap = 4.0F,
                          .viewport_margin = 8.0F,
                          .offset = {},
                          .width = std::nullopt,
                          .dismiss_on_outside_press = true,
                          .dismiss_on_cancel = true,
                          .on_dismiss_request = {},
                      });
          }));
  toolbar_items.Add(Spacer());
  if (!is_redis && model.editable && !model.read_only && session->session) {
    const std::shared_ptr<AdbcSession> adbc_ptr = session->session;
    const std::string target_table_name = model.object;
    const DbObject* matched_obj = nullptr;
    for (const DbObject& o : session->objects) {
      if (o.name == model.object && o.db_schema == tab->object_schema) {
        matched_obj = &o;
        break;
      }
    }
    const std::string target_catalog = matched_obj ? matched_obj->catalog : std::string{};
    const std::string target_schema = matched_obj ? matched_obj->db_schema : tab->object_schema;
    toolbar_items.Add(
        CompactActionButton(
            Button(app::strings::table_import)
                .OnClick([menu, import_anchor, picker, tasks, app, workspace, tab_id, notify, loc,
                          adbc_ptr, target_table_name, target_catalog, target_schema, refresh] {
                  if (!picker || !picker->CanOpenFiles()) return;
                  const auto launch_import = [=](bool is_jsonl) {
                    tasks.Launch([picker, adbc_ptr, target_table_name, target_catalog,
                                  target_schema, notify, loc, refresh, is_jsonl]() -> Task<void> {
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
                                                                 IngestMode::Append, target_catalog,
                                                                 target_schema)
                                       : IngestCsvPathBlocking(*adbc_ptr, p, target_table_name,
                                                               IngestMode::Append, target_catalog,
                                                               target_schema);
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
                      refresh();
                    });
                  };
                  std::vector<MenuEntry> entries;
                  entries.emplace_back(MenuItem(loc.Get(app::strings::table_import_csv),
                                                [launch_import] { launch_import(false); }));
                  entries.emplace_back(MenuItem(loc.Get(app::strings::table_import_jsonl),
                                                [launch_import] { launch_import(true); }));
                  menu.Show(import_anchor, std::move(entries),
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
                .With(Enabled{!model.loading && !model.committing}),
            ButtonTone::Default, kToolbarHeight, 10.0F)
            .With(import_anchor));
  }
  if (!is_redis && session->session) {
    const std::shared_ptr<AdbcSession> adbc_ptr = session->session;
    const std::string base_name = model.object;
    std::string export_sql = "SELECT * FROM " + model.qualified_name;
    if (!model.filter.empty()) {
      export_sql += " WHERE (" + model.filter + ")";
    }
    if (model.sort.has_value() && !model.sort->column_id.empty()) {
      export_sql += " ORDER BY " + QuoteIdentifier(session->dialect, model.sort->column_id);
      export_sql +=
          model.sort->direction == DataTableSortDirection::Descending ? " DESC" : " ASC";
    }
    toolbar_items.Add(
        CompactActionButton(
            Button(app::strings::sql_export)
                .OnClick([menu, export_anchor, adbc_ptr, export_sql, base_name, picker, tasks,
                          notify, loc] {
                  std::vector<MenuEntry> entries;
                  entries.emplace_back(MenuItem(
                      loc.Get(app::strings::sql_export_csv),
                      [adbc_ptr, export_sql, base_name, picker, tasks, notify, loc] {
                        ExportSqlWithPicker(adbc_ptr, export_sql, ExportFormat::Csv,
                                            base_name + ".csv", picker, tasks, notify, loc);
                      }));
                  entries.emplace_back(MenuItem(
                      loc.Get(app::strings::sql_export_jsonl),
                      [adbc_ptr, export_sql, base_name, picker, tasks, notify, loc] {
                        ExportSqlWithPicker(adbc_ptr, export_sql, ExportFormat::Jsonl,
                                            base_name + ".jsonl", picker, tasks, notify, loc);
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
                .With(Enabled{!model.loading}),
            ButtonTone::Default, kToolbarHeight, 10.0F)
            .With(export_anchor));
  }
  toolbar_items.Add(CompactActionButton(Button(app::strings::action_refresh).OnClick(refresh),
                                        ButtonTone::Default, kToolbarHeight, 10.0F));
  toolbar_items.Add(
      CompactActionButton(Button(is_redis ? app::strings::table_add_key : app::strings::table_add_row)
                              .OnClick(add_draft_row),
                          ButtonTone::Default, kToolbarHeight, 10.0F)
          .With(Enabled{model.editable && !model.read_only && !model.committing}));
  if (!model.pending_inserts.empty()) {
    const std::string commit_label =
        model.committing ? UseString(app::strings::table_committing)
                         : UseString(app::strings::table_commit_n, model.pending_inserts.size());
    toolbar_items.Add(
        CompactActionButton(Button(commit_label).OnClick(commit_inserts), ButtonTone::Primary,
                            kToolbarHeight, 10.0F)
            .With(Enabled{!model.committing}));
    toolbar_items.Add(
        CompactActionButton(Button(app::strings::table_rollback).OnClick(rollback_inserts),
                            ButtonTone::Default, kToolbarHeight, 10.0F)
            .With(Enabled{!model.committing}));
  }
  toolbar_items.Add(
      CompactActionButton(Button(is_redis ? app::strings::table_delete_key
                                          : app::strings::table_delete_row)
                              .OnClick(delete_selected_action),
                          ButtonTone::Danger, kToolbarHeight, 10.0F)
          .With(Enabled{model.editable && !model.read_only && !model.committing &&
                        tab->selected_cell.has_value()}));

  View toolbar = Row{std::move(toolbar_items)}.With(
      CrossAlign{CrossAxisAlignment::Center}, Spacing{4.0F},
      Padding{EdgeInsets::Symmetric(6.0F, 0.0F)},
      Frame{.height = kToolbarHeight, .min_height = kToolbarHeight, .max_height = kToolbarHeight});

  const auto change_page_size = [app, workspace, tasks, tab_id, notify, settings,
                                 block_if_dirty](std::int64_t next_size) {
    if (block_if_dirty()) {
      return;
    }
    const std::int64_t clamped = std::clamp(next_size, kMinPageSize, kMaxPageSize);
    if (settings && settings->default_page_size.Get() != clamped) {
      (void)settings->SetDefaultPageSize(clamped);
    }
    const WorkspaceTab* cur = workspace.Get().FindTab(tab_id);
    if (cur == nullptr || (cur->table.page_size == clamped && cur->table.page == 0 &&
                           cur->table.snapshot != nullptr)) {
      return;
    }
    RequestLoadTable(app, workspace, tasks, tab_id, 0, cur->table.sort, cur->table.filter, false,
                     notify, clamped);
  };

  const std::int64_t current_page_size = model.page_size > 0 ? model.page_size : kPageSize;
  View page_size_selector =
      Row{
          Text(UseString(app::strings::table_page_size_label, current_page_size))
              .Style(TextStyle{Font::System(font_size::kCaption).WithWeight(FontWeight::Medium),
                               spec.colors.on_surface}),
          TintedIcon(app::images::chevron_down, spec.colors.on_surface_variant, 12.0F),
      }
          .With(Frame{.height = 24.0F, .min_height = 24.0F, .max_height = 24.0F},
                Padding{EdgeInsets::Symmetric(6.0F, 0.0F)}, Spacing{4.0F},
                CrossAlign{CrossAxisAlignment::Center}, CornerRadius{5.0F},
                page_size_anchor, Enabled{model.pending_inserts.empty()})
          .OnClick([page_size_popup, page_size_anchor, change_page_size, current_page_size,
                    theme_config, language_mode, system_dark] {
            page_size_popup.Show(
                page_size_anchor,
                [change_page_size, current_page_size, theme_config, language_mode,
                 system_dark](PopupContext popup) {
                  return WithDialogTheme(
                      theme_config, language_mode, system_dark,
                      PageSizePopup(popup, current_page_size, change_page_size));
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

  const std::int64_t page_count =
      model.total <= 0 ? 1
                       : ((model.total - 1) / std::max<std::int64_t>(1, model.page_size) + 1);
  const std::string status_text =
      !model.pending_inserts.empty()
          ? UseString(app::strings::table_pending_hint, model.pending_inserts.size())
          : (model.message.empty() ? UseString(app::strings::table_no_data) : model.message);
  Views status_items;
  status_items.Add(AuditDurationBadge(model.elapsed_us));
  status_items.Add(AuditSqlBadge(model.executed_sql, notify));
  if (!model.filter.empty()) {
    status_items.Add(
        StatusBadge(UseString(app::strings::table_active_filter, model.filter),
                    spec.colors.primary, spec.colors.primary_container, Color::Transparent())
            .With(LayoutPriority(1.0F)));
  }
  if (model.read_only) {
    status_items.Add(
        StatusBadge(UseString(app::strings::table_read_only), spec.colors.on_surface_variant,
                    spec.colors.surface_container, Color::Transparent())
            .With(LayoutPriority(1.0F)));
  }
  status_items.Add(Spacer());
  status_items.Add(Text(status_text)
                       .Style(TextStyle{Font::System(font_size::kCaption),
                                        spec.colors.on_surface_variant})
                       .LineLimit(1)
                       .Overflow(TextOverflow::Ellipsis)
                       .With(LayoutPriority(1.0F)));
  status_items.Add(std::move(page_size_selector).With(LayoutPriority(1.0F)));
  status_items.Add(CompactPagerControls(
      model.page, page_count, model.page + 1 < page_count, model.pending_inserts.empty(),
      model.snapshot_version,
      [reload, filter = model.filter](std::int64_t target) { reload(target, filter); }, notify));
  View pager = BottomStatusBar(std::move(status_items));

  View table_view;
  if ((model.snapshot == nullptr && model.pending_inserts.empty()) || model.columns.empty()) {
    if (model.loading) {
      table_view = LoadingState(app::strings::table_loading);
    } else {
      table_view = TableEmptyState(!model.error.empty() ? app::strings::table_load_failed_retry
                                                        : app::strings::table_no_columns);
    }
  } else {
    const std::shared_ptr<const GridSnapshot> snapshot = model.snapshot;
    const std::vector<PendingInsertRow> drafts = model.pending_inserts;
    const std::string draft_first_empty = UseString(app::strings::table_draft_first_col_empty);
    const std::string draft_other_empty = UseString(app::strings::table_draft_other_col_empty);
    const std::string draft_acc_empty = UseString(app::strings::table_draft_accessibility_empty);
    std::vector<DataTableColumn> columns;
    columns.reserve(model.columns.size() + 1);
    columns.push_back(MakeRowNumberColumn(
        SafePageOffset(model.page, current_page_size), snapshot ? snapshot->rows.size() : 0,
        model.column_widths, spec.colors.on_surface_variant, semantic.draft_accent));
    for (std::size_t index = 0; index < model.columns.size(); ++index) {
      const ColumnMeta& meta = model.columns[index];
      const auto width_it = model.column_widths.find(meta.name);
      const float raw_width = width_it != model.column_widths.end()
                                  ? width_it->second
                                  : ComputeAutoColumnWidth(meta.name, index, snapshot.get());
      const float width = std::clamp(raw_width, kMinColumnWidth, kMaxColumnWidth);
      DataTableColumn column;
      column.id = meta.name;
      column.header = std::string(meta.name);
      column.width = DataTableColumnWidth{
          .initial = width,
          .min = kMinColumnWidth,
          .max = kMaxColumnWidth,
      };
      column.sortable = true;
      column.render_cell = [snapshot, drafts, index, spec, semantic, draft_first_empty,
                            draft_other_empty, draft_acc_empty](
                               const DataTableCellContext& cell) -> DataTableCellContent {
        if (const auto draft_id = ParseDraftRowId(cell.id.row_id)) {
          const PendingInsertRow* draft = FindDraftRow(drafts, *draft_id);
          const std::string raw = (draft != nullptr && index < draft->values.size())
                                      ? draft->values[index]
                                      : std::string{};
          const bool is_blank = raw.empty();
          const std::string display =
              index == 0 ? (is_blank ? draft_first_empty : "＋ " + raw)
                         : (is_blank ? draft_other_empty : raw);
          const Color draft_color =
              is_blank ? WithAlpha(semantic.draft_accent, 0.72F) : semantic.draft_accent;
          TextStyle style{is_blank ? Font::Monospace(13.0F).WithSlant(FontSlant::Italic)
                                   : Font::Monospace(13.0F).WithWeight(FontWeight::SemiBold),
                          draft_color};
          return DataTableCellContent{
              .view = Text(SingleLineCellPreview(display))
                          .Style(style)
                          .LineLimit(1)
                          .Overflow(TextOverflow::Ellipsis),
              .label = is_blank ? draft_acc_empty : raw,
          };
        }

        const bool is_null = CellIsNull(snapshot, cell.id.row_id, index);
        const std::string text = CellText(snapshot, cell.id.row_id, index);
        TextStyle style{Font::Monospace(13.0F),
                        is_null ? semantic.null_text : spec.colors.on_surface};
        if (is_null) {
          style.font = Font::Monospace(13.0F).WithSlant(FontSlant::Italic);
        }
        return DataTableCellContent{
            .view = Text(SingleLineCellPreview(text))
                        .Style(style)
                        .LineLimit(1)
                        .Overflow(TextOverflow::Ellipsis),
            .label = text,
        };
      };
      if (model.editable && !model.read_only && !IsBlobColumn(meta) &&
          !(is_redis && meta.name == "size")) {
        column.editor = DataTableEditor{
            .initial_text = [workspace, tab_id, live_drafts, snapshot, index,
                             col_id = meta.name](const DataTableCellContext& cell) {
              if (const auto draft_id = ParseDraftRowId(cell.id.row_id)) {
                auto live = live_drafts.Get();
                if (const auto it = live->find(DraftCellBufferKey(cell.id.row_id, col_id));
                    it != live->end()) {
                  return it->second;
                }
                const WorkspaceTab* cur = workspace.Get().FindTab(tab_id);
                if (cur != nullptr) {
                  if (const PendingInsertRow* draft =
                          FindDraftRow(cur->table.pending_inserts, *draft_id)) {
                    if (index < draft->values.size()) {
                      return draft->values[index];
                    }
                  }
                }
                return std::string{};
              }
              return CellIsNull(snapshot, cell.id.row_id, index)
                         ? std::string{"NULL"}
                         : CellText(snapshot, cell.id.row_id, index);
            },
            .render = [live_drafts, flush_draft_cell](const DataTableEditContext& edit) {
              const std::string row_id = edit.cell.id.row_id;
              const std::string col_id = edit.cell.id.column_id;
              View field = TextField(edit.draft)
                               .Variant(TextFieldVariant::Standard)
                               .LineLimits(TextFieldLineLimits::SingleLine())
                               .VerticalAlign(TextVerticalAlign::Center)
                               .InputConfiguration({.action = TextInputAction::Done})
                               .OnChanged([draft = edit.draft, live_drafts, row_id, col_id](
                                              const TextEditingValue& value) {
                                 draft = value;
                                 if (IsDraftRowId(row_id)) {
                                   (*live_drafts.Get())[DraftCellBufferKey(row_id, col_id)] =
                                       value.text;
                                 }
                               })
                               .OnSubmitted(edit.commit)
                               .With(Padding{EdgeInsets::Symmetric(4.0F, 8.0F)}, edit.focus_target);
              if (IsDraftRowId(row_id)) {
                field = std::move(field).On<ViewEvents::FocusChanged>(
                    [flush_draft_cell, row_id, col_id](bool focused) {
                      if (!focused) {
                        flush_draft_cell(row_id, col_id);
                      }
                    });
              }
              return field;
            },
        };
      }
      columns.push_back(std::move(column));
    }

    DataTableOptions options{
        .rows = MakeTableRows(snapshot, model.pending_inserts),
        .columns = std::move(columns),
        .row_height = 26.0F,
        .header_height = 28.0F,
        .frozen_column_count = 0,
        .sort = model.sort,
        .selected_cell = tab->selected_cell,
    };

    View grid =
        DataTable(std::move(options))
            .On<DataTableEvents::SortRequested>(
                [app, workspace, tasks, tab_id, notify,
                 block_if_dirty](const DataTableSort& requested) {
                  if (requested.column_id == kRowNumberColumnId) {
                    return;
                  }
                  if (block_if_dirty()) {
                    return;
                  }
                  const WorkspaceTab* cur = workspace.Get().FindTab(tab_id);
                  if (cur == nullptr) {
                    return;
                  }
                  std::optional<DataTableSort> next;
                  if (!cur->table.sort.has_value() ||
                      cur->table.sort->column_id != requested.column_id) {
                    next = DataTableSort{
                        .column_id = requested.column_id,
                        .direction = DataTableSortDirection::Ascending,
                    };
                  } else if (cur->table.sort->direction == DataTableSortDirection::Ascending) {
                    next = DataTableSort{
                        .column_id = requested.column_id,
                        .direction = DataTableSortDirection::Descending,
                    };
                  } else {
                    next = std::nullopt;
                  }
                  RequestLoadTable(app, workspace, tasks, tab_id, 0, std::move(next),
                                   cur->table.filter, false, notify);
                })
            .On<DataTableEvents::SelectionRequested>(
                [workspace, tab_id](const DataTableCellId& cell) {
                  workspace.Update([&](WorkspaceModel& ws) {
                    if (WorkspaceTab* target = ws.FindTab(tab_id)) {
                      target->selected_cell = cell;
                    }
                  });
                })
            .On<DataTableEvents::EditSubmitted>(
                [app, workspace, tasks, tab_id, live_drafts, apply_draft_value,
                 notify](const DataTableEditRequest& edit) {
                  if (IsDraftRowId(edit.cell_id.row_id)) {
                    live_drafts.Get()->erase(
                        DraftCellBufferKey(edit.cell_id.row_id, edit.cell_id.column_id));
                    apply_draft_value(edit.cell_id.row_id, edit.cell_id.column_id, edit.text);
                    return;
                  }
                  RequestSaveCell(app, workspace, tasks, tab_id, edit, notify);
                })
            .On<DataTableEvents::ColumnResized>(
                [workspace, tab_id](const DataTableColumnResize& resize) {
                  workspace.Update([&](WorkspaceModel& ws) {
                    if (WorkspaceTab* target = ws.FindTab(tab_id)) {
                      target->table.column_widths[resize.column_id] = resize.width;
                    }
                  });
                })
            .With(Border{semantic.border_subtle, 1.0F})
            .Key("table-grid-" + std::to_string(tab_id) + "-" +
                 std::to_string(model.snapshot_version));
    const bool table_busy = model.loading || model.committing;
    const StringVariant table_busy_label =
        model.committing ? StringVariant(app::strings::table_committing)
                         : StringVariant(app::strings::table_loading);
    table_view = LoadingOverlay(std::move(grid), table_busy, table_busy_label).With(Grow(1.0F));
  }

  return Column{
      std::move(toolbar),
      (!model.error.empty() && model.snapshot != nullptr) ? ErrorText(model.error) : View{},
      std::move(table_view),
      std::move(pager),
  }.With(Grow(1.0F), Padding{EdgeInsets{.top = 4.0F}}, Spacing{4.0F},
         CrossAlign{CrossAxisAlignment::Stretch});
}

} // namespace dbpp
