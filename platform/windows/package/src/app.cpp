#include <algorithm>
#include <filesystem>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <installer_resources.h>
#include <huxerui/huxerui.h>
#include <huxerui/windows/installer.h>

using namespace huxerui;
using namespace huxerui::windows;
namespace installer_strings = installer::strings;

namespace {

Color WithAlpha(Color color, float alpha) {
  color.alpha = alpha;
  return color;
}

LinearGradient TriGradient(Color a, Color b, Color c) {
  return LinearGradient{
      .start = {0.0F, 0.5F},
      .end = {1.0F, 0.5F},
      .stops =
          {
              GradientStop{.offset = 0.0F, .color = a},
              GradientStop{.offset = 0.52F, .color = b},
              GradientStop{.offset = 1.0F, .color = c},
          },
      .transform = {},
  };
}

RadialGradient SoftGlow(Color inner, Color mid) {
  return RadialGradient{
      .center = {0.5F, 0.28F},
      .radius = {0.68F, 0.52F},
      .stops =
          {
              GradientStop{.offset = 0.0F, .color = inner},
              GradientStop{.offset = 0.55F, .color = mid},
              GradientStop{.offset = 1.0F, .color = Color::Transparent()},
          },
      .transform = {},
  };
}

Indication TintedIndication(Color hover_fill, Color press_fill, Color focus_color) {
  return Indication{
      .focus =
          IndicationLayer{
              .fill = VisualFill{hover_fill},
              .border = Border{WithAlpha(focus_color, 0.55F), 1.0F},
              .corner_radii = {},
              .placement = IndicationPlacement::AboveContent,
              .enter = TweenSpec{.duration = 0.08, .easing = Easing::EaseOut},
              .exit = TweenSpec{.duration = 0.14, .easing = Easing::EaseOut},
              .foreground = {},
          },
      .hover =
          IndicationLayer{
              .fill = VisualFill{hover_fill},
              .border = {},
              .corner_radii = {},
              .placement = IndicationPlacement::AboveContent,
              .enter = TweenSpec{.duration = 0.08, .easing = Easing::EaseOut},
              .exit = TweenSpec{.duration = 0.14, .easing = Easing::EaseOut},
              .foreground = {},
          },
      .press =
          IndicationLayer{
              .fill = VisualFill{press_fill},
              .border = {},
              .corner_radii = {},
              .placement = IndicationPlacement::AboveContent,
              .enter = TweenSpec{.duration = 0.06, .easing = Easing::EaseOut},
              .exit = TweenSpec{.duration = 0.14, .easing = Easing::EaseOut},
              .foreground = {},
          },
      .ripple = {},
  };
}

// 与 db++ 默认「Gemini 星光」主题及语义 Token 保持一致（支持浅色/深色系统主题）。
struct InstallerPalette {
  ThemeSpec spec;
  bool dark = false;
  Color border_subtle;
  Color border_strong;
  Color hover_fill;
  Color press_fill;
  Color success;
  Color on_primary_button;
  VisualFill accent_gradient{Color::Transparent()};
  VisualFill primary_button_fill{Color::Transparent()};
  Color brand_bg;
  VisualFill brand_glow{Color::Transparent()};
  Color brand_eyebrow;
  Color brand_text;
  Color brand_secondary;
  Color brand_tile_bg;
  Color brand_tile_border;
};

InstallerPalette BuildInstallerPalette(bool dark) {
  InstallerPalette p;
  p.dark = dark;
  p.spec = dark ? MaterialDarkThemeSpec() : MaterialLightThemeSpec();
  auto& c = p.spec.colors;

  if (dark) {
    c.primary = Color::Rgb(106, 166, 255);
    c.on_primary = Color::Rgb(12, 20, 36);
    c.primary_container = Color::Rgb(30, 50, 86);
    c.on_primary_container = Color::Rgb(218, 234, 255);
    c.secondary = Color::Rgb(182, 146, 246);
    c.on_secondary = Color::Rgb(22, 14, 42);
    c.secondary_container = Color::Rgb(42, 34, 68);
    c.on_secondary_container = Color::Rgb(233, 224, 255);
    c.background = Color::Rgb(13, 17, 26);
    c.surface_container_low = Color::Rgb(19, 25, 38);
    c.surface = Color::Rgb(24, 31, 46);
    c.surface_container = Color::Rgb(30, 39, 58);
    c.surface_container_high = Color::Rgb(35, 46, 68);
    c.surface_container_highest = Color::Rgb(36, 47, 70);
    c.on_surface = Color::Rgb(236, 241, 250);
    c.on_surface_variant = Color::Rgb(148, 162, 192);
    c.outline = Color::Rgb(88, 106, 140);
    c.inverse_surface = Color::Rgb(236, 241, 250);
    c.inverse_on_surface = Color::Rgb(18, 24, 38);
    c.scrim = Color::Rgb(5, 8, 16, 0.58F);
    c.error = Color::Rgb(248, 113, 113);

    p.border_subtle = Color::Rgb(126, 148, 190, 0.24F);
    p.border_strong = Color::Rgb(148, 172, 218, 0.42F);
    p.hover_fill = Color::Rgb(128, 168, 255, 0.11F);
    p.press_fill = Color::Rgb(128, 168, 255, 0.18F);
    p.success = Color::Rgb(74, 222, 128);
    p.on_primary_button = Color::Rgb(255, 255, 255);
    p.accent_gradient = TriGradient(Color::Rgb(96, 165, 250), Color::Rgb(182, 146, 246),
                                    Color::Rgb(240, 138, 149));
    p.primary_button_fill = TriGradient(Color::Rgb(58, 120, 240), Color::Rgb(131, 92, 230),
                                        Color::Rgb(179, 100, 158));

    p.brand_bg = Color::Rgb(11, 15, 24);
    p.brand_glow = SoftGlow(Color::Rgb(106, 166, 255, 0.20F), Color::Rgb(182, 146, 246, 0.12F));
    p.brand_eyebrow = Color::Rgb(138, 180, 255);
    p.brand_text = Color::Rgb(236, 241, 250);
    p.brand_secondary = Color::Rgb(148, 162, 192);
    p.brand_tile_bg = Color::Rgb(255, 255, 255, 0.06F);
    p.brand_tile_border = Color::Rgb(126, 148, 190, 0.24F);
  } else {
    c.primary = Color::Rgb(66, 133, 244);
    c.on_primary = Color::Rgb(255, 255, 255);
    c.primary_container = Color::Rgb(226, 238, 255);
    c.on_primary_container = Color::Rgb(22, 68, 150);
    c.secondary = Color::Rgb(139, 92, 246);
    c.on_secondary = Color::Rgb(255, 255, 255);
    c.secondary_container = Color::Rgb(238, 232, 255);
    c.on_secondary_container = Color::Rgb(68, 38, 138);
    c.background = Color::Rgb(245, 247, 252);
    c.surface_container_low = Color::Rgb(236, 241, 250);
    c.surface = Color::Rgb(255, 255, 255);
    c.surface_container = Color::Rgb(232, 238, 249);
    c.surface_container_high = Color::Rgb(225, 233, 248);
    c.surface_container_highest = Color::Rgb(229, 236, 250);
    c.on_surface = Color::Rgb(24, 30, 46);
    c.on_surface_variant = Color::Rgb(92, 104, 130);
    c.outline = Color::Rgb(148, 163, 192);
    c.inverse_surface = Color::Rgb(28, 36, 54);
    c.inverse_on_surface = Color::Rgb(245, 247, 252);
    c.scrim = Color::Rgb(15, 20, 36, 0.42F);
    c.error = Color::Rgb(217, 63, 76);

    p.border_subtle = Color::Rgb(120, 138, 174, 0.28F);
    p.border_strong = Color::Rgb(96, 118, 164, 0.48F);
    p.hover_fill = Color::Rgb(66, 133, 244, 0.08F);
    p.press_fill = Color::Rgb(66, 133, 244, 0.14F);
    p.success = Color::Rgb(24, 148, 92);
    p.on_primary_button = Color::Rgb(255, 255, 255);
    p.accent_gradient = TriGradient(Color::Rgb(66, 133, 244), Color::Rgb(155, 114, 203),
                                    Color::Rgb(217, 101, 112));
    p.primary_button_fill = TriGradient(Color::Rgb(56, 125, 244), Color::Rgb(124, 99, 224),
                                        Color::Rgb(166, 106, 178));

    p.brand_bg = Color::Rgb(18, 24, 39);
    p.brand_glow = SoftGlow(Color::Rgb(66, 133, 244, 0.24F), Color::Rgb(155, 114, 203, 0.12F));
    p.brand_eyebrow = Color::Rgb(138, 180, 255);
    p.brand_text = Color::Rgb(244, 247, 253);
    p.brand_secondary = Color::Rgb(164, 178, 208);
    p.brand_tile_bg = Color::Rgb(255, 255, 255, 0.08F);
    p.brand_tile_border = Color::Rgb(255, 255, 255, 0.16F);
  }

  p.spec.interactions.indication = TintedIndication(p.hover_fill, p.press_fill, c.primary);
  p.spec.interactions.focus_ring.color = c.primary;
  return p;
}

ThemeDefinition InstallerThemeDefinition(const InstallerPalette& palette) {
  const ThemeSpec& spec = palette.spec;
  const bool dark = palette.dark;
  ThemeDefinition definition = MaterialThemeDefinition(spec);

  ButtonStyle primary_buttons = ButtonStyle::Default();
  primary_buttons.background = palette.primary_button_fill;
  primary_buttons.label_style =
      TextStyle{Font::System(spec.typography.label_large).WithWeight(FontWeight::SemiBold),
                palette.on_primary_button};
  primary_buttons.disabled_background = VisualFill{WithAlpha(spec.colors.on_surface, 0.10F)};
  primary_buttons.disabled_label = WithAlpha(spec.colors.on_surface, 0.38F);
  primary_buttons.padding = EdgeInsets::Symmetric(20.0F, 8.0F);
  primary_buttons.minimum_width = 76.0F;
  primary_buttons.minimum_height = 38.0F;
  primary_buttons.corner_radii = CornerRadii{8.0F};
  primary_buttons.indication =
      TintedIndication(Color::Rgb(255, 255, 255, 0.14F), Color::Rgb(255, 255, 255, 0.22F),
                       spec.colors.primary);
  definition.Set(primary_buttons);

  ProgressBarStyle bars = ProgressBarStyle::Default();
  bars.height = 5.0F;
  bars.track_color = WithAlpha(spec.colors.outline, 0.24F);
  bars.indicator_color = spec.colors.primary;
  bars.corner_radius = 2.5F;
  definition.Set(bars);

  DividerStyle dividers = DividerStyle::Default();
  dividers.color = palette.border_subtle;
  dividers.thickness = 1.0F;
  definition.Set(dividers);

  TextFieldStyle text_fields = TextFieldStyle::Default();
  text_fields.variant = TextFieldVariant::Outlined;
  text_fields.show_label = true;
  text_fields.outlined.background = VisualFill{spec.colors.surface};
  text_fields.outlined.disabled_background =
      VisualFill{WithAlpha(spec.colors.surface_container_low, 0.65F)};
  text_fields.outlined.border = palette.border_subtle;
  text_fields.outlined.hovered_border = palette.border_strong;
  text_fields.outlined.focused_border = spec.colors.primary;
  text_fields.outlined.disabled_border = WithAlpha(spec.colors.on_surface, 0.12F);
  text_fields.outlined.corner_radii = CornerRadii{8.0F};
  text_fields.outlined.minimum_height = 42.0F;
  text_fields.text_style = TextStyle{Font::System(spec.typography.body_medium), spec.colors.on_surface};
  text_fields.label_style =
      TextStyle{Font::System(spec.typography.body_medium), spec.colors.on_surface_variant};
  text_fields.floating_label_style =
      TextStyle{Font::System(spec.typography.body_small), spec.colors.on_surface_variant};
  text_fields.placeholder_style = TextStyle{
      Font::System(spec.typography.body_medium), WithAlpha(spec.colors.on_surface_variant, 0.72F)};
  text_fields.focused_label = spec.colors.primary;
  text_fields.error_label = spec.colors.error;
  text_fields.validation_error = spec.colors.error;
  text_fields.validation_text_style =
      TextStyle{Font::System(spec.typography.body_small), spec.colors.error};
  text_fields.selection = WithAlpha(spec.colors.primary, 0.24F);
  text_fields.caret = spec.colors.primary;
  text_fields.error_caret = spec.colors.error;
  text_fields.composition = spec.colors.secondary;
  definition.Set(text_fields);

  const Color soft_shadow = Color::Rgb(0, 0, 0, dark ? 0.44F : 0.15F);
  DialogStyle dialogs = DialogStyle::Default();
  dialogs.scrim = spec.colors.scrim;
  dialogs.background = VisualFill{spec.colors.surface_container_highest};
  dialogs.shadow = Shadow{soft_shadow, Point{0.0F, 8.0F}, 24.0F, 0.0F};
  dialogs.corner_radii = CornerRadii{10.0F};
  definition.Set(dialogs);

  return definition;
}

std::string InstallPathText(const std::filesystem::path& path) {
  const std::u8string value = path.u8string();
  return {value.begin(), value.end()};
}

std::optional<std::filesystem::path> ParseInstallPath(std::string_view value) {
  if (value.empty()) {
    return std::nullopt;
  }
  try {
    std::filesystem::path path(std::u8string(value.begin(), value.end()));
    return path.is_absolute() ? std::optional<std::filesystem::path>{path.lexically_normal()}
                              : std::nullopt;
  } catch (const std::filesystem::filesystem_error&) {
    return std::nullopt;
  }
}

View InstallerMark(const InstallerPalette& palette) {
  return Column {
    Image(installer::images::dbpp_logo)
        .Fit(ImageFit::Contain)
        .With(Grow(0.0F),
              Frame{.width = 48.0F, .height = 48.0F, .min_width = 48.0F, .max_width = 48.0F,
                    .min_height = 48.0F, .max_height = 48.0F}),
  }.With(
      Grow(0.0F),
      Frame{.width = 68.0F, .height = 68.0F, .min_width = 68.0F, .max_width = 68.0F,
            .min_height = 68.0F, .max_height = 68.0F},
      MainAlign(MainAxisAlignment::Center),
      CrossAlign(CrossAxisAlignment::Center),
      CornerRadius(18.0F),
      Background(VisualFill{palette.brand_tile_bg}),
      Border{palette.brand_tile_border, 1.0F});
}

View BrandPanel(const InstallerPalette& palette) {
  const ThemeSpec& theme = palette.spec;
  return Column {
    Column {
      InstallerMark(palette),
      Column {
        Text(installer_strings::application_setup)
            .Style(TextStyle{Font::System(12.0F).WithWeight(FontWeight::SemiBold),
                             palette.brand_eyebrow}),
        Text("db++")
            .Style(TextStyle{Font::System(28.0F).WithWeight(FontWeight::Bold),
                             palette.brand_text}),
        Spacer().With(
            Grow(0.0F),
            Frame{.width = 64.0F, .height = 3.5F, .min_width = 64.0F, .max_width = 64.0F,
                  .min_height = 3.5F, .max_height = 3.5F},
            CornerRadius(2.0F),
            Background(palette.accent_gradient)),
        Text(installer_strings::guided_setup)
            .Style(TextStyle{Font::System(theme.typography.body_medium),
                             palette.brand_secondary}),
      }.With(Spacing(10.0F), CrossAlign(CrossAxisAlignment::Start)),
      Spacer().With(Grow()),
      Text(installer_strings::platform_windows)
          .Style(TextStyle{Font::System(11.0F).WithWeight(FontWeight::SemiBold),
                           palette.brand_secondary}),
    }.With(
        Grow(),
        Padding(32.0F),
        Spacing(28.0F),
        Background(palette.brand_glow),
        CrossAlign(CrossAxisAlignment::Start)
    ),
  }.With(
      Frame{.width = 240.0F},
      Background(VisualFill{palette.brand_bg}),
      CrossAlign(CrossAxisAlignment::Stretch),
      WindowDragRegion{}
  );
}

View CompletionMark(const InstallerPalette& palette) {
  const Color foreground = palette.success;
  const Color background = WithAlpha(foreground, 0.14F);
  return Canvas([background, foreground](PaintContext& paint, Size) {
    paint.DrawCircle({26.0F, 26.0F}, 26.0F, background);
    const StrokeStyle stroke{.width = 3.0F, .cap = StrokeCap::Round, .join = StrokeJoin::Round};
    paint.DrawLine({15.0F, 27.0F}, {23.0F, 35.0F}, foreground, stroke);
    paint.DrawLine({23.0F, 35.0F}, {39.0F, 18.0F}, foreground, stroke);
  }).With(Frame{.width = 52.0F, .height = 52.0F});
}

View SecondaryAction(View action, const InstallerPalette& palette) {
  const ThemeSpec& theme = palette.spec;
  const Color background = palette.dark ? theme.colors.surface_container : theme.colors.surface;
  const Color disabled_background = WithAlpha(theme.colors.on_surface, 0.08F);
  const Color disabled_label = WithAlpha(theme.colors.on_surface, 0.38F);
  ThemeDefinition definition;
  definition.Set(ButtonStyle{
      .background = VisualFill{background},
      .label_style = TextStyle{
          Font::System(theme.typography.label_large).WithWeight(FontWeight::Medium),
          theme.colors.on_surface},
      .disabled_background = VisualFill{disabled_background},
      .disabled_label = disabled_label,
      .padding = EdgeInsets::Symmetric(20.0F, 8.0F),
      .minimum_width = 76.0F,
      .minimum_height = 38.0F,
      .corner_radii = CornerRadii{8.0F},
      .indication = theme.interactions.indication,
  });
  return Theme {std::move(definition),
                std::move(action).With(Border{palette.border_subtle, 1.0F}, CornerRadius(8.0F))};
}

std::vector<View> PromptActions(const InstallerHandle& installer, const InstallerPrompt& prompt,
                                const InstallerPalette& palette) {
  std::vector<View> actions;
  for (const InstallerPromptChoice choice : prompt.choices) {
    StringVariant label = installer_strings::continue_action;
    switch (choice) {
    case InstallerPromptChoice::Ok:
      label = prompt.kind == InstallerPromptKind::FilesInUse
                  ? installer_strings::close_apps
                  : installer_strings::ok;
      break;
    case InstallerPromptChoice::Cancel:
      label = installer_strings::cancel;
      break;
    case InstallerPromptChoice::Abort:
      label = installer_strings::abort;
      break;
    case InstallerPromptChoice::Retry:
      label = installer_strings::retry;
      break;
    case InstallerPromptChoice::TryAgain:
      label = installer_strings::try_again;
      break;
    case InstallerPromptChoice::Ignore:
      label = installer_strings::ignore;
      break;
    case InstallerPromptChoice::Yes:
      label = prompt.kind == InstallerPromptKind::Restart
                  ? installer_strings::restart_now
                  : installer_strings::yes;
      break;
    case InstallerPromptChoice::No:
      label = prompt.kind == InstallerPromptKind::Restart
                  ? installer_strings::restart_later
                  : installer_strings::no;
      break;
    case InstallerPromptChoice::Continue:
      label = installer_strings::continue_action;
      break;
    }
    View action = Button(std::move(label)).OnClick([installer, id = prompt.id, choice] {
      installer.Respond(id, choice);
    });
    if (!prompt.recommended || choice != *prompt.recommended) {
      action = SecondaryAction(std::move(action), palette);
    }
    actions.push_back(std::move(action));
  }
  return actions;
}

[[huxerui::composable]]
View RestartDialog(const InstallerHandle& installer, const InstallerPrompt& prompt,
                   const InstallerPalette& palette) {
  const ThemeSpec& theme = UseTheme();
  return Column {
    Text(installer_strings::restart_title, TextRole::Title),
    Text(installer_strings::restart_confirmation).With(Foreground(theme.colors.on_surface_variant)),
    Row(PromptActions(installer, prompt, palette)).With(
        Spacing(10.0F), MainAlign(MainAxisAlignment::End)),
  }.With(
      Frame{.width = 420.0F},
      Padding(24.0F),
      Spacing(16.0F),
      Background(theme.colors.surface_container_highest),
      Border{palette.border_subtle, 1.0F},
      CornerRadius(10.0F)
  );
}

[[huxerui::composable]]
View InstallerContent(const InstallerPalette& palette) {
  const InstallerHandle installer = UseInstaller();
  const WindowHandle window = UseWindow();
  const TaskScope tasks = UseTaskScope();
  const ThemeSpec& theme = UseTheme();
  const InstallerStatus status = installer.Status();
  auto destination = UseState<std::optional<TextEditingValue>>(std::nullopt);
  auto desktop_shortcut = UseState<std::optional<bool>>(std::nullopt);
  StringVariant eyebrow;
  StringVariant heading;
  std::vector<View> details;
  std::vector<View> actions;
  MainAxisAlignment panel_alignment = MainAxisAlignment::Start;

  const bool restart_prompt =
      status.prompt && status.prompt->kind == InstallerPromptKind::Restart;
  if (status.prompt && !restart_prompt) {
    eyebrow = installer_strings::action_required;
    heading = installer_strings::setup_needs_attention;
    if (status.prompt->kind == InstallerPromptKind::FilesInUse) {
      details.push_back(
          Text(installer_strings::files_in_use).With(Foreground(theme.colors.on_surface_variant)));
      if (!status.prompt->message.empty()) {
        details.push_back(ScrollView {
          Text(status.prompt->message).With(Foreground(theme.colors.on_surface_variant)),
        }.With(Frame{.max_height = 144.0F}));
      }
    } else {
      details.push_back(
          Text(status.prompt->message).With(Foreground(theme.colors.on_surface_variant)));
    }
    actions = PromptActions(installer, *status.prompt, palette);
  } else if (status.phase == InstallerPhase::Detecting) {
    eyebrow = installer_strings::welcome;
    heading = installer_strings::preparing_setup;
    panel_alignment = MainAxisAlignment::Center;
    details.push_back(Text(installer_strings::checking_installed_version)
                          .With(Foreground(theme.colors.on_surface_variant)));
    details.push_back(ProgressBar(status.progress));
  } else if (status.phase == InstallerPhase::Ready &&
             status.product == InstallerProductState::NewerVersion) {
    eyebrow = installer_strings::version_check;
    heading = installer_strings::newer_version_installed;
    panel_alignment = MainAxisAlignment::Center;
    details.push_back(Text(installer_strings::remove_newer_version)
                          .With(Foreground(theme.colors.on_surface_variant)));
    actions.push_back(Button(installer_strings::close)
                          .OnClick([window] { window.Close(); })
                          .With(Frame{.min_width = 112.0F}));
  } else if (status.phase == InstallerPhase::Ready &&
             status.product == InstallerProductState::SameVersion) {
    eyebrow = installer_strings::maintenance;
    heading = installer_strings::manage_installation;
    details.push_back(Text(installer_strings::already_installed)
                          .With(Foreground(theme.colors.on_surface_variant)));
    actions.push_back(SecondaryAction(
        Button(installer_strings::cancel).OnClick([window] { window.Close(); }), palette));
    actions.push_back(SecondaryAction(
        Button(installer_strings::uninstall).OnClick([installer] { installer.Uninstall(); }),
        palette));
    actions.push_back(Button(installer_strings::repair)
                          .OnClick([installer] { installer.Repair(); })
                          .With(Frame{.min_width = 112.0F}));
  } else if (status.phase == InstallerPhase::Ready &&
             (status.product == InstallerProductState::Absent ||
              status.product == InstallerProductState::Present)) {
    const TextEditingValue destination_value = destination.Get().value_or(
        TextEditingValue::FromText(InstallPathText(status.default_destination)));
    const std::optional<std::filesystem::path> destination_path =
        ParseInstallPath(destination_value.text);
    const std::filesystem::path browse_initial =
        destination_path.value_or(status.default_destination);
    const bool create_desktop_shortcut =
        desktop_shortcut.Get().value_or(status.default_create_desktop_shortcut);
    eyebrow = installer_strings::installation;
    heading = installer_strings::ready_to_install;
    details.push_back(Text(installer_strings::installation_options)
                          .With(Foreground(theme.colors.on_surface_variant)));
    details.push_back(Row {
      TextField(destination_value)
          .Label(installer_strings::installation_folder)
          .Variant(TextFieldVariant::Outlined)
          .Validation(
              destination_path ? ValidationResult::None()
                               : ValidationResult::Invalid(
                                     installer_strings::invalid_installation_folder))
          .OnChanged([destination](const TextEditingValue& value) { destination = value; })
          .With(Grow()),
      SecondaryAction(
          Button(installer_strings::browse)
              .OnClick([installer, destination, browse_initial, tasks] {
                tasks.Launch([installer, destination, browse_initial]() -> Task<void> {
                  const std::optional<std::filesystem::path> selected =
                      co_await installer.ChooseDestinationAsync(browse_initial);
                  if (selected) {
                    destination = TextEditingValue::FromText(InstallPathText(*selected));
                  }
                });
              }),
          palette),
    }.With(Spacing(12.0F), CrossAlign(CrossAxisAlignment::Center)));
    details.push_back(
        Checkbox(installer_strings::create_desktop_shortcut, create_desktop_shortcut)
            .OnChanged([desktop_shortcut](bool enabled) { desktop_shortcut = enabled; })
            .With(Padding(EdgeInsets::Symmetric(14.0F, 10.0F)),
                  Background(theme.colors.surface),
                  Border{palette.border_subtle, 1.0F},
                  CornerRadius(8.0F)));
    actions.push_back(SecondaryAction(
        Button(installer_strings::cancel).OnClick([window] { window.Close(); }), palette));
    actions.push_back(
        Button(installer_strings::install)
            .OnClick([installer, destination_path, create_desktop_shortcut] {
              if (destination_path) {
                installer.Install({
                    .destination = *destination_path,
                    .create_desktop_shortcut = create_desktop_shortcut,
                });
              }
            })
            .With(Enabled(destination_path.has_value()), Frame{.min_width = 112.0F}));
  } else if (status.phase == InstallerPhase::Ready) {
    eyebrow = installer_strings::setup;
    heading = installer_strings::setup_could_not_continue;
    panel_alignment = MainAxisAlignment::Center;
    details.push_back(Text(installer_strings::installed_version_unknown)
                          .With(Foreground(theme.colors.on_surface_variant)));
    actions.push_back(Button(installer_strings::close)
                          .OnClick([window] { window.Close(); })
                          .With(Frame{.min_width = 112.0F}));
  } else if (status.phase == InstallerPhase::Planning || status.phase == InstallerPhase::Applying ||
             status.phase == InstallerPhase::Canceling) {
    eyebrow = installer_strings::installing;
    heading = installer_strings::installing_product;
    panel_alignment = MainAxisAlignment::Center;
    if (status.phase == InstallerPhase::Canceling) {
      eyebrow = installer_strings::rollback;
      heading = installer_strings::canceling_and_rolling_back;
    } else if (status.action == InstallerAction::Repair) {
      eyebrow = installer_strings::repairing;
      heading = installer_strings::repairing_product;
    } else if (status.action == InstallerAction::Uninstall) {
      eyebrow = installer_strings::removing;
      heading = installer_strings::uninstalling_product;
    }
    StringVariant progress_message =
        status.current_package.empty()
            ? StringVariant(installer_strings::operation_may_take_a_moment)
            : StringVariant(status.current_package);
    details.push_back(
        Text(std::move(progress_message)).With(Foreground(theme.colors.on_surface_variant)));
    details.push_back(ProgressBar(status.progress));
    actions.push_back(SecondaryAction(
        Button(installer_strings::cancel).OnClick([installer] { installer.Cancel(); }), palette));
  } else if (status.phase == InstallerPhase::Completed) {
    eyebrow = installer_strings::complete;
    heading = status.action == InstallerAction::Uninstall
                  ? installer_strings::application_removed
                  : installer_strings::installation_complete;
    panel_alignment = MainAxisAlignment::Center;
    StringVariant message;
    if (status.action == InstallerAction::Uninstall) {
      message = status.restart == InstallerRestart::Required
                    ? installer_strings::application_removed_restart_message
                    : installer_strings::application_removed_message;
    } else {
      message = status.restart == InstallerRestart::Required
                    ? installer_strings::installation_complete_restart_message
                    : installer_strings::installation_complete_message;
    }
    details.push_back(Row {CompletionMark(palette)}.With(MainAlign(MainAxisAlignment::Center)));
    details.push_back(
        Text(std::move(message)).With(Foreground(theme.colors.on_surface_variant)));
    actions.push_back(Button(installer_strings::close)
                          .OnClick([window] { window.Close(); })
                          .With(Frame{.min_width = 112.0F}));
  } else if (status.phase == InstallerPhase::Canceled) {
    eyebrow = installer_strings::canceled;
    heading = installer_strings::installation_canceled;
    panel_alignment = MainAxisAlignment::Center;
    details.push_back(Text(installer_strings::no_further_changes)
                          .With(Foreground(theme.colors.on_surface_variant)));
    actions.push_back(Button(installer_strings::close)
                          .OnClick([window] { window.Close(); })
                          .With(Frame{.min_width = 112.0F}));
  } else if (status.failure) {
    eyebrow = installer_strings::setup_error;
    heading = installer_strings::installation_failed;
    panel_alignment = MainAxisAlignment::Center;
    details.push_back(Text(status.failure->message).With(Foreground(theme.colors.error)));
    actions.push_back(Button(installer_strings::close)
                          .OnClick([window] { window.Close(); })
                          .With(Frame{.min_width = 112.0F}));
  }

  std::vector<View> panel{
      Text(std::move(eyebrow))
          .Style(TextStyle{Font::System(12.0F).WithWeight(FontWeight::SemiBold),
                           theme.colors.primary}),
      Text(std::move(heading), TextRole::Title).With(FontSize(theme.typography.headline_small)),
  };
  panel.insert(panel.end(), std::make_move_iterator(details.begin()),
               std::make_move_iterator(details.end()));

  std::vector<View> content;
  content.push_back(Column(std::move(panel)).With(
      Spacing(16.0F),
      MainAlign(panel_alignment),
      CrossAlign(CrossAxisAlignment::Stretch),
      Grow()));
  if (!actions.empty()) {
    content.push_back(Divider());
    content.push_back(Row(std::move(actions)).With(
        Spacing(10.0F),
        MainAlign(MainAxisAlignment::End),
        CrossAlign(CrossAxisAlignment::Center)));
  }

  Dialog restart_dialog;
  if (restart_prompt) {
    restart_dialog = Dialog{
        .visible = true,
        .content = [installer, prompt = *status.prompt, palette] {
          return RestartDialog(installer, prompt, palette);
        },
        .dismiss_on_cancel = true,
        .on_dismiss_request =
            [installer, id = status.prompt->id] { installer.Respond(id, InstallerPromptChoice::No); },
    };
  }

  return Row {
    BrandPanel(palette),
    Spacer().With(Grow(0.0F), Frame{.width = 1.0F, .min_width = 1.0F, .max_width = 1.0F},
                  Background(VisualFill{palette.border_subtle})),
    Column {
      WindowTitleBar{}.With(Background(theme.colors.background)),
      Spacer().With(Grow(0.0F), Frame{.height = 2.0F, .min_height = 2.0F, .max_height = 2.0F},
                    Background(palette.accent_gradient)),
      Column(std::move(content)).With(
          Padding(EdgeInsets{.top = 12.0F, .right = 36.0F, .bottom = 32.0F, .left = 36.0F}),
          Spacing(24.0F),
          CrossAlign(CrossAxisAlignment::Stretch),
          Grow()),
    }.With(CrossAlign(CrossAxisAlignment::Stretch), Grow()),
  }.With(CrossAlign(CrossAxisAlignment::Stretch), Background(theme.colors.background), Grow(),
         std::move(restart_dialog));
}

[[huxerui::composable]]
View InstallerPage() {
  const bool dark = UseSystemThemeBrightness() == ThemeBrightness::Dark;
  const InstallerPalette palette = BuildInstallerPalette(dark);
  return Theme(InstallerThemeDefinition(palette), InstallerContent(palette));
}

} // namespace

const Application application{
    InstallerPage,
    {
        .window = {
            .title = "db++",
            .initial_size = {780.0F, 500.0F},
            .minimum_size = Size{720.0F, 460.0F},
            .chrome_mode = WindowChromeMode::Custom,
            .title_bar_height = 40.0F,
        },
        .show_debug_overlay = false,
        .window_hooks = {InstallInstallerSession},
    },
};
