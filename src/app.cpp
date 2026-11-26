#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <huxerui/datatable.h>
#include <huxerui/huxerui.h>
#include "app_resources.h"

import dbpp.settings;
import dbpp.i18n;
import dbpp.types;
import dbpp.adbc;
import dbpp.redis;
import dbpp.driver_installer;
import dbpp.db;
import dbpp.mcp;
import dbpp.config_db;
import dbpp.instance;

#ifndef DBPP_VERSION
#define DBPP_VERSION "0.3.6"
#endif

#include "theme_palette.h"
#include "theme.h"
#include "root.h"
#include "tray.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
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

bool OpenExternalUrl(std::string_view url) {
  if (!url.starts_with("https://") && !url.starts_with("http://")) {
    return false;
  }
  const std::string owned(url);
#ifdef _WIN32
  const int wide_len = ::MultiByteToWideChar(CP_UTF8, 0, owned.c_str(), -1, nullptr, 0);
  if (wide_len <= 0) {
    return false;
  }
  std::wstring wide(static_cast<std::size_t>(wide_len), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, owned.c_str(), -1, wide.data(), wide_len);
  const auto res = ::ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  return reinterpret_cast<std::intptr_t>(res) > 32;
#else
  const pid_t pid = ::fork();
  if (pid < 0) {
    return false;
  }
  if (pid == 0) {
    const pid_t grandchild = ::fork();
    if (grandchild == 0) {
#ifdef __APPLE__
      ::execlp("open", "open", owned.c_str(), nullptr);
#else
      ::execlp("xdg-open", "xdg-open", owned.c_str(), nullptr);
#endif
      ::_exit(127);
    }
    ::_exit(grandchild < 0 ? 127 : 0);
  }
  int status = 0;
  (void)::waitpid(pid, &status, 0);
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
}

const ImageResource& ObjectGroupIconResource(std::string_view key) {
  if (key == "databases") {
    return app::images::database;
  }
  if (key == "views") {
    return app::images::view;
  }
  if (key == "mviews") {
    return app::images::mview;
  }
  if (key == "foreign") {
    return app::images::foreign_table;
  }
  if (key == "tables") {
    return app::images::table;
  }
  return app::images::folder;
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
  return DisplayNameFromUri(uri, Tr("ADBC Connection", "ADBC 连接"));
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
         text.find("没有") != std::string_view::npos ||
         ContainsIgnoreCaseAscii(text, "failed") ||
         ContainsIgnoreCaseAscii(text, "error") ||
         ContainsIgnoreCaseAscii(text, "cannot") ||
         ContainsIgnoreCaseAscii(text, "unable") ||
         ContainsIgnoreCaseAscii(text, "not found") ||
         ContainsIgnoreCaseAscii(text, "unavailable") ||
         ContainsIgnoreCaseAscii(text, "unsupported") ||
         ContainsIgnoreCaseAscii(text, "invalid") ||
         ContainsIgnoreCaseAscii(text, "missing") ||
         ContainsIgnoreCaseAscii(text, "timed out") ||
         ContainsIgnoreCaseAscii(text, "timeout");
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

struct LocalizedCatalog {
  std::shared_ptr<Resources> resources;
  std::shared_ptr<SettingsModel> settings;

  [[nodiscard]] ResourceConfiguration Config() const {
    const LanguageMode mode = settings ? settings->language.Get() : LanguageMode::System;
    const Locale platform =
        resources ? resources->Configuration().locale : Locale::Default();
    return ResourceConfiguration{.locale = EffectiveLocale(mode, platform)};
  }

  [[nodiscard]] std::string Get(const StringVariant& value) const {
    if (!resources) {
      return {};
    }
    return resources->GetString(value, Config());
  }

  template <class... Args>
  [[nodiscard]] std::string Fmt(StringResource resource, Args&&... args) const {
    return Get(StringVariant::Format(std::move(resource), std::forward<Args>(args)...));
  }
};

struct LocalizedPresetStrings {
  StringResource name;
  StringResource subtitle;
  std::optional<StringResource> uri_placeholder;
};

LocalizedPresetStrings DatabasePresetStrings(const DatabasePreset& preset) {
  if (preset.id == "sqlite") {
    return {app::strings::db_preset_sqlite_name, app::strings::db_preset_sqlite_sub,
            app::strings::db_preset_sqlite_ph};
  }
  if (preset.id == "postgresql") {
    return {app::strings::db_preset_postgresql_name, app::strings::db_preset_postgresql_sub,
            std::nullopt};
  }
  if (preset.id == "mysql") {
    return {app::strings::db_preset_mysql_name, app::strings::db_preset_mysql_sub, std::nullopt};
  }
  if (preset.id == "mariadb") {
    return {app::strings::db_preset_mariadb_name, app::strings::db_preset_mariadb_sub,
            std::nullopt};
  }
  if (preset.id == "duckdb") {
    return {app::strings::db_preset_duckdb_name, app::strings::db_preset_duckdb_sub,
            app::strings::db_preset_duckdb_ph};
  }
  if (preset.id == "redis") {
    return {app::strings::db_preset_redis_name, app::strings::db_preset_redis_sub, std::nullopt};
  }
  if (preset.id == "mssql") {
    return {app::strings::db_preset_mssql_name, app::strings::db_preset_mssql_sub, std::nullopt};
  }
  if (preset.id == "clickhouse") {
    return {app::strings::db_preset_clickhouse_name, app::strings::db_preset_clickhouse_sub,
            std::nullopt};
  }
  if (preset.id == "flightsql") {
    return {app::strings::db_preset_flightsql_name, app::strings::db_preset_flightsql_sub,
            std::nullopt};
  }
  if (preset.id == "snowflake") {
    return {app::strings::db_preset_snowflake_name, app::strings::db_preset_snowflake_sub,
            std::nullopt};
  }
  if (preset.id == "bigquery") {
    return {app::strings::db_preset_bigquery_name, app::strings::db_preset_bigquery_sub,
            std::nullopt};
  }
  if (preset.id == "trino") {
    return {app::strings::db_preset_trino_name, app::strings::db_preset_trino_sub, std::nullopt};
  }
  if (preset.id == "databricks") {
    return {app::strings::db_preset_databricks_name, app::strings::db_preset_databricks_sub,
            std::nullopt};
  }
  if (preset.id == "redshift") {
    return {app::strings::db_preset_redshift_name, app::strings::db_preset_redshift_sub,
            std::nullopt};
  }
  return {app::strings::db_preset_custom_name, app::strings::db_preset_custom_sub, std::nullopt};
}

std::pair<StringResource, StringResource> ThemePresetStrings(ThemePreset preset) {
  switch (preset) {
    case ThemePreset::Emerald:
      return {app::strings::theme_preset_emerald_name, app::strings::theme_preset_emerald_sub};
    case ThemePreset::WarmAmber:
      return {app::strings::theme_preset_warm_amber_name,
              app::strings::theme_preset_warm_amber_sub};
    case ThemePreset::VioletDusk:
      return {app::strings::theme_preset_violet_dusk_name,
              app::strings::theme_preset_violet_dusk_sub};
    case ThemePreset::Monochrome:
      return {app::strings::theme_preset_monochrome_name,
              app::strings::theme_preset_monochrome_sub};
    case ThemePreset::GeminiSpark:
    default:
      return {app::strings::theme_preset_gemini_name, app::strings::theme_preset_gemini_sub};
  }
}

View WithDialogTheme(const AppThemeConfig& config, LanguageMode language_mode, bool system_dark,
                     View content) {
  View themed = Theme(AppThemeDefinition(ResolveTheme(config, system_dark)), std::move(content));
  if (auto override_locale = EffectiveLocaleOverride(language_mode)) {
    return ProvideEnvironment(*override_locale, std::move(themed));
  }
  return themed;
}

[[huxerui::composable]] View TableEmptyState(StringVariant text) {
  return Column{
      MutedText(UseString(std::move(text))),
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
      MutedText(UseString(app::strings::app_subtitle)),
      ActionButton(Button(app::strings::empty_new_connection).OnClick(std::move(open_new)),
                   ButtonTone::Primary),
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

struct ConnectionFormStates {
  State<std::string> driver_slug;
  State<TextEditingValue> driver;
  State<TextEditingValue> uri;
  State<TextEditingValue> host;
  State<TextEditingValue> port;
  State<TextEditingValue> database;
  State<TextEditingValue> username;
  State<TextEditingValue> password;
  State<TextEditingValue> title;
  State<std::string> extra_query;
  State<std::size_t> input_mode;
};

[[huxerui::composable]] View ConnectionFormFields(
    const DatabasePreset& preset, const std::string& active_preset_placeholder,
    const ConnectionFormStates& form, const std::vector<AdbcDriverInfo>& driver_list,
    bool can_pick_file, std::function<void()> pick_local_file,
    std::function<void()> on_password_submit, StringVariant name_placeholder) {
  Views form_fields;
  if (preset.form_kind == ConnectionFormKind::File) {
    form_fields.Add(
        Row{
            TextField(form.uri)
                .Label(app::strings::connect_file_label)
                .Placeholder(active_preset_placeholder)
                .Variant(TextFieldVariant::Outlined)
                .LineLimits(TextFieldLineLimits::SingleLine())
                .OnChanged([uri = form.uri](const TextEditingValue& value) { uri = value; })
                .With(Grow(1.0F)),
            ActionButton(Button(app::strings::connect_browse_btn).OnClick(std::move(pick_local_file)))
                .With(Enabled{can_pick_file}),
        }.With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Center}));
    form_fields.Add(
        TextField(form.title)
            .Label(app::strings::connect_name_label)
            .Placeholder(std::move(name_placeholder))
            .Variant(TextFieldVariant::Outlined)
            .LineLimits(TextFieldLineLimits::SingleLine())
            .OnChanged([title = form.title](const TextEditingValue& value) { title = value; }));
    form_fields.Add(
        MonospaceField(
            TextField(form.driver)
                .Label(app::strings::connect_driver_label)
                .Variant(TextFieldVariant::Outlined)
                .LineLimits(TextFieldLineLimits::SingleLine())
                .OnChanged([driver = form.driver](const TextEditingValue& value) { driver = value; })));
    return Column{std::move(form_fields)}
        .With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch});
  }

  if (preset.form_kind == ConnectionFormKind::HostPort) {
    form_fields.Add(
        SegmentedButton({app::strings::connect_mode_form, app::strings::connect_mode_uri},
                        form.input_mode.Get())
            .OnChanged([form, preset](std::size_t idx) {
              if (idx == 1 && form.input_mode.Get() == 0) {
                form.uri = TextEditingValue::FromText(
                    ComposePresetUri(preset, form.host.Get().text, form.port.Get().text,
                                     form.database.Get().text, form.extra_query.Get()));
              } else if (idx == 0 && form.input_mode.Get() == 1) {
                const DecomposedPresetUri parts = DecomposePresetUri(preset, form.uri.Get().text);
                if (parts.parsed) {
                  form.host = TextEditingValue::FromText(parts.host);
                  form.port = TextEditingValue::FromText(
                      parts.port.empty() ? preset.default_port : parts.port);
                  form.database = TextEditingValue::FromText(parts.database);
                  form.extra_query = parts.extra_query;
                  if (!parts.username.empty()) {
                    form.username = TextEditingValue::FromText(parts.username);
                  }
                  if (!parts.password.empty()) {
                    form.password = TextEditingValue::FromText(parts.password);
                  }
                }
              }
              form.input_mode = idx;
            }));
    if (form.input_mode.Get() == 0) {
      form_fields.Add(
          Row{
              TextField(form.host)
                  .Label(app::strings::connect_host_label)
                  .Placeholder(preset.default_host)
                  .Variant(TextFieldVariant::Outlined)
                  .LineLimits(TextFieldLineLimits::SingleLine())
                  .OnChanged([host = form.host](const TextEditingValue& value) { host = value; })
                  .With(Grow(2.0F)),
              TextField(form.port)
                  .Label(app::strings::connect_port_label)
                  .Placeholder(preset.default_port)
                  .Variant(TextFieldVariant::Outlined)
                  .LineLimits(TextFieldLineLimits::SingleLine())
                  .OnChanged([port = form.port](const TextEditingValue& value) { port = value; })
                  .With(Grow(1.0F)),
          }.With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch}));
      form_fields.Add(
          TextField(form.database)
              .Label(preset.id == "redis" ? app::strings::connect_redis_db_label
                                          : app::strings::connect_db_label)
              .Placeholder(preset.default_database)
              .Variant(TextFieldVariant::Outlined)
              .LineLimits(TextFieldLineLimits::SingleLine())
              .OnChanged([database = form.database](const TextEditingValue& value) { database = value; }));
      form_fields.Add(
          Row{
              TextField(form.username)
                  .Label(preset.id == "redis" ? app::strings::connect_user_acl_label
                                              : app::strings::connect_user_label)
                  .Variant(TextFieldVariant::Outlined)
                  .LineLimits(TextFieldLineLimits::SingleLine())
                  .OnChanged([username = form.username](const TextEditingValue& value) { username = value; })
                  .With(Grow(1.0F)),
              PasswordField(form.password, app::strings::connect_password_opt_label,
                            on_password_submit)
                  .With(Grow(1.0F)),
          }.With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch}));
      const std::string preview =
          ComposePresetUri(preset, form.host.Get().text, form.port.Get().text,
                           form.database.Get().text, form.extra_query.Get());
      form_fields.Add(MutedText(UseString(app::strings::connect_uri_preview, preview)));
    } else {
      form_fields.Add(
          MonospaceField(
              TextField(form.uri)
                  .Label(app::strings::connect_uri_label)
                  .Placeholder(active_preset_placeholder)
                  .Variant(TextFieldVariant::Outlined)
                  .LineLimits(TextFieldLineLimits::SingleLine())
                  .OnChanged([uri = form.uri](const TextEditingValue& value) { uri = value; })));
      form_fields.Add(
          Row{
              TextField(form.username)
                  .Label(app::strings::connect_user_opt_label)
                  .Variant(TextFieldVariant::Outlined)
                  .LineLimits(TextFieldLineLimits::SingleLine())
                  .OnChanged([username = form.username](const TextEditingValue& value) { username = value; })
                  .With(Grow(1.0F)),
              PasswordField(form.password, app::strings::connect_password_opt_label,
                            on_password_submit)
                  .With(Grow(1.0F)),
          }.With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch}));
      form_fields.Add(
          MonospaceField(
              TextField(form.driver)
                  .Label(app::strings::connect_driver_name_or_path_label)
                  .Variant(TextFieldVariant::Outlined)
                  .LineLimits(TextFieldLineLimits::SingleLine())
                  .OnChanged([driver = form.driver](const TextEditingValue& value) { driver = value; })));
    }
    form_fields.Add(
        TextField(form.title)
            .Label(app::strings::connect_name_label)
            .Placeholder(std::move(name_placeholder))
            .Variant(TextFieldVariant::Outlined)
            .LineLimits(TextFieldLineLimits::SingleLine())
            .OnChanged([title = form.title](const TextEditingValue& value) { title = value; }));
    return Column{std::move(form_fields)}
        .With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch});
  }

  if (preset.form_kind == ConnectionFormKind::Uri) {
    form_fields.Add(
        MonospaceField(
            TextField(form.uri)
                .Label(app::strings::connect_uri_label)
                .Placeholder(active_preset_placeholder)
                .Variant(TextFieldVariant::Outlined)
                .LineLimits(TextFieldLineLimits::SingleLine())
                .OnChanged([uri = form.uri](const TextEditingValue& value) { uri = value; })));
    form_fields.Add(
        Row{
            TextField(form.username)
                .Label(app::strings::connect_user_opt_label)
                .Variant(TextFieldVariant::Outlined)
                .LineLimits(TextFieldLineLimits::SingleLine())
                .OnChanged([username = form.username](const TextEditingValue& value) { username = value; })
                .With(Grow(1.0F)),
            PasswordField(form.password, app::strings::connect_password_opt_label,
                          on_password_submit)
                .With(Grow(1.0F)),
        }.With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch}));
    form_fields.Add(
        MonospaceField(
            TextField(form.driver)
                .Label(app::strings::connect_driver_name_or_path_label)
                .Variant(TextFieldVariant::Outlined)
                .LineLimits(TextFieldLineLimits::SingleLine())
                .OnChanged([driver = form.driver](const TextEditingValue& value) { driver = value; })));
    form_fields.Add(
        TextField(form.title)
            .Label(app::strings::connect_name_label)
            .Placeholder(std::move(name_placeholder))
            .Variant(TextFieldVariant::Outlined)
            .LineLimits(TextFieldLineLimits::SingleLine())
            .OnChanged([title = form.title](const TextEditingValue& value) { title = value; }));
    return Column{std::move(form_fields)}
        .With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch});
  }

  Views discovered_rows;
  discovered_rows.Reserve(driver_list.size());
  for (const AdbcDriverInfo& info : driver_list) {
    const std::string& detail = info.detail.empty() ? info.path : info.detail;
    discovered_rows.Add(
        NavItem(info.name, detail, form.driver.Get().text == info.path,
                [driver = form.driver, info] { driver = TextEditingValue::FromText(info.path); },
                {}, std::nullopt,
                DatabaseIcon(DetectDatabaseBrand(info.path + " " + info.name), 18.0F))
            .Key("custom-driver-" + info.path + "-" + detail));
  }
  if (discovered_rows.Size() > 0) {
    form_fields.Add(Caption(UseString(app::strings::connect_discovered_drivers)));
    form_fields.Add(
        ScrollView(Column{std::move(discovered_rows)}.With(Spacing{2.0F}))
            .With(Frame{.height = 110.0F}));
  }
  form_fields.Add(
      MonospaceField(
          TextField(form.driver)
              .Label(app::strings::connect_driver_custom_label)
              .Placeholder(app::strings::connect_driver_custom_placeholder)
              .Variant(TextFieldVariant::Outlined)
              .LineLimits(TextFieldLineLimits::SingleLine())
              .OnChanged([driver = form.driver](const TextEditingValue& value) { driver = value; })));
  form_fields.Add(
      MonospaceField(
          TextField(form.uri)
              .Label(app::strings::adbc_uri_field_label)
              .Placeholder(active_preset_placeholder)
              .Variant(TextFieldVariant::Outlined)
              .LineLimits(TextFieldLineLimits::SingleLine())
              .OnChanged([uri = form.uri](const TextEditingValue& value) { uri = value; })));
  form_fields.Add(
      Row{
          TextField(form.username)
              .Label(app::strings::connect_user_opt_label)
              .Variant(TextFieldVariant::Outlined)
              .LineLimits(TextFieldLineLimits::SingleLine())
              .OnChanged([username = form.username](const TextEditingValue& value) { username = value; })
              .With(Grow(1.0F)),
          PasswordField(form.password, app::strings::connect_password_opt_label,
                        std::move(on_password_submit))
              .With(Grow(1.0F)),
      }.With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch}));
  form_fields.Add(
      TextField(form.title)
          .Label(app::strings::connect_name_label)
          .Placeholder(std::move(name_placeholder))
          .Variant(TextFieldVariant::Outlined)
          .LineLimits(TextFieldLineLimits::SingleLine())
          .OnChanged([title = form.title](const TextEditingValue& value) { title = value; }));
  return Column{std::move(form_fields)}
      .With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch});
}

[[huxerui::composable]] View EditConnectionProfileDialog(
    DialogContext dialog, std::optional<ConnectionProfile> original_profile,
    ConnectionProfile initial_values, std::optional<std::uint64_t> active_session_id,
    State<AppModel> app, State<WorkspaceModel> workspace,
    State<std::vector<ConnectionProfile>> profiles, std::string profiles_path, TaskScope tasks,
    NotifyFn notify) {
  const auto config_db = UseService<ConfigDbService>();
  const auto picker = UseService<FilePicker>();
  const LocalizedCatalog loc{UseService<Resources>(), UseService<SettingsModel>()};
  auto dialog_tasks = UseTaskScope();

  const DatabasePreset initial_preset =
      ResolvePresetForProfile(initial_values.driver_path, initial_values.uri);
  const DecomposedPresetUri initial_parts =
      DecomposePresetUri(initial_preset, initial_values.uri);
  const std::string init_user =
      !initial_values.username.empty() ? initial_values.username : initial_parts.username;
  const std::string init_pass =
      !initial_values.password.empty() ? initial_values.password : initial_parts.password;

  ConnectionFormStates form{
      .driver_slug = UseState(initial_preset.id),
      .driver = UseState(TextEditingValue::FromText(initial_values.driver_path)),
      .uri = UseState(TextEditingValue::FromText(initial_values.uri)),
      .host = UseState(TextEditingValue::FromText(
          initial_parts.parsed ? initial_parts.host : initial_preset.default_host)),
      .port = UseState(TextEditingValue::FromText(
          !initial_parts.port.empty() ? initial_parts.port : initial_preset.default_port)),
      .database = UseState(TextEditingValue::FromText(
          initial_parts.parsed ? initial_parts.database : initial_preset.default_database)),
      .username = UseState(TextEditingValue::FromText(init_user)),
      .password = UseState(TextEditingValue::FromText(init_pass)),
      .title = UseState(TextEditingValue::FromText(initial_values.name)),
      .extra_query = UseState(initial_parts.extra_query),
      .input_mode = UseState<std::size_t>(
          (initial_preset.form_kind == ConnectionFormKind::HostPort && initial_parts.parsed) ? 0
                                                                                             : 1),
  };

  auto drivers = UseState(std::vector<AdbcDriverInfo>{});
  Lifecycle([drivers] {
    drivers = DiscoverAdbcDrivers();
    return [] {};
  });

  const LocalizedPresetStrings preset_strings = DatabasePresetStrings(initial_preset);
  const std::string preset_placeholder = preset_strings.uri_placeholder
                                             ? UseString(*preset_strings.uri_placeholder)
                                             : initial_preset.uri_placeholder;

  const auto pick_local_file = [picker, dialog_tasks, initial_preset, preset_strings, form, notify,
                                loc]() {
    if (!picker || !picker->CanOpenFiles()) {
      if (notify) {
        notify(loc.Get(app::strings::connect_picker_unsupported));
      }
      return;
    }
    const std::string filter_label =
        loc.Fmt(app::strings::connect_picker_filter, loc.Get(preset_strings.name));
    dialog_tasks.Launch([picker, filter_label, extensions = initial_preset.file_extensions, form,
                         notify, loc]() -> Task<void> {
      FilePickerFilter filter;
      filter.name = filter_label;
      filter.extensions = extensions;
      auto file_reference = co_await picker->OpenFileAsync(std::move(filter));
      if (!file_reference) {
        co_return;
      }
      auto file = file_reference->AsFile();
      if (!file) {
        if (notify) {
          notify(loc.Get(app::strings::connect_picker_no_local_path));
        }
        co_return;
      }
      form.uri = TextEditingValue::FromText(file->Path());
      if (form.title.Get().text.empty()) {
        form.title = TextEditingValue::FromText(file->Name());
      }
    });
  };

  const auto persist_profile = [original_profile, initial_preset, form, profiles, profiles_path,
                                config_db, loc, notify](ConnectionProfile& out_profile) {
    const std::string driver_text = TrimmedSql(form.driver.Get().text);
    const std::string raw_uri =
        (initial_preset.form_kind == ConnectionFormKind::HostPort && form.input_mode.Get() == 0)
            ? ComposePresetUri(initial_preset, form.host.Get().text, form.port.Get().text,
                               form.database.Get().text, form.extra_query.Get())
            : form.uri.Get().text;
    const std::string uri_raw = TrimmedSql(raw_uri);
    if (driver_text.empty() || uri_raw.empty()) {
      if (notify) {
        notify(loc.Get(app::strings::toast_driver_uri_required));
      }
      return false;
    }
    const SanitizedUriResult sanitized = SanitizeUriCredentials(uri_raw);
    const std::string title_text = TrimmedSql(form.title.Get().text);
    const std::string user_text =
        form.username.Get().text.empty() ? sanitized.username : form.username.Get().text;
    const std::string pass_text =
        form.password.Get().text.empty() ? sanitized.password : form.password.Get().text;
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
          notify(loc.Fmt(app::strings::toast_save_profiles_path_failed, profiles_path));
        }
        return false;
      }
      profiles = std::move(list);
    }
    return true;
  };

  const auto save_only = [dialog, persist_profile, loc, notify] {
    ConnectionProfile saved;
    if (!persist_profile(saved)) {
      return;
    }
    dialog.Dismiss();
    if (notify) {
      notify(loc.Fmt(app::strings::toast_profile_updated, saved.name));
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

  const DatabaseBrand icon_brand =
      DetectDatabaseBrand(form.driver.Get().text, form.uri.Get().text);
  return DialogCard(Column{
      Row{
          DatabaseIcon(icon_brand, 20.0F),
          Text(app::strings::edit_conn_title, TextRole::Title),
          Spacer(),
          ActionButton(Button(app::strings::action_cancel).OnClick([dialog] { dialog.Dismiss(); })),
          ActionButton(Button(app::strings::action_save).OnClick(save_only)),
          ActionButton(Button(active_session_id.has_value()
                                  ? app::strings::edit_conn_save_reconnect
                                  : app::strings::edit_conn_save_connect)
                           .OnClick(save_and_connect),
                       ButtonTone::Primary),
      }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{8.0F}),
      ScrollView(
          ConnectionFormFields(
              initial_preset, preset_placeholder, form, drivers.Get(),
              picker && picker->CanOpenFiles(), pick_local_file, save_and_connect,
              app::strings::edit_conn_name_placeholder))
          .With(Grow(1.0F), Frame{.max_height = 420.0F}),
  }.With(Spacing{10.0F}, CrossAlign{CrossAxisAlignment::Stretch})
                    .With(Frame{.width = 580.0F}));
}

