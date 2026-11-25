#include "theme.h"
#include "model.h"

#include <algorithm>
#include <utility>

namespace dbpp::ui {

using namespace huxerui;

namespace {

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

View AccentDot(Color color, float size) {
  return Spacer().With(
      Grow(0.0F),
      Frame{.width = size, .height = size, .min_width = size, .max_width = size,
            .min_height = size, .max_height = size},
      CornerRadius{size * 0.5F}, Background{VisualFill{color}});
}

View StatusBadge(std::string text, Color foreground, Color background, Color border) {
  return Text(std::move(text))
      .Style(TextStyle{Font::System(font_size::kCaption).WithWeight(FontWeight::Medium), foreground})
      .LineLimit(1)
      .With(Padding{EdgeInsets::Symmetric(7.0F, 2.0F)}, CornerRadius{10.0F},
            Background{VisualFill{background}}, Border{border, 1.0F});
}

[[huxerui::composable]] View ActionButton(View action, ButtonTone tone) {
  const ThemeSpec& spec = UseTheme();
  const SemanticColors& semantic = UseEnvironment<SemanticColors>();
  ButtonStyle style = UseEnvironment<ButtonStyle>();
  style.corner_radii = CornerRadii{6.0F};

  Color border_color = semantic.border_subtle;
  switch (tone) {
    case ButtonTone::Primary: {
      const bool light_ink = semantic.on_primary_button.red > 0.5F;
      const Color hover = light_ink ? Color::Rgb(255, 255, 255, 0.14F)
                                    : Color::Rgb(0, 0, 0, 0.08F);
      const Color press = light_ink ? Color::Rgb(255, 255, 255, 0.22F)
                                    : Color::Rgb(0, 0, 0, 0.14F);
      style.background = semantic.primary_button_fill;
      style.label_style =
          TextStyle{Font::System(font_size::kBody).WithWeight(FontWeight::Medium),
                    semantic.on_primary_button};
      style.indication = TintedIndication(hover, press, spec.colors.primary);
      border_color = Color::Transparent();
      break;
    }
    case ButtonTone::Danger:
      style.background = VisualFill{semantic.danger_container};
      style.label_style =
          TextStyle{Font::System(font_size::kBody).WithWeight(FontWeight::Medium), semantic.danger};
      style.indication = TintedIndication(WithAlpha(semantic.danger, 0.12F),
                                          WithAlpha(semantic.danger, 0.20F), semantic.danger);
      border_color = WithAlpha(semantic.danger, 0.45F);
      break;
    case ButtonTone::DangerSolid:
      style.background = VisualFill{semantic.danger_solid};
      style.label_style =
          TextStyle{Font::System(font_size::kBody).WithWeight(FontWeight::SemiBold),
                    semantic.on_danger};
      style.indication = TintedIndication(Color::Rgb(255, 255, 255, 0.14F),
                                          Color::Rgb(255, 255, 255, 0.22F), semantic.danger_solid);
      border_color = Color::Transparent();
      break;
    case ButtonTone::Subtle:
      style.background = VisualFill{Color::Transparent()};
      style.label_style =
          TextStyle{Font::System(font_size::kBody), spec.colors.on_surface_variant};
      style.indication =
          TintedIndication(semantic.hover_fill, semantic.press_fill, spec.colors.primary);
      border_color = Color::Transparent();
      break;
    case ButtonTone::Default:
    default:
      style.background = VisualFill{spec.colors.surface};
      style.label_style = TextStyle{Font::System(font_size::kBody), spec.colors.on_surface};
      style.indication =
          TintedIndication(semantic.hover_fill, semantic.press_fill, spec.colors.primary);
      border_color = semantic.border_subtle;
      break;
  }

  ThemeDefinition definition;
  definition.Set(style);
  View button = action;
  button = std::move(button).With(Border{border_color, 1.0F}, CornerRadius{6.0F});
  return Theme(std::move(definition), std::move(button));
}

[[huxerui::composable]] View TitleBarButton(std::string label, std::function<void()> on_click,
                                            bool active) {
  const ThemeSpec& spec = UseTheme();
  const SemanticColors& semantic = UseEnvironment<SemanticColors>();
  ButtonStyle style = UseEnvironment<ButtonStyle>();
  style.padding = EdgeInsets::Symmetric(10.0F, 4.0F);
  style.minimum_height = 26.0F;
  style.corner_radii = CornerRadii{6.0F};
  Color border_color = Color::Transparent();
  if (active) {
    style.background = VisualFill{spec.colors.primary_container};
    style.label_style =
        TextStyle{Font::System(font_size::kChip).WithWeight(FontWeight::SemiBold),
                  spec.colors.on_primary_container};
    style.indication =
        TintedIndication(semantic.hover_fill, semantic.press_fill, spec.colors.primary);
    border_color = WithAlpha(spec.colors.primary, 0.50F);
  } else {
    style.background = VisualFill{Color::Transparent()};
    style.label_style =
        TextStyle{Font::System(font_size::kChip).WithWeight(FontWeight::Medium),
                  spec.colors.on_surface};
    style.indication =
        TintedIndication(semantic.hover_fill, semantic.press_fill, spec.colors.primary);
  }
  ThemeDefinition definition;
  definition.Set(style);
  View button = Button(std::move(label))
                    .OnClick(std::move(on_click))
                    .With(Border{border_color, 1.0F}, CornerRadius{6.0F});
  return Theme(std::move(definition), std::move(button));
}

[[huxerui::composable]] View NavItem(std::string label, std::string detail, bool selected,
                                     std::function<void()> on_click, View trailing,
                                     std::optional<Color> accent_dot) {
  const ThemeSpec& spec = UseTheme();
  const SemanticColors& semantic = UseEnvironment<SemanticColors>();
  auto interaction = UseState(InteractionState{});
  const InteractionState state = interaction.Get();
  const bool hovered = state.enabled && (state.hovered || state.pressed || state.focus_visible);
  const Color foreground = selected ? spec.colors.on_primary_container : spec.colors.on_surface;
  const Color detail_color =
      selected ? WithAlpha(spec.colors.on_primary_container, 0.78F) : spec.colors.on_surface_variant;

  View label_view = Text(std::move(label))
                        .Style(TextStyle{Font::System(font_size::kBody).WithWeight(FontWeight::SemiBold),
                                         foreground});
  if (!detail.empty()) {
    label_view = Column{
        std::move(label_view),
        Text(std::move(detail)).Style(TextStyle{Font::System(font_size::kCaption), detail_color}),
    }.With(Spacing{2.0F}, CrossAlign{CrossAxisAlignment::Stretch});
  }

  Views row_items;
  if (accent_dot.has_value()) {
    row_items.Add(AccentDot(*accent_dot, 8.0F));
  }
  row_items.Add(std::move(label_view).With(Grow(1.0F)));
  if (trailing) {
    row_items.Add(std::move(trailing));
  }

  const Color bg_color =
      selected ? spec.colors.primary_container
               : (state.pressed ? semantic.press_fill
                                : (hovered ? semantic.hover_fill : Color::Transparent()));
  const Color border_color =
      selected ? WithAlpha(spec.colors.primary, 0.55F)
               : (hovered ? semantic.border_subtle : Color::Transparent());

  View row = Row{std::move(row_items)}
                 .With(Padding{EdgeInsets::Symmetric(10.0F, 8.0F)}, Spacing{8.0F},
                       CrossAlign{CrossAxisAlignment::Center}, CornerRadius{7.0F},
                       Background{VisualFill{bg_color}}, Border{border_color, 1.0F},
                       InteractionSink{interaction});

  if (on_click) {
    return std::move(row).OnClick(std::move(on_click));
  }
  return row;
}

[[huxerui::composable]] View TreeRow(TreeRowOptions options, std::function<void()> on_click) {
  const ThemeSpec& spec = UseTheme();
  const SemanticColors& semantic = UseEnvironment<SemanticColors>();
  auto interaction = UseState(InteractionState{});
  const InteractionState state = interaction.Get();
  const bool hovered = state.enabled && (state.hovered || state.pressed || state.focus_visible);

  const Color foreground =
      options.selected
          ? spec.colors.on_primary_container
          : (options.muted ? spec.colors.on_surface_variant : spec.colors.on_surface);
  const Color secondary =
      options.selected
          ? WithAlpha(spec.colors.on_primary_container, 0.78F)
          : WithAlpha(spec.colors.on_surface_variant, options.muted ? 0.72F : 0.90F);
  const Color bg_color =
      options.selected
          ? spec.colors.primary_container
          : (state.pressed
                 ? semantic.press_fill
                 : (hovered ? semantic.hover_fill
                            : (options.active_hint ? WithAlpha(spec.colors.primary, 0.06F)
                                                   : Color::Transparent())));
  const Color border_color =
      options.selected
          ? WithAlpha(spec.colors.primary, 0.40F)
          : (hovered ? semantic.border_subtle
                     : (options.active_hint ? WithAlpha(spec.colors.primary, 0.28F)
                                            : Color::Transparent()));

  const float left_padding = 8.0F + static_cast<float>(std::max(0, options.indent_level)) * 14.0F;

  Views children;
  if (options.selected) {
    children.Add(Spacer().With(
        Grow(0.0F),
        Frame{.width = 3.0F, .height = 14.0F, .min_width = 3.0F, .max_width = 3.0F,
              .min_height = 14.0F, .max_height = 14.0F},
        CornerRadius{1.5F}, Background{VisualFill{spec.colors.primary}}));
  }
  if (!options.prefix.empty()) {
    const Color p_color = options.selected
                              ? spec.colors.primary
                              : options.prefix_color.value_or(secondary);
    children.Add(Text(options.prefix)
                     .Style(TextStyle{Font::System(font_size::kCaption), p_color})
                     .Align(TextAlign::Center)
                     .LineLimit(1)
                     .With(Frame{.width = 12.0F}));
  }
  if (options.status_dot.has_value()) {
    children.Add(AccentDot(*options.status_dot, 7.0F));
  }

  const float font_px =
      options.emphasize && options.indent_level > 0 ? font_size::kChip : 13.0F;
  const Font label_font = (options.emphasize || options.selected)
                              ? Font::System(font_px).WithWeight(FontWeight::SemiBold)
                              : Font::System(font_px);
  children.Add(Text(options.label)
                   .Style(TextStyle{label_font, foreground})
                   .LineLimit(1)
                   .Overflow(TextOverflow::Ellipsis)
                   .With(Grow(1.0F)));

  if (!options.trailing_text.empty()) {
    children.Add(Text(options.trailing_text)
                     .Style(TextStyle{Font::System(font_size::kCaption), secondary})
                     .LineLimit(1));
  }

  View row = Row{std::move(children)}.With(
      Frame{.min_height = 26.0F},
      Padding{EdgeInsets{.top = 4.0F, .right = 8.0F, .bottom = 4.0F, .left = left_padding}},
      Spacing{5.0F}, CrossAlign{CrossAxisAlignment::Center}, CornerRadius{6.0F},
      Background{VisualFill{bg_color}}, Border{border_color, 1.0F},
      InteractionSink{interaction});

  if (on_click) {
    return std::move(row).OnClick(std::move(on_click));
  }
  return row;
}

[[huxerui::composable]] View WorkspaceTabItem(WorkspaceTabItemOptions options,
                                              std::function<void()> on_select,
                                              std::function<void()> on_close) {
  const ThemeSpec& spec = UseTheme();
  const SemanticColors& semantic = UseEnvironment<SemanticColors>();
  auto interaction = UseState(InteractionState{});
  const InteractionState state = interaction.Get();
  const bool hovered = state.enabled && (state.hovered || state.pressed || state.focus_visible);

  const Color foreground =
      options.active ? spec.colors.on_surface : spec.colors.on_surface_variant;
  const Color secondary =
      options.active ? spec.colors.on_surface_variant : WithAlpha(spec.colors.on_surface_variant, 0.78F);
  const Color bg_color =
      options.active
          ? spec.colors.surface
          : (state.pressed ? semantic.press_fill
                           : (hovered ? semantic.hover_fill : spec.colors.surface_container));
  const Color border_color =
      options.active ? semantic.border_strong
                     : (hovered ? semantic.border_subtle : Color::Transparent());

  const Font label_font =
      options.active ? Font::System(font_size::kChip).WithWeight(FontWeight::SemiBold)
                     : Font::System(font_size::kChip).WithWeight(FontWeight::Medium);

  Views label_parts;
  if (options.kind_accent.has_value()) {
    label_parts.Add(AccentDot(*options.kind_accent, 6.0F));
  }
  if (options.dirty) {
    label_parts.Add(
        Text("●").Style(TextStyle{Font::System(font_size::kCaption), semantic.warning}));
  }
  label_parts.Add(Text(options.label)
                      .Style(TextStyle{label_font, foreground})
                      .LineLimit(1)
                      .Overflow(TextOverflow::Ellipsis));
  if (!options.sublabel.empty()) {
    label_parts.Add(Text(options.sublabel)
                        .Style(TextStyle{Font::System(font_size::kCaption), secondary})
                        .LineLimit(1)
                        .Overflow(TextOverflow::Ellipsis));
  }

  View label_area =
      Row{std::move(label_parts)}
          .With(Padding{EdgeInsets{.top = 4.0F, .right = 4.0F, .bottom = 4.0F, .left = 10.0F}},
                Spacing{5.0F}, CrossAlign{CrossAxisAlignment::Center})
          .OnClick(std::move(on_select));

  View close_area =
      Text("×")
          .Style(TextStyle{Font::System(13.0F), secondary})
          .With(Padding{EdgeInsets{.top = 4.0F, .right = 8.0F, .bottom = 4.0F, .left = 4.0F}},
                CornerRadius{4.0F})
          .OnClick(std::move(on_close));

  const VisualFill indicator_fill =
      options.active ? (options.is_sql ? VisualFill{semantic.accent_sql} : semantic.accent_gradient)
                     : VisualFill{Color::Transparent()};

  return Column{
      Row{std::move(label_area), std::move(close_area)}
          .With(Spacing{0.0F}, CrossAlign{CrossAxisAlignment::Center}),
      Spacer().With(Grow(0.0F), Frame{.height = 2.5F, .min_height = 2.5F, .max_height = 2.5F},
                    Background{indicator_fill}),
  }.With(Frame{.max_width = 240.0F, .min_height = 30.0F}, Spacing{0.0F},
         CrossAlign{CrossAxisAlignment::Stretch}, CornerRadius{6.0F}, ClipChildren{},
         Background{VisualFill{bg_color}}, Border{border_color, 1.0F},
         InteractionSink{interaction});
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
  return Text(SanitizeUtf8(text)).Style(
      TextStyle{Font::System(font_size::kCaption), spec.colors.on_surface_variant});
}

[[huxerui::composable]] View ErrorText(std::string text) {
  const ThemeSpec& spec = UseTheme();
  return Text(SanitizeUtf8(text)).Style(
      TextStyle{Font::System(font_size::kChip).WithWeight(FontWeight::SemiBold), spec.colors.error});
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
  const SemanticColors& semantic = UseEnvironment<SemanticColors>();
  View card = std::move(content);
  return std::move(card).With(Background{VisualFill{spec.colors.surface_container_highest}},
                              CornerRadius{10.0F}, Border{semantic.border_subtle, 1.0F},
                              Padding{EdgeInsets::Symmetric(20.0F, 18.0F)});
}

} // namespace dbpp::ui
