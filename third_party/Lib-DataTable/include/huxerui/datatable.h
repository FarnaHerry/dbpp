#pragma once

/// @file
/// @brief A virtualized data table with application-controlled rows, sorting, selection, and edits for HuxerUI.
///
/// Include <huxerui/datatable.h> and link HuxerUI::DataTable through huxerui_use_library(). The table composes into
/// an ordinary view tree and realizes cells around the visible viewport. Applications supply the displayed row order
/// and decide how to apply requests from DataTableEvents.

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <huxerui/event.h>
#include <huxerui/modifier.h>
#include <huxerui/resource.h>
#include <huxerui/state.h>
#include <huxerui/text_input.h>
#include <huxerui/view.h>

namespace huxerui {

/// @brief Identifies one data cell across snapshots, including when rows or columns are reordered.
struct DataTableCellId {
  /// Stable ID from the displayed row source.
  std::string row_id;
  /// Stable ID of the column in DataTableOptions::columns.
  std::string column_id;

  /// @brief Compares the cell's row and column IDs.
  /// @return True when both IDs match.
  bool operator==(const DataTableCellId&) const = default;
};

/// @brief Indexed access to one row snapshot in display order.
///
/// Row IDs are unique within the snapshot and stable across reordering. A nonempty source requires both callbacks,
/// and index_of(id_at(index)) must resolve to the same index for every displayed row. Capture row data by value or
/// retain shared storage while the table uses the snapshot. Lookups run on the UI thread as needed.
struct DataTableRowSource {
  /// Total number of displayed rows, including rows outside the viewport.
  std::size_t count = 0;
  /// Returns the owning row ID at a zero-based index in [0, count).
  std::function<std::string(std::size_t)> id_at;
  /// Returns the current zero-based index of a row ID, or std::nullopt if absent. The input is borrowed for the call.
  std::function<std::optional<std::size_t>(std::string_view)> index_of;
};

/// @brief Initial width and resize bounds for a column, expressed in logical pixels.
///
/// All widths must be finite and satisfy 0 < min <= initial <= max. Set all three to the same value for a fixed
/// width. A resized width is retained by column ID and clamped to the current bounds.
struct DataTableColumnWidth {
  /// Width used before the table retains a resize for this column.
  float initial = 120.0F;
  /// Inclusive lower width bound.
  float min = 40.0F;
  /// Inclusive upper width bound.
  float max = 640.0F;
};

/// @brief Identity and display position supplied to a cell or editor factory.
struct DataTableCellContext {
  /// Stable row and column IDs of the cell.
  DataTableCellId id;
  /// Zero-based index in the current row snapshot. Use id to identify the cell across snapshots.
  std::size_t row_index = 0;
};

/// @brief The display declaration and accessible name of a data cell.
struct DataTableCellContent {
  /// Nonempty View rendered inside the cell's layout and clip.
  View view;
  /// Name published by the grid cell's semantics, resolved in the current resource locale.
  StringVariant label;
};

/// @brief State, focus target, and actions supplied to the editor for one cell.
///
/// Write back the complete TextEditingValue from TextField::OnChanged() to preserve selection and composition.
/// Call commit or cancel from the current editor on the UI thread. The table retains the draft when the editor
/// scrolls out of view and restores it when the same cell is reopened. Editing a different cell replaces that draft.
/// @code{.cpp}
/// View RenderEditor(const DataTableEditContext& edit) {
///   return TextField(edit.draft)
///       .Variant(TextFieldVariant::Standard)
///       .VerticalAlign(TextVerticalAlign::Center)
///       .OnChanged([draft = edit.draft](const TextEditingValue& next) { draft = next; })
///       .OnSubmitted(edit.commit)
///       .With(
///           Padding(EdgeInsets{.top = 4.0F, .right = 8.0F, .bottom = 4.0F, .left = 8.0F}),
///           Semantics{.label = "Name"},
///           edit.focus_target
///       );
/// }
/// @endcode
struct DataTableEditContext {
  /// Cell identity and position in the current row snapshot.
  DataTableCellContext cell;
  /// Table-owned draft text, selection, and composition for this edit session.
  State<TextEditingValue> draft;
  /// Bind this handle to one focusable editor control with View::With(). The table requests focus after it mounts.
  ViewHandle focus_target;
  /// Finishes the session, requests focus for the table, and emits one EditSubmitted event with the draft text.
  std::function<void()> commit;
  /// Discards the draft, finishes the session, and requests focus for the table.
  std::function<void()> cancel;
};

/// @brief Text editing callbacks for one column.
struct DataTableEditor {
  /// Returns valid UTF-8 text to initialize a new session for the cell.
  std::function<std::string(const DataTableCellContext&)> initial_text;
  /// Returns a nonempty editor View. Copy any context values needed by the returned View or its handlers.
  std::function<View(const DataTableEditContext&)> render;
};

/// @brief Header, width, and cell behavior for one column in display order.
struct DataTableColumn {
  /// Column ID. Supply a unique, stable value to preserve cell identity and resized width across declarations.
  std::string id;
  /// Displayed header text and accessible name of the header.
  StringVariant header;
  /// Initial width and the bounds used when the user resizes the column.
  DataTableColumnWidth width;
  /// Allows header activation to propose a sort through DataTableEvents::SortRequested.
  bool sortable = false;
  /// Returns the display View and accessible name. Measurement may invoke this factory repeatedly. Keep the
  /// factory declarative and copy any borrowed cell values needed by the returned View.
  std::function<DataTableCellContent(const DataTableCellContext&)> render_cell;
  /// Editor opened by double-clicking or double-tapping the cell, or by pressing Enter or F2 on the active cell.
  /// Use std::nullopt for a display-only column.
  std::optional<DataTableEditor> editor;
};

/// @brief Direction of the row ordering supplied by the application for a column.
enum class DataTableSortDirection {
  Ascending,
  Descending,
};

/// @brief One column and direction for the current or requested row order.
struct DataTableSort {
  /// Stable ID of the column used to order the rows.
  std::string column_id;
  /// Direction indicated by the header or proposed by a sort request.
  DataTableSortDirection direction = DataTableSortDirection::Ascending;

