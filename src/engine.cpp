/*
 * engine.cpp — see engine.hpp.
 */
#include "engine.hpp"
#include "fanout_chip.hpp"

extern "C" {
#include "retrowave_serial.h"
}

#include <cstdio>
#include <cstring>

bool Engine::init(unsigned sample_rate, const std::string &serial_dev,
                  const std::string &midi_port, int bank_no)
{
    m_rate = sample_rate;
    m_bank_no = bank_no;

    // ── RetroWave board ───────────────────────────────────────────────────────
    // "" -> no board (PC only); "-" -> dry run; else open the serial device.
    // A missing board is not fatal: we simply fall back to PC audio.
    if(!serial_dev.empty()){
        m_have_board = retrowave_open(serial_dev.c_str());
        if(!m_have_board)
            fprintf(stderr, "engine: no RetroWave board — PC audio only\n");
    }
    g_board_out.store(m_have_board);   // don't route to a board that isn't there

    // ── External MIDI device ──────────────────────────────────────────────────
    // If -m was given, connect now. Otherwise leave it disconnected: the GUI can
    // connect on demand (ensure_midi) when the user switches to External output.
    m_midi_port = (midi_port == "auto") ? "" : midi_port;
    if(!midi_port.empty())
        m_have_midi = m_midi.open(m_midi_port);
    g_gm_out.store(false);

    // ── libADLMIDI ────────────────────────────────────────────────────────────
    m_adl = adl_init((long)m_rate);
    if(!m_adl){ fprintf(stderr, "engine: adl_init failed\n"); return false; }

    // Observe the raw MIDI event stream (to forward to the external device).
    adl_setRawEventHook(m_adl, &Engine::raw_event_cb, this);

    if(adl_setBank(m_adl, m_bank_no) != 0){
        fprintf(stderr, "engine: bank %d: %s\n", m_bank_no, adl_errorInfo(m_adl));
        // keep going with whatever the default bank is
    }
    adl_setVolumeRangeModel(m_adl, ADLMIDI_VolumeModel_AUTO);
    adl_setNumChips(m_adl, 1);
    adl_setSoftPanEnabled(m_adl, 0);   // real OPL3 = hardware panning only

    // ── SDL audio ─────────────────────────────────────────────────────────────
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq     = (int)m_rate;
    want.format   = AUDIO_S16SYS;
    want.channels = 2;
    want.samples  = 1024;
    want.callback = &Engine::sdl_audio_cb;
    want.userdata = this;

    m_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if(!m_dev){ fprintf(stderr, "engine: SDL_OpenAudioDevice: %s\n", SDL_GetError()); return false; }
    m_rate = (unsigned)have.freq;   // honour what SDL actually gave us
    SDL_PauseAudioDevice(m_dev, 0); // start the callback (idles until a song loads)
    return true;
}

void Engine::shutdown()
{
    if(m_dev){ SDL_CloseAudioDevice(m_dev); m_dev = 0; }
    if(m_have_midi){ m_midi.close(); m_have_midi = false; }
    if(m_adl){ adl_panic(m_adl); adl_close(m_adl); m_adl = nullptr; }
    if(m_have_board){ retrowave_reset(); retrowave_close(); m_have_board = false; }
}

// ── Song loading (locks the audio callback out while reconfiguring) ───────────
bool Engine::load_locked(const std::string &path)
{
    // Re-assert bank/chip config, then load. adl_openFile resets the synth and
    // recreates its chip, so we must re-inject FanoutOPL3 *after* it.
    if(m_have_midi) m_midi.panic();   // clear any notes held by the previous song

    adl_setBank(m_adl, m_bank_no);
    adl_setVolumeRangeModel(m_adl, ADLMIDI_VolumeModel_AUTO);
    adl_setNumChips(m_adl, 1);
    adl_setLoopEnabled(m_adl, m_loop ? 1 : 0);

    if(adl_openFile(m_adl, path.c_str()) != 0){
        fprintf(stderr, "engine: %s: %s\n", path.c_str(), adl_errorInfo(m_adl));
        m_have_song = false;
        return false;
    }
    if(!inject_fanout_chip(m_adl, m_rate)){
        fprintf(stderr, "engine: chip injection failed\n");
        m_have_song = false;
        return false;
    }
    adl_setRawEventHook(m_adl, &Engine::raw_event_cb, this);   // survive the reset
    adl_setTempo(m_adl, m_tempo);

    m_cur_path = path;
    m_song_ended.store(false);
    m_have_song = true;
    return true;
}

bool Engine::load(const std::string &path)
{
    SDL_LockAudioDevice(m_dev);
    bool ok = load_locked(path);
    SDL_UnlockAudioDevice(m_dev);
    return ok;
}

void Engine::stop()
{
    SDL_LockAudioDevice(m_dev);
    m_have_song = false;
    m_cur_path.clear();
    if(m_have_midi) m_midi.panic();       // hush the external device
    SDL_UnlockAudioDevice(m_dev);
    if(m_have_board) retrowave_reset();   // hush the board
}

// ── Transport / routing ───────────────────────────────────────────────────────
void Engine::set_loop(bool l)
{
    m_loop = l;
    if(m_adl){
        SDL_LockAudioDevice(m_dev);
        adl_setLoopEnabled(m_adl, l ? 1 : 0);
        SDL_UnlockAudioDevice(m_dev);
    }
}

