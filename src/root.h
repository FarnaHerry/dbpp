#pragma once

#include <huxerui/huxerui.h>

/// Root view factory shared by the application declaration and the tray
/// runtime's window rebuild path (they must present the same UI).
huxerui::View AppRoot();
