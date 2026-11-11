#include <huxerui/datatable.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <huxerui/gesture.h>
#include <huxerui/lifecycle.h>
#include <huxerui/scroll.h>
#include <huxerui/semantics.h>
#include <huxerui/theme.h>

#include "table_layout.h"

namespace huxerui {
namespace {

using detail::ClippedTableCell;
using detail::DataTableLayout;
using detail::TableGeometry;
using detail::TableResizeAnchor;

struct CellPosition {
  std::size_t row;
  std::size_t column;
};

struct ColumnWidths {
  std::unordered_map<std::string, float> by_id;
  std::shared_ptr<TableResizeAnchor> anchor;

  bool operator==(const ColumnWidths&) const = default;
};

struct ColumnDrag {
  std::string column_id;
  std::int64_t pointer_id;
  float width;
  float window_x;

  bool operator==(const ColumnDrag&) const = default;
};

struct EditSession {
  DataTableCellId id;
  State<TextEditingValue> draft;
  bool suspended = false;
};

void ValidateOptions(const DataTableOptions& options) {
  if (!std::isfinite(options.row_height) || options.row_height <= 0.0F ||
      !std::isfinite(options.header_height) || options.header_height <= 0.0F) {
    throw std::invalid_argument("DataTable row and header heights must be finite and positive");
  }
  if (options.rows.count > 0 && (!options.rows.id_at || !options.rows.index_of)) {
    throw std::invalid_argument("DataTable requires both row lookup callbacks for a nonempty row source");
  }

  std::unordered_set<std::string_view> ids;
  for (const auto& column : options.columns) {
    if (!ids.insert(column.id).second) {
      throw std::invalid_argument("DataTable column IDs must be unique");
    }
    const auto& width = column.width;
    if (!std::isfinite(width.initial) || !std::isfinite(width.min) || !std::isfinite(width.max) ||
        width.min <= 0.0F || width.initial < width.min || width.initial > width.max) {
      throw std::invalid_argument("DataTable widths must be finite and satisfy 0 < min <= initial <= max");
    }
    if (!column.render_cell || (column.editor && (!column.editor->initial_text || !column.editor->render))) {
      throw std::invalid_argument("DataTable column renderer and editor callbacks must be present");
    }
  }
}

std::string CellKey(const DataTableCellId& id) {
  return "cell:" + std::to_string(id.row_id.size()) + ":" + id.row_id + id.column_id;
}

Color GridLineColor(const ColorScheme& colors) {
  Color line = colors.outline;
  line.alpha *= 0.25F;
  return line;
}

bool RevealAxis(const ScrollController& controller, float start, float extent, float frozen) {
  const auto metrics = controller.Metrics();
  const float visible_extent = metrics.viewport_extent - frozen;
  if (visible_extent <= 0.0F) {
    return false;
  }

  float target = metrics.offset;
  if (extent > visible_extent || start < target + frozen) {
    target = start - frozen;
  } else if (start + extent > target + metrics.viewport_extent) {
    target = start + extent - metrics.viewport_extent;
  }
  target = std::clamp(target, 0.0F, metrics.maximum_offset);
  return target == metrics.offset || controller.ScrollTo(target);
}

struct TableContext {
  std::shared_ptr<const DataTableOptions> options;
  std::shared_ptr<const TableGeometry> geometry;
  ScrollController horizontal;
  ScrollController vertical;
  State<ColumnWidths> widths;
  State<std::optional<ColumnDrag>> drag;
  State<std::optional<DataTableCellId>> active;
  State<std::shared_ptr<EditSession>> editing;
  ViewHandle focus;
  EventEmitter events;
  ColorScheme colors;

