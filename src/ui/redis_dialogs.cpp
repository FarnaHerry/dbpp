#include "ui/ui_common.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dbpp {

using namespace huxerui;
using namespace dbpp::ui;

[[huxerui::composable]] View CustomDelimiterDialog(
    DialogContext dialog, std::string initial_delim, std::function<void(std::string)> on_apply) {
  auto value = UseState(TextEditingValue::FromText(std::move(initial_delim)));
  const auto submit = [dialog, value, on_apply = std::move(on_apply)] {
    const std::string next = value.Get().text;
    dialog.Dismiss();
    if (on_apply) {
      on_apply(next);
    }
  };
  return DialogCard(Column{
      Row{
          Text(app::strings::redis_tree_delim_dialog_title, TextRole::Title).With(Grow(1.0F)),
          ActionButton(Button(app::strings::action_cancel).OnClick([dialog] { dialog.Dismiss(); })),
          ActionButton(Button(app::strings::action_apply).OnClick(submit), ButtonTone::Primary),
      }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{8.0F}),
      MonospaceField(
          TextField(value)
              .Placeholder(app::strings::redis_tree_delim_dialog_ph)
              .Variant(TextFieldVariant::Outlined)
              .LineLimits(TextFieldLineLimits::SingleLine())
              .OnChanged([value](const TextEditingValue& next) { value = next; })
              .OnSubmitted(submit)),
  }.With(Frame{.width = 400.0F}, Spacing{12.0F}, CrossAlign{CrossAxisAlignment::Stretch}));
}

[[huxerui::composable]] View RenameKeyDialog(
    DialogContext dialog, std::string old_key, std::function<void(std::string)> on_apply) {
  auto value = UseState(TextEditingValue::FromText(std::move(old_key)));
  const auto submit = [dialog, value, on_apply = std::move(on_apply)] {
    const std::string next = value.Get().text;
    if (next.empty()) {
      return;
    }
    dialog.Dismiss();
    if (on_apply) {
      on_apply(next);
    }
  };
  return DialogCard(Column{
      Row{
          Text(app::strings::redis_detail_rename_title, TextRole::Title).With(Grow(1.0F)),
          ActionButton(Button(app::strings::action_cancel).OnClick([dialog] { dialog.Dismiss(); })),
          ActionButton(Button(app::strings::action_save).OnClick(submit), ButtonTone::Primary)
              .With(Enabled{!value.Get().text.empty()}),
      }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{8.0F}),
      MonospaceField(
          TextField(value)
              .Placeholder(app::strings::redis_detail_rename_ph)
              .Variant(TextFieldVariant::Outlined)
              .LineLimits(TextFieldLineLimits::SingleLine())
              .OnChanged([value](const TextEditingValue& next) { value = next; })
              .OnSubmitted(submit)),
  }.With(Frame{.width = 440.0F}, Spacing{12.0F}, CrossAlign{CrossAxisAlignment::Stretch}));
}

[[huxerui::composable]] View AddRedisKeyDialog(
    DialogContext dialog, std::function<void(NewRedisKeyInput)> on_submit) {
  static constexpr std::array<std::string_view, 5> kTypes = {
      "string", "hash", "list", "set", "zset"};
  auto key_input = UseState(TextEditingValue{});
  auto type_index = UseState<std::size_t>(0);
  auto ttl_input = UseState(TextEditingValue::FromText("-1"));
  auto val_input = UseState(TextEditingValue{});

  const std::size_t idx = std::min<std::size_t>(type_index.Get(), kTypes.size() - 1);
  const std::string_view selected_type = kTypes[idx];
  const StringVariant val_placeholder = [&]() -> StringVariant {
    if (selected_type == "hash") return StringVariant("{\"field\":\"value\"}");
    if (selected_type == "list" || selected_type == "set") {
      return StringVariant("[\"item1\",\"item2\"]");
    }
    if (selected_type == "zset") return StringVariant("[{\"member\":\"a\",\"score\":1}]");
    return StringVariant(app::strings::redis_add_key_val_ph_string);
  }();

  const auto submit = [dialog, key_input, type_index, ttl_input, val_input,
                       on_submit = std::move(on_submit)] {
    const std::string key = key_input.Get().text;
    if (key.empty()) {
      return;
    }
    const std::size_t cur_idx = std::min<std::size_t>(type_index.Get(), kTypes.size() - 1);
    NewRedisKeyInput input{
        .key = key,
        .type = std::string(kTypes[cur_idx]),
        .value = val_input.Get().text,
        .ttl = ttl_input.Get().text,
    };
    dialog.Dismiss();
    if (on_submit) {
      on_submit(std::move(input));
    }
  };

  return DialogCard(Column{
      Row{
          Text(app::strings::redis_add_key_title, TextRole::Title).With(Grow(1.0F)),
          ActionButton(Button(app::strings::action_cancel).OnClick([dialog] { dialog.Dismiss(); })),
          ActionButton(Button(app::strings::action_save).OnClick(submit), ButtonTone::Primary)
              .With(Enabled{!key_input.Get().text.empty()}),
      }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{8.0F}),
      Caption(UseString(app::strings::redis_add_key_name_label)),
      MonospaceField(
          TextField(key_input)
              .Placeholder(app::strings::redis_add_key_name_ph)
              .Variant(TextFieldVariant::Outlined)
              .LineLimits(TextFieldLineLimits::SingleLine())
              .OnChanged([key_input](const TextEditingValue& next) { key_input = next; })
              .OnSubmitted(submit)),
      Caption(UseString(app::strings::redis_add_key_type_label)),
      SegmentedButton({"string", "hash", "list", "set", "zset"}, idx)
          .OnChanged([type_index](std::size_t next) { type_index = next; }),
      Caption(UseString(app::strings::redis_add_key_ttl_label)),
      MonospaceField(
          TextField(ttl_input)
              .Placeholder(app::strings::redis_detail_ttl_ph)
              .Variant(TextFieldVariant::Outlined)
              .LineLimits(TextFieldLineLimits::SingleLine())
              .OnChanged([ttl_input](const TextEditingValue& next) { ttl_input = next; })),
      Caption(UseString(app::strings::redis_add_key_val_label)),
      MonospaceField(
          TextField(val_input)
              .Placeholder(val_placeholder)
              .Variant(TextFieldVariant::Outlined)
              .LineLimits(TextFieldLineLimits::MultiLine(4, 10))
              .OnChanged([val_input](const TextEditingValue& next) { val_input = next; })),
  }.With(Frame{.width = 480.0F}, Spacing{10.0F}, CrossAlign{CrossAxisAlignment::Stretch}));
}

