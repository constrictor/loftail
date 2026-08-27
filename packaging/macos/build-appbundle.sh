#!/usr/bin/env bash
# loftail — a desktop viewer for log4cplus logs.
# Copyright (C) 2026 Valentyn Pavliuchenko
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
#
# SPDX-License-Identifier: GPL-3.0-or-later

# Build a self-contained loftail.app on macOS and wrap it in a .dmg.
#
# Configures + builds loftail (Release) as a MACOSX_BUNDLE (see
# src/CMakeLists.txt), runs macdeployqt to copy the Qt frameworks and plugins
# into the bundle, and produces a .dmg. The bundle runs on a Mac with no Qt
# installed.
#
# SIGNING / NOTARIZATION WARNING:
#   This produces an UNSIGNED bundle. Distributed as-is, Gatekeeper will warn
#   users ("loftail can't be opened because Apple cannot check it for malicious
#   software") and they must right-click > Open or clear the quarantine bit
#   (`xattr -dr com.apple.quarantine loftail.app`). For real distribution, pass
#   a Developer ID identity to macdeployqt (`-codesign=<identity>`) and notarize
#   the .dmg with `notarytool`. Codesigning/notarization is intentionally out of
#   scope for this baseline artifact (PLAN.md M7).
#
# STATUS: authored on Linux, NOT yet run/verified on macOS. Run this on a Mac
# with Qt 6 (>= 6.4) installed, then clean-machine-verify per packaging/README.md.
#
# Usage:
#   packaging/macos/build-appbundle.sh [build-type]
#
# Environment overrides:
#   BUILD_DIR   build tree            (default: build-macos)
#   OUTPUT_DIR  where the .dmg lands  (default: dist)
#   QT_DIR      Qt prefix (contains bin/macdeployqt); else PATH/CMAKE_PREFIX_PATH
#   CODESIGN_IDENTITY  optional Developer ID for -codesign (unsigned if empty)
set -euo pipefail

BUILD_TYPE="${1:-Release}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO_ROOT"

BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build-macos}"
OUTPUT_DIR="${OUTPUT_DIR:-$REPO_ROOT/dist}"
mkdir -p "$OUTPUT_DIR"

CMAKE_ARGS=(-S "$REPO_ROOT" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE="$BUILD_TYPE")
if [[ -n "${QT_DIR:-}" ]]; then
    CMAKE_ARGS+=(-DCMAKE_PREFIX_PATH="$QT_DIR")
    MACDEPLOYQT="$QT_DIR/bin/macdeployqt"
else
    MACDEPLOYQT="$(command -v macdeployqt || echo macdeployqt)"
fi

echo ">> Configuring ($BUILD_TYPE)"
cmake "${CMAKE_ARGS[@]}"

echo ">> Building"
cmake --build "$BUILD_DIR"

# The release this artifact belongs to, read back from the configured build rather than
# repeated here — see the same three lines in build-appimage.sh and build-portable.ps1.
VERSION=$(sed -n 's/^CMAKE_PROJECT_VERSION:STATIC=//p' "$BUILD_DIR/CMakeCache.txt")
if [[ -z "$VERSION" ]]; then
    echo "!! could not read CMAKE_PROJECT_VERSION from $BUILD_DIR/CMakeCache.txt" >&2
    exit 1
fi

# Locate the built bundle (qt_add_executable + MACOSX_BUNDLE -> loftail.app).
APP="$(find "$BUILD_DIR" -maxdepth 4 -name 'loftail.app' -type d | head -n1)"
if [[ -z "$APP" ]]; then
    echo "!! loftail.app not found under $BUILD_DIR" >&2
    exit 1
fi
echo ">> Bundle: $APP"

# The licence travels with the binary (GPLv3 §4). BEFORE macdeployqt, not after: it
# takes -dmg, so the disk image is produced in the same run and a file added afterwards
# would be in the .app on this machine and in nothing that ships. The bundle's own
# NSHumanReadableCopyright names the terms; this is the copy of them.
cp "$REPO_ROOT/LICENSE" "$APP/Contents/Resources/LICENSE"

echo ">> Running macdeployqt"
DEPLOY_ARGS=("$APP" -dmg)
if [[ -n "${CODESIGN_IDENTITY:-}" ]]; then
    DEPLOY_ARGS+=("-codesign=$CODESIGN_IDENTITY")
else
    echo "!! No CODESIGN_IDENTITY set — producing an UNSIGNED bundle (Gatekeeper will warn)."
fi
"$MACDEPLOYQT" "${DEPLOY_ARGS[@]}"

# macdeployqt writes loftail.dmg next to the .app; move it to OUTPUT_DIR.
DMG="$(dirname "$APP")/loftail.dmg"
if [[ -f "$DMG" ]]; then
    # Named for the release, and keeping the build type only when it is not the shipped
    # one, exactly as the AppImage and the Windows zip are.
    if [[ "$BUILD_TYPE" == "Release" ]]; then
        DEST="$OUTPUT_DIR/loftail-${VERSION}-macos.dmg"
    else
        DEST="$OUTPUT_DIR/loftail-${VERSION}-${BUILD_TYPE}-macos.dmg"
    fi
    mv -f "$DMG" "$DEST"
    echo ">> Built: $DEST"
    ls -lh "$DEST"
else
    echo "!! Expected DMG not found ($DMG); the .app is at $APP" >&2
    exit 1
fi
