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

ResolvedConnectionUris ResolveOrderedConnectionUris(
    const DatabasePreset& preset, std::string_view uri_field, std::string_view host,
    std::string_view port, std::string_view database, std::string_view extra_query) {
  ResolvedConnectionUris out;
  const std::string typed_uri = TrimmedSql(uri_field);
  if (preset.form_kind != ConnectionFormKind::HostPort) {
    out.primary_uri = typed_uri;
    return out;
  }
  const std::string trimmed_host = TrimmedSql(host);
  const std::string composed_uri =
      (!trimmed_host.empty() || typed_uri.empty())
          ? ComposePresetUri(preset, host, port, database, extra_query)
          : std::string{};
  if (!typed_uri.empty()) {
    out.primary_uri = typed_uri;
    if (!composed_uri.empty() && composed_uri != typed_uri) {
      out.fallback_uri = composed_uri;
    }
  } else {
    out.primary_uri = composed_uri;
  }
  return out;
}

[[huxerui::composable]] View ConnectionFormFields(
    const DatabasePreset& preset, const std::string& active_preset_placeholder,
    const ConnectionFormStates& form, const std::vector<AdbcDriverInfo>& driver_list,
    bool can_pick_file, std::function<void()> pick_local_file,
    std::function<void()> on_password_submit, StringVariant name_placeholder) {
  const ThemeSpec& spec = UseTheme();
  const SemanticColors& semantic = UseEnvironment<SemanticColors>();
  Views form_fields;

  form_fields.Add(
      SegmentedButton({app::strings::connect_mode_form, app::strings::connect_mode_uri},
                      form.input_mode.Get())
          .OnChanged([form](std::size_t idx) { form.input_mode = idx; }));

  if (form.input_mode.Get() == 1) {
    form_fields.Add(
        Column{
            Row{
                TintedIcon(app::images::shield, semantic.warning, 15.0F),
                Text(app::strings::connect_perm_section)
                    .Style(TextStyle{Font::System(font_size::kChip).WithWeight(FontWeight::SemiBold),
                                     spec.colors.on_surface}),
            }.With(Spacing{6.0F}, CrossAlign{CrossAxisAlignment::Center}),
            Switch(app::strings::connect_readonly_switch, form.read_only)
                .OnChanged([ro = form.read_only](bool v) { ro = v; }),
            MutedText(UseString(app::strings::connect_readonly_hint)),
            Switch(app::strings::connect_production_switch, form.production_mode)
                .OnChanged([prod = form.production_mode](bool v) { prod = v; }),
            MutedText(UseString(app::strings::connect_production_hint)),
            Switch(app::strings::connect_show_sysdb_switch, form.show_system_databases)
                .OnChanged([sysdb = form.show_system_databases](bool v) { sysdb = v; }),
            MutedText(UseString(app::strings::connect_show_sysdb_hint)),
        }.With(Spacing{6.0F}, CrossAlign{CrossAxisAlignment::Stretch},
               Padding{EdgeInsets::Symmetric(10.0F, 8.0F)}, CornerRadius{7.0F},
               Background{VisualFill{spec.colors.surface}},
               Border{semantic.border_subtle, 1.0F}));

    if (preset.form_kind != ConnectionFormKind::File &&
        preset.form_kind != ConnectionFormKind::Custom) {
      form_fields.Add(
          MonospaceField(
              TextField(form.driver)
                  .Label(app::strings::connect_driver_name_or_path_label)
                  .Variant(TextFieldVariant::Outlined)
                  .LineLimits(TextFieldLineLimits::SingleLine())
                  .OnChanged([driver = form.driver](const TextEditingValue& value) {
                    driver = value;
                  })));
    }
    return Column{std::move(form_fields)}
        .With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch});
  }

  const auto add_name_and_remark = [&] {
    form_fields.Add(
        Row{
            TextField(form.title)
                .Label(app::strings::connect_name_label)
                .Placeholder(std::move(name_placeholder))
                .Variant(TextFieldVariant::Outlined)
                .LineLimits(TextFieldLineLimits::SingleLine())
                .OnChanged([title = form.title](const TextEditingValue& value) { title = value; })
                .With(Grow(1.0F)),
            TextField(form.remark)
                .Label(app::strings::connect_remark_label)
                .Placeholder(app::strings::connect_remark_placeholder)
                .Variant(TextFieldVariant::Outlined)
                .LineLimits(TextFieldLineLimits::SingleLine())
                .OnChanged([remark = form.remark](const TextEditingValue& value) { remark = value; })
                .With(Grow(1.0F)),
        }.With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch}));
  };

  if (preset.form_kind == ConnectionFormKind::File) {
    form_fields.Add(
        Row{
            TextField(form.uri)
                .Label(app::strings::connect_file_label)
                .Placeholder(active_preset_placeholder)
                .Variant(TextFieldVariant::Outlined)
                .LineLimits(TextFieldLineLimits::SingleLine())
                .OnChanged([uri = form.uri](const TextEditingValue& value) { uri = value; })
                .With(Grow(1.0F)),
            ActionButton(Button(app::strings::connect_browse_btn).OnClick(std::move(pick_local_file)))
                .With(Enabled{can_pick_file}),
        }.With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Center}));
    add_name_and_remark();
    form_fields.Add(
        MonospaceField(
            TextField(form.driver)
                .Label(app::strings::connect_driver_label)
                .Variant(TextFieldVariant::Outlined)
                .LineLimits(TextFieldLineLimits::SingleLine())
                .OnChanged([driver = form.driver](const TextEditingValue& value) { driver = value; })));
    return Column{std::move(form_fields)}
        .With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch});
  }

  if (preset.form_kind == ConnectionFormKind::HostPort) {
    form_fields.Add(
        MonospaceField(
            TextField(form.uri)
                .Label(app::strings::connect_uri_optional_label)
                .Placeholder(active_preset_placeholder)
                .Variant(TextFieldVariant::Outlined)
                .LineLimits(TextFieldLineLimits::SingleLine())
                .OnChanged([uri = form.uri](const TextEditingValue& value) { uri = value; })));
    form_fields.Add(MutedText(UseString(app::strings::connect_uri_priority_hint)));
    form_fields.Add(
        Row{
            TextField(form.host)
                .Label(app::strings::connect_host_label)
                .Placeholder(preset.default_host)
                .Variant(TextFieldVariant::Outlined)
                .LineLimits(TextFieldLineLimits::SingleLine())
                .OnChanged([host = form.host](const TextEditingValue& value) { host = value; })
                .With(Grow(2.0F)),
            TextField(form.port)
                .Label(app::strings::connect_port_label)
                .Placeholder(preset.default_port)
                .Variant(TextFieldVariant::Outlined)
                .LineLimits(TextFieldLineLimits::SingleLine())
                .OnChanged([port = form.port](const TextEditingValue& value) { port = value; })
                .With(Grow(1.0F)),
        }.With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch}));
    form_fields.Add(
        TextField(form.database)
            .Label(preset.id == "redis" ? app::strings::connect_redis_db_label
                                        : app::strings::connect_db_label)
            .Placeholder(preset.default_database)
            .Variant(TextFieldVariant::Outlined)
            .LineLimits(TextFieldLineLimits::SingleLine())
            .OnChanged([database = form.database](const TextEditingValue& value) { database = value; }));
    form_fields.Add(
        Row{
            TextField(form.username)
                .Label(preset.id == "redis" ? app::strings::connect_user_acl_label
                                            : app::strings::connect_user_label)
                .Variant(TextFieldVariant::Outlined)
                .LineLimits(TextFieldLineLimits::SingleLine())
                .OnChanged([username = form.username](const TextEditingValue& value) { username = value; })
                .With(Grow(1.0F)),
            PasswordField(form.password, app::strings::connect_password_opt_label,
                          on_password_submit)
                .With(Grow(1.0F)),
        }.With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch}));
    add_name_and_remark();
    const ResolvedConnectionUris resolved = ResolveOrderedConnectionUris(
        preset, form.uri.Get().text, form.host.Get().text, form.port.Get().text,
        form.database.Get().text, form.extra_query.Get());
    form_fields.Add(MutedText(UseString(app::strings::connect_uri_preview, resolved.primary_uri)));
    return Column{std::move(form_fields)}
        .With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch});
  }

  if (preset.form_kind == ConnectionFormKind::Uri) {
    form_fields.Add(
        MonospaceField(
            TextField(form.uri)
                .Label(app::strings::connect_uri_optional_label)
                .Placeholder(active_preset_placeholder)
                .Variant(TextFieldVariant::Outlined)
                .LineLimits(TextFieldLineLimits::SingleLine())
                .OnChanged([uri = form.uri](const TextEditingValue& value) { uri = value; })));
    form_fields.Add(
        Row{
            TextField(form.username)
                .Label(app::strings::connect_user_opt_label)
                .Variant(TextFieldVariant::Outlined)
                .LineLimits(TextFieldLineLimits::SingleLine())
                .OnChanged([username = form.username](const TextEditingValue& value) { username = value; })
                .With(Grow(1.0F)),
            PasswordField(form.password, app::strings::connect_password_opt_label,
                          on_password_submit)
                .With(Grow(1.0F)),
        }.With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch}));
    add_name_and_remark();
    return Column{std::move(form_fields)}
        .With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch});
  }

  form_fields.Add(
      MonospaceField(
          TextField(form.uri)
              .Label(app::strings::connect_uri_optional_label)
              .Placeholder(active_preset_placeholder)
              .Variant(TextFieldVariant::Outlined)
              .LineLimits(TextFieldLineLimits::SingleLine())
              .OnChanged([uri = form.uri](const TextEditingValue& value) { uri = value; })));
  Views discovered_rows;
  discovered_rows.Reserve(driver_list.size());
  for (const AdbcDriverInfo& info : driver_list) {
    const std::string& detail = info.detail.empty() ? info.path : info.detail;
    discovered_rows.Add(
        NavItem(info.name, detail, form.driver.Get().text == info.path,
                [driver = form.driver, info] { driver = TextEditingValue::FromText(info.path); },
                {}, std::nullopt,
                DatabaseIcon(DetectDatabaseBrand(info.path + " " + info.name), 18.0F))
            .Key("custom-driver-" + info.path + "-" + detail));
  }
  if (discovered_rows.Size() > 0) {
    form_fields.Add(Caption(UseString(app::strings::connect_discovered_drivers)));
    form_fields.Add(
        ScrollView(Column{std::move(discovered_rows)}.With(Spacing{2.0F}))
            .With(Frame{.height = 100.0F}));
  }
  form_fields.Add(
      MonospaceField(
          TextField(form.driver)
              .Label(app::strings::connect_driver_custom_label)
              .Placeholder(app::strings::connect_driver_custom_placeholder)
              .Variant(TextFieldVariant::Outlined)
              .LineLimits(TextFieldLineLimits::SingleLine())
              .OnChanged([driver = form.driver](const TextEditingValue& value) { driver = value; })));
  form_fields.Add(
      Row{
          TextField(form.username)
              .Label(app::strings::connect_user_opt_label)
              .Variant(TextFieldVariant::Outlined)
              .LineLimits(TextFieldLineLimits::SingleLine())
              .OnChanged([username = form.username](const TextEditingValue& value) { username = value; })
              .With(Grow(1.0F)),
          PasswordField(form.password, app::strings::connect_password_opt_label,
                        std::move(on_password_submit))
              .With(Grow(1.0F)),
      }.With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch}));
  add_name_and_remark();
  return Column{std::move(form_fields)}
      .With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch});
}

