#include "model.h"
#include "config_db.h"
#include "theme.h"
#include "adbc_session.h"
#include "driver_installer.h"
#include "root.h"
#include "settings.h"
#include "tray.h"
#include "app_resources.h"

#include <huxerui/datatable.h>
#include <huxerui/huxerui.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
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

bool IsDraftRowId(std::string_view id) {
  return id.starts_with("draft:");
}

std::optional<std::uint64_t> ParseDraftRowId(std::string_view id) {
  if (!IsDraftRowId(id)) {
    return std::nullopt;
  }
  std::uint64_t draft_id = 0;
  const char* begin = id.data() + 6;
  const char* end_ptr = id.data() + id.size();
  const auto [end, error] = std::from_chars(begin, end_ptr, draft_id);
  if (error != std::errc{} || end != end_ptr) {
    return std::nullopt;
  }
  return draft_id;
}

const PendingInsertRow* FindDraftRow(const std::vector<PendingInsertRow>& drafts,
                                     std::uint64_t draft_id) {
  for (const PendingInsertRow& draft : drafts) {
    if (draft.draft_id == draft_id) {
      return &draft;
    }
  }
  return nullptr;
}

PendingInsertRow* FindDraftRowMutable(std::vector<PendingInsertRow>& drafts,
                                      std::uint64_t draft_id) {
  for (PendingInsertRow& draft : drafts) {
    if (draft.draft_id == draft_id) {
      return &draft;
    }
  }
  return nullptr;
}

std::string DraftCellBufferKey(std::string_view row_id, std::string_view col_id) {
  std::string key(row_id);
  key.push_back('\n');
  key += col_id;
  return key;
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

DataTableRowSource MakeTableRows(const std::shared_ptr<const GridSnapshot>& snapshot,
                                 const std::vector<PendingInsertRow>& drafts) {
  const std::size_t committed_count = snapshot ? snapshot->rows.size() : 0;
  const std::size_t total_count = committed_count + drafts.size();
  return DataTableRowSource{
      .count = total_count,
      .id_at = [committed_count, drafts](std::size_t index) -> std::string {
        if (index < committed_count) {
          return std::to_string(index);
        }
        const std::size_t draft_index = index - committed_count;
        if (draft_index < drafts.size()) {
          return "draft:" + std::to_string(drafts[draft_index].draft_id);
        }
        return std::to_string(index);
      },
      .index_of = [committed_count, drafts](std::string_view id) -> std::optional<std::size_t> {
        if (const auto draft_id = ParseDraftRowId(id)) {
          for (std::size_t i = 0; i < drafts.size(); ++i) {
            if (drafts[i].draft_id == *draft_id) {
              return committed_count + i;
            }
          }
          return std::nullopt;
        }
        const std::size_t index = RowIndexFromId(id);
        if (index == static_cast<std::size_t>(-1) || index >= committed_count) {
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

bool IsBlobColumn(const ColumnMeta& column) {
  std::string type = column.type;
  std::ranges::transform(type, type.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return type.find("blob") != std::string::npos;
}

bool ContainsIgnoreCaseAscii(std::string_view haystack, std::string_view needle) {
  if (needle.empty()) {
    return true;
  }
  const auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                              [](char left, char right) {
                                return std::tolower(static_cast<unsigned char>(left)) ==
                                       std::tolower(static_cast<unsigned char>(right));
                              });
  return it != haystack.end();
}

std::string_view StripLeadingV(std::string_view text) {
  if (!text.empty() && (text.front() == 'v' || text.front() == 'V')) {
    text.remove_prefix(1);
  }
  return text;
}

std::string UriDisplayName(std::string_view uri) {
  return DisplayNameFromUri(uri, "ADBC 连接");
}

std::string TrimmedSql(std::string_view text) {
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) {
    return {};
  }
  const auto last = text.find_last_not_of(" \t\r\n");
  return std::string(text.substr(first, last - first + 1));
}

std::string NormalizeToastMessage(std::string_view text) {
  const std::string utf8 = SanitizeUtf8(text);
  std::string out;
  out.reserve(utf8.size());
  bool pending_sep = false;
  for (const char c : utf8) {
    if (c == '\r' || c == '\n') {
      if (!out.empty()) {
        pending_sep = true;
      }
      continue;
    }
    if (pending_sep) {
      if (c != ' ' && c != '\t') {
        out += " · ";
        pending_sep = false;
      } else {
        continue;
      }
    }
    out.push_back(c);
  }
  const std::size_t first = out.find_first_not_of(" \t");
  if (first == std::string::npos) {
    return {};
  }
  const std::size_t last = out.find_last_not_of(" \t");
  out = out.substr(first, last - first + 1);

  constexpr std::size_t kMaxToastBytes = 240;
  if (out.size() > kMaxToastBytes) {
    std::size_t cut = kMaxToastBytes - 3;
    while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0U) == 0x80U) {
      --cut;
    }
    out.resize(cut);
    out += "…";
  }
  return out;
}

bool IsLikelyErrorMessage(std::string_view text) {
  return text.find("失败") != std::string_view::npos ||
         text.find("错误") != std::string_view::npos ||
         text.find("不能") != std::string_view::npos ||
         text.find("无法") != std::string_view::npos ||
         text.find("未找到") != std::string_view::npos ||
         text.find("未提供") != std::string_view::npos ||
         text.find("不可用") != std::string_view::npos ||
         text.find("不支持") != std::string_view::npos ||
         text.find("没有") != std::string_view::npos;
}

NotifyFn MakeNotify(ToastHandle toast,
                    std::shared_ptr<std::optional<LayerId>> last_toast_id) {
  return [toast, last_toast_id](std::string message) mutable {
    const std::string cleaned = NormalizeToastMessage(message);
    if (cleaned.empty()) {
      return;
    }
    if (last_toast_id && last_toast_id->has_value()) {
      (void)toast.Dismiss(**last_toast_id);
      last_toast_id->reset();
    }
    const double duration =
        (IsLikelyErrorMessage(cleaned) || cleaned.size() > 48) ? 4.5 : 2.5;
    const LayerId id = toast.Show(cleaned, ToastOptions{.duration = duration});
    if (last_toast_id) {
      *last_toast_id = id;
    }
  };
}

std::vector<DataTableColumn> BuildReadOnlyColumns(
    const std::vector<std::string>& names, const std::shared_ptr<const GridSnapshot>& snapshot,
    const std::unordered_map<std::string, float>& column_widths, Color text_color,
    Color null_color) {
  std::vector<DataTableColumn> columns;
  columns.reserve(names.size());
  for (std::size_t i = 0; i < names.size(); ++i) {
    DataTableColumn column;
    column.id = "result-" + std::to_string(i);
    column.header = names[i];
    const auto width_it = column_widths.find(column.id);
    const float raw_width = width_it != column_widths.end()
                                ? width_it->second
                                : ComputeAutoColumnWidth(names[i], i, snapshot.get());
    const float initial = std::clamp(raw_width, kMinColumnWidth, kMaxColumnWidth);
    column.width = DataTableColumnWidth{
        .initial = initial,
        .min = kMinColumnWidth,
        .max = kMaxColumnWidth,
    };
    column.render_cell = [snapshot, i, text_color, null_color](
                             const DataTableCellContext& cell) -> DataTableCellContent {
      const bool is_null = CellIsNull(snapshot, cell.id.row_id, i);
      const std::string text = CellText(snapshot, cell.id.row_id, i);
      TextStyle style{Font::Monospace(13.0F), is_null ? null_color : text_color};
      if (is_null) {
        style.font = Font::Monospace(13.0F).WithSlant(FontSlant::Italic);
      }
      return DataTableCellContent{
          .view = Text(SingleLineCellPreview(text))
                      .Style(style)
                      .LineLimit(1)
                      .Overflow(TextOverflow::Ellipsis),
          .label = text,
      };
    };
    columns.push_back(std::move(column));
  }
  return columns;
}

View WithDialogTheme(const AppThemeConfig& config, bool system_dark, View content) {
  return Theme(AppThemeDefinition(ResolveTheme(config, system_dark)), std::move(content));
}

View TableEmptyState(const std::string& text) {
  return Column{
      MutedText(text),
  }.With(Grow(1.0F), MainAlign{MainAxisAlignment::Center},
         CrossAlign{CrossAxisAlignment::Center});
}

[[huxerui::composable]] View BigEmptyState(std::function<void()> open_new) {
  const ThemeSpec& spec = UseTheme();
  const SemanticColors& semantic = UseEnvironment<SemanticColors>();
  return Column{
      Image(app::images::dbpp_logo)
          .Fit(ImageFit::Contain)
          .With(Grow(0.0F),
                Frame{.width = 64.0F, .height = 64.0F, .min_width = 64.0F, .max_width = 64.0F,
                      .min_height = 64.0F, .max_height = 64.0F}),
      Text("db++").Style(TextStyle{
          Font::System(52.0F).WithWeight(FontWeight::Bold), spec.colors.on_surface}),
      Spacer().With(Grow(0.0F),
                    Frame{.width = 84.0F, .height = 3.5F, .min_width = 84.0F, .max_width = 84.0F,
                          .min_height = 3.5F, .max_height = 3.5F},
                    CornerRadius{2.0F}, Background{semantic.accent_gradient}),
      MutedText("ADBC 数据管理器"),
      ActionButton(Button("新建连接…").OnClick(std::move(open_new)), ButtonTone::Primary),
  }.With(Grow(1.0F), MainAlign{MainAxisAlignment::Center},
         CrossAlign{CrossAxisAlignment::Center}, Spacing{12.0F},
         Background{semantic.hero_glow});
}

std::optional<std::size_t> FindMatchingSessionIndex(const ConnectionProfile& profile,
                                                    const std::vector<DbSession>& sessions,
                                                    const std::vector<bool>& claimed) {
  for (std::size_t i = 0; i < sessions.size(); ++i) {
    if (claimed[i]) {
      continue;
    }
    const DbSession& session = sessions[i];
    if (session.uri != profile.uri) {
      continue;
    }
    if (session.driver_path == profile.driver_path) {
      return i;
    }
    if (session.sqlite && ContainsIgnoreCaseAscii(profile.driver_path, "sqlite")) {
      return i;
    }
  }
  return std::nullopt;
}

bool PersistConnectionProfileList(const std::shared_ptr<ConfigDbService>& config_db,
                                  const std::string& profiles_path,
                                  const std::vector<ConnectionProfile>& list) {
  if (config_db) {
    config_db->PersistConnections(list);
    return true;
  }
  return !profiles_path.empty() && SaveConnectionProfiles(profiles_path, list);
}

[[huxerui::composable]] View EditConnectionProfileDialog(
    DialogContext dialog, std::optional<ConnectionProfile> original_profile,
    ConnectionProfile initial_values, std::optional<std::uint64_t> active_session_id,
    State<AppModel> app, State<WorkspaceModel> workspace,
    State<std::vector<ConnectionProfile>> profiles, std::string profiles_path, TaskScope tasks,
    NotifyFn notify) {
  const auto config_db = UseService<ConfigDbService>();
  auto name = UseState(TextEditingValue::FromText(initial_values.name));
  auto driver = UseState(TextEditingValue::FromText(initial_values.driver_path));
  auto uri = UseState(TextEditingValue::FromText(initial_values.uri));
  auto username = UseState(TextEditingValue::FromText(initial_values.username));
  auto password = UseState(TextEditingValue::FromText(initial_values.password));
  auto show_password = UseState(false);

  const auto persist_profile = [original_profile, name, driver, uri, username, password,
                                profiles, profiles_path, config_db,
                                notify](ConnectionProfile& out_profile) {
    const std::string driver_text = TrimmedSql(driver.Get().text);
    const std::string uri_raw = TrimmedSql(uri.Get().text);
    if (driver_text.empty() || uri_raw.empty()) {
      if (notify) {
        notify("ADBC 驱动与连接 URI 都不能为空");
      }
      return false;
    }
    const SanitizedUriResult sanitized = SanitizeUriCredentials(uri_raw);
    const std::string title_text = TrimmedSql(name.Get().text);
    const std::string user_text =
        username.Get().text.empty() ? sanitized.username : username.Get().text;
    const std::string pass_text =
        password.Get().text.empty() ? sanitized.password : password.Get().text;
    out_profile = ConnectionProfile{
        .name = title_text.empty() ? UriDisplayName(sanitized.uri) : title_text,
        .driver_path = driver_text,
        .uri = sanitized.uri,
        .username = user_text,
        .password = pass_text,
    };

    if (!profiles_path.empty()) {
      std::vector<ConnectionProfile> list = profiles.Get();
      bool replaced = false;
      if (original_profile.has_value()) {
        if (auto it = std::ranges::find(list, *original_profile); it != list.end()) {
          *it = out_profile;
          replaced = true;
        } else {
          for (ConnectionProfile& existing : list) {
            if (existing.name == original_profile->name &&
                existing.uri == original_profile->uri) {
              existing = out_profile;
              replaced = true;
              break;
            }
          }
        }
      }
      if (!replaced) {
        for (ConnectionProfile& existing : list) {
          if (existing.name == out_profile.name) {
            existing = out_profile;
            replaced = true;
            break;
          }
        }
      }
      if (!replaced) {
        list.push_back(out_profile);
      }
      if (!PersistConnectionProfileList(config_db, profiles_path, list)) {
        if (notify) {
          notify("保存连接配置失败: " + profiles_path);
        }
        return false;
      }
      profiles = std::move(list);
    }
    return true;
  };

  const auto save_only = [dialog, persist_profile, notify] {
    ConnectionProfile saved;
    if (!persist_profile(saved)) {
      return;
    }
    dialog.Dismiss();
    if (notify) {
      notify("已更新连接配置 " + saved.name);
    }
  };

  const auto save_and_connect = [dialog, persist_profile, active_session_id, app, workspace,
                                 tasks, notify] {
    ConnectionProfile saved;
    if (!persist_profile(saved)) {
      return;
    }
    if (active_session_id.has_value()) {
      CloseSession(app, workspace, tasks, *active_session_id, {});
    }
    ConnectionRequest request{
        .driver_path = saved.driver_path,
        .uri = saved.uri,
        .username = saved.username,
        .password = saved.password,
        .title = saved.name,
    };
    OpenConnection(app, workspace, tasks, std::move(request), notify);
    dialog.Dismiss();
  };

  const DatabaseBrand icon_brand = DetectDatabaseBrand(driver.Get().text, uri.Get().text);
  return DialogCard(Column{
      Row{
          DatabaseIcon(icon_brand, 20.0F),
          Text("编辑连接", TextRole::Title),
          Spacer(),
          ActionButton(Button("取消").OnClick([dialog] { dialog.Dismiss(); })),
          ActionButton(Button("保存").OnClick(save_only)),
          ActionButton(Button(active_session_id.has_value() ? "保存并重连" : "保存并连接")
                           .OnClick(save_and_connect),
                       ButtonTone::Primary),
      }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{8.0F}),
      TextField(name)
          .Label("显示名称（可选）")
          .Placeholder("留空则自动命名")
          .Variant(TextFieldVariant::Outlined)
          .LineLimits(TextFieldLineLimits::SingleLine())
          .OnChanged([name](const TextEditingValue& value) { name = value; }),
      MonospaceField(
          TextField(driver)
              .Label("ADBC 驱动（短名或路径）")
              .Placeholder("redis · postgresql · /path/to/libadbc_driver_*.so")
              .Variant(TextFieldVariant::Outlined)
              .LineLimits(TextFieldLineLimits::SingleLine())
              .OnChanged([driver](const TextEditingValue& value) { driver = value; })),
      MonospaceField(
          TextField(uri)
              .Label("连接 URI")
              .Placeholder("redis://127.0.0.1:6379/0 · postgresql://127.0.0.1:5432/postgres")
              .Variant(TextFieldVariant::Outlined)
              .LineLimits(TextFieldLineLimits::SingleLine())
              .OnChanged([uri](const TextEditingValue& value) { uri = value; })),
      TextField(username)
          .Label("用户名（可选）")
          .Variant(TextFieldVariant::Outlined)
          .LineLimits(TextFieldLineLimits::SingleLine())
          .OnChanged([username](const TextEditingValue& value) { username = value; }),
      Row{
          TextField(password)
              .Label("密码（可选，留空则无密码）")
              .Variant(TextFieldVariant::Outlined)
              .LineLimits(TextFieldLineLimits::SingleLine())
              .Secure(!show_password.Get())
              .OnChanged([password](const TextEditingValue& value) { password = value; })
              .OnSubmitted([save_and_connect] { save_and_connect(); })
              .With(Grow(1.0F)),
          ActionButton(Button(show_password.Get() ? "隐藏" : "显示")
                           .OnClick([show_password] { show_password = !show_password.Get(); })),
      }.With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Center}),
  }.With(Spacing{10.0F}, CrossAlign{CrossAxisAlignment::Stretch})
                    .With(Frame{.width = 540.0F}));
}

struct SidebarObjectGroup {
  std::string key;
  std::string label;
  std::vector<const DbObject*> items;
};

std::vector<SidebarObjectGroup> GroupSessionObjects(const DbSession& session) {
  if (session.dialect == Dialect::Redis) {
    SidebarObjectGroup dbs{.key = "redis_dbs", .label = "数据库", .items = {}};
    for (const DbObject& object : session.objects) {
      dbs.items.push_back(&object);
    }
    std::vector<SidebarObjectGroup> groups;
    if (!dbs.items.empty()) {
      groups.push_back(std::move(dbs));
    }
    return groups;
  }

  SidebarObjectGroup tables{.key = "tables", .label = "表", .items = {}};
  SidebarObjectGroup views{.key = "views", .label = "视图", .items = {}};
  SidebarObjectGroup mviews{.key = "mviews", .label = "物化视图", .items = {}};
  SidebarObjectGroup foreign{.key = "foreign", .label = "外部表", .items = {}};

  for (const DbObject& object : session.objects) {
    if (object.is_view) {
      if (ContainsIgnoreCaseAscii(object.table_type, "materialized")) {
        mviews.items.push_back(&object);
      } else {
        views.items.push_back(&object);
      }
    } else if (ContainsIgnoreCaseAscii(object.table_type, "foreign")) {
      foreign.items.push_back(&object);
    } else {
      tables.items.push_back(&object);
    }
  }

  std::vector<SidebarObjectGroup> groups;
  for (SidebarObjectGroup* group : {&tables, &views, &mviews, &foreign}) {
    if (!group->items.empty()) {
      groups.push_back(std::move(*group));
    }
  }
  return groups;
}

