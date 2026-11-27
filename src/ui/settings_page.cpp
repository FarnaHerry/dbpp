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

[[huxerui::composable]] View AllTabsPopup(
    PopupContext popup, State<AppModel> app, State<WorkspaceModel> workspace,
    std::function<void(std::vector<std::uint64_t>)> request_close_tabs) {
  const ThemeSpec& spec = UseTheme();
  const SemanticColors& semantic = UseEnvironment<SemanticColors>();
  auto search = UseState(TextEditingValue{});
  const WorkspaceModel& ws = workspace.Get();
  const AppModel& app_model = app.Get();
  const std::string query = TrimmedSql(search.Get().text);

  TextField search_field =
      TextField(search)
          .Placeholder(app::strings::tabs_list_search_ph)
          .Variant(TextFieldVariant::Outlined)
          .LineLimits(TextFieldLineLimits::SingleLine())
          .LeadingIcon(app::images::search)
          .OnChanged([search](const TextEditingValue& val) { search = val; });
  if (!query.empty()) {
    search_field =
        std::move(search_field)
            .TrailingIcon(app::images::close, app::strings::action_clear)
            .OnTrailingIconClick([search] { search = TextEditingValue{}; });
  }

  Views items;
  items.Reserve(ws.tabs.size());
  for (const WorkspaceTab& t : ws.tabs) {
    if (!query.empty() && !ContainsIgnoreCaseAscii(t.title, query) &&
        !ContainsIgnoreCaseAscii(t.session_name, query) &&
        !ContainsIgnoreCaseAscii(t.object_name, query)) {
      continue;
    }
    const std::uint64_t tid = t.id;
    const bool is_active = tid == ws.active_tab_id;
    bool is_redis_tab = false;
    if (t.kind == WorkspaceTabKind::Table) {
      for (const DbSession& open : app_model.sessions) {
        if (open.id == t.session_id) {
          is_redis_tab = open.dialect == Dialect::Redis;
          break;
        }
      }
    }
    const Color icon_color =
        t.kind == WorkspaceTabKind::Settings
            ? (is_active ? spec.colors.primary : spec.colors.on_surface_variant)
            : (t.kind == WorkspaceTabKind::Sql
                   ? semantic.accent_sql
                   : (t.kind == WorkspaceTabKind::TableDesign
                          ? semantic.accent_table
                          : (is_redis_tab
                                 ? semantic.accent_database
                                 : (t.is_view ? semantic.accent_view : semantic.accent_table))));
    const ImageResource& icon_res =
        t.kind == WorkspaceTabKind::Settings
            ? app::images::settings
            : (t.kind == WorkspaceTabKind::Sql
                   ? app::images::sql_console
                   : (t.kind == WorkspaceTabKind::TableDesign
                          ? app::images::edit
                          : (is_redis_tab
                                 ? app::images::database
                                 : (t.is_view ? app::images::view : app::images::table))));
    const std::string detail =
        t.kind == WorkspaceTabKind::Settings
            ? std::string{}
            : (app_model.sessions.size() > 1 ? t.session_name : std::string{});
    const bool dirty = t.kind == WorkspaceTabKind::Table && !t.table.pending_inserts.empty();
    const std::string label = dirty ? ("● " + t.title) : t.title;

    View close_btn =
        TintedIcon(app::images::close, spec.colors.on_surface_variant, 12.0F)
            .With(Padding{EdgeInsets::Symmetric(4.0F, 4.0F)}, CornerRadius{4.0F})
            .OnClick([request_close_tabs, tid] { request_close_tabs({tid}); });

    items.Add(NavItem(label, detail, is_active,
                      [app, workspace, tid, popup] {
                        ActivateWorkspaceTab(app, workspace, tid);
                        popup.Dismiss();
                      },
                      std::move(close_btn), std::nullopt,
                      TintedIcon(icon_res, icon_color, 15.0F))
                  .Key("popup-tab-" + std::to_string(tid)));
  }

  View list_body =
      items.Size() == 0
          ? View(MutedText(UseString(app::strings::tabs_list_empty))
                     .With(Padding{EdgeInsets::Symmetric(8.0F, 12.0F)}))
          : View(ScrollView(Column{std::move(items)}
                                .With(Spacing{2.0F}, CrossAlign{CrossAxisAlignment::Stretch}))
                     .With(Frame{.max_height = 320.0F}));

  return Column{
      Row{
          Caption(UseString(app::strings::tabs_list_count, ws.tabs.size())),
          Spacer(),
      }.With(CrossAlign{CrossAxisAlignment::Center}),
      std::move(search_field),
      std::move(list_body),
  }.With(Frame{.width = 320.0F}, Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch},
         Background{VisualFill{spec.colors.surface_container_highest}}, CornerRadius{10.0F},
         Border{semantic.border_subtle, 1.0F}, Padding{EdgeInsets::Symmetric(12.0F, 12.0F)});
}

