/*
 * main.cpp — OPL3 RetroWave MIDI Player.
 *
 * A generic Standard MIDI File player: libADLMIDI turns each .mid into an OPL3
 * register stream that a custom chip (FanoutOPL3) sends to a RetroWave OPL3
 * Express board and/or a software emulator for PC audio. GUI-first (SDL2), with
 * folder loading, shuffle, and bank selection; a headless mode is available for
 * boards-only playback with no display.
 */
#include "engine.hpp"
#include "playlist.hpp"
#include "gui.hpp"
#include "alsamidi.hpp"
#include "config.hpp"

#include <adlmidi.h>
#include <SDL.h>

#include <csignal>
#include <filesystem>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static volatile sig_atomic_t g_int = 0;
static void on_sigint(int) { g_int = 1; }

static void usage(const char *prog)
{
    printf(
"OPL3 RetroWave MIDI Player — play Standard MIDI files on a RetroWave OPL3\n"
"Express (and/or your PC's speakers) via libADLMIDI FM synthesis.\n\n"
"Usage: %s [options] [folder | file.mid ...]\n\n"
"Options:\n"
"  -d DEV        RetroWave serial device (default: ttyACM0; '-' = dry-run\n"
"                framing with no hardware; 'none' = PC audio only)\n"
"  -m PORT       External MIDI device (ALSA), e.g. '20:0' or a client name, or\n"
"                'auto' for the first one found. Enables the External output.\n"
"  -r            Recurse into subfolders when scanning folders\n"
"  -b N          Start on libADLMIDI built-in bank #N (default: a General MIDI\n"
"                bank if present)\n"
"  -s            Start with shuffle enabled\n"
"  -l            Loop the current track (in the GUI, LOOP also has ALL)\n"
"  --no-gui      Headless: stream the playlist with no window (Ctrl-C to quit)\n"
"  --list-banks  Print libADLMIDI's built-in banks and exit\n"
"  --list-midi   Print available external MIDI ports and exit\n"
"  -h, --help    This help\n\n"
"In the GUI: click a track to play it, click the progress bar to seek, use OUT:\n"
"to switch Board / PC / External MIDI, BANK < > (or [ ]) to change FM\n"
"instruments (Board/PC only), SHUF/LOOP toggles, and OPEN FOLDER (needs zenity\n"
"or kdialog) to add more music. Click a visualizer bar to mute that OPL3\n"
"channel, right-click to solo it, 'u' to unmute all.\n", prog);
}

// A reasonable default bank for arbitrary MIDIs: the first whose name mentions
// "General MIDI"; otherwise bank 0.
static int default_bank()
{
    int n = adl_getBanksCount();
    const char *const *names = adl_getBankNames();
    for (int i = 0; i < n; ++i)
        if (names[i] && strstr(names[i], "General MIDI"))
            return i;
    return 0;
}

static void list_banks()
{
    int n = adl_getBanksCount();
    const char *const *names = adl_getBankNames();
    printf("libADLMIDI built-in banks (%d):\n", n);
    for (int i = 0; i < n; ++i)
        printf("  %3d  %s\n", i, names[i] ? names[i] : "");
}

