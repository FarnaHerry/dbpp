module;

#include <huxerui/resource.h>

export module dbpp.i18n;

import std;
import dbpp.settings;

export namespace dbpp {

/// Effective UI language used for runtime formatting of non-composable/worker messages
/// (`db.cpp`, `adbc_session.cpp`, `driver_installer.cpp`, `redis_client.cpp`).
/// Declarative UI views use HuxerUI's localized string resources (`resources/strings/*.properties`),
/// while both share the same resolved language synchronized from `SettingsModel::language`.
enum class UiLanguage {
  En = 0,
  Zh = 1,
};

[[nodiscard]] UiLanguage ResolveUiLanguage(LanguageMode mode,
                                           std::string_view system_locale_tag) noexcept;
void SetActiveUiLanguage(UiLanguage language) noexcept;
[[nodiscard]] UiLanguage ActiveUiLanguage() noexcept;
[[nodiscard]] bool IsChineseUi() noexcept;

[[nodiscard]] std::string_view Tr(std::string_view en, std::string_view zh) noexcept;
[[nodiscard]] std::string TrFmt(std::string_view en_fmt, std::string_view zh_fmt,
                                std::initializer_list<std::string_view> args);

[[nodiscard]] std::optional<huxerui::Locale> EffectiveLocaleOverride(LanguageMode mode);
[[nodiscard]] huxerui::Locale EffectiveLocale(LanguageMode mode,
                                              const huxerui::Locale& platform_locale);

} // namespace dbpp
