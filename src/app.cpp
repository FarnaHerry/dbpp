#include "model.h"
#include "theme.h"
#include "adbc_session.h"

#include <huxerui/datatable.h>
#include <huxerui/huxerui.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dbpp {

using namespace huxerui;
using namespace dbpp::ui;

namespace {

std::size_t RowIndexFromId(std::string_view id) {
  std::size_t index = 0;
  const auto [end, error] = std::from_chars(id.data(), id.data() + id.size(), index);
  if (error != std::errc{} || end != id.data() + id.size()) {
    return static_cast<std::size_t>(-1);
  }
  return index;
}

const GridRow* RowAt(const std::shared_ptr<const GridSnapshot>& snapshot, std::size_t index) {
  if (!snapshot || index >= snapshot->rows.size()) {
    return nullptr;
  }
  return &snapshot->rows[index];
}

std::string CellText(const std::shared_ptr<const GridSnapshot>& snapshot, std::string_view row_id,
                     std::size_t column) {
  const GridRow* row = RowAt(snapshot, RowIndexFromId(row_id));
  if (row == nullptr || column >= row->values.size()) {
    return {};
  }
  return row->values[column].text;
}

bool CellIsNull(const std::shared_ptr<const GridSnapshot>& snapshot, std::string_view row_id,
                std::size_t column) {
  const GridRow* row = RowAt(snapshot, RowIndexFromId(row_id));
  return row != nullptr && column < row->values.size() &&
         row->values[column].is_null;
}

DataTableRowSource MakeRows(const std::shared_ptr<const GridSnapshot>& snapshot) {
  const std::size_t count = snapshot ? snapshot->rows.size() : 0;
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

std::string TypeLabel(const ColumnMeta& column) {
  std::string type = column.type;
  if (type.empty()) {
    type = "ANY";
  }
  if (column.primary_key_order > 0) {
    type += " · PK";
  }
  if (!column.nullable) {
    type += " · NOT NULL";
  }
  return type;
}

float InitialWidth(const ColumnMeta& column) {
  const int name_width = static_cast<int>(column.name.size()) * 9 + 48;
  int width = std::clamp(name_width, 96, 300);
  const bool numeric = column.type.find("INT") != std::string::npos ||
                       column.type.find("REAL") != std::string::npos ||
                       column.type.find("NUM") != std::string::npos ||
                       column.type.find("DEC") != std::string::npos;
  if (numeric) {
    width = std::min(width, 140);
  }
  return static_cast<float>(width);
}

bool IsBlobColumn(const ColumnMeta& column) {
  std::string type = column.type;
  std::ranges::transform(type, type.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return type.find("blob") != std::string::npos;
}

std::string UriDisplayName(std::string_view uri) {
  const auto slash = uri.find_last_of("/\\");
  std::string name = slash == std::string_view::npos ? std::string(uri)
                                                     : std::string(uri.substr(slash + 1));
  if (name.empty()) {
    name = std::string(uri);
  }
  return name.empty() ? std::string("ADBC 连接") : name;
}

std::string TrimmedSql(std::string_view text) {
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) {
    return {};
  }
  const auto last = text.find_last_not_of(" \t\r\n");
  return std::string(text.substr(first, last - first + 1));
}

NotifyFn MakeNotify(ToastHandle toast, State<AppModel> app) {
  return [toast, app](std::string message) mutable {
    toast.Show(message);
    app.Update([message = std::move(message)](AppModel& model) { model.status = message; });
  };
}

std::vector<DataTableColumn> BuildReadOnlyColumns(
    const std::vector<std::string>& names, const std::shared_ptr<const GridSnapshot>& snapshot,
    Color text_color, Color null_color) {
  std::vector<DataTableColumn> columns;
  columns.reserve(names.size());
  for (std::size_t i = 0; i < names.size(); ++i) {
    DataTableColumn column;
    column.id = "result-" + std::to_string(i);
    column.header = names[i];
    column.width = DataTableColumnWidth{.initial = 160.0F, .min = 80.0F, .max = 520.0F};
    column.render_cell = [snapshot, i, text_color, null_color](
                             const DataTableCellContext& cell) -> DataTableCellContent {
      const bool is_null = CellIsNull(snapshot, cell.id.row_id, i);
      const std::string text = CellText(snapshot, cell.id.row_id, i);
      TextStyle style{Font::Monospace(13.0F), is_null ? null_color : text_color};
      if (is_null) {
        style.font = Font::Monospace(13.0F).WithSlant(FontSlant::Italic);
      }
      return DataTableCellContent{
          .view = Text(text).Style(style),
          .label = text,
      };
    };
    columns.push_back(std::move(column));
  }
  return columns;
}

View TableEmptyState(const std::string& text) {
  return Column{
      MutedText(text),
  }.With(Grow(1.0F), MainAlign{MainAxisAlignment::Center},
         CrossAlign{CrossAxisAlignment::Center});
}

[[huxerui::composable]] View BigEmptyState(std::function<void()> open_picker, std::function<void()> open_new) {
  const ThemeSpec& spec = UseTheme();
  return Column{
      Text("db++").Style(TextStyle{
          Font::System(52.0F).WithWeight(FontWeight::Bold), spec.colors.on_surface}),
      MutedText("ADBC 数据管理器"),
      Row{
          ActionButton(Button("打开数据库…").OnClick(std::move(open_picker)), true),
          ActionButton(Button("连接数据库…").OnClick(std::move(open_new))),
      }.With(Spacing{8.0F}, MainAlign{MainAxisAlignment::Center}),
  }.With(Grow(1.0F), MainAlign{MainAxisAlignment::Center},
         CrossAlign{CrossAxisAlignment::Center}, Spacing{12.0F});
}

} // namespace

// ---------------------------------------------------------------------------
// Sidebar
// ---------------------------------------------------------------------------

[[huxerui::composable]] View Sidebar(State<AppModel> app, State<TableModel> table,
             State<std::optional<DataTableCellId>> selected,
             State<TextEditingValue> filter_input, State<std::size_t> page,
             TaskScope tasks, std::shared_ptr<FilePicker> picker, NotifyFn notify,
             std::function<void()> open_picker, std::function<void()> open_new,
             std::function<void()> open_manager) {
  const ThemeSpec& spec = UseTheme();
  const AppModel& model = app.Get();
  const DbSession* active = model.ActiveSession();
  const std::size_t object_count = active ? active->objects.size() : 0;

  Views session_rows;
  session_rows.Reserve(model.sessions.size());
  for (const DbSession& session : model.sessions) {
    const bool is_active = session.id == model.active_session;
    const std::uint64_t session_id = session.id;
    const bool has_objects = !session.objects.empty();
    const std::string first_object = has_objects ? session.objects.front().name : std::string{};
    const bool first_is_view = has_objects && session.objects.front().is_view;

    View close = Text("×")
                     .Style(TextStyle{Font::System(16.0F), spec.colors.on_surface_variant})
                     .With(Padding{EdgeInsets::Symmetric(6.0F, 2.0F)}, CornerRadius{4.0F})
                     .OnClick([app, table, selected, filter_input, tasks, notify, session_id] {
                       CloseSession(app, table, selected, filter_input, tasks, session_id, notify);
                     });

    View item = NavItem(
        session.name, session.uri, is_active,
        [app, table, selected, filter_input, tasks, page, notify, session_id, first_object,
         first_is_view] {
          if (!first_object.empty()) {
            SelectObject(app, table, selected, filter_input, tasks, session_id, first_object,
                         first_is_view, notify);
          } else {
            app.Update([session_id](AppModel& value) {
              value.active_session = session_id;
              value.active_object.clear();
              value.active_is_view = false;
            });
            table = TableModel{};
            selected = std::nullopt;
          }
          page = 0;
        });
    item = std::move(item).With(Grow(1.0F));

    session_rows.Add(
        Row{std::move(item), std::move(close)}
            .With(Spacing{2.0F}, CrossAlign{CrossAxisAlignment::Center})
            .Key("session-" + std::to_string(session.id)));
  }

  View session_list;
  if (model.sessions.empty()) {
    session_list = MutedText("尚未打开数据库");
  } else {
    session_list = ScrollView(Column{std::move(session_rows)}
                                  .With(Spacing{2.0F}, CrossAlign{CrossAxisAlignment::Stretch}))
                       .With(Frame{.max_height = 220.0F});
  }

  View objects;
  if (active == nullptr) {
    objects = MutedText("打开数据库后显示表和视图");
  } else if (active->objects.empty()) {
    objects = MutedText("该数据库没有表和视图");
  } else {
    Views object_rows;
    object_rows.Reserve(active->objects.size());
    for (const DbObject& object : active->objects) {
      const bool is_selected =
          object.name == model.active_object && active->id == model.active_session;
      const std::uint64_t session_id = active->id;
      const std::string name = object.name;
      const bool is_view = object.is_view;
      object_rows.Add(
          NavItem(name, is_view ? "视图" : "表", is_selected,
                  [app, table, selected, filter_input, tasks, page, notify, session_id, name,
                   is_view] {
                    SelectObject(app, table, selected, filter_input, tasks, session_id, name,
                                 is_view, notify);
                    page = 0;
                  })
              .Key("object-" + std::to_string(session_id) + "-" + name));
    }
    objects = ScrollView(Column{std::move(object_rows)}
                             .With(Spacing{2.0F}, CrossAlign{CrossAxisAlignment::Stretch}))
                  .With(Grow(1.0F));
  }

  View status_block;
  if (!model.error.empty()) {
    status_block = Column{
        ErrorText(model.error),
        MutedText(model.status),
    }.With(Spacing{2.0F}, CrossAlign{CrossAxisAlignment::Stretch});
  } else {
    status_block = MutedText(model.status);
  }

  return Column{
      Row{
          Caption("数据库"),
          Spacer(),
          MutedText(std::to_string(model.sessions.size())),
      }.With(CrossAlign{CrossAxisAlignment::Center}),
      Row{
          ActionButton(Button("打开…").OnClick(std::move(open_picker)))
              .With(Grow(1.0F), Enabled{picker->CanOpenFiles()}),
          ActionButton(Button("连接…").OnClick(std::move(open_new))).With(Grow(1.0F)),
      }.With(Spacing{6.0F}, CrossAlign{CrossAxisAlignment::Stretch}),
      ActionButton(Button("ADBC 管理器…").OnClick(std::move(open_manager)))
          .With(Frame{.width = 236.0F}),
      std::move(session_list),
      Divider(),
      Row{
          Caption("对象"),
          Spacer(),
          MutedText(std::to_string(object_count)),
      }.With(CrossAlign{CrossAxisAlignment::Center}),
      std::move(objects),
      Divider(),
      std::move(status_block),
  }.With(Frame{.width = 260.0F}, Padding{EdgeInsets::Symmetric(12.0F, 12.0F)},
         Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch},
         Background{VisualFill{spec.colors.surface}}, Border{spec.colors.outline, 1.0F});
}

