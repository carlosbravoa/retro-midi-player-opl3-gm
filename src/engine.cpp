/*
 * engine.cpp — see engine.hpp.
 */
#include "engine.hpp"
#include "fanout_chip.hpp"
#include "playlist.hpp"

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
    if(m_have_board){
        // From here on register writes are staged and delivered, paced, by
        // the writer thread (see board_writer.hpp).
        retrowave_set_buffered(true);
        m_writer.start();
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
    m_writer.stop();
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

    m_have_song = false;
    m_pos = 0.0; m_len = 0.0;
    if(adl_openFile(m_adl, path.c_str()) != 0){
        fprintf(stderr, "engine: %s: %s\n", path.c_str(), adl_errorInfo(m_adl));
        return false;
    }
    if(!inject_fanout_chip(m_adl, m_rate)){
        fprintf(stderr, "engine: chip injection failed\n");
        return false;
    }
    adl_setRawEventHook(m_adl, &Engine::raw_event_cb, this);   // survive the reset
    adl_setTempo(m_adl, m_tempo);

    m_cur_path = path;
    m_len = adl_totalTimeLength(m_adl);
    m_paused = false;                 // loading a song means playing it
    g_opl_hold = false;
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
    m_paused = false;
    g_opl_hold = false;
    m_cur_path.clear();
    m_pos = 0.0; m_len = 0.0;
    if(m_have_midi) m_midi.panic();       // hush the external device
    fanout_clear_keys();                  // don't replay dead notes to the board
    if(m_have_board) board_reset_locked();   // hush the board
    SDL_UnlockAudioDevice(m_dev);
}

void Engine::set_paused(bool p)
{
    if(p == m_paused.load()) return;
    SDL_LockAudioDevice(m_dev);
    m_paused = p;
    // Key every channel off (or back on) through the mute path, so held notes
    // stop on the board too instead of droning until resume.
    g_opl_hold = p;
    fanout_rekey();
    if(p && m_have_midi) m_midi.panic();   // the external synth can't be re-keyed
    SDL_UnlockAudioDevice(m_dev);
}

void Engine::seek(double seconds)
{
    SDL_LockAudioDevice(m_dev);
    if(m_have_song){
        if(seconds < 0) seconds = 0;
        adl_positionSeek(m_adl, seconds);
        if(m_have_midi) m_midi.panic();
        m_pos = adl_positionTell(m_adl);
        m_song_ended = false;
    }
    SDL_UnlockAudioDevice(m_dev);
}

// Drop anything staged/queued for the board and reset the chip (the reset
// itself runs on the writer thread, so the audio lock isn't held for 20 ms).
void Engine::board_reset_locked()
{
    retrowave_discard();
    m_writer.reset();
}

// Hand this slice's staged board bytes to the writer, to go out at `when`.
void Engine::board_submit(BoardWriter::Clock::time_point when)
{
    std::vector<uint8_t> bytes;
    uint8_t tmp[4096];
    size_t n;
    while((n = retrowave_take(tmp, sizeof tmp)) > 0)
        bytes.insert(bytes.end(), tmp, tmp + n);
    if(!bytes.empty())
        m_writer.submit(std::move(bytes), when);
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
    bool now = on && board_available();
    if(was == now) return;
    SDL_LockAudioDevice(m_dev);   // don't race the callback's register writes
    g_board_out.store(now);
    if(now){
        // The board was reset (or never saw this song): it missed the OPL3-mode
        // enable and the patch setup libADLMIDI wrote at load. Replay the whole
        // register shadow so it joins mid-song in the same state as the emulator.
        fanout_replay_to_board();
    } else if(m_have_board){
        board_reset_locked();
    }
    SDL_UnlockAudioDevice(m_dev);
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

void Engine::set_mute_mask(uint32_t mask)
{
    SDL_LockAudioDevice(m_dev);   // key regs are rewritten from the shadow
    g_opl_mute.store(mask & 0x3FFFF);
    fanout_rekey();
    SDL_UnlockAudioDevice(m_dev);
}

void Engine::set_channel_muted(int ch, bool muted)
{
    if(ch < 0 || ch >= 18) return;
    uint32_t m = g_opl_mute.load();
    set_mute_mask(muted ? (m | (1u << ch)) : (m & ~(1u << ch)));
}

bool Engine::channel_muted(int ch) const
{
    return ch >= 0 && ch < 18 && (g_opl_mute.load() & (1u << ch));
}

uint32_t Engine::mute_mask() const { return g_opl_mute.load(); }

bool Engine::board_out() const     { return g_board_out.load(); }
bool Engine::pc_out() const        { return g_pc_out.load(); }
bool Engine::gm_out() const        { return g_gm_out.load(); }
bool Engine::board_available() const { return m_have_board && retrowave_active; }
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
    if(type >= 0x08 && type <= 0x0E)   // note/CC/program/pitch etc.
        m_midi.send(type, channel, len > 0 ? data[0] : 0, len > 1 ? data[1] : 0);
    else if(type == 0xF0 && len > 1){
        // SysEx (GM/GS/XG resets, reverb/chorus setup...). libADLMIDI keeps the
        // lead byte: 0xF0 = a normal message (payload ends in 0xF7); 0xF7 = an
        // escape, whose payload is sent raw.
        if(data[0] == 0xF0)      m_midi.send_sysex(data, len);
        else if(data[0] == 0xF7) m_midi.send_sysex(data + 1, len - 1);
    }
}

