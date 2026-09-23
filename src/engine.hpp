/*
 * engine.hpp — playback engine: libADLMIDI + SDL audio + the fan-out chip.
 *
 * Owns the ADL_MIDIPlayer, the RetroWave board connection, and the SDL audio
 * device. The SDL audio callback is the single clock: each block it calls
 * adl_play(), which advances the MIDI sequencer, fires OPL register writes
 * (fanned out to the board and the embedded emulator by FanoutOPL3), and hands
 * back the emulator's samples for the speakers. Board writes are staged, not
 * sent: the callback renders in short slices and hands each slice's bytes to a
 * BoardWriter thread, which delivers them to the serial port evenly paced.
 *
 * Threading: audio runs on SDL's callback thread. Anything that reconfigures
 * libADLMIDI (loading a song, changing bank, seeking) locks the audio device
 * first so it never races the callback. The GUI/control thread polls
 * consume_song_ended() to advance the playlist.
 */
#ifndef ENGINE_HPP
#define ENGINE_HPP

#include <atomic>
#include <chrono>
#include <string>
#include <cstdint>

#include <adlmidi.h>
#include <SDL.h>

#include "alsamidi.hpp"
#include "board_writer.hpp"

class Playlist;

class Engine {
public:
    // sample_rate: SDL + libADLMIDI output rate (e.g. 44100).
    // serial_dev : RetroWave device ("ttyACM0", "/dev/ttyACM0", "-" dry-run,
    //              "" no board). midi_port: ALSA destination for external MIDI
    //              ("20:0", a client name, "" = none). bank_no: built-in bank.
    bool init(unsigned sample_rate, const std::string &serial_dev,
              const std::string &midi_port, int bank_no);
    void shutdown();

    // Load and begin a song from the start (unpaused). Locks audio while
    // reconfiguring. False if the file can't be loaded.
    bool load(const std::string &path);
    void stop();                 // silence + forget current song

    // Pausing silences held notes on every output (board, PC, external) and
    // resuming re-keys them.
    void set_paused(bool p);
    bool paused() const     { return m_paused.load(); }
    bool has_song() const   { return m_have_song.load(); }   // false after stop()

    void set_loop(bool l);       // repeat the current track
    bool loop() const { return m_loop; }

    void set_tempo(double scale);
    double tempo() const { return m_tempo; }

    // Jump to `seconds` into the current song (no-op when stopped).
    void seek(double seconds);

    // Output routing (see fanout_chip.hpp globals). The three sinks are
    // independent flags; the GUI drives them mutually-exclusively. Turning a
    // sink off silences it (board reset / MIDI all-notes-off) to avoid hangs.
    void set_board_out(bool on);
    void set_pc_out(bool on);
    void set_gm_out(bool on);
    bool board_out() const;
    bool pc_out() const;
    bool gm_out() const;
    bool board_available() const;   // a board (or dry-run) is open and alive
    bool gm_available() const;      // an external MIDI port is connected
    bool gm_connectable() const;    // connected, or one could be connected now
    bool ensure_midi();             // connect the external device if not already
    bool connect_midi(const std::string &addr);   // (re)connect to a specific port
    const std::string &gm_name() const;   // destination description for the UI
    std::string gm_addr() const;          // "client:port" of the connection

    // Per-OPL3-channel mute (0..17), applied to board + PC alike. Persists
    // across track changes; not saved to the config.
    void set_channel_muted(int ch, bool muted);
    bool channel_muted(int ch) const;
    void set_mute_mask(uint32_t mask);    // bit N = channel N
    uint32_t mute_mask() const;

    // Bank selection from libADLMIDI's built-in banks. A playing song is
    // reloaded with the new patches and resumes where it was.
    bool set_bank(int bank_no);
    int  bank() const { return m_bank_no; }

    // True exactly once after the current song reaches its end (poll it).
    bool consume_song_ended() { return m_song_ended.exchange(false); }

    // Playback position / length in seconds (cached by the audio thread).
    double position() const { return m_pos.load(); }
    double length() const   { return m_len.load(); }

    unsigned rate() const { return m_rate; }

private:
    static void sdl_audio_cb(void *ud, Uint8 *stream, int len);
    void render(int16_t *out, int frames);
    bool load_locked(const std::string &path);   // caller holds audio lock
    void board_submit(BoardWriter::Clock::time_point when);   // audio lock held
    void board_reset_locked();                   // caller holds audio lock

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
    std::string       m_cur_path;
    std::string       m_midi_port;   // configured -m target ("" = auto-pick)
    AlsaMidi          m_midi;

    BoardWriter       m_writer;
    BoardWriter::Clock::time_point m_board_clock{};   // next slice's send time

    std::atomic<bool>   m_have_song{false};
    std::atomic<bool>   m_paused{false};
    std::atomic<bool>   m_song_ended{false};
    std::atomic<double> m_pos{0.0};
    std::atomic<double> m_len{0.0};

    float             m_gain = 2.0f;        // emulator output runs quiet
};

// Play the playlist's current track, skipping forward past any file that
// won't load (wrapping to the start only if `wrap`). Stops the engine and
// returns false if nothing playable was found. `announce` prints each title.
bool play_from_playlist(Engine &eng, Playlist &pl, bool wrap, bool announce);

#endif /* ENGINE_HPP */