struct SidebarObjectGroup {
  std::string key;
  StringResource label;
  std::vector<const DbObject*> items;
};

struct SidebarDatabaseGroup {
  std::string name;
  bool is_system = false;
  bool is_target = false;
  bool default_open = true;
  std::size_t object_count = 0;
  std::vector<SidebarObjectGroup> groups;
};

std::vector<SidebarObjectGroup> GroupObjectItems(
    Dialect dialect, const std::vector<const DbObject*>& items) {
  if (dialect == Dialect::Redis) {
    SidebarObjectGroup dbs{
        .key = "redis_dbs", .label = app::strings::sidebar_group_redis_dbs, .items = items};
    std::vector<SidebarObjectGroup> groups;
    if (!dbs.items.empty()) {
      groups.push_back(std::move(dbs));
    }
    return groups;
  }

  SidebarObjectGroup tables{
      .key = "tables", .label = app::strings::sidebar_group_tables, .items = {}};
  SidebarObjectGroup views{
      .key = "views", .label = app::strings::sidebar_group_views, .items = {}};
  SidebarObjectGroup mviews{
      .key = "mviews", .label = app::strings::sidebar_group_mviews, .items = {}};
  SidebarObjectGroup foreign{
      .key = "foreign", .label = app::strings::sidebar_group_foreign, .items = {}};

  for (const DbObject* object : items) {
    if (object->is_view) {
      if (ContainsIgnoreCaseAscii(object->table_type, "materialized")) {
        mviews.items.push_back(object);
      } else {
        views.items.push_back(object);
      }
    } else if (ContainsIgnoreCaseAscii(object->table_type, "foreign")) {
      foreign.items.push_back(object);
    } else {
      tables.items.push_back(object);
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

std::vector<SidebarObjectGroup> GroupSessionObjects(const DbSession& session) {
  std::vector<const DbObject*> all_items;
  all_items.reserve(session.objects.size());
  for (const DbObject& object : session.objects) {
    all_items.push_back(&object);
  }
  return GroupObjectItems(session.dialect, all_items);
}

std::string SessionObjectBucketName(const DbSession& session, const DbObject& object,
                                    bool multi_catalog) {
  if (session.dialect == Dialect::Redis) {
    return {};
  }
  if (session.dialect == Dialect::Mysql) {
    return ObjectDatabaseName(Dialect::Mysql, object);
  }
  if (session.dialect == Dialect::Sqlite) {
    if (!object.db_schema.empty()) {
      return object.db_schema;
    }
    if (!object.catalog.empty()) {
      return object.catalog;
    }
    return "main";
  }
  if (session.dialect == Dialect::Duckdb) {
    const std::string name = ObjectDatabaseName(Dialect::Duckdb, object);
    if (!name.empty()) {
      return name;
    }
    if (!object.catalog.empty()) {
      return object.catalog;
    }
    return "main";
  }
  if (multi_catalog && !object.catalog.empty() &&
      !ContainsIgnoreCaseAscii(object.catalog, "def") &&
      !ContainsIgnoreCaseAscii(object.catalog, "main")) {
    if (!object.db_schema.empty() && object.db_schema != object.catalog) {
      return object.catalog + "." + object.db_schema;
    }
    return object.catalog;
  }
  return ObjectDatabaseName(session.dialect, object);
}

bool SessionHasMultipleCatalogs(const DbSession& session) {
  std::string first_catalog;
  for (const DbObject& object : session.objects) {
    if (object.catalog.empty()) {
      continue;
    }
    std::string lower = object.catalog;
    for (char& ch : lower) {
      ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    if (lower == "def" || lower == "main") {
      continue;
    }
    if (first_catalog.empty()) {
      first_catalog = lower;
    } else if (lower != first_catalog) {
      return true;
    }
  }
  return false;
}

bool SessionUsesDatabaseHierarchy(const DbSession& session) {
  if (session.dialect == Dialect::Redis || session.objects.empty()) {
    return false;
  }
  const bool multi_catalog = SessionHasMultipleCatalogs(session);
  std::vector<std::string> distinct_buckets;
  for (const DbObject& object : session.objects) {
    const std::string bucket = SessionObjectBucketName(session, object, multi_catalog);
    if (bucket.empty()) {
      continue;
    }
    if (std::ranges::find(distinct_buckets, bucket) == distinct_buckets.end()) {
      distinct_buckets.push_back(bucket);
      if (distinct_buckets.size() >= 2) {
        return true;
      }
    }
  }
  if (distinct_buckets.size() == 1) {
    return session.dialect == Dialect::Sqlite || session.dialect == Dialect::Duckdb ||
           session.dialect == Dialect::Mysql || session.dialect == Dialect::Postgresql ||
           ContainsIgnoreCaseAscii(session.driver_path, "mssql") ||
           ContainsIgnoreCaseAscii(session.uri, "mssql://");
  }
  return false;
}

std::vector<SidebarDatabaseGroup> GroupSessionDatabases(const DbSession& session) {
  const bool multi_catalog = SessionHasMultipleCatalogs(session);
  const DatabasePreset& preset = ResolvePresetForProfile(session.driver_path, session.uri);
  const std::string target_db = DecomposePresetUri(preset, session.uri).database;
  std::string target_lower = target_db;
  for (char& ch : target_lower) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  if (target_lower.empty() &&
      (session.dialect == Dialect::Sqlite || session.dialect == Dialect::Duckdb)) {
    target_lower = "main";
  }

  struct RawBucket {
    std::string name;
    bool is_system = false;
    bool is_target = false;
    std::vector<const DbObject*> items;
  };
  std::vector<RawBucket> raw_buckets;
  for (const DbObject& object : session.objects) {
    std::string bucket = SessionObjectBucketName(session, object, multi_catalog);
    if (bucket.empty()) {
      bucket = !target_db.empty() ? target_db : std::string("default");
    }
    auto it = std::ranges::find_if(raw_buckets,
                                   [&bucket](const RawBucket& b) { return b.name == bucket; });
    if (it == raw_buckets.end()) {
      std::string lower_name = bucket;
      for (char& ch : lower_name) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      }
      raw_buckets.push_back(RawBucket{
          .name = bucket,
          .is_system = IsSystemDatabaseOrSchema(bucket),
          .is_target = !target_lower.empty() && lower_name == target_lower,
          .items = {&object},
      });
    } else {
      it->items.push_back(&object);
    }
  }

  std::ranges::sort(raw_buckets, [](const RawBucket& a, const RawBucket& b) {
    if (a.is_target != b.is_target) {
      return a.is_target;
    }
    if (a.is_system != b.is_system) {
      return !a.is_system;
    }
    return a.name < b.name;
  });

  std::size_t non_system_count = 0;
  bool any_target = false;
  for (const RawBucket& b : raw_buckets) {
    if (!b.is_system) {
      ++non_system_count;
    }
    if (b.is_target && !b.is_system) {
      any_target = true;
    }
  }

  std::vector<SidebarDatabaseGroup> result;
  result.reserve(raw_buckets.size());
  bool opened_first_non_system = false;
  bool opened_first_fallback = false;
  for (RawBucket& b : raw_buckets) {
    bool open = false;
    if (!b.is_system) {
      if (non_system_count <= 4 || b.is_target || (!any_target && !opened_first_non_system)) {
        open = true;
        opened_first_non_system = true;
      }
    } else if (non_system_count == 0 && !opened_first_fallback) {
      open = true;
      opened_first_fallback = true;
    }
    result.push_back(SidebarDatabaseGroup{
        .name = std::move(b.name),
        .is_system = b.is_system,
        .is_target = b.is_target,
        .default_open = open,
        .object_count = b.items.size(),
        .groups = GroupObjectItems(session.dialect, b.items),
    });
  }
  return result;
}

bool SessionHasMultipleSchemas(const DbSession& session) {
  std::string first_schema;
  for (const DbObject& object : session.objects) {
    const std::string db_name = ObjectDatabaseName(session.dialect, object);
    if (db_name.empty()) {
      continue;
    }
    if (first_schema.empty()) {
      first_schema = db_name;
    } else if (db_name != first_schema) {
      return true;
    }
  }
  return false;
}

StringResource ObjectKindResource(const DbObject& object) {
  if (ContainsIgnoreCaseAscii(object.table_type, "redis")) {
    return app::strings::object_kind_redis;
  }
  if (object.is_view) {
    return ContainsIgnoreCaseAscii(object.table_type, "materialized")
               ? app::strings::object_kind_mview
               : app::strings::object_kind_view;
  }
  return ContainsIgnoreCaseAscii(object.table_type, "foreign")
             ? app::strings::object_kind_foreign
             : app::strings::object_kind_table;
}

[[huxerui::composable]] View ObjectSchemaDialog(DialogContext dialog,
                                                 std::shared_ptr<AdbcSession> adbc,
                                                 std::uint64_t session_id, DbObject object,
                                                 std::string display_label,
                                                 std::string qualified_name, State<AppModel> app,
                                                 State<WorkspaceModel> workspace, TaskScope tasks,
                                                 NotifyFn notify) {
  const ThemeSpec& spec = UseTheme();
  const SemanticColors& semantic = UseEnvironment<SemanticColors>();
  const LocalizedCatalog loc{UseService<Resources>(), UseService<SettingsModel>()};
  auto loading = UseState(true);
  auto columns = UseState(std::vector<ColumnMeta>(object.columns));
  auto error = UseState(std::string{});
  auto dialog_tasks = UseTaskScope();

  Lifecycle([dialog, dialog_tasks, adbc, session_id, object, app, workspace, tasks, loading,
             columns, error, notify, loc] {
    if (!adbc || adbc->IsClosed()) {
      loading = false;
      error = loc.Get(app::strings::toast_connection_closed);
    } else {
      dialog_tasks.Launch([dialog, adbc, session_id, object, app, workspace, tasks, loading,
                           columns, error, notify, loc]() -> Task<void> {
        DbObject object_copy = object;
        AdbcSchemaResult res = co_await adbc->sequence.Run(
            [adbc, object_copy = std::move(object_copy)](std::stop_token) {
              return LoadSchemaBlocking(*adbc, object_copy);
            });
        loading = false;
        if (res.connection_lost) {
          error = res.error;
          HandleSessionDisconnected(app, workspace, tasks, session_id, res.error, notify);
          dialog.Dismiss();
          co_return;
        }
        if (!res.error.empty()) {
          error = res.error;
          if (notify) {
            notify(loc.Fmt(app::strings::toast_load_schema_failed, res.error));
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
    body_content = LoadingState(app::strings::schema_dialog_loading).With(Frame{.height = 200.0F});
  } else if (cols.empty()) {
    body_content = TableEmptyState(error.Get().empty() ? app::strings::schema_dialog_empty
                                                       : app::strings::schema_dialog_load_failed)
                       .With(Frame{.height = 140.0F});
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
          Text::Format(TextRole::Title, app::strings::schema_dialog_title, display_label),
          Spacer(),
          ActionButton(Button(app::strings::action_close).OnClick([dialog] { dialog.Dismiss(); })),
      }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{8.0F}),
      MutedText(UseString(app::strings::schema_dialog_summary,
                          UseString(ObjectKindResource(object)), qualified_name,
                          cols.size())),
      std::move(body_content),
  }.With(Spacing{10.0F}, CrossAlign{CrossAxisAlignment::Stretch})
                    .With(Frame{.width = 560.0F}));
}

[[huxerui::composable]] View EditTableDialog(
    DialogContext dialog, std::shared_ptr<AdbcSession> adbc, std::uint64_t session_id,
    Dialect dialect, DbObject initial_object, std::string display_label,
    State<AppModel> app, State<WorkspaceModel> workspace, TaskScope tasks, NotifyFn notify) {
  const ThemeSpec& spec = UseTheme();
  const SemanticColors& semantic = UseEnvironment<SemanticColors>();
  const LocalizedCatalog loc{UseService<Resources>(), UseService<SettingsModel>()};
  auto dialog_tasks = UseTaskScope();
  auto current_object = UseState(initial_object);
  auto loading = UseState(true);
  auto busy = UseState(false);
  auto columns = UseState(std::vector<ColumnMeta>(initial_object.columns));
  auto error_text = UseState(std::string{});

  auto table_name_input = UseState(TextEditingValue::FromText(initial_object.name));
  auto selected_col = UseState<int>(-1);
  auto edit_col_name = UseState(TextEditingValue{});
  auto edit_col_type = UseState(TextEditingValue{});
  auto edit_col_nullable = UseState(true);

  auto add_col_name = UseState(TextEditingValue{});
  auto add_col_type = UseState(TextEditingValue::FromText("TEXT"));
  auto add_col_default = UseState(TextEditingValue{});
  auto add_col_nullable = UseState(true);
  auto preview_override = UseState(std::string{});

  const auto reload_columns = [dialog, dialog_tasks, adbc, session_id, current_object, columns,
                               selected_col, edit_col_name, edit_col_type, edit_col_nullable,
                               loading, error_text, app, workspace, tasks, notify, loc] {
    if (!adbc || adbc->IsClosed()) {
      loading = false;
      error_text = loc.Get(app::strings::toast_connection_closed);
      return;
    }
    loading = true;
    const DbObject obj = current_object.Get();
    dialog_tasks.Launch([dialog, adbc, session_id, obj, columns, selected_col, edit_col_name,
                         edit_col_type, edit_col_nullable, loading, error_text, app, workspace,
                         tasks, notify]() -> Task<void> {
      AdbcSchemaResult res = co_await adbc->sequence.Run(
          [adbc, obj](std::stop_token) { return LoadSchemaBlocking(*adbc, obj); });
      loading = false;
      if (res.connection_lost) {
        HandleSessionDisconnected(app, workspace, tasks, session_id, res.error, notify);
        dialog.Dismiss();
        co_return;
      }
      if (!res.error.empty() && res.columns.empty()) {
        error_text = res.error;
        co_return;
      }
      error_text = std::string{};
      columns = res.columns;
      const int sel = selected_col.Get();
      if (sel >= 0 && static_cast<std::size_t>(sel) < res.columns.size()) {
        const ColumnMeta& c = res.columns[static_cast<std::size_t>(sel)];
        edit_col_name = TextEditingValue::FromText(c.name);
        edit_col_type = TextEditingValue::FromText(c.type.empty() ? "ANY" : c.type);
        edit_col_nullable = c.nullable;
      } else {
        selected_col = -1;
      }
    });
  };

  Lifecycle([reload_columns] {
    reload_columns();
    return [] {};
  });

  const DbObject cur_obj = current_object.Get();
  const auto execute_ddl = [dialog, dialog_tasks, adbc, session_id, dialect, cur_obj,
                            current_object, table_name_input, add_col_name, add_col_default, busy,
                            error_text, preview_override, reload_columns, app, workspace, tasks,
                            notify, loc](std::string sql, std::optional<std::string> renamed_table) {
    if (sql.empty() || busy.Get()) {
      return;
    }
    if (!adbc || adbc->IsClosed()) {
      HandleSessionDisconnected(app, workspace, tasks, session_id, {}, notify);
      dialog.Dismiss();
      return;
    }
    busy = true;
    preview_override = sql;
    dialog_tasks.Launch([dialog, adbc, session_id, dialect, cur_obj, current_object,
                         table_name_input, add_col_name, add_col_default, busy, error_text,
                         reload_columns, sql = std::move(sql),
                         renamed_table = std::move(renamed_table), app, workspace, tasks, notify,
                         loc]() -> Task<void> {
      AdbcQueryResult res = co_await ExecuteQueryAsync(adbc, sql, false, 0);
      busy = false;
      if (res.connection_lost) {
        dialog.Dismiss();
        HandleSessionDisconnected(app, workspace, tasks, session_id, res.error, notify);
        co_return;
      }
      if (!res.error.empty()) {
        error_text = res.error;
        if (notify) {
          notify(loc.Fmt(app::strings::toast_edit_table_failed, res.error));
        }
        co_return;
      }

      error_text = std::string{};
      std::string old_name = cur_obj.name;
      std::string next_name = old_name;
      if (renamed_table.has_value() && !renamed_table->empty()) {
        next_name = *renamed_table;
        DbObject updated = cur_obj;
        updated.name = next_name;
        current_object = std::move(updated);
        table_name_input = TextEditingValue::FromText(next_name);
      } else {
        add_col_name = TextEditingValue{};
        add_col_default = TextEditingValue{};
      }

      const std::string cur_schema =
          cur_obj.db_schema.empty() ? ObjectDatabaseName(dialect, cur_obj) : cur_obj.db_schema;
      if (renamed_table.has_value()) {
        workspace.Update([&](WorkspaceModel& ws) {
          for (WorkspaceTab& tab : ws.tabs) {
            if (tab.kind == WorkspaceTabKind::Table && tab.session_id == session_id &&
                tab.object_name == old_name &&
                (tab.object_schema == cur_obj.db_schema || tab.object_schema == cur_schema) &&
                !tab.is_view) {
              tab.object_name = next_name;
              tab.table.object = next_name;
              tab.title = cur_schema.empty() ? next_name : (cur_schema + "." + next_name);
            }
          }
        });
      }

      RefreshSessionObjects(app, workspace, tasks, session_id, notify, false);
      if (!renamed_table.has_value()) {
        std::vector<std::tuple<std::uint64_t, std::int64_t, std::optional<DataTableSort>, std::string>>
            tabs_info;
        for (const WorkspaceTab& tab : workspace.Get().tabs) {
          if (tab.kind == WorkspaceTabKind::Table && tab.session_id == session_id &&
              tab.object_name == cur_obj.name &&
              (tab.object_schema == cur_obj.db_schema || tab.object_schema == cur_schema) &&
              !tab.is_view && tab.table.pending_inserts.empty()) {
            tabs_info.emplace_back(tab.id, tab.table.page, tab.table.sort, tab.table.filter);
          }
        }
        for (auto& [tid, pg, srt, flt] : tabs_info) {
          RequestLoadTable(app, workspace, tasks, tid, pg, srt, std::move(flt), false, notify);
        }
      }
      reload_columns();
      if (notify) {
        notify(loc.Get(app::strings::toast_edit_table_done));
      }
    });
  };

  const std::vector<ColumnMeta>& cols = columns.Get();
  const int sel_idx = selected_col.Get();
  const bool has_selected_col =
      sel_idx >= 0 && static_cast<std::size_t>(sel_idx) < cols.size();
  const ColumnMeta selected_meta =
      has_selected_col ? cols[static_cast<std::size_t>(sel_idx)] : ColumnMeta{};

  const std::string rename_table_sql =
      TrimmedSql(table_name_input.Get().text) != cur_obj.name
          ? BuildRenameTableSql(dialect, cur_obj, table_name_input.Get().text)
          : std::string{};
  const std::string add_col_sql =
      BuildAddColumnSql(dialect, cur_obj, add_col_name.Get().text, add_col_type.Get().text,
                        add_col_nullable.Get(), add_col_default.Get().text);
  const std::string rename_col_sql =
      has_selected_col
          ? BuildRenameColumnSql(dialect, cur_obj, selected_meta.name, edit_col_name.Get().text)
          : std::string{};
  const std::string alter_type_sql =
      has_selected_col
          ? BuildAlterColumnTypeSql(dialect, cur_obj, selected_meta.name, edit_col_type.Get().text,
                                    edit_col_nullable.Get())
          : std::string{};
  const std::string drop_col_sql =
      has_selected_col ? BuildDropColumnSql(dialect, cur_obj, selected_meta.name) : std::string{};

  std::string sql_preview = add_col_sql;
  if (sql_preview.empty()) {
    sql_preview = !rename_table_sql.empty() ? rename_table_sql
                  : (!rename_col_sql.empty() ? rename_col_sql
                     : (!preview_override.Get().empty() ? preview_override.Get() : "--"));
  }

  Views col_rows;
  col_rows.Reserve(cols.size());
  for (std::size_t idx = 0; idx < cols.size(); ++idx) {
    const ColumnMeta& col = cols[idx];
    const bool is_sel = sel_idx == static_cast<int>(idx);
    Views badges;
    if (col.primary_key) {
      badges.Add(StatusBadge(UseString(app::strings::edit_table_col_pk), spec.colors.primary,
                             spec.colors.primary_container, WithAlpha(spec.colors.primary, 0.35F)));
    }
    if (!col.nullable) {
      badges.Add(StatusBadge(UseString(app::strings::edit_table_col_not_null),
                             spec.colors.on_surface_variant, spec.colors.surface_container,
                             semantic.border_subtle));
    }
    badges.Add(MutedText(col.type.empty() ? "ANY" : col.type));

    col_rows.Add(
        NavItem(col.name, {}, is_sel,
                [selected_col, edit_col_name, edit_col_type, edit_col_nullable, col, idx] {
                  selected_col = static_cast<int>(idx);
                  edit_col_name = TextEditingValue::FromText(col.name);
                  edit_col_type = TextEditingValue::FromText(col.type.empty() ? "ANY" : col.type);
                  edit_col_nullable = col.nullable;
                },
                Row{std::move(badges)}.With(Spacing{4.0F}, CrossAlign{CrossAxisAlignment::Center}),
                std::nullopt,
                TintedIcon(app::images::table,
                           is_sel ? spec.colors.primary : spec.colors.on_surface_variant, 14.0F))
            .Key("edit-col-" + std::to_string(idx) + "-" + col.name));
  }

  View selected_panel;
  if (has_selected_col) {
    View type_row;
    if (dialect == Dialect::Sqlite) {
      type_row = MutedText(UseString(app::strings::edit_table_sqlite_type_note));
    } else {
      type_row = Row{
          TextField(edit_col_type)
              .Label(app::strings::edit_table_type_label)
              .Variant(TextFieldVariant::Outlined)
              .LineLimits(TextFieldLineLimits::SingleLine())
              .OnChanged([edit_col_type](const TextEditingValue& val) { edit_col_type = val; })
              .With(Grow(1.0F)),
          ActionButton(Button(app::strings::edit_table_alter_type_btn)
                           .OnClick([execute_ddl, alter_type_sql] {
                             execute_ddl(alter_type_sql, std::nullopt);
                           }))
              .With(Enabled{!busy.Get() && !alter_type_sql.empty()}),
      }.With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Center});
    }

    selected_panel = Column{
        Row{
            TextField(edit_col_name)
                .Label(app::strings::edit_table_rename_col_label)
                .Variant(TextFieldVariant::Outlined)
                .LineLimits(TextFieldLineLimits::SingleLine())
                .OnChanged([edit_col_name](const TextEditingValue& val) { edit_col_name = val; })
                .With(Grow(1.0F)),
            ActionButton(Button(app::strings::edit_table_rename_col_btn)
                             .OnClick([execute_ddl, rename_col_sql] {
                               execute_ddl(rename_col_sql, std::nullopt);
                             }))
                .With(Enabled{!busy.Get() && !rename_col_sql.empty()}),
            ActionButton(Button(app::strings::edit_table_drop_col_btn)
                             .OnClick([execute_ddl, drop_col_sql] {
                               execute_ddl(drop_col_sql, std::nullopt);
                             }),
                         ButtonTone::Danger)
                .With(Enabled{!busy.Get() && cols.size() > 1 && !drop_col_sql.empty()}),
        }.With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Center}),
        std::move(type_row),
        cols.size() <= 1 ? MutedText(UseString(app::strings::edit_table_drop_last_hint)) : View{},
    }.With(Padding{EdgeInsets::Symmetric(10.0F, 8.0F)}, Spacing{8.0F},
           CrossAlign{CrossAxisAlignment::Stretch}, CornerRadius{7.0F},
           Background{VisualFill{spec.colors.surface}}, Border{semantic.border_subtle, 1.0F});
  }

  View columns_body =
      loading.Get() && cols.empty()
          ? LoadingState(app::strings::schema_dialog_loading).With(Frame{.height = 170.0F})
          : LoadingOverlay(
                ScrollView(Column{std::move(col_rows)}
                               .With(Spacing{3.0F}, CrossAlign{CrossAxisAlignment::Stretch})),
                loading.Get(), app::strings::schema_dialog_loading)
                .With(Frame{.height = 170.0F});

  const std::string header_label =
      cur_obj.name.empty() ? display_label : QualifiedName(dialect, cur_obj);

  return DialogCard(LoadingOverlay(
      Column{
      Row{
          TintedIcon(app::images::table, spec.colors.primary, 18.0F),
          Text::Format(TextRole::Title, app::strings::edit_table_title, header_label),
          Spacer(),
          ActionButton(Button(app::strings::action_close).OnClick([dialog] { dialog.Dismiss(); })),
      }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{8.0F}),
      ScrollView(
          Column{
              Caption(UseString(app::strings::edit_table_rename_section)),
              Row{
                  TextField(table_name_input)
                      .Label(app::strings::edit_table_name_label)
                      .Variant(TextFieldVariant::Outlined)
                      .LineLimits(TextFieldLineLimits::SingleLine())
                      .OnChanged(
                          [table_name_input](const TextEditingValue& v) { table_name_input = v; })
                      .With(Grow(1.0F)),
                  ActionButton(Button(app::strings::edit_table_rename_btn)
                                   .OnClick([execute_ddl, rename_table_sql, table_name_input] {
                                     execute_ddl(rename_table_sql, TrimmedSql(table_name_input.Get().text));
                                   }))
                      .With(Enabled{!busy.Get() && !rename_table_sql.empty()}),
              }.With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Center}),
              Divider(),
              Caption(UseString(app::strings::edit_table_columns_section, cols.size())),
              std::move(columns_body),
              std::move(selected_panel),
              Divider(),
              Caption(UseString(app::strings::edit_table_add_section)),
              Row{
                  TextField(add_col_name)
                      .Label(app::strings::edit_table_add_name_label)
                      .Placeholder(app::strings::edit_table_add_name_ph)
                      .Variant(TextFieldVariant::Outlined)
                      .LineLimits(TextFieldLineLimits::SingleLine())
                      .OnChanged([add_col_name](const TextEditingValue& v) { add_col_name = v; })
                      .With(Grow(1.0F)),
                  TextField(add_col_type)
                      .Label(app::strings::edit_table_add_type_label)
                      .Placeholder(app::strings::edit_table_add_type_ph)
                      .Variant(TextFieldVariant::Outlined)
                      .LineLimits(TextFieldLineLimits::SingleLine())
                      .OnChanged([add_col_type](const TextEditingValue& v) { add_col_type = v; })
                      .With(Frame{.width = 160.0F}),
                  TextField(add_col_default)
                      .Label(app::strings::edit_table_add_default_label)
                      .Placeholder(app::strings::edit_table_add_default_ph)
                      .Variant(TextFieldVariant::Outlined)
                      .LineLimits(TextFieldLineLimits::SingleLine())
                      .OnChanged(
                          [add_col_default](const TextEditingValue& v) { add_col_default = v; })
                      .With(Frame{.width = 160.0F}),
              }.With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Center}),
              Row{
                  Switch(app::strings::edit_table_add_nullable, add_col_nullable)
                      .OnChanged([add_col_nullable](bool v) { add_col_nullable = v; }),
                  Spacer(),
                  ActionButton(Button(busy.Get() ? app::strings::edit_table_executing
                                                 : app::strings::edit_table_add_btn)
                                   .OnClick([execute_ddl, add_col_sql] {
                                     execute_ddl(add_col_sql, std::nullopt);
                                   }),
                               ButtonTone::Primary)
                      .With(Enabled{!busy.Get() && !add_col_sql.empty()}),
              }.With(CrossAlign{CrossAxisAlignment::Center}),
              Caption(UseString(app::strings::edit_table_sql_preview)),
              Text(sql_preview)
                  .Style(TextStyle{Font::Monospace(12.0F), spec.colors.on_surface_variant})
                  .With(Padding{EdgeInsets::Symmetric(10.0F, 8.0F)}, CornerRadius{6.0F},
                        Background{VisualFill{spec.colors.surface}},
                        Border{semantic.border_subtle, 1.0F}),
              error_text.Get().empty() ? View{} : ErrorText(error_text.Get()),
          }.With(Spacing{10.0F}, CrossAlign{CrossAxisAlignment::Stretch}))
          .With(Grow(1.0F)),
  }.With(Spacing{10.0F}, CrossAlign{CrossAxisAlignment::Stretch}),
      busy.Get(), app::strings::edit_table_executing)
                        .With(Frame{.width = 660.0F, .height = 590.0F}));
}

