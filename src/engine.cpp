/*
 * engine.cpp — see engine.hpp.
 */
#include "engine.hpp"
#include "fanout_chip.hpp"
#include "playlist.hpp"

extern "C" {
#include "retrowave_serial.h"
}

#include "serial_ports.hpp"
#include "config.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <strings.h>
#include <unistd.h>
#include <vector>

bool Engine::init(unsigned sample_rate, const std::string &serial_dev,
                  const std::string &midi_port, int bank_no)
{
    m_rate = sample_rate;
    m_bank_no = m_default_bank = bank_no;

    // ── RetroWave board ───────────────────────────────────────────────────────
    // "" -> no board (PC only); "-" -> dry run; else open the serial device.
    // A missing board is not fatal: we simply fall back to PC audio.
    if(!open_board(serial_dev) && !serial_dev.empty())
        fprintf(stderr, "engine: no RetroWave board — PC audio only\n");
    m_serial_dev = serial_dev;         // the configured device, present or not
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
    adl_setNoteHook(m_adl, &Engine::note_cb, this);
    reset_song_state();

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

// Swap the board's serial device. The audio lock is held throughout so the
// callback never stages writes for a half-open board; that costs a short
// audio dropout (the chip resets take ~20 ms each), fine for a manual switch.
bool Engine::open_board(const std::string &serial_dev)
{
    if(m_dev) SDL_LockAudioDevice(m_dev);
    g_board_out.store(false);
    m_writer.stop();                       // nothing may write while we swap
    if(m_have_board){ retrowave_reset(); retrowave_close(); m_have_board = false; }

    if(!serial_dev.empty())
        m_have_board = retrowave_open(serial_dev.c_str());
    if(m_have_board || serial_dev.empty())
        m_serial_dev = serial_dev;         // a failed attempt isn't remembered
    if(m_have_board){
        // From here on register writes are staged and delivered, paced, by
        // the writer thread (see board_writer.hpp).
        retrowave_set_buffered(true);
        m_writer.start();
    }
    if(m_have_board) m_reconnect_armed = false;
    if(m_dev) SDL_UnlockAudioDevice(m_dev);
    return m_have_board;
}

// Reopen the board when its device node shows up. Only "armed" after the node
// has been seen missing (or was never opened): a board that died with its node
// still present is wedged, and reopening it would just stall on its reset.
// A fresh node may not have its permissions yet, hence a few attempts.
bool Engine::poll_board_reconnect()
{
    if(m_serial_dev.empty() || m_serial_dev == "-") return false;
    if(board_available()){ m_reconnect_armed = false; return false; }

    if(access(serial_dev_path(m_serial_dev).c_str(), F_OK) != 0){
        m_reconnect_armed = true;      // gone: try again when it comes back
        m_reconnect_tries = 0;
        return false;
    }
    if(!m_reconnect_armed || m_reconnect_tries >= 5) return false;
    ++m_reconnect_tries;
    return open_board(std::string(m_serial_dev));
}

void Engine::set_volume(float v)
{
    m_volume.store(v < 0.0f ? 0.0f : v > 2.0f ? 2.0f : v);
}

void Engine::set_midi_hint(const std::string &spec)
{
    if(m_midi_port.empty()) m_midi_port = spec;
}

std::string Engine::gm_spec() const { return m_midi.dest_spec(); }

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
    reset_song_state();
    if(adl_openFile(m_adl, path.c_str()) != 0){
        fprintf(stderr, "engine: %s: %s\n", path.c_str(), adl_errorInfo(m_adl));
        return false;
    }
    if(!inject_fanout_chip(m_adl, m_rate)){
        fprintf(stderr, "engine: chip injection failed\n");
        return false;
    }
    adl_setRawEventHook(m_adl, &Engine::raw_event_cb, this);   // survive the reset
    adl_setNoteHook(m_adl, &Engine::note_cb, this);
    adl_setTempo(m_adl, m_tempo);

    {
        std::lock_guard<std::mutex> lk(m_text_mx);
        const char *t = adl_metaMusicTitle(m_adl), *c = adl_metaMusicCopyright(m_adl);
        m_title = t ? t : "";
        m_copyright = c ? c : "";
    }
    size_t dot = path.rfind('.');
    m_is_kar = dot != std::string::npos && strcasecmp(path.c_str() + dot, ".kar") == 0;

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
    reset_song_state();
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
        for(auto &ch : m_midi_vel) for(auto &v : ch) v = 0;   // nothing is held now
        {
            std::lock_guard<std::mutex> lk(m_text_mx);
            m_lyric.clear(); m_lyric_break = false;
        }
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
    // The remembered/requested device first; if it's gone, any that works.
    m_have_midi = m_midi.open(m_midi_port)
               || (!m_midi_port.empty() && m_midi.open(""));
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
void Engine::raw_event_cb(void *ud, uint8_t type, uint8_t subtype,
                          uint8_t channel, const uint8_t *data, size_t len)
{
    static_cast<Engine *>(ud)->on_raw_event(type, subtype, channel, data, len);
}

// A lyric fragment. Lines end with CR/LF (lyric events) or start with '/' or
// '\\' (.kar text events); the line is cleared when the *next* one begins, so
// a finished line stays readable until then.
void Engine::on_lyric(const uint8_t *data, size_t len, bool kar_text)
{
    if(kar_text && len > 0 && data[0] == '@') return;   // .kar file info, not lyrics
    std::lock_guard<std::mutex> lk(m_text_mx);
    for(size_t i = 0; i < len; ++i){
        char c = (char)data[i];
        if(c == '\r' || c == '\n' || (kar_text && i == 0 && (c == '/' || c == '\\'))){
            m_lyric_break = true;
            continue;
        }
        if(m_lyric_break){ m_lyric.clear(); m_lyric_break = false; }
        if(m_lyric.size() < 200) m_lyric += c;
    }
}

void Engine::note_cb(void *ud, int adlchn, int /*note*/, int ins, int pressure, double /*bend*/)
{
    if(pressure <= 0) return;              // only key-ons say what is playing
    // libADLMIDI's channels 18..22 are the rhythm-mode drums on OPL channels 6-8.
    static const int rhythm[5] = { 6, 7, 8, 8, 7 };
    int ch = adlchn % 23;
    if(ch >= 18) ch = rhythm[ch - 18];
    static_cast<Engine *>(ud)->m_chan_ins[ch].store(ins, std::memory_order_relaxed);
}

void Engine::reset_song_state()
{
    for(auto &i : m_chan_ins)  i = -1;
    for(auto &p : m_midi_prog) p = 0;
    for(auto &p : m_midi_peak) p = 0;
    for(auto &ch : m_midi_vel) for(auto &v : ch) v = 0;
    std::lock_guard<std::mutex> lk(m_text_mx);
    m_title.clear(); m_copyright.clear(); m_lyric.clear();
    m_lyric_break = false;
}

int Engine::channel_instrument(int ch) const
{
    return (ch >= 0 && ch < 18) ? m_chan_ins[ch].load(std::memory_order_relaxed) : -1;
}

int Engine::midi_program(int ch) const
{
    return (ch >= 0 && ch < 16) ? m_midi_prog[ch].load(std::memory_order_relaxed) : 0;
}

void Engine::midi_held(int ch, uint8_t vel[128]) const
{
    for(int n = 0; n < 128; ++n)
        vel[n] = (ch >= 0 && ch < 16) ? m_midi_vel[ch][n].load(std::memory_order_relaxed) : 0;
}

float Engine::midi_take_peak(int ch)
{
    if(ch < 0 || ch >= 16) return 0.0f;
    return m_midi_peak[ch].exchange(0, std::memory_order_relaxed) / 127.0f;
}

void Engine::set_midi_mute_mask(uint32_t mask)
{
    mask &= 0xFFFF;
    uint32_t newly = mask & ~m_midi_mute.load();
    SDL_LockAudioDevice(m_dev);
    m_midi_mute.store(mask);
    if(m_have_midi && g_gm_out.load())
        for(int ch = 0; ch < 16; ++ch)
            if(newly & (1u << ch)){ m_midi.send(0xB, ch, 123, 0); m_midi.send(0xB, ch, 120, 0); }
    SDL_UnlockAudioDevice(m_dev);
}

std::string Engine::title() const     { std::lock_guard<std::mutex> lk(m_text_mx); return m_title; }
std::string Engine::copyright() const { std::lock_guard<std::mutex> lk(m_text_mx); return m_copyright; }
std::string Engine::lyric() const     { std::lock_guard<std::mutex> lk(m_text_mx); return m_lyric; }

void Engine::on_raw_event(uint8_t type, uint8_t subtype, uint8_t channel,
                          const uint8_t *data, size_t len)
{
    // Track the MIDI view of the song for the display, whatever the output.
    const int ch = channel & 0x0F;
    const uint8_t d0 = len > 0 ? data[0] : 0, d1 = len > 1 ? data[1] : 0;
    switch(type){
    case 0x09:
        if(d1 > 0){
            m_midi_vel[ch][d0 & 0x7F] = d1;
            if(d1 > m_midi_peak[ch].load(std::memory_order_relaxed)) m_midi_peak[ch] = d1;
            break;
        }
        [[fallthrough]];   // note-on with velocity 0 is a note-off
    case 0x08: m_midi_vel[ch][d0 & 0x7F] = 0; break;
    case 0x0B: if(d0 == 120 || d0 == 123) for(auto &v : m_midi_vel[ch]) v = 0; break;
    case 0x0C: m_midi_prog[ch] = d0 & 0x7F; break;
    case 0xFF:
        if(subtype == 0x05)                  on_lyric(data, len, false);
        else if(subtype == 0x01 && m_is_kar) on_lyric(data, len, true);
        break;
    }

    if(!g_gm_out.load(std::memory_order_relaxed) || !m_have_midi)
        return;
    if(type == 0x09 && d1 > 0 && (m_midi_mute.load(std::memory_order_relaxed) & (1u << ch)))
        return;   // muted channel: its note-ons stay home
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
    m_bank_no = m_default_bank = bank_no;
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

// Emulator output is quiet: apply gain. Peaks past the knee are rounded off
// (tanh) rather than clipped flat, so loud passages and volumes above 100%
// distort gently instead of crackling.
static void apply_gain(int16_t *out, int count, float gain)
{
    const float knee = 24000.0f, room = 32767.0f - knee;
    for(int i = 0; i < count; ++i){
        float s = out[i] * gain;
        float a = std::fabs(s);
        if(a > knee){
            a = knee + room * std::tanh((a - knee) / room);
            s = s < 0 ? -a : a;
        }
        out[i] = (int16_t)s;
    }
}

// ── Offline export ────────────────────────────────────────────────────────────
static void put32(std::vector<uint8_t> &v, size_t at, uint32_t x)
{
    for(int i = 0; i < 4; ++i) v[at + i] = (uint8_t)(x >> (8 * i));
}

static bool write_file(const std::string &path, const void *data, size_t len, std::string &err)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if(f) f.write(static_cast<const char *>(data), (std::streamsize)len);
    if(!f){ err = "cannot write " + path; return false; }
    return true;
}

// The song is rendered through the live player — the emulator and the register
// shadow are process-wide, so a second player would trample it — with the
// audio callback locked out, the board and the external synth disconnected,
// and every register write captured. Then what was playing is put back.
bool Engine::export_song(const std::string &src, const std::string &vgm_path,
                         const std::string &wav_path, std::string &err)
{
    std::vector<uint8_t> vgm(0x100, 0);   // header, filled in at the end
    std::vector<int16_t> pcm;
    uint64_t vgm_samples = 0;             // VGM time is always in 44100 Hz samples

    SDL_LockAudioDevice(m_dev);
    const bool        had_song = m_have_song.load(), was_paused = m_paused.load();
    const std::string prev = m_cur_path;
    const double      pos = had_song ? adl_positionTell(m_adl) : 0.0;
    const bool        board_out = g_board_out.load(), gm_out = g_gm_out.load(), loop = m_loop;

    g_board_out.store(false);
    g_gm_out.store(false);
    if(m_have_board) board_reset_locked();
    if(m_have_midi)  m_midi.panic();
    m_loop = false;                       // render it once, start to end

    std::vector<OplWrite> cap;
    g_opl_capture = &cap;
    bool ok = load_locked(src);
    if(!ok) err = "cannot load " + src;
    if(ok){
        uint32_t pending = 0;             // wait not yet written (merged across slices)
        double   frac = 0.0;
        auto flush = [&](bool force){
            if(cap.empty() && !force) return;
            for(; pending; ){
                uint32_t k = pending > 65535 ? 65535 : pending;
                vgm.push_back(0x61); vgm.push_back((uint8_t)k); vgm.push_back((uint8_t)(k >> 8));
                pending -= k;
            }
            for(const OplWrite &w : cap){
                vgm.push_back((w.addr & 0x100) ? 0x5F : 0x5E);   // YMF262 port 1 / 0
                vgm.push_back((uint8_t)w.addr);
                vgm.push_back(w.data);
            }
            cap.clear();
        };
        auto wait = [&](int frames){
            frac += frames * 44100.0 / m_rate;
            uint32_t n = (uint32_t)frac;
            frac -= n; pending += n; vgm_samples += n;
        };

        // Short slices keep each write within ~0.4 ms of its true time.
        const int slice = 16;
        int16_t buf[slice * 2];
        const uint64_t max_frames = (uint64_t)m_rate * 3600;   // runaway guard: 1 h
        for(uint64_t done = 0; done < max_frames; ){
            int g = adl_play(m_adl, slice * 2, buf);
            if(g < 0) g = 0;
            flush(false);
            if(!wav_path.empty()) pcm.insert(pcm.end(), buf, buf + g);
            wait(g / 2);
            done += (uint64_t)(g / 2);
            if(g < slice * 2) break;      // song end
        }
        // Key everything off and let the releases ring out for a second.
        fanout_clear_keys();
        fanout_rekey();
        flush(false);
        for(int left = (int)m_rate; left > 0; left -= slice){
            adlib_getsample(buf, slice);
            if(!wav_path.empty()) pcm.insert(pcm.end(), buf, buf + slice * 2);
            wait(slice);
        }
        flush(true);
        vgm.push_back(0x66);              // end of sound data
    }
    g_opl_capture = nullptr;

    // Put playback back as it was.
    m_loop = loop;
    if(had_song && load_locked(prev)){
        adl_positionSeek(m_adl, pos);
        m_pos = adl_positionTell(m_adl);
        if(was_paused){ m_paused = true; g_opl_hold = true; fanout_rekey(); }
    } else {
        m_have_song = false;
        m_cur_path.clear();
        m_pos = 0.0; m_len = 0.0;
        reset_song_state();
        fanout_clear_keys();
    }
    g_gm_out.store(gm_out);
    g_board_out.store(board_out && board_available());
    if(g_board_out.load()) fanout_replay_to_board();
    SDL_UnlockAudioDevice(m_dev);
    if(!ok) return false;

    if(!vgm_path.empty()){
        memcpy(vgm.data(), "Vgm ", 4);
        put32(vgm, 0x04, (uint32_t)vgm.size() - 4);     // EOF offset
        put32(vgm, 0x08, 0x00000151);                   // version 1.51 (first with YMF262)
        put32(vgm, 0x18, (uint32_t)vgm_samples);        // total samples
        put32(vgm, 0x34, 0x100 - 0x34);                 // data offset
        put32(vgm, 0x5C, 14318180);                     // YMF262 clock
        if(!write_file(vgm_path, vgm.data(), vgm.size(), err)) return false;
    }
    if(!wav_path.empty()){
        apply_gain(pcm.data(), (int)pcm.size(), kBaseGain);
        std::vector<uint8_t> wav(44, 0);
        const uint32_t bytes = (uint32_t)(pcm.size() * sizeof(int16_t));
        memcpy(wav.data(), "RIFF", 4);  put32(wav, 4, 36 + bytes);
        memcpy(wav.data() + 8, "WAVEfmt ", 8);
        put32(wav, 16, 16);
        wav[20] = 1; wav[22] = 2;                       // PCM, stereo
        put32(wav, 24, m_rate); put32(wav, 28, m_rate * 4);
        wav[32] = 4; wav[34] = 16;                      // block align, bits
        memcpy(wav.data() + 36, "data", 4); put32(wav, 40, bytes);
        wav.insert(wav.end(), (const uint8_t *)pcm.data(), (const uint8_t *)pcm.data() + bytes);
        if(!write_file(wav_path, wav.data(), wav.size(), err)) return false;
    }
    return true;
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
    const bool gm    = m_have_midi && g_gm_out.load();
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
    // With the board or an external synth on, render in short slices so each
    // slice's output goes out at its own time instead of in one burst per
    // block: board bytes via the writer thread, MIDI events stamped for the
    // ALSA sequencer to deliver.
    const int slice = (board || gm) ? 64 : frames;   // 64 frames ~ 1.5 ms at 44.1 kHz
    int want = frames * 2, got = 0;
    for(int f = 0; f < frames; f += slice){
        int n = (frames - f < slice ? frames - f : slice) * 2;
        const Clock::time_point when = base + block * f / frames;
        if(gm) m_midi.set_delay(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    when - Clock::now()).count());
        int g = adl_play(m_adl, n, out + f * 2);
        if(g < 0) g = 0;
        got += g;
        if(board) board_submit(when);
        if(g < n) break;   // song end
    }
    if(gm) m_midi.set_delay(0);   // anything sent from outside the callback: now
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

    apply_gain(out, want, kBaseGain * m_volume.load());
}

// ── Playlist helper ───────────────────────────────────────────────────────────
// The bank the user chose for this file last time (see song_bank_set), as an
// ordinal in this libADLMIDI's bank table; -1 if none or it no longer exists.
static int remembered_bank(const std::string &path)
{
    std::string name = song_bank_get(path);
    if(name.empty()) return -1;
    const char *const *names = adl_getBankNames();
    for(int i = 0, n = adl_getBanksCount(); i < n; ++i)
        if(names[i] && name == names[i]) return i;
    return -1;
}

bool play_from_playlist(Engine &eng, Playlist &pl, bool wrap, bool announce)
{
    for(size_t tries = 0; tries < pl.size(); ++tries){
        const Track *t = pl.current();
        if(!t) break;
        if(announce) printf("Playing: %s\n", t->name.c_str());
        // This song's own bank if it has one; otherwise the general choice
        // (not whatever the previous song happened to bring with it).
        int rb = remembered_bank(t->path);
        eng.set_next_bank(rb >= 0 ? rb : eng.default_bank());
        if(eng.load(t->path)) return true;
        fprintf(stderr, "Skipping unplayable file: %s\n", t->path.c_str());
        if(!pl.next(wrap)) break;
    }
    eng.stop();
    return false;
}
