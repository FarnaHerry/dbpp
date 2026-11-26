#pragma once

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

  huxerui::Color status_connected;
  huxerui::Color status_failed;

  huxerui::Color accent_database;
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
[[nodiscard]] huxerui::ThemeDefinition OverrideButtonStyle(huxerui::ButtonStyle style);
[[nodiscard]] huxerui::ThemeDefinition OverrideTextFieldStyle(huxerui::TextFieldStyle style);

} // namespace dbpp::ui