// ---------------------------------------------------------------------------
// Table browser
// ---------------------------------------------------------------------------

[[huxerui::composable]] View AddRowDialog(DialogContext dialog, AddRowContext context) {
  auto values = UseState(std::vector<TextEditingValue>(
      context.columns.size(), TextEditingValue::FromText({})));
  auto error = UseState(std::string{});
  auto busy = UseState(false);

  const auto insert = [context, values, error, busy, dialog] {
    if (busy.Get()) {
      return;
    }
    std::vector<std::string> texts;
    texts.reserve(context.columns.size());
    for (const TextEditingValue& value : values.Get()) {
      texts.push_back(value.text);
    }
    busy = true;
    error = {};
    context.tasks.Launch([context, texts = std::move(texts), error, busy, dialog]() -> Task<void> {
      AdbcQueryResult inserted =
          co_await InsertRowAsync(context.session, context.table.Get(), texts);
      busy = false;
      if (!inserted.error.empty()) {
        error = "插入失败: " + inserted.error;
        co_return;
      }
      const std::string message =
          "已插入 " + std::to_string(inserted.affected) + " 行";
      if (context.notify) {
        context.notify(message);
      }
      dialog.Dismiss();
      RequestLoadTable(context.app, context.table, context.selected, context.tasks, 0,
                       context.table.Get().sort, context.table.Get().filter, true,
                       context.notify);
    });
  };

  Views fields;
  fields.Reserve(context.columns.size());
  for (std::size_t i = 0; i < context.columns.size(); ++i) {
    const ColumnMeta& column = context.columns[i];
    std::string placeholder = "NULL";
    if (column.primary_key_order > 0 && !IsBlobColumn(column) &&
        column.type.find("INT") != std::string::npos) {
      placeholder = "留空 = 自动生成";
    } else {
      placeholder = "留空 = NULL / 默认值";
    }
    View field = TextField(values.Get()[i])
                     .Label(column.name + "  ·  " + TypeLabel(column))
                     .Placeholder(placeholder)
                     .Variant(TextFieldVariant::Outlined)
                     .LineLimits(TextFieldLineLimits::SingleLine())
                     .OnChanged([values, i](const TextEditingValue& value) {
                       values.Update([i, value](std::vector<TextEditingValue>& list) {
                         if (i < list.size()) {
                           list[i] = value;
                         }
                       });
                     });
    fields.Add(std::move(field).Key("add-field-" + std::to_string(i)));
  }

  View body = Column{
      Row{
          Text("添加行", TextRole::Title),
          Spacer(),
          ActionButton(Button("取消").OnClick([dialog] { dialog.Dismiss(); })),
          ActionButton(Button(busy.Get() ? "插入中…" : "插入行")
                           .OnClick(insert)
                           .With(Enabled{!busy.Get()}),
                       true),
      }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{8.0F}),
      MutedText("留空或输入 NULL 会写入 SQL NULL；未填写的列使用默认值或自动主键。"),
      ScrollView(Column{std::move(fields)}.With(Spacing{8.0F}))
          .With(Frame{.max_height = 320.0F}),
      error.Get().empty() ? View{} : ErrorText(error.Get()),
  }.With(Spacing{12.0F}, CrossAlign{CrossAxisAlignment::Stretch});

  return DialogCard(std::move(body).With(Frame{.width = 640.0F}));
}

