/*
 * engine.hpp — playback engine: libADLMIDI + SDL audio + the fan-out chip.
 *
 * Owns the ADL_MIDIPlayer, the RetroWave board connection, and the SDL audio
 * device. The SDL audio callback is the single clock: each block it calls
 * adl_play(), which advances the MIDI sequencer, fires OPL register writes
 * (fanned out to the board and the embedded emulator by FanoutOPL3), and hands
 * back the emulator's samples for the speakers.
 *
 * Threading: audio runs on SDL's callback thread. Anything that reconfigures
 * libADLMIDI (loading a song, changing bank) locks the audio device first so it
 * never races the callback. The GUI/control thread polls consume_song_ended()
 * to advance the playlist.
 */
#ifndef ENGINE_HPP
#define ENGINE_HPP

#include <atomic>
#include <string>
#include <cstdint>

#include <adlmidi.h>
#include <SDL.h>

#include "alsamidi.hpp"

struct Track;

class Engine {
public:
    // sample_rate: SDL + libADLMIDI output rate (e.g. 44100).
    // serial_dev : RetroWave device ("ttyACM0", "/dev/ttyACM0", "-" dry-run,
    //              "" no board). midi_port: ALSA destination for external MIDI
    //              ("20:0", a client name, "" = none). bank_no: built-in bank.
    bool init(unsigned sample_rate, const std::string &serial_dev,
              const std::string &midi_port, int bank_no);
    void shutdown();

    // Load and begin a song from the start. Locks audio while reconfiguring.
    bool load(const std::string &path);
    void stop();                 // silence + forget current song

    void set_paused(bool p) { m_paused.store(p); }
    bool paused() const     { return m_paused.load(); }

    void set_loop(bool l);       // repeat the current track
    bool loop() const { return m_loop; }

    void set_tempo(double scale);
    double tempo() const { return m_tempo; }

    // Output routing (see fanout_chip.hpp globals). The three sinks are
    // independent flags; the GUI drives them mutually-exclusively. Turning a
    // sink off silences it (board reset / MIDI all-notes-off) to avoid hangs.
    void set_board_out(bool on);
    void set_pc_out(bool on);
    void set_gm_out(bool on);
    bool board_out() const;
    bool pc_out() const;
    bool gm_out() const;
    bool board_available() const;   // a board (or dry-run) is actually open
    bool gm_available() const;      // an external MIDI port is connected
    bool gm_connectable() const;    // connected, or one could be connected now
    bool ensure_midi();             // connect the external device if not already
    bool connect_midi(const std::string &addr);   // (re)connect to a specific port
    const std::string &gm_name() const;   // destination description for the UI
    std::string gm_addr() const;          // "client:port" of the connection

    // Bank selection from libADLMIDI's built-in banks; reloads current song.
    bool set_bank(int bank_no);
    int  bank() const { return m_bank_no; }

    // True exactly once after the current song reaches its end (poll it).
    bool consume_song_ended() { return m_song_ended.exchange(false); }

    // Approximate playback position / length in seconds (for the GUI).
    double position() const;
    double length() const;

    unsigned rate() const { return m_rate; }

private:
    static void sdl_audio_cb(void *ud, Uint8 *stream, int len);
    void render(int16_t *out, int frames);
    bool load_locked(const std::string &path);   // caller holds audio lock

    // libADLMIDI raw-MIDI-event hook -> external MIDI device (audio thread).
    static void raw_event_cb(void *ud, uint8_t type, uint8_t subtype,
                             uint8_t channel, const uint8_t *data, size_t len);
    void on_raw_event(uint8_t type, uint8_t channel, const uint8_t *data, size_t len);

    ADL_MIDIPlayer   *m_adl = nullptr;
    SDL_AudioDeviceID m_dev = 0;
    unsigned          m_rate = 44100;
    int               m_bank_no = 0;
    double            m_tempo = 1.0;
    bool              m_loop = false;
    bool              m_have_board = false;
    bool              m_have_midi = false;
    bool              m_have_song = false;
    std::string       m_cur_path;
    std::string       m_midi_port;   // configured -m target ("" = auto-pick)
    AlsaMidi          m_midi;

    std::atomic<bool> m_paused{false};
    std::atomic<bool> m_song_ended{false};

    float             m_gain = 2.0f;        // emulator output runs quiet
};

#endif /* ENGINE_HPP */
