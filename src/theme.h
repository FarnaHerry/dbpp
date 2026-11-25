#pragma once

#include "settings.h"

#include <huxerui/huxerui.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace dbpp {
enum class DatabaseBrand;
} // namespace dbpp

namespace dbpp::ui {

namespace font_size {
inline constexpr float kCaption = 11.0F;
inline constexpr float kChip = 12.0F;
inline constexpr float kBody = 14.0F;
inline constexpr float kTitle = 20.0F;
} // namespace font_size

/// Semantic colors and brand gradients injected into the UI environment via
/// ThemeDefinition::Set(SemanticColors) so controls and pages never hardcode RGBs.
struct SemanticColors {
  huxerui::Color border_subtle;
  huxerui::Color border_strong;
  huxerui::Color hover_fill;
  huxerui::Color press_fill;

  huxerui::Color success;
  huxerui::Color success_container;
  huxerui::Color warning;
  huxerui::Color warning_container;
  huxerui::Color danger;
  huxerui::Color danger_solid;
  huxerui::Color on_danger;
  huxerui::Color danger_container;
  huxerui::Color on_primary_button;

  huxerui::Color accent_table;
  huxerui::Color accent_view;
  huxerui::Color accent_sql;
  huxerui::Color accent_external;

  huxerui::Color category_local;
  huxerui::Color category_relational;
  huxerui::Color category_analytics;
  huxerui::Color category_custom;

  huxerui::Color null_text;
  huxerui::Color draft_accent;

  huxerui::VisualFill accent_gradient{huxerui::Color::Transparent()};
  huxerui::VisualFill primary_button_fill{huxerui::Color::Transparent()};
  huxerui::VisualFill hero_glow{huxerui::Color::Transparent()};
  bool uses_gradient_chrome = true;

  static SemanticColors Default();
  bool operator==(const SemanticColors&) const = default;
};

struct ThemePresetMeta {
  ThemePreset preset = ThemePreset::GeminiSpark;
  std::string name;
  std::string subtitle;
  huxerui::Color swatch_a;
  huxerui::Color swatch_b;
  huxerui::Color swatch_c;
  huxerui::VisualFill swatch_bar{huxerui::Color::Transparent()};
};

struct ResolvedTheme {
  ThemePreset preset = ThemePreset::GeminiSpark;
  bool dark = false;
  huxerui::ThemeSpec spec;
  SemanticColors semantic;
};

enum class ButtonTone {
  Default,
  Primary,
  Danger,
  DangerSolid,
  Subtle,
};

[[nodiscard]] huxerui::Color WithAlpha(huxerui::Color color, float alpha);
[[nodiscard]] const std::vector<ThemePresetMeta>& AvailableThemePresets();
[[nodiscard]] bool ResolveIsDark(ThemeMode mode, bool system_dark);
[[nodiscard]] huxerui::ThemeSpec BuildThemeSpec(ThemePreset preset, bool dark);
[[nodiscard]] SemanticColors BuildSemanticColors(ThemePreset preset, bool dark);
[[nodiscard]] ResolvedTheme ResolveTheme(const AppThemeConfig& config, bool system_dark);

[[nodiscard]] huxerui::Indication TintedIndication(huxerui::Color hover_fill,
                                                     huxerui::Color press_fill,
                                                     huxerui::Color focus_border);
[[nodiscard]] huxerui::ThemeDefinition AppThemeDefinition(const ResolvedTheme& resolved);

huxerui::View ActionButton(huxerui::View action, ButtonTone tone = ButtonTone::Default);
huxerui::View AccentDot(huxerui::Color color, float size = 7.0F);
huxerui::View DatabaseIcon(dbpp::DatabaseBrand brand, float size = 16.0F);
huxerui::View DatabaseBadgeIcon(dbpp::DatabaseBrand brand, float icon_size = 20.0F,
                                float tile_size = 34.0F);
huxerui::View StatusBadge(std::string text, huxerui::Color foreground,
                          huxerui::Color background,
                          huxerui::Color border = huxerui::Color::Transparent());
huxerui::View TitleBarButton(std::string label, std::function<void()> on_click,
                             bool active = false);
huxerui::View NavItem(std::string label, std::string detail, bool selected,
                      std::function<void()> on_click, huxerui::View trailing = {},
                      std::optional<huxerui::Color> accent_dot = std::nullopt,
                      huxerui::View leading = {});

struct TreeRowOptions {
  std::string prefix;
  std::string label;
  std::string trailing_text;
  int indent_level = 0;
  bool selected = false;
  bool emphasize = false;
  bool active_hint = false;
  bool muted = false;
  std::optional<huxerui::Color> prefix_color{};
  std::optional<huxerui::Color> status_dot{};
  huxerui::View leading_icon{};
};

huxerui::View TreeRow(TreeRowOptions options, std::function<void()> on_click = {});

struct WorkspaceTabItemOptions {
  std::string label;
  std::string sublabel;
  bool active = false;
  bool dirty = false;
  std::optional<huxerui::Color> kind_accent{};
  bool is_sql = false;
  huxerui::View leading_icon{};
};

huxerui::View WorkspaceTabItem(WorkspaceTabItemOptions options,
                               std::function<void()> on_select,
                               std::function<void()> on_close);
huxerui::View PageTitle(std::string title, huxerui::View actions);
huxerui::View Caption(std::string text);
huxerui::View MutedText(std::string text);
huxerui::View ErrorText(std::string text);
huxerui::View MonospaceField(huxerui::View field);
huxerui::View DialogCard(huxerui::View content);

} // namespace dbpp::ui
