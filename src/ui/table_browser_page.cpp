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
  const auto clipboard = UseApplication().Clipboard();
  const auto sort_anchor = UseViewHandle();
  const auto type_anchor = UseViewHandle();
  const auto delim_anchor = UseViewHandle();
  const auto import_anchor = UseViewHandle();
  const auto export_anchor = UseViewHandle();
  const auto page_size_anchor = UseViewHandle();
  const auto page_size_popup = UsePopup();
  const auto settings = UseService<SettingsModel>();
  auto live_drafts = UseState(std::make_shared<std::unordered_map<std::string, std::string>>());
  const auto inline_editor_focused = UseState(std::make_shared<std::atomic<bool>>(false));
  auto tree_reload_token = UseState<std::uint64_t>(0);

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
  const bool is_redis_tree = is_redis && tab->redis_view_mode == RedisBrowserViewMode::Tree;

  const auto apply_draft_value = [workspace, tab_id](const std::string& row_id,
                                                     const std::string& col_id,
                                                     const std::string& value) {
    ApplyDraftCellValue(workspace, tab_id, row_id, col_id, value);
  };

  const auto flush_live_drafts = [app, workspace, tab_id, live_drafts]() {
    FlushLiveTableEdits(app, workspace, tab_id, live_drafts.Get());
  };

  const auto block_if_dirty = [workspace, tab_id, notify, loc]() -> bool {
    const WorkspaceTab* cur = workspace.Get().FindTab(tab_id);
    if (cur != nullptr && cur->table.HasPendingChanges()) {
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

  const auto apply_filter = [reload, workspace, tab_id, is_redis_tree, tree_reload_token] {
    const WorkspaceTab* cur = workspace.Get().FindTab(tab_id);
    if (cur == nullptr) {
      return;
    }
    if (is_redis_tree) {
      const std::string f = cur->filter_input.text;
      workspace.Update([tab_id, &f](WorkspaceModel& ws) {
        if (WorkspaceTab* target = ws.FindTab(tab_id)) {
          target->table.filter = f;
        }
      });
      tree_reload_token = tree_reload_token.Get() + 1;
      return;
    }
    reload(0, cur->filter_input.text);
  };

  const auto clear_filter = [app, workspace, tasks, tab_id, notify, block_if_dirty,
                             is_redis_tree, tree_reload_token] {
    if (block_if_dirty()) {
      return;
    }
    workspace.Update([tab_id, is_redis_tree](WorkspaceModel& ws) {
      if (WorkspaceTab* target = ws.FindTab(tab_id)) {
        target->filter_input = TextEditingValue{};
        if (is_redis_tree) {
          target->table.filter.clear();
          target->table.redis_type_filter.clear();
        }
      }
    });
    if (is_redis_tree) {
      tree_reload_token = tree_reload_token.Get() + 1;
      return;
    }
    const WorkspaceTab* cur = workspace.Get().FindTab(tab_id);
    if (cur == nullptr) {
      return;
    }
    RequestLoadTable(app, workspace, tasks, tab_id, 0, cur->table.sort, {}, false, notify, 0,
                     std::string{});
  };

  const auto apply_redis_type = [app, workspace, tasks, tab_id, notify, block_if_dirty,
                                 is_redis_tree, tree_reload_token](std::string next_type) {
    if (block_if_dirty()) {
      return;
    }
    const WorkspaceTab* cur = workspace.Get().FindTab(tab_id);
    if (cur == nullptr) {
      return;
    }
    if (is_redis_tree) {
      workspace.Update([tab_id, &next_type](WorkspaceModel& ws) {
        if (WorkspaceTab* target = ws.FindTab(tab_id)) {
          target->table.redis_type_filter = next_type;
          target->table.filter = target->filter_input.text;
        }
      });
      tree_reload_token = tree_reload_token.Get() + 1;
      return;
    }
    RequestLoadTable(app, workspace, tasks, tab_id, 0, cur->table.sort, cur->filter_input.text,
                     false, notify, 0, std::move(next_type));
  };

  const auto refresh = [reload, workspace, tab_id, is_redis_tree, tree_reload_token] {
    if (is_redis_tree) {
      tree_reload_token = tree_reload_token.Get() + 1;
      return;
    }
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

  const bool is_production = session->production_mode;
  const std::string prod_conn_name = session->name;
  const auto commit_inserts = [dialogs, app, workspace, tasks, tab_id, flush_live_drafts, notify,
                               loc, is_production, prod_conn_name, theme_config, language_mode,
                               system_dark] {
    flush_live_drafts();
    const WorkspaceTab* cur = workspace.Get().FindTab(tab_id);
    if (cur == nullptr || !cur->table.HasPendingChanges()) {
      return;
    }
    const std::string tbl = cur->table.qualified_name;
    std::string summary;
    if (!cur->table.pending_edits.empty()) {
      summary = "UPDATE " + tbl + " (" + std::to_string(cur->table.pending_edits.size()) +
                " cell(s))";
    }
    if (!cur->table.pending_deletes.empty()) {
      if (!summary.empty()) summary += "; ";
      summary += "DELETE FROM " + tbl + " (" +
                 std::to_string(cur->table.pending_deletes.size()) + " row(s))";
    }
    if (!cur->table.pending_inserts.empty()) {
      if (!summary.empty()) summary += "; ";
      summary += "INSERT INTO " + tbl + " (" +
                 std::to_string(cur->table.pending_inserts.size()) + " row(s))";
    }
    RunGuardedWrite(
        WriteGuardContext{
            .read_only = cur->table.read_only,
            .production_mode = is_production,
            .connection_name = prod_conn_name,
            .dialogs = dialogs,
            .notify = notify,
            .loc = loc,
            .theme_config = theme_config,
            .language_mode = language_mode,
            .system_dark = system_dark,
        },
        std::move(summary), [app, workspace, tasks, tab_id, notify] {
          RequestCommitTableChanges(app, workspace, tasks, tab_id, notify);
        });
  };

  const auto rollback_inserts = [workspace, tab_id, live_drafts, notify, loc] {
    live_drafts.Get()->clear();
    bool cleared = false;
    workspace.Update([&](WorkspaceModel& ws) {
      WorkspaceTab* target = ws.FindTab(tab_id);
      if (target == nullptr || target->kind != WorkspaceTabKind::Table ||
          !target->table.HasPendingChanges() || target->table.committing) {
        return;
      }
      target->table.pending_edits.clear();
      target->table.pending_deletes.clear();
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

  const auto delete_selected_action = [workspace, tab_id, live_drafts, flush_live_drafts,
                                       notify, loc] {
    const WorkspaceTab* cur = workspace.Get().FindTab(tab_id);
    if (cur == nullptr || !cur->selected_cell.has_value() || !cur->table.editable ||
        cur->table.read_only || cur->table.committing) {
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
    const std::size_t row_idx = RowIndexFromId(cur->selected_cell->row_id);
    if (row_idx == static_cast<std::size_t>(-1)) {
      return;
    }
    flush_live_drafts();
    (void)ToggleTableRowPendingDelete(workspace, tab_id, row_idx);
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
  if (is_redis) {
    toolbar_items.Add(RedisViewModeSwitch(
        tab->redis_view_mode, !model.HasPendingChanges(), kToolbarHeight,
        [app, workspace, tasks, tab_id, notify, block_if_dirty,
         settings](RedisBrowserViewMode next_mode) {
          if (block_if_dirty()) {
            return;
          }
          if (settings && settings->redis_view_mode.Get() != next_mode) {
            (void)settings->SetRedisViewMode(next_mode);
          }
          app.Update([next_mode](AppModel& m) { m.default_redis_view_mode = next_mode; });
          workspace.Update([tab_id, next_mode](WorkspaceModel& ws) {
            if (WorkspaceTab* target = ws.FindTab(tab_id)) {
              target->redis_view_mode = next_mode;
            }
          });
          if (next_mode == RedisBrowserViewMode::Table) {
            if (const WorkspaceTab* cur = workspace.Get().FindTab(tab_id)) {
              RequestLoadTable(app, workspace, tasks, tab_id, cur->table.page, cur->table.sort,
                               cur->filter_input.text, false, notify, 0,
                               cur->table.redis_type_filter);
            }
          }
        }));
    toolbar_items.Add(RedisTypeFilterButton(menu, type_anchor, loc, model.redis_type_filter,
                                            !model.HasPendingChanges(), kToolbarHeight,
                                            apply_redis_type));
    if (is_redis_tree) {
      toolbar_items.Add(RedisDelimiterFilterButton(
          menu, delim_anchor, dialogs, loc, tab->redis_tree_delimiter, true, kToolbarHeight,
          theme_config, language_mode, system_dark, [workspace, tab_id](std::string next_delim) {
            workspace.Update([tab_id, &next_delim](WorkspaceModel& ws) {
              if (WorkspaceTab* target = ws.FindTab(tab_id)) {
                target->redis_tree_delimiter = next_delim;
              }
            });
          }));
    }
  }
  toolbar_items.Add(CompactActionButton(Button(app::strings::table_apply_filter).OnClick(apply_filter),
                                        ButtonTone::Default, kToolbarHeight, 10.0F));
  if (!model.filter.empty() || !TrimmedSql(tab->filter_input.text).empty() ||
      (is_redis && !model.redis_type_filter.empty())) {
    toolbar_items.Add(
        CompactActionButton(Button(app::strings::table_clear_filter).OnClick(clear_filter),
                            ButtonTone::Default, kToolbarHeight, 10.0F));
  }
  if (!is_redis_tree) {
    toolbar_items.Add(TableSortButton(menu, sort_anchor, loc, model.columns, model.sort,
                                      !model.columns.empty() && !model.HasPendingChanges(),
                                      kToolbarHeight, apply_sort));
  }
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
                .OnClick([menu, import_anchor, picker, tasks, dialogs, notify, loc, adbc_ptr,
                          target_table_name, target_catalog, target_schema, is_production,
                          prod_conn_name, refresh, theme_config, language_mode, system_dark] {
                  if (!picker || !picker->CanOpenFiles()) return;
                  std::vector<MenuEntry> entries;
                  AppendDataImportMenuEntries(entries, picker, adbc_ptr, target_table_name,
                                              target_catalog, target_schema, is_production,
                                              prod_conn_name, dialogs, tasks, notify, loc, refresh,
                                              theme_config, language_mode, system_dark);
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
                .OnClick([menu, export_anchor, adbc_ptr, export_sql, base_name,
                          qualified_name = model.qualified_name, picker, tasks, notify, loc] {
                  std::vector<MenuEntry> entries;
                  AppendDataExportMenuEntries(entries, adbc_ptr, export_sql, base_name,
                                              qualified_name, picker, tasks, notify, loc);
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
  if (!is_redis_tree) {
    toolbar_items.Add(
        CompactActionButton(Button(is_redis ? app::strings::table_add_key : app::strings::table_add_row)
                                .OnClick(add_draft_row),
                            ButtonTone::Default, kToolbarHeight, 10.0F)
            .With(Enabled{model.editable && !model.read_only && !model.committing}));
    if (model.HasPendingChanges()) {
      const std::string commit_label =
          model.committing ? UseString(app::strings::table_committing)
                           : UseString(app::strings::table_commit_n, model.PendingChangeCount());
      toolbar_items.Add(
          CompactActionButton(Button(commit_label).OnClick(commit_inserts), ButtonTone::Primary,
                              kToolbarHeight, 10.0F)
              .With(Enabled{!model.committing}));
      toolbar_items.Add(
          CompactActionButton(Button(app::strings::table_rollback).OnClick(rollback_inserts),
                              ButtonTone::Default, kToolbarHeight, 10.0F)
              .With(Enabled{!model.committing}));
    }
    bool selected_pending_delete = false;
    if (tab->selected_cell.has_value() && !IsDraftRowId(tab->selected_cell->row_id)) {
      selected_pending_delete =
          model.IsRowPendingDelete(RowIndexFromId(tab->selected_cell->row_id));
    }
    toolbar_items.Add(
        CompactActionButton(
            Button(selected_pending_delete
                       ? app::strings::table_undo_delete_row
                       : (is_redis ? app::strings::table_delete_key : app::strings::table_delete_row))
                .OnClick(delete_selected_action),
            selected_pending_delete ? ButtonTone::Default : ButtonTone::Danger, kToolbarHeight,
            10.0F)
            .With(Enabled{model.editable && !model.read_only && !model.committing &&
                          tab->selected_cell.has_value()}));
  }

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
                page_size_anchor, Enabled{!model.HasPendingChanges()})
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
      model.HasPendingChanges()
          ? UseString(app::strings::table_pending_hint, model.PendingChangeCount())
          : (model.message.empty() ? UseString(app::strings::table_no_data) : model.message);
  Views status_items;
  status_items.Add(AuditDurationBadge(model.elapsed_us));
  status_items.Add(AuditCommandBadge(model.executed_sql, notify));
  if (!model.filter.empty()) {
    status_items.Add(
        StatusBadge(UseString(app::strings::table_active_filter, model.filter),
                    spec.colors.primary, spec.colors.primary_container, Color::Transparent())
            .With(LayoutPriority(1.0F)));
  }
  if (is_redis && !model.redis_type_filter.empty()) {
    status_items.Add(
        StatusBadge(UseString(app::strings::table_active_type_filter, model.redis_type_filter),
                    spec.colors.primary, spec.colors.primary_container, Color::Transparent())
            .With(LayoutPriority(1.0F)));
  }
  if (model.read_only) {
    status_items.Add(
        StatusBadge(UseString(app::strings::table_read_only), spec.colors.on_surface_variant,
                    spec.colors.surface_container, Color::Transparent())
            .With(LayoutPriority(1.0F)));
  }
  if (session->production_mode) {
    status_items.Add(
        StatusBadge(loc.Get(app::strings::badge_prod), semantic.danger,
                    semantic.danger_container, WithAlpha(semantic.danger, 0.35F))
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
      model.page, page_count, model.page + 1 < page_count, !model.HasPendingChanges(),
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
    const std::vector<PendingCellEdit> pending_edits = model.pending_edits;
    const std::vector<std::size_t> pending_deletes = model.pending_deletes;
    const std::vector<PendingInsertRow> drafts = model.pending_inserts;
    const std::string draft_first_empty = UseString(app::strings::table_draft_first_col_empty);
    const std::string draft_other_empty = UseString(app::strings::table_draft_other_col_empty);
    const std::string draft_acc_empty = UseString(app::strings::table_draft_accessibility_empty);
    const std::string pending_del_tip = UseString(app::strings::table_pending_delete_tooltip);
    const Dialect session_dialect = session->dialect;
    const bool table_editable = model.editable;
    const bool table_read_only = model.read_only;
    std::vector<DataTableColumn> columns;
    columns.reserve(model.columns.size() + 1);
    columns.push_back(MakeRowNumberColumn(
        SafePageOffset(model.page, current_page_size), snapshot ? snapshot->rows.size() : 0,
        model.column_widths, spec.colors.on_surface_variant, semantic.draft_accent,
        pending_deletes, semantic.danger));
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
      column.render_cell = [snapshot, pending_edits, pending_deletes, drafts, index, spec, semantic,
                            draft_first_empty, draft_other_empty, draft_acc_empty, pending_del_tip,
                            menu, dialogs, clipboard, app, workspace, tab_id, session_dialect,
                            is_redis, is_production, prod_conn_name, table_editable,
                            table_read_only, meta, tasks, notify, theme_config, language_mode,
                            system_dark, loc, add_draft_row,
                            reload](const DataTableCellContext& cell) -> DataTableCellContent {
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

        const std::size_t row_idx = RowIndexFromId(cell.id.row_id);
        const bool is_pending_del =
            row_idx != static_cast<std::size_t>(-1) &&
            std::ranges::find(pending_deletes, row_idx) != pending_deletes.end();
        const PendingCellEdit* pe = nullptr;
        if (row_idx != static_cast<std::size_t>(-1)) {
          for (const PendingCellEdit& cand : pending_edits) {
            if (cand.row_index == row_idx && cand.column_index == index) {
              pe = &cand;
              break;
            }
          }
        }
        const bool is_null = pe != nullptr ? pe->is_null : CellIsNull(snapshot, cell.id.row_id, index);
        const std::string raw =
            pe != nullptr ? (pe->is_null ? std::string{"NULL"} : pe->raw_text)
                          : CellText(snapshot, cell.id.row_id, index);
        const std::string text =
            pe != nullptr ? (pe->is_null ? std::string{"NULL"} : pe->display_text)
                          : CellDisplayText(snapshot, cell.id.row_id, index);
        View cell_content;
        if (is_null) {
          const Color null_c = is_pending_del
                                   ? semantic.danger
                                   : (pe != nullptr ? semantic.draft_accent : semantic.null_text);
          cell_content = NullValueBadge(null_c, is_pending_del);
        } else {
          TextStyle style =
              is_pending_del
                  ? TextStyle{
                        .font = Font::Monospace(13.0F).WithWeight(FontWeight::SemiBold),
                        .foreground = semantic.danger,
                        .decoration = TextDecoration::StrikeThrough,
                    }
                  : (pe != nullptr
                         ? TextStyle{Font::Monospace(13.0F).WithWeight(FontWeight::SemiBold),
                                     semantic.draft_accent}
                         : TextStyle{Font::Monospace(13.0F), spec.colors.on_surface});
          cell_content = Text(SingleLineCellPreview(text))
                             .Style(style)
                             .LineLimit(1)
                             .Overflow(TextOverflow::Ellipsis);
          if (is_pending_del) {
            cell_content = std::move(cell_content).With(Tooltip(StringVariant(pending_del_tip)));
          } else if (pe != nullptr) {
            const std::string orig_text = CellIsNull(snapshot, cell.id.row_id, index)
                                              ? std::string{"NULL"}
                                              : CellDisplayText(snapshot, cell.id.row_id, index);
            cell_content = std::move(cell_content).With(Tooltip(StringVariant(orig_text + " -> " + text)));
          } else if (text != raw && !raw.empty()) {
            cell_content = std::move(cell_content).With(Tooltip(StringVariant(raw)));
          }
        }
        const bool can_modify_row =
            table_editable && !table_read_only && row_idx != static_cast<std::size_t>(-1);
        const bool cell_editable = can_modify_row && !is_pending_del && !IsBlobColumn(meta) &&
                                   !(is_redis && meta.name == "size");
        const bool can_set_null = can_modify_row && !is_pending_del &&
                                  !(is_redis && meta.name == "size") && !is_null;
        std::string row_tsv;
        if (const GridRow* gr = RowAt(snapshot, row_idx)) {
          for (std::size_t ci = 0; ci < gr->values.size(); ++ci) {
            if (ci > 0) row_tsv.push_back('\t');
            row_tsv += gr->values[ci].is_null ? std::string{"NULL"} : gr->values[ci].DisplayText();
          }
        }
        const CellValue raw_cell =
            pe != nullptr
                ? CellValue{.text = pe->raw_text, .is_null = pe->is_null, .display_text = pe->display_text}
                : ((RowAt(snapshot, row_idx) != nullptr &&
                    index < RowAt(snapshot, row_idx)->values.size())
                       ? RowAt(snapshot, row_idx)->values[index]
                       : CellValue{});
        const std::string copy_text_val = is_null ? std::string{"NULL"} : text;
        const auto apply_val_filter = [workspace, tab_id, reload](std::string f) {
          workspace.Update([tab_id, &f](WorkspaceModel& ws) {
            if (WorkspaceTab* target = ws.FindTab(tab_id)) {
              target->filter_input = TextEditingValue::FromText(f);
            }
          });
          reload(0, std::move(f));
        };
        const Color cell_bg = is_pending_del
                                  ? WithAlpha(semantic.danger, 0.12F)
                                  : (pe != nullptr ? WithAlpha(semantic.draft_accent, 0.10F)
                                                   : Color::Transparent());
        View cell_box =
            Row{std::move(cell_content)}
                .With(Frame{.min_height = 26.0F}, CrossAlign{CrossAxisAlignment::Center},
                      Background{VisualFill{cell_bg}})
                .On<ViewEvents::ContextMenuRequested>(
                    [menu, dialogs, clipboard, app, workspace, tab_id, row_idx, index,
                     session_dialect, is_redis, cell_editable, can_modify_row, is_pending_del,
                     can_set_null, is_production, prod_conn_name, copy_text_val,
                     row_tsv = std::move(row_tsv), meta, raw_cell, tasks, notify, theme_config,
                     language_mode, system_dark, danger = semantic.status_failed, loc,
                     add_draft_row, apply_val_filter](Point pos) {
                      if (row_idx == static_cast<std::size_t>(-1)) {
                        return;
                      }
                      workspace.Update([tab_id, row_idx, col_name = meta.name](WorkspaceModel& ws) {
                        if (WorkspaceTab* target = ws.FindTab(tab_id)) {
                          target->selected_cell = DataTableCellId{
                              .row_id = std::to_string(row_idx), .column_id = col_name};
                        }
                      });
                      ShowTableCellContextMenu(
                          pos, menu, dialogs, clipboard, app, workspace, tab_id, row_idx, index,
                          session_dialect, is_redis, cell_editable, can_modify_row, is_pending_del,
                          can_set_null, is_production, prod_conn_name, copy_text_val, row_tsv, meta,
                          raw_cell, tasks, notify, theme_config, language_mode, system_dark, danger,
                          loc, add_draft_row, apply_val_filter);
                    });
        return DataTableCellContent{
            .view = std::move(cell_box),
            .label = text,
        };
      };
      if (model.editable && !model.read_only && !IsBlobColumn(meta) &&
          !(is_redis && meta.name == "size")) {
        column.editor = DataTableEditor{
            .initial_text = [workspace, tab_id, live_drafts, snapshot, index,
                             col_id = meta.name](const DataTableCellContext& cell) {
              auto live = live_drafts.Get();
              if (const auto it = live->find(DraftCellBufferKey(cell.id.row_id, col_id));
                  it != live->end()) {
                return it->second;
              }
              const WorkspaceTab* cur = workspace.Get().FindTab(tab_id);
              if (const auto draft_id = ParseDraftRowId(cell.id.row_id)) {
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
              if (cur != nullptr) {
                const std::size_t ridx = RowIndexFromId(cell.id.row_id);
                if (const PendingCellEdit* pe = cur->table.FindPendingEdit(ridx, index)) {
                  return pe->is_null ? std::string{"NULL"} : pe->display_text;
                }
              }
              return CellIsNull(snapshot, cell.id.row_id, index)
                         ? std::string{"NULL"}
                         : CellDisplayText(snapshot, cell.id.row_id, index);
            },
            .render = [live_drafts, inline_editor_focused](const DataTableEditContext& edit) {
              const std::string row_id = edit.cell.id.row_id;
              const std::string col_id = edit.cell.id.column_id;
              const auto entered_focus = inline_editor_focused.Get();
              return CompactTableCellEditor(
                  TextField(edit.draft)
                      .Variant(TextFieldVariant::Standard)
                      .LineLimits(TextFieldLineLimits::SingleLine())
                      .VerticalAlign(TextVerticalAlign::Center)
                      .InputConfiguration({.action = TextInputAction::Done})
                      .OnChanged([draft = edit.draft, live_drafts, row_id, col_id](
                                     const TextEditingValue& value) {
                        draft = value;
                        (*live_drafts.Get())[DraftCellBufferKey(row_id, col_id)] = value.text;
                      })
                      .OnSubmitted(edit.commit)
                      .On<ViewEvents::FocusChanged>(
                          [entered_focus, commit = edit.commit](bool focused) {
                            if (focused) {
                              entered_focus->store(true, std::memory_order_release);
                            } else if (entered_focus->exchange(false, std::memory_order_acq_rel)) {
                              commit();
                            }
                          })
                      .With(edit.focus_target));
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
                [dialogs, app, workspace, tasks, tab_id, live_drafts, apply_draft_value,
                 is_production, prod_conn_name, theme_config, language_mode, system_dark,
                 notify](const DataTableEditRequest& edit) {
                  live_drafts.Get()->erase(
                      DraftCellBufferKey(edit.cell_id.row_id, edit.cell_id.column_id));
                  if (IsDraftRowId(edit.cell_id.row_id)) {
                    apply_draft_value(edit.cell_id.row_id, edit.cell_id.column_id, edit.text);
                    return;
                  }
                  HandleTableEditSubmitted(dialogs, app, workspace, tasks, tab_id, edit,
                                           is_production, prod_conn_name, theme_config,
                                           language_mode, system_dark, notify);
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

  Views page_items;
  page_items.Add(std::move(toolbar).Key("table-toolbar-" + std::to_string(tab_id)));
  if (is_redis_tree) {
    page_items.Add(RedisTreeWorkspace(app, workspace, tab_id, tree_reload_token.Get(), tasks,
                                      notify, theme_config, language_mode, system_dark)
                       .Key("redis-tree-workspace-" + std::to_string(tab_id)));
  } else {
    if (!model.error.empty() && model.snapshot != nullptr) {
      page_items.Add(ErrorText(model.error).Key("table-error-" + std::to_string(tab_id)));
    }
    page_items.Add(std::move(table_view).Key("table-view-" + std::to_string(tab_id)));
    page_items.Add(std::move(pager).Key("table-pager-" + std::to_string(tab_id)));
  }

  return Column{std::move(page_items)}.With(Grow(1.0F), Padding{EdgeInsets{.top = 4.0F}},
                                            Spacing{4.0F},
                                            CrossAlign{CrossAxisAlignment::Stretch});
}

} // namespace dbpp
