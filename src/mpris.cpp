/*
 * mpris.cpp — see mpris.hpp.
 */
#include "mpris.hpp"

#ifndef HAVE_MPRIS

bool mpris_start() { return false; }
void mpris_stop() { }
void mpris_update(const MprisState &, std::vector<MprisCommand> &) { }

#else

#include <systemd/sd-bus.h>

#include <cstdio>
#include <cstring>
#include <unistd.h>

namespace {

const char *OBJ    = "/org/mpris/MediaPlayer2";
const char *ROOT   = "org.mpris.MediaPlayer2";
const char *PLAYER = "org.mpris.MediaPlayer2.Player";

sd_bus      *g_bus = nullptr;
sd_bus_slot *g_slot_root = nullptr, *g_slot_player = nullptr;
MprisState   g_st;
int          g_track_no = 0;                 // bumped per title, for a unique track id
std::vector<MprisCommand> g_cmds;

const char *status_name(MprisState::Status s)
{
    return s == MprisState::PLAYING ? "Playing" : s == MprisState::PAUSED ? "Paused" : "Stopped";
}

// ── org.mpris.MediaPlayer2 ────────────────────────────────────────────────────
int root_method(sd_bus_message *m, void *, sd_bus_error *)
{
    g_cmds.push_back({ strcmp(sd_bus_message_get_member(m), "Quit") == 0
                       ? MprisCommand::QUIT : MprisCommand::RAISE, 0.0 });
    return sd_bus_reply_method_return(m, "");
}

int root_prop(sd_bus *, const char *, const char *, const char *prop,
              sd_bus_message *reply, void *, sd_bus_error *)
{
    if (!strcmp(prop, "Identity"))     return sd_bus_message_append(reply, "s", "OPL3 RetroWave MIDI Player");
    if (!strcmp(prop, "DesktopEntry")) return sd_bus_message_append(reply, "s", "retro-midi-player-opl3-gm");
    if (!strcmp(prop, "HasTrackList")) return sd_bus_message_append(reply, "b", 0);
    if (!strcmp(prop, "SupportedUriSchemes")) return sd_bus_message_append(reply, "as", 1, "file");
    if (!strcmp(prop, "SupportedMimeTypes"))  return sd_bus_message_append(reply, "as", 2, "audio/midi", "audio/x-midi");
    return sd_bus_message_append(reply, "b", 1);   // CanQuit, CanRaise
}

const sd_bus_vtable root_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Raise", "", "", root_method, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Quit",  "", "", root_method, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_PROPERTY("CanQuit",      "b", root_prop, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("CanRaise",     "b", root_prop, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("HasTrackList", "b", root_prop, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Identity",     "s", root_prop, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("DesktopEntry", "s", root_prop, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("SupportedUriSchemes", "as", root_prop, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("SupportedMimeTypes",  "as", root_prop, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_VTABLE_END
};

// ── org.mpris.MediaPlayer2.Player ─────────────────────────────────────────────
int player_method(sd_bus_message *m, void *, sd_bus_error *)
{
    const char *name = sd_bus_message_get_member(m);
    if (!strcmp(name, "Seek")) {                       // relative, microseconds
        int64_t us = 0;
        if (sd_bus_message_read(m, "x", &us) >= 0)
            g_cmds.push_back({ MprisCommand::SEEK_BY, us / 1e6 });
    } else if (!strcmp(name, "SetPosition")) {         // (track id, absolute microseconds)
        const char *track = nullptr; int64_t us = 0;
        if (sd_bus_message_read(m, "ox", &track, &us) >= 0)
            g_cmds.push_back({ MprisCommand::SEEK_TO, us / 1e6 });
    } else {
        MprisCommand::Type t =
            !strcmp(name, "Next")      ? MprisCommand::NEXT :
            !strcmp(name, "Previous")  ? MprisCommand::PREVIOUS :
            !strcmp(name, "Pause")     ? MprisCommand::PAUSE :
            !strcmp(name, "PlayPause") ? MprisCommand::PLAY_PAUSE :
            !strcmp(name, "Stop")      ? MprisCommand::STOP : MprisCommand::PLAY;
        g_cmds.push_back({ t, 0.0 });
    }
    return sd_bus_reply_method_return(m, "");
}

int append_metadata(sd_bus_message *reply)
{
    char track[64];
    if (g_st.title.empty()) snprintf(track, sizeof track, "/org/mpris/MediaPlayer2/TrackList/NoTrack");
    else                    snprintf(track, sizeof track, "/org/mpris/MediaPlayer2/track/%d", g_track_no);
    int r = sd_bus_message_open_container(reply, 'a', "{sv}");
    if (r >= 0) r = sd_bus_message_append(reply, "{sv}", "mpris:trackid", "o", track);
    if (r >= 0 && !g_st.title.empty()) {
        r = sd_bus_message_append(reply, "{sv}", "mpris:length", "x", (int64_t)(g_st.length * 1e6));
        if (r >= 0) r = sd_bus_message_append(reply, "{sv}", "xesam:title", "s", g_st.title.c_str());
    }
    if (r >= 0) r = sd_bus_message_close_container(reply);
    return r;
}

int player_prop(sd_bus *, const char *, const char *, const char *prop,
                sd_bus_message *reply, void *, sd_bus_error *)
{
    if (!strcmp(prop, "PlaybackStatus")) return sd_bus_message_append(reply, "s", status_name(g_st.status));
    if (!strcmp(prop, "Metadata"))       return append_metadata(reply);
    if (!strcmp(prop, "Position"))       return sd_bus_message_append(reply, "x", (int64_t)(g_st.position * 1e6));
    if (!strcmp(prop, "Volume"))         return sd_bus_message_append(reply, "d", g_st.volume);
    if (!strcmp(prop, "Rate") || !strcmp(prop, "MinimumRate") || !strcmp(prop, "MaximumRate"))
        return sd_bus_message_append(reply, "d", 1.0);
    if (!strcmp(prop, "CanGoNext") || !strcmp(prop, "CanGoPrevious") ||
        !strcmp(prop, "CanPlay")   || !strcmp(prop, "CanPause"))
        return sd_bus_message_append(reply, "b", g_st.can_go ? 1 : 0);
    if (!strcmp(prop, "CanSeek"))        return sd_bus_message_append(reply, "b", g_st.title.empty() ? 0 : 1);
    return sd_bus_message_append(reply, "b", 1);   // CanControl
}

int player_set_volume(sd_bus *, const char *, const char *, const char *,
                      sd_bus_message *value, void *, sd_bus_error *)
{
    double v = 1.0;
    int r = sd_bus_message_read(value, "d", &v);
    if (r >= 0) g_cmds.push_back({ MprisCommand::VOLUME, v });
    return r;
}

const sd_bus_vtable player_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Next",      "", "", player_method, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Previous",  "", "", player_method, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Pause",     "", "", player_method, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("PlayPause", "", "", player_method, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Stop",      "", "", player_method, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Play",      "", "", player_method, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Seek",        "x",  "", player_method, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("SetPosition", "ox", "", player_method, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_SIGNAL("Seeked", "x", 0),
    SD_BUS_PROPERTY("PlaybackStatus", "s",     player_prop, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("Metadata",       "a{sv}", player_prop, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("Position",       "x",     player_prop, 0, 0),
    SD_BUS_PROPERTY("Rate",           "d",     player_prop, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("MinimumRate",    "d",     player_prop, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("MaximumRate",    "d",     player_prop, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_WRITABLE_PROPERTY("Volume", "d", player_prop, player_set_volume, 0,
                             SD_BUS_VTABLE_UNPRIVILEGED | SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("CanGoNext",     "b", player_prop, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("CanGoPrevious", "b", player_prop, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("CanPlay",       "b", player_prop, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("CanPause",      "b", player_prop, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("CanSeek",       "b", player_prop, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("CanControl",    "b", player_prop, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_VTABLE_END
};

} // namespace

bool mpris_start()
{
    if (g_bus) return true;
    if (sd_bus_open_user(&g_bus) < 0) { g_bus = nullptr; return false; }

    int r = sd_bus_add_object_vtable(g_bus, &g_slot_root, OBJ, ROOT, root_vtable, nullptr);
    if (r >= 0) r = sd_bus_add_object_vtable(g_bus, &g_slot_player, OBJ, PLAYER, player_vtable, nullptr);
    // One well-known name per player; a second copy of us adds its pid, as the
    // MPRIS spec asks.
    if (r >= 0) {
        r = sd_bus_request_name(g_bus, "org.mpris.MediaPlayer2.opl3-rw-midi-player", 0);
        if (r < 0) {
            char name[96];
            snprintf(name, sizeof name, "org.mpris.MediaPlayer2.opl3-rw-midi-player.instance%d", (int)getpid());
            r = sd_bus_request_name(g_bus, name, 0);
        }
    }
    if (r < 0) { mpris_stop(); return false; }
    return true;
}

void mpris_stop()
{
    if (!g_bus) return;
    sd_bus_slot_unref(g_slot_root);   g_slot_root = nullptr;
    sd_bus_slot_unref(g_slot_player); g_slot_player = nullptr;
    sd_bus_flush_close_unref(g_bus);
    g_bus = nullptr;
}

void mpris_update(const MprisState &now, std::vector<MprisCommand> &commands)
{
    if (!g_bus) return;

    // Announce what changed (the desktop caches these; Position is polled).
    bool status   = now.status != g_st.status;
    bool metadata = now.title != g_st.title || now.length != g_st.length;
    bool volume   = now.volume != g_st.volume;
    bool can      = now.can_go != g_st.can_go || now.title.empty() != g_st.title.empty();
    bool jumped   = !metadata && (now.position < g_st.position - 0.5 || now.position > g_st.position + 2.0);
    if (now.title != g_st.title) ++g_track_no;
    g_st = now;
    if (status)   sd_bus_emit_properties_changed(g_bus, OBJ, PLAYER, "PlaybackStatus", nullptr);
    if (metadata) sd_bus_emit_properties_changed(g_bus, OBJ, PLAYER, "Metadata", nullptr);
    if (volume)   sd_bus_emit_properties_changed(g_bus, OBJ, PLAYER, "Volume", nullptr);
    if (can)      sd_bus_emit_properties_changed(g_bus, OBJ, PLAYER, "CanGoNext", "CanGoPrevious",
                                                 "CanPlay", "CanPause", "CanSeek", nullptr);
    if (jumped)   sd_bus_emit_signal(g_bus, OBJ, PLAYER, "Seeked", "x", (int64_t)(now.position * 1e6));

    while (sd_bus_process(g_bus, nullptr) > 0) { }
    commands.insert(commands.end(), g_cmds.begin(), g_cmds.end());
    g_cmds.clear();
}

#endif /* HAVE_MPRIS */