bool SessionHasMultipleSchemas(const DbSession& session) {
  std::string first_schema;
  for (const DbObject& object : session.objects) {
    if (object.db_schema.empty()) {
      continue;
    }
    if (first_schema.empty()) {
      first_schema = object.db_schema;
    } else if (object.db_schema != first_schema) {
      return true;
    }
  }
  return false;
}

std::string ObjectKindLabel(const DbObject& object) {
  if (ContainsIgnoreCaseAscii(object.table_type, "redis")) {
    return "Redis 数据库";
  }
  if (object.is_view) {
    return ContainsIgnoreCaseAscii(object.table_type, "materialized") ? "物化视图" : "视图";
  }
  return ContainsIgnoreCaseAscii(object.table_type, "foreign") ? "外部表" : "表";
}

[[huxerui::composable]] View ObjectSchemaDialog(DialogContext dialog,
                                                 std::shared_ptr<AdbcSession> adbc,
                                                 DbObject object, std::string display_label,
                                                 std::string qualified_name, NotifyFn notify) {
  const ThemeSpec& spec = UseTheme();
  const SemanticColors& semantic = UseEnvironment<SemanticColors>();
  auto loading = UseState(true);
  auto columns = UseState(std::vector<ColumnMeta>(object.columns));
  auto error = UseState(std::string{});
  auto dialog_tasks = UseTaskScope();

  Lifecycle([dialog_tasks, adbc, object, loading, columns, error, notify] {
    if (!adbc) {
      loading = false;
      error = "连接已关闭";
    } else {
      dialog_tasks.Launch([adbc, object, loading, columns, error, notify]() -> Task<void> {
        DbObject object_copy = object;
        AdbcSchemaResult res = co_await adbc->sequence.Run(
            [adbc, object_copy = std::move(object_copy)](std::stop_token) {
              return LoadSchemaBlocking(*adbc, object_copy);
            });
        loading = false;
        if (!res.error.empty()) {
          error = res.error;
          if (notify) {
            notify("读取结构失败: " + res.error);
          }
          if (!res.columns.empty()) {
            columns = std::move(res.columns);
          }
          co_return;
        }
        columns = std::move(res.columns);
      });
    }
    return [] {};
  });

  const std::vector<ColumnMeta>& cols = columns.Get();
  View body_content;
  if (loading.Get()) {
    body_content = MutedText("正在读取列结构…");
  } else if (cols.empty()) {
    body_content = MutedText(error.Get().empty() ? "未读取到列信息" : "读取列结构失败");
  } else {
    Views col_rows;
    col_rows.Reserve(cols.size());
    for (std::size_t idx = 0; idx < cols.size(); ++idx) {
      const ColumnMeta& col = cols[idx];
      col_rows.Add(
          Row{
              Text(col.name)
                  .Style(TextStyle{Font::System(font_size::kChip).WithWeight(FontWeight::SemiBold),
                                   spec.colors.on_surface})
                  .LineLimit(1)
                  .Overflow(TextOverflow::Ellipsis)
                  .With(Grow(1.0F)),
              MutedText(TypeLabel(col)),
          }
              .With(Padding{EdgeInsets::Symmetric(8.0F, 5.0F)}, Spacing{12.0F},
                    CrossAlign{CrossAxisAlignment::Center}, CornerRadius{5.0F},
                    Background{VisualFill{spec.colors.surface}},
                    Border{semantic.border_subtle, 1.0F})
              .Key("schema-col-" + std::to_string(idx)));
    }
    body_content = ScrollView(Column{std::move(col_rows)}
                                  .With(Spacing{4.0F}, CrossAlign{CrossAxisAlignment::Stretch}))
                       .With(Frame{.max_height = 360.0F});
  }

  return DialogCard(Column{
      Row{
          Text(display_label + " · 结构", TextRole::Title),
          Spacer(),
          ActionButton(Button("关闭").OnClick([dialog] { dialog.Dismiss(); })),
      }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{8.0F}),
      MutedText(ObjectKindLabel(object) + " · " + qualified_name + " · 共 " +
                std::to_string(cols.size()) + " 列"),
      std::move(body_content),
  }.With(Spacing{10.0F}, CrossAlign{CrossAxisAlignment::Stretch})
                    .With(Frame{.width = 560.0F}));
}

[[huxerui::composable]] View DropObjectDialog(
    DialogContext dialog, std::shared_ptr<AdbcSession> adbc, std::uint64_t session_id,
    DbObject object, std::string display_label, std::string drop_sql, State<AppModel> app,
    State<WorkspaceModel> workspace, TaskScope tasks, NotifyFn notify) {
  auto busy = UseState(false);
  const std::string kind = ObjectKindLabel(object);

  const auto confirm = [dialog, adbc, session_id, display_label, kind, drop_sql, app, workspace,
                        tasks, busy, notify] {
    if (busy.Get()) {
      return;
    }
    if (!adbc) {
      if (notify) {
        notify("连接已关闭");
      }
      dialog.Dismiss();
      return;
    }
    busy = true;
    tasks.Launch([dialog, adbc, session_id, display_label, kind, drop_sql, app, workspace, tasks,
                  busy, notify]() -> Task<void> {
      AdbcQueryResult res = co_await ExecuteQueryAsync(adbc, drop_sql, false, 0);
      if (!res.error.empty()) {
        busy = false;
        if (notify) {
          notify("删除失败: " + res.error);
        }
        co_return;
      }
      dialog.Dismiss();
      RefreshSessionObjects(app, workspace, tasks, session_id, notify, false);
      if (notify) {
        notify("已删除" + kind + " " + display_label);
      }
    });
  };

  return DialogCard(Column{
      Row{
          Text("删除" + kind, TextRole::Title),
          Spacer(),
          ActionButton(Button("取消")
                           .OnClick([dialog] { dialog.Dismiss(); })
                           .With(Enabled{!busy.Get()})),
          ActionButton(Button(busy.Get() ? "删除中…" : "确认删除")
                           .OnClick(confirm)
                           .With(Enabled{!busy.Get()}),
                       ButtonTone::DangerSolid),
      }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{8.0F}),
      MutedText("确定删除「" + display_label + "」？此操作不可撤销。"),
      MutedText("将执行：" + drop_sql),
  }.With(Spacing{10.0F}, CrossAlign{CrossAxisAlignment::Stretch})
                    .With(Frame{.width = 520.0F}));
}

[[huxerui::composable]] View DeleteSavedProfileDialog(
    DialogContext dialog, ConnectionProfile profile, std::string display_name,
    State<std::vector<ConnectionProfile>> profiles, std::string profiles_path, NotifyFn notify) {
  const auto config_db = UseService<ConfigDbService>();
  const auto confirm = [dialog, profile, display_name, profiles, profiles_path, config_db,
                        notify] {
    std::vector<ConnectionProfile> updated = profiles.Get();
    const auto it = std::ranges::find(updated, profile);
    if (it != updated.end()) {
      updated.erase(it);
    }
    if (!PersistConnectionProfileList(config_db, profiles_path, updated)) {
      if (notify) {
        notify("保存连接配置失败");
      }
      return;
    }
    profiles = std::move(updated);
    dialog.Dismiss();
    if (notify) {
      notify("已删除连接配置 " + display_name);
    }
  };

  return DialogCard(Column{
      Row{
          Text("删除已保存连接", TextRole::Title),
          Spacer(),
          ActionButton(Button("取消").OnClick([dialog] { dialog.Dismiss(); })),
          ActionButton(Button("删除").OnClick(confirm), ButtonTone::DangerSolid),
      }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{8.0F}),
      MutedText("将从已保存列表中移除「" + display_name + "」（" + profile.uri +
                "），不会删除数据库文件本身。"),
  }.With(Spacing{10.0F}, CrossAlign{CrossAxisAlignment::Stretch})
                    .With(Frame{.width = 480.0F}));
}

[[huxerui::composable]] View ConfirmCloseTabsDialog(DialogContext dialog, State<AppModel> app,
                                                     State<WorkspaceModel> workspace,
                                                     std::vector<std::uint64_t> tab_ids,
                                                     std::size_t draft_count) {
  return DialogCard(Column{
      Row{
          Text("放弃未保存的新增行？", TextRole::Title),
          Spacer(),
          ActionButton(Button("取消").OnClick([dialog] { dialog.Dismiss(); })),
          ActionButton(
              Button("放弃并关闭")
                  .OnClick([app, workspace, tab_ids = std::move(tab_ids), dialog] {
                    CloseWorkspaceTabs(app, workspace, tab_ids);
                    dialog.Dismiss();
                  }),
              ButtonTone::DangerSolid),
      }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{8.0F}),
      MutedText("即将关闭的标签页中共有 " + std::to_string(draft_count) +
                " 条未提交的新增行，关闭后这些空白编辑内容将会丢失。"),
  }.With(Spacing{10.0F}, CrossAlign{CrossAxisAlignment::Stretch})
                    .With(Frame{.width = 480.0F}));
}

struct ConnectionDragPayload {
  std::string key;
  std::string label;
};

struct SidebarConnectionEntry {
  std::string key;
  std::optional<ConnectionProfile> profile;
  const DbSession* session = nullptr;
};

} // namespace

// ---------------------------------------------------------------------------
// Sidebar
// ---------------------------------------------------------------------------

