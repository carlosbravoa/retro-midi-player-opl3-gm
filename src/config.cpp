/*
 * config.cpp — see config.hpp.
 */
#include "config.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
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
}
