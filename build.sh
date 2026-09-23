#!/usr/bin/env bash
# build.sh — build libADLMIDI (static) and then the OPL3 RetroWave MIDI Player.
#
#   ./build.sh        full build (fetches + builds libADLMIDI, then the player)
#   ./build.sh fast   rebuild just the player after editing src/
#
# Produces: ./opl3-rw-midi-player
#
# SDL2 is REQUIRED (the GUI and the audio engine both use it). We do NOT patch
# libADLMIDI: this player forwards OPL register writes to the RetroWave board
# itself (src/retrowave_serial.c), so libADLMIDI's own serial backend is unused.

set -euo pipefail

# ── Sanitize LD_LIBRARY_PATH (foreign libstdc++ from some IDEs breaks g++) ─────
if [ -n "${LD_LIBRARY_PATH:-}" ]; then
    CLEAN_LLP=""
    IFS=: read -ra LLP_PARTS <<< "$LD_LIBRARY_PATH"
    for p in "${LLP_PARTS[@]}"; do
        if [ -f "$p/libstdc++.so.6" ] && [[ "$p" != /usr/lib* ]] && [[ "$p" != /lib* ]]; then
            echo ">>> Dropping '$p' from LD_LIBRARY_PATH (foreign libstdc++)"
        else
            CLEAN_LLP="${CLEAN_LLP:+$CLEAN_LLP:}$p"
        fi
    done
    export LD_LIBRARY_PATH="$CLEAN_LLP"
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

LIBADLMIDI_DIR="$SCRIPT_DIR/libADLMIDI"
BUILD_DIR="$SCRIPT_DIR/build/libadlmidi"
INSTALL_DIR="$SCRIPT_DIR/build/install"
OBJ_DIR="$SCRIPT_DIR/build/obj"
BIN="$SCRIPT_DIR/opl3-rw-midi-player"

mkdir -p "$OBJ_DIR"

# ── SDL2 (required) ───────────────────────────────────────────────────────────
SDL_CFLAGS="$(sdl2-config --cflags 2>/dev/null || pkg-config --cflags sdl2 2>/dev/null || true)"
SDL_LIBS="$(sdl2-config --libs 2>/dev/null || pkg-config --libs sdl2 2>/dev/null || true)"
if [ -z "$SDL_LIBS" ]; then
    echo "ERROR: SDL2 not found. Install it, e.g.: sudo apt install libsdl2-dev" >&2
    exit 1
fi

# ── 1. Fetch libADLMIDI ───────────────────────────────────────────────────────
if [ ! -f "$LIBADLMIDI_DIR/CMakeLists.txt" ]; then
    if [ -f "$SCRIPT_DIR/.gitmodules" ] && git -C "$SCRIPT_DIR" rev-parse --git-dir >/dev/null 2>&1; then
        echo ">>> Initialising libADLMIDI submodule..."
        git -C "$SCRIPT_DIR" submodule update --init --recursive libADLMIDI
    else
        echo ">>> Cloning libADLMIDI..."
        git clone --depth 1 https://github.com/Wohlstand/libADLMIDI.git "$LIBADLMIDI_DIR"
    fi
else
    echo ">>> libADLMIDI present"
fi

# ── 2. Build libADLMIDI (static) ──────────────────────────────────────────────
LIBFILE="$INSTALL_DIR/lib/libADLMIDI.a"
if [ "${1:-}" = "fast" ] && [ -f "$LIBFILE" ]; then
    echo ">>> Skipping libADLMIDI build (fast mode)"
else
    echo ">>> Building libADLMIDI..."
    cmake -B "$BUILD_DIR" -S "$LIBADLMIDI_DIR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
        -DlibADLMIDI_STATIC=ON \
        -DlibADLMIDI_SHARED=OFF \
        -DUSE_HW_SERIAL=OFF \
        -DWITH_MIDI_SEQUENCER=ON \
        -DWITH_EMBEDDED_BANKS=ON \
        -DUSE_DOSBOX_EMULATOR=ON \
        -DUSE_NUKED_EMULATOR=OFF \
        -DUSE_OPAL_EMULATOR=OFF \
        -DUSE_JAVA_EMULATOR=OFF \
        -DWITH_MIDIPLAY=OFF \
        -DWITH_GENADLDATA=OFF \
        -DWITH_OLD_UTILS=OFF \
        2>&1 | grep -v "^--" || true
    cmake --build "$BUILD_DIR" --parallel "$(nproc)"
    cmake --install "$BUILD_DIR"
    echo ">>> libADLMIDI installed to $INSTALL_DIR"
fi

# ── 3. Compile the player ─────────────────────────────────────────────────────
echo ">>> Compiling player..."
CXX="${CXX:-g++}"
CC="${CC:-gcc}"
CFLAGS="-O2 -Wall"
CXXFLAGS="-std=c++17 -O2 -Wall -Wextra"
# libADLMIDI/src is on the path because fanout_chip.* uses libADLMIDI internals.
INC="-I$SCRIPT_DIR/src -I$INSTALL_DIR/include -I$LIBADLMIDI_DIR/src $SDL_CFLAGS"

# C sources (DOSBox emulator in OPL3 mode; RetroWave serial framing).
$CC  $CFLAGS -DOPLTYPE_IS_OPL3 -c "$SCRIPT_DIR/src/emu/opl.c"        -o "$OBJ_DIR/opl.o"
$CC  $CFLAGS                   -c "$SCRIPT_DIR/src/retrowave_serial.c" -o "$OBJ_DIR/retrowave_serial.o"

# C++ sources.
for u in playlist alsamidi config board_writer engine fanout_chip gui main; do
    $CXX $CXXFLAGS $INC -c "$SCRIPT_DIR/src/$u.cpp" -o "$OBJ_DIR/$u.o"
done

# Link (-lasound for the external ALSA MIDI output).
$CXX $CXXFLAGS -o "$BIN" \
    "$OBJ_DIR/opl.o" "$OBJ_DIR/retrowave_serial.o" \
    "$OBJ_DIR/playlist.o" "$OBJ_DIR/alsamidi.o" "$OBJ_DIR/config.o" "$OBJ_DIR/board_writer.o" \
    "$OBJ_DIR/engine.o" "$OBJ_DIR/fanout_chip.o" "$OBJ_DIR/gui.o" "$OBJ_DIR/main.o" \
    -L"$INSTALL_DIR/lib" -lADLMIDI $SDL_LIBS -lasound -lm -lpthread

echo ""
echo ">>> Done: $BIN"
echo ""
echo "Try:"
echo "  ./opl3-rw-midi-player -d - /path/to/midis/     # GUI, dry-run board (PC audio)"
echo "  ./opl3-rw-midi-player /path/to/midis/          # GUI, board on /dev/ttyACM0 + PC"
echo "  ./opl3-rw-midi-player --list-banks             # list instrument banks"
