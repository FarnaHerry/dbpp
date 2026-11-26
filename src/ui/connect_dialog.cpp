#include "ui/ui_common.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <ranges>

namespace dbpp {

using namespace huxerui;
using namespace dbpp::ui;

[[huxerui::composable]] View ConnectDialog(DialogContext dialog, ConnectContext context) {
  const auto config_db = UseService<ConfigDbService>();
  const LocalizedCatalog loc{UseService<Resources>(), UseService<SettingsModel>()};
  const ThemeSpec& spec = UseTheme();
  const SemanticColors& semantic = UseEnvironment<SemanticColors>();
  auto dialog_tasks = UseTaskScope();
  auto step = UseState<int>(0);
  auto type_filter = UseState(TextEditingValue::FromText({}));
  auto extra_query = UseState(std::string{});
  auto category_tab = UseState<std::size_t>(0);
  auto selected_preset_id = UseState<std::string>("sqlite");
  auto drivers = UseState(std::vector<AdbcDriverInfo>{});
  auto input_mode = UseState<std::size_t>(0);

  auto driver = UseState(TextEditingValue::FromText(context.default_driver));
  auto uri = UseState(TextEditingValue::FromText(context.default_uri));
  auto host = UseState(TextEditingValue::FromText("127.0.0.1"));
  auto port = UseState(TextEditingValue::FromText({}));
  auto database = UseState(TextEditingValue::FromText({}));
  auto username = UseState(TextEditingValue::FromText({}));
  auto password = UseState(TextEditingValue::FromText({}));
  auto title = UseState(TextEditingValue::FromText({}));
  auto save_profile_flag = UseState(true);

  const ConnectionFormStates form{
      .driver_slug = selected_preset_id,
      .driver = driver,
      .uri = uri,
      .host = host,
      .port = port,
      .database = database,
      .username = username,
      .password = password,
      .title = title,
      .extra_query = extra_query,
      .input_mode = input_mode,
  };

  auto installing = UseState(false);
  auto install_status = UseState(std::string{});

  const auto refresh_drivers = [drivers]() {
    drivers = DiscoverAdbcDrivers();
  };

  Lifecycle([refresh_drivers] {
    refresh_drivers();
    return [] {};
  });

  const auto& presets = SupportedDatabasePresets();
  const DatabasePreset* active_ptr = &presets.front();
  for (const DatabasePreset& candidate : presets) {
    if (candidate.id == selected_preset_id.Get()) {
      active_ptr = &candidate;
      break;
    }
  }
  const DatabasePreset active_preset = *active_ptr;
  const LocalizedPresetStrings active_preset_strings = DatabasePresetStrings(active_preset);
  const std::string active_preset_name = UseString(active_preset_strings.name);
  const std::string active_preset_placeholder = active_preset_strings.uri_placeholder
                                                    ? UseString(*active_preset_strings.uri_placeholder)
                                                    : active_preset.uri_placeholder;
  const std::vector<AdbcDriverInfo>& driver_list = drivers.Get();
  const std::optional<AdbcDriverInfo> matched_driver =
      FindInstalledDriverForPreset(active_preset, driver_list);

  const auto select_preset = [context, step, selected_preset_id, input_mode, driver, uri,
                              host, port, database, username, password, title, extra_query,
                              install_status, drivers](const DatabasePreset& preset) {
    selected_preset_id = preset.id;
    step = 1;
    input_mode = 0;
    install_status = {};
    title = TextEditingValue::FromText({});
    password = TextEditingValue::FromText({});
    extra_query = {};

    const std::optional<AdbcDriverInfo> installed =
        FindInstalledDriverForPreset(preset, drivers.Get());
    if (preset.builtin && preset.form_kind == ConnectionFormKind::File) {
      driver = TextEditingValue::FromText(
          context.default_driver.empty() ? DefaultSqliteDriverPath() : context.default_driver);
      uri = TextEditingValue::FromText(context.default_uri);
      username = TextEditingValue::FromText({});
      return;
    }
    if (preset.form_kind == ConnectionFormKind::File) {
      driver = TextEditingValue::FromText(installed ? installed->path : preset.driver_slug);
      uri = TextEditingValue::FromText({});
      username = TextEditingValue::FromText({});
      return;
    }
    if (preset.form_kind == ConnectionFormKind::HostPort) {
      driver = TextEditingValue::FromText(installed ? installed->path : preset.driver_slug);
      host = TextEditingValue::FromText(preset.default_host);
      port = TextEditingValue::FromText(preset.default_port);
      database = TextEditingValue::FromText(preset.default_database);
      username = TextEditingValue::FromText(preset.default_user);
      uri = TextEditingValue::FromText(
          ComposePresetUri(preset, preset.default_host, preset.default_port, preset.default_database));
      return;
    }
    if (preset.form_kind == ConnectionFormKind::Uri) {
      driver = TextEditingValue::FromText(installed ? installed->path : preset.driver_slug);
      uri = TextEditingValue::FromText({});
      username = TextEditingValue::FromText({});
      return;
    }
    driver = TextEditingValue::FromText({});
    uri = TextEditingValue::FromText({});
    username = TextEditingValue::FromText({});
  };

  if (step.Get() == 0) {
    const std::string filter_text = type_filter.Get().text;
    const std::size_t cat_idx = category_tab.Get();
    struct VisiblePreset {
      const DatabasePreset* preset = nullptr;
      std::string name;
      std::string subtitle;
    };
    std::vector<VisiblePreset> visible;
    visible.reserve(presets.size());
    for (const DatabasePreset& preset : presets) {
      bool cat_ok = true;
      switch (cat_idx) {
        case 1: cat_ok = preset.category == DatabaseCategory::LocalFile; break;
        case 2: cat_ok = preset.category == DatabaseCategory::Relational; break;
        case 3: cat_ok = preset.category == DatabaseCategory::KeyValue; break;
        case 4: cat_ok = preset.category == DatabaseCategory::AnalyticsCloud; break;
        case 5: cat_ok = preset.category == DatabaseCategory::Custom; break;
        default: break;
      }
      if (!cat_ok) {
        continue;
      }
      const LocalizedPresetStrings p_strs = DatabasePresetStrings(preset);
      std::string p_name = UseString(p_strs.name);
      std::string p_sub = UseString(p_strs.subtitle);
      if (!filter_text.empty() &&
          !ContainsIgnoreCaseAscii(p_name, filter_text) &&
          !ContainsIgnoreCaseAscii(p_sub, filter_text) &&
          !ContainsIgnoreCaseAscii(preset.name, filter_text) &&
          !ContainsIgnoreCaseAscii(preset.id, filter_text) &&
          !ContainsIgnoreCaseAscii(preset.driver_slug, filter_text) &&
          !ContainsIgnoreCaseAscii(preset.subtitle, filter_text)) {
        continue;
      }
      visible.push_back(VisiblePreset{
          .preset = &preset,
          .name = std::move(p_name),
          .subtitle = std::move(p_sub),
      });
    }

    Views grid_rows;
    grid_rows.Reserve((visible.size() + 1) / 2);
    const std::string badge_builtin = UseString(app::strings::connect_badge_builtin);
    const std::string badge_manual = UseString(app::strings::connect_badge_manual);
    const std::string badge_ready = UseString(app::strings::connect_badge_ready);
    const std::string badge_install = UseString(app::strings::connect_badge_install);
    const auto make_card = [spec, semantic, &driver_list, select_preset, &badge_builtin,
                            &badge_manual, &badge_ready,
                            &badge_install](const VisiblePreset& entry) {
      const DatabasePreset& preset = *entry.preset;
      View badge;
      if (preset.builtin) {
        badge = StatusBadge(badge_builtin, semantic.success, semantic.success_container,
                            WithAlpha(semantic.success, 0.35F));
      } else if (preset.form_kind == ConnectionFormKind::Custom) {
        badge = StatusBadge(badge_manual, spec.colors.on_surface_variant,
                            spec.colors.surface_container, semantic.border_subtle);
      } else if (FindInstalledDriverForPreset(preset, driver_list).has_value()) {
        badge = StatusBadge(badge_ready, semantic.success, semantic.success_container,
                            WithAlpha(semantic.success, 0.35F));
      } else {
        badge = StatusBadge(badge_install, semantic.warning, semantic.warning_container,
                            WithAlpha(semantic.warning, 0.35F));
      }
      View nav = NavItem(
          entry.name, entry.subtitle, false,
          [select_preset, preset]() mutable { select_preset(preset); }, std::move(badge),
          std::nullopt, DatabaseBadgeIcon(DetectDatabaseBrand(preset.id), 20.0F, 34.0F));
      return Column{std::move(nav)}
          .With(Grow(1.0F), CrossAlign{CrossAxisAlignment::Stretch},
                Background{VisualFill{spec.colors.surface}}, Border{semantic.border_subtle, 1.0F},
                CornerRadius{7.0F})
          .Key("db-type-" + preset.id);
    };

    for (std::size_t i = 0; i < visible.size(); i += 2) {
      const VisiblePreset* left = &visible[i];
      const VisiblePreset* right = (i + 1 < visible.size()) ? &visible[i + 1] : nullptr;
      View left_card = make_card(*left);
      View right_slot = right != nullptr ? make_card(*right) : View(Spacer().With(Grow(1.0F)));
      grid_rows.Add(
          Row{std::move(left_card), std::move(right_slot)}
              .With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch})
              .Key("db-type-row-" + left->preset->id));
    }

