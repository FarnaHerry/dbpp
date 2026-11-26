// tray.cpp — 桌面托盘运行时：图标、菜单、关闭策略与轻量级模式。
// 参考 Clash-Flux 的 DesktopTrayRuntime：托盘拥有应用生命周期
// （quit_on_last_window_closed = false），窗口只是托盘之上的展示层。

#include <huxerui/huxerui.h>
#include "app_resources.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

import dbpp.settings;
import dbpp.i18n;
import dbpp.types;
import dbpp.db;
import dbpp.mcp;
import dbpp.config_db;
import dbpp.instance;

#include "theme_palette.h"
#include "tray.h"
#include "root.h"

#include <chrono>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace dbpp {

#ifdef __APPLE__
void ApplyMacOsTrayAppearance(int mode, bool effective_dark) noexcept;
#endif

using namespace huxerui;

namespace {

constexpr std::chrono::duration<double> kPollInterval{0.25};

#ifdef _WIN32
void ApplyNativeTrayMenuTheme(ThemeMode, bool effective_dark) noexcept {
  using RtlGetVersionFunction = LONG(WINAPI*)(OSVERSIONINFOW*);
  using SetPreferredAppModeFunction = int(WINAPI*)(int);
  using FlushMenuThemesFunction = void(WINAPI*)();

  const HMODULE native_lib = GetModuleHandleW(L"ntdll.dll");
  const auto get_version = reinterpret_cast<RtlGetVersionFunction>(
      native_lib ? GetProcAddress(native_lib, "RtlGetVersion") : nullptr);
  if (!get_version) {
    return;
  }

  OSVERSIONINFOW version{};
  version.dwOSVersionInfoSize = sizeof(version);
  if (get_version(&version) < 0 || version.dwMajorVersion < 10 ||
      version.dwBuildNumber < 17763) {
    return;
  }

  static const HMODULE theme_lib = LoadLibraryW(L"uxtheme.dll");
  if (!theme_lib) {
    return;
  }

  auto set_preferred_app_mode = reinterpret_cast<SetPreferredAppModeFunction>(
      GetProcAddress(theme_lib, "SetPreferredAppMode"));
  const bool modern_api =
      set_preferred_app_mode != nullptr || version.dwBuildNumber >= 18362;
  if (!set_preferred_app_mode) {
    set_preferred_app_mode = reinterpret_cast<SetPreferredAppModeFunction>(
        GetProcAddress(theme_lib, MAKEINTRESOURCEA(135)));
  }
  if (!set_preferred_app_mode) {
    return;
  }

  const int app_mode = modern_api ? (effective_dark ? 2 : 3) : (effective_dark ? 1 : 0);
  (void)set_preferred_app_mode(app_mode);

  auto flush_menu_themes = reinterpret_cast<FlushMenuThemesFunction>(
      GetProcAddress(theme_lib, "FlushMenuThemes"));
  if (!flush_menu_themes) {
    flush_menu_themes = reinterpret_cast<FlushMenuThemesFunction>(
        GetProcAddress(theme_lib, MAKEINTRESOURCEA(136)));
  }
  if (flush_menu_themes) {
    flush_menu_themes();
  }
}
#elif defined(__APPLE__)
void ApplyNativeTrayMenuTheme(ThemeMode mode, bool effective_dark) noexcept {
  ApplyMacOsTrayAppearance(static_cast<int>(mode), effective_dark);
}
#else
void ApplyNativeTrayMenuTheme(ThemeMode, bool) noexcept {}
#endif

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
  if (mcp_service) {
    mcp_service->Shutdown();
  }
  MarkInstanceClosing();
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

void TrayRuntime::UpdateSystemDark(bool dark) {
  if (system_dark == dark) {
    return;
  }
  system_dark = dark;
  if (settings) {
    const ThemeMode mode = settings->theme_mode.Get();
    const bool effective_dark = ui::ResolveIsDark(mode, system_dark);
    const std::pair<int, bool> target{static_cast<int>(mode), effective_dark};
    if (!applied_menu_theme.has_value() || *applied_menu_theme != target) {
      ApplyNativeTrayMenuTheme(mode, effective_dark);
      applied_menu_theme = target;
    }
  }
}

// Rebuilds the tray presentation only when its state key changes; the poll
// loop calls this every tick so menu checkboxes follow settings changes.
void TrayRuntime::RefreshTray() {
  if (!application || !settings) {
    return;
  }
  const ThemeMode theme_mode = settings->theme_mode.Get();
  const bool effective_dark = ui::ResolveIsDark(theme_mode, system_dark);
  const std::pair<int, bool> desired_menu_theme{static_cast<int>(theme_mode), effective_dark};
  if (!applied_menu_theme.has_value() || *applied_menu_theme != desired_menu_theme) {
    ApplyNativeTrayMenuTheme(theme_mode, effective_dark);
    applied_menu_theme = desired_menu_theme;
  }

  const LanguageMode language_mode = settings->language.Get();
  const Locale effective_locale =
      resources ? EffectiveLocale(language_mode, resources->Configuration().locale)
                : Locale::Default();
  SetActiveUiLanguage(ResolveUiLanguage(language_mode, effective_locale.LanguageTag()));

  const bool enabled = settings->tray_enabled.Get();
  const bool lightweight = settings->lightweight.Get();
  std::string key = enabled ? "1" : "0";
  key += lightweight ? "1" : "0";
  key += effective_dark ? "D" : "L";
  key += std::to_string(window_generation);
  key.push_back('|');
  key.append(effective_locale.LanguageTag());
  if (key == presentation_key) {
    return;
  }

  const ResourceConfiguration resource_config{.locale = effective_locale};
  const auto tr_res = [&](const StringResource& res_id, std::string_view fallback_en,
                          std::string_view fallback_zh) -> std::string {
    if (resources) {
      return resources->GetString(res_id, resource_config);
    }
    return std::string(Tr(fallback_en, fallback_zh));
  };

  const std::weak_ptr<TrayRuntime> weak = shared_from_this();
  std::vector<MenuEntry> menu;
  menu.emplace_back(MenuItem(
      tr_res(app::strings::tray_show_window, "Show Window", "显示主窗口"), [weak] {
        if (const auto runtime = weak.lock()) {
          runtime->ShowWindow();
        }
      }));
  menu.emplace_back(MenuSection{});
  menu.emplace_back(MenuItem(
      tr_res(app::strings::tray_enable_icon, "Enable Tray Icon", "启用托盘图标"), [weak] {
        if (const auto runtime = weak.lock()) {
          runtime->ToggleTray();
        }
      }).Checked(enabled));
  menu.emplace_back(MenuItem(
      tr_res(app::strings::tray_lightweight, "Lightweight Mode", "轻量级模式"), [weak] {
        if (const auto runtime = weak.lock()) {
          runtime->ToggleLightweight();
        }
      }).Checked(lightweight));
  menu.emplace_back(MenuSection{});
  menu.emplace_back(MenuItem(
      tr_res(app::strings::tray_quit, "Quit db++", "退出 db++"), [weak] {
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

void TrayRuntime::SetOpenRequestHandler(std::function<void(InstanceOpenRequest)> handler) {
  open_request_handler = std::move(handler);
  if (open_request_handler && pending_open_request.has_value()) {
    InstanceOpenRequest queued = std::move(*pending_open_request);
    pending_open_request.reset();
    open_request_handler(std::move(queued));
  }
}

void TrayRuntime::ClearOpenRequestHandler() {
  open_request_handler = nullptr;
}

void TrayRuntime::DeliverOpenRequest(InstanceOpenRequest request) {
  if (open_request_handler) {
    open_request_handler(std::move(request));
  } else {
    pending_open_request = std::move(request);
  }
}

huxerui::WindowOptions MainWindowOptions() {
  return WindowOptions{
      .title = "db++",
      .initial_size = {1240.0F, 800.0F},
      .minimum_size = huxerui::Size{900.0F, 600.0F},
      .chrome_mode = huxerui::WindowChromeMode::Custom,
      .title_bar_height = 40.0F,
  };
}

void InstallTrayRuntime(ApplicationContext& context) {
  auto runtime = std::make_shared<TrayRuntime>(UseApplication());
  auto config_db = std::make_shared<ConfigDbService>();
  auto settings = std::make_shared<SettingsModel>();
  const File data_directory = runtime->application->Directories().data_directory;
  const std::string db_path = data_directory.Child("dbpp.sqlite3").Path();
  settings->path = db_path;
  const TraySettings startup = ConfigDbService::ReadStartupSettingsSync(db_path);
  settings->tray_enabled = startup.tray_enabled;
  settings->lightweight = startup.lightweight;
  settings->theme_mode = startup.theme_mode;
  settings->theme_preset = startup.theme_preset;
  settings->language = startup.language;
  settings->mcp_enabled = startup.mcp_enabled;
  settings->mcp_host = startup.mcp_host;
  settings->mcp_port = startup.mcp_port;
  settings->mcp_read_only = startup.mcp_read_only;
  settings->persist_setting = [config_db](std::string key, std::string value) {
    config_db->PersistSetting(std::move(key), std::move(value));
  };
  try {
    runtime->resources = UseService<Resources>();
  } catch (const std::exception&) {
    runtime->resources.reset();
  }
  {
    const Locale effective =
        runtime->resources
            ? EffectiveLocale(startup.language, runtime->resources->Configuration().locale)
            : Locale::Default();
    SetActiveUiLanguage(ResolveUiLanguage(startup.language, effective.LanguageTag()));
  }
  runtime->settings = settings;
  {
    const bool effective_dark = ui::ResolveIsDark(startup.theme_mode, runtime->system_dark);
    ApplyNativeTrayMenuTheme(startup.theme_mode, effective_dark);
    runtime->applied_menu_theme = std::pair<int, bool>{
        static_cast<int>(startup.theme_mode), effective_dark};
  }
  auto mcp_service = std::make_shared<McpServerService>();
  mcp_service->profiles_db_path = db_path;
  (void)mcp_service->ApplySettings(startup.mcp_enabled, startup.mcp_host, startup.mcp_port,
                                   startup.mcp_read_only);
  runtime->mcp_service = mcp_service;
  context.Provide(mcp_service);
  context.Provide(config_db);
  context.Provide(std::move(settings));
  context.Provide(runtime);

  const TaskScope tasks = UseApplicationTaskScope();
  runtime->tasks = tasks;

  runtime->tray.OnActivate([runtime] { runtime->ShowWindow(); });

  // Hooks run before the event loop can create native windows, so the main
  // window opens from a task. Open/migrate the Lib-SQLite config database first
  // so legacy TSV configs are imported cleanly before UI composition starts.
  tasks.Launch([runtime, config_db, db_path, tasks]() -> Task<void> {
    (void)co_await config_db->OpenAndMigrateAsync(db_path, tasks);
    runtime->ShowWindow();
    for (;;) {
      if (runtime->exit_requested) {
        co_return;
      }
      const bool activated = ConsumeInstanceActivation();
      auto open_req = TakePendingInstanceOpenRequest();
      if (activated || open_req.has_value()) {
        runtime->ShowWindow();
        if (open_req.has_value()) {
          runtime->DeliverOpenRequest(std::move(*open_req));
        }
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