[[huxerui::composable]] View Sidebar(
    State<AppModel> app, State<WorkspaceModel> workspace,
    State<std::vector<ConnectionProfile>> profiles, std::string profiles_path, TaskScope tasks,
    NotifyFn notify, AppThemeConfig theme_config, bool system_dark,
    std::function<void(ConnectionProfile)> connect_profile) {
  const ThemeSpec& spec = UseTheme();
  const SemanticColors& semantic = UseEnvironment<SemanticColors>();
  const auto config_db = UseService<ConfigDbService>();
  const auto menu = UseMenu();
  const auto dialogs = UseDialog();
  const auto clipboard = UseApplication().Clipboard();
  const AppModel& model = app.Get();
  const WorkspaceTab* active_tab = workspace.Get().ActiveTab();
  auto collapsed_sessions = UseState(std::vector<std::uint64_t>{});
  auto collapsed_groups = UseState(std::vector<std::string>{});
  auto custom_order = UseState(std::vector<std::string>{});
  auto drop_hover_key = UseState(std::string{});

  const auto copy_text = [clipboard, notify](std::string text, std::string label) {
    if (clipboard && clipboard->SupportsSynchronousText() && clipboard->WriteText(text)) {
      if (notify) {
        notify("已复制" + label);
      }
    } else if (notify) {
      notify("当前环境不支持剪贴板");
    }
  };

  const auto& collapsed_session_ids = collapsed_sessions.Get();
  const auto& collapsed_group_keys = collapsed_groups.Get();
  const std::string& hovered_drop_key = drop_hover_key.Get();

  // Assemble saved profiles and live sessions in one unified order so
  // clicking a connection connects/expands in place without shifting rows.
  std::vector<SidebarConnectionEntry> candidates;
  const std::vector<ConnectionProfile>& saved_profiles = profiles.Get();
  std::vector<bool> claimed_sessions(model.sessions.size(), false);
  std::unordered_map<std::string, std::size_t> profile_key_counts;
  candidates.reserve(saved_profiles.size() + model.sessions.size());

  for (const ConnectionProfile& profile : saved_profiles) {
    const std::string base_key =
        "p:" + profile.name + "\t" + profile.driver_path + "\t" + profile.uri;
    const std::size_t occ = profile_key_counts[base_key]++;
    std::string entry_key = base_key;
    if (occ > 0) {
      entry_key += "#" + std::to_string(occ);
    }

    const DbSession* matched_session = nullptr;
    if (const auto match_idx = FindMatchingSessionIndex(profile, model.sessions, claimed_sessions)) {
      claimed_sessions[*match_idx] = true;
      matched_session = &model.sessions[*match_idx];
    }

    SidebarConnectionEntry entry;
    entry.key = std::move(entry_key);
    entry.profile = profile;
    entry.session = matched_session;
    candidates.push_back(std::move(entry));
  }

  for (std::size_t i = 0; i < model.sessions.size(); ++i) {
    if (claimed_sessions[i]) {
      continue;
    }
    const DbSession& session = model.sessions[i];
    SidebarConnectionEntry entry;
    entry.key = "s:" + std::to_string(session.id);
    entry.session = &session;
    candidates.push_back(std::move(entry));
  }

  std::vector<std::string> natural_keys;
  natural_keys.reserve(candidates.size());
  for (const SidebarConnectionEntry& entry : candidates) {
    natural_keys.push_back(entry.key);
  }
  const std::vector<std::string> ordered_keys =
      OrderConnectionKeys(natural_keys, custom_order.Get());

  std::vector<SidebarConnectionEntry> ordered_entries;
  ordered_entries.reserve(ordered_keys.size());
  for (const std::string& key : ordered_keys) {
    for (const SidebarConnectionEntry& entry : candidates) {
      if (entry.key == key) {
        ordered_entries.push_back(entry);
        break;
      }
    }
  }

  const auto reorder_connections = [profiles, profiles_path, config_db, custom_order, notify,
                                    natural_keys, candidates](std::string from_key,
                                                              std::string to_key) {
    const std::vector<std::string> next_order =
        ReorderConnectionKeys(natural_keys, custom_order.Get(), from_key, to_key);
    custom_order = next_order;

    std::vector<ConnectionProfile> reordered_profiles;
    reordered_profiles.reserve(profiles.Get().size());
    for (const std::string& key : next_order) {
      for (const SidebarConnectionEntry& entry : candidates) {
        if (entry.key == key && entry.profile.has_value()) {
          reordered_profiles.push_back(*entry.profile);
          break;
        }
      }
    }
    if (reordered_profiles != profiles.Get()) {
      profiles = reordered_profiles;
      if (!PersistConnectionProfileList(config_db, profiles_path, reordered_profiles) && notify) {
        notify("保存连接顺序失败");
      }
    }
  };

  const auto make_drag_preview = [spec, semantic](const std::string& label) {
    return [label, spec, semantic] {
      return Text(label)
          .Style(TextStyle{Font::System(font_size::kChip).WithWeight(FontWeight::SemiBold),
                           spec.colors.on_surface})
          .LineLimit(1)
          .Overflow(TextOverflow::Ellipsis)
          .With(Padding{EdgeInsets::Symmetric(10.0F, 5.0F)},
                Background{VisualFill{spec.colors.surface_container_highest}},
                Border{semantic.border_strong, 1.0F}, CornerRadius{6.0F});
    };
  };

  Views tree_rows;
  for (const SidebarConnectionEntry& entry : ordered_entries) {
    const std::string& entry_key = entry.key;
    const bool is_drop_hover = hovered_drop_key == entry_key;
    const Color drop_border = is_drop_hover ? spec.colors.primary : Color::Transparent();

    if (entry.session == nullptr) {
      const ConnectionProfile& profile = *entry.profile;
      const std::string display_name =
          profile.name.empty() ? UriDisplayName(profile.uri) : profile.name;
      const auto open_edit_profile_dialog = [dialogs, profile, app, workspace, profiles,
                                             profiles_path, tasks, notify, theme_config,
                                             system_dark] {
        dialogs.Show([profile, app, workspace, profiles, profiles_path, tasks, notify,
                      theme_config, system_dark](DialogContext dialog) {
          return WithDialogTheme(
              theme_config, system_dark,
              EditConnectionProfileDialog(dialog, profile, profile, std::nullopt, app, workspace,
                                          profiles, profiles_path, tasks, notify));
        });
      };
      const auto open_delete_profile_dialog = [dialogs, profile, display_name, profiles,
                                               profiles_path, notify, theme_config, system_dark] {
        dialogs.Show([profile, display_name, profiles, profiles_path, notify, theme_config,
                      system_dark](DialogContext dialog) {
          return WithDialogTheme(theme_config, system_dark,
                                 DeleteSavedProfileDialog(dialog, profile, display_name, profiles,
                                                          profiles_path, notify));
        });
      };

      View unopened_row =
          TreeRow(TreeRowOptions{
                      .prefix = "▸",
                      .label = display_name,
                      .trailing_text = "未连接",
                      .indent_level = 0,
                      .selected = false,
                      .emphasize = false,
                      .active_hint = false,
                      .muted = true,
                      .prefix_color = std::nullopt,
                      .status_dot = WithAlpha(spec.colors.on_surface_variant, 0.45F),
                      .leading_icon =
                          DatabaseIcon(DetectDatabaseBrand(profile.driver_path, profile.uri), 15.0F),
                  },
                  [connect_profile, profile] { connect_profile(profile); })
              .With(Grow(1.0F),
                    DragSource(ConnectionDragPayload{entry_key, display_name},
                               make_drag_preview(display_name),
                               DragGesture{.minimum_distance = 6.0F}))
              .On<DragSourceEvents::Ended>(
                  [drop_hover_key](const DragDropResult&) { drop_hover_key = std::string{}; })
              .On<DragSourceEvents::Canceled>(
                  [drop_hover_key](const DragEvent&) { drop_hover_key = std::string{}; })
              .On<ViewEvents::ContextMenuRequested>(
                  [menu, connect_profile, profile, copy_text, open_edit_profile_dialog,
                   open_delete_profile_dialog](Point position) {
                    menu.ShowAt(position, {
                        MenuItem("连接", [connect_profile, profile] { connect_profile(profile); }),
                        MenuItem("编辑连接…", open_edit_profile_dialog),
                        MenuSection{},
                        MenuItem("复制连接 URI",
                                 [copy_text, uri = profile.uri] { copy_text(uri, "连接 URI"); }),
                        MenuSection{},
                        MenuItem("删除已保存连接…", open_delete_profile_dialog),
                    });
                  });

      tree_rows.Add(
          Row{std::move(unopened_row)}
              .With(CornerRadius{5.0F}, Border{drop_border, 2.0F},
                    DropTarget::Accepts<ConnectionDragPayload>(
                        [entry_key](const ConnectionDragPayload& payload) {
                          return payload.key != entry_key;
                        }))
              .On<DropEvents<ConnectionDragPayload>::Entered>(
                  [drop_hover_key, entry_key](const ConnectionDragPayload&, const DropEvent&) {
                    drop_hover_key = entry_key;
                  })
              .On<DropEvents<ConnectionDragPayload>::Exited>(
                  [drop_hover_key, entry_key](const ConnectionDragPayload&, const DropEvent&) {
                    if (drop_hover_key.Get() == entry_key) {
                      drop_hover_key = std::string{};
                    }
                  })
              .On<DropEvents<ConnectionDragPayload>::Dropped>(
                  [drop_hover_key, reorder_connections, entry_key](
                      const ConnectionDragPayload& payload, const DropEvent&) {
                    drop_hover_key = std::string{};
                    reorder_connections(payload.key, entry_key);
                  })
              .Key("conn-" + entry_key));
      continue;
    }

    const DbSession& session = *entry.session;
    const std::uint64_t session_id = session.id;
    const bool is_active = session_id == model.active_session;
    const bool is_expanded =
        std::ranges::find(collapsed_session_ids, session_id) == collapsed_session_ids.end();
    const bool session_has_selected_child =
        active_tab != nullptr && active_tab->kind == WorkspaceTabKind::Table &&
        active_tab->session_id == session_id;
    const bool highlight_session =
        is_active && (!is_expanded || !session_has_selected_child);
    const bool active_parent_hint = is_expanded && session_has_selected_child;

    const auto toggle_session = [app, session_id, is_expanded, is_active, collapsed_sessions] {
      if (is_expanded) {
        collapsed_sessions.Update([session_id](std::vector<std::uint64_t>& ids) {
          if (std::ranges::find(ids, session_id) == ids.end()) {
            ids.push_back(session_id);
          }
        });
        return;
      }
      collapsed_sessions.Update(
          [session_id](std::vector<std::uint64_t>& ids) { std::erase(ids, session_id); });
      if (!is_active) {
        app.Update([session_id](AppModel& value) { value.active_session = session_id; });
      }
    };

    const auto refresh_session = [app, workspace, tasks, notify, session_id] {
      RefreshSessionObjects(app, workspace, tasks, session_id, notify, true);
    };

    const auto disconnect_session = [app, workspace, tasks, notify, session_id,
                                     collapsed_sessions, collapsed_groups] {
      collapsed_sessions.Update([session_id](std::vector<std::uint64_t>& ids) {
        std::erase(ids, session_id);
      });
      const std::string group_prefix = std::to_string(session_id) + ":";
      collapsed_groups.Update([&group_prefix](std::vector<std::string>& keys) {
        std::erase_if(keys, [&group_prefix](const std::string& key) {
          return key.starts_with(group_prefix);
        });
      });
      CloseSession(app, workspace, tasks, session_id, notify);
    };

    View close = Text("×")
                     .Style(TextStyle{Font::System(14.0F), spec.colors.on_surface_variant})
                     .With(Padding{EdgeInsets::Symmetric(6.0F, 3.0F)}, CornerRadius{4.0F})
                     .OnClick(disconnect_session);

    View connection_row =
        TreeRow(
            TreeRowOptions{
                .prefix = is_expanded ? "▾" : "▸",
                .label = session.name,
                .trailing_text = std::to_string(session.objects.size()),
                .indent_level = 0,
                .selected = highlight_session,
                .emphasize = true,
                .active_hint = active_parent_hint,
                .muted = false,
                .prefix_color = std::nullopt,
                .status_dot = semantic.success,
                .leading_icon = DatabaseIcon(
                    DetectDatabaseBrand(session.driver_path, session.uri, session.dialect), 15.0F),
            },
            toggle_session)
            .With(Grow(1.0F),
                  DragSource(ConnectionDragPayload{entry_key, session.name},
                             make_drag_preview(session.name),
                             DragGesture{.minimum_distance = 6.0F}))
            .On<DragSourceEvents::Ended>(
                [drop_hover_key](const DragDropResult&) { drop_hover_key = std::string{}; })
            .On<DragSourceEvents::Canceled>(
                [drop_hover_key](const DragEvent&) { drop_hover_key = std::string{}; });

    const ConnectionProfile edit_initial = entry.profile.value_or(ConnectionProfile{
        .name = session.name,
        .driver_path = session.driver_path,
        .uri = session.uri,
        .username = {},
        .password = {},
    });
    const std::optional<ConnectionProfile> edit_original = entry.profile;
    const auto open_edit_session_dialog = [dialogs, edit_original, edit_initial, session_id, app,
                                           workspace, profiles, profiles_path, tasks, notify,
                                           theme_config, system_dark] {
      dialogs.Show([edit_original, edit_initial, session_id, app, workspace, profiles,
                    profiles_path, tasks, notify, theme_config,
                    system_dark](DialogContext dialog) {
        return WithDialogTheme(
            theme_config, system_dark,
            EditConnectionProfileDialog(dialog, edit_original, edit_initial, session_id, app,
                                        workspace, profiles, profiles_path, tasks, notify));
      });
    };

    tree_rows.Add(
        Row{std::move(connection_row), std::move(close)}
            .With(Spacing{2.0F}, CrossAlign{CrossAxisAlignment::Center}, CornerRadius{5.0F},
                  Border{drop_border, 2.0F},
                  DropTarget::Accepts<ConnectionDragPayload>(
                      [entry_key](const ConnectionDragPayload& payload) {
                        return payload.key != entry_key;
                      }))
            .On<DropEvents<ConnectionDragPayload>::Entered>(
                [drop_hover_key, entry_key](const ConnectionDragPayload&, const DropEvent&) {
                  drop_hover_key = entry_key;
                })
            .On<DropEvents<ConnectionDragPayload>::Exited>(
                [drop_hover_key, entry_key](const ConnectionDragPayload&, const DropEvent&) {
                  if (drop_hover_key.Get() == entry_key) {
                    drop_hover_key = std::string{};
                  }
                })
            .On<DropEvents<ConnectionDragPayload>::Dropped>(
                [drop_hover_key, reorder_connections, entry_key](
                    const ConnectionDragPayload& payload, const DropEvent&) {
                  drop_hover_key = std::string{};
                  reorder_connections(payload.key, entry_key);
                })
            .On<ViewEvents::ContextMenuRequested>(
                [menu, is_expanded, toggle_session, refresh_session, app, workspace, copy_text,
                 open_edit_session_dialog, disconnect_session, session_id, uri = session.uri,
                 session_name = session.name, is_redis = (session.dialect == Dialect::Redis),
                 notify](Point position) {
                  menu.ShowAt(position, {
                      MenuItem(is_expanded ? (is_redis ? "收起数据库" : "收起对象")
                                           : (is_redis ? "展开数据库" : "展开对象"),
                               toggle_session),
                      MenuItem(is_redis ? "刷新数据库列表" : "刷新对象列表", refresh_session),
                      MenuItem(is_redis ? "新建命令台" : "新建查询",
                               [app, workspace, session_id] {
                                 (void)OpenSqlTab(app, workspace, session_id);
                               }),
                      MenuSection{},
                      MenuItem("编辑连接…", open_edit_session_dialog),
                      MenuItem("复制连接 URI",
                               [copy_text, uri] { copy_text(uri, "连接 URI"); }),
                      MenuSection{},
                      MenuItem("断开连接",
                               [disconnect_session, session_name, notify] {
                                 disconnect_session();
                                 if (notify) {
                                   notify("已断开 " + session_name);
                                 }
                               }),
                  });
                })
            .Key("conn-" + entry_key));

    if (!is_expanded) {
      continue;
    }

    if (session.objects.empty()) {
      tree_rows.Add(TreeRow(TreeRowOptions{
                                .prefix = {},
                                .label = session.dialect == Dialect::Redis ? "暂无可用数据库"
                                                                           : "暂无表或视图",
                                .trailing_text = {},
                                .indent_level = 1,
                                .selected = false,
                                .emphasize = false,
                                .active_hint = false,
                                .muted = true,
                            })
                        .Key("empty-" + std::to_string(session_id)));
      continue;
    }

    const bool multi_schema = SessionHasMultipleSchemas(session);
    const std::vector<SidebarObjectGroup> groups = GroupSessionObjects(session);
    for (const SidebarObjectGroup& group : groups) {
      const std::string group_id = std::to_string(session_id) + ":" + group.key;
      const bool group_open =
          std::ranges::find(collapsed_group_keys, group_id) == collapsed_group_keys.end();

      const auto toggle_group = [collapsed_groups, group_id] {
        collapsed_groups.Update([&group_id](std::vector<std::string>& keys) {
          const auto it = std::ranges::find(keys, group_id);
          if (it == keys.end()) {
            keys.push_back(group_id);
          } else {
            keys.erase(it);
          }
        });
      };

      const bool is_view_group = group.key == "views" || group.key == "mviews";
      const bool is_foreign_group = group.key == "foreign";
      const std::string item_icon = is_view_group ? "◈" : (is_foreign_group ? "◇" : "⊞");
      const Color group_accent =
          is_view_group ? semantic.accent_view
                        : (is_foreign_group ? semantic.accent_external : semantic.accent_table);

      tree_rows.Add(
          TreeRow(TreeRowOptions{
                      .prefix = group_open ? "▾" : "▸",
                      .label = group.label,
                      .trailing_text = std::to_string(group.items.size()),
                      .indent_level = 1,
                      .selected = false,
                      .emphasize = true,
                      .active_hint = false,
                      .muted = false,
                      .prefix_color = group_accent,
                      .status_dot = std::nullopt,
                  },
                  toggle_group)
              .On<ViewEvents::ContextMenuRequested>(
                  [menu, group_open, toggle_group, refresh_session, app, workspace,
                   session_id](Point position) {
                    menu.ShowAt(position, {
                        MenuItem(group_open ? "收起分组" : "展开分组", toggle_group),
                        MenuItem("刷新对象列表", refresh_session),
                        MenuItem("新建查询",
                                 [app, workspace, session_id] {
                                   (void)OpenSqlTab(app, workspace, session_id);
                                 }),
                    });
                  })
              .Key("group-" + group_id));

      if (!group_open) {
        continue;
      }

      for (std::size_t idx = 0; idx < group.items.size(); ++idx) {
        const DbObject& object = *group.items[idx];
        const bool is_redis = session.dialect == Dialect::Redis;
        const bool is_selected =
            active_tab != nullptr && active_tab->kind == WorkspaceTabKind::Table &&
            active_tab->session_id == session_id && active_tab->object_name == object.name &&
            active_tab->object_schema == object.db_schema &&
            active_tab->is_view == object.is_view;
        const std::string display_label =
            (multi_schema && !object.db_schema.empty())
                ? (object.db_schema + "." + object.name)
                : object.name;
        const std::string name = object.name;
        const std::string db_schema = object.db_schema;
        const bool is_view = object.is_view;
        const std::string qualified =
            is_redis ? object.name : QualifiedName(session.dialect, object);
        const std::string select_sql = "SELECT * FROM " + qualified + " LIMIT 100;";
        const std::string drop_sql = BuildDropObjectSql(session.dialect, object);
        const std::string kind_label = ObjectKindLabel(object);
        const std::string row_trailing =
            (is_redis && object.item_count.has_value())
                ? std::to_string(*object.item_count)
                : std::string{};
        std::string redis_db_index = "0";
        if (is_redis && object.name.size() > 2 &&
            (object.name.starts_with("db") || object.name.starts_with("DB"))) {
          redis_db_index = object.name.substr(2);
        }

        const auto open_object = [app, workspace, tasks, notify, session_id, name, db_schema,
                                  is_view] {
          SelectObject(app, workspace, tasks, session_id, name, db_schema, is_view, notify);
        };

        const auto open_schema_dialog = [dialogs, adbc = session.session, object, display_label,
                                         qualified, notify, theme_config, system_dark] {
          if (!adbc) {
            if (notify) {
              notify("连接已关闭");
            }
            return;
          }
          dialogs.Show([adbc, object, display_label, qualified, notify, theme_config,
                        system_dark](DialogContext dialog) {
            return WithDialogTheme(
                theme_config, system_dark,
                ObjectSchemaDialog(dialog, adbc, object, display_label, qualified, notify));
          });
        };

        const auto open_drop_dialog = [dialogs, adbc = session.session, session_id, object,
                                       display_label, drop_sql, app, workspace, tasks, notify,
                                       theme_config, system_dark] {
          if (!adbc) {
            if (notify) {
              notify("连接已关闭");
            }
            return;
          }
          dialogs.Show([adbc, session_id, object, display_label, drop_sql, app, workspace, tasks,
                        notify, theme_config, system_dark](DialogContext dialog) {
            return WithDialogTheme(theme_config, system_dark,
                                   DropObjectDialog(dialog, adbc, session_id, object, display_label,
                                                    drop_sql, app, workspace, tasks, notify));
          });
        };

        tree_rows.Add(
            TreeRow(TreeRowOptions{
                        .prefix = item_icon,
                        .label = display_label,
                        .trailing_text = row_trailing,
                        .indent_level = 2,
                        .selected = is_selected,
                        .emphasize = false,
                        .active_hint = false,
                        .muted = false,
                        .prefix_color = group_accent,
                        .status_dot = std::nullopt,
                    },
                    open_object)
                .On<ViewEvents::ContextMenuRequested>(
                    [menu, open_object, app, workspace, open_schema_dialog, copy_text,
                     refresh_session, open_drop_dialog, session_id, select_sql, display_label,
                     kind_label, is_redis, redis_db_index](Point position) {
                      if (is_redis) {
                        menu.ShowAt(position, {
                            MenuItem("打开浏览", open_object),
                            MenuItem("在命令台中操作",
                                     [app, workspace, session_id, redis_db_index, display_label] {
                                       (void)OpenSqlTab(app, workspace, session_id,
                                                        "SELECT " + redis_db_index,
                                                        "Redis · " + display_label);
                                     }),
                            MenuItem("查看字段结构", open_schema_dialog),
                            MenuSection{},
                            MenuItem("复制名称",
                                     [copy_text, display_label] {
                                       copy_text(display_label, "对象名称");
                                     }),
                            MenuSection{},
                            MenuItem("刷新数据库列表", refresh_session),
                        });
                        return;
                      }
                      menu.ShowAt(position, {
                          MenuItem("打开浏览", open_object),
                          MenuItem("在 SQL 编辑器中查询",
                                   [app, workspace, session_id, select_sql, display_label] {
                                     (void)OpenSqlTab(app, workspace, session_id, select_sql,
                                                      "SQL · " + display_label);
                                   }),
                          MenuItem("查看结构", open_schema_dialog),
                          MenuSection{},
                          MenuItem("复制名称",
                                   [copy_text, display_label] {
                                     copy_text(display_label, "对象名称");
                                   }),
                          MenuItem("复制 SELECT 语句",
                                   [copy_text, select_sql] {
                                     copy_text(select_sql, "SELECT 语句");
                                   }),
                          MenuSection{},
                          MenuItem("刷新对象列表", refresh_session),
                          MenuItem("删除" + kind_label + "…", open_drop_dialog),
                      });
                    })
                .Key("obj-" + std::to_string(session_id) + "-" + group.key + "-" +
                     object.db_schema + "-" + name + "-" + std::to_string(idx)));
      }
    }
  }

  const std::size_t total_connections = ordered_entries.size();
  View tree_view;
  if (tree_rows.Size() == 0) {
    tree_view = MutedText("暂无数据库连接，点击左上角「新建连接」").With(Grow(1.0F));
  } else {
    tree_view = ScrollView(Column{std::move(tree_rows)}
                               .With(Spacing{1.0F}, CrossAlign{CrossAxisAlignment::Stretch}))
                    .With(Grow(1.0F));
  }

  return Column{
      Row{
          Caption("数据库连接"),
          Spacer(),
          MutedText(std::to_string(total_connections)),
      }.With(CrossAlign{CrossAxisAlignment::Center}),
      std::move(tree_view),
  }.With(Frame{.width = 260.0F}, Padding{EdgeInsets::Symmetric(10.0F, 10.0F)},
         Spacing{6.0F}, CrossAlign{CrossAxisAlignment::Stretch},
         Background{VisualFill{spec.colors.surface_container_low}},
         Border{semantic.border_subtle, 1.0F});
}

// ---------------------------------------------------------------------------
// Table browser
// ---------------------------------------------------------------------------

[[huxerui::composable]] View DeleteRowDialog(DialogContext dialog, State<AppModel> app,
                                             State<WorkspaceModel> workspace,
                                             std::uint64_t tab_id, TaskScope tasks,
                                             NotifyFn notify, bool is_redis) {
  return DialogCard(Column{
      Row{
          Text(is_redis ? "删除键" : "删除行", TextRole::Title),
          Spacer(),
          ActionButton(Button("取消").OnClick([dialog] { dialog.Dismiss(); })),
          ActionButton(
              Button("删除").OnClick([app, workspace, tab_id, tasks, notify, dialog] {
                RequestDeleteSelected(app, workspace, tasks, tab_id, notify);
                dialog.Dismiss();
              }),
              ButtonTone::DangerSolid),
      }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{8.0F}),
      Text(is_redis ? "选中的键会立即从 Redis 中删除，此操作不可撤销。"
                    : "选中的行会立即从数据库中删除，此操作不可撤销。"),
  }.With(Spacing{12.0F}, CrossAlign{CrossAxisAlignment::Stretch})
                    .With(Frame{.width = 520.0F}));
}