[[huxerui::composable]] View EditConnectionProfileDialog(
    DialogContext dialog, std::optional<ConnectionProfile> original_profile,
    ConnectionProfile initial_values, std::optional<std::uint64_t> active_session_id,
    State<AppModel> app, State<WorkspaceModel> workspace,
    State<std::vector<ConnectionProfile>> profiles, std::string profiles_path, TaskScope tasks,
    NotifyFn notify) {
  const auto config_db = UseService<ConfigDbService>();
  const auto picker = UseService<FilePicker>();
  const LocalizedCatalog loc{UseService<Resources>(), UseService<SettingsModel>()};
  auto dialog_tasks = UseTaskScope();

  const DatabasePreset initial_preset =
      ResolvePresetForProfile(initial_values.driver_path, initial_values.uri);
  const DecomposedPresetUri initial_parts =
      DecomposePresetUri(initial_preset, initial_values.uri);
  const std::string init_user =
      !initial_values.username.empty() ? initial_values.username : initial_parts.username;
  const std::string init_pass =
      !initial_values.password.empty() ? initial_values.password : initial_parts.password;

  ConnectionFormStates form{
      .driver_slug = UseState(initial_preset.id),
      .driver = UseState(TextEditingValue::FromText(initial_values.driver_path)),
      .uri = UseState(TextEditingValue::FromText(
          (initial_preset.form_kind == ConnectionFormKind::HostPort && initial_parts.parsed)
              ? std::string{}
              : initial_values.uri)),
      .host = UseState(TextEditingValue::FromText(
          initial_parts.parsed ? initial_parts.host : initial_preset.default_host)),
      .port = UseState(TextEditingValue::FromText(
          !initial_parts.port.empty() ? initial_parts.port : initial_preset.default_port)),
      .database = UseState(TextEditingValue::FromText(
          initial_parts.parsed ? initial_parts.database : initial_preset.default_database)),
      .username = UseState(TextEditingValue::FromText(init_user)),
      .password = UseState(TextEditingValue::FromText(init_pass)),
      .title = UseState(TextEditingValue::FromText(initial_values.name)),
      .remark = UseState(TextEditingValue::FromText(initial_values.remark)),
      .read_only = UseState(initial_values.read_only),
      .production_mode = UseState(initial_values.production_mode),
      .show_system_databases = UseState(initial_values.show_system_databases),
      .extra_query = UseState(initial_parts.extra_query),
      .input_mode = UseState<std::size_t>(0),
  };

  auto drivers = UseState(std::vector<AdbcDriverInfo>{});
  Lifecycle([drivers] {
    drivers = DiscoverAdbcDrivers();
    return [] {};
  });

  const LocalizedPresetStrings preset_strings = DatabasePresetStrings(initial_preset);
  const std::string preset_placeholder = preset_strings.uri_placeholder
                                             ? UseString(*preset_strings.uri_placeholder)
                                             : initial_preset.uri_placeholder;

  const auto pick_local_file = [picker, dialog_tasks, initial_preset, preset_strings, form, notify,
                                loc]() {
    if (!picker || !picker->CanOpenFiles()) {
      if (notify) {
        notify(loc.Get(app::strings::connect_picker_unsupported));
      }
      return;
    }
    const std::string filter_label =
        loc.Fmt(app::strings::connect_picker_filter, loc.Get(preset_strings.name));
    dialog_tasks.Launch([picker, filter_label, extensions = initial_preset.file_extensions, form,
                         notify, loc]() -> Task<void> {
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
      form.uri = TextEditingValue::FromText(file->Path());
      if (form.title.Get().text.empty()) {
        form.title = TextEditingValue::FromText(file->Name());
      }
    });
  };

  const auto persist_profile = [original_profile, initial_preset, form, profiles, profiles_path,
                                config_db, loc, notify](ConnectionProfile& out_profile,
                                                        std::string& fallback_uri_out) {
    const std::string driver_text = TrimmedSql(form.driver.Get().text);
    const ResolvedConnectionUris resolved = ResolveOrderedConnectionUris(
        initial_preset, form.uri.Get().text, form.host.Get().text, form.port.Get().text,
        form.database.Get().text, form.extra_query.Get());
    const std::string uri_raw = resolved.primary_uri;
    fallback_uri_out = SanitizeUriCredentials(resolved.fallback_uri).uri;
    if (driver_text.empty() || uri_raw.empty()) {
      if (notify) {
        notify(loc.Get(app::strings::toast_driver_uri_required));
      }
      return false;
    }
    const SanitizedUriResult sanitized = SanitizeUriCredentials(uri_raw);
    const std::string title_text = TrimmedSql(form.title.Get().text);
    const std::string user_text =
        form.username.Get().text.empty() ? sanitized.username : form.username.Get().text;
    const std::string pass_text =
        form.password.Get().text.empty() ? sanitized.password : form.password.Get().text;
    out_profile = ConnectionProfile{
        .name = title_text.empty() ? UriDisplayName(sanitized.uri) : title_text,
        .driver_path = driver_text,
        .uri = sanitized.uri,
        .username = user_text,
        .password = pass_text,
        .remark = TrimmedSql(form.remark.Get().text),
        .read_only = form.read_only.Get(),
        .production_mode = form.production_mode.Get(),
        .show_system_databases = form.show_system_databases.Get(),
    };

    if (!profiles_path.empty()) {
      std::vector<ConnectionProfile> list = profiles.Get();
      bool replaced = false;
      if (original_profile.has_value()) {
        if (auto it = std::ranges::find(list, *original_profile); it != list.end()) {
          *it = out_profile;
          replaced = true;
        } else {
          for (ConnectionProfile& existing : list) {
            if (existing.name == original_profile->name &&
                existing.uri == original_profile->uri) {
              existing = out_profile;
              replaced = true;
              break;
            }
          }
        }
      }
      if (!replaced) {
        for (ConnectionProfile& existing : list) {
          if (existing.name == out_profile.name) {
            existing = out_profile;
            replaced = true;
            break;
          }
        }
      }
      if (!replaced) {
        list.push_back(out_profile);
      }
      if (!PersistConnectionProfileList(config_db, profiles_path, list)) {
        if (notify) {
          notify(loc.Fmt(app::strings::toast_save_profiles_path_failed, profiles_path));
        }
        return false;
      }
      profiles = std::move(list);
    }
    return true;
  };

  const auto save_only = [dialog, persist_profile, original_profile, active_session_id, app, loc,
                          notify] {
    ConnectionProfile saved;
    std::string unused_fallback;
    if (!persist_profile(saved, unused_fallback)) {
      return;
    }
    app.Update([&original_profile, &saved, active_session_id](AppModel& model) {
      if (original_profile.has_value()) {
        model.failed_connections.erase(
            ConnectionFailureKey(original_profile->driver_path, original_profile->uri));
      }
      model.failed_connections.erase(ConnectionFailureKey(saved.driver_path, saved.uri));
      if (active_session_id.has_value()) {
        for (DbSession& live : model.sessions) {
          if (live.id == *active_session_id) {
            live.name = saved.name;
            live.remark = saved.remark;
            live.show_system_databases = saved.show_system_databases;
            live.production_mode = saved.production_mode;
            if (live.session) {
              live.session->remark = saved.remark;
              live.session->show_system_databases = saved.show_system_databases;
              live.session->production_mode = saved.production_mode;
            }
            ++model.revision;
            break;
          }
        }
      }
    });
    dialog.Dismiss();
    if (notify) {
      notify(loc.Fmt(app::strings::toast_profile_updated, saved.name));
    }
  };

  const auto save_and_connect = [dialog, persist_profile, active_session_id, app, workspace,
                                 tasks, notify] {
    ConnectionProfile saved;
    std::string fallback_uri;
    if (!persist_profile(saved, fallback_uri)) {
      return;
    }
    if (active_session_id.has_value()) {
      CloseSession(app, workspace, tasks, *active_session_id, {});
    }
    ConnectionRequest request{
        .driver_path = saved.driver_path,
        .uri = saved.uri,
        .fallback_uri = std::move(fallback_uri),
        .username = saved.username,
        .password = saved.password,
        .title = saved.name,
        .remark = saved.remark,
        .read_only = saved.read_only,
        .production_mode = saved.production_mode,
        .show_system_databases = saved.show_system_databases,
    };
    OpenConnection(app, workspace, tasks, std::move(request), notify);
    dialog.Dismiss();
  };

  const DatabaseBrand icon_brand =
      DetectDatabaseBrand(form.driver.Get().text, form.uri.Get().text);
  return DialogCard(Column{
      Row{
          DatabaseIcon(icon_brand, 20.0F),
          Text(app::strings::edit_conn_title, TextRole::Title),
          Spacer(),
          ActionButton(Button(app::strings::action_cancel).OnClick([dialog] { dialog.Dismiss(); })),
          ActionButton(Button(app::strings::action_save).OnClick(save_only)),
          ActionButton(Button(active_session_id.has_value()
                                  ? app::strings::edit_conn_save_reconnect
                                  : app::strings::edit_conn_save_connect)
                           .OnClick(save_and_connect),
                       ButtonTone::Primary),
      }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{8.0F}),
      ScrollView(
          ConnectionFormFields(
              initial_preset, preset_placeholder, form, drivers.Get(),
              picker && picker->CanOpenFiles(), pick_local_file, save_and_connect,
              app::strings::edit_conn_name_placeholder))
          .With(Grow(1.0F), Frame{.max_height = 420.0F}),
  }.With(Spacing{10.0F}, CrossAlign{CrossAxisAlignment::Stretch})
                    .With(Frame{.width = 580.0F}));
}