    View grid_content;
    if (grid_rows.Size() == 0) {
      grid_content = Column{
          MutedText(UseString(app::strings::connect_no_match)),
      }.With(Grow(1.0F), MainAlign{MainAxisAlignment::Center},
             CrossAlign{CrossAxisAlignment::Center});
    } else {
      grid_content = ScrollView(Column{std::move(grid_rows)}
                                    .With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch}))
                         .With(Grow(1.0F));
    }

    View body = Column{
        Row{
            Text(app::strings::connect_title, TextRole::Title),
            MutedText(UseString(app::strings::connect_step0_subtitle)),
            Spacer(),
            ActionButton(Button(app::strings::connect_saved_profiles_btn)
                             .OnClick([dialog, open_manager = context.open_manager] {
                               dialog.Dismiss();
                               if (open_manager) {
                                 open_manager(0, {});
                               }
                             })),
            ActionButton(Button(app::strings::action_cancel).OnClick([dialog] { dialog.Dismiss(); })),
        }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{8.0F}),
        TextField(type_filter)
            .Label(app::strings::connect_search_label)
            .Placeholder(app::strings::connect_search_placeholder)
            .Variant(TextFieldVariant::Outlined)
            .LineLimits(TextFieldLineLimits::SingleLine())
            .OnChanged([type_filter](const TextEditingValue& value) { type_filter = value; }),
        SegmentedButton({app::strings::connect_cat_all, app::strings::connect_cat_file,
                         app::strings::connect_cat_relational, app::strings::connect_cat_kv,
                         app::strings::connect_cat_analytics, app::strings::connect_cat_custom},
                        cat_idx)
            .OnChanged([category_tab](std::size_t index) { category_tab = index; }),
        std::move(grid_content),
        MutedText(UseString(app::strings::connect_step0_hint)),
    }.With(Spacing{10.0F}, CrossAlign{CrossAxisAlignment::Stretch},
           Frame{.width = 740.0F, .height = 560.0F});

    return DialogCard(std::move(body));
  }

  const auto install_active_driver = [context, dialog_tasks, active_preset, installing,
                                      install_status, driver,
                                      refresh_drivers, loc]() {
    if (installing.Get() || active_preset.driver_slug.empty()) {
      return;
    }
    if (!context.http) {
      if (context.notify) {
        context.notify(loc.Get(app::strings::toast_http_unavailable));
      }
      return;
    }
    installing = true;
    install_status = loc.Get(app::strings::connect_query_registry);
    const std::string target_slug = active_preset.driver_slug;
    const auto http = context.http;
    const NotifyFn notify = context.notify;

    dialog_tasks.Launch([http, target_slug, installing, install_status,
                         driver, refresh_drivers, notify, loc]() -> Task<void> {
      RegistryCatalogResult cat = co_await FetchRegistryCatalogAsync(http);
      if (!cat.error.empty()) {
        installing = false;
        install_status = {};
        if (notify) {
          notify(cat.error);
        }
        co_return;
      }

      const RegistryDriverInfo* found = nullptr;
      for (const RegistryDriverInfo& item : cat.drivers) {
        if (item.slug == target_slug) {
          found = &item;
          break;
        }
      }
      if (found == nullptr) {
        installing = false;
        install_status = {};
        if (notify) {
          notify(loc.Fmt(app::strings::connect_platform_no_driver, CurrentPlatformTuple(),
                         target_slug));
        }
        co_return;
      }

      RegistryDriverInfo item = *found;
      auto on_progress = [install_status](std::string msg) { install_status = std::move(msg); };
      DriverInstallResult res =
          co_await InstallRegistryDriverAsync(http, item, std::move(on_progress));
      installing = false;
      install_status = {};
      if (!res.ok) {
        if (notify) {
          notify(res.message);
        }
        co_return;
      }
      refresh_drivers();
      driver = TextEditingValue::FromText(res.slug);
      if (notify) {
        notify(res.message);
      }
    });
  };

  const auto pick_local_file = [context, dialog_tasks, active_preset, active_preset_strings, uri,
                                title, loc]() {
    if (!context.picker || !context.picker->CanOpenFiles()) {
      if (context.notify) {
        context.notify(loc.Get(app::strings::connect_picker_unsupported));
      }
      return;
    }
    const std::string filter_label =
        loc.Fmt(app::strings::connect_picker_filter, loc.Get(active_preset_strings.name));
    dialog_tasks.Launch([picker = context.picker, filter_label,
                         extensions = active_preset.file_extensions, uri, title,
                         notify = context.notify, loc]() -> Task<void> {
      FilePickerFilter filter;
      filter.name = filter_label;
      filter.extensions = extensions;
      auto file_reference = co_await picker->OpenFileAsync(std::move(filter));
      if (!file_reference) {
        co_return;
      }
      auto file = file_reference->AsFile();
      if (!file) {
        if (notify) {
          notify(loc.Get(app::strings::connect_picker_no_local_path));
        }
        co_return;
      }
      uri = TextEditingValue::FromText(file->Path());
      if (title.Get().text.empty()) {
        title = TextEditingValue::FromText(file->Name());
      }
    });
  };

  const auto connect = [context, config_db, active_preset, input_mode, driver, uri, host, port,
                        database, extra_query, username, password, title, save_profile_flag, dialog,
                        loc] {
    const std::string effective_uri =
        (active_preset.form_kind == ConnectionFormKind::HostPort && input_mode.Get() == 0)
            ? ComposePresetUri(active_preset, host.Get().text, port.Get().text,
                               database.Get().text, extra_query.Get())
            : uri.Get().text;
    const SanitizedUriResult sanitized = SanitizeUriCredentials(effective_uri);

    ConnectionRequest request;
    request.driver_path = driver.Get().text;
    request.uri = sanitized.uri;
    request.username = username.Get().text.empty() ? sanitized.username : username.Get().text;
    request.password = password.Get().text.empty() ? sanitized.password : password.Get().text;
    request.title = title.Get().text.empty() ? UriDisplayName(sanitized.uri) : title.Get().text;

    if (request.driver_path.empty() || request.uri.empty()) {
      if (context.notify) {
        context.notify(loc.Get(app::strings::toast_driver_addr_required));
      }
      return;
    }

    if (save_profile_flag.Get() && !context.profiles_path.empty()) {
      ConnectionProfile profile{
          .name = request.title,
          .driver_path = request.driver_path,
          .uri = request.uri,
          .username = request.username,
          .password = request.password,
      };
      std::vector<ConnectionProfile> list = context.profiles.Get();
      bool updated = false;
      for (ConnectionProfile& existing : list) {
        if (existing.name == profile.name) {
          existing = profile;
          updated = true;
          break;
        }
      }
      if (!updated) {
        for (ConnectionProfile& existing : list) {
          if (existing.driver_path == profile.driver_path && existing.uri == profile.uri &&
              existing.username == profile.username) {
            existing = profile;
            updated = true;
            break;
          }
        }
      }
      if (!updated) {
        list.push_back(profile);
      }
      if (!PersistConnectionProfileList(config_db, context.profiles_path, list)) {
        if (context.notify) {
          context.notify(
              loc.Fmt(app::strings::toast_save_profiles_path_failed, context.profiles_path));
        }
        return;
      }
      context.profiles = std::move(list);
    }

    OpenConnection(context.app, context.workspace, context.tasks, std::move(request),
                   context.notify);
    dialog.Dismiss();
  };

  View readiness_strip;
  if (active_preset.form_kind != ConnectionFormKind::Custom) {
    if (matched_driver.has_value()) {
      std::string ready_text;
      if (active_preset.id == "redis") {
        ready_text = UseString(app::strings::connect_ready_redis);
      } else if (active_preset.builtin) {
        ready_text = UseString(app::strings::connect_ready_sqlite, matched_driver->path);
      } else {
        const std::string& driver_detail =
            matched_driver->detail.empty() ? matched_driver->path : matched_driver->detail;
        ready_text = UseString(app::strings::connect_ready_driver, driver_detail);
      }
      readiness_strip = Row{
          AccentDot(semantic.success, 7.0F),
          MutedText(ready_text),
      }.With(Spacing{6.0F}, CrossAlign{CrossAxisAlignment::Center});
    } else {
      readiness_strip =
          Row{
              Column{
                  Text::Format(app::strings::connect_missing_driver_title, active_preset_name,
                               active_preset.driver_slug),
                  MutedText(UseString(app::strings::connect_missing_driver_hint)),
              }.With(Grow(1.0F), Spacing{2.0F}, CrossAlign{CrossAxisAlignment::Stretch}),
              ActionButton(Button(installing.Get() ? app::strings::connect_installing_btn
                                                   : app::strings::connect_one_click_install)
                               .OnClick(install_active_driver)
                               .With(Enabled{!installing.Get()}),
                           ButtonTone::Primary),
              ActionButton(Button(app::strings::connect_open_manager_btn)
                               .OnClick([dialog, open_manager = context.open_manager,
                                         slug = active_preset.driver_slug] {
                                 dialog.Dismiss();
                                 if (open_manager) {
                                   open_manager(1, slug);
                                 }
                               })),
          }.With(Padding{EdgeInsets::Symmetric(10.0F, 8.0F)}, Spacing{8.0F},
                 CrossAlign{CrossAxisAlignment::Center},
                 Background{VisualFill{semantic.warning_container}},
                 Border{WithAlpha(semantic.warning, 0.45F), 1.0F}, CornerRadius{7.0F});
    }
  }

  View form_fields = Column{
      ConnectionFormFields(
          active_preset, active_preset_placeholder, form, driver_list,
          context.picker && context.picker->CanOpenFiles(), pick_local_file, connect,
          (active_preset.form_kind == ConnectionFormKind::File ||
           active_preset.form_kind == ConnectionFormKind::Custom)
              ? app::strings::connect_name_filename_hint
              : app::strings::connect_name_auto_hint),
      Switch(app::strings::connect_save_profile_switch, save_profile_flag)
          .OnChanged([save_profile_flag](bool value) { save_profile_flag = value; }),
  }.With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch});

  View step1_body = Column{
      Row{
          ActionButton(Button(app::strings::connect_back_step0).OnClick([step] {
            step = 0;
          })),
          DatabaseIcon(DetectDatabaseBrand(active_preset.id), 22.0F),
          Text::Format(TextRole::Title, app::strings::connect_step1_title, active_preset_name),
          Spacer(),
          ActionButton(Button(app::strings::action_cancel).OnClick([dialog] { dialog.Dismiss(); })),
          ActionButton(Button(app::strings::action_connect).OnClick(connect), ButtonTone::Primary)
              .With(Enabled{!installing.Get()}),
      }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{8.0F}),
      std::move(readiness_strip),
      ScrollView(std::move(form_fields)).With(Grow(1.0F)),
  }.With(Spacing{10.0F}, CrossAlign{CrossAxisAlignment::Stretch});

  const StringVariant install_label =
      !install_status.Get().empty() ? StringVariant(install_status.Get())
                                    : StringVariant(app::strings::connect_installing_btn);
  return DialogCard(LoadingOverlay(std::move(step1_body), installing.Get(), install_label)
                        .With(Frame{.width = 740.0F, .height = 560.0F}));
}

} // namespace dbpp