[[huxerui::composable]] View AdbcManagerDialog(DialogContext dialog,
                                                   AdbcManagerContext context) {
  const auto config_db = UseService<ConfigDbService>();
  auto profiles = context.profiles;
  auto dialog_tasks = UseTaskScope();
  auto tab = UseState(context.initial_tab);
  auto drivers = UseState(std::vector<AdbcDriverInfo>{});
  auto user_installed = UseState(std::vector<std::string>{});
  auto catalog = UseState(std::vector<RegistryDriverInfo>{});
  auto catalog_loaded = UseState(false);
  auto catalog_loading = UseState(false);
  auto catalog_failed = UseState(false);
  auto registry_filter = UseState(TextEditingValue::FromText(context.initial_filter));
  auto installing_slug = UseState(std::string{});
  auto install_status = UseState(std::string{});
  auto selected = UseState<int>(-1);
  auto driver = UseState(TextEditingValue::FromText(context.default_driver));
  auto uri = UseState(TextEditingValue::FromText(context.default_uri));
  auto username = UseState(TextEditingValue::FromText({}));
  auto password = UseState(TextEditingValue::FromText({}));
  auto name = UseState(TextEditingValue::FromText({}));

  const auto refresh_drivers = [drivers, user_installed]() -> std::size_t {
    std::vector<AdbcDriverInfo> list = DiscoverAdbcDrivers();
    std::vector<std::string> user_slugs;
    for (const AdbcDriverInfo& info : list) {
      if (info.is_manifest && IsUserInstalledAdbcDriver(info.path)) {
        user_slugs.push_back(info.path);
      }
    }
    const std::size_t count = list.size();
    drivers = std::move(list);
    user_installed = std::move(user_slugs);
    return count;
  };

  const auto load_catalog = [context, dialog_tasks, catalog, catalog_loaded, catalog_loading,
                             catalog_failed]() {
    if (catalog_loading.Get()) {
      return;
    }
    if (!context.http) {
      catalog_failed = true;
      if (context.notify) {
        context.notify("HTTP 客户端不可用");
      }
      return;
    }
    catalog_loading = true;
    catalog_failed = false;
    dialog_tasks.Launch([http = context.http, notify = context.notify, catalog, catalog_loaded,
                         catalog_loading, catalog_failed]() -> Task<void> {
      RegistryCatalogResult res = co_await FetchRegistryCatalogAsync(http);
      catalog_loading = false;
      catalog_loaded = true;
      if (!res.error.empty()) {
        catalog_failed = true;
        if (notify) {
          notify(res.error);
        }
        co_return;
      }
      catalog_failed = false;
      catalog = std::move(res.drivers);
    });
  };

  Lifecycle([refresh_drivers, load_catalog, initial_tab = context.initial_tab] {
    (void)refresh_drivers();
    if (initial_tab == 1) {
      load_catalog();
    }
    return [] {};
  });

  const auto switch_tab = [tab, catalog_loaded, catalog_loading,
                           load_catalog](std::size_t index) {
    tab = index;
    if (index == 1 && !catalog_loaded.Get() && !catalog_loading.Get()) {
      load_catalog();
    }
  };

  const auto start_install = [context, dialog_tasks, driver, installing_slug,
                              install_status, refresh_drivers](RegistryDriverInfo item) {
    if (!installing_slug.Get().empty()) {
      return;
    }
    if (!context.http) {
      if (context.notify) {
        context.notify("HTTP 客户端不可用");
      }
      return;
    }
    installing_slug = item.slug;
    install_status = "正在准备下载 " + item.slug + " …";
    dialog_tasks.Launch([context, item = std::move(item), driver, installing_slug,
                         install_status, refresh_drivers]() -> Task<void> {
      auto on_progress = [install_status](std::string msg) { install_status = std::move(msg); };
      DriverInstallResult res =
          co_await InstallRegistryDriverAsync(context.http, item, std::move(on_progress));
      installing_slug = {};
      install_status = {};
      if (!res.ok) {
        if (context.notify) {
          context.notify(res.message);
        }
        co_return;
      }
      (void)refresh_drivers();
      driver = TextEditingValue::FromText(res.slug);
      if (context.notify) {
        context.notify(res.message);
      }
    });
  };

  const auto start_uninstall = [context, dialog_tasks, installing_slug,
                                install_status, refresh_drivers](std::string slug) {
    if (!installing_slug.Get().empty()) {
      return;
    }
    installing_slug = slug;
    install_status = "正在卸载 " + slug + " …";
    dialog_tasks.Launch([context, slug = std::move(slug), installing_slug,
                         install_status, refresh_drivers]() -> Task<void> {
      auto uninstall_job = [slug]() { return UninstallUserAdbcDriverBlocking(slug); };
      DriverInstallResult res = co_await RunWorker(std::move(uninstall_job));
      installing_slug = {};
      install_status = {};
      if (!res.ok) {
        if (context.notify) {
          context.notify(res.message);
        }
        co_return;
      }
      (void)refresh_drivers();
      if (context.notify) {
        context.notify(res.message);
      }
    });
  };

  const auto save_profile = [context, config_db, profiles, selected, driver, uri, username,
                             password, name]() -> bool {
    const std::string driver_path = driver.Get().text;
    const std::string uri_text = uri.Get().text;
    if (driver_path.empty() || uri_text.empty()) {
      if (context.notify) {
        context.notify("ADBC driver 名称/路径和 URI 都不能为空");
      }
      return false;
    }
    const SanitizedUriResult sanitized = SanitizeUriCredentials(uri_text);
    const std::string effective_user =
        username.Get().text.empty() ? sanitized.username : username.Get().text;
    const std::string effective_password =
        password.Get().text.empty() ? sanitized.password : password.Get().text;
    ConnectionProfile profile{
        .name = name.Get().text.empty() ? UriDisplayName(sanitized.uri) : name.Get().text,
        .driver_path = driver_path,
        .uri = sanitized.uri,
        .username = effective_user,
        .password = effective_password,
    };
    std::vector<ConnectionProfile> list = profiles.Get();
    int index = selected.Get();
    if (index >= 0 && static_cast<std::size_t>(index) < list.size()) {
      list[static_cast<std::size_t>(index)] = profile;
    } else {
      list.push_back(profile);
      index = static_cast<int>(list.size()) - 1;
    }
    if (!PersistConnectionProfileList(config_db, context.profiles_path, list)) {
      if (context.notify) {
        context.notify("保存连接配置失败: " + context.profiles_path);
      }
      return false;
    }
    profiles = std::move(list);
    selected = index;
    name = TextEditingValue::FromText(profile.name);
    if (context.notify) {
      context.notify("已保存 " + profile.name);
    }
    return true;
  };

  const auto delete_profile = [context, config_db, profiles, selected, name] {
    const int index = selected.Get();
    std::vector<ConnectionProfile> list = profiles.Get();
    if (index < 0 || static_cast<std::size_t>(index) >= list.size()) {
      return;
    }
    const auto position = list.begin() + index;
    const std::string removed = position->name.empty() ? position->uri : position->name;
    list.erase(position);
    if (!PersistConnectionProfileList(config_db, context.profiles_path, list)) {
      if (context.notify) {
        context.notify("删除后保存失败: " + context.profiles_path);
      }
      return;
    }
    profiles = std::move(list);
    selected = -1;
    name = TextEditingValue::FromText({});
    if (context.notify) {
      context.notify("已删除 " + removed);
    }
  };

  const auto connect = [context, dialog, save_profile, driver, uri, username, password, name] {
    if (!save_profile()) {
      return;
    }
    const SanitizedUriResult sanitized = SanitizeUriCredentials(uri.Get().text);
    ConnectionRequest request{
        .driver_path = driver.Get().text,
        .uri = sanitized.uri,
        .username = username.Get().text.empty() ? sanitized.username : username.Get().text,
        .password = password.Get().text.empty() ? sanitized.password : password.Get().text,
        .title = name.Get().text.empty() ? UriDisplayName(sanitized.uri) : name.Get().text,
    };
    OpenConnection(context.app, context.workspace, context.tasks, std::move(request),
                   context.notify);
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
                [profiles, selected, driver, uri, username, password, name,
                 index] {
                  const auto& list = profiles.Get();
                  if (index >= list.size()) {
                    return;
                  }
                  const ConnectionProfile& value = list[index];
                  driver = TextEditingValue::FromText(value.driver_path);
                  uri = TextEditingValue::FromText(value.uri);
                  username = TextEditingValue::FromText(value.username);
                  password = TextEditingValue::FromText(value.password);
                  name = TextEditingValue::FromText(value.name);
                  selected = static_cast<int>(index);
                },
                {}, std::nullopt,
                DatabaseIcon(DetectDatabaseBrand(profile.driver_path, profile.uri), 18.0F))
            .Key("profile-" + std::to_string(index) + "-" + profile.uri));
  }

  Views driver_rows;
  const auto& driver_list = drivers.Get();
  driver_rows.Reserve(driver_list.size());
  for (const AdbcDriverInfo& info : driver_list) {
    const std::string& detail = info.detail.empty() ? info.path : info.detail;
    driver_rows.Add(
        NavItem(info.name, detail, driver.Get().text == info.path,
                [driver, info] { driver = TextEditingValue::FromText(info.path); }, {},
                std::nullopt, DatabaseIcon(DetectDatabaseBrand(info.path + " " + info.name), 18.0F))
            .Key("driver-" + info.path + "-" + detail));
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
          ActionButton(Button("安装驱动…").OnClick([switch_tab]() mutable {
            switch_tab(1);
          })),
          ActionButton(Button("刷新").OnClick([refresh_drivers, notify = context.notify] {
            const std::size_t count = refresh_drivers();
            if (notify) {
              notify("发现 " + std::to_string(count) + " 个驱动");
            }
          })),
      }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{6.0F}),
      driver_rows.Size() == 0
          ? View(MutedText("未发现 ADBC 驱动"))
          : View(ScrollView(Column{std::move(driver_rows)}.With(Spacing{2.0F}))
                     .With(Frame{.height = 140.0F})),
      TextField(driver)
          .Label("ADBC Driver（名称或路径）")
          .Placeholder("postgresql  ·  /path/to/libadbc_driver_postgresql.so")
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
              .Label("密码（可选）")
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
  }.With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch}, Grow(1.0F));

  View manage_tab = Column{
      MutedText("连接配置（默认保留可选密码）保存在 dbpp.sqlite3；自动发现标准 ADBC manifest "
                "目录、ADBC_DRIVER_PATH、DBPP_ADBC_DRIVERS 与系统库目录，支持短名、.toml "
                "与动态库路径。"),
      Row{
          std::move(saved_panel),
          Divider(Axis::Vertical),
          std::move(form_panel),
      }.With(Grow(1.0F), Spacing{12.0F}, CrossAlign{CrossAxisAlignment::Stretch}),
  }.With(Spacing{10.0F}, CrossAlign{CrossAxisAlignment::Stretch}, Grow(1.0F));

  const auto& user_slugs = user_installed.Get();
  const auto& catalog_list = catalog.Get();
  const std::string filter_query = registry_filter.Get().text;
  const std::string active_install = installing_slug.Get();
  const bool busy = !active_install.empty();

  Views registry_rows;
  registry_rows.Reserve(catalog_list.size());
  for (const RegistryDriverInfo& item : catalog_list) {
    if (!filter_query.empty() &&
        !ContainsIgnoreCaseAscii(item.slug, filter_query) &&
        !ContainsIgnoreCaseAscii(item.name, filter_query) &&
        !ContainsIgnoreCaseAscii(item.description, filter_query)) {
      continue;
    }

    bool has_manifest = false;
    std::string installed_version;
    for (const AdbcDriverInfo& local : driver_list) {
      if (local.is_manifest && local.path == item.slug) {
        has_manifest = true;
        installed_version = local.version;
        break;
      }
    }
    const bool user_is_installed =
        std::ranges::find(user_slugs, item.slug) != user_slugs.end();
    const bool same_version =
        !installed_version.empty() &&
        StripLeadingV(installed_version) == StripLeadingV(item.version);

    std::string detail;
    if (user_is_installed) {
      detail = "已安装" +
               (installed_version.empty() ? std::string{} : " " + installed_version) +
               " · ";
    } else if (has_manifest) {
      detail = "已有外部配置" +
               (installed_version.empty() ? std::string{} : " " + installed_version) +
               " · ";
    }
    detail += "可用 " + item.version;
    if (!item.license.empty()) {
      detail += " · " + item.license;
    }
    if (!item.description.empty()) {
      detail += " — " + item.description;
    }

    View trailing;
    if (active_install == item.slug) {
      trailing = MutedText("处理中…");
    } else if (user_is_installed) {
      trailing = Row{
          ActionButton(Button("卸载").OnClick([start_uninstall, slug = item.slug]() mutable {
                         start_uninstall(slug);
                       }),
                       ButtonTone::Danger)
              .With(Enabled{!busy}),
          ActionButton(Button(same_version ? "重装" : "更新")
                           .OnClick([start_install, item]() mutable {
                             start_install(item);
                           }))
              .With(Enabled{!busy}),
      }.With(Spacing{6.0F}, CrossAlign{CrossAxisAlignment::Center});
    } else {
      trailing = ActionButton(Button("安装").OnClick([start_install, item]() mutable {
                                start_install(item);
                              }),
                              ButtonTone::Primary)
                     .With(Enabled{!busy});
    }

    registry_rows.Add(
        NavItem(item.name + " (" + item.slug + ")", detail, false, {}, std::move(trailing),
                std::nullopt, DatabaseIcon(DetectDatabaseBrand(item.slug + " " + item.name), 20.0F))
            .Key("registry-" + item.slug));
  }

  View install_list_content;
  if (catalog_loading.Get() && catalog_list.empty()) {
    install_list_content = Column{MutedText("正在获取在线驱动目录…")}.With(Grow(1.0F));
  } else if (catalog_failed.Get() && catalog_list.empty()) {
    install_list_content =
        Column{MutedText("目录加载失败，请点击右上角「刷新目录」重试")}.With(Grow(1.0F));
  } else if (registry_rows.Size() == 0) {
    install_list_content = Column{
        MutedText(catalog_list.empty() ? "点击「刷新目录」加载可安装驱动"
                                       : "没有匹配的驱动"),
    }.With(Grow(1.0F));
  } else {
    install_list_content =
        ScrollView(Column{std::move(registry_rows)}.With(Spacing{4.0F})).With(Grow(1.0F));
  }

  const std::string user_dir = UserAdbcDriversDir();
  View install_tab = Column{
      MutedText("目录源: " + AdbcRegistryBaseUrl() + " · 平台: " +
                CurrentPlatformTuple() +
                (user_dir.empty() ? std::string{} : " · 安装到: " + user_dir)),
      TextField(registry_filter)
          .Label("筛选驱动")
          .Placeholder("postgresql / mysql / duckdb / clickhouse …")
          .Variant(TextFieldVariant::Outlined)
          .OnChanged([registry_filter](const TextEditingValue& value) {
            registry_filter = value;
          }),
      std::move(install_list_content),
      busy && !install_status.Get().empty() ? MutedText(install_status.Get()) : View{},
  }.With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch}, Grow(1.0F));

  View header_actions;
  if (tab.Get() == 0) {
    header_actions = Row{
        ActionButton(Button("关闭").OnClick([dialog] { dialog.Dismiss(); })),
        ActionButton(Button("删除").OnClick(delete_profile), ButtonTone::Danger)
            .With(Enabled{selected.Get() >= 0}),
        ActionButton(Button("保存").OnClick([save_profile] { (void)save_profile(); })),
        ActionButton(Button("连接").OnClick(connect), ButtonTone::Primary),
    }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{8.0F});
  } else {
    header_actions = Row{
        ActionButton(Button("关闭").OnClick([dialog] { dialog.Dismiss(); })),
        ActionButton(Button("刷新目录").OnClick([load_catalog]() mutable {
          load_catalog();
        })).With(Enabled{!catalog_loading.Get()}),
    }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{8.0F});
  }

  View body = Column{
      Row{
          Text("ADBC 管理器", TextRole::Title),
          SegmentedButton({"连接与驱动", "安装驱动"}, tab.Get())
              .OnChanged([switch_tab](std::size_t index) mutable {
                switch_tab(index);
              }),
          Spacer(),
          std::move(header_actions),
      }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{10.0F}),
      IndexedPages({std::move(manage_tab), std::move(install_tab)}, tab.Get())
          .With(Grow(1.0F)),
  }.With(Spacing{10.0F}, CrossAlign{CrossAxisAlignment::Stretch},
         Frame{.width = 860.0F, .height = 620.0F});

  return DialogCard(std::move(body));
}

