#include "ui/ui_common.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dbpp {

using namespace huxerui;
using namespace dbpp::ui;

namespace {

float ResolvedColumnWidth(const std::unordered_map<std::string, float>& widths,
                          const std::string& id, float fallback, float min_w = 60.0F,
                          float max_w = 800.0F) {
  if (const auto it = widths.find(id); it != widths.end()) {
    return std::clamp(it->second, min_w, max_w);
  }
  return std::clamp(fallback, min_w, max_w);
}

std::string JoinCommaIdentifiers(const std::vector<std::string>& items) {
  std::string out;
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i > 0) {
      out += ", ";
    }
    out += items[i];
  }
  return out;
}

DataTableCellContent MakeTextCell(
    std::string display, std::string full_tooltip, const TextStyle& style,
    std::size_t row_idx, std::size_t col_idx,
    const std::function<void(Point, std::size_t, std::size_t)>& on_context_menu) {
  std::string label = display;
  const std::string preview = SingleLineCellPreview(display);
  View text_view = Text(preview.empty() ? std::string{"—"} : preview)
                       .Style(style)
                       .LineLimit(1)
                       .Overflow(TextOverflow::Ellipsis);
  if (!full_tooltip.empty()) {
    text_view = std::move(text_view).With(Tooltip(StringVariant(std::move(full_tooltip))));
  } else if (preview != display && !display.empty()) {
    text_view = std::move(text_view).With(Tooltip(StringVariant(display)));
  }
  if (on_context_menu) {
    text_view =
        Row{std::move(text_view)}
            .With(Frame{.min_height = 26.0F}, CrossAlign{CrossAxisAlignment::Center})
            .On<ViewEvents::ContextMenuRequested>([on_context_menu, row_idx, col_idx](Point pos) {
              if (row_idx != static_cast<std::size_t>(-1)) {
                on_context_menu(pos, row_idx, col_idx);
              }
            });
  }
  return DataTableCellContent{
      .view = std::move(text_view),
      .label = std::move(label),
  };
}

DataTableColumn MakeSimpleTextColumn(
    std::string id, std::string header, float fallback_width, float min_width, float max_width,
    const std::unordered_map<std::string, float>& widths, TextStyle style, std::size_t row_count,
    std::size_t col_idx, std::function<std::pair<std::string, std::string>(std::size_t)> extract,
    std::function<void(Point, std::size_t, std::size_t)> on_context_menu = {}) {
  DataTableColumn col;
  col.id = id;
  col.header = std::move(header);
  col.sortable = false;
  col.width = DataTableColumnWidth{
      .initial = ResolvedColumnWidth(widths, id, fallback_width, min_width, max_width),
      .min = min_width,
      .max = max_width,
  };
  col.render_cell = [row_count, col_idx, style, extract = std::move(extract),
                     on_context_menu = std::move(on_context_menu)](
                        const DataTableCellContext& cell) -> DataTableCellContent {
    const std::size_t row_idx = RowIndexFromId(cell.id.row_id);
    if (row_idx == static_cast<std::size_t>(-1) || row_idx >= row_count) {
      return DataTableCellContent{.view = View{}, .label = {}};
    }
    auto [display, tooltip] = extract(row_idx);
    return MakeTextCell(std::move(display), std::move(tooltip), style, row_idx, col_idx,
                        on_context_menu);
  };
  return col;
}

} // namespace

View SchemaSectionPill(std::string label, bool selected, std::function<void()> on_click,
                       const ThemeSpec& spec, const SemanticColors& semantic) {
  const Color bg = selected ? spec.colors.primary_container : spec.colors.surface_container;
  const Color fg = selected ? spec.colors.on_primary_container : spec.colors.on_surface_variant;
  const Color border = selected ? spec.colors.primary : semantic.border_subtle;
  return Row{
      Text(std::move(label))
          .Style(TextStyle{Font::System(font_size::kChip)
                               .WithWeight(selected ? FontWeight::SemiBold : FontWeight::Medium),
                           fg})
          .LineLimit(1),
  }
      .With(Frame{.height = 28.0F, .min_height = 28.0F, .max_height = 28.0F},
            Padding{EdgeInsets::Symmetric(12.0F, 0.0F)},
            CrossAlign{CrossAxisAlignment::Center}, CornerRadius{6.0F},
            Background{VisualFill{bg}}, Border{border, selected ? 1.5F : 1.0F})
      .OnClick(std::move(on_click));
}

