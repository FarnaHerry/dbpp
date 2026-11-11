#include "theme.h"

#include <utility>

namespace dbpp::ui {

using namespace huxerui;

namespace {

Color WithAlpha(Color color, float alpha) {
  color.alpha = alpha;
  return color;
}

struct InteractionSink {
  class Extension;
  State<InteractionState> target;
};

class InteractionSink::Extension final : public NodeExtension {
public:
  Extension(ViewNode&, const InteractionSink& spec) : target_(spec.target) {}

  void Update(ViewNode&, const InteractionSink& spec) {
    target_ = spec.target;
  }

  void OnInteraction(ViewNode&, const InteractionState& state,
                     const std::optional<InteractionEvent>&) override {
    target_ = state;
  }

private:
  State<InteractionState> target_;
};

} // namespace

Indication MonochromeIndication(Color foreground) {
  const auto layer = [foreground](float width) {
    return IndicationLayer{
        .border = Border{foreground, width},
        .enter = SnapSpec{},
        .exit = SnapSpec{},
    };
  };
  return Indication{
      .focus = layer(2.0F),
      .hover = layer(1.0F),
      .press = layer(2.0F),
  };
}

ThemeSpec DarkThemeSpec() {
  ThemeSpec spec = MaterialDarkThemeSpec();
  spec.typography = TypographyScheme{
      .body_large = 16.0F,
      .body_medium = font_size::kBody,
      .body_small = font_size::kChip,
      .label_large = font_size::kBody,
      .title_large = font_size::kTitle,
      .headline_small = 24.0F,
  };
  spec.colors.primary = Color::Rgb(255, 255, 255);
  spec.colors.on_primary = Color::Rgb(0, 0, 0);
  spec.colors.primary_container = Color::Rgb(0, 0, 0);
  spec.colors.on_primary_container = Color::Rgb(255, 255, 255);
  spec.colors.secondary = Color::Rgb(255, 255, 255);
  spec.colors.on_secondary = Color::Rgb(0, 0, 0);
  spec.colors.secondary_container = Color::Rgb(0, 0, 0);
  spec.colors.on_secondary_container = Color::Rgb(255, 255, 255);
  spec.colors.tertiary_container = Color::Rgb(0, 0, 0);
  spec.colors.on_tertiary_container = Color::Rgb(255, 255, 255);
  spec.colors.background = Color::Rgb(0, 0, 0);
  spec.colors.surface = Color::Rgb(0, 0, 0);
  spec.colors.surface_container_low = Color::Rgb(0, 0, 0);
  spec.colors.surface_container = Color::Rgb(0, 0, 0);
  spec.colors.surface_container_high = Color::Rgb(0, 0, 0);
  spec.colors.surface_container_highest = Color::Rgb(0, 0, 0);
  spec.colors.on_surface = Color::Rgb(255, 255, 255);
  spec.colors.on_surface_variant = Color::Rgb(255, 255, 255);
  spec.colors.outline = Color::Rgb(255, 255, 255);
  spec.colors.inverse_surface = Color::Rgb(255, 255, 255);
  spec.colors.inverse_on_surface = Color::Rgb(0, 0, 0);
  spec.colors.scrim = spec.colors.background;
  spec.colors.error = spec.colors.on_surface;
  spec.interactions.indication = MonochromeIndication(spec.colors.on_surface);
  spec.interactions.focus_ring.color = spec.colors.primary;
  return spec;
}

ThemeSpec LightThemeSpec() {
  ThemeSpec spec = MaterialLightThemeSpec();
  spec.typography = TypographyScheme{
      .body_large = 16.0F,
      .body_medium = font_size::kBody,
      .body_small = font_size::kChip,
      .label_large = font_size::kBody,
      .title_large = font_size::kTitle,
      .headline_small = 24.0F,
  };
  spec.colors.primary = Color::Rgb(0, 0, 0);
  spec.colors.on_primary = Color::Rgb(255, 255, 255);
  spec.colors.primary_container = Color::Rgb(255, 255, 255);
  spec.colors.on_primary_container = Color::Rgb(0, 0, 0);
  spec.colors.secondary = Color::Rgb(0, 0, 0);
  spec.colors.on_secondary = Color::Rgb(255, 255, 255);
  spec.colors.secondary_container = Color::Rgb(255, 255, 255);
  spec.colors.on_secondary_container = Color::Rgb(0, 0, 0);
  spec.colors.tertiary_container = Color::Rgb(255, 255, 255);
  spec.colors.on_tertiary_container = Color::Rgb(0, 0, 0);
  spec.colors.background = Color::Rgb(255, 255, 255);
  spec.colors.surface = Color::Rgb(255, 255, 255);
  spec.colors.surface_container_low = Color::Rgb(255, 255, 255);
  spec.colors.surface_container = Color::Rgb(255, 255, 255);
  spec.colors.surface_container_high = Color::Rgb(255, 255, 255);
  spec.colors.surface_container_highest = Color::Rgb(255, 255, 255);
  spec.colors.on_surface = Color::Rgb(0, 0, 0);
  spec.colors.on_surface_variant = Color::Rgb(0, 0, 0);
  spec.colors.outline = Color::Rgb(0, 0, 0);
  spec.colors.inverse_surface = Color::Rgb(0, 0, 0);
  spec.colors.inverse_on_surface = Color::Rgb(255, 255, 255);
  spec.colors.scrim = spec.colors.background;
  spec.colors.error = spec.colors.on_surface;
  spec.interactions.indication = MonochromeIndication(spec.colors.on_surface);
  spec.interactions.focus_ring.color = spec.colors.primary;
  return spec;
}

ThemeDefinition AppThemeDefinition(const ThemeSpec& spec) {
  ThemeDefinition definition = MaterialThemeDefinition(spec);

  ButtonStyle buttons; // transparent + outline by default
  buttons.background = VisualFill{Color::Transparent()};
  buttons.label_style = TextStyle{Font::System(font_size::kBody), spec.colors.on_surface};
  buttons.disabled_background = VisualFill{Color::Transparent()};
  buttons.disabled_label = WithAlpha(spec.colors.on_surface, 0.38F);
  buttons.indication = MonochromeIndication(spec.colors.on_surface);
  buttons.corner_radii = CornerRadii{6.0F};
  buttons.padding = EdgeInsets::Symmetric(12.0F, 6.0F);
  buttons.minimum_height = 30.0F;
  definition.Set(buttons);

  IconButtonStyle icon_buttons = IconButtonStyle::Default();
  icon_buttons.foreground = spec.colors.on_surface;
  icon_buttons.disabled_foreground = WithAlpha(spec.colors.on_surface, 0.38F);
  icon_buttons.indication = MonochromeIndication(spec.colors.on_surface);
  definition.Set(icon_buttons);

  SegmentedButtonStyle segments = SegmentedButtonStyle::Default();
  segments.background = VisualFill{spec.colors.surface};
  segments.selected_background = VisualFill{spec.colors.primary};
  segments.label_style = TextStyle{Font::System(font_size::kChip), spec.colors.on_surface};
  segments.selected_label = spec.colors.on_primary;
  segments.border = Border{spec.colors.outline, 1.0F};
  segments.selected_border = Border{spec.colors.outline, 1.0F};
  segments.indication = MonochromeIndication(spec.colors.on_surface);
  segments.selected_indication = MonochromeIndication(spec.colors.on_primary);
  segments.corner_radii = CornerRadii{6.0F};
  segments.padding = EdgeInsets::Symmetric(10.0F, 5.0F);
  segments.minimum_height = 28.0F;
  definition.Set(segments);

  TextFieldStyle text_fields = TextFieldStyle::Default();
  text_fields.variant = TextFieldVariant::Outlined;
  text_fields.show_label = true;
  text_fields.outlined.background = Color::Transparent();
  text_fields.outlined.disabled_background = Color::Transparent();
  text_fields.outlined.border = spec.colors.outline;
  text_fields.outlined.hovered_border = spec.colors.on_surface_variant;
  text_fields.outlined.focused_border = spec.colors.primary;
  text_fields.outlined.disabled_border = WithAlpha(spec.colors.on_surface, 0.12F);
  text_fields.outlined.corner_radii = CornerRadii{6.0F};
  text_fields.outlined.minimum_height = 42.0F;
  text_fields.text_style = TextStyle{Font::System(font_size::kBody), spec.colors.on_surface};
  text_fields.label_style = TextStyle{Font::System(font_size::kBody), spec.colors.on_surface_variant};
  text_fields.floating_label_style =
      TextStyle{Font::System(font_size::kChip), spec.colors.on_surface_variant};
  text_fields.placeholder_style =
      TextStyle{Font::System(font_size::kBody), WithAlpha(spec.colors.on_surface, 0.55F)};
  text_fields.disabled_text = WithAlpha(spec.colors.on_surface, 0.38F);
  text_fields.disabled_label = WithAlpha(spec.colors.on_surface, 0.38F);
  text_fields.disabled_placeholder = WithAlpha(spec.colors.on_surface, 0.38F);
  text_fields.disabled_supporting_text = WithAlpha(spec.colors.on_surface, 0.38F);
  text_fields.focused_label = spec.colors.on_surface;
  text_fields.error_label = spec.colors.error;
  text_fields.validation_error = spec.colors.error;
  text_fields.validation_text_style =
      TextStyle{Font::System(font_size::kChip), spec.colors.error};
  text_fields.leading_icon = spec.colors.on_surface;
  text_fields.focused_leading_icon = spec.colors.primary;
  text_fields.disabled_leading_icon = WithAlpha(spec.colors.on_surface, 0.38F);
  text_fields.error_leading_icon = spec.colors.error;
  text_fields.trailing_icon = spec.colors.on_surface;
  text_fields.focused_trailing_icon = spec.colors.primary;
  text_fields.disabled_trailing_icon = WithAlpha(spec.colors.on_surface, 0.38F);
  text_fields.error_trailing_icon = spec.colors.error;
  text_fields.selection = WithAlpha(spec.colors.primary, 0.24F);
  text_fields.caret = spec.colors.primary;
  text_fields.error_caret = spec.colors.error;
  text_fields.composition = spec.colors.primary;
  text_fields.border_width = 1.0F;
  text_fields.focused_border_width = 2.0F;
  text_fields.padding = EdgeInsets::Symmetric(12.0F, 8.0F);
  definition.Set(text_fields);

  DialogStyle dialogs = DialogStyle::Default();
  dialogs.scrim = spec.colors.scrim;
  dialogs.background = spec.colors.surface_container_highest;
  dialogs.title_style =
      TextStyle{Font::System(font_size::kTitle).WithWeight(FontWeight::Bold), spec.colors.on_surface};
  dialogs.message_style = TextStyle{Font::System(font_size::kBody), spec.colors.on_surface};
  dialogs.positive_action_style =
      TextStyle{Font::System(font_size::kBody), spec.colors.on_primary};
  dialogs.positive_action_background = VisualFill{spec.colors.primary};
  dialogs.positive_action_indication = MonochromeIndication(spec.colors.on_primary);
  dialogs.negative_action_style =
      TextStyle{Font::System(font_size::kBody), spec.colors.on_surface};
  dialogs.negative_action_indication = MonochromeIndication(spec.colors.on_surface);
  dialogs.action_separator_color = spec.colors.outline;
  dialogs.shadow = Shadow{spec.colors.outline, {}, 0.0F, 1.0F};
  definition.Set(dialogs);

  MenuStyle menus = MenuStyle::Default();
  menus.background = spec.colors.surface_container;
  menus.foreground = spec.colors.on_surface;
  menus.icon_tint = spec.colors.on_surface_variant;
  menus.separator_color = spec.colors.outline;
  menus.shadow = Shadow{spec.colors.outline, {}, 0.0F, 1.0F};
  menus.corner_radii = CornerRadii{6.0F};
  menus.item_indication = MonochromeIndication(spec.colors.on_surface);
  definition.Set(menus);

  DividerStyle dividers = DividerStyle::Default();
  dividers.color = spec.colors.outline;
  dividers.thickness = 1.0F;
  definition.Set(dividers);

  return definition;
}

[[huxerui::composable]] View ActionButton(View action, bool primary) {
  const ThemeSpec& spec = UseTheme();
  View button = std::move(action);
  button = std::move(button).With(Border{spec.colors.outline, 1.0F}, CornerRadius{6.0F});
  if (primary) {
    ButtonStyle style = UseEnvironment<ButtonStyle>();
    style.background = VisualFill{spec.colors.primary};
    style.label_style = TextStyle{Font::System(font_size::kBody), spec.colors.on_primary};
    style.indication = MonochromeIndication(spec.colors.on_primary);
    ThemeDefinition definition;
    definition.Set(style);
    return Theme(std::move(definition), std::move(button));
  }
  return button;
}

[[huxerui::composable]] View NavItem(std::string label, std::string detail, bool selected,
             std::function<void()> on_click, View trailing) {
  const ThemeSpec& spec = UseTheme();
  auto interaction = UseState(InteractionState{});
  const InteractionState state = interaction.Get();
  const bool hovered = state.enabled && (state.hovered || state.pressed || state.focus_visible);
  const Color foreground = selected ? spec.colors.on_primary : spec.colors.on_surface;
  const Color detail_color = selected ? spec.colors.on_primary : spec.colors.on_surface_variant;

  View label_view = Text(label).Style(
      TextStyle{Font::System(font_size::kBody).WithWeight(FontWeight::SemiBold), foreground});
  if (!detail.empty()) {
    label_view = Column{
        std::move(label_view),
        Text(detail).Style(TextStyle{Font::System(font_size::kCaption), detail_color}),
    }.With(Spacing{2.0F}, CrossAlign{CrossAxisAlignment::Stretch});
  }

  View trailing_view = trailing;
  View row = Row{
      std::move(label_view).With(Grow(1.0F)),
      std::move(trailing_view),
  }.With(Padding{EdgeInsets::Symmetric(10.0F, 7.0F)}, Spacing{6.0F},
         CrossAlign{CrossAxisAlignment::Center}, CornerRadius{6.0F},
         Background{selected ? VisualFill{spec.colors.primary}
                             : VisualFill{Color::Transparent()}},
         Border{selected ? Color::Transparent()
                         : (hovered ? spec.colors.outline : Color::Transparent()),
                1.0F},
         InteractionSink{interaction});

  if (on_click) {
    return std::move(row).OnClick(std::move(on_click));
  }
  return row;
}

View PageTitle(std::string title, View actions) {
  return Row{
      Text(title, TextRole::Title),
      Spacer(),
      std::move(actions),
  }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{8.0F});
}

