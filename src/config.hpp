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
    int  loop    = 0;       // repeat: 0 off, 1 current track, 2 whole playlist
    bool shuffle = false;   // shuffled playback order
    int  style   = 0;       // visualizer: 0 LED, 1 NEON, 2 SPEC
    int  outmode = -1;      // output: -1 auto, 0 PC, 1 BOARD, 2 EXT-MIDI
    int  win_w   = 980;     // GUI window size
    int  win_h   = 600;
    // Board serial device: "ttyACM0", "/dev/ttyUSB1", ... or "none" (PC only).
    std::string serial_dev = "ttyACM0";
    int  volume  = 100;     // PC output volume, percent (0..200)
    std::string midi_dev;       // last external MIDI device ("Client name:port")
    std::string last_folder;    // folder reopened when started with no files
};

std::string config_path();            // full path to the config file
bool config_load(Config &c);          // fills c from disk; false if no file
void config_save(const Config &c);    // writes c (creating the directory)

#endif /* CONFIG_HPP */