[[huxerui::composable]] View DeleteRowDialog(DialogContext dialog, State<AppModel> app,
                                             State<TableModel> table,
                                             State<std::optional<DataTableCellId>> selected,
                                             TaskScope tasks, NotifyFn notify) {
  return DialogCard(Column{
      Row{
          Text("删除行", TextRole::Title),
          Spacer(),
          ActionButton(Button("取消").OnClick([dialog] { dialog.Dismiss(); })),
          ActionButton(
              Button("删除").OnClick([app, table, selected, tasks, notify, dialog] {
                RequestDeleteSelected(app, table, selected, tasks, notify);
                dialog.Dismiss();
              }),
              true),
      }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{8.0F}),
      Text("选中的行会立即从数据库中删除，此操作不可撤销。"),
  }.With(Spacing{12.0F}, CrossAlign{CrossAxisAlignment::Stretch})
                    .With(Frame{.width = 520.0F}));
}

[[huxerui::composable]] View AdbcManagerDialog(DialogContext dialog,
                                                   AdbcManagerContext context) {
  auto profiles = context.profiles;
  auto drivers = UseState(std::vector<AdbcDriverInfo>{});
  Lifecycle([drivers] {
    drivers = DiscoverAdbcDrivers();
    return [] {};
  });
  auto selected = UseState<int>(-1);
  auto driver = UseState(TextEditingValue::FromText(context.default_driver));
  auto uri = UseState(TextEditingValue::FromText(context.default_uri));
  auto username = UseState(TextEditingValue::FromText({}));
  auto password = UseState(TextEditingValue::FromText({}));
  auto name = UseState(TextEditingValue::FromText({}));
  auto status = UseState(std::string{});
  auto error = UseState(std::string{});

  const auto save_profile = [context, profiles, selected, driver, uri, username, name, status,
                             error]() -> bool {
    const std::string driver_path = driver.Get().text;
    const std::string uri_text = uri.Get().text;
    if (driver_path.empty() || uri_text.empty()) {
      error = "ADBC driver 路径和 URI 都不能为空";
      return false;
    }
    ConnectionProfile profile{
        .name = name.Get().text.empty() ? UriDisplayName(uri_text) : name.Get().text,
        .driver_path = driver_path,
        .uri = uri_text,
        .username = username.Get().text,
    };
    std::vector<ConnectionProfile> list = profiles.Get();
    int index = selected.Get();
    if (index >= 0 && static_cast<std::size_t>(index) < list.size()) {
      list[static_cast<std::size_t>(index)] = profile;
    } else {
      list.push_back(profile);
      index = static_cast<int>(list.size()) - 1;
    }
    if (!SaveConnectionProfiles(context.profiles_path, list)) {
      error = "保存连接配置失败: " + context.profiles_path;
      return false;
    }
    profiles = std::move(list);
    selected = index;
    name = TextEditingValue::FromText(profile.name);
    error = {};
    status = "已保存 " + profile.name;
    return true;
  };

  const auto delete_profile = [context, profiles, selected, name, status, error] {
    const int index = selected.Get();
    std::vector<ConnectionProfile> list = profiles.Get();
    if (index < 0 || static_cast<std::size_t>(index) >= list.size()) {
      return;
    }
    const auto position = list.begin() + index;
    const std::string removed = position->name.empty() ? position->uri : position->name;
    list.erase(position);
    if (!SaveConnectionProfiles(context.profiles_path, list)) {
      error = "删除后保存失败: " + context.profiles_path;
      return;
    }
    profiles = std::move(list);
    selected = -1;
    name = TextEditingValue::FromText({});
    error = {};
    status = "已删除 " + removed;
  };

  const auto connect = [context, dialog, save_profile, driver, uri, username, password, name] {
    if (!save_profile()) {
      return;
    }
    ConnectionRequest request{
        .driver_path = driver.Get().text,
        .uri = uri.Get().text,
        .username = username.Get().text,
        .password = password.Get().text,
        .title = name.Get().text.empty() ? UriDisplayName(uri.Get().text) : name.Get().text,
    };
    OpenConnection(context.app, context.table, context.selected, context.filter_input,
                   context.tasks, std::move(request), context.notify);
    dialog.Dismiss();
  };

  Views profile_rows;
  const auto& profile_list = profiles.Get();
  profile_rows.Reserve(profile_list.size());
  for (std::size_t index = 0; index < profile_list.size(); ++index) {
    const ConnectionProfile profile = profile_list[index];
    profile_rows.Add(
        NavItem(profile.name.empty() ? UriDisplayName(profile.uri) : profile.name,
                profile.uri, selected.Get() == static_cast<int>(index),
                [profiles, selected, driver, uri, username, password, name, error,
                 index] {
                  const auto& list = profiles.Get();
                  if (index >= list.size()) {
                    return;
                  }
                  const ConnectionProfile& value = list[index];
                  driver = TextEditingValue::FromText(value.driver_path);
                  uri = TextEditingValue::FromText(value.uri);
                  username = TextEditingValue::FromText(value.username);
                  password = TextEditingValue::FromText({});
                  name = TextEditingValue::FromText(value.name);
                  selected = static_cast<int>(index);
                  error = {};
                })
            .Key("profile-" + std::to_string(index) + "-" + profile.uri));
  }

  Views driver_rows;
  const auto& driver_list = drivers.Get();
  driver_rows.Reserve(driver_list.size());
  for (const AdbcDriverInfo& info : driver_list) {
    driver_rows.Add(
        NavItem(info.name, info.path, driver.Get().text == info.path,
                [driver, info] { driver = TextEditingValue::FromText(info.path); })
            .Key("driver-" + info.path));
  }

  View saved_panel = Column{
      Caption("已保存的连接"),
      profile_rows.Size() == 0
          ? View(MutedText("暂无保存的连接"))
          : View(ScrollView(Column{std::move(profile_rows)}.With(Spacing{2.0F}))
                     .With(Grow(1.0F))),
  }.With(Frame{.width = 220.0F}, Spacing{6.0F}, CrossAlign{CrossAxisAlignment::Stretch});

  View form_panel = Column{
      Row{
          Caption("ADBC 驱动"),
          Spacer(),
          ActionButton(Button("刷新").OnClick([drivers, status] {
            drivers = DiscoverAdbcDrivers();
            status = "发现 " + std::to_string(drivers.Get().size()) + " 个驱动";
          })),
      }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{6.0F}),
      driver_rows.Size() == 0
          ? View(MutedText("未发现 ADBC 驱动"))
          : View(ScrollView(Column{std::move(driver_rows)}.With(Spacing{2.0F}))
                     .With(Frame{.height = 140.0F})),
      TextField(driver)
          .Label("ADBC Driver 路径")
          .Placeholder("/path/to/libadbc_driver_postgresql.so")
          .Variant(TextFieldVariant::Outlined)
          .OnChanged([driver](const TextEditingValue& value) { driver = value; }),
      TextField(uri)
          .Label("URI")
          .Placeholder("/path/to/app.sqlite  ·  postgresql://host/db")
          .Variant(TextFieldVariant::Outlined)
          .OnChanged([uri](const TextEditingValue& value) { uri = value; }),
      Row{
          TextField(username)
              .Label("用户名（可选）")
              .Variant(TextFieldVariant::Outlined)
              .OnChanged([username](const TextEditingValue& value) { username = value; })
              .With(Grow(1.0F)),
          TextField(password)
              .Label("密码（不保存）")
              .Variant(TextFieldVariant::Outlined)
              .Secure(true)
              .OnChanged([password](const TextEditingValue& value) { password = value; })
              .With(Grow(1.0F)),
      }.With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch}),
      TextField(name)
          .Label("显示名称（可选）")
          .Placeholder("留空则使用 URI 文件名")
          .Variant(TextFieldVariant::Outlined)
          .OnChanged([name](const TextEditingValue& value) { name = value; }),
      error.Get().empty() ? View{} : ErrorText(error.Get()),
      MutedText(status.Get()),
  }.With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch}, Grow(1.0F));

  View body = Column{
      Row{
          Text("ADBC 管理器", TextRole::Title),
          Spacer(),
          ActionButton(Button("关闭").OnClick([dialog] { dialog.Dismiss(); })),
          ActionButton(Button("删除").OnClick(delete_profile))
              .With(Enabled{selected.Get() >= 0}),
          ActionButton(Button("保存").OnClick([save_profile] { (void)save_profile(); })),
          ActionButton(Button("连接").OnClick(connect), true),
      }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{8.0F}),
      MutedText("保存的连接不含密码；驱动列表来自 DBPP_ADBC_DRIVERS、ADBC_DRIVER_PATH 和系统库目录。"),
      Row{
          std::move(saved_panel),
          Divider(Axis::Vertical),
          std::move(form_panel),
      }.With(Grow(1.0F), Spacing{12.0F}, CrossAlign{CrossAxisAlignment::Stretch}),
  }.With(Spacing{10.0F}, CrossAlign{CrossAxisAlignment::Stretch},
         Frame{.width = 820.0F, .height = 600.0F});

  return DialogCard(std::move(body));
}

