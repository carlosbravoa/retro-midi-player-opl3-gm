/*
 * config.hpp — persisted user preferences.
 *
 * A tiny key=value file under $XDG_CONFIG_HOME (or ~/.config) remembers the
 * settings the user changes in the GUI, so they survive across runs. Loaded at
 * startup as defaults (command-line flags still override), and written back
 * when the GUI exits.
 *
 * Fields use -1 / defaults to mean "not set — decide at runtime".
 */
#ifndef CONFIG_HPP
#define CONFIG_HPP

#include <string>

struct Config {
    int  bank    = -1;      // libADLMIDI bank ordinal; -1 => pick a default
    bool recurse = false;   // scan subfolders
    bool loop    = false;   // repeat current track
    bool shuffle = false;   // shuffled playback order
    int  style   = 0;       // visualizer: 0 LED, 1 NEON, 2 SPEC
    int  outmode = -1;      // output: -1 auto, 0 PC, 1 BOARD, 2 EXT-MIDI
};

std::string config_path();            // full path to the config file
bool config_load(Config &c);          // fills c from disk; false if no file
void config_save(const Config &c);    // writes c (creating the directory)

#endif /* CONFIG_HPP */