[[huxerui::composable]] View ConnectDialog(DialogContext dialog, ConnectContext context) {
  const auto config_db = UseService<ConfigDbService>();
  const ThemeSpec& spec = UseTheme();
  const SemanticColors& semantic = UseEnvironment<SemanticColors>();
  auto dialog_tasks = UseTaskScope();
  auto step = UseState<int>(0);
  auto type_filter = UseState(TextEditingValue::FromText({}));
  auto category_tab = UseState<std::size_t>(0);
  auto selected_preset_id = UseState<std::string>("sqlite");
  auto drivers = UseState(std::vector<AdbcDriverInfo>{});
  auto input_mode = UseState<std::size_t>(0);

  auto driver = UseState(TextEditingValue::FromText(context.default_driver));
  auto uri = UseState(TextEditingValue::FromText(context.default_uri));
  auto host = UseState(TextEditingValue::FromText("127.0.0.1"));
  auto port = UseState(TextEditingValue::FromText({}));
  auto database = UseState(TextEditingValue::FromText({}));
  auto username = UseState(TextEditingValue::FromText({}));
  auto password = UseState(TextEditingValue::FromText({}));
  auto title = UseState(TextEditingValue::FromText({}));
  auto save_profile_flag = UseState(true);

  auto installing = UseState(false);
  auto install_status = UseState(std::string{});

  const auto refresh_drivers = [drivers]() {
    drivers = DiscoverAdbcDrivers();
  };

  Lifecycle([refresh_drivers] {
    refresh_drivers();
    return [] {};
  });

  const auto& presets = SupportedDatabasePresets();
  const DatabasePreset* active_ptr = &presets.front();
  for (const DatabasePreset& candidate : presets) {
    if (candidate.id == selected_preset_id.Get()) {
      active_ptr = &candidate;
      break;
    }
  }
  const DatabasePreset active_preset = *active_ptr;
  const std::vector<AdbcDriverInfo>& driver_list = drivers.Get();
  const std::optional<AdbcDriverInfo> matched_driver =
      FindInstalledDriverForPreset(active_preset, driver_list);

  const auto select_preset = [context, step, selected_preset_id, input_mode, driver, uri,
                              host, port, database, username, password, title,
                              install_status, drivers](const DatabasePreset& preset) {
    selected_preset_id = preset.id;
    step = 1;
    input_mode = 0;
    install_status = {};
    title = TextEditingValue::FromText({});
    password = TextEditingValue::FromText({});

    const std::optional<AdbcDriverInfo> installed =
        FindInstalledDriverForPreset(preset, drivers.Get());
    if (preset.builtin && preset.form_kind == ConnectionFormKind::File) {
      driver = TextEditingValue::FromText(
          context.default_driver.empty() ? DefaultSqliteDriverPath() : context.default_driver);
      uri = TextEditingValue::FromText(context.default_uri);
      username = TextEditingValue::FromText({});
      return;
    }
    if (preset.form_kind == ConnectionFormKind::File) {
      driver = TextEditingValue::FromText(installed ? installed->path : preset.driver_slug);
      uri = TextEditingValue::FromText({});
      username = TextEditingValue::FromText({});
      return;
    }
    if (preset.form_kind == ConnectionFormKind::HostPort) {
      driver = TextEditingValue::FromText(installed ? installed->path : preset.driver_slug);
      host = TextEditingValue::FromText(preset.default_host);
      port = TextEditingValue::FromText(preset.default_port);
      database = TextEditingValue::FromText(preset.default_database);
      username = TextEditingValue::FromText(preset.default_user);
      uri = TextEditingValue::FromText(
          ComposePresetUri(preset, preset.default_host, preset.default_port, preset.default_database));
      return;
    }
    if (preset.form_kind == ConnectionFormKind::Uri) {
      driver = TextEditingValue::FromText(installed ? installed->path : preset.driver_slug);
      uri = TextEditingValue::FromText({});
      username = TextEditingValue::FromText({});
      return;
    }
    driver = TextEditingValue::FromText({});
    uri = TextEditingValue::FromText({});
    username = TextEditingValue::FromText({});
  };

  if (step.Get() == 0) {
    const std::string filter_text = type_filter.Get().text;
    const std::size_t cat_idx = category_tab.Get();
    std::vector<const DatabasePreset*> visible;
    visible.reserve(presets.size());
    for (const DatabasePreset& preset : presets) {
      bool cat_ok = true;
      switch (cat_idx) {
        case 1: cat_ok = preset.category == DatabaseCategory::LocalFile; break;
        case 2: cat_ok = preset.category == DatabaseCategory::Relational; break;
        case 3: cat_ok = preset.category == DatabaseCategory::KeyValue; break;
        case 4: cat_ok = preset.category == DatabaseCategory::AnalyticsCloud; break;
        case 5: cat_ok = preset.category == DatabaseCategory::Custom; break;
        default: break;
      }
      if (!cat_ok) {
        continue;
      }
      if (!filter_text.empty() &&
          !ContainsIgnoreCaseAscii(preset.name, filter_text) &&
          !ContainsIgnoreCaseAscii(preset.id, filter_text) &&
          !ContainsIgnoreCaseAscii(preset.driver_slug, filter_text) &&
          !ContainsIgnoreCaseAscii(preset.subtitle, filter_text)) {
        continue;
      }
      visible.push_back(&preset);
    }

    Views grid_rows;
    grid_rows.Reserve((visible.size() + 1) / 2);
    const auto make_card = [spec, semantic, &driver_list,
                            select_preset](const DatabasePreset& preset) {
      View badge;
      if (preset.builtin) {
        badge = StatusBadge("内置", semantic.success, semantic.success_container,
                            WithAlpha(semantic.success, 0.35F));
      } else if (preset.form_kind == ConnectionFormKind::Custom) {
        badge = StatusBadge("手动", spec.colors.on_surface_variant, spec.colors.surface_container,
                            semantic.border_subtle);
      } else if (FindInstalledDriverForPreset(preset, driver_list).has_value()) {
        badge = StatusBadge("已就绪", semantic.success, semantic.success_container,
                            WithAlpha(semantic.success, 0.35F));
      } else {
        badge = StatusBadge("需安装", semantic.warning, semantic.warning_container,
                            WithAlpha(semantic.warning, 0.35F));
      }
      View nav = NavItem(
          preset.name, preset.subtitle, false,
          [select_preset, preset]() mutable { select_preset(preset); }, std::move(badge),
          std::nullopt, DatabaseBadgeIcon(DetectDatabaseBrand(preset.id), 20.0F, 34.0F));
      return Column{std::move(nav)}
          .With(Grow(1.0F), CrossAlign{CrossAxisAlignment::Stretch},
                Background{VisualFill{spec.colors.surface}}, Border{semantic.border_subtle, 1.0F},
                CornerRadius{7.0F})
          .Key("db-type-" + preset.id);
    };

    for (std::size_t i = 0; i < visible.size(); i += 2) {
      const DatabasePreset* left = visible[i];
      const DatabasePreset* right = (i + 1 < visible.size()) ? visible[i + 1] : nullptr;
      View left_card = make_card(*left);
      View right_slot = right != nullptr ? make_card(*right) : View(Spacer().With(Grow(1.0F)));
      grid_rows.Add(
          Row{std::move(left_card), std::move(right_slot)}
              .With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch})
              .Key("db-type-row-" + left->id));
    }

    View grid_content;
    if (grid_rows.Size() == 0) {
      grid_content = Column{
          MutedText("没有匹配的数据库类型"),
      }.With(Grow(1.0F), MainAlign{MainAxisAlignment::Center},
             CrossAlign{CrossAxisAlignment::Center});
    } else {
      grid_content = ScrollView(Column{std::move(grid_rows)}
                                    .With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch}))
                         .With(Grow(1.0F));
    }

    View body = Column{
        Row{
            Text("新建连接", TextRole::Title),
            MutedText("选择数据库类型"),
            Spacer(),
            ActionButton(Button("已保存连接…").OnClick([dialog, open_manager = context.open_manager] {
              dialog.Dismiss();
              if (open_manager) {
                open_manager(0, {});
              }
            })),
            ActionButton(Button("取消").OnClick([dialog] { dialog.Dismiss(); })),
        }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{8.0F}),
        TextField(type_filter)
            .Label("搜索数据库")
            .Placeholder("SQLite / PostgreSQL / MySQL / Redis / DuckDB / ClickHouse …")
            .Variant(TextFieldVariant::Outlined)
            .LineLimits(TextFieldLineLimits::SingleLine())
            .OnChanged([type_filter](const TextEditingValue& value) { type_filter = value; }),
        SegmentedButton({"全部", "本地文件", "关系型", "键值 / 缓存", "分析 / 云端", "自定义"}, cat_idx)
            .OnChanged([category_tab](std::size_t index) { category_tab = index; }),
        std::move(grid_content),
        MutedText("选择数据库类型后进入连接配置；未安装的 ADBC 驱动可在下一步一键安装。"),
    }.With(Spacing{10.0F}, CrossAlign{CrossAxisAlignment::Stretch},
           Frame{.width = 740.0F, .height = 560.0F});

    return DialogCard(std::move(body));
  }

  const auto install_active_driver = [context, dialog_tasks, active_preset, installing,
                                      install_status, driver,
                                      refresh_drivers]() {
    if (installing.Get() || active_preset.driver_slug.empty()) {
      return;
    }
    if (!context.http) {
      if (context.notify) {
        context.notify("HTTP 客户端不可用");
      }
      return;
    }
    installing = true;
    install_status = "正在查询 ADBC Driver Registry …";
    const std::string target_slug = active_preset.driver_slug;
    const auto http = context.http;
    const NotifyFn notify = context.notify;

    dialog_tasks.Launch([http, target_slug, installing, install_status,
                         driver, refresh_drivers, notify]() -> Task<void> {
      RegistryCatalogResult cat = co_await FetchRegistryCatalogAsync(http);
      if (!cat.error.empty()) {
        installing = false;
        install_status = {};
        if (notify) {
          notify(cat.error);
        }
        co_return;
      }

      const RegistryDriverInfo* found = nullptr;
      for (const RegistryDriverInfo& item : cat.drivers) {
        if (item.slug == target_slug) {
          found = &item;
          break;
        }
      }
      if (found == nullptr) {
        installing = false;
        install_status = {};
        if (notify) {
          notify("当前平台 (" + CurrentPlatformTuple() + ") 未提供在线驱动: " + target_slug);
        }
        co_return;
      }

      RegistryDriverInfo item = *found;
      auto on_progress = [install_status](std::string msg) { install_status = std::move(msg); };
      DriverInstallResult res =
          co_await InstallRegistryDriverAsync(http, item, std::move(on_progress));
      installing = false;
      install_status = {};
      if (!res.ok) {
        if (notify) {
          notify(res.message);
        }
        co_return;
      }
      refresh_drivers();
      driver = TextEditingValue::FromText(res.slug);
      if (notify) {
        notify(res.message);
      }
    });
  };

  const auto pick_local_file = [context, dialog_tasks, active_preset, uri, title]() {
    if (!context.picker || !context.picker->CanOpenFiles()) {
      if (context.notify) {
        context.notify("当前平台不支持系统文件选择器");
      }
      return;
    }
    dialog_tasks.Launch([picker = context.picker, preset_name = active_preset.name,
                         extensions = active_preset.file_extensions, uri, title,
                         notify = context.notify]() -> Task<void> {
      FilePickerFilter filter;
      filter.name = preset_name + " 数据库";
      filter.extensions = extensions;
      auto file_reference = co_await picker->OpenFileAsync(std::move(filter));
      if (!file_reference) {
        co_return;
      }
      auto file = file_reference->AsFile();
      if (!file) {
        if (notify) {
          notify("所选文件没有本地路径");
        }
        co_return;
      }
      uri = TextEditingValue::FromText(file->Path());
      if (title.Get().text.empty()) {
        title = TextEditingValue::FromText(file->Name());
      }
    });
  };

  const auto connect = [context, config_db, active_preset, input_mode, driver, uri, host, port,
                        database, username, password, title, save_profile_flag, dialog] {
    const std::string effective_uri =
        (active_preset.form_kind == ConnectionFormKind::HostPort && input_mode.Get() == 0)
            ? ComposePresetUri(active_preset, host.Get().text, port.Get().text, database.Get().text)
            : uri.Get().text;
    const SanitizedUriResult sanitized = SanitizeUriCredentials(effective_uri);

    ConnectionRequest request;
    request.driver_path = driver.Get().text;
    request.uri = sanitized.uri;
    request.username = username.Get().text.empty() ? sanitized.username : username.Get().text;
    request.password = password.Get().text.empty() ? sanitized.password : password.Get().text;
    request.title = title.Get().text.empty() ? UriDisplayName(sanitized.uri) : title.Get().text;

    if (request.driver_path.empty() || request.uri.empty()) {
      if (context.notify) {
        context.notify("ADBC 驱动与连接地址不能为空");
      }
      return;
    }

    if (save_profile_flag.Get() && !context.profiles_path.empty()) {
      ConnectionProfile profile{
          .name = request.title,
          .driver_path = request.driver_path,
          .uri = request.uri,
          .username = request.username,
          .password = request.password,
      };
      std::vector<ConnectionProfile> list = context.profiles.Get();
      bool updated = false;
      for (ConnectionProfile& existing : list) {
        if (existing.name == profile.name) {
          existing = profile;
          updated = true;
          break;
        }
      }
      if (!updated) {
        for (ConnectionProfile& existing : list) {
          if (existing.driver_path == profile.driver_path && existing.uri == profile.uri &&
              existing.username == profile.username) {
            existing = profile;
            updated = true;
            break;
          }
        }
      }
      if (!updated) {
        list.push_back(profile);
      }
      if (!PersistConnectionProfileList(config_db, context.profiles_path, list)) {
        if (context.notify) {
          context.notify("保存连接配置失败: " + context.profiles_path);
        }
        return;
      }
      context.profiles = std::move(list);
    }

    OpenConnection(context.app, context.workspace, context.tasks, std::move(request),
                   context.notify);
    dialog.Dismiss();
  };

  View readiness_strip;
  if (active_preset.form_kind != ConnectionFormKind::Custom) {
    if (matched_driver.has_value()) {
      const std::string ready_detail =
          active_preset.id == "redis"
              ? std::string("内置 Redis 客户端（RESP2/RESP3，无需安装驱动）")
              : (active_preset.builtin
                     ? ("内置 SQLite 驱动 · " + matched_driver->path)
                     : (matched_driver->detail.empty() ? matched_driver->path
                                                       : matched_driver->detail));
      readiness_strip = Row{
          AccentDot(semantic.success, 7.0F),
          MutedText(active_preset.id == "redis" ? ready_detail : ("驱动已就绪 · " + ready_detail)),
      }.With(Spacing{6.0F}, CrossAlign{CrossAxisAlignment::Center});
    } else {
      readiness_strip =
          Row{
              Column{
                  Text("尚未安装 " + active_preset.name + " 驱动（" + active_preset.driver_slug + "）"),
                  MutedText("可点击右侧一键从官方 ADBC Registry 下载安装，或手动指定驱动路径。"),
              }.With(Grow(1.0F), Spacing{2.0F}, CrossAlign{CrossAxisAlignment::Stretch}),
              ActionButton(Button(installing.Get() ? "安装中…" : "一键安装驱动")
                               .OnClick(install_active_driver)
                               .With(Enabled{!installing.Get()}),
                           ButtonTone::Primary),
              ActionButton(Button("ADBC 管理器…").OnClick([dialog, open_manager = context.open_manager,
                                                         slug = active_preset.driver_slug] {
                dialog.Dismiss();
                if (open_manager) {
                  open_manager(1, slug);
                }
              })),
          }.With(Padding{EdgeInsets::Symmetric(10.0F, 8.0F)}, Spacing{8.0F},
                 CrossAlign{CrossAxisAlignment::Center},
                 Background{VisualFill{semantic.warning_container}},
                 Border{WithAlpha(semantic.warning, 0.45F), 1.0F}, CornerRadius{7.0F});
    }
  }

  Views form_fields;
  if (active_preset.form_kind == ConnectionFormKind::File) {
    form_fields.Add(
        Row{
            TextField(uri)
                .Label("数据库文件路径")
                .Placeholder(active_preset.uri_placeholder)
                .Variant(TextFieldVariant::Outlined)
                .LineLimits(TextFieldLineLimits::SingleLine())
                .OnChanged([uri](const TextEditingValue& value) { uri = value; })
                .With(Grow(1.0F)),
            ActionButton(Button("浏览文件…").OnClick(pick_local_file))
                .With(Enabled{context.picker && context.picker->CanOpenFiles()}),
        }.With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Center}));
    form_fields.Add(
        TextField(title)
            .Label("显示名称（可选）")
            .Placeholder("留空则使用文件名")
            .Variant(TextFieldVariant::Outlined)
            .LineLimits(TextFieldLineLimits::SingleLine())
            .OnChanged([title](const TextEditingValue& value) { title = value; }));
    form_fields.Add(
        MonospaceField(
            TextField(driver)
                .Label("ADBC 驱动")
                .Variant(TextFieldVariant::Outlined)
                .LineLimits(TextFieldLineLimits::SingleLine())
                .OnChanged([driver](const TextEditingValue& value) { driver = value; })));
  } else if (active_preset.form_kind == ConnectionFormKind::HostPort) {
    form_fields.Add(
        SegmentedButton({"表单填写", "URI 字符串"}, input_mode.Get())
            .OnChanged([input_mode, active_preset, host, port, database, uri](std::size_t idx) {
              if (idx == 1 && input_mode.Get() == 0) {
                uri = TextEditingValue::FromText(
                    ComposePresetUri(active_preset, host.Get().text, port.Get().text,
                                     database.Get().text));
              }
              input_mode = idx;
            }));
    if (input_mode.Get() == 0) {
      form_fields.Add(
          Row{
              TextField(host)
                  .Label("主机")
                  .Placeholder(active_preset.default_host)
                  .Variant(TextFieldVariant::Outlined)
                  .LineLimits(TextFieldLineLimits::SingleLine())
                  .OnChanged([host](const TextEditingValue& value) { host = value; })
                  .With(Grow(2.0F)),
              TextField(port)
                  .Label("端口")
                  .Placeholder(active_preset.default_port)
                  .Variant(TextFieldVariant::Outlined)
                  .LineLimits(TextFieldLineLimits::SingleLine())
                  .OnChanged([port](const TextEditingValue& value) { port = value; })
                  .With(Grow(1.0F)),
          }.With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch}));
      form_fields.Add(
          TextField(database)
              .Label(active_preset.id == "redis" ? "数据库编号（0-15）" : "数据库名")
              .Placeholder(active_preset.default_database)
              .Variant(TextFieldVariant::Outlined)
              .LineLimits(TextFieldLineLimits::SingleLine())
              .OnChanged([database](const TextEditingValue& value) { database = value; }));
      form_fields.Add(
          Row{
              TextField(username)
                  .Label(active_preset.id == "redis" ? "用户名（ACL 可选）" : "用户名")
                  .Variant(TextFieldVariant::Outlined)
                  .LineLimits(TextFieldLineLimits::SingleLine())
                  .OnChanged([username](const TextEditingValue& value) { username = value; })
                  .With(Grow(1.0F)),
              TextField(password)
                  .Label("密码（可选）")
                  .Variant(TextFieldVariant::Outlined)
                  .LineLimits(TextFieldLineLimits::SingleLine())
                  .Secure(true)
                  .OnChanged([password](const TextEditingValue& value) { password = value; })
                  .With(Grow(1.0F)),
          }.With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch}));
      const std::string preview = ComposePresetUri(
          active_preset, host.Get().text, port.Get().text, database.Get().text);
      form_fields.Add(MutedText("连接 URI: " + preview));
    } else {
      form_fields.Add(
          MonospaceField(
              TextField(uri)
                  .Label("连接 URI")
                  .Placeholder(active_preset.uri_placeholder)
                  .Variant(TextFieldVariant::Outlined)
                  .LineLimits(TextFieldLineLimits::SingleLine())
                  .OnChanged([uri](const TextEditingValue& value) { uri = value; })));
      form_fields.Add(
          Row{
              TextField(username)
                  .Label("用户名（可选）")
                  .Variant(TextFieldVariant::Outlined)
                  .LineLimits(TextFieldLineLimits::SingleLine())
                  .OnChanged([username](const TextEditingValue& value) { username = value; })
                  .With(Grow(1.0F)),
              TextField(password)
                  .Label("密码（可选）")
                  .Variant(TextFieldVariant::Outlined)
                  .LineLimits(TextFieldLineLimits::SingleLine())
                  .Secure(true)
                  .OnChanged([password](const TextEditingValue& value) { password = value; })
                  .With(Grow(1.0F)),
          }.With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch}));
      form_fields.Add(
          MonospaceField(
              TextField(driver)
                  .Label("ADBC 驱动（短名或路径）")
                  .Variant(TextFieldVariant::Outlined)
                  .LineLimits(TextFieldLineLimits::SingleLine())
                  .OnChanged([driver](const TextEditingValue& value) { driver = value; })));
    }
    form_fields.Add(
        TextField(title)
            .Label("显示名称（可选）")
            .Placeholder("留空则自动命名")
            .Variant(TextFieldVariant::Outlined)
            .LineLimits(TextFieldLineLimits::SingleLine())
            .OnChanged([title](const TextEditingValue& value) { title = value; }));
  } else if (active_preset.form_kind == ConnectionFormKind::Uri) {
    form_fields.Add(
        MonospaceField(
            TextField(uri)
                .Label("连接 URI")
                .Placeholder(active_preset.uri_placeholder)
                .Variant(TextFieldVariant::Outlined)
                .LineLimits(TextFieldLineLimits::SingleLine())
                .OnChanged([uri](const TextEditingValue& value) { uri = value; })));
    form_fields.Add(
        Row{
            TextField(username)
                .Label("用户名（可选）")
                .Variant(TextFieldVariant::Outlined)
                .LineLimits(TextFieldLineLimits::SingleLine())
                .OnChanged([username](const TextEditingValue& value) { username = value; })
                .With(Grow(1.0F)),
            TextField(password)
                .Label("密码（可选）")
                .Variant(TextFieldVariant::Outlined)
                .LineLimits(TextFieldLineLimits::SingleLine())
                .Secure(true)
                .OnChanged([password](const TextEditingValue& value) { password = value; })
                .With(Grow(1.0F)),
        }.With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch}));
    form_fields.Add(
        MonospaceField(
            TextField(driver)
                .Label("ADBC 驱动（短名或路径）")
                .Variant(TextFieldVariant::Outlined)
                .LineLimits(TextFieldLineLimits::SingleLine())
                .OnChanged([driver](const TextEditingValue& value) { driver = value; })));
    form_fields.Add(
        TextField(title)
            .Label("显示名称（可选）")
            .Placeholder("留空则自动命名")
            .Variant(TextFieldVariant::Outlined)
            .LineLimits(TextFieldLineLimits::SingleLine())
            .OnChanged([title](const TextEditingValue& value) { title = value; }));
  } else {
    Views discovered_rows;
    discovered_rows.Reserve(driver_list.size());
    for (const AdbcDriverInfo& info : driver_list) {
      const std::string& detail = info.detail.empty() ? info.path : info.detail;
      discovered_rows.Add(
          NavItem(info.name, detail, driver.Get().text == info.path,
                  [driver, info] { driver = TextEditingValue::FromText(info.path); }, {},
                  std::nullopt,
                  DatabaseIcon(DetectDatabaseBrand(info.path + " " + info.name), 18.0F))
              .Key("custom-driver-" + info.path + "-" + detail));
    }
    if (discovered_rows.Size() > 0) {
      form_fields.Add(Caption("已发现的 ADBC 驱动（点击填入）"));
      form_fields.Add(
          ScrollView(Column{std::move(discovered_rows)}.With(Spacing{2.0F}))
              .With(Frame{.height = 110.0F}));
    }
    form_fields.Add(
        MonospaceField(
            TextField(driver)
                .Label("ADBC 驱动（短名 / .toml / 共享库路径）")
                .Placeholder("postgresql · /path/to/libadbc_driver_*.so")
                .Variant(TextFieldVariant::Outlined)
                .LineLimits(TextFieldLineLimits::SingleLine())
                .OnChanged([driver](const TextEditingValue& value) { driver = value; })));
    form_fields.Add(
        MonospaceField(
            TextField(uri)
                .Label("URI")
                .Placeholder(active_preset.uri_placeholder)
                .Variant(TextFieldVariant::Outlined)
                .LineLimits(TextFieldLineLimits::SingleLine())
                .OnChanged([uri](const TextEditingValue& value) { uri = value; })));
    form_fields.Add(
        Row{
            TextField(username)
                .Label("用户名（可选）")
                .Variant(TextFieldVariant::Outlined)
                .LineLimits(TextFieldLineLimits::SingleLine())
                .OnChanged([username](const TextEditingValue& value) { username = value; })
                .With(Grow(1.0F)),
            TextField(password)
                .Label("密码（可选）")
                .Variant(TextFieldVariant::Outlined)
                .LineLimits(TextFieldLineLimits::SingleLine())
                .Secure(true)
                .OnChanged([password](const TextEditingValue& value) { password = value; })
                .With(Grow(1.0F)),
        }.With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch}));
    form_fields.Add(
        TextField(title)
            .Label("显示名称（可选）")
            .Placeholder("留空则使用 URI 文件名")
            .Variant(TextFieldVariant::Outlined)
            .LineLimits(TextFieldLineLimits::SingleLine())
            .OnChanged([title](const TextEditingValue& value) { title = value; }));
  }

  form_fields.Add(
      Switch("保存到已保存连接（默认保留密码）", save_profile_flag)
          .OnChanged([save_profile_flag](bool value) { save_profile_flag = value; }));

  View step1_body = Column{
      Row{
          ActionButton(Button("← 选择类型").OnClick([step] {
            step = 0;
          })),
          DatabaseIcon(DetectDatabaseBrand(active_preset.id), 22.0F),
          Text("连接 " + active_preset.name, TextRole::Title),
          Spacer(),
          ActionButton(Button("取消").OnClick([dialog] { dialog.Dismiss(); })),
          ActionButton(Button("连接").OnClick(connect), ButtonTone::Primary)
              .With(Enabled{!installing.Get()}),
      }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{8.0F}),
      std::move(readiness_strip),
      ScrollView(Column{std::move(form_fields)}
                     .With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch}))
          .With(Grow(1.0F)),
      installing.Get() && !install_status.Get().empty() ? MutedText(install_status.Get())
                                                        : View{},
  }.With(Spacing{10.0F}, CrossAlign{CrossAxisAlignment::Stretch},
         Frame{.width = 740.0F, .height = 560.0F});

  return DialogCard(std::move(step1_body));
}

