/*
 * mpris.hpp — desktop media controls (MPRIS over D-Bus).
 *
 * Publishing the standard org.mpris.MediaPlayer2 interface is what makes the
 * keyboard's media keys, the desktop's media widget and tools like playerctl
 * work while the window is not focused. Everything runs on the GUI thread:
 * mpris_update() is called once per frame, hands back whatever the desktop
 * asked for, and announces state changes.
 *
 * Built only when libsystemd (sd-bus) is available; otherwise these are no-ops.
 */
#ifndef MPRIS_HPP
#define MPRIS_HPP

#include <string>
#include <vector>

struct MprisState {
    enum Status { STOPPED, PLAYING, PAUSED };
    Status      status = STOPPED;
    std::string title;            // "" = no track
    double      length = 0.0;     // seconds
    double      position = 0.0;   // seconds
    double      volume = 1.0;     // 0..1 (and beyond)
    bool        can_go = false;   // next/previous make sense (playlist not empty)
};

struct MprisCommand {
    enum Type { PLAY, PAUSE, PLAY_PAUSE, STOP, NEXT, PREVIOUS, RAISE, QUIT,
                SEEK_BY,      // value: seconds, relative
                SEEK_TO,      // value: seconds, absolute
                VOLUME };     // value: 0..1
    Type   type;
    double value;
};

bool mpris_start();   // false if there is no session bus (or no sd-bus support)
void mpris_stop();
void mpris_update(const MprisState &now, std::vector<MprisCommand> &commands);

#endif /* MPRIS_HPP */