[[huxerui::composable]] View ConnectDialog(DialogContext dialog, ConnectContext context) {
  auto driver = UseState(TextEditingValue::FromText(context.default_driver));
  auto uri = UseState(TextEditingValue::FromText(context.default_uri));
  auto username = UseState(TextEditingValue::FromText({}));
  auto password = UseState(TextEditingValue::FromText({}));
  auto title = UseState(TextEditingValue::FromText({}));
  auto error = UseState(std::string{});

  const auto connect = [context, driver, uri, username, password, title, error, dialog] {
    ConnectionRequest request;
    request.driver_path = driver.Get().text;
    request.uri = uri.Get().text;
    request.username = username.Get().text;
    request.password = password.Get().text;
    request.title = title.Get().text;
    if (request.driver_path.empty() || request.uri.empty()) {
      error = "ADBC driver 路径和 URI 都不能为空";
      return;
    }
    OpenConnection(context.app, context.table, context.selected, context.filter_input,
                   context.tasks, std::move(request), context.notify);
    dialog.Dismiss();
  };

  return DialogCard(Column{
      Row{
          Text("连接 ADBC 数据库", TextRole::Title),
          Spacer(),
          ActionButton(Button("取消").OnClick([dialog] { dialog.Dismiss(); })),
          ActionButton(Button("连接").OnClick(connect), true),
      }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{8.0F}),
      MutedText("SQLite 填写文件路径即可；PostgreSQL / MySQL / DuckDB 等填写对应 "
                "ADBC driver 的 .so 路径与连接 URI。"),
      TextField(driver)
          .Label("ADBC Driver 路径")
          .Placeholder("/path/to/libadbc_driver_postgresql.so")
          .Variant(TextFieldVariant::Outlined)
          .OnChanged([driver](const TextEditingValue& value) { driver = value; }),
      TextField(uri)
          .Label("URI")
          .Placeholder("/home/user/data/example.sqlite  ·  postgresql://host/db")
          .Variant(TextFieldVariant::Outlined)
          .OnChanged([uri](const TextEditingValue& value) { uri = value; }),
      Row{
          TextField(username)
              .Label("用户名（可选）")
              .Variant(TextFieldVariant::Outlined)
              .OnChanged([username](const TextEditingValue& value) { username = value; })
              .With(Grow(1.0F)),
          TextField(password)
              .Label("密码（可选）")
              .Variant(TextFieldVariant::Outlined)
              .Secure(true)
              .OnChanged([password](const TextEditingValue& value) { password = value; })
              .With(Grow(1.0F)),
      }.With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch}),
      TextField(title)
          .Label("显示名称（可选）")
          .Placeholder("留空则使用 URI 文件名")
          .Variant(TextFieldVariant::Outlined)
          .OnChanged([title](const TextEditingValue& value) { title = value; }),
      error.Get().empty() ? View{} : ErrorText(error.Get()),
  }.With(Spacing{12.0F}, CrossAlign{CrossAxisAlignment::Stretch})
                    .With(Frame{.width = 680.0F}));
}

