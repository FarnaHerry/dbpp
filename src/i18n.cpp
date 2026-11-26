module;

#include <huxerui/resource.h>

module dbpp.i18n;

import std;
import dbpp.settings;

namespace dbpp {

namespace {

std::atomic<int> g_active_language{static_cast<int>(UiLanguage::En)};

bool StartsWithZhSubtag(std::string_view tag) noexcept {
  if (tag.size() < 2) {
    return false;
  }
  const char c0 = static_cast<char>(std::tolower(static_cast<unsigned char>(tag[0])));
  const char c1 = static_cast<char>(std::tolower(static_cast<unsigned char>(tag[1])));
  if (c0 != 'z' || c1 != 'h') {
    return false;
  }
  return tag.size() == 2 || tag[2] == '-' || tag[2] == '_';
}

std::string FormatIndexedTemplate(std::string_view pattern, std::span<const std::string_view> args) {
  std::string out;
  out.reserve(pattern.size() + args.size() * 8);
  for (std::size_t i = 0; i < pattern.size(); ++i) {
    if (pattern[i] == '{') {
      if (i + 1 < pattern.size() && pattern[i + 1] == '{') {
        out.push_back('{');
        ++i;
        continue;
      }
      const std::size_t close = pattern.find('}', i + 1);
      if (close != std::string_view::npos && close > i + 1) {
        bool all_digits = true;
        std::size_t index = 0;
        for (std::size_t j = i + 1; j < close; ++j) {
          const char ch = pattern[j];
          if (ch < '0' || ch > '9') {
            all_digits = false;
            break;
          }
          index = index * 10 + static_cast<std::size_t>(ch - '0');
        }
        if (all_digits && index < args.size()) {
          out.append(args[index]);
          i = close;
          continue;
        }
      }
    } else if (pattern[i] == '}' && i + 1 < pattern.size() && pattern[i + 1] == '}') {
      out.push_back('}');
      ++i;
      continue;
    }
    out.push_back(pattern[i]);
  }
  return out;
}

} // namespace

UiLanguage ResolveUiLanguage(LanguageMode mode, std::string_view system_locale_tag) noexcept {
  switch (mode) {
    case LanguageMode::Zh:
      return UiLanguage::Zh;
    case LanguageMode::En:
      return UiLanguage::En;
    case LanguageMode::System:
    default:
      return StartsWithZhSubtag(system_locale_tag) ? UiLanguage::Zh : UiLanguage::En;
  }
}

void SetActiveUiLanguage(UiLanguage language) noexcept {
  g_active_language.store(static_cast<int>(language), std::memory_order_relaxed);
}

UiLanguage ActiveUiLanguage() noexcept {
  return static_cast<UiLanguage>(g_active_language.load(std::memory_order_relaxed));
}

bool IsChineseUi() noexcept {
  return ActiveUiLanguage() == UiLanguage::Zh;
}

std::string_view Tr(std::string_view en, std::string_view zh) noexcept {
  return IsChineseUi() ? zh : en;
}

std::string TrFmt(std::string_view en_fmt, std::string_view zh_fmt,
                  std::initializer_list<std::string_view> args) {
  const std::vector<std::string_view> list(args);
  return FormatIndexedTemplate(Tr(en_fmt, zh_fmt), list);
}

std::optional<huxerui::Locale> EffectiveLocaleOverride(LanguageMode mode) {
  switch (mode) {
    case LanguageMode::Zh:
      return huxerui::Locale::FromLanguageTag("zh");
    case LanguageMode::En:
      return huxerui::Locale::FromLanguageTag("en");
    case LanguageMode::System:
    default:
      return std::nullopt;
  }
}

huxerui::Locale EffectiveLocale(LanguageMode mode, const huxerui::Locale& platform_locale) {
  if (auto override_locale = EffectiveLocaleOverride(mode)) {
    return *override_locale;
  }
  return platform_locale;
}

} // namespace dbpp