[[huxerui::composable]] View DeleteSavedProfileDialog(
    DialogContext dialog, ConnectionProfile profile, std::string display_name,
    State<std::vector<ConnectionProfile>> profiles, std::string profiles_path, NotifyFn notify) {
  const auto config_db = UseService<ConfigDbService>();
  const LocalizedCatalog loc{UseService<Resources>(), UseService<SettingsModel>()};
  const auto confirm = [dialog, profile, display_name, profiles, profiles_path, config_db,
                        notify, loc] {
    std::vector<ConnectionProfile> updated = profiles.Get();
    const auto it = std::ranges::find(updated, profile);
    if (it != updated.end()) {
      updated.erase(it);
    }
    if (!PersistConnectionProfileList(config_db, profiles_path, updated)) {
      if (notify) {
        notify(loc.Get(app::strings::toast_save_profiles_failed));
      }
      return;
    }
    profiles = std::move(updated);
    dialog.Dismiss();
    if (notify) {
      notify(loc.Fmt(app::strings::toast_delete_profile_done, display_name));
    }
  };

  return DialogCard(Column{
      Row{
          Text(app::strings::delete_profile_title, TextRole::Title),
          Spacer(),
          ActionButton(Button(app::strings::action_cancel).OnClick([dialog] { dialog.Dismiss(); })),
          ActionButton(Button(app::strings::action_delete).OnClick(confirm),
                       ButtonTone::DangerSolid),
      }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{8.0F}),
      MutedText(UseString(app::strings::delete_profile_body, display_name, profile.uri)),
  }.With(Spacing{10.0F}, CrossAlign{CrossAxisAlignment::Stretch})
                    .With(Frame{.width = 480.0F}));
}

