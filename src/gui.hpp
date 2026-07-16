/*
 * gui.hpp — SDL2 front-end: OPL3 channel visualizer + Winamp-style playlist.
 *
 * gui_run() owns the window, the event loop, and the render loop. It reads
 * playback state from the Engine (channel levels come from the same emulator
 * the audio callback drives) and drives the Playlist for navigation/shuffle.
 * Returns after the user quits, or false immediately if SDL could not start
 * (the caller then falls back to the headless player).
 */
#ifndef GUI_HPP
#define GUI_HPP

class Engine;
class Playlist;
struct Config;

// Runs the GUI. Reads initial style/output/recurse from `cfg`, and writes the
// user's final settings back into `cfg` on exit (the caller persists them).
bool gui_run(Engine &engine, Playlist &playlist, Config &cfg);

#endif /* GUI_HPP */