[[huxerui::composable]] View DropObjectDialog(
    DialogContext dialog, std::shared_ptr<AdbcSession> adbc, std::uint64_t session_id,
    DbObject object, std::string display_label, std::string drop_sql, State<AppModel> app,
    State<WorkspaceModel> workspace, TaskScope tasks, NotifyFn notify) {
  auto busy = UseState(false);
  const LocalizedCatalog loc{UseService<Resources>(), UseService<SettingsModel>()};
  const StringResource kind_res = ObjectKindResource(object);
  const std::string kind = UseString(kind_res);

  const auto confirm = [dialog, adbc, session_id, display_label, kind_res, drop_sql, app,
                        workspace, tasks, busy, notify, loc] {
    if (busy.Get()) {
      return;
    }
    if (!adbc || adbc->IsClosed()) {
      HandleSessionDisconnected(app, workspace, tasks, session_id, {}, notify);
      dialog.Dismiss();
      return;
    }
    busy = true;
    tasks.Launch([dialog, adbc, session_id, display_label, kind_res, drop_sql, app, workspace,
                  tasks, busy, notify, loc]() -> Task<void> {
      AdbcQueryResult res = co_await ExecuteQueryAsync(adbc, drop_sql, false, 0);
      if (res.connection_lost) {
        busy = false;
        dialog.Dismiss();
        HandleSessionDisconnected(app, workspace, tasks, session_id, res.error, notify);
        co_return;
      }
      if (!res.error.empty()) {
        busy = false;
        if (notify) {
          notify(loc.Fmt(app::strings::toast_drop_failed, res.error));
        }
        co_return;
      }
      dialog.Dismiss();
      RefreshSessionObjects(app, workspace, tasks, session_id, notify, false);
      if (notify) {
        notify(loc.Fmt(app::strings::toast_drop_done, loc.Get(kind_res), display_label));
      }
    });
  };

  return DialogCard(LoadingOverlay(
      Column{
          Row{
              Text::Format(TextRole::Title, app::strings::drop_dialog_title, kind),
              Spacer(),
              ActionButton(Button(app::strings::action_cancel)
                               .OnClick([dialog] { dialog.Dismiss(); })
                               .With(Enabled{!busy.Get()})),
              ActionButton(Button(busy.Get() ? app::strings::drop_dialog_confirming
                                             : app::strings::drop_dialog_confirm)
                               .OnClick(confirm)
                               .With(Enabled{!busy.Get()}),
                           ButtonTone::DangerSolid),
          }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{8.0F}),
          MutedText(UseString(app::strings::drop_dialog_warning, display_label)),
          MutedText(UseString(app::strings::drop_dialog_sql_preview, drop_sql)),
      }.With(Spacing{10.0F}, CrossAlign{CrossAxisAlignment::Stretch}),
      busy.Get(), app::strings::drop_dialog_confirming)
                        .With(Frame{.width = 520.0F}));
}

[[huxerui::composable]] View DeleteSavedProfileDialog(
    DialogContext dialog, ConnectionProfile profile, std::string display_name,
    State<std::vector<ConnectionProfile>> profiles, std::string profiles_path, NotifyFn notify) {
  const auto config_db = UseService<ConfigDbService>();
  const LocalizedCatalog loc{UseService<Resources>(), UseService<SettingsModel>()};
  const auto confirm = [dialog, profile, display_name, profiles, profiles_path, config_db,
                        notify, loc] {
    std::vector<ConnectionProfile> updated = profiles.Get();
    const auto it = std::ranges::find(updated, profile);
    if (it != updated.end()) {
      updated.erase(it);
    }
    if (!PersistConnectionProfileList(config_db, profiles_path, updated)) {
      if (notify) {
        notify(loc.Get(app::strings::toast_save_profiles_failed));
      }
      return;
    }
    profiles = std::move(updated);
    dialog.Dismiss();
    if (notify) {
      notify(loc.Fmt(app::strings::toast_delete_profile_done, display_name));
    }
  };

  return DialogCard(Column{
      Row{
          Text(app::strings::delete_profile_title, TextRole::Title),
          Spacer(),
          ActionButton(Button(app::strings::action_cancel).OnClick([dialog] { dialog.Dismiss(); })),
          ActionButton(Button(app::strings::action_delete).OnClick(confirm),
                       ButtonTone::DangerSolid),
      }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{8.0F}),
      MutedText(UseString(app::strings::delete_profile_body, display_name, profile.uri)),
  }.With(Spacing{10.0F}, CrossAlign{CrossAxisAlignment::Stretch})
                    .With(Frame{.width = 480.0F}));
}

[[huxerui::composable]] View ConfirmCloseTabsDialog(DialogContext dialog, State<AppModel> app,
                                                     State<WorkspaceModel> workspace,
                                                     std::vector<std::uint64_t> tab_ids,
                                                     std::size_t draft_count) {
  return DialogCard(Column{
      Row{
          Text(app::strings::close_tabs_title, TextRole::Title),
          Spacer(),
          ActionButton(Button(app::strings::action_cancel).OnClick([dialog] { dialog.Dismiss(); })),
          ActionButton(
              Button(app::strings::close_tabs_confirm)
                  .OnClick([app, workspace, tab_ids = std::move(tab_ids), dialog] {
                    CloseWorkspaceTabs(app, workspace, tab_ids);
                    dialog.Dismiss();
                  }),
              ButtonTone::DangerSolid),
      }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{8.0F}),
      MutedText(UseString(app::strings::close_tabs_message, draft_count)),
  }.With(Spacing{10.0F}, CrossAlign{CrossAxisAlignment::Stretch})
                    .With(Frame{.width = 480.0F}));
}

