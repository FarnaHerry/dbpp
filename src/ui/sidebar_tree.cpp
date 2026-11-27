#include "ui/ui_common.h"

#include <algorithm>
#include <ranges>
#include <sstream>

namespace dbpp {

using namespace huxerui;
using namespace dbpp::ui;

namespace {

bool IsNodeExpanded(const State<std::unordered_map<std::string, bool>>& overrides,
                    const std::string& key, bool default_open = false) {
  const auto& map = overrides.Get();
  const auto it = map.find(key);
  return it != map.end() ? it->second : default_open;
}

void ToggleNodeExpanded(const State<std::unordered_map<std::string, bool>>& overrides,
                        const std::string& key, bool currently_open) {
  overrides.Update([&key, currently_open](std::unordered_map<std::string, bool>& map) {
    map[key] = !currently_open;
  });
}

bool EqualsIgnoreCaseAscii(std::string_view left, std::string_view right) {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t i = 0; i < left.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(left[i])) !=
        std::tolower(static_cast<unsigned char>(right[i]))) {
      return false;
    }
  }
  return true;
}

std::string JoinStrings(const std::vector<std::string>& items, std::string_view sep) {
  std::string out;
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i > 0) {
      out += sep;
    }
    out += items[i];
  }
  return out;
}

void AddEmptyPlaceholderRow(Views& tree_rows, const std::string& key, int indent,
                            const LocalizedCatalog& loc) {
  tree_rows.Add(TreeRow(TreeRowOptions{
                            .prefix = {},
                            .label = loc.Get(app::strings::sidebar_empty_items),
                            .trailing_text = {},
                            .indent_level = indent,
                            .selected = false,
                            .emphasize = false,
                            .active_hint = false,
                            .muted = true,
                        })
                    .Key(key));
}

} // namespace

