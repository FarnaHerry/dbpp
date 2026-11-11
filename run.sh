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

exec "$BIN" "$@"
