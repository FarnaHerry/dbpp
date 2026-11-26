#include <huxerui/huxerui.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

import dbpp.settings;

#include "theme_palette.h"

namespace dbpp::ui {

using namespace huxerui;

namespace {

LinearGradient HorizontalGradient(Color a, Color b) {
  return LinearGradient{
      .start = {0.0F, 0.5F},
      .end = {1.0F, 0.5F},
      .stops =
          {
              GradientStop{.offset = 0.0F, .color = a},
              GradientStop{.offset = 1.0F, .color = b},
          },
      .transform = {},
  };
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
      .center = {0.5F, 0.36F},
      .radius = {0.55F, 0.50F},
      .stops =
          {
              GradientStop{.offset = 0.0F, .color = inner},
              GradientStop{.offset = 0.55F, .color = mid},
              GradientStop{.offset = 1.0F, .color = Color::Transparent()},
          },
      .transform = {},
  };
}

} // namespace

Color WithAlpha(Color color, float alpha) {
  color.alpha = alpha;
  return color;
}

SemanticColors SemanticColors::Default() {
  return BuildSemanticColors(ThemePreset::GeminiSpark, false);
}

const std::vector<ThemePresetMeta>& AvailableThemePresets() {
  static const std::vector<ThemePresetMeta> kPresets = {
      ThemePresetMeta{
          .preset = ThemePreset::GeminiSpark,
          .name = "Gemini 星光",
          .subtitle = "蓝紫珊瑚渐变 · 四色语义高光",
          .swatch_a = Color::Rgb(66, 133, 244),
          .swatch_b = Color::Rgb(155, 114, 203),
          .swatch_c = Color::Rgb(217, 101, 112),
          .swatch_bar = TriGradient(Color::Rgb(66, 133, 244), Color::Rgb(155, 114, 203),
                                    Color::Rgb(217, 101, 112)),
      },
      ThemePresetMeta{
          .preset = ThemePreset::Emerald,
          .name = "翡翠青",
          .subtitle = "低蓝光清透石板灰 · 护眼数据工程风",
          .swatch_a = Color::Rgb(13, 148, 136),
          .swatch_b = Color::Rgb(16, 185, 129),
          .swatch_c = Color::Rgb(56, 189, 248),
          .swatch_bar = TriGradient(Color::Rgb(15, 118, 110), Color::Rgb(16, 185, 129),
                                    Color::Rgb(56, 189, 248)),
      },
      ThemePresetMeta{
          .preset = ThemePreset::WarmAmber,
          .name = "暖沙琥珀",
          .subtitle = "柔和暖纸调表面 · 琥珀日落活力",
          .swatch_a = Color::Rgb(217, 119, 6),
          .swatch_b = Color::Rgb(245, 158, 11),
          .swatch_c = Color::Rgb(234, 88, 12),
          .swatch_bar = TriGradient(Color::Rgb(217, 119, 6), Color::Rgb(245, 158, 11),
                                    Color::Rgb(239, 68, 68)),
      },
      ThemePresetMeta{
          .preset = ThemePreset::VioletDusk,
          .name = "暮光紫",
          .subtitle = "深灰蓝层次 · 紫玫霓虹焦点",
          .swatch_a = Color::Rgb(109, 40, 217),
          .swatch_b = Color::Rgb(139, 92, 246),
          .swatch_c = Color::Rgb(236, 72, 153),
          .swatch_bar = TriGradient(Color::Rgb(109, 40, 217), Color::Rgb(139, 92, 246),
                                    Color::Rgb(236, 72, 153)),
      },
      ThemePresetMeta{
          .preset = ThemePreset::Monochrome,
          .name = "极简黑白",
          .subtitle = "柔和中性灰阶分层 · 高对比纯简",
          .swatch_a = Color::Rgb(24, 24, 27),
          .swatch_b = Color::Rgb(113, 113, 122),
          .swatch_c = Color::Rgb(228, 228, 231),
          .swatch_bar = HorizontalGradient(Color::Rgb(24, 24, 27), Color::Rgb(113, 113, 122)),
      },
  };
  return kPresets;
}

bool ResolveIsDark(ThemeMode mode, bool system_dark) {
  switch (mode) {
    case ThemeMode::Dark: return true;
    case ThemeMode::Light: return false;
    case ThemeMode::System:
    default: return system_dark;
  }
}