[[huxerui::composable]] View TableBrowserPage(
    State<AppModel> app, State<WorkspaceModel> workspace, std::uint64_t tab_id,
    TaskScope tasks, NotifyFn notify, AppThemeConfig theme_config, bool system_dark) {
  const ThemeSpec& spec = UseTheme();
  const SemanticColors& semantic = UseEnvironment<SemanticColors>();
  const auto dialogs = UseDialog();
  auto live_drafts = UseState(std::make_shared<std::unordered_map<std::string, std::string>>());

  const WorkspaceTab* tab = workspace.Get().FindTab(tab_id);
  if (tab == nullptr || tab->kind != WorkspaceTabKind::Table) {
    return TableEmptyState("标签页已关闭");
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
    return TableEmptyState("数据库连接已断开");
  }
  const bool is_redis = session->dialect == Dialect::Redis;

  const auto apply_draft_value = [workspace, tab_id](const std::string& row_id,
                                                     const std::string& col_id,
                                                     const std::string& value) {
    const auto draft_id = ParseDraftRowId(row_id);
    if (!draft_id) {
      return;
    }
    const WorkspaceTab* snapshot_tab = workspace.Get().FindTab(tab_id);
    if (snapshot_tab == nullptr || snapshot_tab->kind != WorkspaceTabKind::Table) {
      return;
    }
    std::size_t col_idx = snapshot_tab->table.columns.size();
    for (std::size_t i = 0; i < snapshot_tab->table.columns.size(); ++i) {
      if (snapshot_tab->table.columns[i].name == col_id) {
        col_idx = i;
        break;
      }
    }
    if (col_idx >= snapshot_tab->table.columns.size()) {
      return;
    }
    if (const PendingInsertRow* existing =
            FindDraftRow(snapshot_tab->table.pending_inserts, *draft_id)) {
      const std::string& cur_val =
          col_idx < existing->values.size() ? existing->values[col_idx] : std::string{};
      if (cur_val == value) {
        return;
      }
    } else {
      return;
    }

    workspace.Update([&](WorkspaceModel& ws) {
      WorkspaceTab* target = ws.FindTab(tab_id);
      if (target == nullptr || target->kind != WorkspaceTabKind::Table) {
        return;
      }
      PendingInsertRow* draft = FindDraftRowMutable(target->table.pending_inserts, *draft_id);
      if (draft == nullptr) {
        return;
      }
      if (draft->values.size() < target->table.columns.size()) {
        draft->values.resize(target->table.columns.size());
      }
      draft->values[col_idx] = value;
    });
  };

  const auto flush_draft_cell = [live_drafts, apply_draft_value](const std::string& row_id,
                                                                 const std::string& col_id) {
    auto live = live_drafts.Get();
    const std::string key = DraftCellBufferKey(row_id, col_id);
    const auto it = live->find(key);
    if (it == live->end()) {
      return;
    }
    std::string text = std::move(it->second);
    live->erase(it);
    apply_draft_value(row_id, col_id, text);
  };

  const auto flush_live_drafts = [workspace, tab_id, live_drafts]() {
    auto live = live_drafts.Get();
    if (live->empty()) {
      return;
    }
    auto pending = std::move(*live);
    live->clear();
    workspace.Update([&](WorkspaceModel& ws) {
      WorkspaceTab* target = ws.FindTab(tab_id);
      if (target == nullptr || target->kind != WorkspaceTabKind::Table) {
        return;
      }
      for (const auto& [key, val] : pending) {
        const std::size_t sep = key.find('\n');
        if (sep == std::string::npos) {
          continue;
        }
        const std::string_view row_id(key.data(), sep);
        const std::string_view col_id(key.data() + sep + 1, key.size() - sep - 1);
        const auto draft_id = ParseDraftRowId(row_id);
        if (!draft_id) {
          continue;
        }
        PendingInsertRow* draft =
            FindDraftRowMutable(target->table.pending_inserts, *draft_id);
        if (draft == nullptr) {
          continue;
        }
        if (draft->values.size() < target->table.columns.size()) {
          draft->values.resize(target->table.columns.size());
        }
        for (std::size_t i = 0; i < target->table.columns.size(); ++i) {
          if (target->table.columns[i].name == col_id) {
            draft->values[i] = val;
            break;
          }
        }
      }
    });
  };

  const auto block_if_dirty = [workspace, tab_id, notify]() -> bool {
    const WorkspaceTab* cur = workspace.Get().FindTab(tab_id);
    if (cur != nullptr && !cur->table.pending_inserts.empty()) {
      if (notify) {
        notify("当前有未提交的新增行，请先点击「提交」或「回滚」");
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

  const auto apply_filter = [reload, workspace, tab_id] {
    const WorkspaceTab* cur = workspace.Get().FindTab(tab_id);
    if (cur == nullptr) {
      return;
    }
    reload(0, cur->filter_input.text);
  };

  const auto refresh = [reload, workspace, tab_id] {
    const WorkspaceTab* cur = workspace.Get().FindTab(tab_id);
    if (cur == nullptr) {
      return;
    }
    reload(cur->table.page, cur->table.filter);
  };

  const auto add_draft_row = [workspace, tab_id, flush_live_drafts, notify] {
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
      notify("已在表格最底部添加空白行，滚动到底部编辑后点击「提交」或「回滚」");
    }
  };

  const auto commit_inserts = [app, workspace, tasks, tab_id, flush_live_drafts, notify] {
    flush_live_drafts();
    RequestCommitPendingInserts(app, workspace, tasks, tab_id, notify);
  };

  const auto rollback_inserts = [workspace, tab_id, live_drafts, notify] {
    live_drafts.Get()->clear();
    bool cleared = false;
    workspace.Update([&](WorkspaceModel& ws) {
      WorkspaceTab* target = ws.FindTab(tab_id);
      if (target == nullptr || target->kind != WorkspaceTabKind::Table ||
          target->table.pending_inserts.empty() || target->table.committing) {
        return;
      }
      target->table.pending_inserts.clear();
      if (target->selected_cell.has_value() && IsDraftRowId(target->selected_cell->row_id)) {
        target->selected_cell = std::nullopt;
      }
      cleared = true;
    });
    if (cleared && notify) {
      notify("已回滚未保存的新增行");
    }
  };

  const auto delete_selected_action = [dialogs, app, workspace, tab_id, live_drafts,
                                       flush_live_drafts, tasks, notify, theme_config,
                                       system_dark, is_redis] {
    const WorkspaceTab* cur = workspace.Get().FindTab(tab_id);
    if (cur == nullptr || !cur->selected_cell.has_value()) {
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
        notify("已移除 1 条未保存行");
      }
      return;
    }
    dialogs.Show([app, workspace, tab_id, tasks, notify, theme_config, system_dark,
                  is_redis](DialogContext dialog) {
      return WithDialogTheme(theme_config, system_dark,
                             DeleteRowDialog(dialog, app, workspace, tab_id, tasks, notify, is_redis));
    });
  };

  Views toolbar_items;
  toolbar_items.Add(
      TextField(tab->filter_input)
          .Label(is_redis ? "筛选  MATCH" : "筛选  WHERE")
          .Placeholder(is_redis ? "例如 user:* 或 session:*" : "例如 price > 10 AND name LIKE 'A%'")
          .Variant(TextFieldVariant::Outlined)
          .LineLimits(TextFieldLineLimits::SingleLine())
          .OnChanged([workspace, tab_id](const TextEditingValue& value) {
            workspace.Update([&](WorkspaceModel& ws) {
              if (WorkspaceTab* target = ws.FindTab(tab_id)) {
                target->filter_input = value;
              }
            });
          })
          .OnSubmitted([apply_filter] { apply_filter(); })
          .With(Frame{.width = 240.0F}));
  toolbar_items.Add(ActionButton(Button("应用筛选").OnClick(apply_filter)));
  toolbar_items.Add(Spacer());
  toolbar_items.Add(ActionButton(Button("刷新").OnClick(refresh)));
  toolbar_items.Add(
      ActionButton(Button(is_redis ? "＋ 添加键" : "＋ 添加行").OnClick(add_draft_row))
          .With(Enabled{model.editable && !model.read_only && !model.committing}));
  if (!model.pending_inserts.empty()) {
    const std::string commit_label =
        model.committing ? "提交中…"
                         : "提交 (" + std::to_string(model.pending_inserts.size()) + ")";
    toolbar_items.Add(
        ActionButton(Button(commit_label).OnClick(commit_inserts), ButtonTone::Primary)
            .With(Enabled{!model.committing}));
    toolbar_items.Add(
        ActionButton(Button("回滚").OnClick(rollback_inserts)).With(Enabled{!model.committing}));
  }
  toolbar_items.Add(
      ActionButton(Button(is_redis ? "删除键" : "删除行").OnClick(delete_selected_action),
                   ButtonTone::Danger)
          .With(Enabled{model.editable && !model.read_only && !model.committing &&
                        tab->selected_cell.has_value()}));

  View toolbar = Row{std::move(toolbar_items)}.With(CrossAlign{CrossAxisAlignment::Center},
                                                    Spacing{8.0F});

  const std::int64_t page_count =
      std::max<std::int64_t>(1, (model.total + model.page_size - 1) / model.page_size);
  const std::string status_text =
      !model.pending_inserts.empty()
          ? ("待提交 " + std::to_string(model.pending_inserts.size()) +
             " 行 · 双击或按 Enter 填写表格底部空白行，点击「提交」保存或「回滚」放弃")
          : (model.message.empty() ? "没有数据" : model.message);
  View pager = Row{
      MutedText(status_text),
      model.read_only ? Chip("只读") : View{},
      Spacer(),
      ActionButton(Button("上一页")
                       .OnClick([reload, model] {
                         if (model.page > 0) {
                           reload(model.page - 1, model.filter);
                         }
                       }))
          .With(Enabled{model.page > 0 && model.pending_inserts.empty()}),
      ActionButton(Button("下一页")
                       .OnClick([reload, model, page_count] {
                         if (model.page + 1 < page_count) {
                           reload(model.page + 1, model.filter);
                         }
                       }))
          .With(Enabled{model.page + 1 < page_count && model.pending_inserts.empty()}),
  }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{8.0F});

  View table_view;
  if ((model.snapshot == nullptr && model.pending_inserts.empty()) || model.columns.empty()) {
    const std::string empty_text =
        model.loading
            ? "正在加载…"
            : (!model.error.empty() ? "加载失败，点击「刷新」重试" : "表中没有可显示的列");
    table_view = TableEmptyState(empty_text);
  } else {
    const std::shared_ptr<const GridSnapshot> snapshot = model.snapshot;
    const std::vector<PendingInsertRow> drafts = model.pending_inserts;
    std::vector<DataTableColumn> columns;
    columns.reserve(model.columns.size());
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
      column.render_cell = [snapshot, drafts, index, spec, semantic](
                               const DataTableCellContext& cell) -> DataTableCellContent {
        if (const auto draft_id = ParseDraftRowId(cell.id.row_id)) {
          const PendingInsertRow* draft = FindDraftRow(drafts, *draft_id);
          const std::string raw = (draft != nullptr && index < draft->values.size())
                                      ? draft->values[index]
                                      : std::string{};
          const bool is_blank = raw.empty();
          const std::string display =
              index == 0 ? (is_blank ? "＋ (空白)" : "＋ " + raw) : (is_blank ? "(空白)" : raw);
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
              .label = is_blank ? std::string{"空白"} : raw,
          };
        }

        const bool is_null = CellIsNull(snapshot, cell.id.row_id, index);
        const std::string text = CellText(snapshot, cell.id.row_id, index);
        TextStyle style{Font::Monospace(13.0F),
                        is_null ? semantic.null_text : spec.colors.on_surface};
        if (is_null) {
          style.font = Font::Monospace(13.0F).WithSlant(FontSlant::Italic);
        }
        return DataTableCellContent{
            .view = Text(SingleLineCellPreview(text))
                        .Style(style)
                        .LineLimit(1)
                        .Overflow(TextOverflow::Ellipsis),
            .label = text,
        };
      };
      if (model.editable && !model.read_only && !IsBlobColumn(meta) &&
          !(is_redis && meta.name == "size")) {
        column.editor = DataTableEditor{
            .initial_text = [workspace, tab_id, live_drafts, snapshot, index,
                             col_id = meta.name](const DataTableCellContext& cell) {
              if (const auto draft_id = ParseDraftRowId(cell.id.row_id)) {
                auto live = live_drafts.Get();
                if (const auto it = live->find(DraftCellBufferKey(cell.id.row_id, col_id));
                    it != live->end()) {
                  return it->second;
                }
                const WorkspaceTab* cur = workspace.Get().FindTab(tab_id);
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
              return CellIsNull(snapshot, cell.id.row_id, index)
                         ? std::string{"NULL"}
                         : CellText(snapshot, cell.id.row_id, index);
            },
            .render = [live_drafts, flush_draft_cell](const DataTableEditContext& edit) {
              const std::string row_id = edit.cell.id.row_id;
              const std::string col_id = edit.cell.id.column_id;
              View field = TextField(edit.draft)
                               .Variant(TextFieldVariant::Standard)
                               .LineLimits(TextFieldLineLimits::SingleLine())
                               .VerticalAlign(TextVerticalAlign::Center)
                               .InputConfiguration({.action = TextInputAction::Done})
                               .OnChanged([draft = edit.draft, live_drafts, row_id, col_id](
                                              const TextEditingValue& value) {
                                 draft = value;
                                 if (IsDraftRowId(row_id)) {
                                   (*live_drafts.Get())[DraftCellBufferKey(row_id, col_id)] =
                                       value.text;
                                 }
                               })
                               .OnSubmitted(edit.commit)
                               .With(Padding{EdgeInsets::Symmetric(4.0F, 8.0F)}, edit.focus_target);
              if (IsDraftRowId(row_id)) {
                field = std::move(field).On<ViewEvents::FocusChanged>(
                    [flush_draft_cell, row_id, col_id](bool focused) {
                      if (!focused) {
                        flush_draft_cell(row_id, col_id);
                      }
                    });
              }
              return field;
            },
        };
      }
      columns.push_back(std::move(column));
    }

    DataTableOptions options{
        .rows = MakeTableRows(snapshot, model.pending_inserts),
        .columns = std::move(columns),
        .row_height = 30.0F,
        .header_height = 32.0F,
        .frozen_column_count = 0,
        .sort = model.sort,
        .selected_cell = tab->selected_cell,
    };

    table_view =
        DataTable(std::move(options))
            .On<DataTableEvents::SortRequested>(
                [app, workspace, tasks, tab_id, notify,
                 block_if_dirty](const DataTableSort& requested) {
                  if (block_if_dirty()) {
                    return;
                  }
                  const WorkspaceTab* cur = workspace.Get().FindTab(tab_id);
                  if (cur == nullptr) {
                    return;
                  }
                  DataTableSort next = requested;
                  if (cur->table.sort.has_value() &&
                      cur->table.sort->column_id == requested.column_id) {
                    next.direction =
                        cur->table.sort->direction == DataTableSortDirection::Ascending
                            ? DataTableSortDirection::Descending
                            : DataTableSortDirection::Ascending;
                  } else {
                    next.direction = DataTableSortDirection::Ascending;
                  }
                  RequestLoadTable(app, workspace, tasks, tab_id, 0, next, cur->table.filter,
                                   false, notify);
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
                [app, workspace, tasks, tab_id, live_drafts, apply_draft_value,
                 notify](const DataTableEditRequest& edit) {
                  if (IsDraftRowId(edit.cell_id.row_id)) {
                    live_drafts.Get()->erase(
                        DraftCellBufferKey(edit.cell_id.row_id, edit.cell_id.column_id));
                    apply_draft_value(edit.cell_id.row_id, edit.cell_id.column_id, edit.text);
                    return;
                  }
                  RequestSaveCell(app, workspace, tasks, tab_id, edit, notify);
                })
            .On<DataTableEvents::ColumnResized>(
                [workspace, tab_id](const DataTableColumnResize& resize) {
                  workspace.Update([&](WorkspaceModel& ws) {
                    if (WorkspaceTab* target = ws.FindTab(tab_id)) {
                      target->table.column_widths[resize.column_id] = resize.width;
                    }
                  });
                })
            .With(Grow(1.0F), Border{semantic.border_subtle, 1.0F});
  }

  return Column{
      std::move(toolbar),
      std::move(table_view),
      std::move(pager),
  }.With(Grow(1.0F), Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch});
}

