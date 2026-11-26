#pragma once

namespace dbpp::ui {

enum class ButtonTone {
  Default,
  Primary,
  Danger,
  DangerSolid,
  Subtle,
};

huxerui::View ActionButton(huxerui::View action, ButtonTone tone = ButtonTone::Default);
huxerui::View AccentDot(huxerui::Color color, float size = 7.0F);
huxerui::View TintedIcon(const huxerui::ImageResource& resource, huxerui::Color color,
                         float size = 14.0F);
huxerui::View DatabaseIcon(dbpp::DatabaseBrand brand, float size = 16.0F);
huxerui::View DatabaseBadgeIcon(dbpp::DatabaseBrand brand, float icon_size = 20.0F,
                                float tile_size = 34.0F);
huxerui::View StatusBadge(std::string text, huxerui::Color foreground,
                          huxerui::Color background,
                          huxerui::Color border = huxerui::Color::Transparent());
huxerui::View TitleBarButton(std::string label, std::function<void()> on_click,
                             bool active = false,
                             const huxerui::ImageResource* leading_icon = nullptr);
huxerui::View NavItem(std::string label, std::string detail, bool selected,
                      std::function<void()> on_click, huxerui::View trailing = {},
                      std::optional<huxerui::Color> accent_dot = std::nullopt,
                      huxerui::View leading = {});

struct TreeRowOptions {
  std::string prefix;
  std::string label;
  std::string trailing_text;
  int indent_level = 0;
  bool selected = false;
  bool emphasize = false;
  bool active_hint = false;
  bool muted = false;
  std::optional<huxerui::Color> prefix_color{};
  std::optional<huxerui::Color> status_dot{};
  huxerui::View leading_icon{};
  std::optional<bool> expanded{};
};

huxerui::View TreeRow(TreeRowOptions options, std::function<void()> on_click = {});

struct WorkspaceTabItemOptions {
  std::string label;
  std::string sublabel;
  bool active = false;
  bool dirty = false;
  std::optional<huxerui::Color> kind_accent{};
  bool is_sql = false;
  huxerui::View leading_icon{};
};

huxerui::View WorkspaceTabItem(WorkspaceTabItemOptions options,
                               std::function<void()> on_select,
                               std::function<void()> on_close);
huxerui::View PageTitle(std::string title, huxerui::View actions);
huxerui::View Caption(std::string text);
huxerui::View MutedText(std::string text);
huxerui::View ErrorText(std::string text);
huxerui::View MonospaceField(huxerui::View field);
huxerui::View PasswordField(huxerui::State<huxerui::TextEditingValue> value,
                            huxerui::StringVariant label,
                            std::function<void()> on_submitted = {});
huxerui::View DialogCard(huxerui::View content);
inline constexpr int kDefaultLoadingDelayMs = 200;
huxerui::View LoadingState(huxerui::StringVariant label = {},
                           int delay_ms = kDefaultLoadingDelayMs);
huxerui::View LoadingOverlay(huxerui::View content, bool loading,
                             huxerui::StringVariant label = {},
                             int delay_ms = kDefaultLoadingDelayMs);

} // namespace dbpp::ui
