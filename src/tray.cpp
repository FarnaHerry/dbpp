// tray.cpp — 桌面托盘运行时：图标、菜单、关闭策略与轻量级模式。
// 参考 Clash-Flux 的 DesktopTrayRuntime：托盘拥有应用生命周期
// （quit_on_last_window_closed = false），窗口只是托盘之上的展示层。

#include "tray.h"
#include "root.h"
#include "settings.h"
#include "app_resources.h"

#include <chrono>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace dbpp {

using namespace huxerui;

namespace {

constexpr std::chrono::duration<double> kPollInterval{0.25};

} // namespace

void TrayRuntime::ShowWindow() {
  if (!application || exit_requested) {
    return;
  }
  const bool opening = retired || !window;
  try {
    if (opening) {
      ++window_generation;
      retired = false;
      window = application->OpenWindow([] { return AppRoot(); },
                                       MainWindowOptions());
      presentation_key.clear();
    }
    if (window && !retired) {
      window->Show();
      window->Activate();
      window_visible = true;
    }
  } catch (const std::exception& error) {
    if (opening) {
      retired = true;
    }
    std::fprintf(stderr, "db++: 唤出主窗口失败：%s\n", error.what());
  }
}

void TrayRuntime::HideWindow() {
  if (window && !retired) {
    window->Hide();
  }
  window_visible = false;
}

void TrayRuntime::RequestExit() {
  if (exit_requested || !application) {
    return;
  }
  exit_requested = true;
  HideTray();
  HideWindow();
  application->Quit();
}

void TrayRuntime::ToggleTray() {
  if (settings) {
    (void)settings->SetTrayEnabled(!settings->tray_enabled.Get());
  }
}

void TrayRuntime::ToggleLightweight() {
  if (settings) {
    (void)settings->SetLightweight(!settings->lightweight.Get());
  }
}

// Rebuilds the tray presentation only when its state key changes; the poll
// loop calls this every tick so menu checkboxes follow settings changes.
void TrayRuntime::RefreshTray() {
  if (!application || !settings) {
    return;
  }
  const bool enabled = settings->tray_enabled.Get();
  const bool lightweight = settings->lightweight.Get();
  std::string key = enabled ? "1" : "0";
  key += lightweight ? "1" : "0";
  key += std::to_string(window_generation);
  if (key == presentation_key) {
    return;
  }
  const std::weak_ptr<TrayRuntime> weak = shared_from_this();
  std::vector<MenuEntry> menu;
  menu.emplace_back(MenuItem("显示主窗口", [weak] {
    if (const auto runtime = weak.lock()) {
      runtime->ShowWindow();
    }
  }));
  menu.emplace_back(MenuSection{});
  menu.emplace_back(MenuItem("启用托盘图标", [weak] {
    if (const auto runtime = weak.lock()) {
      runtime->ToggleTray();
    }
  }).Checked(enabled));
  menu.emplace_back(MenuItem("轻量级模式", [weak] {
    if (const auto runtime = weak.lock()) {
      runtime->ToggleLightweight();
    }
  }).Checked(lightweight));
  menu.emplace_back(MenuSection{});
  menu.emplace_back(MenuItem("退出 db++", [weak] {
    if (const auto runtime = weak.lock()) {
      runtime->RequestExit();
    }
  }));
  tray.Show(app::images::tray, {.tooltip = "db++", .menu = std::move(menu)});
  presentation_key = std::move(key);
}

void TrayRuntime::HideTray() {
  tray.Hide();
  // Hide discards the native presentation; unchanged settings must still
  // republish it after tray re-enable or host recovery.
  presentation_key.clear();
  tray_refresh_failed = false;
}

huxerui::WindowOptions MainWindowOptions() {
  return WindowOptions{
      .title = "db++",
      .initial_size = {1240.0F, 800.0F},
      .minimum_size = huxerui::Size{900.0F, 600.0F},
      .chrome_mode = huxerui::WindowChromeMode::Custom,
      .title_bar_height = 32.0F,
  };
}

void InstallTrayRuntime(ApplicationContext& context) {
  auto runtime = std::make_shared<TrayRuntime>(UseApplication());
  auto settings = std::make_shared<SettingsModel>();
  const File data_directory = runtime->application->Directories().data_directory;
  settings->path = data_directory.Child("settings.tsv").Path();
  settings->Load();
  runtime->settings = settings;
  context.Provide(std::move(settings));
  context.Provide(runtime);

  const TaskScope tasks = UseApplicationTaskScope();
  runtime->tasks = tasks;

  runtime->tray.OnActivate([runtime] { runtime->ShowWindow(); });

  // Hooks run before the event loop can create native windows, so the main
  // window opens from a task. The loop then keeps the tray presentation in
  // sync with settings and recovers the window when no tray entry point
  // remains (lightweight destroy + missing tray host must not strand the app).
  tasks.Launch([runtime]() -> Task<void> {
    runtime->ShowWindow();
    for (;;) {
      if (runtime->exit_requested) {
        co_return;
      }
      const bool enabled = runtime->settings->tray_enabled.Get();
      // The tray is the only entry point into a hidden or destroyed window.
      // Whenever it goes away (toggle off, host loss), recover the window so
      // the app can never strand behind a missing icon.
      const bool entry_point = enabled && runtime->tray.IsAvailable();
      if (!entry_point && (runtime->retired || !runtime->window_visible)) {
        runtime->ShowWindow();
      }
      if (entry_point) {
        try {
          runtime->RefreshTray();
          runtime->tray_refresh_failed = false;
        } catch (const std::exception& error) {
          if (!runtime->tray_refresh_failed) {
            std::fprintf(stderr, "db++: 刷新托盘失败：%s\n", error.what());
            runtime->ShowWindow();
          }
          runtime->tray_refresh_failed = true;
        }
      } else {
        runtime->HideTray();
      }
      co_await Delay(kPollInterval);
    }
  });
}

} // namespace dbpp
