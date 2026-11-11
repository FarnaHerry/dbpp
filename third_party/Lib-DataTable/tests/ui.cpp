#include <charconv>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string_view>

#include <huxerui/datatable.h>
#include <huxerui/huxerui.h>
#include <huxerui/testing/ui_test.h>

using namespace huxerui;
using namespace huxerui::testing;

namespace {

constexpr std::size_t row_count = 10'000;
constexpr std::size_t column_count = 100;
constexpr Size viewport{350.0F, 210.0F};
constexpr UiPointerOptions mouse{.device_kind = PointerDeviceKind::Mouse};

void Check(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

bool Near(float actual, float expected) {
  return std::abs(actual - expected) < 0.01F;
}

struct Model {
  std::vector<DataTableSort> sort_requests;
  std::vector<DataTableCellId> selection_requests;
  std::vector<DataTableEditRequest> edit_requests;
  std::size_t frozen_column_count = 1;
  bool accept_selection = true;
};

Model model;

std::string Label(const DataTableCellContext& cell, const std::string& name) {
  return cell.id == DataTableCellId{"1", "1"} ? name : cell.id.row_id + ":" + cell.id.column_id;
}

View TestTable() {
  const auto sort = UseState(DataTableSort{"0", DataTableSortDirection::Ascending});
  const auto selected = UseState(std::optional<DataTableCellId>{});
  const auto name_value = UseState(std::string("1:1"));
  const DataTableSort& sort_value = sort;
  const bool descending = sort_value.direction == DataTableSortDirection::Descending;
  DataTableOptions options{
      .rows = {
          .count = row_count,
          .id_at = [descending](std::size_t index) {
            return std::to_string(descending ? row_count - index : index + 1);
          },
          .index_of = [descending](std::string_view id) -> std::optional<std::size_t> {
            if (id.empty()) {
              return std::nullopt;
            }
            std::size_t row = 0;
            const auto [end, error] = std::from_chars(id.data(), id.data() + id.size(), row);
            if (error != std::errc{} || end != id.data() + id.size() || row == 0 || row > row_count) {
              return std::nullopt;
            }
            return descending ? row_count - row : row - 1;
          },
      },
      .row_height = 32.0F,
      .header_height = 30.0F,
      .frozen_column_count = model.frozen_column_count,
      .sort = sort_value,
      .selected_cell = selected,
  };

  const std::string name = name_value;
  for (std::size_t index = 0; index < column_count; ++index) {
    const auto id = std::to_string(index);
    DataTableColumn column{
        .id = id,
        .header = "Column " + id,
        .width = {.initial = 80.0F, .min = 60.0F, .max = 120.0F},
        .sortable = index == 0,
        .render_cell = [name](const DataTableCellContext& cell) -> DataTableCellContent {
          const auto label = Label(cell, name);
          return {Text(label), label};
        },
    };
    if (index == 1) {
      column.width = {.initial = 80.0F, .min = 80.0F, .max = 80.0F};
      column.editor = DataTableEditor{
          .initial_text = [name](const DataTableCellContext& cell) { return Label(cell, name); },
          .render = [](const DataTableEditContext& edit) {
            return TextField(edit.draft)
                .Label("Cell editor")
                .OnChanged([draft = edit.draft](const TextEditingValue& next) { draft = next; })
                .OnSubmitted(edit.commit)
                .With(edit.focus_target);
          },
      };
    }
    options.columns.push_back(std::move(column));
  }

  return DataTable(std::move(options))
      .On<DataTableEvents::SortRequested>([sort](const DataTableSort& next) {
        model.sort_requests.push_back(next);
        sort = next;
      })
      .On<DataTableEvents::SelectionRequested>([selected](const DataTableCellId& cell) {
        model.selection_requests.push_back(cell);
        if (model.accept_selection) {
          selected = cell;
        }
      })
      .On<DataTableEvents::EditSubmitted>([name_value](const DataTableEditRequest& edit) {
        model.edit_requests.push_back(edit);
        name_value = edit.text;
      });
}

const Application app{TestTable, {.show_debug_overlay = false}};

UiSemanticQuery Cell(UiTest& ui, std::string label) {
  return ui.FindSemantics(UiSemanticSelector::AllOf(
      UiSemanticSelector::Role(SemanticRole::GridCell), UiSemanticSelector::Label(std::move(label))));
}

void CheckHeaderAlignment(UiTest& ui, const char* column, const char* body_label) {
  const auto header = ui.Find(UiSelector::Key(std::string("header:") + column)).One().bounds;
  const auto body = Cell(ui, body_label).One().bounds;
  Check(Near(header.x, body.x) && Near(header.width, body.width),
        "The header and body must share the same horizontal cell bounds");
}

bool HasBorderSize(const std::string& snapshot, float width, float height) {
  const std::string expected_width = "width=" + std::to_string(width) + '\n';
  const std::string expected_height = "height=" + std::to_string(height) + '\n';
  for (auto border = snapshot.find("border {\n"); border != std::string::npos;
       border = snapshot.find("border {\n", border + 1)) {
    const auto bounds_end = snapshot.find('}', border);
    const auto bounds = std::string_view(snapshot).substr(border, bounds_end - border);
    if (bounds.find(expected_width) != std::string_view::npos &&
        bounds.find(expected_height) != std::string_view::npos) {
      return true;
    }
  }
  return false;
}

void CheckBoundedGrid(UiTest& ui) {
  const auto grid = ui.FindSemantics(UiSemanticSelector::Role(SemanticRole::Grid)).One();
  Check(grid.collection && grid.collection->row_count == row_count && grid.collection->column_count == column_count &&
            grid.collection->item_count == row_count * column_count,
        "Grid semantics must publish the logical dimensions");
  const auto mounted = ui.FindSemantics(UiSemanticSelector::Role(SemanticRole::GridCell)).Count();
  Check(mounted > 0 && mounted <= 64, "The number of mounted cells must stay bounded by the viewport");
}

void FrozenGeometryAndHitTesting() {
  model = Model{};
  model.frozen_column_count = 2;
  UiTest ui(app, {.viewport = viewport});
  ui.Pump();
  CheckBoundedGrid(ui);

  const auto consumed = ui.SendScroll({.position = {190.0F, 100.0F}, .delta_x = 30.0F, .delta_y = 12.0F});
  ui.Pump();
  Check(Near(consumed.x, 30.0F) && Near(consumed.y, 12.0F), "Both axes must move on diagonal input");
  const auto scrolling = Cell(ui, "1:2").One().bounds;
  Check(Near(scrolling.x, 160.0F) && Near(scrolling.y, 30.0F) && Near(scrolling.width, 50.0F) &&
            Near(scrolling.height, 20.0F),
        "The committed cell fragment must begin at both frozen edges");
  CheckHeaderAlignment(ui, "1", "1:1");
  CheckHeaderAlignment(ui, "2", "1:2");
  Check(HasBorderSize(ui.CaptureSnapshot().ToString(), 50.0F, 30.0F),
        "The partially clipped header must paint a border at its visible bounds");

  model.accept_selection = false;
  ui.TapAt({160.0F, 34.0F}, mouse);
  Check(!model.selection_requests.empty() && model.selection_requests.back() == DataTableCellId{"1", "1"},
        "The frozen column must win on the seam");
  Check(Cell(ui, "1:1").One().selected == false, "A selection request must remain controlled");
  model.accept_selection = true;
  ui.TapAt({162.0F, 34.0F});
  Check(model.selection_requests.back() == DataTableCellId{"1", "2"} && Cell(ui, "1:2").One().selected == true,
        "A touch just beyond the frozen edge must hit the visible scrolling cell");
  const auto count = model.selection_requests.size();
  ui.TapAt({162.0F, 28.0F}, mouse);
  Check(model.selection_requests.size() == count, "The header clip must block a body cell's hit target");

  ui.Drag({76.0F, 15.0F}, {134.0F, 15.0F}, {.segments = 4, .pointer = mouse});
  const auto resized = Cell(ui, "1:0").One().bounds;
  const auto after = Cell(ui, "1:2").One().bounds;
  Check(Near(resized.width, 120.0F) && Near(after.x, 200.0F) && Near(after.width, 50.0F),
        "Resize must clamp the width and move the frozen clip with its scroll anchor");
  CheckHeaderAlignment(ui, "0", "1:0");
  CheckHeaderAlignment(ui, "1", "1:1");
  CheckHeaderAlignment(ui, "2", "1:2");
  Check(HasBorderSize(ui.CaptureSnapshot().ToString(), 120.0F, 30.0F),
        "The resized header must paint a border across the new width");
  ui.TapAt({199.0F, 34.0F}, mouse);
  Check(model.selection_requests.back() == DataTableCellId{"1", "1"}, "Resize must move the frozen hit target");
  ui.TapAt({201.0F, 34.0F}, mouse);
  Check(model.selection_requests.back() == DataTableCellId{"1", "2"}, "Resize must move the scrolling hit target");
  Check(model.sort_requests.empty(), "Dragging the resize handle must leave sorting untouched");

  ui.SendScroll({.position = {190.0F, 100.0F}, .delta_x = 40.0F});
  ui.Pump();
  PointerEvent resize{
      .type = PointerEventType::Down,
      .pointer_id = 9,
      .position = {206.0F, 15.0F},
      .device_kind = PointerDeviceKind::Mouse,
      .changed_button = PointerButton::Primary,
      .pressed_buttons = PointerButton::Primary,
  };
  ui.SendPointer(resize);
  resize.type = PointerEventType::Move;
  resize.position.x = 181.0F;
  resize.changed_button = PointerButton::None;
  ui.SendPointer(resize);
  ui.Pump();
  const auto anchored = Cell(ui, "1:3").One().bounds;
  Check(Near(anchored.x, 200.0F) && Near(anchored.width, 80.0F),
        "Shrinking past the anchor must correct the scroll offset in the resize frame");
  CheckHeaderAlignment(ui, "3", "1:3");
  resize.type = PointerEventType::Up;
  resize.changed_button = PointerButton::Primary;
  resize.pressed_buttons = PointerButton::None;
  ui.SendPointer(resize);

  ui.SendScroll({.position = {190.0F, 100.0F}, .delta_x = 20'000.0F});
  ui.Pump();
  const auto at_right = ui.SendScroll({.position = {190.0F, 100.0F}, .delta_x = 40.0F, .delta_y = 30.0F});
  ui.Pump();
  Check(Near(at_right.x, 0.0F) && Near(at_right.y, 30.0F), "Vertical scrolling must continue at the right limit");
  CheckBoundedGrid(ui);
}

void KeyboardRevealAndSortIdentity() {
  model = Model{};
  UiTest ui(app, {.viewport = viewport});
  ui.Pump();
  ui.TapAt({110.0F, 43.0F}, mouse);
  Check(Cell(ui, "1:1").One().selected == true, "Click must select the first row by ID");
  ui.TapAt({30.0F, 15.0F}, mouse);
  Check(model.sort_requests.size() == 1 && model.sort_requests.front().direction == DataTableSortDirection::Descending,
        "Clicking the sorted header must request the next direction");

  const auto grid = ui.FindSemantics(UiSemanticSelector::Role(SemanticRole::Grid));
  if (!grid.One().focused) {
    ui.PressKey(Key::Tab, {.shift = true});
  }
  Check(grid.One().focused, "The grid must be focusable from the header");
  ui.PressKey(Key::ArrowDown);
  Check(Cell(ui, "1:1").One().selected == true, "Sorting must preserve the controlled selected ID");
  ui.PressKey(Key::ArrowRight);
  ui.PressKey(Key::Space);
  Check(Cell(ui, "1:2").One().selected == true, "The active row ID must survive a reorder");
  const auto reordered = Cell(ui, "1:2").One();
  Check(reordered.collection_item && reordered.collection_item->row_index == row_count - 1 && !reordered.offscreen,
        "A reordered active cell must be revealed at its new row position");

  ui.PressKey(Key::End, {.control = true});
  ui.PressKey(Key::Space);
  const auto last = Cell(ui, "1:99").One();
  Check(!last.offscreen && last.bounds.x >= 80.0F && last.bounds.y >= 30.0F &&
            last.bounds.x + last.bounds.width <= viewport.width &&
            last.bounds.y + last.bounds.height <= viewport.height,
        "A keyboard jump must reveal the far corner in the unobscured viewport");
  CheckBoundedGrid(ui);
}

void EditorDraftAndFocus() {
  model = Model{};
  UiTest ui(app, {.viewport = viewport});
  ui.Pump();
  const auto editor = ui.Find(UiSelector::Type<TextField>());

  ui.TapAt({110.0F, 43.0F}, mouse);
  ui.PressKey(Key::F2);
  Check(editor.One().focused && ui.ActiveTextInput(), "A mounted editor must take input focus");
  editor.ReplaceText("Draft");
  ui.SendScroll({.position = {240.0F, 130.0F}, .delta_y = 900.0F});
  ui.Pump();
  Check(!editor.Exists() && !ui.ActiveTextInput(), "Scrolling out must evict the editor and its input session");
  Check(model.edit_requests.empty(), "Editor eviction must preserve the draft without submitting it");

  ui.SendScroll({.position = {240.0F, 130.0F}, .delta_y = -900.0F});
  ui.Pump();
  Check(!editor.Exists(), "Scrolling back must leave the draft available for explicit reopening");
  ui.TapAt({110.0F, 43.0F});
  ui.TapAt({110.0F, 43.0F});
  Check(editor.One().focused && editor.One().value == "Draft", "A double tap must reopen the same cell's draft");
  editor.Submit();
  Check(model.edit_requests.size() == 1 && model.edit_requests.front().cell_id == DataTableCellId{"1", "1"} &&
            model.edit_requests.front().text == "Draft",
        "Submission must issue one edit request for the original cell ID");
  const auto grid = ui.FindSemantics(UiSemanticSelector::Role(SemanticRole::Grid));
  Check(grid.One().focused && !editor.Exists(), "Submitting must return focus to the table");

  ui.PressKey(Key::F2);
  editor.ReplaceText("Discarded");
  ui.PressKey(Key::Escape);
  Check(model.edit_requests.size() == 1 && Cell(ui, "Draft").Exists() && grid.One().focused,
        "Cancel must discard the draft and return focus to the table");
}

} // namespace

int main() {
  try {
    FrozenGeometryAndHitTesting();
    KeyboardRevealAndSortIdentity();
    EditorDraftAndFocus();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