ThemeSpec BuildThemeSpec(ThemePreset preset, bool dark) {
  ThemeSpec spec = dark ? MaterialDarkThemeSpec() : MaterialLightThemeSpec();
  spec.typography = TypographyScheme{
      .body_large = 16.0F,
      .body_medium = font_size::kBody,
      .body_small = font_size::kChip,
      .label_large = font_size::kBody,
      .title_large = font_size::kTitle,
      .headline_small = 24.0F,
  };

  auto& c = spec.colors;
  switch (preset) {
    case ThemePreset::GeminiSpark:
      if (dark) {
        c.primary = Color::Rgb(106, 166, 255);
        c.on_primary = Color::Rgb(12, 20, 36);
        c.primary_container = Color::Rgb(30, 50, 86);
        c.on_primary_container = Color::Rgb(218, 234, 255);
        c.secondary = Color::Rgb(182, 146, 246);
        c.on_secondary = Color::Rgb(22, 14, 42);
        c.secondary_container = Color::Rgb(42, 34, 68);
        c.on_secondary_container = Color::Rgb(233, 224, 255);
        c.tertiary_container = Color::Rgb(64, 34, 52);
        c.on_tertiary_container = Color::Rgb(255, 222, 232);
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
      } else {
        c.primary = Color::Rgb(66, 133, 244);
        c.on_primary = Color::Rgb(255, 255, 255);
        c.primary_container = Color::Rgb(226, 238, 255);
        c.on_primary_container = Color::Rgb(22, 68, 150);
        c.secondary = Color::Rgb(139, 92, 246);
        c.on_secondary = Color::Rgb(255, 255, 255);
        c.secondary_container = Color::Rgb(238, 232, 255);
        c.on_secondary_container = Color::Rgb(68, 38, 138);
        c.tertiary_container = Color::Rgb(253, 232, 236);
        c.on_tertiary_container = Color::Rgb(138, 36, 54);
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
      }
      break;

    case ThemePreset::Emerald:
      if (dark) {
        c.primary = Color::Rgb(45, 212, 191);
        c.on_primary = Color::Rgb(10, 30, 28);
        c.primary_container = Color::Rgb(22, 58, 54);
        c.on_primary_container = Color::Rgb(204, 251, 241);
        c.secondary = Color::Rgb(56, 189, 248);
        c.on_secondary = Color::Rgb(10, 28, 40);
        c.secondary_container = Color::Rgb(26, 48, 62);
        c.on_secondary_container = Color::Rgb(224, 242, 254);
        c.tertiary_container = Color::Rgb(42, 52, 46);
        c.on_tertiary_container = Color::Rgb(220, 245, 232);
        c.background = Color::Rgb(13, 21, 20);
        c.surface_container_low = Color::Rgb(18, 29, 27);
        c.surface = Color::Rgb(22, 34, 32);
        c.surface_container = Color::Rgb(28, 42, 40);
        c.surface_container_high = Color::Rgb(33, 49, 46);
        c.surface_container_highest = Color::Rgb(35, 52, 49);
        c.on_surface = Color::Rgb(233, 245, 242);
        c.on_surface_variant = Color::Rgb(142, 174, 168);
        c.outline = Color::Rgb(84, 114, 108);
        c.inverse_surface = Color::Rgb(233, 245, 242);
        c.inverse_on_surface = Color::Rgb(16, 26, 24);
        c.scrim = Color::Rgb(6, 12, 11, 0.56F);
        c.error = Color::Rgb(248, 113, 113);
      } else {
        c.primary = Color::Rgb(13, 148, 136);
        c.on_primary = Color::Rgb(255, 255, 255);
        c.primary_container = Color::Rgb(218, 247, 240);
        c.on_primary_container = Color::Rgb(17, 94, 89);
        c.secondary = Color::Rgb(2, 132, 199);
        c.on_secondary = Color::Rgb(255, 255, 255);
        c.secondary_container = Color::Rgb(224, 242, 254);
        c.on_secondary_container = Color::Rgb(12, 74, 110);
        c.tertiary_container = Color::Rgb(236, 246, 241);
        c.on_tertiary_container = Color::Rgb(24, 72, 54);
        c.background = Color::Rgb(244, 249, 247);
        c.surface_container_low = Color::Rgb(234, 243, 239);
        c.surface = Color::Rgb(255, 255, 255);
        c.surface_container = Color::Rgb(230, 240, 236);
        c.surface_container_high = Color::Rgb(223, 236, 230);
        c.surface_container_highest = Color::Rgb(227, 239, 234);
        c.on_surface = Color::Rgb(22, 36, 34);
        c.on_surface_variant = Color::Rgb(86, 112, 107);
        c.outline = Color::Rgb(144, 172, 166);
        c.inverse_surface = Color::Rgb(22, 36, 34);
        c.inverse_on_surface = Color::Rgb(244, 249, 247);
        c.scrim = Color::Rgb(10, 24, 22, 0.42F);
        c.error = Color::Rgb(217, 63, 76);
      }
      break;

    case ThemePreset::WarmAmber:
      if (dark) {
        c.primary = Color::Rgb(245, 158, 11);
        c.on_primary = Color::Rgb(34, 22, 6);
        c.primary_container = Color::Rgb(66, 46, 20);
        c.on_primary_container = Color::Rgb(254, 243, 199);
        c.secondary = Color::Rgb(251, 146, 60);
        c.on_secondary = Color::Rgb(38, 20, 8);
        c.secondary_container = Color::Rgb(58, 40, 26);
        c.on_secondary_container = Color::Rgb(255, 237, 213);
        c.tertiary_container = Color::Rgb(54, 42, 34);
        c.on_tertiary_container = Color::Rgb(248, 232, 218);
        c.background = Color::Rgb(22, 19, 16);
        c.surface_container_low = Color::Rgb(29, 25, 21);
        c.surface = Color::Rgb(34, 29, 24);
        c.surface_container = Color::Rgb(42, 36, 30);
        c.surface_container_high = Color::Rgb(49, 42, 35);
        c.surface_container_highest = Color::Rgb(52, 44, 37);
        c.on_surface = Color::Rgb(246, 240, 232);
        c.on_surface_variant = Color::Rgb(176, 162, 144);
        c.outline = Color::Rgb(116, 102, 86);
        c.inverse_surface = Color::Rgb(246, 240, 232);
        c.inverse_on_surface = Color::Rgb(28, 24, 20);
        c.scrim = Color::Rgb(14, 10, 6, 0.56F);
        c.error = Color::Rgb(248, 113, 113);
      } else {
        c.primary = Color::Rgb(202, 106, 6);
        c.on_primary = Color::Rgb(255, 255, 255);
        c.primary_container = Color::Rgb(254, 240, 214);
        c.on_primary_container = Color::Rgb(138, 62, 8);
        c.secondary = Color::Rgb(234, 88, 12);
        c.on_secondary = Color::Rgb(255, 255, 255);
        c.secondary_container = Color::Rgb(255, 237, 213);
        c.on_secondary_container = Color::Rgb(124, 45, 18);
        c.tertiary_container = Color::Rgb(244, 236, 224);
        c.on_tertiary_container = Color::Rgb(86, 62, 38);
        c.background = Color::Rgb(250, 247, 242);
        c.surface_container_low = Color::Rgb(242, 236, 226);
        c.surface = Color::Rgb(255, 253, 249);
        c.surface_container = Color::Rgb(239, 232, 220);
        c.surface_container_high = Color::Rgb(234, 225, 210);
        c.surface_container_highest = Color::Rgb(237, 228, 214);
        c.on_surface = Color::Rgb(42, 34, 26);
        c.on_surface_variant = Color::Rgb(120, 106, 90);
        c.outline = Color::Rgb(176, 160, 140);
        c.inverse_surface = Color::Rgb(42, 34, 26);
        c.inverse_on_surface = Color::Rgb(250, 247, 242);
        c.scrim = Color::Rgb(28, 20, 12, 0.42F);
        c.error = Color::Rgb(217, 63, 76);
      }
      break;

    case ThemePreset::VioletDusk:
      if (dark) {
        c.primary = Color::Rgb(167, 139, 250);
        c.on_primary = Color::Rgb(22, 14, 44);
        c.primary_container = Color::Rgb(46, 36, 86);
        c.on_primary_container = Color::Rgb(237, 233, 254);
        c.secondary = Color::Rgb(244, 114, 182);
        c.on_secondary = Color::Rgb(44, 12, 30);
        c.secondary_container = Color::Rgb(64, 30, 56);
        c.on_secondary_container = Color::Rgb(252, 231, 243);
        c.tertiary_container = Color::Rgb(42, 38, 68);
        c.on_tertiary_container = Color::Rgb(226, 222, 252);
        c.background = Color::Rgb(18, 16, 28);
        c.surface_container_low = Color::Rgb(24, 21, 38);
        c.surface = Color::Rgb(28, 25, 44);
        c.surface_container = Color::Rgb(35, 31, 54);
        c.surface_container_high = Color::Rgb(41, 36, 63);
        c.surface_container_highest = Color::Rgb(44, 38, 68);
        c.on_surface = Color::Rgb(240, 238, 252);
        c.on_surface_variant = Color::Rgb(162, 154, 196);
        c.outline = Color::Rgb(98, 90, 136);
        c.inverse_surface = Color::Rgb(240, 238, 252);
        c.inverse_on_surface = Color::Rgb(22, 18, 36);
        c.scrim = Color::Rgb(10, 8, 20, 0.58F);
        c.error = Color::Rgb(248, 113, 113);
      } else {
        c.primary = Color::Rgb(109, 40, 217);
        c.on_primary = Color::Rgb(255, 255, 255);
        c.primary_container = Color::Rgb(237, 233, 254);
        c.on_primary_container = Color::Rgb(76, 29, 149);
        c.secondary = Color::Rgb(219, 39, 119);
        c.on_secondary = Color::Rgb(255, 255, 255);
        c.secondary_container = Color::Rgb(252, 231, 243);
        c.on_secondary_container = Color::Rgb(131, 24, 67);
        c.tertiary_container = Color::Rgb(238, 234, 250);
        c.on_tertiary_container = Color::Rgb(66, 48, 114);
        c.background = Color::Rgb(247, 245, 252);
        c.surface_container_low = Color::Rgb(238, 234, 248);
        c.surface = Color::Rgb(255, 255, 255);
        c.surface_container = Color::Rgb(234, 229, 246);
        c.surface_container_high = Color::Rgb(227, 221, 242);
        c.surface_container_highest = Color::Rgb(231, 226, 245);
        c.on_surface = Color::Rgb(28, 24, 46);
        c.on_surface_variant = Color::Rgb(102, 94, 134);
        c.outline = Color::Rgb(158, 150, 188);
        c.inverse_surface = Color::Rgb(28, 24, 46);
        c.inverse_on_surface = Color::Rgb(247, 245, 252);
        c.scrim = Color::Rgb(18, 14, 34, 0.42F);
        c.error = Color::Rgb(217, 63, 76);
      }
      break;

    case ThemePreset::Monochrome:
      if (dark) {
        c.primary = Color::Rgb(244, 244, 245);
        c.on_primary = Color::Rgb(18, 18, 20);
        c.primary_container = Color::Rgb(46, 46, 52);
        c.on_primary_container = Color::Rgb(248, 248, 250);
        c.secondary = Color::Rgb(161, 161, 170);
        c.on_secondary = Color::Rgb(18, 18, 20);
        c.secondary_container = Color::Rgb(38, 38, 42);
        c.on_secondary_container = Color::Rgb(228, 228, 231);
        c.tertiary_container = Color::Rgb(42, 42, 46);
        c.on_tertiary_container = Color::Rgb(228, 228, 231);
        c.background = Color::Rgb(11, 11, 13);
        c.surface_container_low = Color::Rgb(18, 18, 21);
        c.surface = Color::Rgb(24, 24, 28);
        c.surface_container = Color::Rgb(31, 31, 36);
        c.surface_container_high = Color::Rgb(38, 38, 44);
        c.surface_container_highest = Color::Rgb(42, 42, 48);
        c.on_surface = Color::Rgb(244, 244, 245);
        c.on_surface_variant = Color::Rgb(161, 161, 170);
        c.outline = Color::Rgb(92, 92, 102);
        c.inverse_surface = Color::Rgb(244, 244, 245);
        c.inverse_on_surface = Color::Rgb(18, 18, 20);
        c.scrim = Color::Rgb(0, 0, 0, 0.56F);
        c.error = Color::Rgb(248, 113, 113);
      } else {
        c.primary = Color::Rgb(24, 24, 27);
        c.on_primary = Color::Rgb(255, 255, 255);
        c.primary_container = Color::Rgb(228, 228, 234);
        c.on_primary_container = Color::Rgb(18, 18, 22);
        c.secondary = Color::Rgb(82, 82, 91);
        c.on_secondary = Color::Rgb(255, 255, 255);
        c.secondary_container = Color::Rgb(236, 236, 240);
        c.on_secondary_container = Color::Rgb(39, 39, 42);
        c.tertiary_container = Color::Rgb(232, 232, 236);
        c.on_tertiary_container = Color::Rgb(39, 39, 42);
        c.background = Color::Rgb(245, 245, 247);
        c.surface_container_low = Color::Rgb(236, 236, 240);
        c.surface = Color::Rgb(255, 255, 255);
        c.surface_container = Color::Rgb(234, 234, 238);
        c.surface_container_high = Color::Rgb(226, 226, 231);
        c.surface_container_highest = Color::Rgb(230, 230, 235);
        c.on_surface = Color::Rgb(24, 24, 27);
        c.on_surface_variant = Color::Rgb(102, 102, 112);
        c.outline = Color::Rgb(161, 161, 172);
        c.inverse_surface = Color::Rgb(24, 24, 27);
        c.inverse_on_surface = Color::Rgb(250, 250, 252);
        c.scrim = Color::Rgb(0, 0, 0, 0.40F);
        c.error = Color::Rgb(217, 63, 76);
      }
      break;
  }

  const SemanticColors semantic = BuildSemanticColors(preset, dark);
  spec.interactions.indication =
      TintedIndication(semantic.hover_fill, semantic.press_fill, spec.colors.primary);
  spec.interactions.focus_ring.color = spec.colors.primary;
  return spec;
}

