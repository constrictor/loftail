#!/usr/bin/env bash
#
# Build a self-contained loftail AppImage on Ubuntu 24.04 (the reference build
# environment — ARCHITECTURE.md §1). The resulting AppImage bundles Qt via
# linuxdeploy + linuxdeploy-plugin-qt, so it runs on a machine with no system Qt.
#
# Usage:
#   packaging/linux/build-appimage.sh [build-type]
#
#   build-type   CMake build type for a fresh configure (default: Release).
#                Ignored if $BUILD_DIR already contains a configured build.
#
# Environment overrides:
#   BUILD_DIR    build tree to configure/build      (default: build-appimage)
#   APPDIR       staging AppDir                      (default: $BUILD_DIR/AppDir)
#   TOOLS_DIR    where linuxdeploy tools are cached  (default: packaging/linux/tools)
#   OUTPUT_DIR   where the .AppImage is written      (default: dist)
#   QMAKE        path to qmake6                       (default: autodetected)
#
# The linuxdeploy tools are downloaded from their upstream GitHub releases the
# first time and cached under TOOLS_DIR. Set them in TOOLS_DIR manually for an
# offline / air-gapped build.
set -euo pipefail

BUILD_TYPE="${1:-Release}"

# Resolve repo root from this script's location (packaging/linux/).
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO_ROOT"

BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build-appimage}"
APPDIR="${APPDIR:-$BUILD_DIR/AppDir}"
TOOLS_DIR="${TOOLS_DIR:-$SCRIPT_DIR/tools}"
OUTPUT_DIR="${OUTPUT_DIR:-$REPO_ROOT/dist}"

LINUXDEPLOY_URL="https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage"
LINUXDEPLOY_QT_URL="https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage"

mkdir -p "$TOOLS_DIR" "$OUTPUT_DIR"

fetch_tool() {
    local url="$1" dest="$2"
    if [[ ! -x "$dest" ]]; then
        echo ">> Fetching $(basename "$dest")"
        curl -fL --retry 3 -o "$dest" "$url"
        chmod +x "$dest"
    fi
}

fetch_tool "$LINUXDEPLOY_URL"    "$TOOLS_DIR/linuxdeploy-x86_64.AppImage"
fetch_tool "$LINUXDEPLOY_QT_URL" "$TOOLS_DIR/linuxdeploy-plugin-qt-x86_64.AppImage"

# Point linuxdeploy-plugin-qt at the right qmake so it bundles the matching Qt.
export QMAKE="${QMAKE:-$(command -v qmake6 || command -v qmake)}"
if [[ -z "${QMAKE:-}" ]]; then
    echo "!! qmake6/qmake not found — install Qt 6 dev packages" >&2
    exit 1
fi
echo ">> Using QMAKE=$QMAKE"

# 1. Configure + build (Release by default) if not already configured.
if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]]; then
    echo ">> Configuring ($BUILD_TYPE)"
    cmake -S "$REPO_ROOT" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
fi
echo ">> Building"
cmake --build "$BUILD_DIR"

# 2. Stage a clean install tree into the AppDir.
echo ">> Installing into $APPDIR"
rm -rf "$APPDIR"
DESTDIR="$APPDIR" cmake --install "$BUILD_DIR" --prefix /usr

# 3. Let linuxdeploy pull in Qt + build the AppImage.
#    --plugin qt bundles the Qt libraries, platform plugins, and QPA.
echo ">> Running linuxdeploy (+qt plugin)"
export OUTPUT="$OUTPUT_DIR/loftail-${BUILD_TYPE}-x86_64.AppImage"
# FUSE is often unavailable in CI/containers; extract-and-run avoids needing it.
export APPIMAGE_EXTRACT_AND_RUN=1
# Bundle the offscreen QPA plugin alongside xcb so the AppImage supports the
# headless `QT_QPA_PLATFORM=offscreen` smoke check (packaging/README.md, CI)
# on a machine with no X display. xcb is bundled by the qt plugin by default.
export EXTRA_PLATFORM_PLUGINS="${EXTRA_PLATFORM_PLUGINS:-libqoffscreen.so}"

"$TOOLS_DIR/linuxdeploy-x86_64.AppImage" \
    --appdir "$APPDIR" \
    --executable "$APPDIR/usr/bin/loftail" \
    --desktop-file "$APPDIR/usr/share/applications/loftail.desktop" \
    --icon-file "$APPDIR/usr/share/icons/hicolor/scalable/apps/loftail.svg" \
    --plugin qt \
    --output appimage

echo ">> Built: $OUTPUT"
ls -lh "$OUTPUT"
