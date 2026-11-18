#!/usr/bin/env bash
# build-appimage.sh — build the player and wrap it in an AppImage.
#
#   packaging/appimage/build-appimage.sh
#
# Produces: ./OPL3_RetroWave_MIDI_Player-<version>-<arch>.AppImage
#
# An AppImage runs on any distro whose glibc is at least as new as the one it
# was built against, so release builds are made inside an AlmaLinux 8 container
# (glibc 2.28; see .github/workflows/appimage.yml). Running this script directly
# on a current distro is fine for testing, but the result will only run on
# distros as new as that machine — do not publish it.
#
# Bundled: SDL2 and its non-core dependencies. Left to the host, as is usual
# for AppImages: glibc, X11/Wayland/GL, ALSA, and zenity/kdialog (used for the
# "Open Folder" dialog).

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

case "$(uname -m)" in
    x86_64)  ARCH=x86_64 ;;
    aarch64) ARCH=aarch64 ;;
    *) echo "ERROR: unsupported architecture $(uname -m)" >&2; exit 1 ;;
esac
export ARCH

VERSION="$(sed -n "s/^version: *'\(.*\)'.*/\1/p" snap/snapcraft.yaml | head -n1)"
VERSION="${VERSION:-dev}"

WORK="$ROOT/build/appimage"
APPDIR="$WORK/AppDir"
TOOLS="$WORK/tools"
LINUXDEPLOY="$TOOLS/linuxdeploy-$ARCH.AppImage"
OUT="$ROOT/OPL3_RetroWave_MIDI_Player-$VERSION-$ARCH.AppImage"

# ── 1. Build the player ───────────────────────────────────────────────────────
./build.sh

# ── 2. Fetch linuxdeploy (collects shared libraries, makes the AppImage) ──────
mkdir -p "$TOOLS"
if [ ! -x "$LINUXDEPLOY" ]; then
    echo ">>> Downloading linuxdeploy..."
    curl -fL --retry 3 -o "$LINUXDEPLOY.part" \
        "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-$ARCH.AppImage"
    chmod +x "$LINUXDEPLOY.part"
    mv "$LINUXDEPLOY.part" "$LINUXDEPLOY"
fi

# ── 3. Assemble the AppDir and package it ─────────────────────────────────────
rm -rf "$APPDIR"
rm -f "$OUT"

# Run the tools unpacked, so this also works where FUSE is missing (CI, containers).
export APPIMAGE_EXTRACT_AND_RUN=1
export LINUXDEPLOY_OUTPUT_VERSION="$VERSION"
export LDAI_OUTPUT="$OUT"
export OUTPUT="$OUT"          # name used by older linuxdeploy appimage plugins

"$LINUXDEPLOY" \
    --appdir "$APPDIR" \
    --executable "$ROOT/opl3-rw-midi-player" \
    --desktop-file "$ROOT/packaging/appimage/retro-midi-player-opl3-gm.desktop" \
    --icon-file "$ROOT/snap/gui/retro-midi-player-opl3-gm.png" \
    --output appimage

echo ""
echo ">>> Done: $OUT"