SemanticColors BuildSemanticColors(ThemePreset preset, bool dark) {
  SemanticColors sem;
  if (dark) {
    sem.success = Color::Rgb(74, 222, 128);
    sem.success_container = Color::Rgb(20, 56, 36);
    sem.warning = Color::Rgb(251, 191, 36);
    sem.warning_container = Color::Rgb(62, 46, 18);
    sem.danger = Color::Rgb(248, 113, 113);
    sem.danger_solid = Color::Rgb(220, 53, 69);
    sem.on_danger = Color::Rgb(255, 255, 255);
    sem.danger_container = Color::Rgb(68, 28, 34);
    sem.on_primary_button = Color::Rgb(255, 255, 255);
  } else {
    sem.success = Color::Rgb(24, 148, 92);
    sem.success_container = Color::Rgb(220, 248, 232);
    sem.warning = Color::Rgb(194, 114, 0);
    sem.warning_container = Color::Rgb(254, 243, 214);
    sem.danger = Color::Rgb(217, 63, 76);
    sem.danger_solid = Color::Rgb(217, 63, 76);
    sem.on_danger = Color::Rgb(255, 255, 255);
    sem.danger_container = Color::Rgb(254, 232, 234);
    sem.on_primary_button = Color::Rgb(255, 255, 255);
  }

  switch (preset) {
    case ThemePreset::GeminiSpark:
      sem.border_subtle =
          dark ? Color::Rgb(126, 148, 190, 0.24F) : Color::Rgb(120, 138, 174, 0.28F);
      sem.border_strong =
          dark ? Color::Rgb(138, 166, 220, 0.48F) : Color::Rgb(86, 112, 164, 0.48F);
      sem.hover_fill =
          dark ? Color::Rgb(128, 168, 255, 0.11F) : Color::Rgb(66, 133, 244, 0.08F);
      sem.press_fill =
          dark ? Color::Rgb(128, 168, 255, 0.18F) : Color::Rgb(66, 133, 244, 0.14F);
      sem.accent_table = dark ? Color::Rgb(106, 166, 255) : Color::Rgb(66, 133, 244);
      sem.accent_view = dark ? Color::Rgb(182, 146, 246) : Color::Rgb(139, 92, 246);
      sem.accent_sql = dark ? Color::Rgb(240, 138, 149) : Color::Rgb(155, 114, 203);
      sem.accent_external = sem.warning;
      sem.category_local = sem.success;
      sem.category_relational = sem.accent_table;
      sem.category_analytics = sem.accent_view;
      sem.category_custom = sem.warning;
      sem.null_text = dark ? Color::Rgb(138, 152, 186) : Color::Rgb(124, 136, 164);
      sem.draft_accent = sem.warning;
      sem.accent_gradient = dark
                                ? TriGradient(Color::Rgb(96, 165, 250), Color::Rgb(182, 146, 246),
                                              Color::Rgb(240, 138, 149))
                                : TriGradient(Color::Rgb(66, 133, 244), Color::Rgb(155, 114, 203),
                                              Color::Rgb(217, 101, 112));
      sem.primary_button_fill =
          dark ? TriGradient(Color::Rgb(58, 120, 240), Color::Rgb(131, 92, 230),
                             Color::Rgb(179, 100, 158))
               : TriGradient(Color::Rgb(56, 125, 244), Color::Rgb(124, 99, 224),
                             Color::Rgb(166, 106, 178));
      sem.hero_glow = dark ? SoftGlow(Color::Rgb(96, 165, 250, 0.13F),
                                      Color::Rgb(182, 146, 246, 0.08F))
                           : SoftGlow(Color::Rgb(66, 133, 244, 0.10F),
                                      Color::Rgb(155, 114, 203, 0.06F));
      sem.uses_gradient_chrome = true;
      break;

    case ThemePreset::Emerald:
      sem.border_subtle =
          dark ? Color::Rgb(110, 152, 144, 0.24F) : Color::Rgb(110, 144, 136, 0.28F);
      sem.border_strong =
          dark ? Color::Rgb(94, 212, 190, 0.46F) : Color::Rgb(13, 148, 136, 0.45F);
      sem.hover_fill =
          dark ? Color::Rgb(45, 212, 191, 0.11F) : Color::Rgb(13, 148, 136, 0.08F);
      sem.press_fill =
          dark ? Color::Rgb(45, 212, 191, 0.18F) : Color::Rgb(13, 148, 136, 0.14F);
      sem.accent_table = dark ? Color::Rgb(45, 212, 191) : Color::Rgb(13, 148, 136);
      sem.accent_view = dark ? Color::Rgb(56, 189, 248) : Color::Rgb(2, 132, 199);
      sem.accent_sql = dark ? Color::Rgb(74, 222, 128) : Color::Rgb(16, 185, 129);
      sem.accent_external = sem.warning;
      sem.category_local = sem.success;
      sem.category_relational = sem.accent_view;
      sem.category_analytics = sem.accent_table;
      sem.category_custom = sem.warning;
      sem.null_text = dark ? Color::Rgb(132, 164, 158) : Color::Rgb(116, 142, 136);
      sem.draft_accent = sem.warning;
      sem.accent_gradient =
          TriGradient(dark ? Color::Rgb(45, 212, 191) : Color::Rgb(13, 148, 136),
                      Color::Rgb(16, 185, 129), Color::Rgb(56, 189, 248));
      sem.primary_button_fill =
          HorizontalGradient(Color::Rgb(15, 118, 110), Color::Rgb(16, 163, 127));
      sem.hero_glow = dark ? SoftGlow(Color::Rgb(45, 212, 191, 0.12F),
                                      Color::Rgb(56, 189, 248, 0.07F))
                           : SoftGlow(Color::Rgb(13, 148, 136, 0.09F),
                                      Color::Rgb(56, 189, 248, 0.05F));
      sem.uses_gradient_chrome = true;
      break;

    case ThemePreset::WarmAmber:
      sem.border_subtle =
          dark ? Color::Rgb(164, 144, 120, 0.24F) : Color::Rgb(148, 130, 108, 0.28F);
      sem.border_strong =
          dark ? Color::Rgb(245, 158, 11, 0.46F) : Color::Rgb(202, 106, 6, 0.45F);
      sem.hover_fill =
          dark ? Color::Rgb(245, 158, 11, 0.11F) : Color::Rgb(202, 106, 6, 0.08F);
      sem.press_fill =
          dark ? Color::Rgb(245, 158, 11, 0.18F) : Color::Rgb(202, 106, 6, 0.14F);
      sem.accent_table = dark ? Color::Rgb(245, 158, 11) : Color::Rgb(202, 106, 6);
      sem.accent_view = dark ? Color::Rgb(251, 146, 60) : Color::Rgb(234, 88, 12);
      sem.accent_sql = dark ? Color::Rgb(248, 113, 113) : Color::Rgb(220, 82, 60);
      sem.accent_external = sem.warning;
      sem.category_local = sem.success;
      sem.category_relational = dark ? Color::Rgb(96, 165, 250) : Color::Rgb(37, 99, 235);
      sem.category_analytics = sem.accent_view;
      sem.category_custom = sem.warning;
      sem.null_text = dark ? Color::Rgb(162, 148, 130) : Color::Rgb(140, 126, 110);
      sem.draft_accent = sem.warning;
      sem.accent_gradient =
          TriGradient(Color::Rgb(217, 119, 6), Color::Rgb(245, 158, 11), Color::Rgb(234, 88, 12));
      sem.primary_button_fill =
          HorizontalGradient(Color::Rgb(194, 96, 12), Color::Rgb(224, 116, 20));
      sem.hero_glow = dark ? SoftGlow(Color::Rgb(245, 158, 11, 0.11F),
                                      Color::Rgb(234, 88, 12, 0.06F))
                           : SoftGlow(Color::Rgb(217, 119, 6, 0.09F),
                                      Color::Rgb(234, 88, 12, 0.05F));
      sem.uses_gradient_chrome = true;
      break;

    case ThemePreset::VioletDusk:
      sem.border_subtle =
          dark ? Color::Rgb(142, 132, 186, 0.24F) : Color::Rgb(136, 126, 176, 0.28F);
      sem.border_strong =
          dark ? Color::Rgb(167, 139, 250, 0.48F) : Color::Rgb(109, 40, 217, 0.45F);
      sem.hover_fill =
          dark ? Color::Rgb(167, 139, 250, 0.11F) : Color::Rgb(109, 40, 217, 0.08F);
      sem.press_fill =
          dark ? Color::Rgb(167, 139, 250, 0.18F) : Color::Rgb(109, 40, 217, 0.14F);
      sem.accent_table = dark ? Color::Rgb(167, 139, 250) : Color::Rgb(109, 40, 217);
      sem.accent_view = dark ? Color::Rgb(244, 114, 182) : Color::Rgb(219, 39, 119);
      sem.accent_sql = dark ? Color::Rgb(129, 140, 248) : Color::Rgb(79, 70, 229);
      sem.accent_external = sem.warning;
      sem.category_local = sem.success;
      sem.category_relational = dark ? Color::Rgb(129, 140, 248) : Color::Rgb(79, 70, 229);
      sem.category_analytics = sem.accent_view;
      sem.category_custom = sem.warning;
      sem.null_text = dark ? Color::Rgb(152, 144, 184) : Color::Rgb(128, 120, 158);
      sem.draft_accent = sem.warning;
      sem.accent_gradient =
          TriGradient(Color::Rgb(109, 40, 217), Color::Rgb(167, 139, 250), Color::Rgb(236, 72, 153));
      sem.primary_button_fill =
          HorizontalGradient(Color::Rgb(109, 40, 217), Color::Rgb(168, 85, 247));
      sem.hero_glow = dark ? SoftGlow(Color::Rgb(139, 92, 246, 0.13F),
                                      Color::Rgb(236, 72, 153, 0.07F))
                           : SoftGlow(Color::Rgb(109, 40, 217, 0.10F),
                                      Color::Rgb(219, 39, 119, 0.05F));
      sem.uses_gradient_chrome = true;
      break;

    case ThemePreset::Monochrome: {
      const Color accent = dark ? Color::Rgb(244, 244, 245) : Color::Rgb(24, 24, 27);
      const Color subtle_ink = dark ? Color::Rgb(161, 161, 170) : Color::Rgb(113, 113, 122);
      sem.border_subtle =
          dark ? Color::Rgb(161, 161, 170, 0.24F) : Color::Rgb(113, 113, 122, 0.28F);
      sem.border_strong =
          dark ? Color::Rgb(228, 228, 231, 0.45F) : Color::Rgb(39, 39, 42, 0.45F);
      sem.hover_fill =
          dark ? Color::Rgb(255, 255, 255, 0.08F) : Color::Rgb(0, 0, 0, 0.06F);
      sem.press_fill =
          dark ? Color::Rgb(255, 255, 255, 0.14F) : Color::Rgb(0, 0, 0, 0.12F);
      sem.on_primary_button = dark ? Color::Rgb(18, 18, 20) : Color::Rgb(255, 255, 255);
      sem.accent_table = accent;
      sem.accent_view = subtle_ink;
      sem.accent_sql = accent;
      sem.accent_external = subtle_ink;
      sem.category_local = subtle_ink;
      sem.category_relational = subtle_ink;
      sem.category_analytics = subtle_ink;
      sem.category_custom = subtle_ink;
      sem.null_text = subtle_ink;
      sem.draft_accent = dark ? Color::Rgb(228, 228, 231) : Color::Rgb(63, 63, 70);
      sem.accent_gradient = VisualFill{sem.border_subtle};
      sem.primary_button_fill = VisualFill{accent};
      sem.hero_glow = VisualFill{Color::Transparent()};
      sem.uses_gradient_chrome = false;
      break;
    }
  }
  return sem;
}