void AppendTableSubtreeRows(
    Views& tree_rows, const DbSession& session, const DbObject& object,
    const std::string& node_key, int base_indent,
    const State<std::unordered_map<std::string, bool>>& node_open_overrides,
    const State<AppModel>& app, const State<WorkspaceModel>& workspace, const MenuHandle& menu,
    const std::shared_ptr<Clipboard>& clipboard, const NotifyFn& notify,
    const LocalizedCatalog& loc, const ThemeSpec& spec, const SemanticColors& semantic,
    const std::function<void(std::string, StringResource)>& copy_text) {
  (void)clipboard;
  const std::uint64_t session_id = session.id;
  const std::string qualified = QualifiedName(session.dialect, object);

  // 1. Columns (字段)
  const std::string cols_key = node_key + ":cols";
  const bool cols_open = IsNodeExpanded(node_open_overrides, cols_key, false);
  tree_rows.Add(
      TreeRow(TreeRowOptions{
                  .prefix = {},
                  .label = loc.Get(app::strings::sidebar_tbl_columns),
                  .trailing_text = std::to_string(object.columns.size()),
                  .indent_level = base_indent,
                  .selected = false,
                  .emphasize = false,
                  .active_hint = false,
                  .muted = object.columns.empty(),
                  .prefix_color = semantic.accent_table,
                  .leading_icon = TintedIcon(app::images::column, semantic.accent_table, 13.0F),
                  .expanded = cols_open,
              },
              [node_open_overrides, cols_key, cols_open] {
                ToggleNodeExpanded(node_open_overrides, cols_key, cols_open);
              })
          .Key(cols_key));
  if (cols_open) {
    if (object.columns.empty()) {
      AddEmptyPlaceholderRow(tree_rows, cols_key + ":empty", base_indent + 1, loc);
    } else {
      for (std::size_t i = 0; i < object.columns.size(); ++i) {
        const ColumnMeta& col = object.columns[i];
        const ImageResource& col_icon =
            col.primary_key ? app::images::foreign_key : app::images::column;
        const Color col_color = col.primary_key ? spec.colors.primary : semantic.accent_table;
        const std::string col_name = col.name;
        const std::string where_snippet =
            QuoteIdentifier(session.dialect, col.name) + " = ''";
        const std::string select_col_sql =
            "SELECT " + QuoteIdentifier(session.dialect, col.name) + " FROM " + qualified +
            " LIMIT 100;";
        View col_row =
            TreeRow(TreeRowOptions{
                        .prefix = {},
                        .label = col.name,
                        .trailing_text = TypeLabel(col),
                        .indent_level = base_indent + 1,
                        .selected = false,
                        .emphasize = false,
                        .active_hint = false,
                        .muted = false,
                        .prefix_color = col_color,
                        .leading_icon = TintedIcon(col_icon, col_color, 13.0F),
                    })
                .On<ViewEvents::ContextMenuRequested>(
                    [menu, loc, copy_text, app, workspace, session_id, col_name, where_snippet,
                     select_col_sql](Point pos) {
                      menu.ShowAt(pos, {
                          MenuItem(app::images::copy, loc.Get(app::strings::sidebar_ctx_copy_name),
                                   [copy_text, col_name] {
                                     copy_text(col_name, app::strings::sidebar_copy_col_label);
                                   }),
                          MenuItem(app::images::search,
                                   loc.Get(app::strings::sidebar_ctx_filter_col),
                                   [copy_text, where_snippet] {
                                     copy_text(where_snippet, app::strings::sidebar_copy_col_label);
                                   }),
                          MenuItem(app::images::sql_console,
                                   loc.Get(app::strings::sidebar_ctx_open_in_sql),
                                   [app, workspace, session_id, select_col_sql, col_name] {
                                     (void)OpenSqlTab(app, workspace, session_id, select_col_sql,
                                                      "SQL · " + col_name);
                                   }),
                      });
                    })
                .Key(cols_key + ":" + std::to_string(i) + ":" + col.name);
        if (!col.remarks.empty() || !col.default_expr.empty()) {
          std::string tip = col.remarks;
          if (!col.default_expr.empty()) {
            if (!tip.empty()) tip += "\n";
            tip += "DEFAULT " + col.default_expr;
          }
          col_row = std::move(col_row).With(Tooltip(StringVariant(std::move(tip))));
        }
        tree_rows.Add(std::move(col_row));
      }
    }
  }

  if (object.is_view) {
    return;
  }

  // 2. Indexes (索引)
  const std::string idx_key = node_key + ":idx";
  const bool idx_open = IsNodeExpanded(node_open_overrides, idx_key, false);
  tree_rows.Add(
      TreeRow(TreeRowOptions{
                  .prefix = {},
                  .label = loc.Get(app::strings::sidebar_tbl_indexes),
                  .trailing_text = std::to_string(object.indexes.size()),
                  .indent_level = base_indent,
                  .selected = false,
                  .emphasize = false,
                  .active_hint = false,
                  .muted = object.indexes.empty(),
                  .prefix_color = semantic.accent_sql,
                  .leading_icon = TintedIcon(app::images::index, semantic.accent_sql, 13.0F),
                  .expanded = idx_open,
              },
              [node_open_overrides, idx_key, idx_open] {
                ToggleNodeExpanded(node_open_overrides, idx_key, idx_open);
              })
          .Key(idx_key));
  if (idx_open) {
    if (object.indexes.empty()) {
      AddEmptyPlaceholderRow(tree_rows, idx_key + ":empty", base_indent + 1, loc);
    } else {
      for (std::size_t i = 0; i < object.indexes.size(); ++i) {
        const IndexMeta& idx = object.indexes[i];
        std::string trail = idx.primary ? "PK" : (idx.unique ? "UNIQUE" : "");
        const std::string cols_joined = JoinStrings(idx.columns, ", ");
        if (!cols_joined.empty()) {
          if (!trail.empty()) trail += " ";
          trail += "(" + cols_joined + ")";
        }
        const std::string idx_name = idx.name;
        const std::string def =
            idx.definition.empty() ? std::string{} : FormatSqlScript(idx.definition, session.dialect);
        View row =
            TreeRow(TreeRowOptions{
                        .prefix = {},
                        .label = idx.name,
                        .trailing_text = trail,
                        .indent_level = base_indent + 1,
                        .selected = false,
                        .emphasize = false,
                        .active_hint = false,
                        .muted = false,
                        .prefix_color = semantic.accent_sql,
                        .leading_icon = TintedIcon(app::images::index, semantic.accent_sql, 13.0F),
                    })
                .On<ViewEvents::ContextMenuRequested>(
                    [menu, loc, copy_text, idx_name, def](Point pos) {
                      std::vector<MenuEntry> entries;
                      entries.emplace_back(MenuItem(
                          app::images::copy, loc.Get(app::strings::sidebar_ctx_copy_name),
                          [copy_text, idx_name] {
                            copy_text(idx_name, app::strings::sidebar_copy_name_label);
                          }));
                      if (!def.empty()) {
                        entries.emplace_back(MenuItem(
                            app::images::copy, loc.Get(app::strings::sidebar_ctx_copy_def),
                            [copy_text, def] {
                              copy_text(def, app::strings::sidebar_copy_def_label);
                            }));
                      }
                      menu.ShowAt(pos, std::move(entries));
                    })
                .Key(idx_key + ":" + std::to_string(i) + ":" + idx.name);
        if (!def.empty()) {
          row = std::move(row).With(Tooltip(StringVariant(def)));
        }
        tree_rows.Add(std::move(row));
      }
    }
  }

  // 3. Foreign Keys (外键)
  const std::string fk_key = node_key + ":fk";
  const bool fk_open = IsNodeExpanded(node_open_overrides, fk_key, false);
  tree_rows.Add(
      TreeRow(TreeRowOptions{
                  .prefix = {},
                  .label = loc.Get(app::strings::sidebar_tbl_fks),
                  .trailing_text = std::to_string(object.foreign_keys.size()),
                  .indent_level = base_indent,
                  .selected = false,
                  .emphasize = false,
                  .active_hint = false,
                  .muted = object.foreign_keys.empty(),
                  .prefix_color = semantic.accent_external,
                  .leading_icon =
                      TintedIcon(app::images::foreign_key, semantic.accent_external, 13.0F),
                  .expanded = fk_open,
              },
              [node_open_overrides, fk_key, fk_open] {
                ToggleNodeExpanded(node_open_overrides, fk_key, fk_open);
              })
          .Key(fk_key));
  if (fk_open) {
    if (object.foreign_keys.empty()) {
      AddEmptyPlaceholderRow(tree_rows, fk_key + ":empty", base_indent + 1, loc);
    } else {
      for (std::size_t i = 0; i < object.foreign_keys.size(); ++i) {
        const ForeignKeyMeta& fk = object.foreign_keys[i];
        const std::string local_cols = JoinStrings(fk.columns, ", ");
        const std::string ref_cols = JoinStrings(fk.fk_columns, ", ");
        const std::string title =
            fk.constraint_name.empty() ? (local_cols + " -> " + fk.fk_table) : fk.constraint_name;
        const std::string trail = local_cols + " -> " + fk.fk_table +
                                  (ref_cols.empty() ? "" : "(" + ref_cols + ")");
        View row =
            TreeRow(TreeRowOptions{
                        .prefix = {},
                        .label = title,
                        .trailing_text = trail,
                        .indent_level = base_indent + 1,
                        .selected = false,
                        .emphasize = false,
                        .active_hint = false,
                        .muted = false,
                        .prefix_color = semantic.accent_external,
                        .leading_icon = TintedIcon(app::images::foreign_key,
                                                   semantic.accent_external, 13.0F),
                    })
                .On<ViewEvents::ContextMenuRequested>(
                    [menu, loc, copy_text, app, workspace, session_id, title,
                     fk_table = fk.fk_table](Point pos) {
                      menu.ShowAt(pos, {
                          MenuItem(app::images::copy, loc.Get(app::strings::sidebar_ctx_copy_name),
                                   [copy_text, title] {
                                     copy_text(title, app::strings::sidebar_copy_name_label);
                                   }),
                          MenuItem(app::images::sql_console,
                                   loc.Get(app::strings::sidebar_ctx_open_in_sql),
                                   [app, workspace, session_id, fk_table] {
                                     (void)OpenSqlTab(app, workspace, session_id,
                                                      "SELECT * FROM " + fk_table + " LIMIT 100;",
                                                      "SQL · " + fk_table);
                                   }),
                      });
                    })
                .With(Tooltip(StringVariant(trail)))
                .Key(fk_key + ":" + std::to_string(i));
        tree_rows.Add(std::move(row));
      }
    }
  }

  // 4. Table Triggers (触发器)
  std::vector<const TriggerMeta*> tbl_triggers;
  for (const TriggerMeta& trg : session.triggers) {
    const bool tbl_match = EqualsIgnoreCaseAscii(trg.table_name, object.name);
    const bool sch_match = trg.db_schema.empty() || object.db_schema.empty() ||
                           EqualsIgnoreCaseAscii(trg.db_schema, object.db_schema);
    if (tbl_match && sch_match) {
      tbl_triggers.push_back(&trg);
    }
  }
  const std::string trg_key = node_key + ":trg";
  const bool trg_open = IsNodeExpanded(node_open_overrides, trg_key, false);
  tree_rows.Add(
      TreeRow(TreeRowOptions{
                  .prefix = {},
                  .label = loc.Get(app::strings::sidebar_tbl_triggers),
                  .trailing_text = std::to_string(tbl_triggers.size()),
                  .indent_level = base_indent,
                  .selected = false,
                  .emphasize = false,
                  .active_hint = false,
                  .muted = tbl_triggers.empty(),
                  .prefix_color = semantic.warning,
                  .leading_icon = TintedIcon(app::images::trigger, semantic.warning, 13.0F),
                  .expanded = trg_open,
              },
              [node_open_overrides, trg_key, trg_open] {
                ToggleNodeExpanded(node_open_overrides, trg_key, trg_open);
              })
          .Key(trg_key));
  if (trg_open) {
    if (tbl_triggers.empty()) {
      AddEmptyPlaceholderRow(tree_rows, trg_key + ":empty", base_indent + 1, loc);
    } else {
      for (std::size_t i = 0; i < tbl_triggers.size(); ++i) {
        const TriggerMeta& trg = *tbl_triggers[i];
        std::string trail = trg.timing;
        if (!trg.event.empty()) {
          if (!trail.empty()) trail += " ";
          trail += trg.event;
        }
        const std::string trg_name = trg.name;
        const std::string def =
            trg.definition.empty() ? std::string{} : FormatSqlScript(trg.definition, session.dialect);
        View row =
            TreeRow(TreeRowOptions{
                        .prefix = {},
                        .label = trg.name,
                        .trailing_text = trail,
                        .indent_level = base_indent + 1,
                        .selected = false,
                        .emphasize = false,
                        .active_hint = false,
                        .muted = false,
                        .prefix_color = semantic.warning,
                        .leading_icon = TintedIcon(app::images::trigger, semantic.warning, 13.0F),
                    })
                .On<ViewEvents::ContextMenuRequested>(
                    [menu, loc, copy_text, app, workspace, session_id, trg_name, def](Point pos) {
                      std::vector<MenuEntry> entries;
                      if (!def.empty()) {
                        entries.emplace_back(MenuItem(
                            app::images::sql_console, loc.Get(app::strings::sidebar_ctx_open_in_sql),
                            [app, workspace, session_id, def, trg_name] {
                              (void)OpenSqlTab(app, workspace, session_id, def, "SQL · " + trg_name);
                            }));
                        entries.emplace_back(MenuItem(
                            app::images::copy, loc.Get(app::strings::sidebar_ctx_copy_def),
                            [copy_text, def] {
                              copy_text(def, app::strings::sidebar_copy_def_label);
                            }));
                      }
                      entries.emplace_back(MenuItem(
                          app::images::copy, loc.Get(app::strings::sidebar_ctx_copy_name),
                          [copy_text, trg_name] {
                            copy_text(trg_name, app::strings::sidebar_copy_name_label);
                          }));
                      menu.ShowAt(pos, std::move(entries));
                    })
                .Key(trg_key + ":" + std::to_string(i) + ":" + trg.name);
        if (!def.empty()) {
          row = std::move(row).With(Tooltip(StringVariant(def)));
        }
        tree_rows.Add(std::move(row));
      }
    }
  }
  (void)notify;
}