// ---------------------------------------------------------------------------
// SQL console
// ---------------------------------------------------------------------------

[[huxerui::composable]] View SqlPage(State<AppModel> app, State<WorkspaceModel> workspace,
                                     std::uint64_t tab_id, TaskScope tasks, NotifyFn notify) {
  const ThemeSpec& spec = UseTheme();
  const SemanticColors& semantic = UseEnvironment<SemanticColors>();
  const WorkspaceTab* tab = workspace.Get().FindTab(tab_id);
  if (tab == nullptr || tab->kind != WorkspaceTabKind::Sql) {
    return TableEmptyState("查询标签页已关闭");
  }
  const SqlModel& sql_model = tab->sql;
  const DbSession* session = nullptr;
  for (const DbSession& open : app.Get().sessions) {
    if (open.id == tab->session_id) {
      session = &open;
      break;
    }
  }

  if (session == nullptr || !session->session) {
    return TableEmptyState("数据库连接已断开，请重新连接后执行");
  }
  const bool is_redis = session->dialect == Dialect::Redis;

  const auto run = [app, workspace, tab_id, tasks, notify] {
    const WorkspaceTab* cur_tab = workspace.Get().FindTab(tab_id);
    if (cur_tab == nullptr || cur_tab->kind != WorkspaceTabKind::Sql) {
      return;
    }
    const std::string text = TrimmedSql(cur_tab->sql.editor.text);
    if (text.empty()) {
      return;
    }
    const std::uint64_t session_id = cur_tab->session_id;
    std::shared_ptr<AdbcSession> adbc;
    bool session_is_redis = false;
    for (const DbSession& open : app.Get().sessions) {
      if (open.id == session_id) {
        adbc = open.session;
        session_is_redis = (open.dialect == Dialect::Redis);
        break;
      }
    }
    if (!adbc) {
      if (notify) {
        notify("连接已关闭");
      }
      return;
    }

    workspace.Update([tab_id](WorkspaceModel& ws) {
      if (WorkspaceTab* target = ws.FindTab(tab_id)) {
        target->sql.running = true;
        target->sql.error.clear();
        target->sql.status = "执行中…";
      }
    });

    tasks.Launch([app, workspace, tab_id, tasks, notify, adbc, session_id, session_is_redis,
                  text]() -> Task<void> {
      if (session_is_redis || IsSqlReadStatement(text)) {
        AdbcQueryResult result =
            co_await ExecuteQueryAsync(adbc, text, true, kMaxQueryRows);
        if (!result.error.empty()) {
          const std::string message = result.error;
          workspace.Update([tab_id](WorkspaceModel& ws) {
            if (WorkspaceTab* target = ws.FindTab(tab_id)) {
              target->sql.running = false;
              target->sql.error.clear();
              target->sql.status.clear();
            }
          });
          if (notify) {
            notify((session_is_redis ? "命令失败: " : "SQL 失败: ") + message);
          }
          co_return;
        }
        auto query = std::make_shared<QueryResult>();
        query->columns = std::move(result.columns);
        query->snapshot = std::make_shared<const GridSnapshot>(
            GridSnapshot{.rows = std::move(result.rows)});
        query->truncated = result.truncated;
        std::unordered_map<std::string, float> auto_widths;
        auto_widths.reserve(query->columns.size());
        for (std::size_t i = 0; i < query->columns.size(); ++i) {
          auto_widths.emplace("result-" + std::to_string(i),
                              ComputeAutoColumnWidth(query->columns[i], i, query->snapshot.get()));
        }
        const std::size_t rows = query->snapshot->rows.size();
        workspace.Update([tab_id, query, rows, auto_widths = std::move(auto_widths)](
                             WorkspaceModel& ws) mutable {
          if (WorkspaceTab* target = ws.FindTab(tab_id)) {
            query->id = target->sql.next_result_id++;
            target->sql.result = query;
            target->sql.column_widths = std::move(auto_widths);
            target->sql.running = false;
            target->sql.error.clear();
            target->sql.status = "返回 " + std::to_string(rows) + " 行";
            if (query->truncated) {
              target->sql.status += "（已截断）";
            }
          }
        });
        if (session_is_redis && !IsReadStatement(Dialect::Redis, text)) {
          RefreshSessionObjects(app, workspace, tasks, session_id, notify, false);
        }
        co_return;
      }

      AdbcQueryResult executed =
          co_await ExecuteQueryAsync(adbc, text, false, 0);
      if (!executed.error.empty()) {
        const std::string message = executed.error;
        workspace.Update([tab_id](WorkspaceModel& ws) {
          if (WorkspaceTab* target = ws.FindTab(tab_id)) {
            target->sql.running = false;
            target->sql.error.clear();
            target->sql.status.clear();
          }
        });
        if (notify) {
          notify("SQL 失败: " + message);
        }
        co_return;
      }

      workspace.Update([tab_id, affected = executed.affected](WorkspaceModel& ws) {
        if (WorkspaceTab* target = ws.FindTab(tab_id)) {
          target->sql.result.reset();
          target->sql.column_widths.clear();
          target->sql.running = false;
          target->sql.error.clear();
          target->sql.status =
              affected >= 0 ? "影响 " + std::to_string(affected) + " 行" : "执行完成";
        }
      });
      RefreshSessionObjects(app, workspace, tasks, session_id, notify, false);
      if (notify) {
        notify("SQL 已执行，影响 " + std::to_string(executed.affected) + " 行");
      }
    });
  };

  View result_view;
  if (sql_model.result) {
    const std::shared_ptr<const GridSnapshot> snapshot = sql_model.result->snapshot;
    if (snapshot && !sql_model.result->columns.empty()) {
      const std::uint64_t result_id = sql_model.result->id;
      DataTableOptions options{
          .rows = MakeRows(snapshot),
          .columns = BuildReadOnlyColumns(sql_model.result->columns, snapshot,
                                          sql_model.column_widths, spec.colors.on_surface,
                                          semantic.null_text),
          .row_height = 30.0F,
          .header_height = 32.0F,
          .frozen_column_count = 0,
          .sort = std::nullopt,
          .selected_cell = std::nullopt,
      };
      result_view = DataTable(std::move(options))
                        .On<DataTableEvents::ColumnResized>(
                            [workspace, tab_id, result_id](const DataTableColumnResize& resize) {
                              workspace.Update([&](WorkspaceModel& ws) {
                                if (WorkspaceTab* target = ws.FindTab(tab_id);
                                    target != nullptr && target->sql.result &&
                                    target->sql.result->id == result_id) {
                                  target->sql.column_widths[resize.column_id] = resize.width;
                                }
                              });
                            })
                        .With(Grow(1.0F), Border{semantic.border_subtle, 1.0F})
                        .Key("sql-result-" + std::to_string(result_id));
    } else {
      result_view = TableEmptyState(sql_model.status);
    }
  } else {
    result_view = TableEmptyState("执行一条语句以查看结果");
  }

  return Column{
      Row{
          DatabaseIcon(DetectDatabaseBrand(session->driver_path, session->uri, session->dialect),
                       20.0F),
          Text(is_redis ? "Redis 控制台" : "SQL", TextRole::Title),
          MutedText(session->name),
          Spacer(),
          ActionButton(Button(sql_model.running ? "执行中…" : "执行")
                           .OnClick(run)
                           .With(Enabled{!sql_model.running}),
                       ButtonTone::Primary),
      }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{8.0F}),
      MonospaceField(
          TextField(sql_model.editor)
              .Label(is_redis ? "Redis 命令" : "SQL 语句")
              .Placeholder(is_redis ? "PING  ·  GET key  ·  HGETALL user:1  ·  INFO server"
                                    : "SELECT * FROM table LIMIT 100;")
              .Variant(TextFieldVariant::Outlined)
              .LineLimits(TextFieldLineLimits::MultiLine(6, 12))
              .OnChanged([workspace, tab_id](const TextEditingValue& value) {
                workspace.Update([&](WorkspaceModel& ws) {
                  if (WorkspaceTab* target = ws.FindTab(tab_id)) {
                    target->sql.editor = value;
                  }
                });
              })
              .With(Frame{.min_height = 140.0F})),
      Row{
          MutedText(sql_model.status),
          Spacer(),
          MutedText(is_redis ? "支持 Redis 命令（自动识别读写，写操作后刷新数据库计数）"
                             : "支持 SELECT / PRAGMA / 写语句 / RETURNING"),
      }.With(CrossAlign{CrossAxisAlignment::Center}),
      std::move(result_view),
  }.With(Grow(1.0F), Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch});
}

// ---------------------------------------------------------------------------
// Settings page
// ---------------------------------------------------------------------------

[[huxerui::composable]] View SettingsPage(NotifyFn notify) {
  const auto settings = UseService<SettingsModel>();
  const ThemeSpec& spec = UseTheme();
  const SemanticColors& semantic = UseEnvironment<SemanticColors>();
  auto tasks = UseTaskScope();
  auto transition = UseSceneTransition();
  // 动画互斥旗标放 shared_ptr 里改：不经过 State 更新，防抖期间不触发重组。
  struct ThemeAnimationFlag {
    bool animating = false;
  };
  auto animating = UseState(std::make_shared<ThemeAnimationFlag>());
  const bool system_dark = UseSystemThemeBrightness() == ThemeBrightness::Dark;
  const AppThemeConfig theme_config = settings->ThemeConfig();

  // 明暗真正变化时使用圆形扩散过渡（参考 Clash-Flux），并将偏好持久化到 dbpp.sqlite3。
  const auto apply_mode = [settings, notify, transition, tasks, animating, system_dark,
                           current_mode = theme_config.mode,
                           motion = spec.motion](ThemeMode next_mode) {
    if (animating.Get()->animating) {
      return;
    }
    const bool current_dark = ResolveIsDark(current_mode, system_dark);
    const bool target_dark = ResolveIsDark(next_mode, system_dark);
    const auto mutation = [settings, notify, next_mode] {
      if (!settings->SetThemeMode(next_mode) && notify) {
        notify("主题设置保存失败");
      }
    };
    if (current_dark == target_dark || motion.reduced_motion) {
      mutation();
      return;
    }
    animating.Get()->animating = true;
    tasks.Launch([animating, duration = motion.slow + motion.fast]() -> Task<void> {
      co_await Delay(std::chrono::duration<double>{duration});
      animating.Get()->animating = false;
    });
    const TransitionSpec reveal{CircularRevealTransition{}, TweenSpec{motion.slow}};
    transition.RunFromCurrentInteraction(current_dark ? reveal.Reversed() : reveal,
                                         std::move(mutation));
  };

  const auto& presets = AvailableThemePresets();
  const auto make_preset_card = [settings, notify, &spec, &semantic,
                                 active_preset = theme_config.preset](const ThemePresetMeta& meta) {
    const bool selected = meta.preset == active_preset;
    const Color title_color = selected ? spec.colors.on_primary_container : spec.colors.on_surface;
    const Color sub_color = selected ? WithAlpha(spec.colors.on_primary_container, 0.80F)
                                     : spec.colors.on_surface_variant;
    return Column{
        Spacer().With(Grow(0.0F), Frame{.height = 4.0F, .min_height = 4.0F, .max_height = 4.0F},
                      CornerRadius{2.0F}, Background{meta.swatch_bar}),
        Row{
            Text(meta.name).Style(
                TextStyle{Font::System(font_size::kBody).WithWeight(FontWeight::SemiBold),
                          title_color}),
            Spacer(),
            AccentDot(meta.swatch_a, 8.0F),
            AccentDot(meta.swatch_b, 8.0F),
            AccentDot(meta.swatch_c, 8.0F),
        }.With(Spacing{4.0F}, CrossAlign{CrossAxisAlignment::Center}),
        Text(meta.subtitle)
            .Style(TextStyle{Font::System(font_size::kCaption), sub_color})
            .LineLimit(1)
            .Overflow(TextOverflow::Ellipsis),
    }
        .With(Grow(1.0F), Padding{EdgeInsets::Symmetric(12.0F, 10.0F)}, Spacing{6.0F},
              CrossAlign{CrossAxisAlignment::Stretch}, CornerRadius{8.0F},
              Background{VisualFill{selected ? spec.colors.primary_container : spec.colors.surface}},
              Border{selected ? spec.colors.primary : semantic.border_subtle,
                     selected ? 2.0F : 1.0F})
        .OnClick([settings, notify, preset = meta.preset] {
          if (!settings->SetThemePreset(preset) && notify) {
            notify("主题设置保存失败");
          }
        })
        .Key("theme-preset-" + std::string(ThemePresetKey(meta.preset)));
  };

  Views preset_rows;
  preset_rows.Reserve((presets.size() + 1) / 2);
  for (std::size_t i = 0; i < presets.size(); i += 2) {
    View left_card = make_preset_card(presets[i]);
    View right_slot =
        (i + 1 < presets.size()) ? make_preset_card(presets[i + 1]) : View(Spacer().With(Grow(1.0F)));
    preset_rows.Add(Row{std::move(left_card), std::move(right_slot)}
                        .With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch})
                        .Key("theme-preset-row-" + std::to_string(i)));
  }

  View body = Column{
      Text("设置", TextRole::Title),
      Caption("明暗模式"),
      SegmentedButton({"跟随系统", "深色", "浅色"},
                      static_cast<std::size_t>(theme_config.mode))
          .OnChanged([apply_mode](std::size_t index) {
            apply_mode(static_cast<ThemeMode>(index));
          }),
      Caption("配色主题"),
      Column{std::move(preset_rows)}.With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch}),
      Divider(),
      Caption("托盘"),
      Switch("启用托盘图标", settings->tray_enabled)
          .OnChanged([settings, notify](bool value) {
            if (!settings->SetTrayEnabled(value)) {
              notify("托盘设置保存失败");
            }
          }),
      Switch("轻量级模式", settings->lightweight)
          .OnChanged([settings, notify](bool value) {
            if (!settings->SetLightweight(value)) {
              notify("托盘设置保存失败");
            }
          })
          .With(Enabled{settings->tray_enabled.Get()}),
      MutedText("轻量级模式在关闭窗口时销毁界面（会断开数据库连接），从托盘唤出时重建。"),
      MutedText("db++ · ADBC 数据管理器"),
  }.With(Frame{.width = 620.0F}, Spacing{10.0F}, CrossAlign{CrossAxisAlignment::Stretch});

  return ScrollView(Row{
                        Spacer(),
                        std::move(body),
                        Spacer(),
                    }.With(Padding{EdgeInsets::Symmetric(16.0F, 18.0F)}))
      .With(Grow(1.0F));
}