[[huxerui::composable]] View RedisDelimiterFilterButton(
    const MenuHandle& menu, const ViewHandle& anchor, const DialogHandle& dialogs,
    const LocalizedCatalog& loc, const std::string& current_delim, bool enabled, float height,
    AppThemeConfig theme_config, LanguageMode language_mode, bool system_dark,
    std::function<void(std::string)> on_select) {
  const ThemeSpec& spec = UseTheme();
  const SemanticColors& semantic = UseEnvironment<SemanticColors>();
  const bool active = !current_delim.empty();
  const std::string label = active ? UseString(app::strings::redis_tree_delim_label, current_delim)
                                   : UseString(app::strings::redis_tree_delim_flat);
  const Color fg = active ? spec.colors.on_primary_container : spec.colors.on_surface;
  const Color bg = active ? spec.colors.primary_container : spec.colors.surface;
  const Color border = active ? WithAlpha(spec.colors.primary, 0.45F) : semantic.border_subtle;
  const Color icon_color = active ? spec.colors.primary : spec.colors.on_surface_variant;

  return Row{
      TintedIcon(app::images::folder, icon_color, 13.0F),
      Text(label)
          .Style(TextStyle{Font::System(font_size::kChip).WithWeight(FontWeight::Medium), fg})
          .LineLimit(1),
      TintedIcon(app::images::chevron_down, icon_color, 12.0F),
  }
      .With(Frame{.height = height, .min_height = height, .max_height = height},
            Padding{EdgeInsets::Symmetric(8.0F, 0.0F)}, Spacing{5.0F},
            CrossAlign{CrossAxisAlignment::Center}, CornerRadius{6.0F},
            Background{VisualFill{bg}}, Border{border, 1.0F}, anchor, Enabled{enabled})
      .OnClick([menu, anchor, dialogs, loc, current_delim, theme_config, language_mode,
                system_dark, on_select = std::move(on_select)] {
        std::vector<MenuEntry> entries;
        static constexpr std::array<std::string_view, 5> kPresets = {":", "/", ".", "-", "_"};
        for (const std::string_view d : kPresets) {
          const std::string ds(d);
          entries.emplace_back(
              MenuItem(ds,
                       [on_select, ds] {
                         if (on_select) on_select(ds);
                       })
                  .Checked(current_delim == d));
        }
        entries.emplace_back(MenuSection{});
        entries.emplace_back(
            MenuItem(loc.Get(app::strings::redis_tree_delim_menu_flat),
                     [on_select] {
                       if (on_select) on_select({});
                     })
                .Checked(current_delim.empty()));
        entries.emplace_back(
            MenuItem(loc.Get(app::strings::redis_tree_delim_custom),
                     [dialogs, current_delim, theme_config, language_mode, system_dark,
                      on_select] {
                       dialogs.Show([current_delim, theme_config, language_mode, system_dark,
                                     on_select](DialogContext dialog) {
                         return WithDialogTheme(
                             theme_config, language_mode, system_dark,
                             CustomDelimiterDialog(dialog, current_delim, on_select));
                       });
                     }));
        menu.Show(anchor, std::move(entries),
                  MenuOptions{
                      .placement = {.side = AnchorSide::Below,
                                    .alignment = AnchorAlignment::Start},
                      .gap = 4.0F,
                      .viewport_margin = 8.0F,
                      .offset = {},
                      .width = std::nullopt,
                      .dismiss_on_outside_press = true,
                      .dismiss_on_cancel = true,
                      .on_dismiss_request = {},
                  });
      });
}

} // namespace dbpp
