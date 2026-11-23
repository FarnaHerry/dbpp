#include "settings.h"

#include <filesystem>
#include <fstream>
#include <string_view>
#include <utility>

namespace dbpp {

namespace {

constexpr std::string_view kKeyTrayEnabled = "tray.enabled";
constexpr std::string_view kKeyLightweight = "tray.lightweight";

std::string_view FlagText(bool value) { return value ? "true" : "false"; }

} // namespace

TraySettings LoadTraySettings(const std::string& path) {
  TraySettings settings;
  std::ifstream stream(path);
  if (!stream) {
    return settings;
  }
  std::string line;
  while (std::getline(stream, line)) {
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
    }
  }
  return settings;
}

bool SaveTraySettings(const std::string& path, const TraySettings& settings) {
  // Write to a sibling temp file and atomically replace the target so a
  // crash mid-write never corrupts the previous settings.
  const std::filesystem::path target(path);
  const std::filesystem::path temp = target.parent_path() /
                                     (target.filename().string() + ".tmp");
  {
    std::ofstream stream(temp, std::ios::trunc);
    if (!stream) {
      return false;
    }
    stream << kKeyTrayEnabled << '\t' << FlagText(settings.tray_enabled) << '\n'
           << kKeyLightweight << '\t' << FlagText(settings.lightweight) << '\n';
    stream.flush();
    if (!stream) {
      return false;
    }
  }
  std::error_code error;
  std::filesystem::rename(temp, target, error);
  if (error) {
    // Windows cannot rename onto an existing file; fall back to a replace.
    std::filesystem::remove(target, error);
    error.clear();
    std::filesystem::rename(temp, target, error);
  }
  return !error;
}

void SettingsModel::Load() {
  const TraySettings settings = LoadTraySettings(path);
  tray_enabled = settings.tray_enabled;
  lightweight = settings.lightweight;
}

TraySettings SettingsModel::Snapshot() const {
  return TraySettings{
      .tray_enabled = tray_enabled.Get(),
      .lightweight = lightweight.Get(),
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

} // namespace dbpp