[[huxerui::composable]] View ConfirmCloseTabsDialog(DialogContext dialog, State<AppModel> app,
                                                     State<WorkspaceModel> workspace,
                                                     std::vector<std::uint64_t> tab_ids,
                                                     std::size_t draft_count) {
  return DialogCard(Column{
      Row{
          Text(app::strings::close_tabs_title, TextRole::Title),
          Spacer(),
          ActionButton(Button(app::strings::action_cancel).OnClick([dialog] { dialog.Dismiss(); })),
          ActionButton(
              Button(app::strings::close_tabs_confirm)
                  .OnClick([app, workspace, tab_ids = std::move(tab_ids), dialog] {
                    CloseWorkspaceTabs(app, workspace, tab_ids);
                    dialog.Dismiss();
                  }),
              ButtonTone::DangerSolid),
      }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{8.0F}),
      MutedText(UseString(app::strings::close_tabs_message, draft_count)),
  }.With(Spacing{10.0F}, CrossAlign{CrossAxisAlignment::Stretch})
                    .With(Frame{.width = 480.0F}));
}

[[huxerui::composable]] View CloseConfirmDialog(DialogContext dialog,
                                                std::shared_ptr<TrayRuntime> tray_runtime,
                                                State<bool> close_dialog_open) {
  return DialogCard(Column{
      Row{
          Text(app::strings::close_app_title, TextRole::Title),
          Spacer(),
          ActionButton(Button(app::strings::action_cancel).OnClick([dialog, close_dialog_open] {
            close_dialog_open = false;
            dialog.Dismiss();
          })),
          ActionButton(Button(app::strings::action_quit)
                           .OnClick([dialog, tray_runtime, close_dialog_open] {
                             close_dialog_open = false;
                             dialog.Dismiss();
                             tray_runtime->RequestExit();
                           }),
                       ButtonTone::DangerSolid),
      }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{8.0F}),
      MutedText(UseString(app::strings::close_app_message)),
  }.With(Spacing{12.0F}, CrossAlign{CrossAxisAlignment::Stretch})
                    .With(Frame{.width = 480.0F}));
}

