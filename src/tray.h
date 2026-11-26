#pragma once

#include <huxerui/app.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dbpp {

/// Window options shared by the application declaration and the tray
/// runtime's OpenWindow rebuild path; the two must stay identical.
[[nodiscard]] huxerui::WindowOptions MainWindowOptions();

/// Application-scoped tray service (resolved via UseService in composition).
/// Owns the window handle across lightweight destroy/rebuild cycles; the
/// poll loop keeps the tray presentation and recovery policy in sync.
struct TrayRuntime : std::enable_shared_from_this<TrayRuntime> {
  explicit TrayRuntime(huxerui::ApplicationHandle app)
      : application(std::move(app)), tray(application->SystemTray()) {}

  std::optional<huxerui::ApplicationHandle> application;
  std::optional<huxerui::WindowHandle> window;
  huxerui::SystemTrayHandle tray;
  std::shared_ptr<huxerui::Resources> resources;
  std::shared_ptr<SettingsModel> settings;
  huxerui::TaskScope tasks;
  bool retired = true;
  bool exit_requested = false;
  bool tray_refresh_failed = false;
  bool window_visible = false;
  bool system_dark = false;
  std::uint64_t window_generation = 0;
  std::string presentation_key;
  std::optional<std::pair<int, bool>> applied_menu_theme;
  std::function<void(InstanceOpenRequest)> open_request_handler;
  std::optional<InstanceOpenRequest> pending_open_request;

  void ShowWindow();
  void HideWindow();
  void RequestExit();
  void ToggleTray();
  void ToggleLightweight();
  void UpdateSystemDark(bool dark);
  void RefreshTray();
  void HideTray();
  void SetOpenRequestHandler(std::function<void(InstanceOpenRequest)> handler);
  void ClearOpenRequestHandler();
  void DeliverOpenRequest(InstanceOpenRequest request);
};

/// Installs the desktop tray runtime as an application hook: tray lifecycle
/// and menu, close-to-tray policy, and the lightweight destroy/rebuild mode.
/// Requires the app declaration to use `quit_on_last_window_closed = false`.
void InstallTrayRuntime(huxerui::ApplicationContext& context);

} // namespace dbpp