[[huxerui::composable]] View CloseConfirmDialog(DialogContext dialog,
                                                std::shared_ptr<TrayRuntime> tray_runtime,
                                                State<bool> close_dialog_open) {
  return DialogCard(Column{
      Row{
          Text(app::strings::close_app_title, TextRole::Title),
          Spacer(),
          ActionButton(Button(app::strings::action_cancel).OnClick([dialog, close_dialog_open] {
            close_dialog_open = false;
            dialog.Dismiss();
          })),
          ActionButton(Button(app::strings::action_quit)
                           .OnClick([dialog, tray_runtime, close_dialog_open] {
                             close_dialog_open = false;
                             dialog.Dismiss();
                             tray_runtime->RequestExit();
                           }),
                       ButtonTone::DangerSolid),
      }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{8.0F}),
      MutedText(UseString(app::strings::close_app_message)),
  }.With(Spacing{12.0F}, CrossAlign{CrossAxisAlignment::Stretch})
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
    NotifyFn notify, AppThemeConfig theme_config, LanguageMode language_mode, bool system_dark,
    std::function<void(ConnectionProfile)> connect_profile) {
  const ThemeSpec& spec = UseTheme();
  const SemanticColors& semantic = UseEnvironment<SemanticColors>();
  const auto config_db = UseService<ConfigDbService>();
  const LocalizedCatalog loc{UseService<Resources>(), UseService<SettingsModel>()};
  const auto menu = UseMenu();
  const auto dialogs = UseDialog();
  const auto clipboard = UseApplication().Clipboard();
  const AppModel& model = app.Get();
  const WorkspaceTab* active_tab = workspace.Get().ActiveTab();
  auto collapsed_sessions = UseState(std::vector<std::uint64_t>{});
  auto collapsed_groups = UseState(std::vector<std::string>{});
  auto db_open_overrides = UseState(std::unordered_map<std::string, bool>{});
  auto custom_order = UseState(std::vector<std::string>{});
  auto drop_hover_key = UseState(std::string{});
  auto conn_search = UseState(TextEditingValue{});

  const auto copy_text = [clipboard, notify, loc](std::string text, StringResource label_res) {
    if (clipboard && clipboard->SupportsSynchronousText() && clipboard->WriteText(text)) {
      if (notify) {
        notify(loc.Fmt(app::strings::toast_copied, loc.Get(label_res)));
      }
    } else if (notify) {
      notify(loc.Get(app::strings::toast_clipboard_unsupported));
    }
  };

  const auto& collapsed_session_ids = collapsed_sessions.Get();
  const auto& collapsed_group_keys = collapsed_groups.Get();
  const auto& db_open_map = db_open_overrides.Get();
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

  const auto reorder_connections = [profiles, profiles_path, config_db, custom_order, notify, loc,
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
        notify(loc.Get(app::strings::toast_save_order_failed));
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

  const std::string conn_query = TrimmedSql(conn_search.Get().text);

  Views tree_rows;
  for (const SidebarConnectionEntry& entry : ordered_entries) {
    if (!conn_query.empty()) {
      const std::string entry_name =
          entry.session != nullptr
              ? entry.session->name
              : (entry.profile->name.empty() ? UriDisplayName(entry.profile->uri)
                                             : entry.profile->name);
      const std::string entry_uri =
          entry.session != nullptr ? entry.session->uri : entry.profile->uri;
      const std::string entry_driver =
          entry.session != nullptr ? entry.session->driver_path : entry.profile->driver_path;
      if (!ContainsIgnoreCaseAscii(entry_name, conn_query) &&
          !ContainsIgnoreCaseAscii(entry_uri, conn_query) &&
          !ContainsIgnoreCaseAscii(entry_driver, conn_query)) {
        continue;
      }
    }
    const std::string& entry_key = entry.key;
    const bool is_drop_hover = hovered_drop_key == entry_key;
    const Color drop_border = is_drop_hover ? spec.colors.primary : Color::Transparent();

    if (entry.session == nullptr) {
      const ConnectionProfile& profile = *entry.profile;
      const std::string display_name =
          profile.name.empty() ? UriDisplayName(profile.uri) : profile.name;
      const auto open_edit_profile_dialog = [dialogs, profile, app, workspace, profiles,
                                             profiles_path, tasks, notify, theme_config,
                                             language_mode, system_dark] {
        dialogs.Show([profile, app, workspace, profiles, profiles_path, tasks, notify,
                      theme_config, language_mode, system_dark](DialogContext dialog) {
          return WithDialogTheme(
              theme_config, language_mode, system_dark,
              EditConnectionProfileDialog(dialog, profile, profile, std::nullopt, app, workspace,
                                          profiles, profiles_path, tasks, notify));
        });
      };
      const auto open_delete_profile_dialog = [dialogs, profile, display_name, profiles,
                                               profiles_path, notify, theme_config, language_mode,
                                               system_dark] {
        dialogs.Show([profile, display_name, profiles, profiles_path, notify, theme_config,
                      language_mode, system_dark](DialogContext dialog) {
          return WithDialogTheme(theme_config, language_mode, system_dark,
                                 DeleteSavedProfileDialog(dialog, profile, display_name, profiles,
                                                          profiles_path, notify));
        });
      };

      View unopened_row =
          TreeRow(TreeRowOptions{
                      .prefix = {},
                      .label = display_name,
                      .trailing_text = UseString(app::strings::sidebar_status_disconnected),
                      .indent_level = 0,
                      .selected = false,
                      .emphasize = false,
                      .active_hint = false,
                      .muted = true,
                      .prefix_color = std::nullopt,
                      .status_dot = WithAlpha(spec.colors.on_surface_variant, 0.45F),
                      .leading_icon =
                          DatabaseIcon(DetectDatabaseBrand(profile.driver_path, profile.uri), 15.0F),
                      .expanded = false,
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
                  [menu, loc, connect_profile, profile, copy_text, open_edit_profile_dialog,
                   open_delete_profile_dialog](Point position) {
                    menu.ShowAt(position, {
                        MenuItem(loc.Get(app::strings::sidebar_ctx_connect),
                                 [connect_profile, profile] { connect_profile(profile); }),
                        MenuItem(loc.Get(app::strings::sidebar_ctx_edit_conn),
                                 open_edit_profile_dialog),
                        MenuSection{},
                        MenuItem(loc.Get(app::strings::sidebar_ctx_copy_uri),
                                 [copy_text, uri = profile.uri] {
                                   copy_text(uri, app::strings::sidebar_copy_uri_label);
                                 }),
                        MenuSection{},
                        MenuItem(loc.Get(app::strings::sidebar_ctx_delete_profile),
                                 open_delete_profile_dialog),
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
                                     collapsed_sessions, collapsed_groups, db_open_overrides] {
      collapsed_sessions.Update([session_id](std::vector<std::uint64_t>& ids) {
        std::erase(ids, session_id);
      });
      const std::string group_prefix = std::to_string(session_id) + ":";
      collapsed_groups.Update([&group_prefix](std::vector<std::string>& keys) {
        std::erase_if(keys, [&group_prefix](const std::string& key) {
          return key.starts_with(group_prefix);
        });
      });
      db_open_overrides.Update([&group_prefix](std::unordered_map<std::string, bool>& overrides) {
        std::erase_if(overrides, [&group_prefix](const auto& kv) {
          return kv.first.starts_with(group_prefix);
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
                .prefix = {},
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
                .expanded = is_expanded,
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
                                           theme_config, language_mode, system_dark] {
      dialogs.Show([edit_original, edit_initial, session_id, app, workspace, profiles,
                    profiles_path, tasks, notify, theme_config, language_mode,
                    system_dark](DialogContext dialog) {
        return WithDialogTheme(
            theme_config, language_mode, system_dark,
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
                [menu, loc, is_expanded, toggle_session, refresh_session, app, workspace,
                 copy_text, open_edit_session_dialog, disconnect_session, session_id,
                 uri = session.uri, session_name = session.name,
                 is_redis = (session.dialect == Dialect::Redis), notify](Point position) {
                  menu.ShowAt(position, {
                      MenuItem(loc.Get(is_expanded
                                           ? (is_redis ? app::strings::sidebar_ctx_collapse_dbs
                                                       : app::strings::sidebar_ctx_collapse_objects)
                                           : (is_redis ? app::strings::sidebar_ctx_expand_dbs
                                                       : app::strings::sidebar_ctx_expand_objects)),
                               toggle_session),
                      MenuItem(loc.Get(is_redis ? app::strings::sidebar_ctx_refresh_dbs
                                                : app::strings::sidebar_ctx_refresh_objects),
                               refresh_session),
                      MenuItem(loc.Get(is_redis ? app::strings::sidebar_ctx_new_console
                                                : app::strings::nav_new_query),
                               [app, workspace, session_id] {
                                 (void)OpenSqlTab(app, workspace, session_id);
                               }),
                      MenuSection{},
                      MenuItem(loc.Get(app::strings::sidebar_ctx_edit_conn),
                               open_edit_session_dialog),
                      MenuItem(loc.Get(app::strings::sidebar_ctx_copy_uri),
                               [copy_text, uri] {
                                 copy_text(uri, app::strings::sidebar_copy_uri_label);
                               }),
                      MenuSection{},
                      MenuItem(loc.Get(app::strings::sidebar_ctx_disconnect),
                               [disconnect_session, session_name, notify, loc] {
                                 disconnect_session();
                                 if (notify) {
                                   notify(loc.Fmt(app::strings::toast_disconnected, session_name));
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
                                .label = UseString(session.dialect == Dialect::Redis
                                                       ? app::strings::sidebar_empty_redis
                                                       : app::strings::sidebar_empty_tables),
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
    const bool use_db_hierarchy = SessionUsesDatabaseHierarchy(session);

    const auto render_object_row = [&](const DbObject& object, const std::string& key_prefix,
                                       std::size_t idx, int indent_level,
                                       bool hide_schema_prefix, Color icon_accent) {
      const bool is_redis = session.dialect == Dialect::Redis;
      const std::string object_schema =
          object.db_schema.empty() ? ObjectDatabaseName(session.dialect, object)
                                   : object.db_schema;
      const bool is_selected =
          active_tab != nullptr && active_tab->kind == WorkspaceTabKind::Table &&
          active_tab->session_id == session_id && active_tab->object_name == object.name &&
          active_tab->object_schema == object_schema &&
          active_tab->is_view == object.is_view;
      const std::string display_label =
          (!hide_schema_prefix && multi_schema && !object_schema.empty())
              ? (object_schema + "." + object.name)
              : object.name;
      const std::string name = object.name;
      const bool is_view = object.is_view;
      const std::string qualified =
          is_redis ? object.name : QualifiedName(session.dialect, object);
      const std::string open_in_sql = "SELECT * FROM " + qualified + ";";
      const std::string select_sql = "SELECT * FROM " + qualified + " LIMIT 100;";
      const std::string drop_sql = BuildDropObjectSql(session.dialect, object);
      const StringResource kind_res = ObjectKindResource(object);
      const std::string row_trailing =
          (is_redis && object.item_count.has_value())
              ? std::to_string(*object.item_count)
              : std::string{};
      std::string redis_db_index = "0";
      if (is_redis && object.name.size() > 2 &&
          (object.name.starts_with("db") || object.name.starts_with("DB"))) {
        redis_db_index = object.name.substr(2);
      }

      const auto open_object = [app, workspace, tasks, notify, session_id, name,
                                object_schema, is_view] {
        SelectObject(app, workspace, tasks, session_id, name, object_schema, is_view,
                     notify);
      };

      const auto open_schema_dialog = [dialogs, adbc = session.session, session_id, object,
                                       display_label, qualified, app, workspace, tasks,
                                       notify, theme_config, language_mode, system_dark] {
        if (!adbc || adbc->IsClosed()) {
          HandleSessionDisconnected(app, workspace, tasks, session_id, {}, notify);
          return;
        }
        dialogs.Show([adbc, session_id, object, display_label, qualified, app, workspace,
                      tasks, notify, theme_config, language_mode,
                      system_dark](DialogContext dialog) {
          return WithDialogTheme(
              theme_config, language_mode, system_dark,
              ObjectSchemaDialog(dialog, adbc, session_id, object, display_label, qualified,
                                 app, workspace, tasks, notify));
        });
      };

      const auto open_drop_dialog = [dialogs, adbc = session.session, session_id, object,
                                     display_label, drop_sql, app, workspace, tasks, notify,
                                     theme_config, language_mode, system_dark] {
        if (!adbc || adbc->IsClosed()) {
          HandleSessionDisconnected(app, workspace, tasks, session_id, {}, notify);
          return;
        }
        dialogs.Show([adbc, session_id, object, display_label, drop_sql, app, workspace,
                      tasks, notify, theme_config, language_mode,
                      system_dark](DialogContext dialog) {
          return WithDialogTheme(theme_config, language_mode, system_dark,
                                 DropObjectDialog(dialog, adbc, session_id, object,
                                                  display_label, drop_sql, app, workspace,
                                                  tasks, notify));
        });
      };

      const auto open_edit_table_dialog = [dialogs, adbc = session.session, session_id,
                                           dialect = session.dialect, object, display_label,
                                           app, workspace, tasks, notify, theme_config,
                                           language_mode, system_dark] {
        if (!adbc || adbc->IsClosed()) {
          HandleSessionDisconnected(app, workspace, tasks, session_id, {}, notify);
          return;
        }
        dialogs.Show([adbc, session_id, dialect, object, display_label, app, workspace, tasks,
                      notify, theme_config, language_mode,
                      system_dark](DialogContext dialog) {
          return WithDialogTheme(
              theme_config, language_mode, system_dark,
              EditTableDialog(dialog, adbc, session_id, dialect, object, display_label, app,
                              workspace, tasks, notify));
        });
      };

      const bool is_editable_table =
          !is_redis && !object.is_view &&
          !ContainsIgnoreCaseAscii(object.table_type, "foreign");
      const ImageResource& obj_icon =
          is_redis
              ? app::images::database
              : (ContainsIgnoreCaseAscii(object.table_type, "materialized")
                     ? app::images::mview
                     : (object.is_view
                            ? app::images::view
                            : (ContainsIgnoreCaseAscii(object.table_type, "foreign")
                                   ? app::images::foreign_table
                                   : app::images::table)));
      const Color resolved_icon_color =
          is_redis ? (is_selected ? spec.colors.primary : icon_accent) : icon_accent;

      tree_rows.Add(
          TreeRow(TreeRowOptions{
                      .prefix = {},
                      .label = display_label,
                      .trailing_text = row_trailing,
                      .indent_level = indent_level,
                      .selected = is_selected,
                      .emphasize = is_redis && is_selected,
                      .active_hint = false,
                      .muted = false,
                      .prefix_color = icon_accent,
                      .status_dot = std::nullopt,
                      .leading_icon = TintedIcon(obj_icon, resolved_icon_color, 14.0F),
                  },
                  open_object)
              .On<ViewEvents::ContextMenuRequested>(
                  [menu, loc, open_object, app, workspace, open_schema_dialog,
                   open_edit_table_dialog, copy_text, refresh_session, open_drop_dialog,
                   session_id, open_in_sql, select_sql, display_label, kind_res, is_redis,
                   is_editable_table, redis_db_index](Point position) {
                    if (is_redis) {
                      menu.ShowAt(position, {
                          MenuItem(loc.Get(app::strings::sidebar_ctx_open_browse),
                                   open_object),
                          MenuItem(loc.Get(app::strings::sidebar_ctx_open_in_console),
                                   [app, workspace, session_id, redis_db_index,
                                    display_label] {
                                     (void)OpenSqlTab(app, workspace, session_id,
                                                      "SELECT " + redis_db_index,
                                                      "Redis · " + display_label,
                                                      "db" + redis_db_index);
                                   }),
                          MenuItem(loc.Get(app::strings::sidebar_ctx_view_fields),
                                   open_schema_dialog),
                          MenuSection{},
                          MenuItem(loc.Get(app::strings::sidebar_ctx_copy_name),
                                   [copy_text, display_label] {
                                     copy_text(display_label,
                                               app::strings::sidebar_copy_name_label);
                                   }),
                          MenuSection{},
                          MenuItem(loc.Get(app::strings::sidebar_ctx_refresh_dbs),
                                   refresh_session),
                      });
                      return;
                    }
                    std::vector<MenuEntry> entries;
                    entries.emplace_back(
                        MenuItem(loc.Get(app::strings::sidebar_ctx_open_browse), open_object));
                    entries.emplace_back(MenuItem(
                        loc.Get(app::strings::sidebar_ctx_open_in_sql),
                        [app, workspace, session_id, open_in_sql, display_label] {
                          (void)OpenSqlTab(app, workspace, session_id, open_in_sql,
                                           "SQL · " + display_label);
                        }));
                    entries.emplace_back(MenuItem(
                        loc.Get(app::strings::sidebar_ctx_view_schema), open_schema_dialog));
                    if (is_editable_table) {
                      entries.emplace_back(MenuItem(
                          loc.Get(app::strings::sidebar_ctx_edit_table),
                          open_edit_table_dialog));
                    }
                    entries.emplace_back(MenuSection{});
                    entries.emplace_back(MenuItem(
                        loc.Get(app::strings::sidebar_ctx_copy_name),
                        [copy_text, display_label] {
                          copy_text(display_label, app::strings::sidebar_copy_name_label);
                        }));
                    entries.emplace_back(MenuItem(
                        loc.Get(app::strings::sidebar_ctx_copy_select),
                        [copy_text, select_sql] {
                          copy_text(select_sql, app::strings::sidebar_copy_select_label);
                        }));
                    entries.emplace_back(MenuSection{});
                    entries.emplace_back(MenuItem(
                        loc.Get(app::strings::sidebar_ctx_refresh_objects), refresh_session));
                    entries.emplace_back(MenuItem(
                        loc.Fmt(app::strings::sidebar_ctx_drop_object, loc.Get(kind_res)),
                        open_drop_dialog));
                    menu.ShowAt(position, std::move(entries));
                  })
              .Key("obj-" + key_prefix + "-" + object_schema + "-" + name + "-" +
                   std::to_string(idx)));
    };

    const auto render_object_group =
        [&](const SidebarObjectGroup& group, const std::string& group_id, int group_indent,
            bool hide_schema_prefix) {
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
          const Color group_accent =
              is_view_group ? semantic.accent_view
                            : (is_foreign_group ? semantic.accent_external : semantic.accent_table);
          const ImageResource& group_icon = ObjectGroupIconResource(group.key);

          tree_rows.Add(
              TreeRow(TreeRowOptions{
                          .prefix = {},
                          .label = UseString(group.label),
                          .trailing_text = std::to_string(group.items.size()),
                          .indent_level = group_indent,
                          .selected = false,
                          .emphasize = true,
                          .active_hint = false,
                          .muted = false,
                          .prefix_color = group_accent,
                          .status_dot = std::nullopt,
                          .leading_icon = TintedIcon(group_icon, group_accent, 14.0F),
                          .expanded = group_open,
                      },
                      toggle_group)
                  .On<ViewEvents::ContextMenuRequested>(
                      [menu, loc, group_open, toggle_group, refresh_session, app, workspace,
                       session_id](Point position) {
                        menu.ShowAt(position, {
                            MenuItem(loc.Get(group_open ? app::strings::sidebar_ctx_collapse_group
                                                        : app::strings::sidebar_ctx_expand_group),
                                     toggle_group),
                            MenuItem(loc.Get(app::strings::sidebar_ctx_refresh_objects),
                                     refresh_session),
                            MenuItem(loc.Get(app::strings::nav_new_query),
                                     [app, workspace, session_id] {
                                       (void)OpenSqlTab(app, workspace, session_id);
                                     }),
                        });
                      })
                  .Key("group-" + group_id));

          if (!group_open) {
            return;
          }

          for (std::size_t idx = 0; idx < group.items.size(); ++idx) {
            render_object_row(*group.items[idx], group_id, idx, group_indent + 1,
                              hide_schema_prefix, group_accent);
          }
        };

    if (session.dialect == Dialect::Redis) {
      const std::string key_prefix = std::to_string(session_id) + ":redis";
      for (std::size_t idx = 0; idx < session.objects.size(); ++idx) {
        render_object_row(session.objects[idx], key_prefix, idx, 1, true,
                          spec.colors.on_surface_variant);
      }
    } else if (use_db_hierarchy) {
      const std::vector<SidebarDatabaseGroup> db_buckets = GroupSessionDatabases(session);
      for (const SidebarDatabaseGroup& db_bucket : db_buckets) {
        const std::string db_key = std::to_string(session_id) + ":db:" + db_bucket.name;
        const auto override_it = db_open_map.find(db_key);
        const bool db_open =
            override_it != db_open_map.end() ? override_it->second : db_bucket.default_open;
        const auto toggle_db = [db_open_overrides, db_key, db_open] {
          db_open_overrides.Update([&db_key, db_open](std::unordered_map<std::string, bool>& map) {
            map[db_key] = !db_open;
          });
        };

        const Color db_icon_color =
            db_open ? spec.colors.primary : spec.colors.on_surface_variant;
        tree_rows.Add(
            TreeRow(TreeRowOptions{
                        .prefix = {},
                        .label = db_bucket.name,
                        .trailing_text = std::to_string(db_bucket.object_count),
                        .indent_level = 1,
                        .selected = false,
                        .emphasize = db_open,
                        .active_hint = false,
                        .muted = !db_open && db_bucket.is_system,
                        .prefix_color =
                            db_open
                                ? std::optional<Color>(spec.colors.primary)
                                : std::optional<Color>(spec.colors.on_surface_variant),
                        .status_dot = std::nullopt,
                        .leading_icon = TintedIcon(app::images::database, db_icon_color, 14.0F),
                        .expanded = db_open,
                    },
                    toggle_db)
                .On<ViewEvents::ContextMenuRequested>(
                    [menu, loc, db_open, toggle_db, refresh_session, app, workspace,
                     session_id](Point position) {
                      menu.ShowAt(position, {
                          MenuItem(loc.Get(db_open ? app::strings::sidebar_ctx_collapse_db
                                                   : app::strings::sidebar_ctx_expand_db),
                                   toggle_db),
                          MenuItem(loc.Get(app::strings::sidebar_ctx_refresh_objects),
                                   refresh_session),
                          MenuItem(loc.Get(app::strings::nav_new_query),
                                   [app, workspace, session_id] {
                                     (void)OpenSqlTab(app, workspace, session_id);
                                   }),
                      });
                    })
                .Key("db-" + db_key));

        if (!db_open) {
          continue;
        }
        for (const SidebarObjectGroup& group : db_bucket.groups) {
          render_object_group(group, db_key + ":" + group.key, 2, true);
        }
      }
    } else {
      const std::vector<SidebarObjectGroup> groups = GroupSessionObjects(session);
      for (const SidebarObjectGroup& group : groups) {
        render_object_group(group, std::to_string(session_id) + ":" + group.key, 1, false);
      }
    }
  }

  TextField search_field =
      TextField(conn_search)
          .Placeholder(app::strings::sidebar_search_placeholder)
          .Variant(TextFieldVariant::Outlined)
          .LineLimits(TextFieldLineLimits::SingleLine())
          .LeadingIcon(app::images::search)
          .OnChanged([conn_search](const TextEditingValue& value) { conn_search = value; });
  if (!conn_query.empty()) {
    search_field =
        std::move(search_field)
            .TrailingIcon(app::images::close, app::strings::action_clear)
            .OnTrailingIconClick([conn_search] { conn_search = TextEditingValue{}; });
  }

  View tree_view;
  if (tree_rows.Size() == 0) {
    const std::string empty_msg =
        conn_query.empty() ? UseString(app::strings::sidebar_empty)
                           : UseString(app::strings::sidebar_search_no_match, conn_query);
    tree_view = MutedText(empty_msg).With(Grow(1.0F));
  } else {
    tree_view = ScrollView(Column{std::move(tree_rows)}
                               .With(Spacing{1.0F}, CrossAlign{CrossAxisAlignment::Stretch}))
                    .With(Grow(1.0F));
  }

  return Column{
      std::move(search_field),
      std::move(tree_view),
  }.With(Frame{.width = 260.0F}, Padding{EdgeInsets::Symmetric(10.0F, 10.0F)},
         Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch},
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
          Text(is_redis ? app::strings::delete_row_title_redis
                        : app::strings::delete_row_title_sql,
               TextRole::Title),
          Spacer(),
          ActionButton(Button(app::strings::action_cancel).OnClick([dialog] { dialog.Dismiss(); })),
          ActionButton(
              Button(app::strings::action_delete).OnClick([app, workspace, tab_id, tasks, notify,
                                                          dialog] {
                RequestDeleteSelected(app, workspace, tasks, tab_id, notify);
                dialog.Dismiss();
              }),
              ButtonTone::DangerSolid),
      }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{8.0F}),
      Text(is_redis ? app::strings::delete_row_body_redis : app::strings::delete_row_body_sql),
  }.With(Spacing{12.0F}, CrossAlign{CrossAxisAlignment::Stretch})
                    .With(Frame{.width = 520.0F}));
}

[[huxerui::composable]] View AdbcManagerDialog(DialogContext dialog,
                                                   AdbcManagerContext context) {
  const auto config_db = UseService<ConfigDbService>();
  const LocalizedCatalog loc{UseService<Resources>(), UseService<SettingsModel>()};
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
                             catalog_failed, loc]() {
    if (catalog_loading.Get()) {
      return;
    }
    if (!context.http) {
      catalog_failed = true;
      if (context.notify) {
        context.notify(loc.Get(app::strings::toast_http_unavailable));
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
                              install_status, refresh_drivers, loc](RegistryDriverInfo item) {
    if (!installing_slug.Get().empty()) {
      return;
    }
    if (!context.http) {
      if (context.notify) {
        context.notify(loc.Get(app::strings::toast_http_unavailable));
      }
      return;
    }
    installing_slug = item.slug;
    install_status = loc.Fmt(app::strings::adbc_preparing_download, item.slug);
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
                                install_status, refresh_drivers, loc](std::string slug) {
    if (!installing_slug.Get().empty()) {
      return;
    }
    installing_slug = slug;
    install_status = loc.Fmt(app::strings::adbc_uninstalling, slug);
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
                             password, name, loc]() -> bool {
    const std::string driver_path = driver.Get().text;
    const std::string uri_text = uri.Get().text;
    if (driver_path.empty() || uri_text.empty()) {
      if (context.notify) {
        context.notify(loc.Get(app::strings::toast_driver_uri_empty));
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
        context.notify(loc.Fmt(app::strings::toast_save_profiles_path_failed, context.profiles_path));
      }
      return false;
    }
    profiles = std::move(list);
    selected = index;
    name = TextEditingValue::FromText(profile.name);
    if (context.notify) {
      context.notify(loc.Fmt(app::strings::toast_profile_saved, profile.name));
    }
    return true;
  };

  const auto delete_profile = [context, config_db, profiles, selected, name, loc] {
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
        context.notify(loc.Fmt(app::strings::toast_delete_save_failed, context.profiles_path));
      }
      return;
    }
    profiles = std::move(list);
    selected = -1;
    name = TextEditingValue::FromText({});
    if (context.notify) {
      context.notify(loc.Fmt(app::strings::toast_profile_removed, removed));
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
      Caption(UseString(app::strings::adbc_saved_profiles)),
      profile_rows.Size() == 0
          ? View(MutedText(UseString(app::strings::adbc_no_saved_profiles)))
          : View(ScrollView(Column{std::move(profile_rows)}.With(Spacing{2.0F}))
                     .With(Grow(1.0F))),
  }.With(Frame{.width = 220.0F}, Spacing{6.0F}, CrossAlign{CrossAxisAlignment::Stretch});

  View form_panel = Column{
      Row{
          Caption(UseString(app::strings::adbc_drivers_heading)),
          Spacer(),
          ActionButton(Button(app::strings::adbc_install_drivers_btn).OnClick([switch_tab]() mutable {
            switch_tab(1);
          })),
          ActionButton(Button(app::strings::action_refresh).OnClick([refresh_drivers,
                                                                     notify = context.notify, loc] {
            const std::size_t count = refresh_drivers();
            if (notify) {
              notify(loc.Fmt(app::strings::toast_drivers_found, count));
            }
          })),
      }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{6.0F}),
      driver_rows.Size() == 0
          ? View(MutedText(UseString(app::strings::adbc_no_drivers_found)))
          : View(ScrollView(Column{std::move(driver_rows)}.With(Spacing{2.0F}))
                     .With(Frame{.height = 140.0F})),
      TextField(driver)
          .Label(app::strings::adbc_driver_field_label)
          .Placeholder(app::strings::adbc_driver_field_placeholder)
          .Variant(TextFieldVariant::Outlined)
          .OnChanged([driver](const TextEditingValue& value) { driver = value; }),
      TextField(uri)
          .Label(app::strings::adbc_uri_field_label)
          .Placeholder(app::strings::adbc_uri_field_placeholder)
          .Variant(TextFieldVariant::Outlined)
          .OnChanged([uri](const TextEditingValue& value) { uri = value; }),
      Row{
          TextField(username)
              .Label(app::strings::adbc_user_field_label)
              .Variant(TextFieldVariant::Outlined)
              .OnChanged([username](const TextEditingValue& value) { username = value; })
              .With(Grow(1.0F)),
          PasswordField(password, app::strings::adbc_password_field_label)
              .With(Grow(1.0F)),
      }.With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch}),
      TextField(name)
          .Label(app::strings::adbc_name_field_label)
          .Placeholder(app::strings::adbc_name_placeholder)
          .Variant(TextFieldVariant::Outlined)
          .OnChanged([name](const TextEditingValue& value) { name = value; }),
  }.With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch}, Grow(1.0F));

  View manage_tab = Column{
      MutedText(UseString(app::strings::adbc_manage_hint)),
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
    const std::string ver_suffix =
        installed_version.empty() ? std::string{} : " " + installed_version;
    if (user_is_installed) {
      detail = UseString(app::strings::adbc_installed_prefix, ver_suffix) + " · ";
    } else if (has_manifest) {
      detail = UseString(app::strings::adbc_external_prefix, ver_suffix) + " · ";
    }
    detail += UseString(app::strings::adbc_available_version, item.version);
    if (!item.license.empty()) {
      detail += " · " + item.license;
    }
    if (!item.description.empty()) {
      detail += " — " + item.description;
    }

    View trailing;
    if (active_install == item.slug) {
      trailing = MutedText(UseString(app::strings::adbc_status_busy));
    } else if (user_is_installed) {
      trailing = Row{
          ActionButton(Button(app::strings::action_uninstall).OnClick([start_uninstall,
                                                                       slug = item.slug]() mutable {
                         start_uninstall(slug);
                       }),
                       ButtonTone::Danger)
              .With(Enabled{!busy}),
          ActionButton(Button(same_version ? app::strings::action_reinstall
                                           : app::strings::action_update)
                           .OnClick([start_install, item]() mutable {
                             start_install(item);
                           }))
              .With(Enabled{!busy}),
      }.With(Spacing{6.0F}, CrossAlign{CrossAxisAlignment::Center});
    } else {
      trailing = ActionButton(Button(app::strings::action_install).OnClick([start_install,
                                                                            item]() mutable {
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
    install_list_content = LoadingState(app::strings::adbc_catalog_loading);
  } else if (catalog_failed.Get() && catalog_list.empty()) {
    install_list_content = TableEmptyState(app::strings::adbc_catalog_failed);
  } else if (registry_rows.Size() == 0) {
    install_list_content = TableEmptyState(catalog_list.empty() ? app::strings::adbc_catalog_prompt
                                                                : app::strings::adbc_catalog_no_match);
  } else {
    const StringVariant overlay_label =
        busy ? (!install_status.Get().empty() ? StringVariant(install_status.Get())
                                              : StringVariant(app::strings::adbc_status_busy))
             : StringVariant(app::strings::adbc_catalog_loading);
    install_list_content =
        LoadingOverlay(ScrollView(Column{std::move(registry_rows)}.With(Spacing{4.0F})),
                       catalog_loading.Get() || busy, overlay_label)
            .With(Grow(1.0F));
  }

  const std::string user_dir = UserAdbcDriversDir();
  const std::string catalog_meta_text =
      user_dir.empty()
          ? UseString(app::strings::adbc_catalog_meta, AdbcRegistryBaseUrl(),
                      CurrentPlatformTuple())
          : UseString(app::strings::adbc_catalog_meta_with_dir, AdbcRegistryBaseUrl(),
                      CurrentPlatformTuple(), user_dir);
  View install_tab = Column{
      MutedText(catalog_meta_text),
      TextField(registry_filter)
          .Label(app::strings::adbc_filter_label)
          .Placeholder(app::strings::adbc_filter_placeholder)
          .Variant(TextFieldVariant::Outlined)
          .OnChanged([registry_filter](const TextEditingValue& value) {
            registry_filter = value;
          }),
      std::move(install_list_content),
  }.With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch}, Grow(1.0F));

  View header_actions;
  if (tab.Get() == 0) {
    header_actions = Row{
        ActionButton(Button(app::strings::action_close).OnClick([dialog] { dialog.Dismiss(); })),
        ActionButton(Button(app::strings::action_delete).OnClick(delete_profile),
                     ButtonTone::Danger)
            .With(Enabled{selected.Get() >= 0}),
        ActionButton(Button(app::strings::action_save).OnClick([save_profile] {
          (void)save_profile();
        })),
        ActionButton(Button(app::strings::action_connect).OnClick(connect), ButtonTone::Primary),
    }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{8.0F});
  } else {
    header_actions = Row{
        ActionButton(Button(app::strings::action_close).OnClick([dialog] { dialog.Dismiss(); })),
        ActionButton(Button(app::strings::adbc_refresh_catalog).OnClick([load_catalog]() mutable {
          load_catalog();
        })).With(Enabled{!catalog_loading.Get()}),
    }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{8.0F});
  }

  View body = Column{
      Row{
          Text(app::strings::adbc_title, TextRole::Title),
          SegmentedButton({app::strings::adbc_tab_connections, app::strings::adbc_tab_install},
                          tab.Get())
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
  const LocalizedCatalog loc{UseService<Resources>(), UseService<SettingsModel>()};
  const ThemeSpec& spec = UseTheme();
  const SemanticColors& semantic = UseEnvironment<SemanticColors>();
  auto dialog_tasks = UseTaskScope();
  auto step = UseState<int>(0);
  auto type_filter = UseState(TextEditingValue::FromText({}));
  auto extra_query = UseState(std::string{});
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

  const ConnectionFormStates form{
      .driver_slug = selected_preset_id,
      .driver = driver,
      .uri = uri,
      .host = host,
      .port = port,
      .database = database,
      .username = username,
      .password = password,
      .title = title,
      .extra_query = extra_query,
      .input_mode = input_mode,
  };

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
  const LocalizedPresetStrings active_preset_strings = DatabasePresetStrings(active_preset);
  const std::string active_preset_name = UseString(active_preset_strings.name);
  const std::string active_preset_placeholder = active_preset_strings.uri_placeholder
                                                    ? UseString(*active_preset_strings.uri_placeholder)
                                                    : active_preset.uri_placeholder;
  const std::vector<AdbcDriverInfo>& driver_list = drivers.Get();
  const std::optional<AdbcDriverInfo> matched_driver =
      FindInstalledDriverForPreset(active_preset, driver_list);

  const auto select_preset = [context, step, selected_preset_id, input_mode, driver, uri,
                              host, port, database, username, password, title, extra_query,
                              install_status, drivers](const DatabasePreset& preset) {
    selected_preset_id = preset.id;
    step = 1;
    input_mode = 0;
    install_status = {};
    title = TextEditingValue::FromText({});
    password = TextEditingValue::FromText({});
    extra_query = {};

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
    struct VisiblePreset {
      const DatabasePreset* preset = nullptr;
      std::string name;
      std::string subtitle;
    };
    std::vector<VisiblePreset> visible;
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
      const LocalizedPresetStrings p_strs = DatabasePresetStrings(preset);
      std::string p_name = UseString(p_strs.name);
      std::string p_sub = UseString(p_strs.subtitle);
      if (!filter_text.empty() &&
          !ContainsIgnoreCaseAscii(p_name, filter_text) &&
          !ContainsIgnoreCaseAscii(p_sub, filter_text) &&
          !ContainsIgnoreCaseAscii(preset.name, filter_text) &&
          !ContainsIgnoreCaseAscii(preset.id, filter_text) &&
          !ContainsIgnoreCaseAscii(preset.driver_slug, filter_text) &&
          !ContainsIgnoreCaseAscii(preset.subtitle, filter_text)) {
        continue;
      }
      visible.push_back(VisiblePreset{
          .preset = &preset,
          .name = std::move(p_name),
          .subtitle = std::move(p_sub),
      });
    }

    Views grid_rows;
    grid_rows.Reserve((visible.size() + 1) / 2);
    const std::string badge_builtin = UseString(app::strings::connect_badge_builtin);
    const std::string badge_manual = UseString(app::strings::connect_badge_manual);
    const std::string badge_ready = UseString(app::strings::connect_badge_ready);
    const std::string badge_install = UseString(app::strings::connect_badge_install);
    const auto make_card = [spec, semantic, &driver_list, select_preset, &badge_builtin,
                            &badge_manual, &badge_ready,
                            &badge_install](const VisiblePreset& entry) {
      const DatabasePreset& preset = *entry.preset;
      View badge;
      if (preset.builtin) {
        badge = StatusBadge(badge_builtin, semantic.success, semantic.success_container,
                            WithAlpha(semantic.success, 0.35F));
      } else if (preset.form_kind == ConnectionFormKind::Custom) {
        badge = StatusBadge(badge_manual, spec.colors.on_surface_variant,
                            spec.colors.surface_container, semantic.border_subtle);
      } else if (FindInstalledDriverForPreset(preset, driver_list).has_value()) {
        badge = StatusBadge(badge_ready, semantic.success, semantic.success_container,
                            WithAlpha(semantic.success, 0.35F));
      } else {
        badge = StatusBadge(badge_install, semantic.warning, semantic.warning_container,
                            WithAlpha(semantic.warning, 0.35F));
      }
      View nav = NavItem(
          entry.name, entry.subtitle, false,
          [select_preset, preset]() mutable { select_preset(preset); }, std::move(badge),
          std::nullopt, DatabaseBadgeIcon(DetectDatabaseBrand(preset.id), 20.0F, 34.0F));
      return Column{std::move(nav)}
          .With(Grow(1.0F), CrossAlign{CrossAxisAlignment::Stretch},
                Background{VisualFill{spec.colors.surface}}, Border{semantic.border_subtle, 1.0F},
                CornerRadius{7.0F})
          .Key("db-type-" + preset.id);
    };

    for (std::size_t i = 0; i < visible.size(); i += 2) {
      const VisiblePreset* left = &visible[i];
      const VisiblePreset* right = (i + 1 < visible.size()) ? &visible[i + 1] : nullptr;
      View left_card = make_card(*left);
      View right_slot = right != nullptr ? make_card(*right) : View(Spacer().With(Grow(1.0F)));
      grid_rows.Add(
          Row{std::move(left_card), std::move(right_slot)}
              .With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch})
              .Key("db-type-row-" + left->preset->id));
    }

    View grid_content;
    if (grid_rows.Size() == 0) {
      grid_content = Column{
          MutedText(UseString(app::strings::connect_no_match)),
      }.With(Grow(1.0F), MainAlign{MainAxisAlignment::Center},
             CrossAlign{CrossAxisAlignment::Center});
    } else {
      grid_content = ScrollView(Column{std::move(grid_rows)}
                                    .With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch}))
                         .With(Grow(1.0F));
    }

    View body = Column{
        Row{
            Text(app::strings::connect_title, TextRole::Title),
            MutedText(UseString(app::strings::connect_step0_subtitle)),
            Spacer(),
            ActionButton(Button(app::strings::connect_saved_profiles_btn)
                             .OnClick([dialog, open_manager = context.open_manager] {
                               dialog.Dismiss();
                               if (open_manager) {
                                 open_manager(0, {});
                               }
                             })),
            ActionButton(Button(app::strings::action_cancel).OnClick([dialog] { dialog.Dismiss(); })),
        }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{8.0F}),
        TextField(type_filter)
            .Label(app::strings::connect_search_label)
            .Placeholder(app::strings::connect_search_placeholder)
            .Variant(TextFieldVariant::Outlined)
            .LineLimits(TextFieldLineLimits::SingleLine())
            .OnChanged([type_filter](const TextEditingValue& value) { type_filter = value; }),
        SegmentedButton({app::strings::connect_cat_all, app::strings::connect_cat_file,
                         app::strings::connect_cat_relational, app::strings::connect_cat_kv,
                         app::strings::connect_cat_analytics, app::strings::connect_cat_custom},
                        cat_idx)
            .OnChanged([category_tab](std::size_t index) { category_tab = index; }),
        std::move(grid_content),
        MutedText(UseString(app::strings::connect_step0_hint)),
    }.With(Spacing{10.0F}, CrossAlign{CrossAxisAlignment::Stretch},
           Frame{.width = 740.0F, .height = 560.0F});

    return DialogCard(std::move(body));
  }

  const auto install_active_driver = [context, dialog_tasks, active_preset, installing,
                                      install_status, driver,
                                      refresh_drivers, loc]() {
    if (installing.Get() || active_preset.driver_slug.empty()) {
      return;
    }
    if (!context.http) {
      if (context.notify) {
        context.notify(loc.Get(app::strings::toast_http_unavailable));
      }
      return;
    }
    installing = true;
    install_status = loc.Get(app::strings::connect_query_registry);
    const std::string target_slug = active_preset.driver_slug;
    const auto http = context.http;
    const NotifyFn notify = context.notify;

    dialog_tasks.Launch([http, target_slug, installing, install_status,
                         driver, refresh_drivers, notify, loc]() -> Task<void> {
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
          notify(loc.Fmt(app::strings::connect_platform_no_driver, CurrentPlatformTuple(),
                         target_slug));
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

  const auto pick_local_file = [context, dialog_tasks, active_preset, active_preset_strings, uri,
                                title, loc]() {
    if (!context.picker || !context.picker->CanOpenFiles()) {
      if (context.notify) {
        context.notify(loc.Get(app::strings::connect_picker_unsupported));
      }
      return;
    }
    const std::string filter_label =
        loc.Fmt(app::strings::connect_picker_filter, loc.Get(active_preset_strings.name));
    dialog_tasks.Launch([picker = context.picker, filter_label,
                         extensions = active_preset.file_extensions, uri, title,
                         notify = context.notify, loc]() -> Task<void> {
      FilePickerFilter filter;
      filter.name = filter_label;
      filter.extensions = extensions;
      auto file_reference = co_await picker->OpenFileAsync(std::move(filter));
      if (!file_reference) {
        co_return;
      }
      auto file = file_reference->AsFile();
      if (!file) {
        if (notify) {
          notify(loc.Get(app::strings::connect_picker_no_local_path));
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
                        database, extra_query, username, password, title, save_profile_flag, dialog,
                        loc] {
    const std::string effective_uri =
        (active_preset.form_kind == ConnectionFormKind::HostPort && input_mode.Get() == 0)
            ? ComposePresetUri(active_preset, host.Get().text, port.Get().text,
                               database.Get().text, extra_query.Get())
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
        context.notify(loc.Get(app::strings::toast_driver_addr_required));
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
          context.notify(
              loc.Fmt(app::strings::toast_save_profiles_path_failed, context.profiles_path));
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
      std::string ready_text;
      if (active_preset.id == "redis") {
        ready_text = UseString(app::strings::connect_ready_redis);
      } else if (active_preset.builtin) {
        ready_text = UseString(app::strings::connect_ready_sqlite, matched_driver->path);
      } else {
        const std::string& driver_detail =
            matched_driver->detail.empty() ? matched_driver->path : matched_driver->detail;
        ready_text = UseString(app::strings::connect_ready_driver, driver_detail);
      }
      readiness_strip = Row{
          AccentDot(semantic.success, 7.0F),
          MutedText(ready_text),
      }.With(Spacing{6.0F}, CrossAlign{CrossAxisAlignment::Center});
    } else {
      readiness_strip =
          Row{
              Column{
                  Text::Format(app::strings::connect_missing_driver_title, active_preset_name,
                               active_preset.driver_slug),
                  MutedText(UseString(app::strings::connect_missing_driver_hint)),
              }.With(Grow(1.0F), Spacing{2.0F}, CrossAlign{CrossAxisAlignment::Stretch}),
              ActionButton(Button(installing.Get() ? app::strings::connect_installing_btn
                                                   : app::strings::connect_one_click_install)
                               .OnClick(install_active_driver)
                               .With(Enabled{!installing.Get()}),
                           ButtonTone::Primary),
              ActionButton(Button(app::strings::connect_open_manager_btn)
                               .OnClick([dialog, open_manager = context.open_manager,
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

  View form_fields = Column{
      ConnectionFormFields(
          active_preset, active_preset_placeholder, form, driver_list,
          context.picker && context.picker->CanOpenFiles(), pick_local_file, connect,
          (active_preset.form_kind == ConnectionFormKind::File ||
           active_preset.form_kind == ConnectionFormKind::Custom)
              ? app::strings::connect_name_filename_hint
              : app::strings::connect_name_auto_hint),
      Switch(app::strings::connect_save_profile_switch, save_profile_flag)
          .OnChanged([save_profile_flag](bool value) { save_profile_flag = value; }),
  }.With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch});

  View step1_body = Column{
      Row{
          ActionButton(Button(app::strings::connect_back_step0).OnClick([step] {
            step = 0;
          })),
          DatabaseIcon(DetectDatabaseBrand(active_preset.id), 22.0F),
          Text::Format(TextRole::Title, app::strings::connect_step1_title, active_preset_name),
          Spacer(),
          ActionButton(Button(app::strings::action_cancel).OnClick([dialog] { dialog.Dismiss(); })),
          ActionButton(Button(app::strings::action_connect).OnClick(connect), ButtonTone::Primary)
              .With(Enabled{!installing.Get()}),
      }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{8.0F}),
      std::move(readiness_strip),
      ScrollView(std::move(form_fields)).With(Grow(1.0F)),
  }.With(Spacing{10.0F}, CrossAlign{CrossAxisAlignment::Stretch});

  const StringVariant install_label =
      !install_status.Get().empty() ? StringVariant(install_status.Get())
                                    : StringVariant(app::strings::connect_installing_btn);
  return DialogCard(LoadingOverlay(std::move(step1_body), installing.Get(), install_label)
                        .With(Frame{.width = 740.0F, .height = 560.0F}));
}

[[huxerui::composable]] View PageSizePopup(PopupContext popup, std::int64_t current_size,
                                           std::function<void(std::int64_t)> on_apply) {
  const ThemeSpec& spec = UseTheme();
  const SemanticColors& semantic = UseEnvironment<SemanticColors>();
  const std::int64_t initial =
      current_size > 0 ? std::clamp(current_size, kMinPageSize, kMaxPageSize) : kDefaultPageSize;
  auto custom_input = UseState(TextEditingValue::FromText(std::to_string(initial)));
  auto invalid = UseState(false);

  const auto submit_custom = [custom_input, invalid, on_apply, popup] {
    const std::optional<std::int64_t> parsed = TryParsePageSize(custom_input.Get().text);
    if (!parsed.has_value()) {
      invalid = true;
      return;
    }
    invalid = false;
    if (on_apply) {
      on_apply(*parsed);
    }
    popup.Dismiss();
  };

  static constexpr std::int64_t kPresets[] = {50, 100, 200, 500, 1000};
  Views preset_rows;
  preset_rows.Reserve(std::size(kPresets));
  for (const std::int64_t preset_size : kPresets) {
    preset_rows.Add(NavItem(UseString(app::strings::table_page_size_option, preset_size), {},
                            preset_size == initial,
                            [on_apply, popup, preset_size] {
                              if (on_apply) {
                                on_apply(preset_size);
                              }
                              popup.Dismiss();
                            })
                        .Key("page-size-opt-" + std::to_string(preset_size)));
  }

  return Column{
      Column{std::move(preset_rows)}.With(Spacing{2.0F}, CrossAlign{CrossAxisAlignment::Stretch}),
      Divider(),
      Caption(UseString(app::strings::table_page_size_custom_label)),
      Row{
          TextField(custom_input)
              .Placeholder(app::strings::table_page_size_custom_ph)
              .Variant(TextFieldVariant::Outlined)
              .LineLimits(TextFieldLineLimits::SingleLine())
              .OnChanged([custom_input, invalid](const TextEditingValue& val) {
                custom_input = val;
                if (invalid.Get()) {
                  invalid = false;
                }
              })
              .OnSubmitted(submit_custom)
              .With(Grow(1.0F)),
          ActionButton(Button(app::strings::action_apply).OnClick(submit_custom),
                       ButtonTone::Primary),
      }.With(Spacing{6.0F}, CrossAlign{CrossAxisAlignment::Center}),
      invalid.Get() ? ErrorText(UseString(app::strings::table_page_size_invalid)) : View{},
  }.With(Frame{.width = 270.0F}, Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch},
         Background{VisualFill{spec.colors.surface_container_highest}}, CornerRadius{10.0F},
         Border{semantic.border_subtle, 1.0F}, Padding{EdgeInsets::Symmetric(10.0F, 10.0F)});
}

[[huxerui::composable]] View AuditDurationBadge(std::int64_t elapsed_us) {
  const ThemeSpec& spec = UseTheme();
  const std::string text = FormatExecutionDuration(elapsed_us);
  if (text.empty()) {
    return View{};
  }
  return Text(text)
      .Style(TextStyle{Font::Monospace(font_size::kCaption).WithWeight(FontWeight::Medium),
                       spec.colors.primary})
      .LineLimit(1)
      .With(Padding{EdgeInsets::Symmetric(6.0F, 2.0F)}, CornerRadius{5.0F},
            Background{VisualFill{WithAlpha(spec.colors.primary, 0.10F)}},
            LayoutPriority(1.0F));
}

[[huxerui::composable]] View AuditSqlBadge(std::string executed_sql, NotifyFn notify) {
  const ThemeSpec& spec = UseTheme();
  const SemanticColors& semantic = UseEnvironment<SemanticColors>();
  const LocalizedCatalog loc{UseService<Resources>(), UseService<SettingsModel>()};
  const auto clipboard = UseApplication().Clipboard();
  if (executed_sql.empty()) {
    return View{};
  }
  const std::string preview = CompactSqlPreview(executed_sql);
  return Row{
      Text(preview)
          .Style(TextStyle{Font::Monospace(font_size::kCaption), spec.colors.on_surface_variant})
          .LineLimit(1)
          .Overflow(TextOverflow::Ellipsis),
  }
      .With(Padding{EdgeInsets::Symmetric(7.0F, 3.0F)}, CrossAlign{CrossAxisAlignment::Center},
            CornerRadius{5.0F}, Background{VisualFill{spec.colors.surface_container}},
            Border{semantic.border_subtle, 1.0F}, Tooltip(StringVariant(executed_sql)))
      .OnClick([clipboard, notify, loc, full_sql = std::move(executed_sql)] {
        if (clipboard && clipboard->SupportsSynchronousText() && clipboard->WriteText(full_sql)) {
          if (notify) {
            notify(loc.Fmt(app::strings::toast_copied,
                           loc.Get(app::strings::audit_copy_sql_label)));
          }
        } else if (notify) {
          notify(loc.Get(app::strings::toast_clipboard_unsupported));
        }
      });
}

[[huxerui::composable]] View TableBrowserPage(
    State<AppModel> app, State<WorkspaceModel> workspace, std::uint64_t tab_id,
    TaskScope tasks, NotifyFn notify, AppThemeConfig theme_config, LanguageMode language_mode,
    bool system_dark) {
  const ThemeSpec& spec = UseTheme();
  const SemanticColors& semantic = UseEnvironment<SemanticColors>();
  const LocalizedCatalog loc{UseService<Resources>(), UseService<SettingsModel>()};
  const auto dialogs = UseDialog();
  const auto menu = UseMenu();
  const auto sort_anchor = UseViewHandle();
  const auto page_size_anchor = UseViewHandle();
  const auto page_size_popup = UsePopup();
  const auto settings = UseService<SettingsModel>();
  auto live_drafts = UseState(std::make_shared<std::unordered_map<std::string, std::string>>());

  const WorkspaceTab* tab = workspace.Get().FindTab(tab_id);
  if (tab == nullptr || tab->kind != WorkspaceTabKind::Table) {
    return TableEmptyState(app::strings::table_tab_closed);
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
    return TableEmptyState(app::strings::table_conn_disconnected);
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

  const auto block_if_dirty = [workspace, tab_id, notify, loc]() -> bool {
    const WorkspaceTab* cur = workspace.Get().FindTab(tab_id);
    if (cur != nullptr && !cur->table.pending_inserts.empty()) {
      if (notify) {
        notify(loc.Get(app::strings::table_dirty_block));
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

  const auto clear_filter = [reload, workspace, tab_id] {
    workspace.Update([tab_id](WorkspaceModel& ws) {
      if (WorkspaceTab* target = ws.FindTab(tab_id)) {
        target->filter_input = TextEditingValue{};
      }
    });
    reload(0, {});
  };

  const auto refresh = [reload, workspace, tab_id] {
    const WorkspaceTab* cur = workspace.Get().FindTab(tab_id);
    if (cur == nullptr) {
      return;
    }
    reload(cur->table.page, cur->table.filter);
  };

  const auto add_draft_row = [workspace, tab_id, flush_live_drafts, notify, loc] {
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
      notify(loc.Get(app::strings::table_draft_added_hint));
    }
  };

  const auto commit_inserts = [app, workspace, tasks, tab_id, flush_live_drafts, notify] {
    flush_live_drafts();
    RequestCommitPendingInserts(app, workspace, tasks, tab_id, notify);
  };

  const auto rollback_inserts = [workspace, tab_id, live_drafts, notify, loc] {
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
      notify(loc.Get(app::strings::table_draft_rolled_back));
    }
  };

  const auto delete_selected_action = [dialogs, app, workspace, tab_id, live_drafts,
                                       flush_live_drafts, tasks, notify, theme_config,
                                       language_mode, system_dark, is_redis, loc] {
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
        notify(loc.Get(app::strings::table_draft_removed_one));
      }
      return;
    }
    dialogs.Show([app, workspace, tab_id, tasks, notify, theme_config, language_mode, system_dark,
                  is_redis](DialogContext dialog) {
      return WithDialogTheme(theme_config, language_mode, system_dark,
                             DeleteRowDialog(dialog, app, workspace, tab_id, tasks, notify, is_redis));
    });
  };

  const auto apply_sort = [app, workspace, tasks, tab_id, notify,
                           block_if_dirty](std::optional<DataTableSort> next_sort) {
    if (block_if_dirty()) {
      return;
    }
    const WorkspaceTab* cur = workspace.Get().FindTab(tab_id);
    if (cur == nullptr) {
      return;
    }
    RequestLoadTable(app, workspace, tasks, tab_id, 0, std::move(next_sort), cur->table.filter,
                     false, notify);
  };

  Views toolbar_items;
  TextField filter_field =
      TextField(tab->filter_input)
          .Label(is_redis ? app::strings::table_filter_redis : app::strings::table_filter_sql)
          .Placeholder(is_redis ? app::strings::table_filter_redis_ph
                                : app::strings::table_filter_sql_ph)
          .Variant(TextFieldVariant::Outlined)
          .LineLimits(TextFieldLineLimits::SingleLine())
          .LeadingIcon(app::images::search)
          .OnChanged([workspace, tab_id](const TextEditingValue& value) {
            workspace.Update([&](WorkspaceModel& ws) {
              if (WorkspaceTab* target = ws.FindTab(tab_id)) {
                target->filter_input = value;
              }
            });
          })
          .OnSubmitted([apply_filter] { apply_filter(); });
  toolbar_items.Add(std::move(filter_field).With(Frame{.width = 250.0F}));
  toolbar_items.Add(ActionButton(Button(app::strings::table_apply_filter).OnClick(apply_filter)));
  if (!model.filter.empty() || !TrimmedSql(tab->filter_input.text).empty()) {
    toolbar_items.Add(ActionButton(Button(app::strings::table_clear_filter).OnClick(clear_filter)));
  }
  const bool sort_active = model.sort.has_value();
  const std::string sort_label =
      sort_active ? UseString(model.sort->direction == DataTableSortDirection::Descending
                                  ? app::strings::table_sort_active_desc
                                  : app::strings::table_sort_active_asc,
                              model.sort->column_id)
                  : UseString(app::strings::table_sort);
  const Color sort_fg = sort_active ? spec.colors.on_primary_container : spec.colors.on_surface;
  const Color sort_bg = sort_active ? spec.colors.primary_container : spec.colors.surface;
  const Color sort_border =
      sort_active ? WithAlpha(spec.colors.primary, 0.45F) : semantic.border_subtle;
  const Color sort_icon_color =
      sort_active ? spec.colors.primary : spec.colors.on_surface_variant;
  const std::vector<ColumnMeta> sort_cols = model.columns;
  const std::optional<DataTableSort> current_sort = model.sort;
  toolbar_items.Add(
      Row{
          TintedIcon(app::images::sort, sort_icon_color, 14.0F),
          Text(sort_label)
              .Style(TextStyle{Font::System(font_size::kChip).WithWeight(FontWeight::Medium),
                               sort_fg})
              .LineLimit(1),
          TintedIcon(app::images::chevron_down, sort_icon_color, 12.0F),
      }
          .With(Padding{EdgeInsets::Symmetric(10.0F, 6.0F)}, Spacing{6.0F},
                CrossAlign{CrossAxisAlignment::Center}, CornerRadius{6.0F},
                Background{VisualFill{sort_bg}}, Border{sort_border, 1.0F}, sort_anchor,
                Enabled{!model.columns.empty() && model.pending_inserts.empty()})
          .OnClick([menu, sort_anchor, loc, apply_sort, sort_cols, current_sort] {
            std::vector<MenuEntry> entries;
            if (current_sort.has_value()) {
              const bool is_desc = current_sort->direction == DataTableSortDirection::Descending;
              entries.emplace_back(
                  MenuItem(loc.Get(is_desc ? app::strings::table_sort_toggle_asc
                                           : app::strings::table_sort_toggle_desc),
                           [apply_sort, col_id = current_sort->column_id, is_desc] {
                             apply_sort(DataTableSort{
                                 .column_id = col_id,
                                 .direction = is_desc ? DataTableSortDirection::Ascending
                                                      : DataTableSortDirection::Descending,
                             });
                           }));
              entries.emplace_back(
                  MenuItem(loc.Get(app::strings::table_sort_clear), [apply_sort] {
                    apply_sort(std::nullopt);
                  }));
              entries.emplace_back(MenuSection{});
            }
            for (const ColumnMeta& col : sort_cols) {
              const bool active_col =
                  current_sort.has_value() && current_sort->column_id == col.name;
              const bool active_desc =
                  active_col && current_sort->direction == DataTableSortDirection::Descending;
              std::string item_label = col.name;
              if (active_col) {
                item_label += active_desc ? " ↓" : " ↑";
              }
              entries.emplace_back(
                  MenuItem(std::move(item_label),
                           [apply_sort, col_name = col.name, active_col, active_desc] {
                             DataTableSortDirection next_dir =
                                 (active_col && !active_desc) ? DataTableSortDirection::Descending
                                                              : DataTableSortDirection::Ascending;
                             apply_sort(DataTableSort{.column_id = col_name, .direction = next_dir});
                           })
                      .Checked(active_col));
            }
            menu.Show(sort_anchor, std::move(entries),
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
          }));
  toolbar_items.Add(Spacer());
  toolbar_items.Add(ActionButton(Button(app::strings::action_refresh).OnClick(refresh)));
  toolbar_items.Add(
      ActionButton(Button(is_redis ? app::strings::table_add_key : app::strings::table_add_row)
                       .OnClick(add_draft_row))
          .With(Enabled{model.editable && !model.read_only && !model.committing}));
  if (!model.pending_inserts.empty()) {
    const std::string commit_label =
        model.committing ? UseString(app::strings::table_committing)
                         : UseString(app::strings::table_commit_n, model.pending_inserts.size());
    toolbar_items.Add(
        ActionButton(Button(commit_label).OnClick(commit_inserts), ButtonTone::Primary)
            .With(Enabled{!model.committing}));
    toolbar_items.Add(
        ActionButton(Button(app::strings::table_rollback).OnClick(rollback_inserts))
            .With(Enabled{!model.committing}));
  }
  toolbar_items.Add(
      ActionButton(Button(is_redis ? app::strings::table_delete_key : app::strings::table_delete_row)
                       .OnClick(delete_selected_action),
                   ButtonTone::Danger)
          .With(Enabled{model.editable && !model.read_only && !model.committing &&
                        tab->selected_cell.has_value()}));

  View toolbar = Row{std::move(toolbar_items)}.With(CrossAlign{CrossAxisAlignment::Center},
                                                    Spacing{8.0F});

  const auto change_page_size = [app, workspace, tasks, tab_id, notify, settings,
                                 block_if_dirty](std::int64_t next_size) {
    if (block_if_dirty()) {
      return;
    }
    const std::int64_t clamped = std::clamp(next_size, kMinPageSize, kMaxPageSize);
    if (settings && settings->default_page_size.Get() != clamped) {
      (void)settings->SetDefaultPageSize(clamped);
    }
    const WorkspaceTab* cur = workspace.Get().FindTab(tab_id);
    if (cur == nullptr || (cur->table.page_size == clamped && cur->table.page == 0 &&
                           cur->table.snapshot != nullptr)) {
      return;
    }
    RequestLoadTable(app, workspace, tasks, tab_id, 0, cur->table.sort, cur->table.filter, false,
                     notify, clamped);
  };

  const std::int64_t current_page_size = model.page_size > 0 ? model.page_size : kPageSize;
  View page_size_selector =
      Row{
          Text(UseString(app::strings::table_page_size_label, current_page_size))
              .Style(TextStyle{Font::System(font_size::kCaption).WithWeight(FontWeight::Medium),
                               spec.colors.on_surface}),
          TintedIcon(app::images::chevron_down, spec.colors.on_surface_variant, 13.0F),
      }
          .With(Padding{EdgeInsets::Symmetric(10.0F, 5.0F)}, Spacing{6.0F},
                CrossAlign{CrossAxisAlignment::Center}, CornerRadius{6.0F},
                Background{VisualFill{spec.colors.surface}}, Border{semantic.border_subtle, 1.0F},
                page_size_anchor, Enabled{model.pending_inserts.empty()})
          .OnClick([page_size_popup, page_size_anchor, change_page_size, current_page_size,
                    theme_config, language_mode, system_dark] {
            page_size_popup.Show(
                page_size_anchor,
                [change_page_size, current_page_size, theme_config, language_mode,
                 system_dark](PopupContext popup) {
                  return WithDialogTheme(
                      theme_config, language_mode, system_dark,
                      PageSizePopup(popup, current_page_size, change_page_size));
                },
                PopupOptions{
                    .placement = {.side = AnchorSide::Above, .alignment = AnchorAlignment::End},
                    .gap = 4.0F,
                    .viewport_margin = 8.0F,
                    .offset = {},
                    .dismiss_on_outside_press = true,
                    .dismiss_on_cancel = true,
                    .trap_focus = false,
                    .retain_anchor_focus = false,
                    .on_dismiss_request = {},
                });
          });

  const std::int64_t page_count =
      model.total <= 0 ? 1
                       : ((model.total - 1) / std::max<std::int64_t>(1, model.page_size) + 1);
  const std::string status_text =
      !model.pending_inserts.empty()
          ? UseString(app::strings::table_pending_hint, model.pending_inserts.size())
          : (model.message.empty() ? UseString(app::strings::table_no_data) : model.message);
  View pager = Row{
      AuditDurationBadge(model.elapsed_us),
      MutedText(status_text).With(LayoutPriority(1.0F)),
      !model.filter.empty()
          ? StatusBadge(UseString(app::strings::table_active_filter, model.filter),
                        spec.colors.primary, spec.colors.primary_container,
                        WithAlpha(spec.colors.primary, 0.35F))
                .With(LayoutPriority(1.0F))
          : View{},
      model.read_only ? Chip(app::strings::table_read_only).With(LayoutPriority(1.0F)) : View{},
      AuditSqlBadge(model.executed_sql, notify),
      Spacer(),
      std::move(page_size_selector).With(LayoutPriority(1.0F)),
      ActionButton(Button(app::strings::table_prev_page)
                       .OnClick([reload, model] {
                         if (model.page > 0) {
                           reload(model.page - 1, model.filter);
                         }
                       }))
          .With(Enabled{model.page > 0 && model.pending_inserts.empty()}, LayoutPriority(1.0F)),
      ActionButton(Button(app::strings::table_next_page)
                       .OnClick([reload, model, page_count] {
                         if (model.page + 1 < page_count) {
                           reload(model.page + 1, model.filter);
                         }
                       }))
          .With(Enabled{model.page + 1 < page_count && model.pending_inserts.empty()},
                LayoutPriority(1.0F)),
  }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{8.0F});

  View table_view;
  if ((model.snapshot == nullptr && model.pending_inserts.empty()) || model.columns.empty()) {
    if (model.loading) {
      table_view = LoadingState(app::strings::table_loading);
    } else {
      table_view = TableEmptyState(!model.error.empty() ? app::strings::table_load_failed_retry
                                                        : app::strings::table_no_columns);
    }
  } else {
    const std::shared_ptr<const GridSnapshot> snapshot = model.snapshot;
    const std::vector<PendingInsertRow> drafts = model.pending_inserts;
    const std::string draft_first_empty = UseString(app::strings::table_draft_first_col_empty);
    const std::string draft_other_empty = UseString(app::strings::table_draft_other_col_empty);
    const std::string draft_acc_empty = UseString(app::strings::table_draft_accessibility_empty);
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
      column.render_cell = [snapshot, drafts, index, spec, semantic, draft_first_empty,
                            draft_other_empty, draft_acc_empty](
                               const DataTableCellContext& cell) -> DataTableCellContent {
        if (const auto draft_id = ParseDraftRowId(cell.id.row_id)) {
          const PendingInsertRow* draft = FindDraftRow(drafts, *draft_id);
          const std::string raw = (draft != nullptr && index < draft->values.size())
                                      ? draft->values[index]
                                      : std::string{};
          const bool is_blank = raw.empty();
          const std::string display =
              index == 0 ? (is_blank ? draft_first_empty : "＋ " + raw)
                         : (is_blank ? draft_other_empty : raw);
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
              .label = is_blank ? draft_acc_empty : raw,
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

    View grid =
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
                  std::optional<DataTableSort> next;
                  if (!cur->table.sort.has_value() ||
                      cur->table.sort->column_id != requested.column_id) {
                    next = DataTableSort{
                        .column_id = requested.column_id,
                        .direction = DataTableSortDirection::Ascending,
                    };
                  } else if (cur->table.sort->direction == DataTableSortDirection::Ascending) {
                    next = DataTableSort{
                        .column_id = requested.column_id,
                        .direction = DataTableSortDirection::Descending,
                    };
                  } else {
                    next = std::nullopt;
                  }
                  RequestLoadTable(app, workspace, tasks, tab_id, 0, std::move(next),
                                   cur->table.filter, false, notify);
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
            .With(Border{semantic.border_subtle, 1.0F})
            .Key("table-grid-" + std::to_string(tab_id) + "-" +
                 std::to_string(model.snapshot_version));
    const bool table_busy = model.loading || model.committing;
    const StringVariant table_busy_label =
        model.committing ? StringVariant(app::strings::table_committing)
                         : StringVariant(app::strings::table_loading);
    table_view = LoadingOverlay(std::move(grid), table_busy, table_busy_label).With(Grow(1.0F));
  }

  return Column{
      std::move(toolbar),
      (!model.error.empty() && model.snapshot != nullptr) ? ErrorText(model.error) : View{},
      std::move(table_view),
      std::move(pager),
  }.With(Grow(1.0F), Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch});
}

// ---------------------------------------------------------------------------
// SQL console
// ---------------------------------------------------------------------------

void ExecuteSqlTabQuery(const State<AppModel>& app, const State<WorkspaceModel>& workspace,
                        const std::shared_ptr<SettingsModel>& settings, std::uint64_t tab_id,
                        const TaskScope& tasks, NotifyFn notify, const LocalizedCatalog& loc,
                        std::int64_t requested_page, bool from_editor,
                        std::int64_t override_page_size) {
  const WorkspaceTab* cur_tab = workspace.Get().FindTab(tab_id);
  if (cur_tab == nullptr || cur_tab->kind != WorkspaceTabKind::Sql) {
    return;
  }
  if (!from_editor && !cur_tab->sql.paginated) {
    return;
  }
  const std::string text =
      TrimmedSql(from_editor ? cur_tab->sql.editor.text : cur_tab->sql.last_sql);
  if (text.empty()) {
    return;
  }
  const std::int64_t clamped_page =
      from_editor ? 0 : std::max<std::int64_t>(0, requested_page);
  const std::uint64_t session_id = cur_tab->session_id;
  std::shared_ptr<AdbcSession> adbc;
  bool session_found = false;
  bool session_is_redis = false;
  Dialect session_dialect = Dialect::Generic;
  std::string target_database = TrimmedSql(
      from_editor
          ? cur_tab->sql.target_database
          : (cur_tab->sql.last_database.empty() ? cur_tab->sql.target_database
                                                : cur_tab->sql.last_database));
  for (const DbSession& open : app.Get().sessions) {
    if (open.id == session_id) {
      session_found = true;
      adbc = open.session;
      session_dialect = open.dialect;
      session_is_redis = (open.dialect == Dialect::Redis);
      if (target_database.empty()) {
        target_database = DefaultSessionDatabase(open);
      }
      break;
    }
  }
  if (!adbc || adbc->IsClosed()) {
    if (session_found) {
      HandleSessionDisconnected(app, workspace, tasks, session_id, {}, notify);
    } else if (notify) {
      notify(loc.Get(app::strings::toast_connection_closed));
    }
    return;
  }

  const std::int64_t configured_limit =
      settings ? settings->default_page_size.Get() : kDefaultPageSize;
  const std::int64_t active_limit =
      override_page_size > 0
          ? std::clamp(override_page_size, kMinPageSize, kMaxPageSize)
          : (cur_tab->sql.page_size > 0 ? cur_tab->sql.page_size : configured_limit);

  const std::string running_status = loc.Get(app::strings::sql_running);
  workspace.Update([tab_id, running_status, active_limit](WorkspaceModel& ws) {
    if (WorkspaceTab* target = ws.FindTab(tab_id)) {
      target->sql.page_size = active_limit;
      target->sql.running = true;
      target->sql.error.clear();
      target->sql.status = running_status;
    }
  });

  tasks.Launch([app, workspace, tab_id, tasks, notify, adbc, session_id, session_dialect,
                session_is_redis, text, target_database, active_limit, clamped_page,
                from_editor, loc]() -> Task<void> {
    if (session_is_redis || IsSqlReadStatement(text)) {
      const SqlLimitRewriteResult rewritten =
          session_is_redis
              ? SqlLimitRewriteResult{}
              : ApplyImplicitSqlLimit(session_dialect, text, active_limit, clamped_page);
      const std::string exec_sql = rewritten.applied ? rewritten.sql : text;
      const std::size_t max_rows =
          session_is_redis
              ? kMaxQueryRows
              : (rewritten.applied
                     ? static_cast<std::size_t>(rewritten.effective_limit)
                     : std::max<std::size_t>(kMaxQueryRows,
                                             static_cast<std::size_t>(active_limit)));
      AdbcQueryResult result =
          co_await ExecuteQueryAsync(adbc, exec_sql, true, max_rows, target_database);
      if (result.connection_lost) {
        const std::string message = result.error;
        workspace.Update([tab_id](WorkspaceModel& ws) {
          if (WorkspaceTab* target = ws.FindTab(tab_id)) {
            target->sql.running = false;
            target->sql.error.clear();
            target->sql.status.clear();
          }
        });
        HandleSessionDisconnected(app, workspace, tasks, session_id, message, notify);
        co_return;
      }
      if (!result.error.empty()) {
        const std::string message = result.error;
        const std::int64_t elapsed_us = result.elapsed_us;
        workspace.Update([tab_id, exec_sql, elapsed_us](WorkspaceModel& ws) {
          if (WorkspaceTab* target = ws.FindTab(tab_id)) {
            target->sql.last_executed_sql = exec_sql;
            target->sql.last_elapsed_us = elapsed_us;
            target->sql.running = false;
            target->sql.error.clear();
            target->sql.status.clear();
          }
        });
        if (notify) {
          notify(loc.Fmt(session_is_redis ? app::strings::sql_cmd_failed
                                          : app::strings::sql_query_failed,
                         message));
        }
        co_return;
      }
      auto query = std::make_shared<QueryResult>();
      query->columns = std::move(result.columns);
      query->snapshot =
          std::make_shared<const GridSnapshot>(GridSnapshot{.rows = std::move(result.rows)});
      query->truncated = result.truncated;
      const std::int64_t elapsed_us = result.elapsed_us;
      std::unordered_map<std::string, float> auto_widths;
      auto_widths.reserve(query->columns.size());
      for (std::size_t i = 0; i < query->columns.size(); ++i) {
        auto_widths.emplace("result-" + std::to_string(i),
                            ComputeAutoColumnWidth(query->columns[i], i, query->snapshot.get()));
      }
      const std::size_t rows = query->snapshot->rows.size();
      const std::string returned_status =
          rewritten.applied
              ? loc.Fmt(query->truncated ? app::strings::sql_returned_rows_page_more
                                         : app::strings::sql_returned_rows_page,
                        rows, clamped_page + 1)
              : loc.Fmt(query->truncated ? app::strings::sql_returned_rows_truncated
                                         : app::strings::sql_returned_rows,
                        rows);
      const std::string redis_current_db =
          (session_is_redis && adbc && adbc->redis)
              ? ("db" + std::to_string(adbc->redis->CurrentDb()))
              : std::string{};
      workspace.Update([tab_id, query, returned_status, redis_current_db, rewritten, active_limit,
                        clamped_page, from_editor, text, exec_sql, elapsed_us, target_database,
                        auto_widths = std::move(auto_widths)](WorkspaceModel& ws) mutable {
        if (WorkspaceTab* target = ws.FindTab(tab_id)) {
          query->id = target->sql.next_result_id++;
          const bool keep_widths = !from_editor && target->sql.result &&
                                   target->sql.result->columns == query->columns;
          target->sql.result = query;
          if (!keep_widths) {
            target->sql.column_widths = std::move(auto_widths);
          }
          target->sql.page_size = active_limit;
          target->sql.page = rewritten.applied ? clamped_page : 0;
          target->sql.paginated = rewritten.applied;
          target->sql.has_next_page = rewritten.applied && query->truncated;
          target->sql.last_sql = rewritten.applied ? text : std::string{};
          target->sql.last_database = rewritten.applied ? target_database : std::string{};
          target->sql.last_executed_sql = exec_sql;
          target->sql.last_elapsed_us = elapsed_us;
          target->sql.running = false;
          target->sql.error.clear();
          target->sql.status = returned_status;
          if (!redis_current_db.empty()) {
            target->sql.target_database = redis_current_db;
          }
        }
      });
      if (session_is_redis && !IsReadStatement(Dialect::Redis, text)) {
        RefreshSessionObjects(app, workspace, tasks, session_id, notify, false);
      }
      co_return;
    }

    AdbcQueryResult executed =
        co_await ExecuteQueryAsync(adbc, text, false, 0, target_database);
    if (executed.connection_lost) {
      const std::string message = executed.error;
      workspace.Update([tab_id](WorkspaceModel& ws) {
        if (WorkspaceTab* target = ws.FindTab(tab_id)) {
          target->sql.running = false;
          target->sql.error.clear();
          target->sql.status.clear();
        }
      });
      HandleSessionDisconnected(app, workspace, tasks, session_id, message, notify);
      co_return;
    }
    if (!executed.error.empty()) {
      const std::string message = executed.error;
      const std::int64_t elapsed_us = executed.elapsed_us;
      workspace.Update([tab_id, text, elapsed_us](WorkspaceModel& ws) {
        if (WorkspaceTab* target = ws.FindTab(tab_id)) {
          target->sql.last_executed_sql = text;
          target->sql.last_elapsed_us = elapsed_us;
          target->sql.running = false;
          target->sql.error.clear();
          target->sql.status.clear();
        }
      });
      if (notify) {
        notify(loc.Fmt(app::strings::sql_query_failed, message));
      }
      co_return;
    }

    const std::int64_t elapsed_us = executed.elapsed_us;
    const std::string done_status =
        executed.affected >= 0
            ? loc.Fmt(app::strings::sql_affected_rows, executed.affected)
            : loc.Get(app::strings::sql_exec_done);
    workspace.Update([tab_id, done_status, text, elapsed_us](WorkspaceModel& ws) {
      if (WorkspaceTab* target = ws.FindTab(tab_id)) {
        target->sql.result.reset();
        target->sql.column_widths.clear();
        target->sql.page = 0;
        target->sql.paginated = false;
        target->sql.has_next_page = false;
        target->sql.last_sql.clear();
        target->sql.last_database.clear();
        target->sql.last_executed_sql = text;
        target->sql.last_elapsed_us = elapsed_us;
        target->sql.running = false;
        target->sql.error.clear();
        target->sql.status = done_status;
      }
    });
    RefreshSessionObjects(app, workspace, tasks, session_id, notify, false);
    if (notify) {
      notify(loc.Fmt(app::strings::sql_toast_affected, executed.affected));
    }
  });
}

[[huxerui::composable]] View SqlPage(State<AppModel> app, State<WorkspaceModel> workspace,
                                     State<std::vector<ConnectionProfile>> profiles,
                                     std::uint64_t tab_id, TaskScope tasks, NotifyFn notify,
                                     AppThemeConfig theme_config, LanguageMode language_mode,
                                     bool system_dark) {
  const ThemeSpec& spec = UseTheme();
  const SemanticColors& semantic = UseEnvironment<SemanticColors>();
  const LocalizedCatalog loc{UseService<Resources>(), UseService<SettingsModel>()};
  const auto settings = UseService<SettingsModel>();
  const auto menu = UseMenu();
  const auto limit_popup = UsePopup();
  const auto conn_anchor = UseViewHandle();
  const auto db_anchor = UseViewHandle();
  const auto limit_anchor = UseViewHandle();
  const WorkspaceTab* tab = workspace.Get().FindTab(tab_id);
  if (tab == nullptr || tab->kind != WorkspaceTabKind::Sql) {
    return TableEmptyState(app::strings::sql_tab_closed);
  }
  const SqlModel& sql_model = tab->sql;
  const std::vector<DbSession>& all_sessions = app.Get().sessions;
  const DbSession* session = nullptr;
  bool has_live_session = false;
  for (const DbSession& open : all_sessions) {
    if (open.session && !open.session->IsClosed()) {
      has_live_session = true;
    }
    if (open.id == tab->session_id) {
      session = &open;
    }
  }

  const bool session_connected =
      session != nullptr && session->session && !session->session->IsClosed();
  if (!session_connected && !has_live_session && profiles.Get().empty()) {
    return TableEmptyState(app::strings::sql_conn_disconnected);
  }
  const bool is_redis = session != nullptr && session->dialect == Dialect::Redis;
  const DatabaseBrand current_brand =
      session != nullptr
          ? DetectDatabaseBrand(session->driver_path, session->uri, session->dialect)
          : (!tab->session_uri.empty()
                 ? DetectDatabaseBrand({}, tab->session_uri, Dialect::Generic)
                 : DatabaseBrand::Custom);
  const std::string offline_tag = loc.Get(app::strings::sql_conn_offline_tag);
  const std::string selector_label =
      session_connected
          ? session->name
          : (session != nullptr
                 ? session->name + " (" + offline_tag + ")"
                 : (!tab->session_name.empty()
                        ? tab->session_name + " (" + offline_tag + ")"
                        : loc.Get(app::strings::sql_select_connection)));

  const auto execute_query = [app, workspace, settings, tab_id, tasks, notify,
                              loc](std::int64_t requested_page, bool from_editor,
                                   std::int64_t override_page_size) {
    ExecuteSqlTabQuery(app, workspace, settings, tab_id, tasks, notify, loc, requested_page,
                       from_editor, override_page_size);
  };
  const auto run = [execute_query] { execute_query(0, true, 0); };

  View result_view;
  if (sql_model.running && !sql_model.result) {
    result_view = LoadingState(app::strings::sql_running);
  } else {
    View base_result;
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
        base_result = DataTable(std::move(options))
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
                          .With(Border{semantic.border_subtle, 1.0F})
                          .Key("sql-result-" + std::to_string(result_id));
      } else {
        base_result = TableEmptyState(sql_model.status);
      }
    } else {
      base_result = TableEmptyState(app::strings::sql_empty_prompt);
    }
    result_view = LoadingOverlay(std::move(base_result), sql_model.running, app::strings::sql_running)
                      .With(Grow(1.0F));
  }

  const std::uint64_t cur_session_id = tab->session_id;
  View connection_selector =
      Row{
          DatabaseIcon(current_brand, 15.0F),
          Text(selector_label)
              .Style(TextStyle{Font::System(font_size::kChip).WithWeight(FontWeight::Medium),
                               session_connected ? spec.colors.on_surface
                                                 : spec.colors.on_surface_variant})
              .LineLimit(1)
              .Overflow(TextOverflow::Ellipsis)
              .With(Frame{.max_width = 220.0F}),
          TintedIcon(app::images::chevron_down, spec.colors.on_surface_variant, 13.0F),
      }
          .With(Frame{.min_height = 30.0F}, Padding{EdgeInsets::Symmetric(10.0F, 5.0F)},
                Spacing{6.0F}, CrossAlign{CrossAxisAlignment::Center}, CornerRadius{6.0F},
                Background{VisualFill{spec.colors.surface}}, Border{semantic.border_subtle, 1.0F},
                conn_anchor, Enabled{!sql_model.running})
          .OnClick([menu, conn_anchor, app, workspace, profiles, tasks, notify, loc, tab_id,
                    cur_session_id] {
            const std::vector<DbSession>& live_pool = app.Get().sessions;
            std::unordered_map<std::string, int> name_counts;
            std::vector<bool> claimed(live_pool.size(), false);
            bool any_live = false;
            for (std::size_t i = 0; i < live_pool.size(); ++i) {
              const DbSession& s = live_pool[i];
              if (!s.session || s.session->IsClosed()) {
                claimed[i] = true;
                continue;
              }
              any_live = true;
              ++name_counts[s.name];
            }

            std::vector<MenuEntry> entries;
            for (const DbSession& s : live_pool) {
              if (!s.session || s.session->IsClosed()) {
                continue;
              }
              std::string item_label = s.name;
              if (name_counts[s.name] > 1) {
                const std::string uri_name = UriDisplayName(s.uri);
                if (!uri_name.empty()) {
                  item_label += " (" + uri_name + ")";
                }
              }
              const std::uint64_t sid = s.id;
              entries.emplace_back(
                  MenuItem(std::move(item_label),
                           [app, workspace, tab_id, sid] {
                             (void)SwitchSqlTabSession(app, workspace, tab_id, sid);
                           })
                      .Checked(sid == cur_session_id));
            }

            const std::vector<ConnectionProfile>& saved_profiles = profiles.Get();
            const std::string offline = loc.Get(app::strings::sql_conn_offline_tag);
            bool added_section = false;
            for (const ConnectionProfile& profile : saved_profiles) {
              if (const auto match = FindMatchingSessionIndex(profile, live_pool, claimed)) {
                claimed[*match] = true;
                continue;
              }
              if (any_live && !added_section) {
                entries.emplace_back(MenuSection{});
                added_section = true;
              }
              const SanitizedUriResult sanitized = SanitizeUriCredentials(profile.uri);
              const std::string display =
                  profile.name.empty() ? UriDisplayName(sanitized.uri) : profile.name;
              entries.emplace_back(MenuItem(
                  display + " (" + offline + ")",
                  [app, workspace, tasks, notify, tab_id, profile, sanitized, display] {
                    ConnectionRequest request{
                        .driver_path = profile.driver_path,
                        .uri = sanitized.uri,
                        .username = profile.username.empty() ? sanitized.username : profile.username,
                        .password = profile.password.empty() ? sanitized.password : profile.password,
                        .title = display,
                    };
                    OpenConnection(
                        app, workspace, tasks, std::move(request), notify,
                        [app, workspace, tab_id](std::uint64_t sid) {
                          (void)SwitchSqlTabSession(app, workspace, tab_id, sid);
                        });
                  }));
            }

            if (entries.empty()) {
              return;
            }
            menu.Show(conn_anchor, std::move(entries),
                      MenuOptions{
                          .placement = {.side = AnchorSide::Below,
                                        .alignment = AnchorAlignment::End},
                          .gap = 4.0F,
                          .viewport_margin = 8.0F,
                          .offset = {},
                          .width = std::nullopt,
                          .dismiss_on_outside_press = true,
                          .dismiss_on_cancel = true,
                          .on_dismiss_request = {},
                      });
          });

  const std::vector<std::string> session_dbs =
      session_connected ? ListSessionDatabases(*session) : std::vector<std::string>{};
  const std::string active_db =
      !sql_model.target_database.empty()
          ? sql_model.target_database
          : (session_connected ? DefaultSessionDatabase(*session) : std::string{});
  const std::string db_selector_label =
      active_db.empty() ? loc.Get(app::strings::sql_select_database) : active_db;
  View database_selector =
      Row{
          TintedIcon(app::images::database,
                     session_connected ? spec.colors.primary : spec.colors.on_surface_variant,
                     14.0F),
          Text(db_selector_label)
              .Style(TextStyle{Font::System(font_size::kChip).WithWeight(FontWeight::Medium),
                               session_connected && !active_db.empty()
                                   ? spec.colors.on_surface
                                   : spec.colors.on_surface_variant})
              .LineLimit(1)
              .Overflow(TextOverflow::Ellipsis)
              .With(Frame{.max_width = 180.0F}),
          TintedIcon(app::images::chevron_down, spec.colors.on_surface_variant, 13.0F),
      }
          .With(Frame{.min_height = 30.0F}, Padding{EdgeInsets::Symmetric(10.0F, 5.0F)},
                Spacing{6.0F}, CrossAlign{CrossAxisAlignment::Center}, CornerRadius{6.0F},
                Background{VisualFill{spec.colors.surface}}, Border{semantic.border_subtle, 1.0F},
                db_anchor, Enabled{!sql_model.running && session_connected && !session_dbs.empty()})
          .OnClick([menu, db_anchor, app, workspace, tab_id] {
            const WorkspaceTab* cur_tab = workspace.Get().FindTab(tab_id);
            if (cur_tab == nullptr || cur_tab->kind != WorkspaceTabKind::Sql) {
              return;
            }
            const DbSession* live_session = nullptr;
            for (const DbSession& open : app.Get().sessions) {
              if (open.id == cur_tab->session_id && open.session && !open.session->IsClosed()) {
                live_session = &open;
                break;
              }
            }
            if (live_session == nullptr) {
              return;
            }
            const std::vector<std::string> dbs = ListSessionDatabases(*live_session);
            if (dbs.empty()) {
              return;
            }
            const std::string current_db = !cur_tab->sql.target_database.empty()
                                               ? cur_tab->sql.target_database
                                               : DefaultSessionDatabase(*live_session);
            const bool is_redis_sess = live_session->dialect == Dialect::Redis;
            bool has_non_sys = false;
            if (!is_redis_sess) {
              for (const std::string& db : dbs) {
                if (!IsSystemDatabaseOrSchema(db)) {
                  has_non_sys = true;
                  break;
                }
              }
            }
            std::vector<MenuEntry> entries;
            bool inserted_sys_sep = false;
            for (const std::string& db : dbs) {
              if (!is_redis_sess && has_non_sys && !inserted_sys_sep &&
                  IsSystemDatabaseOrSchema(db)) {
                entries.emplace_back(MenuSection{});
                inserted_sys_sep = true;
              }
              entries.emplace_back(
                  MenuItem(db,
                           [app, workspace, tab_id, db] {
                             (void)SwitchSqlTabDatabase(app, workspace, tab_id, db);
                           })
                      .Checked(db == current_db));
            }
            menu.Show(db_anchor, std::move(entries),
                      MenuOptions{
                          .placement = {.side = AnchorSide::Below,
                                        .alignment = AnchorAlignment::End},
                          .gap = 4.0F,
                          .viewport_margin = 8.0F,
                          .offset = {},
                          .width = std::nullopt,
                          .dismiss_on_outside_press = true,
                          .dismiss_on_cancel = true,
                          .on_dismiss_request = {},
                      });
          });

  const std::string status_display =
      (!session_connected && sql_model.status.empty())
          ? UseString(app::strings::sql_conn_disconnected)
          : sql_model.status;

  const std::int64_t effective_page_size =
      sql_model.page_size > 0
          ? sql_model.page_size
          : (settings ? settings->default_page_size.Get() : kDefaultPageSize);
  View sql_limit_selector = View{};
  if (!is_redis) {
    sql_limit_selector =
        Row{
            Text(UseString(app::strings::table_page_size_label, effective_page_size))
                .Style(TextStyle{Font::System(font_size::kCaption).WithWeight(FontWeight::Medium),
                                 spec.colors.on_surface}),
            TintedIcon(app::images::chevron_down, spec.colors.on_surface_variant, 13.0F),
        }
            .With(Padding{EdgeInsets::Symmetric(10.0F, 5.0F)}, Spacing{6.0F},
                  CrossAlign{CrossAxisAlignment::Center}, CornerRadius{6.0F},
                  Background{VisualFill{spec.colors.surface}}, Border{semantic.border_subtle, 1.0F},
                  limit_anchor, Enabled{!sql_model.running})
            .OnClick([limit_popup, limit_anchor, workspace, tab_id, settings, execute_query,
                      effective_page_size, theme_config, language_mode, system_dark] {
              const auto apply_limit = [workspace, tab_id, settings,
                                        execute_query](std::int64_t next_size) {
                const std::int64_t clamped = std::clamp(next_size, kMinPageSize, kMaxPageSize);
                enum class ReloadKind { None, LastSql, EditorSql };
                ReloadKind reload_kind = ReloadKind::None;
                workspace.Update([tab_id, clamped, &reload_kind](WorkspaceModel& ws) {
                  if (WorkspaceTab* target = ws.FindTab(tab_id);
                      target != nullptr && target->kind == WorkspaceTabKind::Sql) {
                    target->sql.page_size = clamped;
                    if (!target->sql.running) {
                      if (target->sql.paginated && !target->sql.last_sql.empty()) {
                        reload_kind = ReloadKind::LastSql;
                      } else if (target->sql.result != nullptr) {
                        const std::string editor_sql = TrimmedSql(target->sql.editor.text);
                        if (!editor_sql.empty() && IsSqlReadStatement(editor_sql)) {
                          reload_kind = ReloadKind::EditorSql;
                        }
                      }
                    }
                  }
                });
                if (settings) {
                  (void)settings->SetDefaultPageSize(clamped);
                }
                if (reload_kind == ReloadKind::LastSql) {
                  execute_query(0, false, clamped);
                } else if (reload_kind == ReloadKind::EditorSql) {
                  execute_query(0, true, clamped);
                }
              };
              limit_popup.Show(
                  limit_anchor,
                  [apply_limit, effective_page_size, theme_config, language_mode,
                   system_dark](PopupContext popup) {
                    return WithDialogTheme(
                        theme_config, language_mode, system_dark,
                        PageSizePopup(popup, effective_page_size, apply_limit));
                  },
                  PopupOptions{
                      .placement = {.side = AnchorSide::Above, .alignment = AnchorAlignment::End},
                      .gap = 4.0F,
                      .viewport_margin = 8.0F,
                      .offset = {},
                      .dismiss_on_outside_press = true,
                      .dismiss_on_cancel = true,
                      .trap_focus = false,
                      .retain_anchor_focus = false,
                      .on_dismiss_request = {},
                  });
            });
  }

  View sql_footer =
      !is_redis
          ? Row{
                AuditDurationBadge(sql_model.last_elapsed_us),
                MutedText(status_display).With(LayoutPriority(1.0F)),
                AuditSqlBadge(sql_model.last_executed_sql, notify),
                Spacer(),
                std::move(sql_limit_selector).With(LayoutPriority(1.0F)),
                ActionButton(Button(app::strings::table_prev_page)
                                 .OnClick([execute_query, sql_model] {
                                   if (sql_model.paginated && sql_model.page > 0) {
                                     execute_query(sql_model.page - 1, false, 0);
                                   }
                                 }))
                    .With(Enabled{!sql_model.running && session_connected &&
                                  sql_model.paginated && sql_model.page > 0},
                          LayoutPriority(1.0F)),
                ActionButton(Button(app::strings::table_next_page)
                                 .OnClick([execute_query, sql_model] {
                                   if (sql_model.paginated && sql_model.has_next_page) {
                                     execute_query(sql_model.page + 1, false, 0);
                                   }
                                 }))
                    .With(Enabled{!sql_model.running && session_connected &&
                                  sql_model.paginated && sql_model.has_next_page},
                          LayoutPriority(1.0F)),
            }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{8.0F})
          : Row{
                AuditDurationBadge(sql_model.last_elapsed_us),
                MutedText(status_display).With(LayoutPriority(1.0F)),
                AuditSqlBadge(sql_model.last_executed_sql, notify),
                Spacer(),
            }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{8.0F});

  return Column{
      Row{
          DatabaseIcon(current_brand, 20.0F),
          Text(is_redis ? app::strings::sql_title_redis : app::strings::sql_title_sql,
               TextRole::Title),
          Spacer(),
          std::move(connection_selector),
          std::move(database_selector),
          ActionButton(Button(sql_model.running ? app::strings::sql_running : app::strings::sql_run)
                           .OnClick(run)
                           .With(Enabled{!sql_model.running && session_connected}),
                       ButtonTone::Primary),
      }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{8.0F}),
      MonospaceField(
          TextField(sql_model.editor)
              .Label(is_redis ? app::strings::sql_label_redis : app::strings::sql_label_sql)
              .Placeholder(is_redis ? StringVariant(app::strings::sql_ph_redis)
                                    : StringVariant("SELECT * FROM table;"))
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
      MutedText(UseString(is_redis ? app::strings::sql_hint_redis : app::strings::sql_hint_sql)),
      std::move(result_view),
      std::move(sql_footer),
  }.With(Grow(1.0F), Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch});
}

// ---------------------------------------------------------------------------
// Settings page & tab list popup
// ---------------------------------------------------------------------------

[[huxerui::composable]] View AllTabsPopup(
    PopupContext popup, State<AppModel> app, State<WorkspaceModel> workspace,
    std::function<void(std::vector<std::uint64_t>)> request_close_tabs) {
  const ThemeSpec& spec = UseTheme();
  const SemanticColors& semantic = UseEnvironment<SemanticColors>();
  auto search = UseState(TextEditingValue{});
  const WorkspaceModel& ws = workspace.Get();
  const AppModel& app_model = app.Get();
  const std::string query = TrimmedSql(search.Get().text);

  TextField search_field =
      TextField(search)
          .Placeholder(app::strings::tabs_list_search_ph)
          .Variant(TextFieldVariant::Outlined)
          .LineLimits(TextFieldLineLimits::SingleLine())
          .LeadingIcon(app::images::search)
          .OnChanged([search](const TextEditingValue& val) { search = val; });
  if (!query.empty()) {
    search_field =
        std::move(search_field)
            .TrailingIcon(app::images::close, app::strings::action_clear)
            .OnTrailingIconClick([search] { search = TextEditingValue{}; });
  }

  Views items;
  items.Reserve(ws.tabs.size());
  for (const WorkspaceTab& t : ws.tabs) {
    if (!query.empty() && !ContainsIgnoreCaseAscii(t.title, query) &&
        !ContainsIgnoreCaseAscii(t.session_name, query) &&
        !ContainsIgnoreCaseAscii(t.object_name, query)) {
      continue;
    }
    const std::uint64_t tid = t.id;
    const bool is_active = tid == ws.active_tab_id;
    const Color icon_color =
        is_active ? spec.colors.primary : spec.colors.on_surface_variant;
    const ImageResource& icon_res =
        t.kind == WorkspaceTabKind::Settings
            ? app::images::settings
            : (t.kind == WorkspaceTabKind::Sql
                   ? app::images::sql_console
                   : (t.is_view ? app::images::view : app::images::table));
    const std::string detail =
        t.kind == WorkspaceTabKind::Settings
            ? std::string{}
            : (app_model.sessions.size() > 1 ? t.session_name : std::string{});
    const bool dirty = t.kind == WorkspaceTabKind::Table && !t.table.pending_inserts.empty();
    const std::string label = dirty ? ("● " + t.title) : t.title;

    View close_btn =
        TintedIcon(app::images::close, spec.colors.on_surface_variant, 12.0F)
            .With(Padding{EdgeInsets::Symmetric(4.0F, 4.0F)}, CornerRadius{4.0F})
            .OnClick([request_close_tabs, tid] { request_close_tabs({tid}); });

    items.Add(NavItem(label, detail, is_active,
                      [app, workspace, tid, popup] {
                        ActivateWorkspaceTab(app, workspace, tid);
                        popup.Dismiss();
                      },
                      std::move(close_btn), std::nullopt,
                      TintedIcon(icon_res, icon_color, 15.0F))
                  .Key("popup-tab-" + std::to_string(tid)));
  }

  View list_body =
      items.Size() == 0
          ? View(MutedText(UseString(app::strings::tabs_list_empty))
                     .With(Padding{EdgeInsets::Symmetric(8.0F, 12.0F)}))
          : View(ScrollView(Column{std::move(items)}
                                .With(Spacing{2.0F}, CrossAlign{CrossAxisAlignment::Stretch}))
                     .With(Frame{.max_height = 320.0F}));

  return Column{
      Row{
          Caption(UseString(app::strings::tabs_list_count, ws.tabs.size())),
          Spacer(),
      }.With(CrossAlign{CrossAxisAlignment::Center}),
      std::move(search_field),
      std::move(list_body),
  }.With(Frame{.width = 320.0F}, Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch},
         Background{VisualFill{spec.colors.surface_container_highest}}, CornerRadius{10.0F},
         Border{semantic.border_subtle, 1.0F}, Padding{EdgeInsets::Symmetric(12.0F, 12.0F)});
}

[[huxerui::composable]] View SettingsPage(NotifyFn notify,
                                          std::function<void(std::int64_t)> on_page_size_applied = {}) {
  const auto settings = UseService<SettingsModel>();
  const auto mcp_service = UseService<McpServerService>();
  const LocalizedCatalog loc{UseService<Resources>(), settings};
  const ThemeSpec& spec = UseTheme();
  const SemanticColors& semantic = UseEnvironment<SemanticColors>();
  const auto clipboard = UseApplication().Clipboard();
  auto tasks = UseTaskScope();
  auto transition = UseSceneTransition();
  auto section = UseState<int>(0);
  auto mcp_host_input = UseState(TextEditingValue::FromText(settings->mcp_host.Get()));
  auto mcp_port_input =
      UseState(TextEditingValue::FromText(std::to_string(settings->mcp_port.Get())));
  const std::int64_t persisted_page_size = settings->default_page_size.Get();
  auto page_size_input = UseState(TextEditingValue::FromText(std::to_string(persisted_page_size)));
  Lifecycle(
      [page_size_input, persisted_page_size] {
        const std::string expected = std::to_string(persisted_page_size);
        if (page_size_input.Get().text != expected) {
          page_size_input = TextEditingValue::FromText(expected);
        }
        return [] {};
      },
      persisted_page_size);
  // 动画互斥旗标放 shared_ptr 里改：不经过 State 更新，防抖期间不触发重组。
  struct ThemeAnimationFlag {
    bool animating = false;
  };
  auto animating = UseState(std::make_shared<ThemeAnimationFlag>());
  const bool system_dark = UseSystemThemeBrightness() == ThemeBrightness::Dark;
  const AppThemeConfig theme_config = settings->ThemeConfig();
  const LanguageMode language_mode = settings->language.Get();

  const auto copy_text = [clipboard, notify, loc](std::string text, StringResource label_res) {
    if (clipboard && clipboard->SupportsSynchronousText() && clipboard->WriteText(text)) {
      if (notify) {
        notify(loc.Fmt(app::strings::toast_copied, loc.Get(label_res)));
      }
    } else if (notify) {
      notify(loc.Get(app::strings::toast_clipboard_unsupported));
    }
  };

  // 明暗真正变化时使用圆形扩散过渡（参考 Clash-Flux），并将偏好持久化到 dbpp.sqlite3。
  const auto apply_mode = [settings, notify, transition, tasks, animating, system_dark, loc,
                           current_mode = theme_config.mode,
                           motion = spec.motion](ThemeMode next_mode) {
    if (animating.Get()->animating) {
      return;
    }
    const bool current_dark = ResolveIsDark(current_mode, system_dark);
    const bool target_dark = ResolveIsDark(next_mode, system_dark);
    const auto mutation = [settings, notify, next_mode, loc] {
      if (!settings->SetThemeMode(next_mode) && notify) {
        notify(loc.Get(app::strings::toast_theme_save_failed));
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

  const int active_section = section.Get();
  View left_nav = Column{
      NavItem(UseString(app::strings::settings_nav_appearance), {}, active_section == 0,
              [section] { section = 0; }, {}, std::nullopt,
              TintedIcon(app::images::palette,
                         active_section == 0 ? spec.colors.primary : spec.colors.on_surface_variant,
                         16.0F)),
      NavItem(UseString(app::strings::settings_nav_mcp), {}, active_section == 1,
              [section] { section = 1; }, {}, std::nullopt,
              TintedIcon(app::images::mcp,
                         active_section == 1 ? spec.colors.primary : spec.colors.on_surface_variant,
                         16.0F)),
      NavItem(UseString(app::strings::settings_nav_about), {}, active_section == 2,
              [section] { section = 2; }, {}, std::nullopt,
              TintedIcon(app::images::info,
                         active_section == 2 ? spec.colors.primary : spec.colors.on_surface_variant,
                         16.0F)),
      Spacer(),
  }.With(Frame{.width = 188.0F}, Padding{EdgeInsets::Symmetric(8.0F, 8.0F)}, Spacing{4.0F},
         CrossAlign{CrossAxisAlignment::Stretch});

  View right_pane;
  if (active_section == 0) {
    const auto& presets = AvailableThemePresets();
    const auto make_preset_card = [settings, notify, &spec, &semantic, loc,
                                   active_preset = theme_config.preset](
                                      const ThemePresetMeta& meta) {
      const bool selected = meta.preset == active_preset;
      const Color title_color =
          selected ? spec.colors.on_primary_container : spec.colors.on_surface;
      const Color sub_color = selected ? WithAlpha(spec.colors.on_primary_container, 0.80F)
                                       : spec.colors.on_surface_variant;
      const auto [name_res, sub_res] = ThemePresetStrings(meta.preset);
      return Column{
          Spacer().With(Grow(0.0F), Frame{.height = 4.0F, .min_height = 4.0F, .max_height = 4.0F},
                        CornerRadius{2.0F}, Background{meta.swatch_bar}),
          Row{
              Text(name_res).Style(
                  TextStyle{Font::System(font_size::kBody).WithWeight(FontWeight::SemiBold),
                            title_color}),
              Spacer(),
              AccentDot(meta.swatch_a, 8.0F),
              AccentDot(meta.swatch_b, 8.0F),
              AccentDot(meta.swatch_c, 8.0F),
          }.With(Spacing{4.0F}, CrossAlign{CrossAxisAlignment::Center}),
          Text(sub_res)
              .Style(TextStyle{Font::System(font_size::kCaption), sub_color})
              .LineLimit(1)
              .Overflow(TextOverflow::Ellipsis),
      }
          .With(Grow(1.0F), Padding{EdgeInsets::Symmetric(12.0F, 10.0F)}, Spacing{6.0F},
                CrossAlign{CrossAxisAlignment::Stretch}, CornerRadius{8.0F},
                Background{VisualFill{selected ? spec.colors.primary_container : spec.colors.surface}},
                Border{selected ? spec.colors.primary : semantic.border_subtle,
                       selected ? 2.0F : 1.0F})
          .OnClick([settings, notify, preset = meta.preset, loc] {
            if (!settings->SetThemePreset(preset) && notify) {
              notify(loc.Get(app::strings::toast_theme_save_failed));
            }
          })
          .Key("theme-preset-" + std::string(ThemePresetKey(meta.preset)));
    };

    Views preset_rows;
    preset_rows.Reserve((presets.size() + 1) / 2);
    for (std::size_t i = 0; i < presets.size(); i += 2) {
      View left_card = make_preset_card(presets[i]);
      View right_slot =
          (i + 1 < presets.size()) ? make_preset_card(presets[i + 1])
                                   : View(Spacer().With(Grow(1.0F)));
      preset_rows.Add(Row{std::move(left_card), std::move(right_slot)}
                          .With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch})
                          .Key("theme-preset-row-" + std::to_string(i)));
    }

    right_pane = Column{
        Text(app::strings::settings_nav_appearance, TextRole::Title),
        Caption(UseString(app::strings::settings_section_language)),
        SegmentedButton({app::strings::settings_lang_system, app::strings::settings_lang_zh,
                         app::strings::settings_lang_en},
                        static_cast<std::size_t>(language_mode))
            .OnChanged([settings, notify, loc](std::size_t index) {
              const auto next = static_cast<LanguageMode>(index);
              const Locale platform =
                  loc.resources ? loc.resources->Configuration().locale : Locale::Default();
              SetActiveUiLanguage(ResolveUiLanguage(next, platform.LanguageTag()));
              if (!settings->SetLanguage(next) && notify) {
                notify(loc.Get(app::strings::toast_language_save_failed));
              }
            }),
        Caption(UseString(app::strings::settings_section_appearance)),
        SegmentedButton({app::strings::settings_theme_system, app::strings::settings_theme_dark,
                         app::strings::settings_theme_light},
                        static_cast<std::size_t>(theme_config.mode))
            .OnChanged([apply_mode](std::size_t index) {
              apply_mode(static_cast<ThemeMode>(index));
            }),
        Caption(UseString(app::strings::settings_section_palette)),
        Column{std::move(preset_rows)}.With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch}),
        Divider(),
        Caption(UseString(app::strings::settings_section_query)),
        [&]() -> View {
          const auto apply_page_size = [settings, page_size_input, notify, loc,
                                        on_page_size_applied] {
            const std::optional<std::int64_t> parsed =
                TryParsePageSize(page_size_input.Get().text);
            if (!parsed.has_value()) {
              if (notify) {
                notify(loc.Get(app::strings::table_page_size_invalid));
              }
              return;
            }
            (void)settings->SetDefaultPageSize(*parsed);
            page_size_input = TextEditingValue::FromText(std::to_string(*parsed));
            if (on_page_size_applied) {
              on_page_size_applied(*parsed);
            }
            if (notify) {
              notify(loc.Fmt(app::strings::toast_page_size_applied, *parsed));
            }
          };
          return Column{
              Row{
                  TextField(page_size_input)
                      .Label(app::strings::settings_default_page_size_label)
                      .Placeholder(app::strings::table_page_size_custom_ph)
                      .Variant(TextFieldVariant::Outlined)
                      .LineLimits(TextFieldLineLimits::SingleLine())
                      .OnChanged([page_size_input](const TextEditingValue& val) {
                        page_size_input = val;
                      })
                      .OnSubmitted(apply_page_size)
                      .With(Grow(1.0F)),
                  ActionButton(Button(app::strings::action_apply).OnClick(apply_page_size),
                               ButtonTone::Primary),
              }.With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Center}),
              MutedText(UseString(app::strings::settings_default_page_size_hint)),
          }.With(Spacing{6.0F}, CrossAlign{CrossAxisAlignment::Stretch});
        }(),
        Divider(),
        Caption(UseString(app::strings::settings_section_tray)),
        Switch(app::strings::settings_tray_enable, settings->tray_enabled)
            .OnChanged([settings, notify, loc](bool value) {
              if (!settings->SetTrayEnabled(value) && notify) {
                notify(loc.Get(app::strings::toast_tray_save_failed));
              }
            }),
        Switch(app::strings::settings_tray_lightweight, settings->lightweight)
            .OnChanged([settings, notify, loc](bool value) {
              if (!settings->SetLightweight(value) && notify) {
                notify(loc.Get(app::strings::toast_tray_save_failed));
              }
            })
            .With(Enabled{settings->tray_enabled.Get()}),
        MutedText(UseString(app::strings::settings_tray_lightweight_hint)),
        MutedText(UseString(app::strings::settings_footer)),
    }.With(Frame{.max_width = 640.0F}, Spacing{10.0F}, CrossAlign{CrossAxisAlignment::Stretch});
  } else if (active_section == 1) {
    const McpServerStatus mcp_status =
        mcp_service ? mcp_service->status.Get() : McpServerStatus{};
    const bool mcp_enabled = settings->mcp_enabled.Get();
    const std::string effective_host = NormalizeMcpHost(settings->mcp_host.Get());
    const std::uint16_t effective_port =
        mcp_status.running && mcp_status.bound_port != 0 ? mcp_status.bound_port
                                                         : settings->mcp_port.Get();
    const std::string endpoint_url = BuildMcpEndpointUrl(effective_host, effective_port);
    const std::string snippet = BuildMcpClientConfigJson(endpoint_url);

    const auto apply_mcp_config = [settings, mcp_service, mcp_host_input, mcp_port_input, notify,
                                   loc] {
      const std::string raw_port = TrimmedSql(mcp_port_input.Get().text);
      unsigned int parsed_port = 0;
      const auto [end, ec] =
          std::from_chars(raw_port.data(), raw_port.data() + raw_port.size(), parsed_port);
      if (raw_port.empty() || ec != std::errc{} || end != raw_port.data() + raw_port.size() ||
          parsed_port == 0 || parsed_port > 65535U) {
        if (notify) {
          notify(loc.Get(app::strings::toast_mcp_port_invalid));
        }
        return;
      }
      const std::string next_host = NormalizeMcpHost(mcp_host_input.Get().text);
      const std::uint16_t next_port = static_cast<std::uint16_t>(parsed_port);
      (void)settings->SetMcpHost(next_host);
      (void)settings->SetMcpPort(next_port);
      mcp_host_input = TextEditingValue::FromText(next_host);
      mcp_port_input = TextEditingValue::FromText(std::to_string(next_port));
      if (mcp_service) {
        const bool ok = mcp_service->ApplySettings(settings->mcp_enabled.Get(), next_host,
                                                   next_port, settings->mcp_read_only.Get());
        if (!ok) {
          const McpServerStatus st = mcp_service->status.Get();
          if (notify) {
            notify(loc.Fmt(app::strings::toast_mcp_failed, st.error));
          }
          return;
        }
      }
      if (notify) {
        notify(loc.Get(app::strings::toast_mcp_applied));
      }
    };

    View status_badge =
        mcp_status.running
            ? StatusBadge(UseString(app::strings::settings_mcp_status_running), semantic.success,
                          WithAlpha(semantic.success, 0.14F), WithAlpha(semantic.success, 0.40F))
            : StatusBadge(UseString(app::strings::settings_mcp_status_stopped),
                          spec.colors.on_surface_variant, spec.colors.surface_container,
                          semantic.border_subtle);

    right_pane = Column{
        Row{
            TintedIcon(app::images::mcp, spec.colors.primary, 20.0F),
            Text(app::strings::settings_mcp_title, TextRole::Title),
            Spacer(),
            std::move(status_badge),
        }.With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Center}),
        MutedText(UseString(app::strings::settings_mcp_desc)),
        Switch(app::strings::settings_mcp_enable, settings->mcp_enabled)
            .OnChanged([settings, mcp_service, notify, loc](bool value) {
              (void)settings->SetMcpEnabled(value);
              if (mcp_service) {
                const bool ok = mcp_service->ApplySettings(value, settings->mcp_host.Get(),
                                                           settings->mcp_port.Get(),
                                                           settings->mcp_read_only.Get());
                if (!ok) {
                  const McpServerStatus st = mcp_service->status.Get();
                  if (notify) {
                    notify(loc.Fmt(app::strings::toast_mcp_failed, st.error));
                  }
                }
              }
            }),
        Switch(app::strings::settings_mcp_readonly, settings->mcp_read_only)
            .OnChanged([settings, mcp_service, mcp_enabled, notify, loc](bool value) {
              (void)settings->SetMcpReadOnly(value);
              if (mcp_service && mcp_enabled) {
                const bool ok = mcp_service->ApplySettings(true, settings->mcp_host.Get(),
                                                           settings->mcp_port.Get(), value);
                if (!ok && notify) {
                  notify(loc.Fmt(app::strings::toast_mcp_failed, mcp_service->status.Get().error));
                }
              }
            }),
        Row{
            TextField(mcp_host_input)
                .Label(app::strings::settings_mcp_host_label)
                .Variant(TextFieldVariant::Outlined)
                .LineLimits(TextFieldLineLimits::SingleLine())
                .OnChanged([mcp_host_input](const TextEditingValue& val) { mcp_host_input = val; })
                .With(Grow(1.0F)),
            TextField(mcp_port_input)
                .Label(app::strings::settings_mcp_port_label)
                .Variant(TextFieldVariant::Outlined)
                .LineLimits(TextFieldLineLimits::SingleLine())
                .OnChanged([mcp_port_input](const TextEditingValue& val) { mcp_port_input = val; })
                .With(Frame{.width = 140.0F}),
            ActionButton(Button(app::strings::settings_mcp_apply).OnClick(apply_mcp_config),
                         ButtonTone::Primary),
        }.With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Center}),
        !mcp_status.error.empty() ? ErrorText(mcp_status.error) : View{},
        Divider(),
        Row{
            Caption(UseString(app::strings::settings_mcp_endpoint_label)),
            Spacer(),
            ActionButton(Button(app::strings::settings_mcp_copy_endpoint)
                             .OnClick([copy_text, endpoint_url] {
                               copy_text(endpoint_url, app::strings::settings_mcp_endpoint_copied_label);
                             })),
        }.With(CrossAlign{CrossAxisAlignment::Center}),
        Text(endpoint_url)
            .Style(TextStyle{Font::Monospace(13.0F), spec.colors.on_surface})
            .With(Padding{EdgeInsets::Symmetric(10.0F, 8.0F)}, CornerRadius{6.0F},
                  Background{VisualFill{spec.colors.surface}},
                  Border{semantic.border_subtle, 1.0F}),
        Row{
            Caption(UseString(app::strings::settings_mcp_snippet_label)),
            Spacer(),
            ActionButton(Button(app::strings::settings_mcp_copy_snippet)
                             .OnClick([copy_text, snippet] {
                               copy_text(snippet, app::strings::settings_mcp_snippet_copied_label);
                             })),
        }.With(CrossAlign{CrossAxisAlignment::Center}),
        Text(snippet)
            .Style(TextStyle{Font::Monospace(12.0F), spec.colors.on_surface_variant})
            .With(Padding{EdgeInsets::Symmetric(10.0F, 8.0F)}, CornerRadius{6.0F},
                  Background{VisualFill{spec.colors.surface}},
                  Border{semantic.border_subtle, 1.0F}),
        Divider(),
        Caption(UseString(app::strings::settings_mcp_tools_heading)),
        Column{
            MutedText(UseString(app::strings::settings_mcp_tool_profiles)),
            MutedText(UseString(app::strings::settings_mcp_tool_tables)),
            MutedText(UseString(app::strings::settings_mcp_tool_describe)),
            MutedText(UseString(app::strings::settings_mcp_tool_query)),
        }.With(Spacing{4.0F}, CrossAlign{CrossAxisAlignment::Stretch}),
    }.With(Frame{.max_width = 640.0F}, Spacing{10.0F}, CrossAlign{CrossAxisAlignment::Stretch});
  } else {
    const auto open_link = [notify, loc](std::string_view url) {
      if (!OpenExternalUrl(url) && notify) {
        notify(loc.Fmt(app::strings::toast_open_url_failed, std::string(url)));
      }
    };

    const auto link_row = [open_link, &spec](StringResource title_res, StringResource url_res) {
      const std::string url = UseString(url_res);
      return NavItem(UseString(title_res), url, false, [open_link, url] { open_link(url); },
                     TintedIcon(app::images::external_link, spec.colors.primary, 16.0F),
                     std::nullopt,
                     TintedIcon(app::images::github, spec.colors.on_surface, 18.0F));
    };

    right_pane = Column{
        Text(app::strings::settings_about_title, TextRole::Title),
        Row{
            Image(app::images::dbpp_logo)
                .Fit(ImageFit::Contain)
                .With(Frame{.width = 48.0F, .height = 48.0F, .min_width = 48.0F, .max_width = 48.0F,
                            .min_height = 48.0F, .max_height = 48.0F}),
            Column{
                Row{
                    Text("db++", TextRole::Title),
                    StatusBadge(UseString(app::strings::settings_about_version, DBPP_VERSION),
                                spec.colors.primary, spec.colors.primary_container,
                                WithAlpha(spec.colors.primary, 0.35F)),
                }.With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Center}),
                MutedText(UseString(app::strings::settings_about_summary)),
            }.With(Spacing{4.0F}, Grow(1.0F), CrossAlign{CrossAxisAlignment::Stretch}),
        }
            .With(Padding{EdgeInsets::Symmetric(14.0F, 12.0F)}, Spacing{12.0F},
                  CrossAlign{CrossAxisAlignment::Center}, CornerRadius{8.0F},
                  Background{VisualFill{spec.colors.surface}}, Border{semantic.border_subtle, 1.0F}),
        Divider(),
        Caption(UseString(app::strings::settings_about_links_heading)),
        link_row(app::strings::settings_about_link_dbpp, app::strings::settings_about_link_dbpp_sub),
        link_row(app::strings::settings_about_link_huxerui,
                 app::strings::settings_about_link_huxerui_sub),
        link_row(app::strings::settings_about_link_datatable,
                 app::strings::settings_about_link_datatable_sub),
        link_row(app::strings::settings_about_link_adbc, app::strings::settings_about_link_adbc_sub),
        MutedText(UseString(app::strings::settings_footer)),
    }.With(Frame{.max_width = 640.0F}, Spacing{10.0F}, CrossAlign{CrossAxisAlignment::Stretch});
  }

  return Row{
      std::move(left_nav),
      Divider(Axis::Vertical),
      ScrollView(Row{
                     std::move(right_pane).With(Grow(1.0F)),
                     Spacer(),
                 }.With(Padding{EdgeInsets::Symmetric(20.0F, 16.0F)}))
          .With(Grow(1.0F)),
  }.With(Grow(1.0F), CrossAlign{CrossAxisAlignment::Stretch});
}

// ---------------------------------------------------------------------------
// Application shell
// ---------------------------------------------------------------------------

[[huxerui::composable]] View AppContent() {
  auto app = UseState(AppModel{});
  auto workspace = UseState(WorkspaceModel{});
  auto toast = UseToast();
  auto dialogs = UseDialog();
  auto menu = UseMenu();
  const auto tab_scroll = UseScrollController();
  const auto all_tabs_anchor = UseViewHandle();
  const auto all_tabs_popup = UsePopup();
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
  const LocalizedCatalog loc{UseService<Resources>(), settings_model};
  const bool system_dark = UseSystemThemeBrightness() == ThemeBrightness::Dark;
  Lifecycle(
      [tray_runtime, system_dark] {
        if (tray_runtime) {
          tray_runtime->UpdateSystemDark(system_dark);
        }
        return [] {};
      },
      system_dark);
  const AppThemeConfig theme_config = settings_model->ThemeConfig();
  const LanguageMode language_mode = settings_model->language.Get();
  const std::int64_t default_page_size = settings_model->default_page_size.Get();
  Lifecycle(
      [app, default_page_size] {
        app.Update([default_page_size](AppModel& model) {
          model.default_page_size = default_page_size;
        });
        return [] {};
      },
      default_page_size);
  const ResolvedTheme resolved = ResolveTheme(theme_config, system_dark);
  const ThemeSpec& spec = resolved.spec;
  const SemanticColors& semantic = resolved.semantic;

  auto last_toast_id = UseState(std::make_shared<std::optional<LayerId>>());
  const NotifyFn notify = MakeNotify(toast, last_toast_id.Get());

  Lifecycle([app, workspace, tasks, notify] {
    auto active = std::make_shared<std::atomic<bool>>(true);
    tasks.Launch([active, app, workspace, tasks, notify]() -> Task<void> {
      while (active->load(std::memory_order_acquire)) {
        co_await Delay(std::chrono::seconds{15});
        if (!active->load(std::memory_order_acquire)) {
          break;
        }
        ScheduleSessionHealthCheck(app, workspace, tasks, notify);
      }
    });
    return [active, app, tasks] {
      active->store(false, std::memory_order_release);
      CloseAllSessions(app, tasks);
    };
  });

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
        const LanguageMode current_language = settings_model->language.Get();
        dialogs.Show([tray_runtime, close_dialog_open, current_theme, current_language,
                      system_dark](DialogContext dialog) {
          return WithDialogTheme(
              current_theme, current_language, system_dark,
              CloseConfirmDialog(dialog, tray_runtime, close_dialog_open));
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
                             default_file, notify, theme_config, language_mode,
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
        .language_mode = language_mode,
        .system_dark = system_dark,
        .notify = notify,
    };
    dialogs.Show([context](DialogContext dialog) {
      return WithDialogTheme(context.theme_config, context.language_mode, context.system_dark,
                             AdbcManagerDialog(dialog, context));
    });
  };

  const auto open_new = [dialogs, app, workspace, profiles, tasks, picker, http, profiles_path,
                         notify, default_file, theme_config, language_mode, system_dark,
                         open_manager] {
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
        .language_mode = language_mode,
        .system_dark = system_dark,
        .notify = notify,
        .open_manager = open_manager,
    };
    dialogs.Show([context](DialogContext dialog) {
      return WithDialogTheme(context.theme_config, context.language_mode, context.system_dark,
                             ConnectDialog(dialog, context));
    });
  };

  const auto open_new_query = [app, workspace, notify, loc] {
    const DbSession* active_session = app.Get().ActiveSession();
    if (active_session == nullptr) {
      notify(loc.Get(app::strings::toast_select_connection_first));
      return;
    }
    (void)OpenSqlTab(app, workspace, active_session->id);
  };

  const auto connect_profile = [app, workspace, tasks, notify](ConnectionProfile profile) {
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

  const auto request_close_tabs = [dialogs, app, workspace, theme_config, language_mode,
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
    dialogs.Show([app, workspace, ids = std::move(ids), draft_count, theme_config, language_mode,
                  system_dark](DialogContext dialog) {
      return WithDialogTheme(theme_config, language_mode, system_dark,
                             ConfirmCloseTabsDialog(dialog, app, workspace, ids, draft_count));
    });
  };

  const AppModel& model = app.Get();
  const WorkspaceModel& ws = workspace.Get();
  const WorkspaceTab* active_tab = ws.ActiveTab();
  const bool settings_tab_active =
      active_tab != nullptr && active_tab->kind == WorkspaceTabKind::Settings;

  // 标题栏右上角的齿轮负责打开/激活工作区中的「设置」标签页。
  View gear_button = Image(app::images::settings)
                         .Fit(ImageFit::Contain)
                         .Tint(settings_tab_active ? spec.colors.primary : spec.colors.on_surface)
                         .With(Padding{EdgeInsets::Symmetric(6.0F, 6.0F)},
                               CornerRadius{6.0F})
                         .OnClick([app, workspace] {
                           (void)OpenSettingsTab(app, workspace);
                         });
  if (settings_tab_active) {
    gear_button = std::move(gear_button)
                      .With(Background{VisualFill{spec.colors.primary_container}},
                            Border{WithAlpha(spec.colors.primary, 0.45F), 1.0F});
  }

  View title_bar = WindowTitleBar{
      TitleBarButton(UseString(app::strings::nav_new_connection),
                     [open_new] {
                       open_new();
                     },
                     false, &app::images::add_connection),
      TitleBarButton(UseString(app::strings::nav_new_query), open_new_query,
                     active_tab != nullptr && active_tab->kind == WorkspaceTabKind::Sql,
                     &app::images::sql_console),
      TitleBarButton(UseString(app::strings::nav_driver_manager),
                     [open_manager] {
                       open_manager(1, {});
                     },
                     false, &app::images::drivers),
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
      const std::string sublabel =
          (t.kind != WorkspaceTabKind::Settings && model.sessions.size() > 1)
              ? t.session_name
              : std::string{};
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

      const Color tab_accent =
          t.kind == WorkspaceTabKind::Settings
              ? spec.colors.primary
              : (t.kind == WorkspaceTabKind::Sql
                     ? semantic.accent_sql
                     : (t.is_view ? semantic.accent_view : semantic.accent_table));
      const bool is_active = tid == ws.active_tab_id;
      View tab_icon;
      if (t.kind == WorkspaceTabKind::Settings) {
        tab_icon = TintedIcon(app::images::settings,
                              is_active ? spec.colors.primary : spec.colors.on_surface_variant,
                              14.0F);
      } else if (t.kind == WorkspaceTabKind::Sql) {
        tab_icon = TintedIcon(app::images::sql_console, semantic.accent_sql, 14.0F);
      } else {
        bool is_redis_tab = false;
        for (const DbSession& open : model.sessions) {
          if (open.id == t.session_id) {
            is_redis_tab = open.dialect == Dialect::Redis;
            break;
          }
        }
        const ImageResource& obj_res =
            is_redis_tab ? app::images::database
                         : (t.is_view ? app::images::view : app::images::table);
        tab_icon = TintedIcon(obj_res, tab_accent, 14.0F);
      }
      const std::string tab_label =
          t.kind == WorkspaceTabKind::Settings ? UseString(app::strings::settings_title) : t.title;
      tab_items.Add(
          WorkspaceTabItem(
              WorkspaceTabItemOptions{
                  .label = tab_label,
                  .sublabel = sublabel,
                  .active = is_active,
                  .dirty = dirty,
                  .kind_accent = tab_accent,
                  .is_sql = t.kind == WorkspaceTabKind::Sql,
                  .leading_icon = std::move(tab_icon),
              },
              [app, workspace, tid] { ActivateWorkspaceTab(app, workspace, tid); },
              [request_close_tabs, tid] { request_close_tabs({tid}); })
              .On<ViewEvents::ContextMenuRequested>(
                  [menu, loc, request_close_tabs, tid, other_ids, all_ids](Point position) {
                    std::vector<MenuEntry> entries;
                    entries.emplace_back(
                        MenuItem(loc.Get(app::strings::tab_ctx_close),
                                 [request_close_tabs, tid] { request_close_tabs({tid}); }));
                    if (!other_ids.empty()) {
                      entries.emplace_back(
                          MenuItem(loc.Get(app::strings::tab_ctx_close_others),
                                   [request_close_tabs, other_ids] {
                                     request_close_tabs(other_ids);
                                   }));
                    }
                    entries.emplace_back(MenuSection{});
                    entries.emplace_back(
                        MenuItem(loc.Get(app::strings::tab_ctx_close_all),
                                 [request_close_tabs, all_ids] {
                                   request_close_tabs(all_ids);
                                 }));
                    menu.ShowAt(position, std::move(entries));
                  })
              .Key("ws-tab-" + std::to_string(tid)));
    }

    View all_tabs_button =
        TintedIcon(app::images::chevron_down, spec.colors.on_surface_variant, 14.0F)
            .With(Padding{EdgeInsets::Symmetric(7.0F, 6.0F)}, CornerRadius{6.0F},
                  Background{VisualFill{spec.colors.surface_container}},
                  Border{semantic.border_subtle, 1.0F}, all_tabs_anchor)
            .OnClick([all_tabs_popup, all_tabs_anchor, app, workspace, request_close_tabs,
                      theme_config, language_mode, system_dark] {
              all_tabs_popup.Show(
                  all_tabs_anchor,
                  [app, workspace, request_close_tabs, theme_config, language_mode,
                   system_dark](PopupContext popup) {
                    return WithDialogTheme(
                        theme_config, language_mode, system_dark,
                        AllTabsPopup(popup, app, workspace, request_close_tabs));
                  },
                  PopupOptions{
                      .placement = {.side = AnchorSide::Below, .alignment = AnchorAlignment::End},
                      .gap = 4.0F,
                      .viewport_margin = 8.0F,
                      .offset = {},
                      .dismiss_on_outside_press = true,
                      .dismiss_on_cancel = true,
                      .trap_focus = false,
                      .retain_anchor_focus = false,
                      .on_dismiss_request = {},
                  });
            });

    View tab_strip =
        Row{
            ScrollView(Row{std::move(tab_items)}
                           .With(Spacing{4.0F}, CrossAlign{CrossAxisAlignment::Center}))
                .ScrollAxis(Axis::Horizontal)
                .Controller(tab_scroll)
                .On<ViewEvents::ScrollInput>([tab_scroll](const ScrollInputEvent& event) -> bool {
                  if (std::abs(event.delta_x) < 0.01F && std::abs(event.delta_y) > 0.01F) {
                    return tab_scroll.ScrollBy(event.delta_y);
                  }
                  return false;
                })
                .With(Grow(1.0F), Frame{.height = 34.0F}),
            TitleBarButton("＋", open_new_query),
            std::move(all_tabs_button),
        }
            .With(Spacing{6.0F}, CrossAlign{CrossAxisAlignment::Center});

    View active_body;
    if (active_tab == nullptr) {
      active_body = TableEmptyState(app::strings::empty_select_object);
    } else if (active_tab->kind == WorkspaceTabKind::Settings) {
      const auto apply_global_page_size = [app, workspace, settings_model, tasks, notify,
                                           loc](std::int64_t next_size) {
        const std::int64_t clamped = std::clamp(next_size, kMinPageSize, kMaxPageSize);
        app.Update([clamped](AppModel& m) { m.default_page_size = clamped; });
        struct PendingTableReload {
          std::uint64_t tab_id = 0;
          std::optional<DataTableSort> sort;
          std::string filter;
        };
        std::vector<PendingTableReload> table_reloads;
        std::vector<std::uint64_t> sql_reloads;
        workspace.Update([clamped, &table_reloads, &sql_reloads](WorkspaceModel& w) {
          for (WorkspaceTab& tab : w.tabs) {
            if (tab.kind == WorkspaceTabKind::Table) {
              if (tab.table.pending_inserts.empty()) {
                table_reloads.push_back(PendingTableReload{
                    .tab_id = tab.id,
                    .sort = tab.table.sort,
                    .filter = tab.table.filter,
                });
              }
            } else if (tab.kind == WorkspaceTabKind::Sql) {
              tab.sql.page_size = clamped;
              if (!tab.sql.running && tab.sql.paginated && !tab.sql.last_sql.empty()) {
                sql_reloads.push_back(tab.id);
              }
            }
          }
        });
        for (const PendingTableReload& item : table_reloads) {
          RequestLoadTable(app, workspace, tasks, item.tab_id, 0, item.sort, item.filter, false,
                           notify, clamped);
        }
        for (const std::uint64_t sql_tab_id : sql_reloads) {
          ExecuteSqlTabQuery(app, workspace, settings_model, sql_tab_id, tasks, notify, loc, 0,
                             false, clamped);
        }
      };
      active_body = SettingsPage(notify, apply_global_page_size).Key("tab-body-settings");
    } else if (active_tab->kind == WorkspaceTabKind::Table) {
      active_body = TableBrowserPage(app, workspace, active_tab->id, tasks, notify,
                                     theme_config, language_mode, system_dark)
                        .Key("tab-body-" + std::to_string(active_tab->id));
    } else {
      active_body = SqlPage(app, workspace, profiles, active_tab->id, tasks, notify,
                            theme_config, language_mode, system_dark)
                        .Key("tab-body-" + std::to_string(active_tab->id));
    }

    main_content = Column{
        std::move(tab_strip),
        Divider(),
        std::move(active_body),
    }.With(Padding{EdgeInsets::Symmetric(16.0F, 10.0F)}, Spacing{8.0F},
           CrossAlign{CrossAxisAlignment::Stretch});
  }

  View page_content = Row{
      Sidebar(app, workspace, profiles, profiles_path, tasks, notify, theme_config,
              language_mode, system_dark, connect_profile),
      LoadingOverlay(std::move(main_content), !model.status.empty(), model.status)
          .With(Grow(1.0F)),
  }.With(Grow(1.0F), CrossAlign{CrossAxisAlignment::Stretch});

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

[[huxerui::composable]] View AppShell() {
  const auto settings = UseService<SettingsModel>();
  const auto resources = UseService<Resources>();
  const LanguageMode mode = settings ? settings->language.Get() : LanguageMode::System;
  const Locale platform_locale =
      resources ? resources->Configuration().locale : Locale::Default();
  const Locale effective = EffectiveLocale(mode, platform_locale);
  SetActiveUiLanguage(ResolveUiLanguage(mode, effective.LanguageTag()));
  return ProvideEnvironment(effective, AppContent());
}

} // namespace dbpp

huxerui::View AppRoot() {
  return dbpp::AppShell();
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
