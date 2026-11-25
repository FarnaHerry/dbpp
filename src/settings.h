#pragma once

#include <huxerui/huxerui.h>

#include <string>
#include <string_view>

namespace dbpp {

enum class ThemeMode {
  System = 0,
  Dark = 1,
  Light = 2,
};

enum class ThemePreset {
  GeminiSpark = 0,
  Emerald = 1,
  WarmAmber = 2,
  VioletDusk = 3,
  Monochrome = 4,
};

struct AppThemeConfig {
  ThemeMode mode = ThemeMode::System;
  ThemePreset preset = ThemePreset::GeminiSpark;

  bool operator==(const AppThemeConfig&) const = default;
};

[[nodiscard]] std::string_view ThemeModeKey(ThemeMode mode);
[[nodiscard]] ThemeMode ThemeModeFromKey(std::string_view key);
[[nodiscard]] std::string_view ThemePresetKey(ThemePreset preset);
[[nodiscard]] ThemePreset ThemePresetFromKey(std::string_view key);

/// Persisted tray and theme preferences. Stored as key/value TSV in the
/// application data directory (`settings.tsv`), same format as `connections.tsv`.
struct TraySettings {
  bool tray_enabled = true;
  bool lightweight = false;
  ThemeMode theme_mode = ThemeMode::System;
  ThemePreset theme_preset = ThemePreset::GeminiSpark;

  bool operator==(const TraySettings&) const = default;
};

[[nodiscard]] TraySettings LoadTraySettings(const std::string& path);
bool SaveTraySettings(const std::string& path, const TraySettings& settings);

/// Application-scoped settings holder shared by the tray runtime, the window
/// close handler, the theme shell, and the settings page. States are constructed
/// outside any composition; updates go through the Set* methods, which also persist.
struct SettingsModel {
  huxerui::State<bool> tray_enabled{true};
  huxerui::State<bool> lightweight{false};
  huxerui::State<ThemeMode> theme_mode{ThemeMode::System};
  huxerui::State<ThemePreset> theme_preset{ThemePreset::GeminiSpark};
  std::string path;

  void Load();
  bool SetTrayEnabled(bool enabled);
  bool SetLightweight(bool value);
  bool SetThemeMode(ThemeMode mode);
  bool SetThemePreset(ThemePreset preset);

  [[nodiscard]] TraySettings Snapshot() const;
  [[nodiscard]] AppThemeConfig ThemeConfig() const;
};

} // namespace dbpp