DataTableRowSource MakeSequentialRows(std::size_t count) {
  return DataTableRowSource{
      .count = count,
      .id_at = [](std::size_t index) { return std::to_string(index); },
      .index_of = [count](std::string_view id) -> std::optional<std::size_t> {
        const std::size_t index = RowIndexFromId(id);
        if (index == static_cast<std::size_t>(-1) || index >= count) {
          return std::nullopt;
        }
        return index;
      },
  };
}

std::vector<DataTableColumn> BuildSchemaColumnsTable(
    const std::vector<ColumnMeta>& cols, const std::unordered_map<std::string, float>& column_widths,
    const ThemeSpec& spec, const SemanticColors& semantic, const LocalizedCatalog& loc,
    std::function<void(Point, std::size_t, std::size_t)> on_context_menu) {
  (void)semantic;
  auto cols_ptr = std::make_shared<std::vector<ColumnMeta>>(cols);
  std::vector<DataTableColumn> out;
  out.reserve(7);
  out.push_back(MakeRowNumberColumn(0, cols.size(), column_widths, spec.colors.on_surface_variant,
                                    spec.colors.on_surface_variant));

  const TextStyle name_style{Font::System(font_size::kChip).WithWeight(FontWeight::SemiBold),
                             spec.colors.on_surface};
  const TextStyle mono_style{Font::Monospace(font_size::kChip), spec.colors.on_surface};
  const TextStyle muted_style{Font::System(font_size::kChip), spec.colors.on_surface_variant};
  const TextStyle pk_style{Font::Monospace(font_size::kChip).WithWeight(FontWeight::SemiBold),
                           spec.colors.primary};

  out.push_back(MakeSimpleTextColumn(
      "col_name", loc.Get(app::strings::schema_col_name), 180.0F, 80.0F, 460.0F, column_widths,
      name_style, cols.size(), 0,
      [cols_ptr](std::size_t r) {
        return std::pair{(*cols_ptr)[r].name, (*cols_ptr)[r].name};
      },
      on_context_menu));

  out.push_back(MakeSimpleTextColumn(
      "col_type", loc.Get(app::strings::schema_col_type), 150.0F, 76.0F, 400.0F, column_widths,
      mono_style, cols.size(), 1,
      [cols_ptr](std::size_t r) {
        const ColumnMeta& cm = (*cols_ptr)[r];
        std::string t = cm.type.empty() ? std::string{"ANY"} : cm.type;
        return std::pair{t, t};
      },
      on_context_menu));

  const std::string yes_label = loc.Get(app::strings::schema_val_yes);
  const std::string not_null_label = loc.Get(app::strings::schema_val_not_null);
  out.push_back(MakeSimpleTextColumn(
      "col_nullable", loc.Get(app::strings::schema_col_nullable), 100.0F, 72.0F, 180.0F,
      column_widths, muted_style, cols.size(), 2,
      [cols_ptr, yes_label, not_null_label](std::size_t r) {
        return std::pair{(*cols_ptr)[r].nullable ? yes_label : not_null_label, std::string{}};
      },
      on_context_menu));

  const std::string pk_label = loc.Get(app::strings::edit_table_col_pk);
  out.push_back(MakeSimpleTextColumn(
      "col_pk", loc.Get(app::strings::schema_col_pk), 72.0F, 56.0F, 140.0F, column_widths,
      pk_style, cols.size(), 3,
      [cols_ptr, pk_label](std::size_t r) {
        const ColumnMeta& cm = (*cols_ptr)[r];
        if (!cm.primary_key) {
          return std::pair{std::string{"—"}, std::string{}};
        }
        std::string text = cm.primary_key_order > 1
                               ? (pk_label + " #" + std::to_string(cm.primary_key_order))
                               : pk_label;
        return std::pair{text, std::string{}};
      },
      on_context_menu));

  out.push_back(MakeSimpleTextColumn(
      "col_default", loc.Get(app::strings::schema_col_default), 140.0F, 72.0F, 420.0F,
      column_widths, mono_style, cols.size(), 4,
      [cols_ptr](std::size_t r) {
        const std::string& def = (*cols_ptr)[r].default_expr;
        return std::pair{def.empty() ? std::string{"—"} : def, def};
      },
      on_context_menu));

  out.push_back(MakeSimpleTextColumn(
      "col_remarks", loc.Get(app::strings::schema_col_remarks), 180.0F, 80.0F, 520.0F,
      column_widths, muted_style, cols.size(), 5,
      [cols_ptr](std::size_t r) {
        const std::string& rem = (*cols_ptr)[r].remarks;
        return std::pair{rem.empty() ? std::string{"—"} : rem, rem};
      },
      on_context_menu));

  return out;
}