[[huxerui::composable]] View ProductionWriteConfirmDialog(
    DialogContext dialog, std::string connection_name, std::string action_summary,
    std::function<void()> on_confirm) {
  const ThemeSpec& spec = UseTheme();
  const SemanticColors& semantic = UseEnvironment<SemanticColors>();
  return DialogCard(Column{
      Row{
          TintedIcon(app::images::shield, semantic.warning, 20.0F),
          Text(app::strings::prod_confirm_title, TextRole::Title),
          Spacer(),
          ActionButton(Button(app::strings::action_cancel).OnClick([dialog] { dialog.Dismiss(); })),
          ActionButton(Button(app::strings::prod_confirm_run).OnClick([dialog, on_confirm] {
                         dialog.Dismiss();
                         if (on_confirm) {
                           on_confirm();
                         }
                       }),
                       ButtonTone::DangerSolid),
      }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{8.0F}),
      Text(UseString(app::strings::prod_confirm_body, connection_name)),
      action_summary.empty()
          ? View{}
          : Text(std::move(action_summary))
                .Style(TextStyle{Font::Monospace(12.0F), spec.colors.on_surface_variant})
                .With(Padding{EdgeInsets::Symmetric(10.0F, 8.0F)}, CornerRadius{6.0F},
                      Background{VisualFill{spec.colors.surface}},
                      Border{semantic.warning, 1.0F}),
  }.With(Spacing{12.0F}, CrossAlign{CrossAxisAlignment::Stretch})
                    .With(Frame{.width = 540.0F}));
}

} // namespace dbpp
