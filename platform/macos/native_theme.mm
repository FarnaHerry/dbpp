#import <AppKit/AppKit.h>

namespace dbpp {

// Syncs NSApplication appearance so the AppKit status-bar menu renders in
// dark or light aqua matching db++'s active ThemeMode preference.
void ApplyMacOsTrayAppearance(int mode, bool effective_dark) noexcept {
  @autoreleasepool {
    if (NSApp == nil) {
      return;
    }
    // ThemeMode::System == 0 -> restore system inheritance (nil).
    if (mode == 0) {
      NSApp.appearance = nil;
      return;
    }
    NSAppearanceName name =
        effective_dark ? NSAppearanceNameDarkAqua : NSAppearanceNameAqua;
    NSApp.appearance = [NSAppearance appearanceNamed:name];
  }
}

} // namespace dbpp