ResolvedTheme ResolveTheme(const AppThemeConfig& config, bool system_dark) {
  const bool dark = ResolveIsDark(config.mode, system_dark);
  return ResolvedTheme{
      .preset = config.preset,
      .dark = dark,
      .spec = BuildThemeSpec(config.preset, dark),
      .semantic = BuildSemanticColors(config.preset, dark),
  };
}

Indication TintedIndication(Color hover_fill, Color press_fill, Color focus_border) {
  return Indication{
      .geometry = {},
      .focus =
          IndicationLayer{
              .fill = {},
              .border = Border{focus_border, 2.0F},
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

ThemeDefinition AppThemeDefinition(const ResolvedTheme& resolved) {
  const ThemeSpec& spec = resolved.spec;
  const SemanticColors& semantic = resolved.semantic;
  const bool dark = resolved.dark;

  ThemeDefinition definition = MaterialThemeDefinition(spec);
  definition.Set(semantic);

  ButtonStyle buttons = ButtonStyle::Default();
  buttons.background = VisualFill{spec.colors.surface};
  buttons.label_style = TextStyle{Font::System(font_size::kBody), spec.colors.on_surface};
  buttons.disabled_background = VisualFill{WithAlpha(spec.colors.on_surface, 0.06F)};
  buttons.disabled_label = WithAlpha(spec.colors.on_surface, 0.38F);
  buttons.padding = EdgeInsets::Symmetric(12.0F, 6.0F);
  buttons.minimum_height = 30.0F;
  buttons.corner_radii = CornerRadii{6.0F};
  buttons.indication = spec.interactions.indication;
  definition.Set(buttons);

  IconButtonStyle icon_buttons = IconButtonStyle::Default();
  icon_buttons.foreground = spec.colors.on_surface;
  icon_buttons.disabled_foreground = WithAlpha(spec.colors.on_surface, 0.38F);
  icon_buttons.indication = spec.interactions.indication;
  definition.Set(icon_buttons);

  SegmentedButtonStyle segments = SegmentedButtonStyle::Default();
  segments.background = VisualFill{spec.colors.surface};
  segments.selected_background = VisualFill{spec.colors.primary_container};
  segments.label_style = TextStyle{Font::System(font_size::kChip), spec.colors.on_surface_variant};
  segments.selected_label = spec.colors.on_primary_container;
  segments.border = Border{semantic.border_subtle, 1.0F};
  segments.selected_border = Border{WithAlpha(spec.colors.primary, 0.55F), 1.0F};
  segments.padding = EdgeInsets::Symmetric(10.0F, 5.0F);
  segments.minimum_height = 28.0F;
  segments.corner_radii = CornerRadii{6.0F};
  segments.indication = spec.interactions.indication;
  segments.selected_indication =
      TintedIndication(semantic.hover_fill, semantic.press_fill, spec.colors.primary);
  definition.Set(segments);

  ChipStyle chips = ChipStyle::Default();
  chips.background = VisualFill{spec.colors.surface_container};
  chips.selected_background = VisualFill{spec.colors.primary_container};
  chips.label_style = TextStyle{Font::System(font_size::kChip), spec.colors.on_surface};
  chips.selected_label = spec.colors.on_primary_container;
  chips.border = Border{semantic.border_subtle, 1.0F};
  chips.selected_border = Border{WithAlpha(spec.colors.primary, 0.50F), 1.0F};
  chips.padding = EdgeInsets::Symmetric(10.0F, 4.0F);
  chips.minimum_height = 24.0F;
  chips.corner_radii = CornerRadii{12.0F};
  chips.indication = spec.interactions.indication;
  definition.Set(chips);

  SwitchStyle switches = SwitchStyle::Default();
  switches.unchecked_track = WithAlpha(spec.colors.outline, dark ? 0.38F : 0.32F);
  switches.checked_track = spec.colors.primary;
  switches.unchecked_track_border = semantic.border_subtle;
  switches.checked_track_border = Color::Transparent();
  switches.unchecked_thumb = spec.colors.surface;
  switches.checked_thumb = spec.colors.on_primary;
  definition.Set(switches);

  ProgressBarStyle bars = ProgressBarStyle::Default();
  bars.track_color = WithAlpha(spec.colors.outline, 0.22F);
  bars.indicator_color = spec.colors.primary;
  bars.corner_radius = 2.0F;
  definition.Set(bars);

  ProgressCircleStyle circles = ProgressCircleStyle::Default();
  circles.size = 28.0F;
  circles.stroke_width = 3.0F;
  circles.track_color = WithAlpha(spec.colors.outline, 0.22F);
  circles.indeterminate_track_color = WithAlpha(spec.colors.outline, 0.18F);
  circles.indicator_color = spec.colors.primary;
  circles.indeterminate_motion = ProgressCircleIndeterminateMotion::PulsingArc;
  circles.minimum_indeterminate_arc_fraction = 0.12F;
  circles.maximum_indeterminate_arc_fraction = 0.80F;
  circles.animation_duration = spec.motion.reduced_motion ? 0.0 : 6.0;
  definition.Set(circles);

  TextFieldStyle text_fields = TextFieldStyle::Default();
  text_fields.variant = TextFieldVariant::Outlined;
  text_fields.show_label = true;
  text_fields.outlined.background = VisualFill{spec.colors.surface};
  text_fields.outlined.disabled_background =
      VisualFill{WithAlpha(spec.colors.surface_container_low, 0.65F)};
  text_fields.outlined.border = semantic.border_subtle;
  text_fields.outlined.hovered_border = semantic.border_strong;
  text_fields.outlined.focused_border = spec.colors.primary;
  text_fields.outlined.disabled_border = WithAlpha(spec.colors.on_surface, 0.12F);
  text_fields.outlined.corner_radii = CornerRadii{6.0F};
  text_fields.outlined.minimum_height = 42.0F;
  text_fields.text_style = TextStyle{Font::System(font_size::kBody), spec.colors.on_surface};
  text_fields.label_style =
      TextStyle{Font::System(font_size::kBody), spec.colors.on_surface_variant};
  text_fields.floating_label_style =
      TextStyle{Font::System(font_size::kChip), spec.colors.on_surface_variant};
  text_fields.placeholder_style =
      TextStyle{Font::System(font_size::kBody), WithAlpha(spec.colors.on_surface_variant, 0.72F)};
  text_fields.disabled_text = WithAlpha(spec.colors.on_surface, 0.38F);
  text_fields.disabled_label = WithAlpha(spec.colors.on_surface, 0.38F);
  text_fields.disabled_placeholder = WithAlpha(spec.colors.on_surface, 0.38F);
  text_fields.disabled_supporting_text = WithAlpha(spec.colors.on_surface, 0.38F);
  text_fields.focused_label = spec.colors.primary;
  text_fields.error_label = spec.colors.error;
  text_fields.validation_error = spec.colors.error;
  text_fields.validation_text_style =
      TextStyle{Font::System(font_size::kChip), spec.colors.error};
  text_fields.leading_icon = spec.colors.on_surface_variant;
  text_fields.focused_leading_icon = spec.colors.primary;
  text_fields.disabled_leading_icon = WithAlpha(spec.colors.on_surface, 0.38F);
  text_fields.error_leading_icon = spec.colors.error;
  text_fields.trailing_icon = spec.colors.on_surface_variant;
  text_fields.focused_trailing_icon = spec.colors.primary;
  text_fields.disabled_trailing_icon = WithAlpha(spec.colors.on_surface, 0.38F);
  text_fields.error_trailing_icon = spec.colors.error;
  text_fields.selection = WithAlpha(spec.colors.primary, 0.24F);
  text_fields.caret = spec.colors.primary;
  text_fields.error_caret = spec.colors.error;
  text_fields.composition = spec.colors.secondary;
  text_fields.border_width = 1.0F;
  text_fields.focused_border_width = 2.0F;
  text_fields.padding = EdgeInsets::Symmetric(12.0F, 8.0F);
  definition.Set(text_fields);

  const Color soft_shadow = Color::Rgb(0, 0, 0, dark ? 0.44F : 0.15F);

  DialogStyle dialogs = DialogStyle::Default();
  dialogs.scrim = spec.colors.scrim;
  dialogs.background = VisualFill{spec.colors.surface_container_highest};
  dialogs.shadow = Shadow{soft_shadow, Point{0.0F, 8.0F}, 24.0F, 0.0F};
  dialogs.title_style =
      TextStyle{Font::System(font_size::kTitle).WithWeight(FontWeight::Bold), spec.colors.on_surface};
  dialogs.message_style = TextStyle{Font::System(font_size::kBody), spec.colors.on_surface};
  dialogs.positive_action_style =
      TextStyle{Font::System(font_size::kBody), semantic.on_primary_button};
  dialogs.positive_action_background = semantic.primary_button_fill;
  dialogs.positive_action_indication =
      TintedIndication(Color::Rgb(255, 255, 255, 0.14F), Color::Rgb(255, 255, 255, 0.22F),
                       spec.colors.on_primary);
  dialogs.negative_action_style =
      TextStyle{Font::System(font_size::kBody), spec.colors.on_surface};
  dialogs.negative_action_indication = spec.interactions.indication;
  dialogs.action_separator_color = semantic.border_subtle;
  dialogs.corner_radii = CornerRadii{10.0F};
  definition.Set(dialogs);

  MenuStyle menus = MenuStyle::Default();
  menus.background = VisualFill{spec.colors.surface_container_high};
  menus.foreground = spec.colors.on_surface;
  menus.icon_tint = spec.colors.on_surface_variant;
  menus.item_indication = spec.interactions.indication;
  menus.separator_color = semantic.border_subtle;
  menus.separator_mode = MenuSeparatorMode::BetweenSections;
  menus.item_padding = EdgeInsets::Symmetric(12.0F, 6.0F);
  menus.minimum_item_height = 30.0F;
  menus.shadow = Shadow{soft_shadow, Point{0.0F, 4.0F}, 16.0F, 0.0F};
  menus.corner_radii = CornerRadii{8.0F};
  definition.Set(menus);

  DividerStyle dividers = DividerStyle::Default();
  dividers.color = semantic.border_subtle;
  dividers.thickness = 1.0F;
  definition.Set(dividers);

  ToastStyle toasts = ToastStyle::Default();
  toasts.background = VisualFill{spec.colors.inverse_surface};
  toasts.text_style = TextStyle{Font::System(font_size::kChip), spec.colors.inverse_on_surface};
  toasts.padding = EdgeInsets::Symmetric(14.0F, 10.0F);
  toasts.shadow = Shadow{soft_shadow, Point{0.0F, 4.0F}, 14.0F, 0.0F};
  toasts.corner_radii = CornerRadii{8.0F};
  toasts.maximum_width = 560.0F;
  definition.Set(toasts);

  return definition;
}

ThemeDefinition OverrideButtonStyle(ButtonStyle style) {
  ThemeDefinition definition;
  definition.Set(std::move(style));
  return definition;
}

ThemeDefinition OverrideTextFieldStyle(TextFieldStyle style) {
  ThemeDefinition definition;
  definition.Set(std::move(style));
  return definition;
}

} // namespace dbpp::ui