void Engine::set_tempo(double scale)
{
    if(scale <= 0.0) return;
    m_tempo = scale;
    if(m_adl && m_have_song){
        SDL_LockAudioDevice(m_dev);
        adl_setTempo(m_adl, m_tempo);
        SDL_UnlockAudioDevice(m_dev);
    }
}

void Engine::set_board_out(bool on)
{
    bool was = g_board_out.load();
    g_board_out.store(on && m_have_board);
    if(was && !on && m_have_board){
        SDL_LockAudioDevice(m_dev);   // don't race the callback's serial writes
        retrowave_reset();
        SDL_UnlockAudioDevice(m_dev);
    }
}

void Engine::set_pc_out(bool on) { g_pc_out.store(on); }

void Engine::set_gm_out(bool on)
{
    bool was = g_gm_out.load();
    g_gm_out.store(on && m_have_midi);
    if(was && !on && m_have_midi){
        SDL_LockAudioDevice(m_dev);   // stop the hook, then clear held notes
        m_midi.panic();
        SDL_UnlockAudioDevice(m_dev);
    }
}

bool Engine::board_out() const     { return g_board_out.load(); }
bool Engine::pc_out() const        { return g_pc_out.load(); }
bool Engine::gm_out() const        { return g_gm_out.load(); }
bool Engine::board_available() const { return m_have_board; }
bool Engine::gm_available() const    { return m_have_midi; }
bool Engine::gm_connectable() const  { return m_have_midi || AlsaMidi::any_available(); }
const std::string &Engine::gm_name() const { return m_midi.dest_name(); }
std::string Engine::gm_addr() const  { return m_midi.dest_addr(); }

// Connect the external MIDI device on demand (GUI switched to External output).
// Locks audio because the raw-event hook reads m_midi / m_have_midi.
bool Engine::ensure_midi()
{
    if(m_have_midi) return true;
    SDL_LockAudioDevice(m_dev);
    m_have_midi = m_midi.open(m_midi_port);   // "" => auto-pick first device
    SDL_UnlockAudioDevice(m_dev);
    return m_have_midi;
}

// (Re)connect to a specific ALSA address (device picker in the GUI). Closes the
// previous connection (which sends all-notes-off to it first).
bool Engine::connect_midi(const std::string &addr)
{
    SDL_LockAudioDevice(m_dev);
    m_midi.close();
    m_have_midi = m_midi.open(addr);
    SDL_UnlockAudioDevice(m_dev);
    return m_have_midi;
}

// Raw MIDI event hook: fires from adl_play() on the audio thread. Forward
// channel-voice events to the external device when that output is selected.
void Engine::raw_event_cb(void *ud, uint8_t type, uint8_t /*subtype*/,
                          uint8_t channel, const uint8_t *data, size_t len)
{
    static_cast<Engine *>(ud)->on_raw_event(type, channel, data, len);
}

void Engine::on_raw_event(uint8_t type, uint8_t channel, const uint8_t *data, size_t len)
{
    if(!g_gm_out.load(std::memory_order_relaxed) || !m_have_midi)
        return;
    if(type >= 0x08 && type <= 0x0E)   // note/CC/program/pitch etc. (skip meta/sysex)
        m_midi.send(type, channel, len > 0 ? data[0] : 0, len > 1 ? data[1] : 0);
}

bool Engine::set_bank(int bank_no)
{
    m_bank_no = bank_no;
    if(!m_adl) return true;
    // Reload the current song so the new bank's patches take effect cleanly.
    if(m_have_song){
        std::string p = m_cur_path;
        return load(p);
    }
    SDL_LockAudioDevice(m_dev);
    adl_setBank(m_adl, m_bank_no);
    adl_setVolumeRangeModel(m_adl, ADLMIDI_VolumeModel_AUTO);
    adl_setNumChips(m_adl, 1);
    SDL_UnlockAudioDevice(m_dev);
    return true;
}

double Engine::position() const { return (m_adl && m_have_song) ? adl_positionTell(m_adl) : 0.0; }
double Engine::length()   const { return (m_adl && m_have_song) ? adl_totalTimeLength(m_adl) : 0.0; }

// ── Audio callback (the master clock) ─────────────────────────────────────────
void Engine::sdl_audio_cb(void *ud, Uint8 *stream, int len)
{
    static_cast<Engine *>(ud)->render(reinterpret_cast<int16_t *>(stream), len / 4);
}

void Engine::render(int16_t *out, int frames)
{
    if(!m_have_song || m_paused.load()){
        std::memset(out, 0, (size_t)frames * 2 * sizeof(int16_t));
        return;
    }

    // adl_play advances the sequencer (firing OPL writes to board + emu) and
    // returns the emulator's samples. sampleCount is int16 values (2/frame).
    int want = frames * 2;
    int got  = adl_play(m_adl, want, out);
    if(got < 0) got = 0;
    if(got < want){
        std::memset(out + got, 0, (size_t)(want - got) * sizeof(int16_t)); // song end
        m_song_ended.store(true);
    }

    if(!g_pc_out.load()){
        // Board-only: mute speakers, but the sequencer already reached the board
        // and the emulator has updated the visualizer levels this block.
        std::memset(out, 0, (size_t)want * sizeof(int16_t));
        return;
    }

    // Emulator output is quiet; apply a modest gain with hard clipping.
    for(int i = 0; i < want; ++i){
        int s = (int)(out[i] * m_gain);
        out[i] = (int16_t)(s > 32767 ? 32767 : (s < -32768 ? -32768 : s));
    }
}