[[huxerui::composable]] View TableBrowserPage(
    State<AppModel> app, State<TableModel> table,
    State<std::optional<DataTableCellId>> selected, State<TextEditingValue> filter_input,
    TaskScope tasks, NotifyFn notify, bool dark) {
  const ThemeSpec& spec = UseTheme();
  const auto dialogs = UseDialog();
  const AppModel& app_model = app.Get();
  const TableModel& model = table.Get();
  const DbSession* session = app_model.ActiveSession();

  if (session == nullptr) {
    return TableEmptyState("打开或连接一个数据库开始");
  }
  if (app_model.active_object.empty()) {
    return TableEmptyState("从左侧选择一个表或视图");
  }

  const auto reload = [app, table, selected, tasks, notify](std::int64_t page,
                                                            std::string filter) {
    const TableModel current = table.Get();
    RequestLoadTable(app, table, selected, tasks, page, current.sort, std::move(filter),
                     false, notify);
  };

  const auto apply_filter = [reload, filter_input] {
    reload(0, filter_input.Get().text);
  };

  const auto refresh = [reload, table] {
    const TableModel current = table.Get();
    reload(current.page, current.filter);
  };

  const auto open_add = [dialogs, app, table, selected, filter_input, tasks, notify, session, dark] {
    const TableModel current = table.Get();
    if (!current.editable || !session->session) {
      return;
    }
    AddRowContext context{
        .session_id = session->id,
        .object = current.object,
        .qualified_name = current.qualified_name,
        .columns = current.columns,
        .session = session->session,
        .app = app,
        .table = table,
        .selected = selected,
        .filter_input = filter_input,
        .tasks = tasks,
        .dark = dark,
        .notify = notify,
    };
    dialogs.Show([context](DialogContext dialog) {
      const ThemeSpec dialog_spec = context.dark ? DarkThemeSpec() : LightThemeSpec();
      return Theme(AppThemeDefinition(dialog_spec), AddRowDialog(dialog, context));
    });
  };

  const auto open_delete = [dialogs, app, table, selected, tasks, notify, dark] {
    dialogs.Show([app, table, selected, tasks, notify, dark](DialogContext dialog) {
      const ThemeSpec dialog_spec = dark ? DarkThemeSpec() : LightThemeSpec();
      return Theme(AppThemeDefinition(dialog_spec),
                   DeleteRowDialog(dialog, app, table, selected, tasks, notify));
    });
  };

  View toolbar = Row{
      TextField(filter_input)
          .Label("筛选  WHERE")
          .Placeholder("例如 price > 10 AND name LIKE 'A%'")
          .Variant(TextFieldVariant::Outlined)
          .LineLimits(TextFieldLineLimits::SingleLine())
          .OnChanged([filter_input](const TextEditingValue& value) { filter_input = value; })
          .OnSubmitted([apply_filter] { apply_filter(); })
          .With(Frame{.width = 260.0F}),
      ActionButton(Button("应用筛选").OnClick(apply_filter)),
      Spacer(),
      ActionButton(Button("刷新").OnClick(refresh)),
      ActionButton(Button("＋ 添加行").OnClick(open_add))
          .With(Enabled{model.editable && !model.read_only}),
      ActionButton(Button("删除行").OnClick(open_delete))
          .With(Enabled{model.editable && !model.read_only &&
                        selected.Get().has_value()}),
  }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{8.0F});

  const std::int64_t page_count =
      std::max<std::int64_t>(1, (model.total + model.page_size - 1) / model.page_size);
  View pager = Row{
      MutedText(model.message.empty() ? "没有数据" : model.message),
      model.read_only ? Chip("只读") : View{},
      Spacer(),
      ActionButton(Button("上一页")
                       .OnClick([reload, model] {
                         if (model.page > 0) {
                           reload(model.page - 1, model.filter);
                         }
                       }))
          .With(Enabled{model.page > 0}),
      ActionButton(Button("下一页")
                       .OnClick([reload, model, page_count] {
                         if (model.page + 1 < page_count) {
                           reload(model.page + 1, model.filter);
                         }
                       }))
          .With(Enabled{model.page + 1 < page_count}),
  }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{8.0F});

  View table_view;
  if (model.snapshot == nullptr || model.columns.empty()) {
    table_view = TableEmptyState(model.loading ? "正在加载…" : "表中没有可显示的列");
  } else {
    const std::shared_ptr<const GridSnapshot> snapshot = model.snapshot;
    std::vector<DataTableColumn> columns;
    columns.reserve(model.columns.size());
    for (std::size_t index = 0; index < model.columns.size(); ++index) {
      const ColumnMeta& meta = model.columns[index];
      const float width = InitialWidth(meta);
      DataTableColumn column;
      column.id = meta.name;
      column.header = std::string(meta.name);
      column.width = DataTableColumnWidth{.initial = width, .min = 72.0F, .max = 520.0F};
      column.sortable = true;
      column.render_cell = [snapshot, index, spec](
                               const DataTableCellContext& cell) -> DataTableCellContent {
        const bool is_null = CellIsNull(snapshot, cell.id.row_id, index);
        const std::string text = CellText(snapshot, cell.id.row_id, index);
        TextStyle style{Font::Monospace(13.0F),
                        is_null ? spec.colors.on_surface_variant : spec.colors.on_surface};
        if (is_null) {
          style.font = Font::Monospace(13.0F).WithSlant(FontSlant::Italic);
        }
        return DataTableCellContent{
            .view = Text(text).Style(style),
            .label = text,
        };
      };
      if (model.editable && !model.read_only && !IsBlobColumn(meta)) {
        column.editor = DataTableEditor{
            .initial_text = [snapshot, index](const DataTableCellContext& cell) {
              return CellIsNull(snapshot, cell.id.row_id, index)
                         ? std::string{"NULL"}
                         : CellText(snapshot, cell.id.row_id, index);
            },
            .render = [](const DataTableEditContext& edit) {
              return TextField(edit.draft)
                  .Variant(TextFieldVariant::Standard)
                  .LineLimits(TextFieldLineLimits::SingleLine())
                  .VerticalAlign(TextVerticalAlign::Center)
                  .InputConfiguration({.action = TextInputAction::Done})
                  .OnChanged(
                      [draft = edit.draft](const TextEditingValue& value) { draft = value; })
                  .OnSubmitted(edit.commit)
                  .With(Padding{EdgeInsets::Symmetric(4.0F, 8.0F)}, edit.focus_target);
            },
        };
      }
      columns.push_back(std::move(column));
    }

    DataTableOptions options{
        .rows = MakeRows(snapshot),
        .columns = std::move(columns),
        .row_height = 30.0F,
        .header_height = 32.0F,
        .frozen_column_count = 0,
        .sort = model.sort,
        .selected_cell = selected.Get(),
    };

    table_view =
        DataTable(std::move(options))
            .On<DataTableEvents::SortRequested>(
                [app, table, selected, tasks, notify](const DataTableSort& requested) {
                  const TableModel current = table.Get();
                  DataTableSort next = requested;
                  if (current.sort.has_value() &&
                      current.sort->column_id == requested.column_id) {
                    next.direction =
                        current.sort->direction == DataTableSortDirection::Ascending
                            ? DataTableSortDirection::Descending
                            : DataTableSortDirection::Ascending;
                  } else {
                    next.direction = DataTableSortDirection::Ascending;
                  }
                  RequestLoadTable(app, table, selected, tasks, 0, next, current.filter,
                                   false, notify);
                })
            .On<DataTableEvents::SelectionRequested>(
                [selected](const DataTableCellId& cell) { selected = cell; })
            .On<DataTableEvents::EditSubmitted>(
                [app, table, selected, tasks, notify](const DataTableEditRequest& edit) {
                  RequestSaveCell(app, table, selected, tasks, edit, notify);
                })
            .With(Grow(1.0F), Border{spec.colors.outline, 1.0F});
  }

  return Column{
      std::move(toolbar),
      std::move(pager),
      model.error.empty() ? View{} : ErrorText(model.error),
      std::move(table_view),
  }.With(Grow(1.0F), Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch});
}

