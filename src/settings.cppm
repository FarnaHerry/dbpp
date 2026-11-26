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
inline constexpr std::string_view kSettingMcpEnabled = "mcp.enabled";
inline constexpr std::string_view kSettingMcpHost = "mcp.host";
inline constexpr std::string_view kSettingMcpPort = "mcp.port";
inline constexpr std::string_view kSettingMcpReadOnly = "mcp.read_only";
inline constexpr std::uint16_t kDefaultMcpPort = 18765;

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

/// Persisted tray, theme, language, and MCP server preferences. Stored as key/value rows in SQLite
/// (`dbpp.sqlite3` `settings` table, automatically migrating legacy `settings.tsv`).
struct TraySettings {
  bool tray_enabled = true;
  bool lightweight = false;
  ThemeMode theme_mode = ThemeMode::System;
  ThemePreset theme_preset = ThemePreset::GeminiSpark;
  LanguageMode language = LanguageMode::System;
  bool mcp_enabled = false;
  std::string mcp_host = "127.0.0.1";
  std::uint16_t mcp_port = kDefaultMcpPort;
  bool mcp_read_only = true;

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
  huxerui::State<bool> mcp_enabled{false};
  huxerui::State<std::string> mcp_host{"127.0.0.1"};
  huxerui::State<std::uint16_t> mcp_port{kDefaultMcpPort};
  huxerui::State<bool> mcp_read_only{true};
  std::string path;
  std::function<void(std::string, std::string)> persist_setting;

  void Load();
  bool SetTrayEnabled(bool enabled);
  bool SetLightweight(bool value);
  bool SetThemeMode(ThemeMode mode);
  bool SetThemePreset(ThemePreset preset);
  bool SetLanguage(LanguageMode mode);
  bool SetMcpEnabled(bool enabled);
  bool SetMcpHost(std::string host);
  bool SetMcpPort(std::uint16_t port);
  bool SetMcpReadOnly(bool read_only);

  [[nodiscard]] TraySettings Snapshot() const;
  [[nodiscard]] AppThemeConfig ThemeConfig() const;
};

[[nodiscard]] std::uint16_t ParseMcpPort(std::string_view text,
                                         std::uint16_t fallback = kDefaultMcpPort) noexcept;
[[nodiscard]] std::string NormalizeMcpHost(std::string_view host);

} // namespace dbpp
