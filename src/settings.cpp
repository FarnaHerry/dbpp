#include "settings.h"

#include <filesystem>
#include <fstream>
#include <string_view>

namespace dbpp {

namespace {

constexpr std::string_view kKeyTrayEnabled = "tray.enabled";
constexpr std::string_view kKeyLightweight = "tray.lightweight";
constexpr std::string_view kKeyThemeMode = "theme.mode";
constexpr std::string_view kKeyThemePreset = "theme.preset";

std::string_view FlagText(bool value) {
  return value ? "true" : "false";
}

} // namespace

std::string_view ThemeModeKey(ThemeMode mode) {
  switch (mode) {
    case ThemeMode::Dark: return "dark";
    case ThemeMode::Light: return "light";
    case ThemeMode::System:
    default: return "system";
  }
}

ThemeMode ThemeModeFromKey(std::string_view key) {
  if (key == "dark" || key == "1") {
    return ThemeMode::Dark;
  }
  if (key == "light" || key == "2") {
    return ThemeMode::Light;
  }
  return ThemeMode::System;
}

std::string_view ThemePresetKey(ThemePreset preset) {
  switch (preset) {
    case ThemePreset::Emerald: return "emerald";
    case ThemePreset::WarmAmber: return "warm_amber";
    case ThemePreset::VioletDusk: return "violet_dusk";
    case ThemePreset::Monochrome: return "monochrome";
    case ThemePreset::GeminiSpark:
    default: return "gemini";
  }
}

ThemePreset ThemePresetFromKey(std::string_view key) {
  if (key == "emerald") {
    return ThemePreset::Emerald;
  }
  if (key == "warm_amber") {
    return ThemePreset::WarmAmber;
  }
  if (key == "violet_dusk") {
    return ThemePreset::VioletDusk;
  }
  if (key == "monochrome") {
    return ThemePreset::Monochrome;
  }
  return ThemePreset::GeminiSpark;
}

TraySettings LoadTraySettings(const std::string& path) {
  TraySettings settings;
  std::ifstream stream(path);
  if (!stream) {
    return settings;
  }
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    const auto tab = line.find('\t');
    if (tab == std::string::npos) {
      continue;
    }
    const std::string key = line.substr(0, tab);
    const std::string value = line.substr(tab + 1);
    if (key == kKeyTrayEnabled) {
      settings.tray_enabled = value == "true";
    } else if (key == kKeyLightweight) {
      settings.lightweight = value == "true";
    } else if (key == kKeyThemeMode) {
      settings.theme_mode = ThemeModeFromKey(value);
    } else if (key == kKeyThemePreset) {
      settings.theme_preset = ThemePresetFromKey(value);
    }
  }
  return settings;
}

bool SaveTraySettings(const std::string& path, const TraySettings& settings) {
  // Write to a sibling temp file and atomically replace the target so a
  // crash mid-write never corrupts the previous settings.
  const std::filesystem::path target(path);
  if (target.has_parent_path()) {
    std::error_code dir_error;
    std::filesystem::create_directories(target.parent_path(), dir_error);
  }
  const std::filesystem::path temp =
      target.parent_path() / (target.filename().string() + ".tmp");
  {
    std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
    if (!stream) {
      return false;
    }
    stream << kKeyTrayEnabled << '\t' << FlagText(settings.tray_enabled) << '\n'
           << kKeyLightweight << '\t' << FlagText(settings.lightweight) << '\n'
           << kKeyThemeMode << '\t' << ThemeModeKey(settings.theme_mode) << '\n'
           << kKeyThemePreset << '\t' << ThemePresetKey(settings.theme_preset) << '\n';
    stream.flush();
    if (!stream) {
      std::error_code cleanup_error;
      std::filesystem::remove(temp, cleanup_error);
      return false;
    }
  }
  std::error_code error;
  std::filesystem::rename(temp, target, error);
  if (error) {
    // Windows cannot rename onto an existing file; fall back to a replace.
    std::error_code remove_error;
    std::filesystem::remove(target, remove_error);
    error.clear();
    std::filesystem::rename(temp, target, error);
    if (error) {
      std::error_code cleanup_error;
      std::filesystem::remove(temp, cleanup_error);
      return false;
    }
  }
  return true;
}

void SettingsModel::Load() {
  const TraySettings settings = LoadTraySettings(path);
  tray_enabled = settings.tray_enabled;
  lightweight = settings.lightweight;
  theme_mode = settings.theme_mode;
  theme_preset = settings.theme_preset;
}

TraySettings SettingsModel::Snapshot() const {
  return TraySettings{
      .tray_enabled = tray_enabled.Get(),
      .lightweight = lightweight.Get(),
      .theme_mode = theme_mode.Get(),
      .theme_preset = theme_preset.Get(),
  };
}

AppThemeConfig SettingsModel::ThemeConfig() const {
  return AppThemeConfig{
      .mode = theme_mode.Get(),
      .preset = theme_preset.Get(),
  };
}

bool SettingsModel::SetTrayEnabled(bool enabled) {
  tray_enabled = enabled;
  return SaveTraySettings(path, Snapshot());
}

bool SettingsModel::SetLightweight(bool value) {
  lightweight = value;
  return SaveTraySettings(path, Snapshot());
}

bool SettingsModel::SetThemeMode(ThemeMode mode) {
  theme_mode = mode;
  return SaveTraySettings(path, Snapshot());
}

bool SettingsModel::SetThemePreset(ThemePreset preset) {
  theme_preset = preset;
  return SaveTraySettings(path, Snapshot());
}

} // namespace dbpp