  /// @brief Compares the column ID and ordering direction.
  /// @return True when both values match.
  bool operator==(const DataTableSort&) const = default;
};

/// @brief A submitted text edit for the application to apply to its data.
struct DataTableEditRequest {
  /// Stable target cell ID for applying the edit to the row source.
  DataTableCellId cell_id;
  /// UTF-8 draft text at submission.
  std::string text;
};

/// @brief Typed events bound with View::On() on the returned table View.
///
/// Handlers run synchronously on the UI thread. Payload references are valid for the duration of the callback.
/// Copy a payload before retaining it. The application supplies an updated snapshot to apply a request.
struct DataTableEvents {
  /// Proposes ascending order when a sortable header is first activated, then toggles the current direction.
  struct SortRequested : Event<void(const DataTableSort&)> {};
  /// Proposes a selected cell after a pointer activation or Space on the active cell.
  struct SelectionRequested : Event<void(const DataTableCellId&)> {};
  /// Delivers the cell ID and text after the editor commits its draft.
  struct EditSubmitted : Event<void(const DataTableEditRequest&)> {};
};

/// @brief Display snapshot, dimensions, and controlled interaction state for one table declaration.
struct DataTableOptions {
  /// Row source in the order the application wants to display.
  DataTableRowSource rows;
  /// Columns in display order, each with a unique ID.
  std::vector<DataTableColumn> columns;
  /// Fixed height of each data row in logical pixels. Must be finite and positive.
  float row_height = 32.0F;
  /// Fixed height of the frozen header row in logical pixels. Must be finite and positive.
  float header_height = 36.0F;
  /// Number of leading columns to freeze, clamped to the number of columns.
  std::size_t frozen_column_count = 0;
  /// Sort indicator for the row order supplied by rows. Use std::nullopt to clear the indicator.
  std::optional<DataTableSort> sort;
  /// Selected data cell. Use std::nullopt to clear selection. Keyboard navigation tracks an independent active cell.
  std::optional<DataTableCellId> selected_cell;
};

/// @brief Composes a two-axis virtualized grid with a frozen header row and optional leading frozen columns.
///
/// Give the table finite width and height constraints, for example with Frame in an unbounded parent. The table
/// keeps the active cell, widths, and any draft in its composition lifetime. The application owns the displayed row
/// order, sort, selection, and committed data. Supply a new declaration to apply an event request.
///
/// Use the arrow keys to move the active cell and Space to request selection. Enter or F2 opens the editor for an
/// editable cell, and Escape cancels the edit after text composition ends.
/// @param options Display snapshot, columns, dimensions, and controlled state retained by the returned View.
/// @return A View that can be composed with ordinary HuxerUI views and bound to DataTableEvents.
/// @throws std::invalid_argument If required callbacks are missing, column IDs repeat, or width or height limits are
/// invalid. The table also validates layout extents and renderer results when composing or measuring cells.
/// @code{.cpp}
/// View SelectedTable(DataTableOptions options, State<std::optional<DataTableCellId>> selected) {
///   options.selected_cell = selected;
///   return DataTable(std::move(options))
///       .On<DataTableEvents::SelectionRequested>([selected](const DataTableCellId& cell) { selected = cell; })
///       .With(Frame{.width = 640.0F, .height = 360.0F});
/// }
/// @endcode
View DataTable(DataTableOptions options);

} // namespace huxerui
