#!/usr/bin/env bash
# run.sh — 运行 build/ 下的 db++；DBPP_BIN 可覆盖二进制路径。
set -euo pipefail
cd "$(dirname "$0")"

if [[ -n "${DBPP_BIN:-}" ]]; then
    BIN="$DBPP_BIN"
else
    BIN="build/dbpp"
fi

if [ ! -x "$BIN" ]; then
    echo "binary not found — run \`cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build --parallel 4\` first" >&2
    exit 1
fi

# Prefer the SQLite ADBC driver produced next to the binary. The compile-time
# path can become stale when the build tree is moved or copied.
if [[ -z "${DBPP_SQLITE_DRIVER:-}" ]]; then
    BIN_DIR="$(cd "$(dirname "$BIN")" && pwd)"
    DRIVER="$BIN_DIR/adbc/driver/sqlite/libadbc_driver_sqlite.so"
    if [ -f "$DRIVER" ]; then
        export DBPP_SQLITE_DRIVER="$DRIVER"
    fi
fi

# On Linux desktop dev runs, keep the user XDG hicolor icon synced with
# assets/icon.png so Wayland/X11 taskbars pick up logo changes without
# reinstalling the system package.
if [[ "$(uname -s)" == "Linux" && -f "assets/icon.png" && -n "${HOME:-}" && -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]]; then
    ICON_THEME_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/icons/hicolor"
    USER_ICON="$ICON_THEME_DIR/256x256/apps/dbpp.png"
    if ! cmp -s "assets/icon.png" "$USER_ICON" 2>/dev/null; then
        if mkdir -p "$(dirname "$USER_ICON")" 2>/dev/null && cp "assets/icon.png" "$USER_ICON" 2>/dev/null; then
            if command -v gtk-update-icon-cache >/dev/null 2>&1; then
                gtk-update-icon-cache -q -f -t "$ICON_THEME_DIR" 2>/dev/null || true
            else
                rm -f "$ICON_THEME_DIR/icon-theme.cache" 2>/dev/null || true
            fi
            rm -f "${XDG_CACHE_HOME:-$HOME/.cache}/icon-cache.kcache" 2>/dev/null || true
            if command -v dbus-send >/dev/null 2>&1 && [[ -n "${DBUS_SESSION_BUS_ADDRESS:-}" ]]; then
                dbus-send --session --type=signal /KIconLoader org.kde.KIconLoader.iconChanged int32:0 2>/dev/null || true
            fi
        fi
    fi
fi

exec "$BIN" "$@"
