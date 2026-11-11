#include "table_layout.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace huxerui::detail {
namespace {

struct CellLayout {
  using Value = CellLayout;

  Size full_size;
  std::optional<float> display_inset;

  bool operator==(const CellLayout&) const = default;
};

Constraints ExactSize(Size size) noexcept {
  return {size.width, size.width, size.height, size.height};
}

TableRange IntersectingColumns(const std::vector<float>& edges, float start, float end, TableRange allowed) {
  if (start >= end || allowed.begin == allowed.end) {
    return {};
  }

  const auto first = std::upper_bound(edges.begin(), edges.end(), start) - 1;
  const auto last = std::lower_bound(edges.begin(), edges.end(), end);
  return {
      std::clamp(static_cast<std::size_t>(first - edges.begin()), allowed.begin, allowed.end),
      std::clamp(static_cast<std::size_t>(last - edges.begin()), allowed.begin, allowed.end),
  };
}

std::size_t ClampRow(double row, std::size_t count) noexcept {
  return row >= static_cast<double>(count) ? count : static_cast<std::size_t>(row);
}

} // namespace

TableGeometry::TableGeometry(const DataTableOptions& options, const std::unordered_map<std::string, float>& widths)
    : row_count(options.rows.count), frozen_count(std::min(options.frozen_column_count, options.columns.size())),
      row_height(options.row_height), header_height(options.header_height) {
  const std::size_t columns = options.columns.size();
  if (columns > 0 && row_count >= std::numeric_limits<std::size_t>::max() / columns) {
    throw std::invalid_argument("DataTable has too many logical items");
  }

  edges.reserve(columns + 1);
  edges.push_back(0.0F);
  for (const auto& column : options.columns) {
    const auto stored = widths.find(column.id);
    const float width = stored == widths.end() ? column.width.initial
                                               : std::clamp(stored->second, column.width.min, column.width.max);
    const float edge = edges.back() + width;
    if (!std::isfinite(edge) || edge <= edges.back()) {
      throw std::invalid_argument("DataTable column extents must be finite and increasing");
    }
    edges.push_back(edge);
  }

  const double height = static_cast<double>(row_count) * row_height + header_height;
  if (columns > 0 && height > std::numeric_limits<float>::max()) {
    throw std::invalid_argument("DataTable row extent must be finite");
  }
}

std::size_t TableGeometry::ColumnCount() const noexcept {
  return edges.size() - 1;
}

float TableGeometry::ColumnWidth(std::size_t column) const noexcept {
  return edges[column + 1] - edges[column];
}

float TableGeometry::FrozenWidth() const noexcept {
  return edges[frozen_count];
}

Size TableGeometry::ContentSize() const noexcept {
  const double height = static_cast<double>(row_count) * row_height + header_height;
  return {edges.back(), ColumnCount() == 0 ? 0.0F : static_cast<float>(height)};
}

std::size_t TableGeometry::ItemCount() const noexcept {
  return ColumnCount() == 0 ? 0 : (row_count + 1) * ColumnCount();
}

Rect TableGeometry::CellBounds(std::size_t row_slot, std::size_t column) const noexcept {
  const float top = row_slot == 0 ? 0.0F : static_cast<float>(header_height + (row_slot - 1) * double{row_height});
  return {edges[column], top, ColumnWidth(column), row_slot == 0 ? header_height : row_height};
}

TableRange TableGeometry::Rows(VirtualViewport viewport) const noexcept {
  const double height = viewport.size.height - header_height;
  if (height <= 0.0 || row_count == 0) {
    return {};
  }

  const double offset = viewport.offset.y;
  const auto first = ClampRow(std::floor(offset / row_height), row_count);
  const auto last = ClampRow(std::ceil((offset + height) / row_height), row_count);
  return {first, last + (first < last && last < row_count ? 1 : 0)};
}

TableRange TableGeometry::ScrollingColumns(VirtualViewport viewport) const noexcept {
  auto range = IntersectingColumns(edges, viewport.offset.x + FrozenWidth(), viewport.offset.x + viewport.size.width,
                                  {frozen_count, ColumnCount()});
  if (range.begin < range.end && range.end < ColumnCount()) {
    ++range.end;
  }
  return range;
}

TableRange TableGeometry::FrozenColumns(float viewport_width) const noexcept {
  return IntersectingColumns(edges, 0.0F, std::min(viewport_width, FrozenWidth()), {0, frozen_count});
}

ClippedTableCell::ClippedTableCell(View content, Size full_size, std::optional<float> display_inset)
    : Layout(std::move(content)) {
  SetLayoutValue(typeid(CellLayout), CellLayout{full_size, display_inset});
}