std::vector<DataTableColumn> BuildSchemaForeignKeysTable(
    const std::vector<ForeignKeyMeta>& fks,
    const std::unordered_map<std::string, float>& column_widths, const ThemeSpec& spec,
    const SemanticColors& semantic, const LocalizedCatalog& loc,
    std::function<void(Point, std::size_t)> on_context_menu) {
  (void)semantic;
  auto fks_ptr = std::make_shared<std::vector<ForeignKeyMeta>>(fks);
  std::vector<DataTableColumn> out;
  out.reserve(5);
  out.push_back(MakeRowNumberColumn(0, fks.size(), column_widths, spec.colors.on_surface_variant,
                                    spec.colors.on_surface_variant));
  const TextStyle name_style{Font::System(font_size::kChip).WithWeight(FontWeight::SemiBold),
                             spec.colors.on_surface};
  const TextStyle mono_style{Font::Monospace(font_size::kChip), spec.colors.on_surface};

  std::function<void(Point, std::size_t, std::size_t)> ctx;
  if (on_context_menu) {
    ctx = [on_context_menu = std::move(on_context_menu)](Point p, std::size_t r, std::size_t) {
      on_context_menu(p, r);
    };
  }

  out.push_back(MakeSimpleTextColumn(
      "fk_name", loc.Get(app::strings::schema_fk_name), 180.0F, 90.0F, 420.0F, column_widths,
      name_style, fks.size(), 0,
      [fks_ptr](std::size_t r) {
        const std::string& n = (*fks_ptr)[r].constraint_name;
        return std::pair{n.empty() ? std::string{"—"} : n, n};
      },
      ctx));

  out.push_back(MakeSimpleTextColumn(
      "fk_cols", loc.Get(app::strings::schema_fk_cols), 180.0F, 80.0F, 420.0F, column_widths,
      mono_style, fks.size(), 1,
      [fks_ptr](std::size_t r) {
        std::string s = JoinCommaIdentifiers((*fks_ptr)[r].columns);
        return std::pair{s.empty() ? std::string{"—"} : s, s};
      },
      ctx));

  out.push_back(MakeSimpleTextColumn(
      "fk_ref_table", loc.Get(app::strings::schema_fk_ref_table), 180.0F, 90.0F, 420.0F,
      column_widths, name_style, fks.size(), 2,
      [fks_ptr](std::size_t r) {
        const ForeignKeyMeta& fk = (*fks_ptr)[r];
        std::string ref = fk.fk_db_schema.empty() ? fk.fk_table
                                                 : (fk.fk_db_schema + "." + fk.fk_table);
        return std::pair{ref.empty() ? std::string{"—"} : ref, ref};
      },
      ctx));

  out.push_back(MakeSimpleTextColumn(
      "fk_ref_cols", loc.Get(app::strings::schema_fk_ref_cols), 180.0F, 80.0F, 420.0F,
      column_widths, mono_style, fks.size(), 3,
      [fks_ptr](std::size_t r) {
        std::string s = JoinCommaIdentifiers((*fks_ptr)[r].fk_columns);
        return std::pair{s.empty() ? std::string{"—"} : s, s};
      },
      ctx));

  return out;
}

