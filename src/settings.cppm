module;

#include <huxerui/state.h>

export module dbpp.settings;

import std;

export namespace dbpp {

inline constexpr std::string_view kSettingTrayEnabled = "tray.enabled";
inline constexpr std::string_view kSettingLightweight = "tray.lightweight";
inline constexpr std::string_view kSettingThemeMode = "theme.mode";
inline constexpr std::string_view kSettingThemePreset = "theme.preset";
inline constexpr std::string_view kSettingUiLanguage = "ui.language";

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

enum class LanguageMode {
  System = 0,
  Zh = 1,
  En = 2,
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
[[nodiscard]] std::string_view LanguageModeKey(LanguageMode mode);
[[nodiscard]] LanguageMode LanguageModeFromKey(std::string_view key);

/// Persisted tray, theme, and language preferences. Stored as key/value rows in SQLite
/// (`dbpp.sqlite3` `settings` table, automatically migrating legacy `settings.tsv`).
struct TraySettings {
  bool tray_enabled = true;
  bool lightweight = false;
  ThemeMode theme_mode = ThemeMode::System;
  ThemePreset theme_preset = ThemePreset::GeminiSpark;
  LanguageMode language = LanguageMode::System;

  bool operator==(const TraySettings&) const = default;
};

[[nodiscard]] TraySettings LoadTraySettings(const std::string& path);
bool SaveTraySettings(const std::string& path, const TraySettings& settings);

namespace detail {
[[nodiscard]] bool IsLegacyTsvPath(std::string_view path) noexcept;
void RestrictConfigFilePermissions(std::string_view db_path) noexcept;
[[nodiscard]] std::vector<std::pair<std::string, std::string>>
LoadLegacySettingsTsvPairs(const std::string& path);
[[nodiscard]] bool EnsureConfigSqliteSync(const std::string& db_path);
} // namespace detail

/// Application-scoped settings holder shared by the tray runtime, the window
/// close handler, the theme shell, and the settings page. States are constructed
/// outside any composition; updates go through the Set* methods, which also persist.
struct SettingsModel {
  huxerui::State<bool> tray_enabled{true};
  huxerui::State<bool> lightweight{false};
  huxerui::State<ThemeMode> theme_mode{ThemeMode::System};
  huxerui::State<ThemePreset> theme_preset{ThemePreset::GeminiSpark};
  huxerui::State<LanguageMode> language{LanguageMode::System};
  std::string path;
  std::function<void(std::string, std::string)> persist_setting;

  void Load();
  bool SetTrayEnabled(bool enabled);
  bool SetLightweight(bool value);
  bool SetThemeMode(ThemeMode mode);
  bool SetThemePreset(ThemePreset preset);
  bool SetLanguage(LanguageMode mode);

  [[nodiscard]] TraySettings Snapshot() const;
  [[nodiscard]] AppThemeConfig ThemeConfig() const;
};

} // namespace dbpp