LayoutResult ClippedTableCell::Measure(LayoutContext& context, ViewNode& node, Constraints constraints) {
  const auto& layout = *node.LayoutValue<CellLayout>();
  const Size full = layout.full_size;
  const Size fragment = constraints.Constrain(full);
  // Only the leading edges are cut here. The virtual layout clips the far edges of the viewport.
  Point origin{fragment.width - full.width, fragment.height - full.height};
  auto& child = node.ChildAt(0);
  if (layout.display_inset) {
    const float inset = *layout.display_inset;
    const float width = std::max(0.0F, full.width - 2.0F * inset);
    const Size measured = context.Measure(child, {width, width, 0.0F, full.height});
    origin.x += inset;
    origin.y += (full.height - measured.height) * 0.5F;
  } else {
    context.Measure(child, ExactSize(full));
  }
  LayoutResult result;
  return result.SetSize(fragment).Place(child, origin);
}

DataTableLayout::DataTableLayout(TableLayoutData data, std::function<View(std::size_t)> factory)
    : VirtualLayout(data.geometry->ItemCount(), std::move(factory)) {
  EnableBothAxes();
  SetLayoutValue(typeid(TableLayoutData), std::move(data));
}

std::optional<Rect> DataTableLayout::ItemBounds(ViewNode& node, std::size_t index) {
  const auto& geometry = *node.LayoutValue<TableLayoutData>()->geometry;
  if (index >= geometry.ItemCount()) {
    return std::nullopt;
  }
  return geometry.CellBounds(index / geometry.ColumnCount(), index % geometry.ColumnCount());
}

VirtualLayoutResult DataTableLayout::Measure(VirtualLayoutContext& context, ViewNode& node, Constraints) {
  const auto& data = *node.LayoutValue<TableLayoutData>();
  const auto& geometry = *data.geometry;
  const auto viewport = context.Viewport();
  const auto content = geometry.ContentSize();
  const auto columns = geometry.ColumnCount();
  VirtualLayoutResult result;
  result.SetSize(viewport.size)
      .SetContentSize(content)
      .SetCollectionSemantics(SemanticRole::Grid, SemanticRole::GridCell,
                              {.item_count = geometry.row_count * columns, .row_count = geometry.row_count,
                               .column_count = columns});
  if (columns == 0 || viewport.size.width <= 0.0F || viewport.size.height <= 0.0F) {
    return result;
  }

  if (data.resize_anchor && !data.resize_anchor->committed && geometry.FrozenWidth() < viewport.size.width) {
    const float target = std::clamp(data.corrected_x, 0.0F, std::max(0.0F, content.width - viewport.size.width));
    result.SetScrollOffset(Axis::Horizontal, target);
    if (target != viewport.offset.x) {
      return result;
    }
  }

  const auto place_region = [&](TableRange slots, TableRange column_range) {
    for (std::size_t slot = slots.begin; slot < slots.end; ++slot) {
      for (std::size_t column = column_range.begin; column < column_range.end; ++column) {
        Rect bounds = geometry.CellBounds(slot, column);
        if (column < geometry.frozen_count) {
          bounds.x += viewport.offset.x;
        } else {
          const float cut = std::max(0.0F, viewport.offset.x + geometry.FrozenWidth() - bounds.x);
          bounds.x += cut;
          bounds.width -= cut;
        }
        if (slot == 0) {
          bounds.y += viewport.offset.y;
        } else {
          const float cut = std::max(0.0F, viewport.offset.y + geometry.header_height - bounds.y);
          bounds.y += cut;
          bounds.height -= cut;
        }

        const std::size_t item_index = slot * columns + column;
        if (bounds.IsEmpty() || (data.editor_item == item_index && !bounds.Intersects(viewport.Bounds()))) {
          continue;
        }
        auto& item = context.Item(item_index);
        context.Measure(item, ExactSize({bounds.width, bounds.height}));
        if (slot == 0) {
          result.Place(item, {bounds.x, bounds.y});
        } else {
          result.Place(item, {bounds.x, bounds.y},
                       {.index = (slot - 1) * columns + column, .row_index = slot - 1, .column_index = column});
        }
      }
    }
  };

  const auto rows = geometry.Rows(viewport);
  const auto frozen = geometry.FrozenColumns(viewport.size.width);
  const auto scrolling = geometry.ScrollingColumns(viewport);
  const TableRange body_slots{rows.begin + 1, rows.end + 1};
  place_region(body_slots, scrolling);
  place_region(body_slots, frozen);
  place_region({0, 1}, scrolling);
  place_region({0, 1}, frozen);
  return result;
}

} // namespace huxerui::detail