bool Engine::set_bank(int bank_no)
{
    m_bank_no = bank_no;
    if(!m_adl) return true;
    bool ok = true;
    SDL_LockAudioDevice(m_dev);
    if(m_have_song){
        // Reload the current song so the new bank's patches take effect
        // cleanly, then put it back where it was (and paused if it was).
        double pos = adl_positionTell(m_adl);
        bool was_paused = m_paused.load();
        std::string p = m_cur_path;
        ok = load_locked(p);
        if(ok){
            adl_positionSeek(m_adl, pos);
            m_pos = adl_positionTell(m_adl);
            if(was_paused){ m_paused = true; g_opl_hold = true; fanout_rekey(); }
        }
    } else {
        adl_setBank(m_adl, m_bank_no);
        adl_setVolumeRangeModel(m_adl, ADLMIDI_VolumeModel_AUTO);
        adl_setNumChips(m_adl, 1);
    }
    SDL_UnlockAudioDevice(m_dev);
    return ok;
}

// ── Audio callback (the master clock) ─────────────────────────────────────────
void Engine::sdl_audio_cb(void *ud, Uint8 *stream, int len)
{
    static_cast<Engine *>(ud)->render(reinterpret_cast<int16_t *>(stream), len / 4);
}

void Engine::render(int16_t *out, int frames)
{
    using Clock = BoardWriter::Clock;
    const bool board = m_have_board && retrowave_active && g_board_out.load();
    const Clock::duration block = std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>((double)frames / m_rate));

    // Board send times run on their own clock, advanced one block per callback
    // so slices stay evenly spaced even when callbacks arrive unevenly. Never
    // behind "now"; and if a burst of callbacks pushed it far ahead, pull it
    // back rather than let board latency grow.
    Clock::time_point now = Clock::now();
    Clock::time_point base = m_board_clock < now ? now : m_board_clock;
    if(base > now + 2 * block) base = now + 2 * block;

    if(!m_have_song || m_paused.load()){
        std::memset(out, 0, (size_t)frames * 2 * sizeof(int16_t));
        if(board) board_submit(now);   // pause/mute/replay writes from the GUI
        m_board_clock = now;
        return;
    }

    // adl_play advances the sequencer (firing OPL writes to board + emu) and
    // returns the emulator's samples. sampleCount is int16 values (2/frame).
    // With the board on, render in short slices so each slice's register
    // writes can be sent at its own time instead of in one burst per block.
    const int slice = board ? 64 : frames;   // 64 frames ~ 1.5 ms at 44.1 kHz
    int want = frames * 2, got = 0;
    for(int f = 0; f < frames; f += slice){
        int n = (frames - f < slice ? frames - f : slice) * 2;
        int g = adl_play(m_adl, n, out + f * 2);
        if(g < 0) g = 0;
        got += g;
        if(board) board_submit(base + block * f / frames);
        if(g < n) break;   // song end
    }
    m_board_clock = base + block;
    m_pos = adl_positionTell(m_adl);
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

// ── Playlist helper ───────────────────────────────────────────────────────────
bool play_from_playlist(Engine &eng, Playlist &pl, bool wrap, bool announce)
{
    for(size_t tries = 0; tries < pl.size(); ++tries){
        const Track *t = pl.current();
        if(!t) break;
        if(announce) printf("Playing: %s\n", t->name.c_str());
        if(eng.load(t->path)) return true;
        fprintf(stderr, "Skipping unplayable file: %s\n", t->path.c_str());
        if(!pl.next(wrap)) break;
    }
    eng.stop();
    return false;
}
