#pragma once

#include <huxerui/huxerui.h>

#include <functional>
#include <string>

namespace dbpp::ui {

namespace font_size {
inline constexpr float kCaption = 11.0F;
inline constexpr float kChip = 12.0F;
inline constexpr float kBody = 14.0F;
inline constexpr float kTitle = 20.0F;
} // namespace font_size

[[nodiscard]] huxerui::Indication MonochromeIndication(huxerui::Color foreground);
[[nodiscard]] huxerui::ThemeSpec DarkThemeSpec();
[[nodiscard]] huxerui::ThemeSpec LightThemeSpec();
[[nodiscard]] huxerui::ThemeDefinition AppThemeDefinition(const huxerui::ThemeSpec& spec);

huxerui::View ActionButton(huxerui::View action, bool primary = false);
huxerui::View NavItem(std::string label, std::string detail, bool selected,
                      std::function<void()> on_click, huxerui::View trailing = {});
huxerui::View PageTitle(std::string title, huxerui::View actions);
huxerui::View Caption(std::string text);
huxerui::View MutedText(std::string text);
huxerui::View ErrorText(std::string text);
huxerui::View MonospaceField(huxerui::View field);
huxerui::View DialogCard(huxerui::View content);

} // namespace dbpp::ui