  std::optional<std::size_t> FindColumn(std::string_view id) const;
  std::optional<CellPosition> FindCell(const DataTableCellId& id) const;
  DataTableCellContext CellAt(CellPosition position) const;
  CellPosition CurrentCell() const;
  bool Reveal(CellPosition position) const;
  void Select(const DataTableCellId& id) const;
  void Sort(const DataTableColumn& column) const;
  void Resize(std::size_t column_index, float width) const;
  void StartEditing(CellPosition position) const;
  void FinishEditing(const std::shared_ptr<EditSession>& session, bool submit) const;
  bool Navigate(const KeyEvent& event) const;
  bool CancelOnEscape(const KeyEvent& event) const;
};

std::optional<std::size_t> TableContext::FindColumn(std::string_view id) const {
  const auto found = std::ranges::find(options->columns, id, &DataTableColumn::id);
  if (found == options->columns.end()) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(found - options->columns.begin());
}

std::optional<CellPosition> TableContext::FindCell(const DataTableCellId& id) const {
  if (options->rows.count == 0) {
    return std::nullopt;
  }
  const auto column = FindColumn(id.column_id);
  const auto row = options->rows.index_of(id.row_id);
  if (!column || !row || *row >= options->rows.count || options->rows.id_at(*row) != id.row_id) {
    return std::nullopt;
  }
  return CellPosition{*row, *column};
}

DataTableCellContext TableContext::CellAt(CellPosition position) const {
  return {{options->rows.id_at(position.row), options->columns[position.column].id}, position.row};
}

CellPosition TableContext::CurrentCell() const {
  if (const std::optional<DataTableCellId>& id = active; id) {
    if (const auto position = FindCell(*id)) {
      return *position;
    }
  }
  if (options->selected_cell) {
    if (const auto position = FindCell(*options->selected_cell)) {
      return *position;
    }
  }
  return {0, 0};
}

bool TableContext::Reveal(CellPosition position) const {
  const Rect cell = geometry->CellBounds(position.row + 1, position.column);
  const bool visible_x = position.column < geometry->frozen_count
                             ? cell.x < horizontal.ViewportExtent()
                             : RevealAxis(horizontal, cell.x, cell.width, geometry->FrozenWidth());
  const bool visible_y = RevealAxis(vertical, cell.y, cell.height, geometry->header_height);
  return visible_x && visible_y;
}

void TableContext::Select(const DataTableCellId& id) const {
  const std::shared_ptr<EditSession> session = editing;
  if (session && !session->suspended && session->id != id) {
    editing = std::make_shared<EditSession>(EditSession{session->id, session->draft, true});
  }
  active = id;
  static_cast<void>(focus.RequestFocus());
  events.Emit<DataTableEvents::SelectionRequested>(id);
}

void TableContext::Sort(const DataTableColumn& column) const {
  const bool ascending = options->sort && options->sort->column_id == column.id &&
                         options->sort->direction == DataTableSortDirection::Ascending;
  events.Emit<DataTableEvents::SortRequested>(DataTableSort{
      column.id, ascending ? DataTableSortDirection::Descending : DataTableSortDirection::Ascending});
}

void TableContext::Resize(std::size_t column_index, float width) const {
  const auto& column = options->columns[column_index];
  width = std::clamp(width, column.width.min, column.width.max);
  const ColumnWidths& current = widths;
  const auto stored = current.by_id.find(column.id);
  if (width == (stored == current.by_id.end() ? column.width.initial : stored->second)) {
    return;
  }

  ColumnWidths next = current;
  if (width == column.width.initial) {
    next.by_id.erase(column.id);
  } else {
    next.by_id.insert_or_assign(column.id, width);
  }
  next.anchor.reset();

  const auto metrics = horizontal.Metrics();
  const auto scrolling = geometry->ScrollingColumns({{metrics.offset, 0.0F}, {metrics.viewport_extent, 0.0F}});
  if (scrolling.begin < scrolling.end) {
    const auto first = scrolling.begin;
    const float within = metrics.offset + geometry->FrozenWidth() - geometry->edges[first];
    next.anchor = std::make_shared<TableResizeAnchor>(TableResizeAnchor{options->columns[first].id, within});
  }
  widths = std::move(next);
}

void TableContext::StartEditing(CellPosition position) const {
  const auto& editor = options->columns[position.column].editor;
  if (!editor || !Reveal(position)) {
    return;
  }

  auto cell = CellAt(position);
  const std::shared_ptr<EditSession> previous = editing;
  auto draft = previous && previous->id == cell.id ? previous->draft
      : State<TextEditingValue>(TextEditingValue::FromText(editor->initial_text(cell)));
  active = cell.id;
  editing = std::make_shared<EditSession>(EditSession{std::move(cell.id), std::move(draft)});
}

void TableContext::FinishEditing(const std::shared_ptr<EditSession>& session, bool submit) const {
  const std::shared_ptr<EditSession>& current = editing;
  if (current != session || session->suspended) {
    return;
  }
  editing = std::shared_ptr<EditSession>{};
  static_cast<void>(focus.RequestFocus());
  if (submit) {
    events.Emit<DataTableEvents::EditSubmitted>(DataTableEditRequest{session->id, session->draft->text});
  }
}

bool TableContext::Navigate(const KeyEvent& event) const {
  if (geometry->row_count == 0 || geometry->ColumnCount() == 0 || event.modifiers.alt) {
    return false;
  }
  auto position = CurrentCell();
  const std::size_t last_row = geometry->row_count - 1;
  const std::size_t last_column = geometry->ColumnCount() - 1;
  switch (event.key) {
  case Key::ArrowLeft:
    position.column -= position.column > 0;
    break;
  case Key::ArrowRight:
    position.column += position.column < last_column;
    break;
  case Key::ArrowUp:
    position.row -= position.row > 0;
    break;
  case Key::ArrowDown:
    position.row += position.row < last_row;
    break;
  case Key::Home:
    position.column = 0;
    if (event.modifiers.control || event.modifiers.meta) {
      position.row = 0;
    }
    break;
  case Key::End:
    position.column = last_column;
    if (event.modifiers.control || event.modifiers.meta) {
      position.row = last_row;
    }
    break;
  case Key::PageUp:
  case Key::PageDown: {
    const double height = vertical.ViewportExtent() - geometry->header_height;
    const double visible = std::max(1.0, std::floor(height / geometry->row_height));
    const std::size_t page = visible >= static_cast<double>(geometry->row_count)
                                 ? geometry->row_count : static_cast<std::size_t>(visible);
    if (event.key == Key::PageUp) {
      position.row -= std::min(position.row, page);
    } else {
      position.row += std::min(last_row - position.row, page);
    }
    break;
  }
  case Key::Space:
    if (!event.repeat) {
      Select(CellAt(position).id);
      static_cast<void>(Reveal(position));
    }
    return true;
  case Key::Enter:
  case Key::F2:
    if (!event.repeat) {
      StartEditing(position);
    }
    return options->columns[position.column].editor.has_value();
  default:
    return false;
  }

  active = CellAt(position).id;
  static_cast<void>(Reveal(position));
  return true;
}

bool TableContext::CancelOnEscape(const KeyEvent& event) const {
  if (event.type != KeyEventType::Down || event.key != Key::Escape || event.repeat) {
    return false;
  }
  const std::shared_ptr<EditSession> session = editing;
  if (!session || session->suspended || session->draft->composition) {
    return false;
  }
  FinishEditing(session, false);
  return true;
}

View ResizeHandle(std::shared_ptr<const TableContext> table, std::size_t index) {
  const auto id = table->options->columns[index].id;
  const float width = table->geometry->ColumnWidth(index);
  const auto update = [table, id, index](const DragEvent& event) {
    const std::optional<ColumnDrag>& drag = table->drag;
    if (drag && drag->pointer_id == event.pointer_id && drag->column_id == id) {
      table->Resize(index, drag->width + event.window_position.x - drag->window_x);
    }
  };
  const auto end = [table, id](const DragEvent& event) {
    const std::optional<ColumnDrag>& drag = table->drag;
    if (drag && drag->pointer_id == event.pointer_id && drag->column_id == id) {
      table->drag = std::nullopt;
    }
  };
  return Spacer().With(
      Grow(0.0F),
      Frame{.width = std::min(10.0F, width * 0.25F)},
      PointerCursor(PointerCursorKind::ResizeHorizontal),
      DragGesture{.axis = Axis::Horizontal, .minimum_distance = 1.0F}
  )
      .On<DragEvents::Started>([table, id, width](const DragEvent& event) {
        table->drag = ColumnDrag{id, event.pointer_id, width, event.window_position.x - event.translation.x};
      })
      .On<DragEvents::Changed>(update)
      .On<DragEvents::Ended>([update, end](const DragEvent& event) {
        update(event);
        end(event);
      })
      .On<DragEvents::Canceled>(end);
}

View HeaderCell(std::shared_ptr<const TableContext> table, std::size_t index) {
  const auto& column = table->options->columns[index];
  const auto& sort = table->options->sort;
  const bool sorted = sort && sort->column_id == column.id;
  const bool ascending = sorted && sort->direction == DataTableSortDirection::Ascending;
  const auto* arrow = reinterpret_cast<const char*>(ascending ? u8"\u25B2" : u8"\u25BC");
  const auto sort_description = sorted
      ? std::optional<StringVariant>(ascending ? "Sorted ascending" : "Sorted descending") : std::nullopt;

  View title = Row {
    Text(column.header, TextRole::Label).With(Grow(1.0F)),
    sorted ? Text(arrow, TextRole::Label).With(FontSize(9.0F)) : View{},
  }.With(
      Grow(1.0F),
      Padding(EdgeInsets{.right = 6.0F, .left = 8.0F}),
      CrossAlign(CrossAxisAlignment::Center),
      Foreground(table->colors.on_surface),
      Semantics{
          .role = column.sortable ? SemanticRole::Button : SemanticRole::Heading,
          .label = column.header,
          .state_description = sort_description,
          .descendants = SemanticDescendantPolicy::Exclude,
      }
  );
  if (column.sortable) {
    title = std::move(title).With(Focusable{}).OnClick([table, index] { table->Sort(table->options->columns[index]); });
  }

  const auto full = table->geometry->CellBounds(0, index);
  return ClippedTableCell(
      Row {
        title,
        column.width.min < column.width.max ? ResizeHandle(table, index) : View{},
      }.With(CrossAlign(CrossAxisAlignment::Stretch)),
      {full.width, full.height}
  ).With(
      ClipChildren{},
      Background(table->colors.surface_container_highest),
      Border{.color = GridLineColor(table->colors)}
  ).Key("header:" + column.id);
}

[[huxerui::composable]] View CellEditor(std::shared_ptr<const TableContext> table,
                                      std::shared_ptr<EditSession> session, CellPosition position) {
  const auto focus = UseViewHandle();
  const auto cell = table->CellAt(position);
  const auto& editor = *table->options->columns[position.column].editor;
  Lifecycle([table, session, focus] {
    const std::shared_ptr<EditSession>& current = table->editing;
    if (current == session && !session->suspended) {
      static_cast<void>(focus.RequestFocus());
    }
    return [session] { session->suspended = true; };
  }, session);

  View view = editor.render(DataTableEditContext{
      cell, session->draft, focus,
      [table, session] { table->FinishEditing(session, true); },
      [table, session] { table->FinishEditing(session, false); },
  });
  if (!view) {
    throw std::invalid_argument("DataTable editor renderer must return a View");
  }
  return view;
}

View BodyCell(std::shared_ptr<const TableContext> table, CellPosition position,
              const std::optional<DataTableCellId>& active, const std::shared_ptr<EditSession>& session) {
  const auto cell = table->CellAt(position);
  const auto& column = table->options->columns[position.column];
  const bool editing = session && !session->suspended && session->id == cell.id;
  const bool selected = table->options->selected_cell == cell.id;
  const bool current = active == cell.id;
  auto display = column.render_cell(cell);
  if (!display.view) {
    throw std::invalid_argument("DataTable cell renderer must return a View");
  }

  const auto full = table->geometry->CellBounds(position.row + 1, position.column);
  const auto& colors = table->colors;
  const Color background = position.column < table->geometry->frozen_count ? colors.surface_container_low
                                                                         : colors.surface;
  View content = editing ? CellEditor(table, session, position) : std::move(display.view);
  View cell_view = ClippedTableCell(
      std::move(content), {full.width, full.height}, editing ? std::nullopt : std::optional{8.0F}
  ).With(
      ClipChildren{},
      Semantics{.label = std::move(display.label), .selected = selected},
      Background(selected ? colors.primary_container : background),
      Border{.color = current ? colors.primary : GridLineColor(colors), .width = current ? 2.0F : 1.0F}
  )
      .Key(CellKey(cell.id));
  if (editing) {
    return cell_view;
  }

  cell_view = std::move(cell_view)
      .With(Foreground(selected ? colors.on_primary_container : colors.on_surface))
      .OnClick([table, id = cell.id] { table->Select(id); });
  if (column.editor) {
    cell_view = std::move(cell_view).With(MultiTapGesture{})
        .On<MultiTapEvents::Recognized>([table, position](const MultiTapEvent&) { table->StartEditing(position); });
  }
  return cell_view;
}

[[huxerui::composable]] View BuildDataTable(std::shared_ptr<const DataTableOptions> options) {
  const auto horizontal = UseScrollController();
  const auto vertical = UseScrollController();
  const auto widths = UseState(ColumnWidths{});
  const auto drag = UseState(std::optional<ColumnDrag>{});
  const auto active = UseState(std::optional<DataTableCellId>{});
  const auto editing = UseState(std::shared_ptr<EditSession>{});
  const auto focus = UseViewHandle();
  const auto events = UseEvents();
  const auto geometry = std::make_shared<const TableGeometry>(*options, widths->by_id);
  const auto table = std::make_shared<const TableContext>(TableContext{
      options, geometry, horizontal, vertical, widths, drag, active, editing, focus, events, UseTheme().colors,
  });

  const std::optional<DataTableCellId> active_id = active;
  const auto removed_active = active_id && !table->FindCell(*active_id) ? active_id : std::nullopt;
  const std::shared_ptr<EditSession> session = editing;
  const auto edit_position = session ? table->FindCell(session->id) : std::nullopt;
  const auto removed_editor = session && (!edit_position || !options->columns[edit_position->column].editor)
                                  ? session : nullptr;
  Lifecycle([active, editing, removed_active, removed_editor] {
    const std::optional<DataTableCellId>& current_active = active;
    if (removed_active && current_active == removed_active) {
      active = std::nullopt;
    }
    const std::shared_ptr<EditSession>& current_editor = editing;
    if (removed_editor && current_editor == removed_editor) {
      editing = std::shared_ptr<EditSession>{};
    }
  }, removed_active, removed_editor);

  detail::TableLayoutData layout{.geometry = geometry};
  const auto open_editor = removed_editor ? nullptr : session;
  if (open_editor && !open_editor->suspended) {
    layout.editor_item = (edit_position->row + 1) * geometry->ColumnCount() + edit_position->column;
  }
  const auto anchor = widths->anchor;
  if (anchor && !anchor->committed) {
    const auto column = table->FindColumn(anchor->column_id);
    if (column && *column >= geometry->frozen_count) {
      const float within = std::clamp(anchor->within_column, 0.0F, geometry->ColumnWidth(*column));
      layout.resize_anchor = anchor;
      layout.corrected_x = geometry->edges[*column] + within - geometry->FrozenWidth();
    }
  }
  Lifecycle([anchor] {
    if (anchor) {
      anchor->committed = true;
    }
  }, anchor);

  return DataTableLayout(std::move(layout), [table, active_id, open_editor](std::size_t index) {
    const auto columns = table->geometry->ColumnCount();
    const auto column = index % columns;
    if (index < columns) {
      return HeaderCell(table, column);
    }
    return BodyCell(table, {index / columns - 1, column}, active_id, open_editor);
  })
      .Controller(Axis::Horizontal, horizontal)
      .Controller(Axis::Vertical, vertical)
      .With(
          Focusable{}, focus, Background(table->colors.surface),
          ScrollPhysics{.overscroll_enabled = false}, ScrollBar()
      )
      .On<ViewEvents::FocusChanged>([table](bool focused) {
        if (focused && !table->active->has_value() && table->geometry->row_count > 0 &&
            table->geometry->ColumnCount() > 0) {
          table->active = table->CellAt(table->CurrentCell()).id;
        }
      })
      .On<ViewEvents::KeyDown>([table](const KeyEvent& event) { return table->Navigate(event); })
      .On<ViewEvents::KeyIntercept>([table](const KeyEvent& event) { return table->CancelOnEscape(event); });
}

} // namespace

View DataTable(DataTableOptions options) {
  ValidateOptions(options);
  return BuildDataTable(std::make_shared<const DataTableOptions>(std::move(options)));
}

} // namespace huxerui
