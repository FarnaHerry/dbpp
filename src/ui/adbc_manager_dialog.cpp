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

[[huxerui::composable]] View AdbcManagerDialog(DialogContext dialog,
                                                   AdbcManagerContext context) {
  const auto config_db = UseService<ConfigDbService>();
  const LocalizedCatalog loc{UseService<Resources>(), UseService<SettingsModel>()};
  auto profiles = context.profiles;
  auto dialog_tasks = UseTaskScope();
  auto tab = UseState(context.initial_tab);
  auto drivers = UseState(std::vector<AdbcDriverInfo>{});
  auto user_installed = UseState(std::vector<std::string>{});
  auto catalog = UseState(std::vector<RegistryDriverInfo>{});
  auto catalog_loaded = UseState(false);
  auto catalog_loading = UseState(false);
  auto catalog_failed = UseState(false);
  auto registry_filter = UseState(TextEditingValue::FromText(context.initial_filter));
  auto installing_slug = UseState(std::string{});
  auto install_status = UseState(std::string{});
  auto selected = UseState<int>(-1);
  auto driver = UseState(TextEditingValue::FromText(context.default_driver));
  auto uri = UseState(TextEditingValue::FromText(context.default_uri));
  auto username = UseState(TextEditingValue::FromText({}));
  auto password = UseState(TextEditingValue::FromText({}));
  auto name = UseState(TextEditingValue::FromText({}));

  const auto refresh_drivers = [drivers, user_installed]() -> std::size_t {
    std::vector<AdbcDriverInfo> list = DiscoverAdbcDrivers();
    std::vector<std::string> user_slugs;
    for (const AdbcDriverInfo& info : list) {
      if (info.is_manifest && IsUserInstalledAdbcDriver(info.path)) {
        user_slugs.push_back(info.path);
      }
    }
    const std::size_t count = list.size();
    drivers = std::move(list);
    user_installed = std::move(user_slugs);
    return count;
  };

  const auto load_catalog = [context, dialog_tasks, catalog, catalog_loaded, catalog_loading,
                             catalog_failed, loc]() {
    if (catalog_loading.Get()) {
      return;
    }
    if (!context.http) {
      catalog_failed = true;
      if (context.notify) {
        context.notify(loc.Get(app::strings::toast_http_unavailable));
      }
      return;
    }
    catalog_loading = true;
    catalog_failed = false;
    dialog_tasks.Launch([http = context.http, notify = context.notify, catalog, catalog_loaded,
                         catalog_loading, catalog_failed]() -> Task<void> {
      RegistryCatalogResult res = co_await FetchRegistryCatalogAsync(http);
      catalog_loading = false;
      catalog_loaded = true;
      if (!res.error.empty()) {
        catalog_failed = true;
        if (notify) {
          notify(res.error);
        }
        co_return;
      }
      catalog_failed = false;
      catalog = std::move(res.drivers);
    });
  };

  Lifecycle([refresh_drivers, load_catalog, initial_tab = context.initial_tab] {
    (void)refresh_drivers();
    if (initial_tab == 1) {
      load_catalog();
    }
    return [] {};
  });

  const auto switch_tab = [tab, catalog_loaded, catalog_loading,
                           load_catalog](std::size_t index) {
    tab = index;
    if (index == 1 && !catalog_loaded.Get() && !catalog_loading.Get()) {
      load_catalog();
    }
  };

  const auto start_install = [context, dialog_tasks, driver, installing_slug,
                              install_status, refresh_drivers, loc](RegistryDriverInfo item) {
    if (!installing_slug.Get().empty()) {
      return;
    }
    if (!context.http) {
      if (context.notify) {
        context.notify(loc.Get(app::strings::toast_http_unavailable));
      }
      return;
    }
    installing_slug = item.slug;
    install_status = loc.Fmt(app::strings::adbc_preparing_download, item.slug);
    dialog_tasks.Launch([context, item = std::move(item), driver, installing_slug,
                         install_status, refresh_drivers]() -> Task<void> {
      auto on_progress = [install_status](std::string msg) { install_status = std::move(msg); };
      DriverInstallResult res =
          co_await InstallRegistryDriverAsync(context.http, item, std::move(on_progress));
      installing_slug = {};
      install_status = {};
      if (!res.ok) {
        if (context.notify) {
          context.notify(res.message);
        }
        co_return;
      }
      (void)refresh_drivers();
      driver = TextEditingValue::FromText(res.slug);
      if (context.notify) {
        context.notify(res.message);
      }
    });
  };

  const auto start_uninstall = [context, dialog_tasks, installing_slug,
                                install_status, refresh_drivers, loc](std::string slug) {
    if (!installing_slug.Get().empty()) {
      return;
    }
    installing_slug = slug;
    install_status = loc.Fmt(app::strings::adbc_uninstalling, slug);
    dialog_tasks.Launch([context, slug = std::move(slug), installing_slug,
                         install_status, refresh_drivers]() -> Task<void> {
      auto uninstall_job = [slug]() { return UninstallUserAdbcDriverBlocking(slug); };
      DriverInstallResult res = co_await RunWorker(std::move(uninstall_job));
      installing_slug = {};
      install_status = {};
      if (!res.ok) {
        if (context.notify) {
          context.notify(res.message);
        }
        co_return;
      }
      (void)refresh_drivers();
      if (context.notify) {
        context.notify(res.message);
      }
    });
  };

  const auto save_profile = [context, config_db, profiles, selected, driver, uri, username,
                             password, name, loc]() -> bool {
    const std::string driver_path = driver.Get().text;
    const std::string uri_text = uri.Get().text;
    if (driver_path.empty() || uri_text.empty()) {
      if (context.notify) {
        context.notify(loc.Get(app::strings::toast_driver_uri_empty));
      }
      return false;
    }
    const SanitizedUriResult sanitized = SanitizeUriCredentials(uri_text);
    const std::string effective_user =
        username.Get().text.empty() ? sanitized.username : username.Get().text;
    const std::string effective_password =
        password.Get().text.empty() ? sanitized.password : password.Get().text;
    const std::vector<ConnectionProfile>& existing_list = profiles.Get();
    const int idx_now = selected.Get();
    const ConnectionProfile prev_flags =
        (idx_now >= 0 && static_cast<std::size_t>(idx_now) < existing_list.size())
            ? existing_list[static_cast<std::size_t>(idx_now)]
            : ConnectionProfile{};
    ConnectionProfile profile{
        .name = name.Get().text.empty() ? UriDisplayName(sanitized.uri) : name.Get().text,
        .driver_path = driver_path,
        .uri = sanitized.uri,
        .username = effective_user,
        .password = effective_password,
        .remark = prev_flags.remark,
        .read_only = prev_flags.read_only,
        .production_mode = prev_flags.production_mode,
        .show_system_databases = prev_flags.show_system_databases,
    };
    std::vector<ConnectionProfile> list = profiles.Get();
    int index = selected.Get();
    if (index >= 0 && static_cast<std::size_t>(index) < list.size()) {
      list[static_cast<std::size_t>(index)] = profile;
    } else {
      list.push_back(profile);
      index = static_cast<int>(list.size()) - 1;
    }
    if (!PersistConnectionProfileList(config_db, context.profiles_path, list)) {
      if (context.notify) {
        context.notify(loc.Fmt(app::strings::toast_save_profiles_path_failed, context.profiles_path));
      }
      return false;
    }
    profiles = std::move(list);
    selected = index;
    name = TextEditingValue::FromText(profile.name);
    if (context.notify) {
      context.notify(loc.Fmt(app::strings::toast_profile_saved, profile.name));
    }
    return true;
  };

  const auto delete_profile = [context, config_db, profiles, selected, name, loc] {
    const int index = selected.Get();
    std::vector<ConnectionProfile> list = profiles.Get();
    if (index < 0 || static_cast<std::size_t>(index) >= list.size()) {
      return;
    }
    const auto position = list.begin() + index;
    const std::string removed = position->name.empty() ? position->uri : position->name;
    list.erase(position);
    if (!PersistConnectionProfileList(config_db, context.profiles_path, list)) {
      if (context.notify) {
        context.notify(loc.Fmt(app::strings::toast_delete_save_failed, context.profiles_path));
      }
      return;
    }
    profiles = std::move(list);
    selected = -1;
    name = TextEditingValue::FromText({});
    if (context.notify) {
      context.notify(loc.Fmt(app::strings::toast_profile_removed, removed));
    }
  };

  const auto connect = [context, dialog, profiles, selected, save_profile, driver, uri, username,
                        password, name] {
    if (!save_profile()) {
      return;
    }
    const std::vector<ConnectionProfile>& list_now = profiles.Get();
    const int sel_idx = selected.Get();
    const ConnectionProfile sel_prof =
        (sel_idx >= 0 && static_cast<std::size_t>(sel_idx) < list_now.size())
            ? list_now[static_cast<std::size_t>(sel_idx)]
            : ConnectionProfile{};
    const SanitizedUriResult sanitized = SanitizeUriCredentials(uri.Get().text);
    ConnectionRequest request{
        .driver_path = driver.Get().text,
        .uri = sanitized.uri,
        .username = username.Get().text.empty() ? sanitized.username : username.Get().text,
        .password = password.Get().text.empty() ? sanitized.password : password.Get().text,
        .title = name.Get().text.empty() ? UriDisplayName(sanitized.uri) : name.Get().text,
        .remark = sel_prof.remark,
        .read_only = sel_prof.read_only,
        .production_mode = sel_prof.production_mode,
        .show_system_databases = sel_prof.show_system_databases,
    };
    OpenConnection(context.app, context.workspace, context.tasks, std::move(request),
                   context.notify);
    dialog.Dismiss();
  };

  Views profile_rows;
  const auto& profile_list = profiles.Get();
  profile_rows.Reserve(profile_list.size());
  for (std::size_t index = 0; index < profile_list.size(); ++index) {
    const ConnectionProfile profile = profile_list[index];
    profile_rows.Add(
        NavItem(profile.name.empty() ? UriDisplayName(profile.uri) : profile.name,
                profile.uri, selected.Get() == static_cast<int>(index),
                [profiles, selected, driver, uri, username, password, name,
                 index] {
                  const auto& list = profiles.Get();
                  if (index >= list.size()) {
                    return;
                  }
                  const ConnectionProfile& value = list[index];
                  driver = TextEditingValue::FromText(value.driver_path);
                  uri = TextEditingValue::FromText(value.uri);
                  username = TextEditingValue::FromText(value.username);
                  password = TextEditingValue::FromText(value.password);
                  name = TextEditingValue::FromText(value.name);
                  selected = static_cast<int>(index);
                },
                {}, std::nullopt,
                DatabaseIcon(DetectDatabaseBrand(profile.driver_path, profile.uri), 18.0F))
            .Key("profile-" + std::to_string(index) + "-" + profile.uri));
  }

  Views driver_rows;
  const auto& driver_list = drivers.Get();
  driver_rows.Reserve(driver_list.size());
  for (const AdbcDriverInfo& info : driver_list) {
    const std::string& detail = info.detail.empty() ? info.path : info.detail;
    driver_rows.Add(
        NavItem(info.name, detail, driver.Get().text == info.path,
                [driver, info] { driver = TextEditingValue::FromText(info.path); }, {},
                std::nullopt, DatabaseIcon(DetectDatabaseBrand(info.path + " " + info.name), 18.0F))
            .Key("driver-" + info.path + "-" + detail));
  }

  View saved_panel = Column{
      Caption(UseString(app::strings::adbc_saved_profiles)),
      profile_rows.Size() == 0
          ? View(MutedText(UseString(app::strings::adbc_no_saved_profiles)))
          : View(ScrollView(Column{std::move(profile_rows)}.With(Spacing{2.0F}))
                     .With(Grow(1.0F))),
  }.With(Frame{.width = 220.0F}, Spacing{6.0F}, CrossAlign{CrossAxisAlignment::Stretch});

  View form_panel = Column{
      Row{
          Caption(UseString(app::strings::adbc_drivers_heading)),
          Spacer(),
          ActionButton(Button(app::strings::adbc_install_drivers_btn).OnClick([switch_tab]() mutable {
            switch_tab(1);
          })),
          ActionButton(Button(app::strings::action_refresh).OnClick([refresh_drivers,
                                                                     notify = context.notify, loc] {
            const std::size_t count = refresh_drivers();
            if (notify) {
              notify(loc.Fmt(app::strings::toast_drivers_found, count));
            }
          })),
      }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{6.0F}),
      driver_rows.Size() == 0
          ? View(MutedText(UseString(app::strings::adbc_no_drivers_found)))
          : View(ScrollView(Column{std::move(driver_rows)}.With(Spacing{2.0F}))
                     .With(Frame{.height = 140.0F})),
      TextField(driver)
          .Label(app::strings::adbc_driver_field_label)
          .Placeholder(app::strings::adbc_driver_field_placeholder)
          .Variant(TextFieldVariant::Outlined)
          .OnChanged([driver](const TextEditingValue& value) { driver = value; }),
      TextField(uri)
          .Label(app::strings::adbc_uri_field_label)
          .Placeholder(app::strings::adbc_uri_field_placeholder)
          .Variant(TextFieldVariant::Outlined)
          .OnChanged([uri](const TextEditingValue& value) { uri = value; }),
      Row{
          TextField(username)
              .Label(app::strings::adbc_user_field_label)
              .Variant(TextFieldVariant::Outlined)
              .OnChanged([username](const TextEditingValue& value) { username = value; })
              .With(Grow(1.0F)),
          PasswordField(password, app::strings::adbc_password_field_label)
              .With(Grow(1.0F)),
      }.With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch}),
      TextField(name)
          .Label(app::strings::adbc_name_field_label)
          .Placeholder(app::strings::adbc_name_placeholder)
          .Variant(TextFieldVariant::Outlined)
          .OnChanged([name](const TextEditingValue& value) { name = value; }),
  }.With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch}, Grow(1.0F));

  View manage_tab = Column{
      MutedText(UseString(app::strings::adbc_manage_hint)),
      Row{
          std::move(saved_panel),
          Divider(Axis::Vertical),
          std::move(form_panel),
      }.With(Grow(1.0F), Spacing{12.0F}, CrossAlign{CrossAxisAlignment::Stretch}),
  }.With(Spacing{10.0F}, CrossAlign{CrossAxisAlignment::Stretch}, Grow(1.0F));

  const auto& user_slugs = user_installed.Get();
  const auto& catalog_list = catalog.Get();
  const std::string filter_query = registry_filter.Get().text;
  const std::string active_install = installing_slug.Get();
  const bool busy = !active_install.empty();

  Views registry_rows;
  registry_rows.Reserve(catalog_list.size());
  for (const RegistryDriverInfo& item : catalog_list) {
    if (!filter_query.empty() &&
        !ContainsIgnoreCaseAscii(item.slug, filter_query) &&
        !ContainsIgnoreCaseAscii(item.name, filter_query) &&
        !ContainsIgnoreCaseAscii(item.description, filter_query)) {
      continue;
    }

    bool has_manifest = false;
    std::string installed_version;
    for (const AdbcDriverInfo& local : driver_list) {
      if (local.is_manifest && local.path == item.slug) {
        has_manifest = true;
        installed_version = local.version;
        break;
      }
    }
    const bool user_is_installed =
        std::ranges::find(user_slugs, item.slug) != user_slugs.end();
    const bool same_version =
        !installed_version.empty() &&
        StripLeadingV(installed_version) == StripLeadingV(item.version);

    std::string detail;
    const std::string ver_suffix =
        installed_version.empty() ? std::string{} : " " + installed_version;
    if (user_is_installed) {
      detail = UseString(app::strings::adbc_installed_prefix, ver_suffix) + " · ";
    } else if (has_manifest) {
      detail = UseString(app::strings::adbc_external_prefix, ver_suffix) + " · ";
    }
    detail += UseString(app::strings::adbc_available_version, item.version);
    if (!item.license.empty()) {
      detail += " · " + item.license;
    }
    if (!item.description.empty()) {
      detail += " — " + item.description;
    }

    View trailing;
    if (active_install == item.slug) {
      trailing = MutedText(UseString(app::strings::adbc_status_busy));
    } else if (user_is_installed) {
      trailing = Row{
          ActionButton(Button(app::strings::action_uninstall).OnClick([start_uninstall,
                                                                       slug = item.slug]() mutable {
                         start_uninstall(slug);
                       }),
                       ButtonTone::Danger)
              .With(Enabled{!busy}),
          ActionButton(Button(same_version ? app::strings::action_reinstall
                                           : app::strings::action_update)
                           .OnClick([start_install, item]() mutable {
                             start_install(item);
                           }))
              .With(Enabled{!busy}),
      }.With(Spacing{6.0F}, CrossAlign{CrossAxisAlignment::Center});
    } else {
      trailing = ActionButton(Button(app::strings::action_install).OnClick([start_install,
                                                                            item]() mutable {
                                start_install(item);
                              }),
                              ButtonTone::Primary)
                     .With(Enabled{!busy});
    }

    registry_rows.Add(
        NavItem(item.name + " (" + item.slug + ")", detail, false, {}, std::move(trailing),
                std::nullopt, DatabaseIcon(DetectDatabaseBrand(item.slug + " " + item.name), 20.0F))
            .Key("registry-" + item.slug));
  }

  View install_list_content;
  if (catalog_loading.Get() && catalog_list.empty()) {
    install_list_content = LoadingState(app::strings::adbc_catalog_loading);
  } else if (catalog_failed.Get() && catalog_list.empty()) {
    install_list_content = TableEmptyState(app::strings::adbc_catalog_failed);
  } else if (registry_rows.Size() == 0) {
    install_list_content = TableEmptyState(catalog_list.empty() ? app::strings::adbc_catalog_prompt
                                                                : app::strings::adbc_catalog_no_match);
  } else {
    const StringVariant overlay_label =
        busy ? (!install_status.Get().empty() ? StringVariant(install_status.Get())
                                              : StringVariant(app::strings::adbc_status_busy))
             : StringVariant(app::strings::adbc_catalog_loading);
    install_list_content =
        LoadingOverlay(ScrollView(Column{std::move(registry_rows)}.With(Spacing{4.0F})),
                       catalog_loading.Get() || busy, overlay_label)
            .With(Grow(1.0F));
  }

  const std::string user_dir = UserAdbcDriversDir();
  const std::string catalog_meta_text =
      user_dir.empty()
          ? UseString(app::strings::adbc_catalog_meta, AdbcRegistryBaseUrl(),
                      CurrentPlatformTuple())
          : UseString(app::strings::adbc_catalog_meta_with_dir, AdbcRegistryBaseUrl(),
                      CurrentPlatformTuple(), user_dir);
  View install_tab = Column{
      MutedText(catalog_meta_text),
      TextField(registry_filter)
          .Label(app::strings::adbc_filter_label)
          .Placeholder(app::strings::adbc_filter_placeholder)
          .Variant(TextFieldVariant::Outlined)
          .OnChanged([registry_filter](const TextEditingValue& value) {
            registry_filter = value;
          }),
      std::move(install_list_content),
  }.With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch}, Grow(1.0F));

  View header_actions;
  if (tab.Get() == 0) {
    header_actions = Row{
        ActionButton(Button(app::strings::action_close).OnClick([dialog] { dialog.Dismiss(); })),
        ActionButton(Button(app::strings::action_delete).OnClick(delete_profile),
                     ButtonTone::Danger)
            .With(Enabled{selected.Get() >= 0}),
        ActionButton(Button(app::strings::action_save).OnClick([save_profile] {
          (void)save_profile();
        })),
        ActionButton(Button(app::strings::action_connect).OnClick(connect), ButtonTone::Primary),
    }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{8.0F});
  } else {
    header_actions = Row{
        ActionButton(Button(app::strings::action_close).OnClick([dialog] { dialog.Dismiss(); })),
        ActionButton(Button(app::strings::adbc_refresh_catalog).OnClick([load_catalog]() mutable {
          load_catalog();
        })).With(Enabled{!catalog_loading.Get()}),
    }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{8.0F});
  }

  View body = Column{
      Row{
          Text(app::strings::adbc_title, TextRole::Title),
          SegmentedButton({app::strings::adbc_tab_connections, app::strings::adbc_tab_install},
                          tab.Get())
              .OnChanged([switch_tab](std::size_t index) mutable {
                switch_tab(index);
              }),
          Spacer(),
          std::move(header_actions),
      }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{10.0F}),
      IndexedPages({std::move(manage_tab), std::move(install_tab)}, tab.Get())
          .With(Grow(1.0F)),
  }.With(Spacing{10.0F}, CrossAlign{CrossAxisAlignment::Stretch},
         Frame{.width = 860.0F, .height = 620.0F});

  return DialogCard(std::move(body));
}

} // namespace dbpp