[[huxerui::composable]] View Caption(std::string text) {
  const ThemeSpec& spec = UseTheme();
  return Text(text).Style(
      TextStyle{Font::System(font_size::kCaption).WithWeight(FontWeight::Bold),
                spec.colors.on_surface_variant});
}

[[huxerui::composable]] View MutedText(std::string text) {
  const ThemeSpec& spec = UseTheme();
  return Text(text).Style(TextStyle{Font::System(font_size::kCaption), spec.colors.on_surface_variant});
}

[[huxerui::composable]] View ErrorText(std::string text) {
  const ThemeSpec& spec = UseTheme();
  return Text(text).Style(
      TextStyle{Font::System(font_size::kChip).WithWeight(FontWeight::SemiBold),
                spec.colors.on_surface});
}

[[huxerui::composable]] View MonospaceField(View field) {
  TextFieldStyle style = UseEnvironment<TextFieldStyle>();
  style.text_style.font = Font::Monospace(13.0F);
  style.floating_label_style.font = Font::Monospace(11.0F);
  ThemeDefinition definition;
  definition.Set(style);
  View field_view = std::move(field);
  return Theme(std::move(definition), std::move(field_view));
}

[[huxerui::composable]] View DialogCard(View content) {
  const ThemeSpec& spec = UseTheme();
  View card = std::move(content);
  return std::move(card).With(
      Background{VisualFill{spec.colors.surface}},
      CornerRadius{8.0F},
      Border{spec.colors.outline, 1.0F},
      Padding{EdgeInsets::Symmetric(20.0F, 18.0F)});
}

} // namespace dbpp::ui