// ---------------------------------------------------------------------------
// Application shell
// ---------------------------------------------------------------------------

[[huxerui::composable]] View AppContent() {
  auto app = UseState(AppModel{});
  auto workspace = UseState(WorkspaceModel{});
  auto show_settings = UseState(false);
  auto toast = UseToast();
  auto dialogs = UseDialog();
  auto menu = UseMenu();
  auto tasks = UseTaskScope();
  auto picker = UseService<FilePicker>();
  auto http = UseService<HttpClient>();
  auto application = UseApplication();
  const auto config_db = UseService<ConfigDbService>();
  const File profile_dir = application.Directories().data_directory;
  const std::string profiles_path = profile_dir.Child("dbpp.sqlite3").Path();
  const File default_file = application.Directories().data_directory.Child("untitled.sqlite");
  auto profiles = UseState(std::vector<ConnectionProfile>{});
  Lifecycle([profiles, profiles_path, config_db, tasks] {
    if (config_db) {
      tasks.Launch([profiles, config_db]() -> Task<void> {
        profiles = co_await config_db->LoadConnectionsAsync();
      });
    } else {
      profiles = LoadConnectionProfiles(profiles_path);
    }
    return [] {};
  });

  const WindowHandle window = UseWindow();
  const auto tray_runtime = UseService<TrayRuntime>();
  const auto settings_model = UseService<SettingsModel>();
  const bool system_dark = UseSystemThemeBrightness() == ThemeBrightness::Dark;
  const AppThemeConfig theme_config = settings_model->ThemeConfig();
  const ResolvedTheme resolved = ResolveTheme(theme_config, system_dark);
  const ThemeSpec& spec = resolved.spec;
  const SemanticColors& semantic = resolved.semantic;

  auto last_toast_id = UseState(std::make_shared<std::optional<LayerId>>());
  const NotifyFn notify = MakeNotify(toast, last_toast_id.Get());

  // 关闭策略：托盘可用时关闭=隐藏（轻量模式=销毁窗口省内存）；托盘不可用时
  // 弹确认对话框退出，避免窗口一关应用就消失且没有入口唤回。
  auto close_dialog_open = UseState(false);
  window.OnCloseRequest(
      [tray_runtime, settings_model, dialogs, close_dialog_open, system_dark]() -> bool {
        if (tray_runtime->exit_requested) {
          return true;
        }
        if (settings_model->tray_enabled.Get() && tray_runtime->tray.IsAvailable() &&
            !tray_runtime->tray_refresh_failed) {
          if (settings_model->lightweight.Get()) {
            // 轻量级模式走正常关闭路径销毁原生窗口与 UI；Runtime 由
            // quit_on_last_window_closed = false 保活，托盘负责下次唤出重建。
            tray_runtime->retired = true;
            return false;
          }
          tray_runtime->HideWindow();
          return true;
        }
        if (close_dialog_open.Get()) {
          return true;
        }
        close_dialog_open = true;
        const AppThemeConfig current_theme = settings_model->ThemeConfig();
        dialogs.Show([tray_runtime, close_dialog_open, current_theme,
                      system_dark](DialogContext dialog) {
          return WithDialogTheme(
              current_theme, system_dark,
              DialogCard(Column{
                  Row{
                      Text("关闭 db++？", TextRole::Title),
                      Spacer(),
                      ActionButton(Button("取消").OnClick([dialog, close_dialog_open] {
                        close_dialog_open = false;
                        dialog.Dismiss();
                      })),
                      ActionButton(Button("退出").OnClick(
                                       [dialog, tray_runtime, close_dialog_open] {
                                         close_dialog_open = false;
                                         dialog.Dismiss();
                                         tray_runtime->RequestExit();
                                       }),
                                   ButtonTone::DangerSolid),
                  }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{8.0F}),
                  MutedText("托盘不可用；退出会关闭全部数据库连接。"),
              }.With(Spacing{12.0F}, CrossAlign{CrossAxisAlignment::Stretch})
                             .With(Frame{.width = 480.0F})));
        });
        return true;
      });

  // Optional startup connection: `DBPP_OPEN=/path/to.db dbpp` or
  // `DBPP_DRIVER=/path/to/libadbc_driver_x.so DBPP_OPEN=uri dbpp`.
  // Also register the tray handler so secondary single-instance launches
  // can activate this window and forward additional open requests.
  Lifecycle([tray_runtime, app, workspace, tasks, notify] {
    const auto dispatch_open = [app, workspace, tasks, notify](InstanceOpenRequest req) {
      if (req.uri.empty()) {
        return;
      }
      ConnectionRequest request{
          .driver_path =
              req.driver_path.empty() ? InferDriverForUri(req.uri) : std::move(req.driver_path),
          .uri = std::move(req.uri),
          .username = {},
          .password = {},
          .title = {},
      };
      OpenConnection(app, workspace, tasks, std::move(request), notify);
    };

    if (const auto startup = TakeEnvOpenRequest()) {
      dispatch_open(*startup);
    }
    tray_runtime->SetOpenRequestHandler(std::move(dispatch_open));
    return [tray_runtime] { tray_runtime->ClearOpenRequestHandler(); };
  });

  const auto open_manager = [dialogs, app, workspace, profiles, tasks, http, profiles_path,
                             default_file, notify, theme_config,
                             system_dark](std::size_t initial_tab, std::string initial_filter) {
    AdbcManagerContext context{
        .app = app,
        .workspace = workspace,
        .profiles = profiles,
        .tasks = tasks,
        .http = http,
        .profiles_path = profiles_path,
        .default_driver = DefaultSqliteDriverPath(),
        .default_uri = default_file.Path(),
        .initial_tab = initial_tab,
        .initial_filter = std::move(initial_filter),
        .theme_config = theme_config,
        .system_dark = system_dark,
        .notify = notify,
    };
    dialogs.Show([context](DialogContext dialog) {
      return WithDialogTheme(context.theme_config, context.system_dark,
                             AdbcManagerDialog(dialog, context));
    });
  };

  const auto open_new = [dialogs, app, workspace, profiles, tasks, picker, http, profiles_path,
                         notify, default_file, theme_config, system_dark, open_manager] {
    ConnectContext context{
        .app = app,
        .workspace = workspace,
        .profiles = profiles,
        .tasks = tasks,
        .picker = picker,
        .http = http,
        .profiles_path = profiles_path,
        .default_driver = DefaultSqliteDriverPath(),
        .default_uri = default_file.Path(),
        .theme_config = theme_config,
        .system_dark = system_dark,
        .notify = notify,
        .open_manager = open_manager,
    };
    dialogs.Show([context](DialogContext dialog) {
      return WithDialogTheme(context.theme_config, context.system_dark,
                             ConnectDialog(dialog, context));
    });
  };

  const auto open_new_query = [app, workspace, show_settings, notify] {
    show_settings = false;
    const DbSession* active_session = app.Get().ActiveSession();
    if (active_session == nullptr) {
      notify("请先连接或选择左侧数据库连接，再新建查询");
      return;
    }
    (void)OpenSqlTab(app, workspace, active_session->id);
  };

  const auto connect_profile = [app, workspace, tasks, notify,
                                show_settings](ConnectionProfile profile) {
    show_settings = false;
    const SanitizedUriResult sanitized = SanitizeUriCredentials(profile.uri);
    ConnectionRequest request{
        .driver_path = profile.driver_path,
        .uri = sanitized.uri,
        .username = profile.username.empty() ? sanitized.username : profile.username,
        .password = profile.password.empty() ? sanitized.password : profile.password,
        .title = profile.name.empty() ? UriDisplayName(sanitized.uri) : profile.name,
    };
    OpenConnection(app, workspace, tasks, std::move(request), notify);
  };

  const auto request_close_tabs = [dialogs, app, workspace, theme_config,
                                   system_dark](std::vector<std::uint64_t> ids) {
    if (ids.empty()) {
      return;
    }
    std::size_t draft_count = 0;
    const WorkspaceModel& ws_snapshot = workspace.Get();
    for (const std::uint64_t id : ids) {
      if (const WorkspaceTab* target = ws_snapshot.FindTab(id);
          target != nullptr && target->kind == WorkspaceTabKind::Table) {
        draft_count += target->table.pending_inserts.size();
      }
    }
    if (draft_count == 0) {
      CloseWorkspaceTabs(app, workspace, ids);
      return;
    }
    dialogs.Show([app, workspace, ids = std::move(ids), draft_count, theme_config,
                  system_dark](DialogContext dialog) {
      return WithDialogTheme(theme_config, system_dark,
                             ConfirmCloseTabsDialog(dialog, app, workspace, ids, draft_count));
    });
  };

  const AppModel& model = app.Get();
  const WorkspaceModel& ws = workspace.Get();
  const WorkspaceTab* active_tab = ws.ActiveTab();

  // 标题栏右上角的齿轮是设置页唯一入口；打开时给图标一个激活底色，
  // 让「齿轮 = 开关」的语义可见。SVG 固有 16px，Padding 只扩点击热区。
  View gear_button = Image(app::images::settings)
                         .Fit(ImageFit::Contain)
                         .Tint(show_settings.Get() ? spec.colors.primary : spec.colors.on_surface)
                         .With(Padding{EdgeInsets::Symmetric(6.0F, 6.0F)},
                               CornerRadius{6.0F})
                         .OnClick([show_settings] {
                           show_settings = !show_settings.Get();
                         });
  if (show_settings.Get()) {
    gear_button = std::move(gear_button)
                      .With(Background{VisualFill{spec.colors.primary_container}},
                            Border{WithAlpha(spec.colors.primary, 0.45F), 1.0F});
  }

  View title_bar = WindowTitleBar{
      TitleBarButton("新建连接",
                     [show_settings, open_new] {
                       show_settings = false;
                       open_new();
                     }),
      TitleBarButton("新建查询", open_new_query,
                     !show_settings.Get() && active_tab != nullptr &&
                         active_tab->kind == WorkspaceTabKind::Sql),
      TitleBarButton("驱动管理",
                     [show_settings, open_manager] {
                       show_settings = false;
                       open_manager(1, {});
                     }),
      Spacer(),
      std::move(gear_button),
  }.With(Padding{EdgeInsets::Symmetric(10.0F, 0.0F)}, Spacing{6.0F},
         CrossAlign{CrossAxisAlignment::Center},
         Background{VisualFill{spec.colors.surface_container_low}});

  View main_content;
  if (model.sessions.empty() && ws.tabs.empty()) {
    main_content = BigEmptyState(open_new);
  } else {
    Views tab_items;
    tab_items.Reserve(ws.tabs.size());
    for (const WorkspaceTab& t : ws.tabs) {
      const std::uint64_t tid = t.id;
      const bool dirty = t.kind == WorkspaceTabKind::Table && !t.table.pending_inserts.empty();
      const std::string sublabel = model.sessions.size() > 1 ? t.session_name : std::string{};
      std::vector<std::uint64_t> all_ids;
      std::vector<std::uint64_t> other_ids;
      all_ids.reserve(ws.tabs.size());
      other_ids.reserve(ws.tabs.size());
      for (const WorkspaceTab& other : ws.tabs) {
        all_ids.push_back(other.id);
        if (other.id != tid) {
          other_ids.push_back(other.id);
        }
      }

      const Color tab_accent = t.kind == WorkspaceTabKind::Sql
                                   ? semantic.accent_sql
                                   : (t.is_view ? semantic.accent_view : semantic.accent_table);
      DatabaseBrand tab_brand = DetectDatabaseBrand({}, t.session_uri);
      for (const DbSession& open : model.sessions) {
        if (open.id == t.session_id) {
          tab_brand = DetectDatabaseBrand(open.driver_path, open.uri, open.dialect);
          break;
        }
      }
      tab_items.Add(
          WorkspaceTabItem(
              WorkspaceTabItemOptions{
                  .label = t.title,
                  .sublabel = sublabel,
                  .active = tid == ws.active_tab_id,
                  .dirty = dirty,
                  .kind_accent = tab_accent,
                  .is_sql = t.kind == WorkspaceTabKind::Sql,
                  .leading_icon = DatabaseIcon(tab_brand, 14.0F),
              },
              [app, workspace, tid] { ActivateWorkspaceTab(app, workspace, tid); },
              [request_close_tabs, tid] { request_close_tabs({tid}); })
              .On<ViewEvents::ContextMenuRequested>(
                  [menu, request_close_tabs, tid, other_ids, all_ids](Point position) {
                    std::vector<MenuEntry> entries;
                    entries.emplace_back(
                        MenuItem("关闭标签页", [request_close_tabs, tid] {
                          request_close_tabs({tid});
                        }));
                    if (!other_ids.empty()) {
                      entries.emplace_back(
                          MenuItem("关闭其它标签页", [request_close_tabs, other_ids] {
                            request_close_tabs(other_ids);
                          }));
                    }
                    entries.emplace_back(MenuSection{});
                    entries.emplace_back(
                        MenuItem("关闭全部标签页", [request_close_tabs, all_ids] {
                          request_close_tabs(all_ids);
                        }));
                    menu.ShowAt(position, std::move(entries));
                  })
              .Key("ws-tab-" + std::to_string(tid)));
    }

    View tab_strip =
        Row{
            ScrollView(Row{std::move(tab_items)}
                           .With(Spacing{4.0F}, CrossAlign{CrossAxisAlignment::Center}))
                .ScrollAxis(Axis::Horizontal)
                .With(Grow(1.0F), Frame{.height = 34.0F}),
            TitleBarButton("＋", open_new_query),
        }
            .With(Spacing{6.0F}, CrossAlign{CrossAxisAlignment::Center});

    View active_body;
    if (active_tab == nullptr) {
      active_body =
          TableEmptyState("从左侧选择一个表或视图，或点击「新建查询」打开新标签页");
    } else if (active_tab->kind == WorkspaceTabKind::Table) {
      active_body = TableBrowserPage(app, workspace, active_tab->id, tasks, notify,
                                     theme_config, system_dark)
                        .Key("tab-body-" + std::to_string(active_tab->id));
    } else {
      active_body = SqlPage(app, workspace, active_tab->id, tasks, notify)
                        .Key("tab-body-" + std::to_string(active_tab->id));
    }

    main_content = Column{
        std::move(tab_strip),
        Divider(),
        std::move(active_body),
    }.With(Padding{EdgeInsets::Symmetric(16.0F, 10.0F)}, Spacing{8.0F},
           CrossAlign{CrossAxisAlignment::Stretch});
  }

  // 设置页覆盖整个窗口（含侧栏），标题栏齿轮是唯一开关入口。
  View page_content;
  if (show_settings.Get()) {
    page_content = SettingsPage(notify);
  } else {
    page_content = Row{
        Sidebar(app, workspace, profiles, profiles_path, tasks, notify, theme_config,
                system_dark, connect_profile),
        std::move(main_content).With(Grow(1.0F)),
    }.With(Grow(1.0F), CrossAlign{CrossAxisAlignment::Stretch});
  }

  const float strip_h = semantic.uses_gradient_chrome ? 2.0F : 1.0F;
  View top_strip = Spacer().With(
      Grow(0.0F), Frame{.height = strip_h, .min_height = strip_h, .max_height = strip_h},
      Background{semantic.accent_gradient});

  View shell = Column{
      std::move(title_bar),
      std::move(top_strip),
      std::move(page_content),
  }.With(Grow(1.0F), CrossAlign{CrossAxisAlignment::Stretch},
         Background{VisualFill{spec.colors.background}});

  return Theme(AppThemeDefinition(resolved), std::move(shell));
}

} // namespace dbpp

huxerui::View AppRoot() {
  return dbpp::AppContent();
}

const huxerui::Application application{
    AppRoot,
    {
        .window = dbpp::MainWindowOptions(),
        // The tray owns application lifetime: startup opens the window from a
        // hook task, and lightweight mode may destroy the window while the
        // tray keeps the runtime alive for a later OpenWindow rebuild.
        .create_initial_window = false,
        .show_debug_overlay = false,
        .quit_on_last_window_closed = false,
        .application_hooks = {dbpp::InstallTrayRuntime},
    },
};
