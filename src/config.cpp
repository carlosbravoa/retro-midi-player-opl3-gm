/*
 * config.cpp — see config.hpp.
 */
#include "config.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

namespace fs = std::filesystem;

std::string config_path()
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    std::string base;
    if (xdg && *xdg)
        base = xdg;
    else {
        const char *home = getenv("HOME");
        base = std::string(home && *home ? home : ".") + "/.config";
    }
    return base + "/opl3-rw-midi-player/config";
}

static std::string trim(const std::string &s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
}

bool config_load(Config &c)
{
    std::ifstream f(config_path());
    if (!f) return false;

    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq));
        std::string v = trim(line.substr(eq + 1));
        if (v.empty()) continue;
        int n = atoi(v.c_str());
        if      (k == "bank")    c.bank    = n;
        else if (k == "recurse") c.recurse = n != 0;
        else if (k == "loop")    c.loop    = (n >= 0 && n <= 2) ? n : 0;
        else if (k == "shuffle") c.shuffle = n != 0;
        else if (k == "style")   c.style   = n;
        else if (k == "outmode") c.outmode = n;
        else if (k == "win_w")   c.win_w   = n;
        else if (k == "win_h")   c.win_h   = n;
        else if (k == "serial_dev") c.serial_dev = v;
        else if (k == "volume")  c.volume  = (n >= 0 && n <= 200) ? n : 100;
        else if (k == "midi_dev")    c.midi_dev    = v;
        else if (k == "last_folder") c.last_folder = v;
    }
    return true;
}

void config_save(const Config &c)
{
    std::string p = config_path();
    std::error_code ec;
    fs::create_directories(fs::path(p).parent_path(), ec);

    std::ofstream f(p, std::ios::trunc);
    if (!f) return;
    f << "# OPL3 RetroWave MIDI Player — saved settings\n";
    f << "bank="    << c.bank        << "\n";
    f << "recurse=" << (c.recurse ? 1 : 0) << "\n";
    f << "loop="    << c.loop        << "\n";
    f << "shuffle=" << (c.shuffle ? 1 : 0) << "\n";
    f << "style="   << c.style       << "\n";
    f << "outmode=" << c.outmode     << "\n";
    f << "win_w="   << c.win_w       << "\n";
    f << "win_h="   << c.win_h       << "\n";
    f << "serial_dev=" << c.serial_dev << "\n";
    f << "volume="  << c.volume      << "\n";
    f << "midi_dev="    << c.midi_dev    << "\n";
    f << "last_folder=" << c.last_folder << "\n";
}

// ── Per-song banks ────────────────────────────────────────────────────────────
// One "bank name<TAB>file path" per line.
static std::map<std::string, std::string> g_song_banks;
static bool g_song_banks_loaded = false;

static std::string song_banks_path()
{
    return fs::path(config_path()).parent_path().string() + "/song_banks";
}

static void song_banks_load()
{
    if (g_song_banks_loaded) return;
    g_song_banks_loaded = true;
    std::ifstream f(song_banks_path());
    std::string line;
    while (std::getline(f, line)) {
        size_t tab = line.find('\t');
        if (tab == std::string::npos || tab == 0 || tab + 1 >= line.size()) continue;
        g_song_banks[line.substr(tab + 1)] = line.substr(0, tab);
    }
}

std::string song_bank_get(const std::string &path)
{
    song_banks_load();
    auto it = g_song_banks.find(path);
    return it == g_song_banks.end() ? std::string() : it->second;
}

void song_bank_set(const std::string &path, const std::string &bank_name)
{
    song_banks_load();
    if (path.empty() || path.find('\n') != std::string::npos) return;
    auto it = g_song_banks.find(path);
    if (it != g_song_banks.end() && it->second == bank_name) return;
    g_song_banks[path] = bank_name;

    std::string p = song_banks_path();
    std::error_code ec;
    fs::create_directories(fs::path(p).parent_path(), ec);
    std::ofstream f(p, std::ios::trunc);
    for (const auto &kv : g_song_banks) f << kv.second << '\t' << kv.first << '\n';
}