int main(int argc, char **argv)
{
    // Saved preferences are the defaults; command-line flags below override.
    Config cfg;
    config_load(cfg);

    std::string serial_dev = "ttyACM0";
    std::string midi_port;
    bool recurse = cfg.recurse, shuffle = cfg.shuffle, gui = true;
    int  loop = cfg.loop;   // 0 off, 1 track, 2 playlist
    int  bank = cfg.bank;
    std::vector<std::string> inputs;

    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if      (!strcmp(a, "-d") && i + 1 < argc) serial_dev = argv[++i];
        else if (!strcmp(a, "-m") && i + 1 < argc) midi_port = argv[++i];
        else if (!strcmp(a, "-r")) recurse = true;
        else if (!strcmp(a, "-s")) shuffle = true;
        else if (!strcmp(a, "-l")) loop = 1;
        else if (!strcmp(a, "-b") && i + 1 < argc) bank = atoi(argv[++i]);
        else if (!strcmp(a, "--no-gui")) gui = false;
        else if (!strcmp(a, "--list-banks")) { list_banks(); return 0; }
        else if (!strcmp(a, "--list-midi")) { AlsaMidi::list_ports(); return 0; }
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(argv[0]); return 0; }
        else if (a[0] == '-' && a[1] && strcmp(a, "-")) { fprintf(stderr, "Unknown option: %s\n", a); usage(argv[0]); return 1; }
        else inputs.push_back(a);
    }

    // -1 (unset), a typo'd -b, or a stale config entry from a libADLMIDI with
    // more banks would index past the bank table: fall back to the default.
    if (bank < 0 || bank >= adl_getBanksCount()) bank = default_bank();
    if ((serial_dev == "none") || serial_dev == "off") serial_dev.clear();

    // ── Build the playlist ────────────────────────────────────────────────────
    Playlist playlist;
    for (const auto &in : inputs) {
        // A directory is scanned; anything else is added as a file.
        std::error_code ec;
        if (std::filesystem::is_directory(in, ec)) playlist.add_folder(in, recurse);
        else                                      playlist.add_file(in);
    }
    playlist.set_shuffle(shuffle);

    if (playlist.empty() && !gui) {
        fprintf(stderr, "No MIDI files. Pass a folder or .mid file (or use the GUI to open one).\n");
        usage(argv[0]);
        return 1;
    }

    // ── SDL audio (needed by the engine in both GUI and headless modes) ───────
    if (SDL_Init(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL audio init failed: %s\n", SDL_GetError());
        return 1;
    }

    Engine engine;
    if (!engine.init(44100, serial_dev, midi_port, bank)) {
        fprintf(stderr, "Engine init failed.\n");
        SDL_Quit();
        return 1;
    }
    engine.set_loop(loop == 1);
    // Headless has no output selector, so pick one: an explicitly-requested
    // external MIDI device wins, else the board if present, else PC.
    if (!gui) {
        engine.set_pc_out(false); engine.set_board_out(false); engine.set_gm_out(false);
        if (engine.gm_available())         engine.set_gm_out(true);
        else if (engine.board_available()) engine.set_board_out(true);
        else                               engine.set_pc_out(true);
    }

    printf("Bank #%d — %s\n", bank, adl_getBankNames()[bank]);
    printf("Outputs available: PC%s%s | %d track(s)\n",
           engine.board_available() ? ", Board(OPL3)" : "",
           engine.gm_available() ? (std::string(", MIDI[") + engine.gm_name() + "]").c_str() : "",
           (int)playlist.size());

    // Fold the resolved command-line values into the config the GUI reads
    // (and, on exit, writes back for persistence).
    cfg.bank = bank; cfg.recurse = recurse; cfg.loop = loop; cfg.shuffle = shuffle;

    if (gui) {
        if (gui_run(engine, playlist, cfg)) {
            config_save(cfg);   // persist the user's final settings
        } else {
            fprintf(stderr, "GUI unavailable — falling back to headless.\n");
            gui = false;
        }
    }

    if (!gui) {
        // ── Headless: stream the playlist to the board / PC until it ends ─────
        signal(SIGINT, on_sigint);
        signal(SIGTERM, on_sigint);
        bool wrap = (loop == 2);
        bool playing = play_from_playlist(engine, playlist, wrap, true);
        while (!g_int && playing) {
            if (engine.consume_song_ended())
                playing = playlist.next(wrap) && play_from_playlist(engine, playlist, wrap, true);
            if (engine.board_out() && !engine.board_available()) {
                fprintf(stderr, "Board lost — continuing on PC audio.\n");
                engine.set_board_out(false);
                engine.set_pc_out(true);
            }
            SDL_Delay(100);
        }
        printf("\n");
    }

    engine.shutdown();
    SDL_Quit();
    return 0;
}