std::vector<DataTableColumn> BuildSchemaIndexesTable(
    const std::vector<IndexMeta>& indexes, const std::unordered_map<std::string, float>& widths,
    const ThemeSpec& spec, const LocalizedCatalog& loc,
    std::function<void(Point, std::size_t, std::size_t)> on_context_menu) {
  auto idx_ptr = std::make_shared<std::vector<IndexMeta>>(indexes);
  std::vector<DataTableColumn> cols;
  cols.reserve(5);
  cols.push_back(MakeRowNumberColumn(0, indexes.size(), widths, spec.colors.on_surface_variant,
                                     spec.colors.on_surface_variant));
  const TextStyle primary_semi{Font::System(font_size::kChip).WithWeight(FontWeight::SemiBold),
                               spec.colors.on_surface};
  const TextStyle body_style{Font::System(font_size::kChip), spec.colors.on_surface};
  const TextStyle muted_style{Font::System(font_size::kChip), spec.colors.on_surface_variant};
  const TextStyle mono_style{Font::Monospace(font_size::kChip), spec.colors.on_surface_variant};

  cols.push_back(MakeSimpleTextColumn(
      "idx_name", loc.Get(app::strings::schema_idx_name), 200.0F, 90.0F, 480.0F, widths,
      primary_semi, indexes.size(), 0,
      [idx_ptr](std::size_t r) {
        return std::pair{(*idx_ptr)[r].name, (*idx_ptr)[r].name};
      },
      on_context_menu));

  cols.push_back(MakeSimpleTextColumn(
      "idx_type", loc.Get(app::strings::schema_idx_type), 100.0F, 72.0F, 180.0F, widths,
      body_style, indexes.size(), 1,
      [idx_ptr](std::size_t r) {
        const IndexMeta& ix = (*idx_ptr)[r];
        std::string kind = ix.primary ? "PRIMARY" : (ix.unique ? "UNIQUE" : "INDEX");
        return std::pair{kind, std::string{}};
      },
      on_context_menu));

  cols.push_back(MakeSimpleTextColumn(
      "idx_cols", loc.Get(app::strings::schema_idx_cols), 220.0F, 100.0F, 520.0F, widths,
      mono_style, indexes.size(), 2,
      [idx_ptr](std::size_t r) {
        std::string joined = JoinCommaIdentifiers((*idx_ptr)[r].columns);
        std::string tip = joined;
        return std::pair{joined.empty() ? std::string{"—"} : std::move(joined), std::move(tip)};
      },
      on_context_menu));

  cols.push_back(MakeSimpleTextColumn(
      "idx_def", loc.Get(app::strings::schema_idx_def), 380.0F, 140.0F, 900.0F, widths,
      muted_style, indexes.size(), 3,
      [idx_ptr](std::size_t r) {
        const std::string& def = (*idx_ptr)[r].definition;
        return std::pair{def.empty() ? std::string{"—"} : def, def};
      },
      on_context_menu));

  return cols;
}

std::vector<DataTableColumn> BuildSchemaTriggersTable(
    const std::vector<TriggerMeta>& triggers, const std::unordered_map<std::string, float>& widths,
    const ThemeSpec& spec, const LocalizedCatalog& loc,
    std::function<void(Point, std::size_t, std::size_t)> on_context_menu) {
  auto trg_ptr = std::make_shared<std::vector<TriggerMeta>>(triggers);
  std::vector<DataTableColumn> cols;
  cols.reserve(5);
  cols.push_back(MakeRowNumberColumn(0, triggers.size(), widths, spec.colors.on_surface_variant,
                                     spec.colors.on_surface_variant));
  const TextStyle primary_semi{Font::System(font_size::kChip).WithWeight(FontWeight::SemiBold),
                               spec.colors.on_surface};
  const TextStyle body_style{Font::System(font_size::kChip), spec.colors.on_surface};
  const TextStyle mono_style{Font::Monospace(font_size::kChip), spec.colors.on_surface_variant};

  cols.push_back(MakeSimpleTextColumn(
      "trg_name", loc.Get(app::strings::schema_trg_name), 190.0F, 90.0F, 440.0F, widths,
      primary_semi, triggers.size(), 0,
      [trg_ptr](std::size_t r) {
        return std::pair{(*trg_ptr)[r].name, (*trg_ptr)[r].name};
      },
      on_context_menu));

  cols.push_back(MakeSimpleTextColumn(
      "trg_timing", loc.Get(app::strings::schema_trg_timing), 110.0F, 72.0F, 180.0F, widths,
      body_style, triggers.size(), 1,
      [trg_ptr](std::size_t r) {
        const std::string& t = (*trg_ptr)[r].timing;
        return std::pair{t.empty() ? std::string{"—"} : t, std::string{}};
      },
      on_context_menu));

  cols.push_back(MakeSimpleTextColumn(
      "trg_event", loc.Get(app::strings::schema_trg_event), 130.0F, 80.0F, 240.0F, widths,
      body_style, triggers.size(), 2,
      [trg_ptr](std::size_t r) {
        const std::string& e = (*trg_ptr)[r].event;
        return std::pair{e.empty() ? std::string{"—"} : e, std::string{}};
      },
      on_context_menu));

  cols.push_back(MakeSimpleTextColumn(
      "trg_def", loc.Get(app::strings::schema_trg_def), 420.0F, 160.0F, 960.0F, widths,
      mono_style, triggers.size(), 3,
      [trg_ptr](std::size_t r) {
        const std::string& def = (*trg_ptr)[r].definition;
        return std::pair{def.empty() ? std::string{"—"} : def, def};
      },
      on_context_menu));

  return cols;
}

