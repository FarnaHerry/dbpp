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


#include "ui/ui_common.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <ranges>

namespace dbpp {

using namespace huxerui;
using namespace dbpp::ui;

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
  if (!column.extension_name.empty()) {
    type += " [" + column.extension_name + "]";
  }
  if (column.primary_key_order > 0) {
    type += " · PK";
  }
  if (!column.nullable) {
    type += " · NOT NULL";
  }
  if (column.unique && column.primary_key_order <= 0) {
    type += " · UNIQUE";
  }
  if (column.autoincrement) {
    type += " · AUTO";
  }
  if (column.generated) {
    type += " · GEN";
  }
  if (!column.default_expr.empty()) {
    type += " · DEFAULT " + column.default_expr;
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

std::int64_t SafePageOffset(std::int64_t page, std::int64_t page_size) {
  if (page <= 0 || page_size <= 0) {
    return 0;
  }
  if (page_size > std::numeric_limits<std::int64_t>::max() / page) {
    return std::numeric_limits<std::int64_t>::max();
  }
  return page * page_size;
}

std::string FormatGlobalRowNumber(std::int64_t base_offset, std::size_t row_index) {
  const std::uint64_t base = base_offset > 0 ? static_cast<std::uint64_t>(base_offset) : 0ULL;
  const std::uint64_t idx = static_cast<std::uint64_t>(row_index);
  if (idx >= std::numeric_limits<std::uint64_t>::max() - base) {
    return std::to_string(std::numeric_limits<std::uint64_t>::max());
  }
  return std::to_string(base + idx + 1ULL);
}

DataTableColumn MakeRowNumberColumn(std::int64_t base_offset, std::size_t row_count,
                                    const std::unordered_map<std::string, float>& column_widths,
                                    Color muted_color, Color draft_color) {
  DataTableColumn column;
  column.id = std::string(kRowNumberColumnId);
  column.header = "#";
  column.sortable = false;

  const std::string max_label =
      FormatGlobalRowNumber(base_offset, row_count > 0 ? row_count - 1 : 0);
  const float digits = static_cast<float>(std::max<std::size_t>(2, max_label.size()));
  float initial_width = std::clamp(26.0F + digits * 8.0F, 44.0F, 96.0F);
  if (const auto width_it = column_widths.find(column.id); width_it != column_widths.end()) {
    initial_width = std::clamp(width_it->second, 40.0F, 120.0F);
  }
  column.width = DataTableColumnWidth{
      .initial = initial_width,
      .min = 40.0F,
      .max = 120.0F,
  };
  column.render_cell = [base_offset, muted_color,
                        draft_color](const DataTableCellContext& cell) -> DataTableCellContent {
    if (IsDraftRowId(cell.id.row_id)) {
      return DataTableCellContent{
          .view = Text("+")
                      .Style(TextStyle{Font::Monospace(12.0F).WithWeight(FontWeight::SemiBold),
                                       draft_color})
                      .LineLimit(1),
          .label = "+",
      };
    }
    const std::size_t row_idx = RowIndexFromId(cell.id.row_id);
    if (row_idx == static_cast<std::size_t>(-1)) {
      return DataTableCellContent{.view = View{}, .label = {}};
    }
    std::string num_text = FormatGlobalRowNumber(base_offset, row_idx);
    return DataTableCellContent{
        .view = Text(num_text)
                    .Style(TextStyle{Font::Monospace(12.0F), muted_color})
                    .LineLimit(1)
                    .Overflow(TextOverflow::Ellipsis),
        .label = std::move(num_text),
    };
  };
  return column;
}

std::vector<DataTableColumn> BuildReadOnlyColumns(
    const std::vector<std::string>& names, const std::shared_ptr<const GridSnapshot>& snapshot,
    const std::unordered_map<std::string, float>& column_widths, Color text_color,
    Color null_color, std::int64_t base_offset, Color muted_color) {
  std::vector<DataTableColumn> columns;
  columns.reserve(names.size() + 1);
  columns.push_back(MakeRowNumberColumn(base_offset, snapshot ? snapshot->rows.size() : 0,
                                        column_widths, muted_color, muted_color));
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
                Frame{.width = 80.0F, .height = 80.0F, .min_width = 80.0F, .max_width = 80.0F,
                      .min_height = 80.0F, .max_height = 80.0F}),
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
  return Row{
      Text(text)
          .Style(TextStyle{Font::Monospace(font_size::kCaption).WithWeight(FontWeight::Medium),
                           spec.colors.primary})
          .LineLimit(1),
  }.With(Frame{.height = 24.0F, .min_height = 24.0F, .max_height = 24.0F},
         Padding{EdgeInsets::Symmetric(4.0F, 0.0F)}, CrossAlign{CrossAxisAlignment::Center},
         LayoutPriority(1.0F));
}

[[huxerui::composable]] View AuditSqlBadge(std::string executed_sql, NotifyFn notify) {
  const ThemeSpec& spec = UseTheme();
  const LocalizedCatalog loc{UseService<Resources>(), UseService<SettingsModel>()};
  const auto clipboard = UseApplication().Clipboard();
  if (executed_sql.empty()) {
    return View{};
  }
  const std::string preview = CompactSqlPreview(executed_sql);
  const auto copy_sql = [clipboard, notify, loc, full_sql = executed_sql] {
    if (clipboard && clipboard->SupportsSynchronousText() && clipboard->WriteText(full_sql)) {
      if (notify) {
        notify(loc.Fmt(app::strings::toast_copied,
                       loc.Get(app::strings::audit_copy_sql_label)));
      }
    } else if (notify) {
      notify(loc.Get(app::strings::toast_clipboard_unsupported));
    }
  };
  View sql_text =
      Text(preview)
          .Style(TextStyle{Font::Monospace(font_size::kCaption), spec.colors.on_surface_variant})
          .LineLimit(1)
          .Overflow(TextOverflow::Ellipsis)
          .With(Tooltip(StringVariant(executed_sql)));
  View copy_btn =
      Row{TintedIcon(app::images::copy, spec.colors.on_surface_variant, 15.0F)}
          .With(Frame{.width = 24.0F, .height = 24.0F, .min_width = 24.0F, .max_width = 24.0F,
                      .min_height = 24.0F, .max_height = 24.0F},
                MainAlign{MainAxisAlignment::Center}, CrossAlign{CrossAxisAlignment::Center},
                CornerRadius{4.0F}, LayoutPriority(1.0F),
                Tooltip(StringVariant(app::strings::audit_copy_sql_label)))
          .OnClick(copy_sql);
  return Row{std::move(sql_text), std::move(copy_btn)}.With(
      Frame{.height = 24.0F, .min_height = 24.0F, .max_height = 24.0F},
      Padding{EdgeInsets::Symmetric(2.0F, 0.0F)}, Spacing{4.0F},
      CrossAlign{CrossAxisAlignment::Center});
}

[[huxerui::composable]] View PageNumberInput(std::int64_t current_page_one_based,
                                             std::optional<std::int64_t> max_page_one_based,
                                             bool enabled,
                                             std::function<void(std::int64_t)> on_go_zero_based,
                                             NotifyFn notify) {
  const LocalizedCatalog loc{UseService<Resources>(), UseService<SettingsModel>()};
  const std::int64_t safe_current = std::max<std::int64_t>(1, current_page_one_based);
  auto value = UseState(TextEditingValue::FromText(std::to_string(safe_current)));

  const std::int64_t ref_page =
      max_page_one_based.has_value() ? std::max(safe_current, *max_page_one_based) : safe_current;
  const float digits = static_cast<float>(std::max<std::size_t>(2, std::to_string(ref_page).size()));
  const float input_width = std::clamp(24.0F + digits * 8.0F, 44.0F, 76.0F);

  const auto submit = [value, safe_current, max_page_one_based, enabled, on_go_zero_based, notify,
                       loc] {
    if (!enabled) {
      value = TextEditingValue::FromText(std::to_string(safe_current));
      return;
    }
    const std::string trimmed = TrimmedSql(value.Get().text);
    std::int64_t parsed = 0;
    const char* begin = trimmed.data();
    const char* end_ptr = trimmed.data() + trimmed.size();
    const auto [ptr, ec] = std::from_chars(begin, end_ptr, parsed);
    if (trimmed.empty() || ec != std::errc{} || ptr != end_ptr || parsed < 1) {
      value = TextEditingValue::FromText(std::to_string(safe_current));
      if (notify) {
        notify(loc.Get(app::strings::table_page_invalid));
      }
      return;
    }
    if (max_page_one_based.has_value() && *max_page_one_based > 0) {
      parsed = std::min(parsed, *max_page_one_based);
    }
    const std::int64_t target_zero = parsed - 1;
    if (parsed == safe_current) {
      value = TextEditingValue::FromText(std::to_string(safe_current));
      return;
    }
    if (on_go_zero_based) {
      on_go_zero_based(target_zero);
    }
  };

  TextFieldStyle style = UseEnvironment<TextFieldStyle>();
  style.show_label = false;
  style.outlined.minimum_height = 24.0F;
  style.outlined.corner_radii = CornerRadii{5.0F};
  style.text_style.font = Font::Monospace(font_size::kCaption).WithWeight(FontWeight::Medium);
  style.placeholder_style.font = Font::Monospace(font_size::kCaption);
  style.padding = EdgeInsets::Symmetric(4.0F, 0.0F);
  ThemeDefinition definition = OverrideTextFieldStyle(std::move(style));

  View field = TextField(value)
                   .Variant(TextFieldVariant::Outlined)
                   .LineLimits(TextFieldLineLimits::SingleLine())
                   .Align(TextAlign::Center)
                   .VerticalAlign(TextVerticalAlign::Center)
                   .InputConfiguration({.action = TextInputAction::Done})
                   .OnChanged([value](const TextEditingValue& next) { value = next; })
                   .OnSubmitted(submit)
                   .On<ViewEvents::FocusChanged>([value, safe_current](bool focused) {
                     if (!focused) {
                       value = TextEditingValue::FromText(std::to_string(safe_current));
                     }
                   })
                   .With(Enabled{enabled},
                         Frame{.width = input_width, .height = 24.0F, .min_width = input_width,
                               .max_width = input_width, .min_height = 24.0F, .max_height = 24.0F},
                         Tooltip(StringVariant(app::strings::table_page_input_tooltip)));
  return Theme(std::move(definition), std::move(field));
}

[[huxerui::composable]] View CompactPagerControls(std::int64_t current_page,
                                                  std::optional<std::int64_t> total_pages,
                                                  bool has_next, bool enabled,
                                                  std::uint64_t sync_version,
                                                  std::function<void(std::int64_t)> on_go_page,
                                                  NotifyFn notify) {
  const std::int64_t safe_page = std::max<std::int64_t>(0, current_page);
  const bool can_prev = enabled && safe_page > 0;
  const bool can_next = enabled && has_next;
  const bool can_last =
      enabled && total_pages.has_value() && *total_pages > 0 && (safe_page + 1 < *total_pages);
  return Row{
      CompactPagerButton(
          "<<",
          [on_go_page] {
            if (on_go_page) {
              on_go_page(0);
            }
          },
          can_prev, 24.0F, StringVariant(app::strings::table_first_page)),
      CompactPagerButton(
          "<",
          [on_go_page, safe_page] {
            if (on_go_page && safe_page > 0) {
              on_go_page(safe_page - 1);
            }
          },
          can_prev, 24.0F, StringVariant(app::strings::table_prev_page)),
      PageNumberInput(safe_page + 1, total_pages, enabled, on_go_page, std::move(notify))
          .Key("pager-in-" + std::to_string(safe_page) + "-" + std::to_string(sync_version)),
      CompactPagerButton(
          ">",
          [on_go_page, safe_page] {
            if (on_go_page) {
              on_go_page(safe_page + 1);
            }
          },
          can_next, 24.0F, StringVariant(app::strings::table_next_page)),
      CompactPagerButton(
          ">>",
          [on_go_page, total_pages] {
            if (on_go_page && total_pages.has_value() && *total_pages > 0) {
              on_go_page(*total_pages - 1);
            }
          },
          can_last, 24.0F, StringVariant(app::strings::table_last_page)),
  }.With(Spacing{3.0F}, CrossAlign{CrossAxisAlignment::Center}, LayoutPriority(1.0F));
}

void ExportSqlWithPicker(std::shared_ptr<AdbcSession> adbc, std::string sql,
                         ExportFormat format, std::string suggested_name,
                         std::shared_ptr<FilePicker> picker, TaskScope tasks,
                         NotifyFn notify, LocalizedCatalog loc) {
  if (!adbc || adbc->IsClosed()) {
    if (notify) {
      notify(loc.Get(app::strings::toast_connection_closed));
    }
    return;
  }
  if (!picker || !picker->CanSaveFiles()) {
    if (notify) {
      notify(loc.Fmt(app::strings::sql_export_failed, "FilePicker unavailable"));
    }
    return;
  }

  tasks.Launch([adbc = std::move(adbc), sql = std::move(sql), format,
                suggested_name = std::move(suggested_name), picker = std::move(picker),
                notify = std::move(notify), loc]() mutable -> Task<void> {
    const std::string temp_path = MakeTempExportPath(format);

    const ExportStreamResult res = co_await adbc->sequence.Run(
        [adbc, sql = std::move(sql), format, temp_path](std::stop_token) mutable {
          return ExportQueryToFilePathBlocking(*adbc, std::move(sql), format, temp_path, 0);
        });

    auto cleanup = [&temp_path] { RemoveFileQuietly(temp_path); };

    if (!res.error.empty()) {
      cleanup();
      if (notify) {
        notify(loc.Fmt(app::strings::sql_export_failed, res.error));
      }
      co_return;
    }

    SaveFileOptions options{
        .suggested_name = suggested_name,
        .filter = FilePickerFilter{
            .name = (format == ExportFormat::Csv) ? "CSV" : "JSONL",
            .extensions = {(format == ExportFormat::Csv) ? "csv" : "jsonl"},
            .content_types = {(format == ExportFormat::Csv) ? "text/csv"
                                                            : "application/x-ndjson"},
        },
    };
    const bool saved = co_await picker->SaveFileAsync(File(temp_path), options);
    cleanup();
    if (saved && notify) {
      notify(loc.Fmt(app::strings::sql_export_done, std::to_string(res.rows_written)));
    }
  });
}

[[huxerui::composable]] View BottomStatusBar(Views items) {
  const ThemeSpec& spec = UseTheme();
  return Row{std::move(items)}.With(
      CrossAlign{CrossAxisAlignment::Center}, Spacing{6.0F},
      Frame{.height = 32.0F, .min_height = 32.0F, .max_height = 32.0F},
      Padding{EdgeInsets::Symmetric(8.0F, 3.0F)}, CornerRadius{0.0F},
      Background{VisualFill{spec.colors.surface_container_low}});
}

} // namespace dbpp
