#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <huxerui/datatable.h>
#include <huxerui/virtual_layout.h>

namespace huxerui::detail {

struct TableRange {
  std::size_t begin = 0;
  std::size_t end = 0;
};

struct TableGeometry {
  TableGeometry(const DataTableOptions& options, const std::unordered_map<std::string, float>& widths);

  std::size_t ColumnCount() const noexcept;
  float ColumnWidth(std::size_t column) const noexcept;
  float FrozenWidth() const noexcept;
  Size ContentSize() const noexcept;
  std::size_t ItemCount() const noexcept;
  Rect CellBounds(std::size_t row_slot, std::size_t column) const noexcept;
  TableRange Rows(VirtualViewport viewport) const noexcept;
  TableRange ScrollingColumns(VirtualViewport viewport) const noexcept;
  TableRange FrozenColumns(float viewport_width) const noexcept;

  std::vector<float> edges;
  std::size_t row_count;
  std::size_t frozen_count;
  float row_height;
  float header_height;
};

struct TableResizeAnchor {
  std::string column_id;
  float within_column;
  bool committed = false;
};

struct TableLayoutData {
  using Value = TableLayoutData;

  std::shared_ptr<const TableGeometry> geometry;
  std::optional<std::size_t> editor_item;
  std::shared_ptr<const TableResizeAnchor> resize_anchor;
  float corrected_x = 0.0F;

  bool operator==(const TableLayoutData&) const = default;
};

class ClippedTableCell final : public Layout<ClippedTableCell> {
public:
  ClippedTableCell(View content, Size full_size, std::optional<float> display_inset = std::nullopt);

  static LayoutResult Measure(LayoutContext& context, ViewNode& node, Constraints constraints);
};

class DataTableLayout final : public VirtualLayout<DataTableLayout> {
public:
  DataTableLayout(TableLayoutData data, std::function<View(std::size_t)> factory);

  static std::optional<Rect> ItemBounds(ViewNode& node, std::size_t index);
  static VirtualLayoutResult Measure(VirtualLayoutContext& context, ViewNode& node, Constraints constraints);
};

} // namespace huxerui::detail