// ---------------------------------------------------------------------------
// SQL console
// ---------------------------------------------------------------------------

[[huxerui::composable]] View SqlPage(State<AppModel> app, State<SqlModel> sql, State<TableModel> table,
             State<std::optional<DataTableCellId>> selected,
             State<TextEditingValue> filter_input, TaskScope tasks, NotifyFn notify) {
  const ThemeSpec& spec = UseTheme();
  const AppModel& model = app.Get();
  const SqlModel& sql_model = sql.Get();
  const DbSession* session = model.ActiveSession();

  if (session == nullptr || !session->session) {
    return TableEmptyState("打开一个数据库后即可执行 SQL");
  }

  const auto run = [app, sql, table, selected, filter_input, tasks, notify] {
    const AppModel& current = app.Get();
    const DbSession* active = current.ActiveSession();
    if (active == nullptr || !active->session) {
      return;
    }
    const std::string text = TrimmedSql(sql.Get().editor.text);
    if (text.empty()) {
      return;
    }
    const std::uint64_t session_id = active->id;
    auto adbc = active->session;
    sql.Update([](SqlModel& value) {
      value.running = true;
      value.error.clear();
      value.status = "执行中…";
    });
    tasks.Launch([app, sql, table, selected, filter_input, tasks, notify, adbc, session_id,
                  text]() -> Task<void> {
      if (IsSqlReadStatement(text)) {
        AdbcQueryResult result =
            co_await ExecuteQueryAsync(adbc, text, true, kMaxQueryRows);
        if (!result.error.empty()) {
          const std::string message = result.error;
          sql.Update([message](SqlModel& value) {
            value.running = false;
            value.error = message;
            value.status.clear();
          });
          if (notify) {
            notify("SQL 失败: " + message);
          }
          co_return;
        }
        auto query = std::make_shared<QueryResult>();
        query->columns = std::move(result.columns);
        query->snapshot = std::make_shared<const GridSnapshot>(
            GridSnapshot{.rows = std::move(result.rows)});
        query->truncated = result.truncated;
        const std::size_t rows = query->snapshot->rows.size();
        sql.Update([query, rows](SqlModel& value) {
          value.result = query;
          value.running = false;
          value.error.clear();
          value.status = "返回 " + std::to_string(rows) + " 行";
          if (query->truncated) {
            value.status += "（已截断）";
          }
        });
        co_return;
      }

      AdbcQueryResult executed =
          co_await ExecuteQueryAsync(adbc, text, false, 0);
      if (!executed.error.empty()) {
        const std::string message = executed.error;
        sql.Update([message](SqlModel& value) {
          value.running = false;
          value.error = message;
          value.status.clear();
        });
        if (notify) {
          notify("SQL 失败: " + message);
        }
        co_return;
      }

      AdbcObjectsResult objects = co_await adbc->sequence.Run(
          [adbc](std::stop_token) { return LoadObjectsBlocking(*adbc); });
      sql.Update([affected = executed.affected](SqlModel& value) {
        value.result.reset();
        value.running = false;
        value.error.clear();
        value.status = affected >= 0 ? "影响 " + std::to_string(affected) + " 行"
                                     : "执行完成";
      });
      if (!objects.error.empty()) {
        if (notify) {
          notify("执行成功，但刷新结构失败: " + objects.error);
        }
        co_return;
      }

      app.Update([&](AppModel& value) {
        for (DbSession& open : value.sessions) {
          if (open.id != session_id) {
            continue;
          }
          open.objects = std::move(objects.objects);
          bool found = false;
          for (const DbObject& object : open.objects) {
            if (object.name == value.active_object) {
              value.active_is_view = object.is_view;
              found = true;
              break;
            }
          }
          if (!found) {
            value.active_object =
                open.objects.empty() ? std::string{} : open.objects.front().name;
            value.active_is_view =
                !open.objects.empty() && open.objects.front().is_view;
          }
          ++value.revision;
          break;
        }
      });
      filter_input = TextEditingValue::FromText({});
      RequestLoadTable(app, table, selected, tasks, 0, std::nullopt, {}, false, notify);
      if (notify) {
        notify("SQL 已执行，影响 " + std::to_string(executed.affected) + " 行");
      }
    });
  };

  View result_view;
  if (sql_model.result) {
    const std::shared_ptr<const GridSnapshot> snapshot = sql_model.result->snapshot;
    if (snapshot && !sql_model.result->columns.empty()) {
      DataTableOptions options{
          .rows = MakeRows(snapshot),
          .columns = BuildReadOnlyColumns(sql_model.result->columns, snapshot,
                                          spec.colors.on_surface,
                                          spec.colors.on_surface_variant),
          .row_height = 30.0F,
          .header_height = 32.0F,
          .frozen_column_count = 0,
          .sort = std::nullopt,
          .selected_cell = std::nullopt,
      };
      result_view = DataTable(std::move(options))
                        .With(Grow(1.0F), Border{spec.colors.outline, 1.0F});
    } else {
      result_view = TableEmptyState(sql_model.status);
    }
  } else if (!sql_model.error.empty()) {
    result_view = TableEmptyState("查询失败");
  } else {
    result_view = TableEmptyState("执行一条语句以查看结果");
  }

  return Column{
      Row{
          Text("SQL", TextRole::Title),
          MutedText(session->name),
          Spacer(),
          ActionButton(Button(sql_model.running ? "执行中…" : "执行")
                           .OnClick(run)
                           .With(Enabled{!sql_model.running}),
                       true),
      }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{8.0F}),
      MonospaceField(
          TextField(sql_model.editor)
              .Label("SQL 语句")
              .Placeholder("SELECT * FROM table LIMIT 100;")
              .Variant(TextFieldVariant::Outlined)
              .LineLimits(TextFieldLineLimits::MultiLine(6, 12))
              .OnChanged([sql](const TextEditingValue& value) {
                sql.Update([value](SqlModel& model) { model.editor = value; });
              })
              .With(Frame{.min_height = 140.0F})),
      Row{
          MutedText(sql_model.status),
          Spacer(),
          MutedText("支持 SELECT / PRAGMA / 写语句 / RETURNING"),
      }.With(CrossAlign{CrossAxisAlignment::Center}),
      sql_model.error.empty() ? View{} : ErrorText(sql_model.error),
      std::move(result_view),
  }.With(Grow(1.0F), Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch});
}