[[huxerui::composable]] View SettingsPage(NotifyFn notify,
                                          std::function<void(std::int64_t)> on_page_size_applied) {
  const auto settings = UseService<SettingsModel>();
  const auto mcp_service = UseService<McpServerService>();
  const LocalizedCatalog loc{UseService<Resources>(), settings};
  const ThemeSpec& spec = UseTheme();
  const SemanticColors& semantic = UseEnvironment<SemanticColors>();
  const auto clipboard = UseApplication().Clipboard();
  auto tasks = UseTaskScope();
  auto transition = UseSceneTransition();
  auto section = UseState<int>(0);
  auto mcp_host_input = UseState(TextEditingValue::FromText(settings->mcp_host.Get()));
  auto mcp_port_input =
      UseState(TextEditingValue::FromText(std::to_string(settings->mcp_port.Get())));
  const std::int64_t persisted_page_size = settings->default_page_size.Get();
  auto page_size_input = UseState(TextEditingValue::FromText(std::to_string(persisted_page_size)));
  Lifecycle(
      [page_size_input, persisted_page_size] {
        const std::string expected = std::to_string(persisted_page_size);
        if (page_size_input.Get().text != expected) {
          page_size_input = TextEditingValue::FromText(expected);
        }
        return [] {};
      },
      persisted_page_size);
  // 动画互斥旗标放 shared_ptr 里改：不经过 State 更新，防抖期间不触发重组。
  struct ThemeAnimationFlag {
    bool animating = false;
  };
  auto animating = UseState(std::make_shared<ThemeAnimationFlag>());
  const bool system_dark = UseSystemThemeBrightness() == ThemeBrightness::Dark;
  const AppThemeConfig theme_config = settings->ThemeConfig();
  const LanguageMode language_mode = settings->language.Get();

  const auto copy_text = [clipboard, notify, loc](std::string text, StringResource label_res) {
    if (clipboard && clipboard->SupportsSynchronousText() && clipboard->WriteText(text)) {
      if (notify) {
        notify(loc.Fmt(app::strings::toast_copied, loc.Get(label_res)));
      }
    } else if (notify) {
      notify(loc.Get(app::strings::toast_clipboard_unsupported));
    }
  };

  // 明暗真正变化时使用圆形扩散过渡（参考 Clash-Flux），并将偏好持久化到 dbpp.sqlite3。
  const auto apply_mode = [settings, notify, transition, tasks, animating, system_dark, loc,
                           current_mode = theme_config.mode,
                           motion = spec.motion](ThemeMode next_mode) {
    if (animating.Get()->animating) {
      return;
    }
    const bool current_dark = ResolveIsDark(current_mode, system_dark);
    const bool target_dark = ResolveIsDark(next_mode, system_dark);
    const auto mutation = [settings, notify, next_mode, loc] {
      if (!settings->SetThemeMode(next_mode) && notify) {
        notify(loc.Get(app::strings::toast_theme_save_failed));
      }
    };
    if (current_dark == target_dark || motion.reduced_motion) {
      mutation();
      return;
    }
    animating.Get()->animating = true;
    tasks.Launch([animating, duration = motion.slow + motion.fast]() -> Task<void> {
      co_await Delay(std::chrono::duration<double>{duration});
      animating.Get()->animating = false;
    });
    const TransitionSpec reveal{CircularRevealTransition{}, TweenSpec{motion.slow}};
    transition.RunFromCurrentInteraction(current_dark ? reveal.Reversed() : reveal,
                                         std::move(mutation));
  };

  const int active_section = section.Get();
  View left_nav = Column{
      NavItem(UseString(app::strings::settings_nav_appearance), {}, active_section == 0,
              [section] { section = 0; }, {}, std::nullopt,
              TintedIcon(app::images::palette,
                         active_section == 0 ? spec.colors.primary : spec.colors.on_surface_variant,
                         16.0F)),
      NavItem(UseString(app::strings::settings_nav_mcp), {}, active_section == 1,
              [section] { section = 1; }, {}, std::nullopt,
              TintedIcon(app::images::mcp,
                         active_section == 1 ? spec.colors.primary : spec.colors.on_surface_variant,
                         16.0F)),
      NavItem(UseString(app::strings::settings_nav_about), {}, active_section == 2,
              [section] { section = 2; }, {}, std::nullopt,
              TintedIcon(app::images::info,
                         active_section == 2 ? spec.colors.primary : spec.colors.on_surface_variant,
                         16.0F)),
      Spacer(),
  }.With(Frame{.width = 188.0F}, Padding{EdgeInsets::Symmetric(8.0F, 8.0F)}, Spacing{4.0F},
         CrossAlign{CrossAxisAlignment::Stretch});

  View right_pane;
  if (active_section == 0) {
    const auto& presets = AvailableThemePresets();
    const auto make_preset_card = [settings, notify, &spec, &semantic, loc,
                                   active_preset = theme_config.preset](
                                      const ThemePresetMeta& meta) {
      const bool selected = meta.preset == active_preset;
      const Color title_color =
          selected ? spec.colors.on_primary_container : spec.colors.on_surface;
      const Color sub_color = selected ? WithAlpha(spec.colors.on_primary_container, 0.80F)
                                       : spec.colors.on_surface_variant;
      const auto [name_res, sub_res] = ThemePresetStrings(meta.preset);
      return Column{
          Spacer().With(Grow(0.0F), Frame{.height = 4.0F, .min_height = 4.0F, .max_height = 4.0F},
                        CornerRadius{2.0F}, Background{meta.swatch_bar}),
          Row{
              Text(name_res).Style(
                  TextStyle{Font::System(font_size::kBody).WithWeight(FontWeight::SemiBold),
                            title_color}),
              Spacer(),
              AccentDot(meta.swatch_a, 8.0F),
              AccentDot(meta.swatch_b, 8.0F),
              AccentDot(meta.swatch_c, 8.0F),
          }.With(Spacing{4.0F}, CrossAlign{CrossAxisAlignment::Center}),
          Text(sub_res)
              .Style(TextStyle{Font::System(font_size::kCaption), sub_color})
              .LineLimit(1)
              .Overflow(TextOverflow::Ellipsis),
      }
          .With(Grow(1.0F), Padding{EdgeInsets::Symmetric(12.0F, 10.0F)}, Spacing{6.0F},
                CrossAlign{CrossAxisAlignment::Stretch}, CornerRadius{8.0F},
                Background{VisualFill{selected ? spec.colors.primary_container : spec.colors.surface}},
                Border{selected ? spec.colors.primary : semantic.border_subtle,
                       selected ? 2.0F : 1.0F})
          .OnClick([settings, notify, preset = meta.preset, loc] {
            if (!settings->SetThemePreset(preset) && notify) {
              notify(loc.Get(app::strings::toast_theme_save_failed));
            }
          })
          .Key("theme-preset-" + std::string(ThemePresetKey(meta.preset)));
    };

    Views preset_rows;
    preset_rows.Reserve((presets.size() + 1) / 2);
    for (std::size_t i = 0; i < presets.size(); i += 2) {
      View left_card = make_preset_card(presets[i]);
      View right_slot =
          (i + 1 < presets.size()) ? make_preset_card(presets[i + 1])
                                   : View(Spacer().With(Grow(1.0F)));
      preset_rows.Add(Row{std::move(left_card), std::move(right_slot)}
                          .With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch})
                          .Key("theme-preset-row-" + std::to_string(i)));
    }

    right_pane = Column{
        Text(app::strings::settings_nav_appearance, TextRole::Title),
        Caption(UseString(app::strings::settings_section_language)),
        SegmentedButton({app::strings::settings_lang_system, app::strings::settings_lang_zh,
                         app::strings::settings_lang_en},
                        static_cast<std::size_t>(language_mode))
            .OnChanged([settings, notify, loc](std::size_t index) {
              const auto next = static_cast<LanguageMode>(index);
              const Locale platform =
                  loc.resources ? loc.resources->Configuration().locale : Locale::Default();
              SetActiveUiLanguage(ResolveUiLanguage(next, platform.LanguageTag()));
              if (!settings->SetLanguage(next) && notify) {
                notify(loc.Get(app::strings::toast_language_save_failed));
              }
            }),
        Caption(UseString(app::strings::settings_section_appearance)),
        SegmentedButton({app::strings::settings_theme_system, app::strings::settings_theme_dark,
                         app::strings::settings_theme_light},
                        static_cast<std::size_t>(theme_config.mode))
            .OnChanged([apply_mode](std::size_t index) {
              apply_mode(static_cast<ThemeMode>(index));
            }),
        Caption(UseString(app::strings::settings_section_palette)),
        Column{std::move(preset_rows)}.With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch}),
        Divider(),
        Caption(UseString(app::strings::settings_section_query)),
        [&]() -> View {
          const auto apply_page_size = [settings, page_size_input, notify, loc,
                                        on_page_size_applied] {
            const std::optional<std::int64_t> parsed =
                TryParsePageSize(page_size_input.Get().text);
            if (!parsed.has_value()) {
              if (notify) {
                notify(loc.Get(app::strings::table_page_size_invalid));
              }
              return;
            }
            (void)settings->SetDefaultPageSize(*parsed);
            page_size_input = TextEditingValue::FromText(std::to_string(*parsed));
            if (on_page_size_applied) {
              on_page_size_applied(*parsed);
            }
            if (notify) {
              notify(loc.Fmt(app::strings::toast_page_size_applied, *parsed));
            }
          };
          return Column{
              Row{
                  TextField(page_size_input)
                      .Label(app::strings::settings_default_page_size_label)
                      .Placeholder(app::strings::table_page_size_custom_ph)
                      .Variant(TextFieldVariant::Outlined)
                      .LineLimits(TextFieldLineLimits::SingleLine())
                      .OnChanged([page_size_input](const TextEditingValue& val) {
                        page_size_input = val;
                      })
                      .OnSubmitted(apply_page_size)
                      .With(Grow(1.0F)),
                  ActionButton(Button(app::strings::action_apply).OnClick(apply_page_size),
                               ButtonTone::Primary),
              }.With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Center}),
              MutedText(UseString(app::strings::settings_default_page_size_hint)),
          }.With(Spacing{6.0F}, CrossAlign{CrossAxisAlignment::Stretch});
        }(),
        Divider(),
        Caption(UseString(app::strings::settings_section_tray)),
        Switch(app::strings::settings_tray_enable, settings->tray_enabled)
            .OnChanged([settings, notify, loc](bool value) {
              if (!settings->SetTrayEnabled(value) && notify) {
                notify(loc.Get(app::strings::toast_tray_save_failed));
              }
            }),
        Switch(app::strings::settings_tray_lightweight, settings->lightweight)
            .OnChanged([settings, notify, loc](bool value) {
              if (!settings->SetLightweight(value) && notify) {
                notify(loc.Get(app::strings::toast_tray_save_failed));
              }
            })
            .With(Enabled{settings->tray_enabled.Get()}),
        MutedText(UseString(app::strings::settings_tray_lightweight_hint)),
        MutedText(UseString(app::strings::settings_footer)),
    }.With(Frame{.max_width = 640.0F}, Spacing{10.0F}, CrossAlign{CrossAxisAlignment::Stretch});
  } else if (active_section == 1) {
    const McpServerStatus mcp_status =
        mcp_service ? mcp_service->status.Get() : McpServerStatus{};
    const bool mcp_enabled = settings->mcp_enabled.Get();
    const std::string effective_host = NormalizeMcpHost(settings->mcp_host.Get());
    const std::uint16_t effective_port =
        mcp_status.running && mcp_status.bound_port != 0 ? mcp_status.bound_port
                                                         : settings->mcp_port.Get();
    const std::string endpoint_url = BuildMcpEndpointUrl(effective_host, effective_port);
    const std::string snippet = BuildMcpClientConfigJson(endpoint_url);

    const auto apply_mcp_config = [settings, mcp_service, mcp_host_input, mcp_port_input, notify,
                                   loc] {
      const std::string raw_port = TrimmedSql(mcp_port_input.Get().text);
      unsigned int parsed_port = 0;
      const auto [end, ec] =
          std::from_chars(raw_port.data(), raw_port.data() + raw_port.size(), parsed_port);
      if (raw_port.empty() || ec != std::errc{} || end != raw_port.data() + raw_port.size() ||
          parsed_port == 0 || parsed_port > 65535U) {
        if (notify) {
          notify(loc.Get(app::strings::toast_mcp_port_invalid));
        }
        return;
      }
      const std::string next_host = NormalizeMcpHost(mcp_host_input.Get().text);
      const std::uint16_t next_port = static_cast<std::uint16_t>(parsed_port);
      (void)settings->SetMcpHost(next_host);
      (void)settings->SetMcpPort(next_port);
      mcp_host_input = TextEditingValue::FromText(next_host);
      mcp_port_input = TextEditingValue::FromText(std::to_string(next_port));
      if (mcp_service) {
        const bool ok = mcp_service->ApplySettings(settings->mcp_enabled.Get(), next_host,
                                                   next_port, settings->mcp_read_only.Get());
        if (!ok) {
          const McpServerStatus st = mcp_service->status.Get();
          if (notify) {
            notify(loc.Fmt(app::strings::toast_mcp_failed, st.error));
          }
          return;
        }
      }
      if (notify) {
        notify(loc.Get(app::strings::toast_mcp_applied));
      }
    };

    View status_badge =
        mcp_status.running
            ? StatusBadge(UseString(app::strings::settings_mcp_status_running), semantic.success,
                          WithAlpha(semantic.success, 0.14F), WithAlpha(semantic.success, 0.40F))
            : StatusBadge(UseString(app::strings::settings_mcp_status_stopped),
                          spec.colors.on_surface_variant, spec.colors.surface_container,
                          semantic.border_subtle);

    Views mcp_items;
    mcp_items.Add(
        Row{
            TintedIcon(app::images::mcp, spec.colors.primary, 20.0F),
            Text(app::strings::settings_mcp_title, TextRole::Title),
            Spacer(),
            std::move(status_badge),
        }
            .With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Center}));
    mcp_items.Add(MutedText(UseString(app::strings::settings_mcp_desc)));
    mcp_items.Add(
        Switch(app::strings::settings_mcp_enable, settings->mcp_enabled)
            .OnChanged([settings, mcp_service, notify, loc](bool value) {
              (void)settings->SetMcpEnabled(value);
              if (mcp_service) {
                const bool ok = mcp_service->ApplySettings(value, settings->mcp_host.Get(),
                                                           settings->mcp_port.Get(),
                                                           settings->mcp_read_only.Get());
                if (!ok) {
                  const McpServerStatus st = mcp_service->status.Get();
                  if (notify) {
                    notify(loc.Fmt(app::strings::toast_mcp_failed, st.error));
                  }
                }
              }
            }));
    mcp_items.Add(
        Switch(app::strings::settings_mcp_readonly, settings->mcp_read_only)
            .OnChanged([settings, mcp_service, mcp_enabled, notify, loc](bool value) {
              (void)settings->SetMcpReadOnly(value);
              if (mcp_service && mcp_enabled) {
                const bool ok = mcp_service->ApplySettings(true, settings->mcp_host.Get(),
                                                           settings->mcp_port.Get(), value);
                if (!ok && notify) {
                  notify(loc.Fmt(app::strings::toast_mcp_failed, mcp_service->status.Get().error));
                }
              }
            }));
    mcp_items.Add(
        Row{
            TextField(mcp_host_input)
                .Label(app::strings::settings_mcp_host_label)
                .Variant(TextFieldVariant::Outlined)
                .LineLimits(TextFieldLineLimits::SingleLine())
                .OnChanged([mcp_host_input](const TextEditingValue& val) { mcp_host_input = val; })
                .With(Grow(1.0F)),
            TextField(mcp_port_input)
                .Label(app::strings::settings_mcp_port_label)
                .Variant(TextFieldVariant::Outlined)
                .LineLimits(TextFieldLineLimits::SingleLine())
                .OnChanged([mcp_port_input](const TextEditingValue& val) { mcp_port_input = val; })
                .With(Frame{.width = 140.0F}),
            ActionButton(Button(app::strings::settings_mcp_apply).OnClick(apply_mcp_config),
                         ButtonTone::Primary),
        }
            .With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Center}));
    if (!mcp_status.error.empty()) {
      mcp_items.Add(ErrorText(mcp_status.error).Key("settings-mcp-error"));
    }
    mcp_items.Add(Divider().Key("settings-mcp-div-1"));
    mcp_items.Add(
        Row{
            Caption(UseString(app::strings::settings_mcp_endpoint_label)),
            Spacer(),
            ActionButton(Button(app::strings::settings_mcp_copy_endpoint)
                             .OnClick([copy_text, endpoint_url] {
                               copy_text(endpoint_url,
                                         app::strings::settings_mcp_endpoint_copied_label);
                             })),
        }
            .With(CrossAlign{CrossAxisAlignment::Center})
            .Key("settings-mcp-endpoint-row"));
    mcp_items.Add(
        Text(endpoint_url)
            .Style(TextStyle{Font::Monospace(13.0F), spec.colors.on_surface})
            .With(Padding{EdgeInsets::Symmetric(10.0F, 8.0F)}, CornerRadius{6.0F},
                  Background{VisualFill{spec.colors.surface}},
                  Border{semantic.border_subtle, 1.0F})
            .Key("settings-mcp-endpoint-val"));
    mcp_items.Add(
        Row{
            Caption(UseString(app::strings::settings_mcp_snippet_label)),
            Spacer(),
            ActionButton(Button(app::strings::settings_mcp_copy_snippet)
                             .OnClick([copy_text, snippet] {
                               copy_text(snippet, app::strings::settings_mcp_snippet_copied_label);
                             })),
        }
            .With(CrossAlign{CrossAxisAlignment::Center})
            .Key("settings-mcp-snippet-row"));
    mcp_items.Add(
        Text(snippet)
            .Style(TextStyle{Font::Monospace(12.0F), spec.colors.on_surface_variant})
            .With(Padding{EdgeInsets::Symmetric(10.0F, 8.0F)}, CornerRadius{6.0F},
                  Background{VisualFill{spec.colors.surface}},
                  Border{semantic.border_subtle, 1.0F})
            .Key("settings-mcp-snippet-val"));
    mcp_items.Add(Divider().Key("settings-mcp-div-2"));
    mcp_items.Add(
        Caption(UseString(app::strings::settings_mcp_tools_heading)).Key("settings-mcp-tools-hdr"));
    mcp_items.Add(
        Column{
            MutedText(UseString(app::strings::settings_mcp_tool_profiles)),
            MutedText(UseString(app::strings::settings_mcp_tool_tables)),
            MutedText(UseString(app::strings::settings_mcp_tool_describe)),
            MutedText(UseString(app::strings::settings_mcp_tool_query)),
            MutedText(UseString(app::strings::settings_mcp_tool_info)),
            MutedText(UseString(app::strings::settings_mcp_tool_stats)),
            MutedText(UseString(app::strings::settings_mcp_tool_inspect_schema)),
        }
            .With(Spacing{4.0F}, CrossAlign{CrossAxisAlignment::Stretch})
            .Key("settings-mcp-tools-list"));

    right_pane = Column{std::move(mcp_items)}
                     .With(Frame{.max_width = 640.0F}, Spacing{10.0F},
                           CrossAlign{CrossAxisAlignment::Stretch});
  } else {
    const auto open_link = [notify, loc](std::string_view url) {
      if (!OpenExternalUrl(url) && notify) {
        notify(loc.Fmt(app::strings::toast_open_url_failed, std::string(url)));
      }
    };

    const auto link_row = [open_link, &spec](StringResource title_res, StringResource url_res) {
      const std::string url = UseString(url_res);
      return NavItem(UseString(title_res), url, false, [open_link, url] { open_link(url); },
                     TintedIcon(app::images::external_link, spec.colors.primary, 16.0F),
                     std::nullopt,
                     TintedIcon(app::images::github, spec.colors.on_surface, 18.0F));
    };

    right_pane = Column{
        Text(app::strings::settings_about_title, TextRole::Title),
        Row{
            Image(app::images::dbpp_logo)
                .Fit(ImageFit::Contain)
                .With(Frame{.width = 56.0F, .height = 56.0F, .min_width = 56.0F, .max_width = 56.0F,
                            .min_height = 56.0F, .max_height = 56.0F}),
            Column{
                Row{
                    Text("db++", TextRole::Title),
                    StatusBadge(UseString(app::strings::settings_about_version, DBPP_VERSION),
                                spec.colors.primary, spec.colors.primary_container,
                                WithAlpha(spec.colors.primary, 0.35F)),
                }.With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Center}),
                MutedText(UseString(app::strings::settings_about_summary)),
            }.With(Spacing{4.0F}, Grow(1.0F), CrossAlign{CrossAxisAlignment::Stretch}),
        }
            .With(Padding{EdgeInsets::Symmetric(14.0F, 12.0F)}, Spacing{12.0F},
                  CrossAlign{CrossAxisAlignment::Center}, CornerRadius{8.0F},
                  Background{VisualFill{spec.colors.surface}}, Border{semantic.border_subtle, 1.0F}),
        Divider(),
        Caption(UseString(app::strings::settings_about_links_heading)),
        link_row(app::strings::settings_about_link_dbpp, app::strings::settings_about_link_dbpp_sub),
        link_row(app::strings::settings_about_link_huxerui,
                 app::strings::settings_about_link_huxerui_sub),
        link_row(app::strings::settings_about_link_datatable,
                 app::strings::settings_about_link_datatable_sub),
        link_row(app::strings::settings_about_link_adbc, app::strings::settings_about_link_adbc_sub),
        MutedText(UseString(app::strings::settings_footer)),
    }.With(Frame{.max_width = 640.0F}, Spacing{10.0F}, CrossAlign{CrossAxisAlignment::Stretch});
  }

  return Row{
      std::move(left_nav),
      Divider(Axis::Vertical),
      ScrollView(Row{
                     std::move(right_pane).With(Grow(1.0F)),
                     Spacer(),
                 }.With(Padding{EdgeInsets::Symmetric(20.0F, 16.0F)}))
          .With(Grow(1.0F)),
  }.With(Grow(1.0F), CrossAlign{CrossAxisAlignment::Stretch});
}

} // namespace dbpp
