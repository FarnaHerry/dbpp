#include <charconv>
#include <memory>
#include <string>
#include <unordered_map>

#include <huxerui/datatable.h>
#include <huxerui/huxerui.h>

using namespace huxerui;

namespace {

constexpr std::size_t row_count = 10'000;
constexpr std::size_t column_count = 100;
using Names = std::unordered_map<std::string, std::string>;

DataTableRowSource Rows(bool descending) {
  return {
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
  };
}

std::string Name(const Names& names, const DataTableCellContext& cell) {
  const auto found = names.find(cell.id.row_id);
  return found == names.end() ? "Record " + cell.id.row_id : found->second;
}

std::vector<DataTableColumn> Columns(std::shared_ptr<const Names> names) {
  std::vector<DataTableColumn> columns;
  columns.reserve(column_count);
  columns.push_back({
      .id = "id",
      .header = "ID",
      .width = {.initial = 76.0F, .min = 56.0F, .max = 160.0F},
      .sortable = true,
      .render_cell = [](const DataTableCellContext& cell) -> DataTableCellContent {
        return {Text(cell.id.row_id), cell.id.row_id};
      },
  });
  columns.push_back({
      .id = "name",
      .header = "Name (editable)",
      .width = {.initial = 136.0F, .min = 88.0F, .max = 280.0F},
      .render_cell = [names](const DataTableCellContext& cell) -> DataTableCellContent {
        const std::string label = Name(*names, cell);
        return {Text(label), label};
      },
      .editor = DataTableEditor{
          .initial_text = [names](const DataTableCellContext& cell) { return Name(*names, cell); },
          .render = [](const DataTableEditContext& edit) {
            return TextField(edit.draft)
                .Variant(TextFieldVariant::Standard)
                .VerticalAlign(TextVerticalAlign::Center)
                .InputConfiguration({.action = TextInputAction::Done})
                .OnChanged([draft = edit.draft](const TextEditingValue& next) { draft = next; })
                .OnSubmitted(edit.commit)
                .With(
                    Padding(EdgeInsets{.top = 4.0F, .right = 8.0F, .bottom = 4.0F, .left = 8.0F}),
                    Semantics{.label = "Name"},
                    edit.focus_target
                );
          },
      },
  });

  for (std::size_t index = 2; index < column_count; ++index) {
    const auto number = std::to_string(index - 1);
    columns.push_back({
        .id = "field-" + number,
        .header = "Field " + number,
        .render_cell = [number](const DataTableCellContext& cell) -> DataTableCellContent {
          const std::string label = cell.id.row_id + " / " + number;
          return {Text(label), label};
        },
    });
  }
  return columns;
}

[[huxerui::composable]] View Preview() {
  const auto sort = UseState(DataTableSort{"id", DataTableSortDirection::Ascending});
  const auto selected = UseState(std::optional<DataTableCellId>{});
  const auto names = UseState(Names{});
  const DataTableSort& sort_value = sort;
  const Names& name_values = names;

  return Column {
    Row {
      Text("Data table", TextRole::Title).With(Grow(1.0F)),
      Text("10,000 rows, 100 columns", TextRole::Label),
    }.With(CrossAlign(CrossAxisAlignment::Center)),
    Text("Double-click or double-tap a Name cell, or select it and press F2. Enter or Done saves. Esc cancels."),
    DataTable({
        .rows = Rows(sort_value.direction == DataTableSortDirection::Descending),
        .columns = Columns(std::make_shared<const Names>(name_values)),
        .row_height = 36.0F,
        .frozen_column_count = 2,
        .sort = sort_value,
        .selected_cell = selected,
    })
        .On<DataTableEvents::SortRequested>([sort](const DataTableSort& next) { sort = next; })
        .On<DataTableEvents::SelectionRequested>([selected](const DataTableCellId& cell) { selected = cell; })
        .On<DataTableEvents::EditSubmitted>([names](const DataTableEditRequest& edit) {
          names.Update([&edit](Names& current) { current.insert_or_assign(edit.cell_id.row_id, edit.text); });
        })
        .With(Grow(1.0F), Border{.color = UseTheme().colors.surface_container_highest}),
  }.With(Padding(16.0F), Spacing(10.0F), CrossAlign(CrossAxisAlignment::Stretch));
}

View App() {
  return MaterialTheme {
    Preview(),
  };
}

} // namespace

const Application application{
    App,
    {
        .window = {
            .title = "DataTable Preview",
            .initial_size = {900.0F, 560.0F},
        },
    }
};
