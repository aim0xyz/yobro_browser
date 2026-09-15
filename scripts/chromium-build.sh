#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
QT_VERSION="6.11.2"
QT_PREFIX="${QT_PREFIX:-$(brew --prefix qt)}"
QT_WEBENGINE_PREFIX="${QT_WEBENGINE_PREFIX:-$(brew --prefix qtwebengine)}"
QT_PLUGIN_DIR="${QT_PLUGIN_DIR:-$("$QT_PREFIX/bin/qtpaths6" --plugin-dir)}"
BUILD_DIR="$ROOT/chromium/build"
STAGE_DIR="$ROOT/dist/chromium-macos-arm64"
APP="$STAGE_DIR/YOBRO Chromium Feasibility.app"

actual_qt_version="$("$QT_PREFIX/bin/qtpaths6" --qt-version)"
if [[ "$actual_qt_version" != "$QT_VERSION" ]]; then
  printf 'Expected Qt %s, found %s at %s\n' \
    "$QT_VERSION" "$actual_qt_version" "$QT_PREFIX" >&2
  exit 1
fi

cmake --fresh \
  -S "$ROOT/chromium" \
  -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_PREFIX_PATH="$QT_PREFIX" \
  -DYOBRO_QT_VERSION="$QT_VERSION" \
  -DYOBRO_BUILD_QT_SPIKE=ON \
  -DYOBRO_PACKAGE_APP=ON \
  -DYOBRO_QT_WEBENGINE_PREFIX="$QT_WEBENGINE_PREFIX" \
  -DYOBRO_QT_PLUGIN_DIR="$QT_PLUGIN_DIR"
cmake --build "$BUILD_DIR" --parallel "$(sysctl -n hw.ncpu)"
ctest --test-dir "$BUILD_DIR" --output-on-failure
cmake -E remove_directory "$STAGE_DIR"
cmake --install "$BUILD_DIR" --prefix "$STAGE_DIR"
python3 "$ROOT/scripts/chromium-validate-package.py" "$APP"

printf '%s\n' \
  "Built and deployed with pinned Qt $QT_VERSION:" \
  "$APP"
