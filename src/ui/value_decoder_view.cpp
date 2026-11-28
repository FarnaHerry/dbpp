#include "ui/ui_common.h"

#include <array>
#include <string>
#include <utility>
#include <vector>

namespace dbpp {

using namespace huxerui;
using namespace dbpp::ui;

namespace {

constexpr std::array<ValueDecoderKind, 9> kDecoderOrder = {
    ValueDecoderKind::Raw,     ValueDecoderKind::Json,    ValueDecoderKind::Base64,
    ValueDecoderKind::Gzip,    ValueDecoderKind::Jwt,     ValueDecoderKind::Url,
    ValueDecoderKind::Hex,     ValueDecoderKind::Unicode, ValueDecoderKind::Timestamp,
};

void CopyWithToast(const std::shared_ptr<Clipboard>& clipboard, const std::string& text,
                   const NotifyFn& notify, const LocalizedCatalog& loc,
                   StringResource label_res) {
  if (clipboard && clipboard->SupportsSynchronousText() && clipboard->WriteText(text)) {
    if (notify) {
      notify(loc.Fmt(app::strings::toast_copied, loc.Get(label_res)));
    }
  } else if (notify) {
    notify(loc.Get(app::strings::toast_clipboard_unsupported));
  }
}

} // namespace

[[huxerui::composable]] View DecoderSelectorButton(
    const MenuHandle& menu, const ViewHandle& anchor, const LocalizedCatalog& loc,
    ValueDecoderKind current, ValueDecoderKind suggested, bool enabled, float height,
    std::function<void(ValueDecoderKind)> on_select) {
  const ThemeSpec& spec = UseTheme();
  const SemanticColors& semantic = UseEnvironment<SemanticColors>();
  const bool active = (current != ValueDecoderKind::Raw);
  const std::string label =
      UseString(app::strings::decoder_label, std::string(DecoderDisplayName(current)));
  const Color fg = active ? spec.colors.on_primary_container : spec.colors.on_surface;
  const Color bg = active ? spec.colors.primary_container : spec.colors.surface;
  const Color border = active ? WithAlpha(spec.colors.primary, 0.45F) : semantic.border_subtle;
  const Color icon_color = active ? spec.colors.primary : spec.colors.on_surface_variant;

  Views row_items;
  row_items.Add(TintedIcon(app::images::visibility, icon_color, 13.0F));
  row_items.Add(
      Text(label)
          .Style(TextStyle{Font::System(font_size::kChip).WithWeight(FontWeight::Medium), fg})
          .LineLimit(1));
  if (suggested != ValueDecoderKind::Raw && current == ValueDecoderKind::Raw) {
    row_items.Add(
        Text(std::string("· ") + std::string(DecoderDisplayName(suggested)))
            .Style(TextStyle{Font::System(font_size::kCaption).WithWeight(FontWeight::Medium),
                             spec.colors.primary})
            .LineLimit(1));
  }
  row_items.Add(TintedIcon(app::images::chevron_down, icon_color, 12.0F));

  return Row{std::move(row_items)}
      .With(Frame{.height = height, .min_height = height, .max_height = height},
            Padding{EdgeInsets::Symmetric(8.0F, 0.0F)}, Spacing{5.0F},
            CrossAlign{CrossAxisAlignment::Center}, CornerRadius{6.0F},
            Background{VisualFill{bg}}, Border{border, 1.0F}, anchor, Enabled{enabled})
      .OnClick([menu, anchor, loc, current, suggested, on_select = std::move(on_select)] {
        std::vector<MenuEntry> entries;
        entries.reserve(kDecoderOrder.size() + 1);
        for (const ValueDecoderKind k : kDecoderOrder) {
          if (k == ValueDecoderKind::Json) {
            entries.emplace_back(MenuSection{});
          }
          std::string item_label(DecoderDisplayName(k));
          if (k != ValueDecoderKind::Raw && k == suggested) {
            item_label += " " + loc.Get(app::strings::decoder_auto_suffix);
          }
          entries.emplace_back(
              MenuItem(std::move(item_label),
                       [on_select, k] {
                         if (on_select) on_select(k);
                       })
                  .Checked(current == k));
        }
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

[[huxerui::composable]] View DecodedValuePreviewBox(
    const DecodedValueResult& decoded, int min_lines, int max_lines) {
  Views items;
  if (!decoded.error.empty()) {
    items.Add(ErrorText(decoded.error));
  } else if (!decoded.subtitle.empty()) {
    items.Add(MutedText(decoded.subtitle));
  }
  const std::string& content = decoded.ok ? decoded.text : decoded.error;
  items.Add(
      MonospaceField(
          TextField(TextEditingValue::FromText(content))
              .Variant(TextFieldVariant::Outlined)
              .LineLimits(TextFieldLineLimits::MultiLine(min_lines, max_lines))
              .OnChanged([](const TextEditingValue&) {})));
  return Column{std::move(items)}.With(Spacing{6.0F}, CrossAlign{CrossAxisAlignment::Stretch});
}

[[huxerui::composable]] View ValueDecodeDialog(
    DialogContext dialog, std::string title, std::string raw_value,
    std::shared_ptr<Clipboard> clipboard, NotifyFn notify) {
  const LocalizedCatalog loc{UseService<Resources>(), UseService<SettingsModel>()};
  const auto menu = UseMenu();
  const auto selector_anchor = UseViewHandle();
  const ValueDecoderKind suggested = DetectSuggestedDecoder(raw_value);
  auto active_kind = UseState(suggested);
  const DecodedValueResult decoded = DecodeValue(raw_value, active_kind.Get());

  return DialogCard(Column{
      Row{
          Text(UseString(app::strings::decoder_dialog_title, title), TextRole::Title)
              .LineLimit(1)
              .Overflow(TextOverflow::Ellipsis)
              .With(Grow(1.0F)),
          DecoderSelectorButton(
              menu, selector_anchor, loc, active_kind.Get(), suggested, true, 28.0F,
              [active_kind](ValueDecoderKind next) { active_kind = next; }),
          ActionButton(
              Button(app::strings::decoder_copy_decoded).OnClick([clipboard, decoded, notify, loc] {
                CopyWithToast(clipboard, decoded.ok ? decoded.text : decoded.error, notify, loc,
                              app::strings::table_copy_value_label);
              })),
          ActionButton(
              Button(app::strings::decoder_copy_raw).OnClick([clipboard, raw_value, notify, loc] {
                CopyWithToast(clipboard, raw_value, notify, loc,
                              app::strings::table_copy_value_label);
              })),
          ActionButton(Button(app::strings::action_close).OnClick([dialog] { dialog.Dismiss(); })),
      }.With(CrossAlign{CrossAxisAlignment::Center}, Spacing{6.0F}),
      DecodedValuePreviewBox(decoded, 8, 22),
  }.With(Frame{.width = 680.0F}, Spacing{10.0F}, CrossAlign{CrossAxisAlignment::Stretch}));
}

} // namespace dbpp