[[huxerui::composable]] View TableSortButton(
    const MenuHandle& menu, const ViewHandle& anchor, const LocalizedCatalog& loc,
    const std::vector<ColumnMeta>& sort_cols, const std::optional<DataTableSort>& current_sort,
    bool enabled, float height, std::function<void(std::optional<DataTableSort>)> on_select) {
  const ThemeSpec& spec = UseTheme();
  const SemanticColors& semantic = UseEnvironment<SemanticColors>();
  const bool sort_active = current_sort.has_value();
  const std::string sort_label =
      sort_active ? UseString(current_sort->direction == DataTableSortDirection::Descending
                                  ? app::strings::table_sort_active_desc
                                  : app::strings::table_sort_active_asc,
                              current_sort->column_id)
                  : UseString(app::strings::table_sort);
  const Color sort_fg = sort_active ? spec.colors.on_primary_container : spec.colors.on_surface;
  const Color sort_bg = sort_active ? spec.colors.primary_container : spec.colors.surface;
  const Color sort_border =
      sort_active ? WithAlpha(spec.colors.primary, 0.45F) : semantic.border_subtle;
  const Color sort_icon_color =
      sort_active ? spec.colors.primary : spec.colors.on_surface_variant;

  return Row{
      TintedIcon(app::images::sort, sort_icon_color, 13.0F),
      Text(sort_label)
          .Style(TextStyle{Font::System(font_size::kChip).WithWeight(FontWeight::Medium), sort_fg})
          .LineLimit(1),
      TintedIcon(app::images::chevron_down, sort_icon_color, 12.0F),
  }
      .With(Frame{.height = height, .min_height = height, .max_height = height},
            Padding{EdgeInsets::Symmetric(8.0F, 0.0F)}, Spacing{5.0F},
            CrossAlign{CrossAxisAlignment::Center}, CornerRadius{6.0F},
            Background{VisualFill{sort_bg}}, Border{sort_border, 1.0F}, anchor,
            Enabled{enabled})
      .OnClick([menu, anchor, loc, on_select = std::move(on_select), sort_cols, current_sort] {
        std::vector<MenuEntry> entries;
        if (current_sort.has_value()) {
          const bool is_desc = current_sort->direction == DataTableSortDirection::Descending;
          entries.emplace_back(
              MenuItem(loc.Get(is_desc ? app::strings::table_sort_toggle_asc
                                       : app::strings::table_sort_toggle_desc),
                       [on_select, col_id = current_sort->column_id, is_desc] {
                         on_select(DataTableSort{
                             .column_id = col_id,
                             .direction = is_desc ? DataTableSortDirection::Ascending
                                                  : DataTableSortDirection::Descending,
                         });
                       }));
          entries.emplace_back(MenuItem(loc.Get(app::strings::table_sort_clear),
                                        [on_select] { on_select(std::nullopt); }));
          entries.emplace_back(MenuSection{});
        }
        for (const ColumnMeta& col : sort_cols) {
          const bool active_col = current_sort.has_value() && current_sort->column_id == col.name;
          const bool active_desc =
              active_col && current_sort->direction == DataTableSortDirection::Descending;
          std::string item_label = col.name;
          if (active_col) {
            item_label += active_desc ? " ↓" : " ↑";
          }
          entries.emplace_back(
              MenuItem(std::move(item_label),
                       [on_select, col_name = col.name, active_col, active_desc] {
                         DataTableSortDirection next_dir =
                             (active_col && !active_desc) ? DataTableSortDirection::Descending
                                                          : DataTableSortDirection::Ascending;
                         on_select(DataTableSort{.column_id = col_name, .direction = next_dir});
                       })
                  .Checked(active_col));
        }
        menu.Show(anchor, std::move(entries),
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
      });
}

} // namespace dbpp