// ---------------------------------------------------------------------------
// Application shell
// ---------------------------------------------------------------------------

[[huxerui::composable]] View AppContent() {
  auto app = UseState(AppModel{});
  auto table = UseState(TableModel{});
  SqlModel initial_sql;
  initial_sql.editor = TextEditingValue::FromText("SELECT 1 AS hello;");
  auto sql = UseState(std::move(initial_sql));
  auto selected = UseState(std::optional<DataTableCellId>{});
  auto filter_input = UseState(TextEditingValue::FromText({}));
  auto page = UseState<std::size_t>(0);
  auto theme_mode = UseState<int>(0);
  auto toast = UseToast();
  auto dialogs = UseDialog();
  auto tasks = UseTaskScope();
  auto picker = UseService<FilePicker>();
  auto application = UseApplication();
  const File profile_dir = application.Directories().data_directory;
  const std::string profiles_path = profile_dir.Child("connections.tsv").Path();
  const File default_file = application.Directories().data_directory.Child("untitled.sqlite");
  auto profiles = UseState(std::vector<ConnectionProfile>{});
  Lifecycle([profiles, profiles_path] {
    profiles = LoadConnectionProfiles(profiles_path);
    return [] {};
  });

  const bool system_dark = UseSystemThemeBrightness() == ThemeBrightness::Dark;
  const bool dark =
      theme_mode.Get() == 1 || (theme_mode.Get() == 0 && system_dark);
  const ThemeSpec spec = dark ? DarkThemeSpec() : LightThemeSpec();
  const NotifyFn notify = MakeNotify(toast, app);

  // Optional startup connection: `DBPP_OPEN=/path/to.db dbpp` or
  // `DBPP_DRIVER=/path/to/libadbc_driver_x.so DBPP_OPEN=uri dbpp`.
  Lifecycle([app, table, selected, filter_input, tasks, notify] {
    const char* open_path = std::getenv("DBPP_OPEN");
    if (open_path != nullptr && *open_path != '\0') {
      const char* driver = std::getenv("DBPP_DRIVER");
      ConnectionRequest request{
          .driver_path = driver != nullptr && *driver != '\0' ? driver
                                                               : DefaultSqliteDriverPath(),
          .uri = open_path,
          .username = {},
          .password = {},
          .title = {},
      };
      OpenConnection(app, table, selected, filter_input, tasks, std::move(request), notify);
    }
    return [] {};
  });

  const auto open_picker = [picker, tasks, app, table, selected, filter_input,
                            notify]() mutable {
    if (!picker->CanOpenFiles()) {
      notify("当前平台不支持系统文件选择器");
      return;
    }
    tasks.Launch([picker, app, table, selected, filter_input, tasks, notify]() -> Task<void> {
      auto file_reference = co_await picker->OpenFileAsync(FilePickerFilter{
          .name = "SQLite 数据库",
          .extensions = {"sqlite", "sqlite3", "db", "db3", "s3db"},
          .content_types = {},
      });
      if (!file_reference) {
        co_return;
      }
      auto file = file_reference->AsFile();
      if (!file) {
        notify("所选文件没有本地路径");
        co_return;
      }
      OpenConnection(app, table, selected, filter_input, tasks,
                     ConnectionRequest{
                         .driver_path = DefaultSqliteDriverPath(),
                         .uri = file->Path(),
                         .username = {},
                         .password = {},
                         .title = file->Name(),
                     },
                     notify);
    });
  };

  const auto open_new = [dialogs, app, table, sql, selected, filter_input, tasks,
                         notify, default_file, dark] {
    ConnectContext context{
        .app = app,
        .table = table,
        .sql = sql,
        .selected = selected,
        .filter_input = filter_input,
        .tasks = tasks,
        .default_driver = DefaultSqliteDriverPath(),
        .default_uri = default_file.Path(),
        .dark = dark,
        .notify = notify,
    };
    dialogs.Show([context](DialogContext dialog) {
      const ThemeSpec dialog_spec = context.dark ? DarkThemeSpec() : LightThemeSpec();
      return Theme(AppThemeDefinition(dialog_spec), ConnectDialog(dialog, context));
    });
  };

  const auto open_manager = [dialogs, app, table, selected, filter_input, profiles, tasks,
                             profiles_path, default_file, notify, dark] {
    AdbcManagerContext context{
        .app = app,
        .table = table,
        .selected = selected,
        .filter_input = filter_input,
        .profiles = profiles,
        .tasks = tasks,
        .profiles_path = profiles_path,
        .default_driver = DefaultSqliteDriverPath(),
        .default_uri = default_file.Path(),
        .dark = dark,
        .notify = notify,
    };
    dialogs.Show([context](DialogContext dialog) {
      const ThemeSpec dialog_spec = context.dark ? DarkThemeSpec() : LightThemeSpec();
      return Theme(AppThemeDefinition(dialog_spec), AdbcManagerDialog(dialog, context));
    });
  };

  const AppModel& model = app.Get();
  const DbSession* active = model.ActiveSession();

  View title_bar = WindowTitleBar{
      Text("db++").Style(TextStyle{
          Font::System(13.0F).WithWeight(FontWeight::Bold), spec.colors.on_surface}),
      MutedText("ADBC"),
      Spacer(),
      MutedText(active ? active->name : "未打开数据库"),
      ActionButton(Button(dark ? "浅色" : "深色").OnClick([theme_mode, dark] {
        theme_mode = dark ? 2 : 1;
      })),
  }.With(Padding{EdgeInsets::Symmetric(16.0F, 0.0F)}, Spacing{10.0F});

  View main_content;
  if (active == nullptr) {
    main_content = BigEmptyState(open_picker, open_new);
  } else {
    const std::string title =
        model.active_object.empty() ? "数据浏览器" : model.active_object;
    const auto page_index = page.Get();
    View header = Row{
        Column{
            Text(title, TextRole::Title),
            MutedText(active->uri),
        }.With(Grow(1.0F), Spacing{2.0F}, CrossAlign{CrossAxisAlignment::Stretch}),
        SegmentedButton({"浏览", "SQL"}, page_index)
            .OnChanged([page](std::size_t index) { page = index; }),
    }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{12.0F});

    View pages = IndexedPages(
        {
            TableBrowserPage(app, table, selected, filter_input, tasks, notify, dark),
            SqlPage(app, sql, table, selected, filter_input, tasks, notify),
        },
        page)
                     .With(Grow(1.0F));

    main_content = Column{
        std::move(header),
        Divider(),
        std::move(pages),
    }.With(Padding{EdgeInsets::Symmetric(16.0F, 12.0F)}, Spacing{10.0F},
           CrossAlign{CrossAxisAlignment::Stretch});
  }

  View shell = Column{
      std::move(title_bar),
      Row{
          Sidebar(app, table, selected, filter_input, page, tasks, picker, notify,
                  open_picker, open_new, open_manager),
          std::move(main_content).With(Grow(1.0F)),
      }.With(Grow(1.0F), CrossAlign{CrossAxisAlignment::Stretch}),
  }.With(Grow(1.0F), CrossAlign{CrossAxisAlignment::Stretch},
         Background{VisualFill{spec.colors.background}});

  return Theme(AppThemeDefinition(spec), std::move(shell));
}

} // namespace dbpp

huxerui::View AppRoot() {
  return dbpp::AppContent();
}

const huxerui::Application application{
    AppRoot,
    {
        .window =
            {
                .title = "db++",
                .initial_size = {1240.0F, 800.0F},
                .minimum_size = huxerui::Size{900.0F, 600.0F},
                .chrome_mode = huxerui::WindowChromeMode::Custom,
                .title_bar_height = 32.0F,
            },
        .show_debug_overlay = false,
    },
};