void AppendDatabaseExtraGroups(
    Views& tree_rows, const DbSession& session, const SidebarDatabaseGroup& db_bucket,
    const std::string& db_key, int group_indent,
    const State<std::unordered_map<std::string, bool>>& node_open_overrides,
    const State<AppModel>& app, const State<WorkspaceModel>& workspace,
    const State<std::vector<SavedQuery>>& saved_queries, const std::string& db_path,
    const std::shared_ptr<ConfigDbService>& config_db, const MenuHandle& menu,
    const DialogHandle& dialogs, const std::shared_ptr<Clipboard>& clipboard,
    const NotifyFn& notify, const LocalizedCatalog& loc, const ThemeSpec& spec,
    const SemanticColors& semantic, AppThemeConfig theme_config, LanguageMode language_mode,
    bool system_dark, const std::function<void(std::string, StringResource)>& copy_text) {
  (void)clipboard;
  (void)spec;
  const std::uint64_t session_id = session.id;
  const std::string conn_key = SavedQueryConnectionKey(session.driver_path, session.uri);

  const auto add_routine_group =
      [&](std::string_view key_suffix, StringResource label_res, const ImageResource& icon_res,
          Color accent, const std::vector<const RoutineMeta*>& items) {
        const std::string group_key = db_key + ":" + std::string(key_suffix);
        const bool is_open = IsNodeExpanded(node_open_overrides, group_key, !items.empty());
        tree_rows.Add(
            TreeRow(TreeRowOptions{
                        .prefix = {},
                        .label = loc.Get(label_res),
                        .trailing_text = std::to_string(items.size()),
                        .indent_level = group_indent,
                        .selected = false,
                        .emphasize = !items.empty(),
                        .active_hint = false,
                        .muted = items.empty(),
                        .prefix_color = accent,
                        .leading_icon = TintedIcon(icon_res, accent, 14.0F),
                        .expanded = is_open,
                    },
                    [node_open_overrides, group_key, is_open] {
                      ToggleNodeExpanded(node_open_overrides, group_key, is_open);
                    })
                .Key(group_key));
        if (!is_open) {
          return;
        }
        if (items.empty()) {
          AddEmptyPlaceholderRow(tree_rows, group_key + ":empty", group_indent + 1, loc);
          return;
        }
        for (std::size_t i = 0; i < items.size(); ++i) {
          const RoutineMeta& rt = *items[i];
          std::string trail = rt.return_type;
          const std::string name = rt.name;
          const std::string def =
              rt.definition.empty() ? std::string{} : FormatSqlScript(rt.definition, session.dialect);
          const std::string call_sql =
              rt.is_function ? ("SELECT " + rt.name + "(" + rt.signature + ");")
                             : ("CALL " + rt.name + "(" + rt.signature + ");");
          View row =
              TreeRow(TreeRowOptions{
                          .prefix = {},
                          .label = rt.name,
                          .trailing_text = trail,
                          .indent_level = group_indent + 1,
                          .selected = false,
                          .emphasize = false,
                          .active_hint = false,
                          .muted = false,
                          .prefix_color = accent,
                          .leading_icon = TintedIcon(icon_res, accent, 13.0F),
                      },
                      [app, workspace, session_id, call_sql, def, name, db_name = db_bucket.name] {
                        (void)OpenSqlTab(app, workspace, session_id,
                                         !def.empty() ? def : call_sql, "SQL · " + name, db_name);
                      })
                  .On<ViewEvents::ContextMenuRequested>(
                      [menu, loc, copy_text, app, workspace, session_id, name, def, call_sql,
                       db_name = db_bucket.name](Point pos) {
                        std::vector<MenuEntry> entries;
                        entries.emplace_back(MenuItem(
                            app::images::sql_console, loc.Get(app::strings::sidebar_ctx_open_in_sql),
                            [app, workspace, session_id, def, call_sql, name, db_name] {
                              (void)OpenSqlTab(app, workspace, session_id,
                                               !def.empty() ? def : call_sql, "SQL · " + name,
                                               db_name);
                            }));
                        entries.emplace_back(MenuItem(
                            app::images::copy, loc.Get(app::strings::sidebar_ctx_copy_name),
                            [copy_text, name] {
                              copy_text(name, app::strings::sidebar_copy_name_label);
                            }));
                        if (!def.empty()) {
                          entries.emplace_back(MenuItem(
                              app::images::copy, loc.Get(app::strings::sidebar_ctx_copy_def),
                              [copy_text, def] {
                                copy_text(def, app::strings::sidebar_copy_def_label);
                              }));
                        }
                        menu.ShowAt(pos, std::move(entries));
                      })
                  .Key(group_key + ":" + std::to_string(i) + ":" + rt.name);
          if (!rt.signature.empty() || !rt.definition.empty()) {
            row = std::move(row).With(
                Tooltip(StringVariant(!rt.definition.empty() ? rt.definition : rt.signature)));
          }
          tree_rows.Add(std::move(row));
        }
      };

  // Procedures & Functions
  add_routine_group("procedures", app::strings::sidebar_group_procedures, app::images::procedure,
                    semantic.accent_sql, db_bucket.procedures);
  add_routine_group("functions", app::strings::sidebar_group_functions, app::images::function,
                    semantic.accent_sql, db_bucket.functions);

  // Triggers (database level)
  {
    const std::string group_key = db_key + ":triggers";
    const bool is_open =
        IsNodeExpanded(node_open_overrides, group_key, !db_bucket.triggers.empty());
    tree_rows.Add(
        TreeRow(TreeRowOptions{
                    .prefix = {},
                    .label = loc.Get(app::strings::sidebar_group_triggers),
                    .trailing_text = std::to_string(db_bucket.triggers.size()),
                    .indent_level = group_indent,
                    .selected = false,
                    .emphasize = !db_bucket.triggers.empty(),
                    .active_hint = false,
                    .muted = db_bucket.triggers.empty(),
                    .prefix_color = semantic.warning,
                    .leading_icon = TintedIcon(app::images::trigger, semantic.warning, 14.0F),
                    .expanded = is_open,
                },
                [node_open_overrides, group_key, is_open] {
                  ToggleNodeExpanded(node_open_overrides, group_key, is_open);
                })
            .Key(group_key));
    if (is_open) {
      if (db_bucket.triggers.empty()) {
        AddEmptyPlaceholderRow(tree_rows, group_key + ":empty", group_indent + 1, loc);
      } else {
        for (std::size_t i = 0; i < db_bucket.triggers.size(); ++i) {
          const TriggerMeta& trg = *db_bucket.triggers[i];
          std::string trail = trg.table_name;
          if (!trg.timing.empty() || !trg.event.empty()) {
            if (!trail.empty()) trail += " · ";
            trail += trg.timing + (trg.event.empty() ? "" : " " + trg.event);
          }
          const std::string name = trg.name;
          const std::string def =
              trg.definition.empty() ? std::string{} : FormatSqlScript(trg.definition, session.dialect);
          View row =
              TreeRow(TreeRowOptions{
                          .prefix = {},
                          .label = trg.name,
                          .trailing_text = trail,
                          .indent_level = group_indent + 1,
                          .selected = false,
                          .emphasize = false,
                          .active_hint = false,
                          .muted = false,
                          .prefix_color = semantic.warning,
                          .leading_icon = TintedIcon(app::images::trigger, semantic.warning, 13.0F),
                      })
                  .On<ViewEvents::ContextMenuRequested>(
                      [menu, loc, copy_text, app, workspace, session_id, name, def,
                       db_name = db_bucket.name](Point pos) {
                        std::vector<MenuEntry> entries;
                        if (!def.empty()) {
                          entries.emplace_back(MenuItem(
                              app::images::sql_console,
                              loc.Get(app::strings::sidebar_ctx_open_in_sql),
                              [app, workspace, session_id, def, name, db_name] {
                                (void)OpenSqlTab(app, workspace, session_id, def, "SQL · " + name,
                                                 db_name);
                              }));
                          entries.emplace_back(MenuItem(
                              app::images::copy, loc.Get(app::strings::sidebar_ctx_copy_def),
                              [copy_text, def] {
                                copy_text(def, app::strings::sidebar_copy_def_label);
                              }));
                        }
                        entries.emplace_back(MenuItem(
                            app::images::copy, loc.Get(app::strings::sidebar_ctx_copy_name),
                            [copy_text, name] {
                              copy_text(name, app::strings::sidebar_copy_name_label);
                            }));
                        menu.ShowAt(pos, std::move(entries));
                      })
                  .Key(group_key + ":" + std::to_string(i) + ":" + trg.name);
          if (!def.empty()) {
            row = std::move(row).With(Tooltip(StringVariant(def)));
          }
          tree_rows.Add(std::move(row));
        }
      }
    }
  }

  // Events (事件)
  {
    const std::string group_key = db_key + ":events";
    const bool is_open =
        IsNodeExpanded(node_open_overrides, group_key, !db_bucket.events.empty());
    tree_rows.Add(
        TreeRow(TreeRowOptions{
                    .prefix = {},
                    .label = loc.Get(app::strings::sidebar_group_events),
                    .trailing_text = std::to_string(db_bucket.events.size()),
                    .indent_level = group_indent,
                    .selected = false,
                    .emphasize = !db_bucket.events.empty(),
                    .active_hint = false,
                    .muted = db_bucket.events.empty(),
                    .prefix_color = semantic.accent_external,
                    .leading_icon = TintedIcon(app::images::event, semantic.accent_external, 14.0F),
                    .expanded = is_open,
                },
                [node_open_overrides, group_key, is_open] {
                  ToggleNodeExpanded(node_open_overrides, group_key, is_open);
                })
            .Key(group_key));
    if (is_open) {
      if (db_bucket.events.empty()) {
        AddEmptyPlaceholderRow(tree_rows, group_key + ":empty", group_indent + 1, loc);
      } else {
        for (std::size_t i = 0; i < db_bucket.events.size(); ++i) {
          const EventMeta& ev = *db_bucket.events[i];
          std::string trail = ev.schedule;
          if (!ev.status.empty()) {
            if (!trail.empty()) trail += " · ";
            trail += ev.status;
          }
          const std::string name = ev.name;
          const std::string def =
              ev.definition.empty() ? std::string{} : FormatSqlScript(ev.definition, session.dialect);
          View row =
              TreeRow(TreeRowOptions{
                          .prefix = {},
                          .label = ev.name,
                          .trailing_text = trail,
                          .indent_level = group_indent + 1,
                          .selected = false,
                          .emphasize = false,
                          .active_hint = false,
                          .muted = false,
                          .prefix_color = semantic.accent_external,
                          .leading_icon =
                              TintedIcon(app::images::event, semantic.accent_external, 13.0F),
                      })
                  .On<ViewEvents::ContextMenuRequested>(
                      [menu, loc, copy_text, app, workspace, session_id, name, def,
                       db_name = db_bucket.name](Point pos) {
                        std::vector<MenuEntry> entries;
                        if (!def.empty()) {
                          entries.emplace_back(MenuItem(
                              app::images::sql_console,
                              loc.Get(app::strings::sidebar_ctx_open_in_sql),
                              [app, workspace, session_id, def, name, db_name] {
                                (void)OpenSqlTab(app, workspace, session_id, def, "SQL · " + name,
                                                 db_name);
                              }));
                          entries.emplace_back(MenuItem(
                              app::images::copy, loc.Get(app::strings::sidebar_ctx_copy_def),
                              [copy_text, def] {
                                copy_text(def, app::strings::sidebar_copy_def_label);
                              }));
                        }
                        entries.emplace_back(MenuItem(
                            app::images::copy, loc.Get(app::strings::sidebar_ctx_copy_name),
                            [copy_text, name] {
                              copy_text(name, app::strings::sidebar_copy_name_label);
                            }));
                        menu.ShowAt(pos, std::move(entries));
                      })
                  .Key(group_key + ":" + std::to_string(i) + ":" + ev.name);
          if (!def.empty()) {
            row = std::move(row).With(Tooltip(StringVariant(def)));
          }
          tree_rows.Add(std::move(row));
        }
      }
    }
  }

  // Queries (查询 - saved SQL queries for this connection / database)
  {
    const auto& all_queries = saved_queries.Get();
    std::vector<const SavedQuery*> matching;
    for (const SavedQuery& q : all_queries) {
      const bool conn_match = q.connection_key.empty() || q.connection_key == conn_key;
      const bool db_match =
          q.database_name.empty() || EqualsIgnoreCaseAscii(q.database_name, db_bucket.name);
      if (conn_match && db_match) {
        matching.push_back(&q);
      }
    }
    const std::string group_key = db_key + ":queries";
    const bool is_open = IsNodeExpanded(node_open_overrides, group_key, !matching.empty());
    const auto open_new_query_dialog = [dialogs, conn_key, db_name = db_bucket.name, saved_queries,
                                        db_path, notify, theme_config, language_mode, system_dark] {
      dialogs.Show([conn_key, db_name, saved_queries, db_path, notify, theme_config,
                    language_mode, system_dark](DialogContext dialog) {
        return WithDialogTheme(
            theme_config, language_mode, system_dark,
            SaveQueryDialog(dialog, std::nullopt, conn_key, db_name, {}, {}, saved_queries,
                            db_path, notify));
      });
    };

    tree_rows.Add(
        TreeRow(TreeRowOptions{
                    .prefix = {},
                    .label = loc.Get(app::strings::sidebar_group_queries),
                    .trailing_text = std::to_string(matching.size()),
                    .indent_level = group_indent,
                    .selected = false,
                    .emphasize = !matching.empty(),
                    .active_hint = false,
                    .muted = matching.empty(),
                    .prefix_color = semantic.accent_view,
                    .leading_icon = TintedIcon(app::images::query, semantic.accent_view, 14.0F),
                    .expanded = is_open,
                },
                [node_open_overrides, group_key, is_open] {
                  ToggleNodeExpanded(node_open_overrides, group_key, is_open);
                })
            .On<ViewEvents::ContextMenuRequested>(
                [menu, loc, open_new_query_dialog](Point pos) {
                  menu.ShowAt(pos, {
                      MenuItem(app::images::add, loc.Get(app::strings::sidebar_ctx_new_saved_query),
                               open_new_query_dialog),
                  });
                })
            .Key(group_key));
    if (is_open) {
      if (matching.empty()) {
        AddEmptyPlaceholderRow(tree_rows, group_key + ":empty", group_indent + 1, loc);
      } else {
        for (std::size_t i = 0; i < matching.size(); ++i) {
          const SavedQuery q = *matching[i];
          const auto open_query = [app, workspace, session_id, q, db_name = db_bucket.name] {
            (void)OpenSqlTab(app, workspace, session_id, q.sql_text, q.name,
                             q.database_name.empty() ? db_name : q.database_name);
          };
          const auto edit_query = [dialogs, q, saved_queries, db_path, notify, theme_config,
                                   language_mode, system_dark] {
            dialogs.Show([q, saved_queries, db_path, notify, theme_config, language_mode,
                          system_dark](DialogContext dialog) {
              return WithDialogTheme(
                  theme_config, language_mode, system_dark,
                  SaveQueryDialog(dialog, q, q.connection_key, q.database_name, q.name, q.sql_text,
                                  saved_queries, db_path, notify));
            });
          };
          const auto delete_query = [config_db, saved_queries, db_path, q, notify, loc] {
            std::vector<SavedQuery> updated = saved_queries.Get();
            const auto it = std::ranges::find(updated, q);
            if (it != updated.end()) {
              updated.erase(it);
            }
            if (!PersistSavedQueryList(config_db, db_path, updated)) {
              return;
            }
            saved_queries = std::move(updated);
            if (notify) {
              notify(loc.Fmt(app::strings::toast_query_deleted, q.name));
            }
          };
          tree_rows.Add(
              TreeRow(TreeRowOptions{
                          .prefix = {},
                          .label = q.name,
                          .trailing_text = CompactSqlPreview(q.sql_text, 28),
                          .indent_level = group_indent + 1,
                          .selected = false,
                          .emphasize = false,
                          .active_hint = false,
                          .muted = false,
                          .prefix_color = semantic.accent_view,
                          .leading_icon =
                              TintedIcon(app::images::query, semantic.accent_view, 13.0F),
                      },
                      open_query)
                  .On<ViewEvents::ContextMenuRequested>(
                      [menu, loc, open_query, edit_query, delete_query, copy_text,
                       sql_text = q.sql_text, danger = semantic.status_failed](Point pos) {
                        menu.ShowAt(pos, {
                            MenuItem(app::images::sql_console,
                                     loc.Get(app::strings::sidebar_ctx_open_query), open_query),
                            MenuItem(app::images::edit,
                                     loc.Get(app::strings::sidebar_ctx_edit_query), edit_query),
                            MenuItem(app::images::copy,
                                     loc.Get(app::strings::sidebar_ctx_copy_def),
                                     [copy_text, sql_text] {
                                       copy_text(sql_text, app::strings::sidebar_copy_def_label);
                                     }),
                            MenuSection{},
                            MenuItem(app::images::trash,
                                     loc.Get(app::strings::sidebar_ctx_delete_query),
                                     delete_query)
                                .IconTint(danger),
                        });
                      })
                  .With(Tooltip(StringVariant(q.sql_text)))
                  .Key(group_key + ":" + std::to_string(i) + ":" + q.name));
        }
      }
    }
  }
}

} // namespace dbpp
