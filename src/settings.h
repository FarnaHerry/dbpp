#pragma once

#include <huxerui/huxerui.h>

#include <string>

namespace dbpp {

/// Persisted tray preferences. Stored as key/value TSV in the application
/// data directory (`settings.tsv`), same format as `connections.tsv`.
struct TraySettings {
  bool tray_enabled = true;
  bool lightweight = false;

  bool operator==(const TraySettings&) const = default;
};

[[nodiscard]] TraySettings LoadTraySettings(const std::string& path);
bool SaveTraySettings(const std::string& path, const TraySettings& settings);

/// Application-scoped settings holder shared by the tray runtime, the window
/// close handler, and the sidebar switches. States are constructed outside
/// any composition; updates go through the Set* methods, which also persist.
struct SettingsModel {
  huxerui::State<bool> tray_enabled{true};
  huxerui::State<bool> lightweight{false};
  std::string path;

  void Load();
  bool SetTrayEnabled(bool enabled);
  bool SetLightweight(bool value);

  [[nodiscard]] TraySettings Snapshot() const;
};

} // namespace dbpp
