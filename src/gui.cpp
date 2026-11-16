/*
 * gui.cpp — see gui.hpp.
 *
 * Drawing helpers, the three visualizer styles, and the control/icon buttons
 * are adapted from the descent-hmp-player / tyrian-retrowave visualizer. New
 * here: the Winamp-style playlist panel (scrollable track list, current row
 * highlighted, click to play, wheel to scroll) and the extra controls this
 * player needs — output routing (PC / Board / External MIDI), a picker that is
 * the FM bank list for Board/PC and the ALSA device list in External mode, a
 * shuffle toggle, a recurse toggle, an "Open Folder" button (zenity/kdialog),
 * a click-to-seek progress bar, per-OPL3-channel mute/solo on the bars, and a
 * PORT dialog that picks the board's serial device (detected USB-serial ports,
 * or any typed path) and reopens the board on it without restarting.
 */
#include "gui.hpp"
#include "engine.hpp"
#include "playlist.hpp"
#include "fanout_chip.hpp"
#include "config.hpp"

extern "C" {
#include "emu/opl.h"
}
#include "font5x7.h"

#include <adlmidi.h>
#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define NCH 18   // OPL3: 18 two-operator channels

// ── Shared drawing state ──────────────────────────────────────────────────────
namespace {

Engine   *g_eng = nullptr;
Playlist *g_pl  = nullptr;

SDL_Window   *g_win = nullptr;
SDL_Renderer *ren   = nullptr;

struct RGB { Uint8 r, g, b; };

RGB hsv(float h, float s, float v)
{
    h = fmodf(h, 1.0f); if (h < 0) h += 1.0f;
    float i = floorf(h * 6.0f), f = h * 6.0f - i;
    float p = v * (1 - s), q = v * (1 - f * s), t = v * (1 - (1 - f) * s);
    float r, g, b;
    switch (((int)i) % 6) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
    }
    return RGB{ (Uint8)(r * 255), (Uint8)(g * 255), (Uint8)(b * 255) };
}

void fill(int x, int y, int w, int h, RGB c, Uint8 a = 255)
{
    SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, a);
    SDL_Rect q = { x, y, w, h };
    SDL_RenderFillRect(ren, &q);
}

void draw_text(int x, int y, int s, RGB c, const char *txt)
{
    SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, 255);
    int cx = x;
    for (const char *p = txt; *p; ++p) {
        const uint8_t *g = font_glyph(*p);
        if (g)
            for (int ry = 0; ry < FONT_H; ++ry)
                for (int rx = 0; rx < FONT_W; ++rx)
                    if (g[ry] & (1 << (FONT_W - 1 - rx))) {
                        SDL_Rect q = { cx + rx * s, y + ry * s, s, s };
                        SDL_RenderFillRect(ren, &q);
                    }
        cx += (FONT_W + 1) * s;
    }
}
int text_w(const char *t, int s) { return (int)strlen(t) * (FONT_W + 1) * s; }

// Truncate a string to fit `maxw` px at scale `s`, appending ".." if clipped.
std::string ellipsize(const std::string &in, int maxw, int s)
{
    if (text_w(in.c_str(), s) <= maxw) return in;
    std::string out = in;
    while (!out.empty() && text_w((out + "..").c_str(), s) > maxw) out.pop_back();
    return out + "..";
}

void fill_tri(int x, int y, int w, int h, int dir, RGB c)
{
    SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, 255);
    for (int ry = 0; ry < h; ++ry) {
        float t = 1.0f - fabsf((ry - h / 2.0f) / (h / 2.0f));
        int len = (int)(w * t + 0.5f); if (len < 1) len = 1;
        int lx = (dir > 0) ? x : x + (w - len);
        SDL_Rect q = { lx, y + ry, len, 1 };
        SDL_RenderFillRect(ren, &q);
    }
}

// ── Buttons ───────────────────────────────────────────────────────────────────
enum {
    ACT_PREV, ACT_PLAY, ACT_NEXT, ACT_STOP, ACT_LOOP, ACT_SHUFFLE, ACT_STYLE,
    ACT_TDN, ACT_TUP, ACT_OUTPUT, ACT_BANK_PREV, ACT_BANK_NEXT, ACT_RECURSE,
    ACT_OPEN, ACT_MIDI_PREV, ACT_MIDI_NEXT, ACT_UNMUTE_ALL, ACT_SEEK,
    ACT_NOP,                      // swallows clicks (the port dialog's panel)
    ACT_PORT,                     // open / close the board-port dialog
    ACT_PORT_CLOSE, ACT_PORT_RESCAN, ACT_PORT_NONE, ACT_PORT_CUSTOM,
    ACT_CHAN_BASE,                // ACT_CHAN_BASE + ch => toggle OPL3 channel ch
    ACT_PORTSEL_BASE = ACT_CHAN_BASE + NCH,      // + n => detected port n
    ACT_ROW_BASE = ACT_PORTSEL_BASE + 32   // + n => playlist row n (must stay last)
};

struct Button { SDL_Rect r; int act; };
Button g_btn[256];
int    g_nbtn = 0;
void reg_button(SDL_Rect b, int act) { if (g_nbtn < 256) g_btn[g_nbtn++] = Button{ b, act }; }

// ── Visualizer ────────────────────────────────────────────────────────────────
struct VizChan { float level, peak, hue; bool on; };
VizChan viz[NCH];

int chan_a0(int ch) { return ch < 9 ? 0xA0 + ch : 0x1A0 + (ch - 9); }
int chan_b0(int ch) { return ch < 9 ? 0xB0 + ch : 0x1B0 + (ch - 9); }

void viz_update()
{
    for (int ch = 0; ch < NCH; ++ch) {
        float amp = powf((float)opl_channel_level(ch), 0.55f);   // perceptual

        int b0   = g_opl_regs[chan_b0(ch)];
        int fnum = g_opl_regs[chan_a0(ch)] | ((b0 & 3) << 8);
        int block = (b0 >> 2) & 7;
        float pitch = (block * 1024 + fnum) / (8.0f * 1024.0f);

        VizChan *v = &viz[ch];
        v->on = amp > 0.02f;
        v->level += (amp - v->level) * (amp > v->level ? 0.6f : 0.20f);
        if (v->level < 0) v->level = 0;
        if (v->level > v->peak) v->peak = v->level;
        else v->peak -= 0.012f;
        if (v->peak < v->level) v->peak = v->level;
        if (v->on) v->hue = pitch;
    }
}

enum { STYLE_LED, STYLE_NEON, STYLE_SPECTRUM, STYLE_COUNT };
int style = STYLE_LED;
const char *STYLE_SHORT[] = { "LED", "NEON", "SPEC" };

void draw_bars(int ax, int ay, int aw, int ah)
{
    int gap = aw / (NCH * 5); if (gap < 1) gap = 1;
    int slot = (aw - gap) / NCH, bw = slot - gap;

    int mx, my; SDL_GetMouseState(&mx, &my);
    for (int ch = 0; ch < NCH; ++ch) {
        int x = ax + gap + ch * slot;
        bool muted = g_eng->channel_muted(ch);
        SDL_Rect hit = { x - gap / 2, ay, slot, ah + 22 };   // bar + its labels
        bool hover = mx >= hit.x && mx < hit.x + hit.w && my >= hit.y && my < hit.y + hit.h;
        if (hover) fill(hit.x, hit.y, hit.w, hit.h, RGB{ 30, 34, 56 });
        reg_button(hit, ACT_CHAN_BASE + ch);

        if (muted) {
            // Silenced: its key-on is masked at the chip, so draw a greyed-out
            // slot (the unlit meter, desaturated) and a red number + "M" tag.
            RGB grey = { 70, 72, 84 };
            if (style == STYLE_LED) {
                int segs = 18, seg_h = ah / segs;
                for (int s = 0; s < segs; ++s)
                    fill(x, ay + ah - (s + 1) * seg_h + 1, bw, seg_h - 1, grey, 40);
            } else {
                fill(x, ay, bw, ah, grey, 40);
            }
            char lbl[4]; snprintf(lbl, sizeof lbl, "%d", ch + 1);
            RGB red = { 220, 70, 80 };
            draw_text(x + bw / 2 - text_w(lbl, 1) / 2, ay + ah + 4, 1, red, lbl);
            draw_text(x + bw / 2 - text_w("M", 1) / 2, ay + ah + 13, 1, red, "M");
            continue;
        }

        float lv = viz[ch].level; if (lv > 1) lv = 1;
        float pk = viz[ch].peak;  if (pk > 1) pk = 1;
        int bh = (int)(lv * ah), by = ay + ah - bh;

        if (style == STYLE_LED) {
            int segs = 18, seg_h = ah / segs;
            int lit = (int)(lv * segs + 0.5f), pkseg = (int)(pk * segs + 0.5f);
            for (int s = 0; s < segs; ++s) {
                int sy = ay + ah - (s + 1) * seg_h + 1;
                RGB col = (s < segs * 0.6f) ? RGB{ 40, 220, 70 }
                        : (s < segs * 0.85f) ? RGB{ 240, 200, 40 }
                                             : RGB{ 240, 60, 40 };
                if (s + 1 == pkseg)   fill(x, sy, bw, seg_h - 1, RGB{ 255, 255, 255 });
                else if (s < lit)     fill(x, sy, bw, seg_h - 1, col);
                else                  fill(x, sy, bw, seg_h - 1, col, 26);
            }
        } else if (style == STYLE_NEON) {
            SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_ADD);
            for (int g = 3; g >= 1; --g)
                fill(x - g * 3, by - g * 3, bw + g * 6, bh + g * 3, RGB{ 60, 120, 255 }, (Uint8)(16 * lv));
            int slices = bh / 3 + 1;
            for (int s = 0; s < slices; ++s) {
                float t = (float)s / (slices > 1 ? slices - 1 : 1);
                RGB c = { (Uint8)(40 + t * 215), (Uint8)(220 - t * 140), 255 };
                fill(x, by + bh - (s + 1) * 3, bw, 3, c);
            }
            SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
            fill(x, ay + ah - (int)(pk * ah) - 2, bw, 2, RGB{ 255, 255, 255 }, 220);
        } else {
            int tw = bw * 7 / 10, tx = x + (bw - tw) / 2;
            RGB c = hsv(0.62f - viz[ch].hue * 0.62f, 0.85f, 1.0f);
            fill(tx, by, tw, bh, c);
            fill(tx, ay + ah - (int)(pk * ah) - 2, tw, 2, RGB{ 255, 255, 255 }, 230);
        }

        char lbl[4]; snprintf(lbl, sizeof lbl, "%d", ch + 1);
        draw_text(x + bw / 2 - text_w(lbl, 1) / 2, ay + ah + 4, 1,
                  viz[ch].on ? RGB{ 220, 220, 220 } : RGB{ 80, 80, 100 }, lbl);
    }
}


enum { ICON_PREV, ICON_PLAY, ICON_PAUSE, ICON_NEXT, ICON_STOP };

void draw_icon(int type, int x, int y, int sz, RGB c)
{
    int g = sz * 6 / 10, gx = x + (sz - g) / 2, gy = y + (sz - g) / 2;
    int bar = g / 4; if (bar < 2) bar = 2;
    switch (type) {
    case ICON_PLAY:  fill_tri(gx + g / 8, gy, g - g / 8, g, +1, c); break;
    case ICON_PAUSE: fill(gx + g / 8, gy, bar, g, c);
                     fill(gx + g - g / 8 - bar, gy, bar, g, c); break;
    case ICON_PREV:  fill(gx, gy, bar, g, c);
                     fill_tri(gx + bar + 1, gy, g - bar - 1, g, -1, c); break;
    case ICON_NEXT:  fill_tri(gx, gy, g - bar - 1, g, +1, c);
                     fill(gx + g - bar, gy, bar, g, c); break;
    case ICON_STOP:  fill(gx, gy, g, g, c); break;
    }
}

int add_button(int x, int y, int h, const char *label, int act, int mx, int my,
               bool active = false)
{
    int s = 2, pad = 6, w = text_w(label, s) + pad * 2;
    bool hover = mx >= x && mx < x + w && my >= y && my < y + h;
    RGB bg = active ? RGB{ 46, 96, 140 } : (hover ? RGB{ 58, 64, 96 } : RGB{ 34, 36, 54 });
    fill(x, y, w, h, bg);
    SDL_SetRenderDrawColor(ren, 95, 105, 150, 255);
    SDL_Rect b = { x, y, w, h }; SDL_RenderDrawRect(ren, &b);
    draw_text(x + pad, y + (h - FONT_H * s) / 2, s, RGB{ 225, 230, 250 }, label);
    reg_button(b, act);
    return x + w + 6;
}

int add_icon_button(int x, int y, int h, int type, int act, int mx, int my)
{
    int w = h + 6;
    bool hover = mx >= x && mx < x + w && my >= y && my < y + h;
    fill(x, y, w, h, hover ? RGB{ 58, 64, 96 } : RGB{ 34, 36, 54 });
    SDL_SetRenderDrawColor(ren, 95, 105, 150, 255);
    SDL_Rect b = { x, y, w, h }; SDL_RenderDrawRect(ren, &b);
    draw_icon(type, x + (w - h) / 2, y, h, RGB{ 225, 230, 250 });
    reg_button(b, act);
    return x + w + 6;
}

// ── App state the controls act on ─────────────────────────────────────────────
// Mutually-exclusive output: FM to the OPL3 board, FM to PC speakers, or the
// raw MIDI stream to an external device. (No simultaneous output by design.)
enum { OUT_PC, OUT_BOARD, OUT_EXT };
int  g_outmode = OUT_PC;
bool g_recurse = false;
bool g_quit    = false;

// LOOP cycles OFF -> ONE (repeat the track, done by libADLMIDI) -> ALL (the
// playlist wraps around at the end).
enum { LOOP_OFF, LOOP_ONE, LOOP_ALL };
int  g_loopmode = LOOP_OFF;
const char *LOOP_NAME[] = { "OFF", "ONE", "ALL" };

int  g_click_x = 0;          // x of the click being handled (for the seek bar)
SDL_Rect g_seek = { 0, 0, 0, 0 };   // seek bar rect (set each frame)

// Open Folder runs the (blocking) zenity/kdialog on a worker thread so the
// window keeps repainting; the main loop picks up the result.
std::thread       g_pick_thread;
std::atomic<bool> g_picking{false};
std::atomic<bool> g_pick_done{false};
std::mutex        g_pick_mx;
std::string       g_pick_result;   // "" = cancelled / no dialog available

int  g_bank = 0;      // current bank ordinal
std::vector<MidiPort> g_midi_ports;   // external MIDI devices (for the picker)
int  g_midi_idx = 0;  // selected device in g_midi_ports
int  g_scroll = 0;    // top row of the playlist view
int  g_visible_rows = 1;        // rows the playlist shows (set each frame)
int  g_last_scrolled_cur = -2;  // last current-track we auto-revealed

// Scroll the playlist by `delta` rows, clamped. Used by wheel + keyboard.
void scroll_playlist(int delta)
{
    int total = (int)g_pl->size();
    g_scroll += delta;
    if (g_scroll > total - g_visible_rows) g_scroll = total - g_visible_rows;
    if (g_scroll < 0) g_scroll = 0;
}

const char *bank_name(int i)
{
    int n = adl_getBanksCount();
    if (i < 0 || i >= n) return "?";
    return adl_getBankNames()[i];
}

void apply_output()
{
    // Exactly one sink on; the engine silences the others as they switch off.
    g_eng->set_pc_out(g_outmode == OUT_PC);
    g_eng->set_board_out(g_outmode == OUT_BOARD);
    g_eng->set_gm_out(g_outmode == OUT_EXT);
}

// Play the current track, skipping forward past unplayable files.
void play_current()
{
    play_from_playlist(*g_eng, *g_pl, true, false);
}

// Right-click on a bar: solo that channel, or if it is already the only one
// playing, bring every channel back.
void solo_channel(int ch)
{
    uint32_t solo = ~(1u << ch) & 0x3FFFF;
    g_eng->set_mute_mask(g_eng->mute_mask() == solo ? 0 : solo);
}

// External-MIDI device picker (shares the BANK control slot in EXT mode).
void refresh_midi_ports() { g_midi_ports = AlsaMidi::enumerate(); }

void sync_midi_idx()   // point g_midi_idx at whatever device is connected
{
    std::string a = g_eng->gm_addr();
    for (size_t i = 0; i < g_midi_ports.size(); ++i)
        if (g_midi_ports[i].addr == a) { g_midi_idx = (int)i; return; }
}

void cycle_midi(int dir)
{
    if (g_midi_ports.empty()) refresh_midi_ports();
    if (g_midi_ports.empty()) return;
    int n = (int)g_midi_ports.size();
    g_midi_idx = (g_midi_idx + dir + n) % n;
    g_eng->connect_midi(g_midi_ports[g_midi_idx].addr);
}

// Ask the desktop for a folder (zenity/kdialog). Returns false if unavailable
// or cancelled. Blocks until the dialog closes — run it via start_pick_folder.
bool pick_folder(std::string &out)
{
    const char *cmds[] = {
        "zenity --file-selection --directory 2>/dev/null",
        "kdialog --getexistingdirectory . 2>/dev/null",
        nullptr
    };
    for (int i = 0; cmds[i]; ++i) {
        FILE *f = popen(cmds[i], "r");
        if (!f) continue;
        char buf[4096];
        bool got = fgets(buf, sizeof buf, f) != nullptr;
        pclose(f);
        if (got) {
            std::string s(buf);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            if (!s.empty()) { out = s; return true; }
        }
    }
    return false;
}

void start_pick_folder()
{
    if (g_picking) return;   // a dialog is already open
    if (g_pick_thread.joinable()) g_pick_thread.join();
    g_picking = true;
    g_pick_thread = std::thread([] {
        std::string folder;
        bool ok = pick_folder(folder);
        {
            std::lock_guard<std::mutex> lk(g_pick_mx);
            g_pick_result = ok ? folder : std::string();
        }
        g_pick_done = true;
    });
}

// Main-loop side of Open Folder: add the chosen folder once the dialog closes.
void poll_pick_folder()
{
    if (!g_pick_done.exchange(false)) return;
    g_picking = false;
    std::string folder;
    {
        std::lock_guard<std::mutex> lk(g_pick_mx);
        folder = g_pick_result;
    }
    if (folder.empty()) {
        fprintf(stderr, "Open Folder: cancelled, or no zenity/kdialog. Folders can also be passed on the command line.\n");
        return;
    }
    bool was_empty = g_pl->empty();
    g_pl->add_folder(folder, g_recurse);
    if (was_empty && !g_pl->empty()) play_current();
}

// ── Board serial-port dialog ──────────────────────────────────────────────────
// PORT opens a modal list of the USB-serial devices present (ttyACM*/ttyUSB*,
// with their USB product names) plus a text field for any other path. Picking
// one reopens the board there and switches the output to it; the choice is
// saved in the config.
struct SerialPort { std::string name, desc; };   // name: "ttyUSB1"
std::vector<SerialPort> g_ports;
bool        g_port_dlg = false;
std::string g_port_text;          // the custom-path field
std::string g_port_msg;           // why the last attempt failed
Uint32      g_port_open_ts = 0;   // timestamp of the key press that opened it

// A device as the user thinks of it: without the "/dev/" prefix.
std::string short_dev(const std::string &d)
{
    return d.rfind("/dev/", 0) == 0 ? d.substr(5) : d;
}

void refresh_ports()
{
    g_ports.clear();
    std::error_code ec;
    for (std::filesystem::directory_iterator it("/dev", ec), end; !ec && it != end; it.increment(ec)) {
        std::string n = it->path().filename().string();
        if (n.rfind("ttyACM", 0) != 0 && n.rfind("ttyUSB", 0) != 0) continue;
        // The USB product string sits one (CDC-ACM) or two (usb-serial) levels
        // above the tty's device node. Unreadable (e.g. sandboxed) => no name.
        std::string desc;
        for (const char *rel : { "/device/../product", "/device/../../product" }) {
            std::ifstream f("/sys/class/tty/" + n + rel);
            if (f && std::getline(f, desc) && !desc.empty()) break;
            desc.clear();
        }
        g_ports.push_back(SerialPort{ n, desc });
    }
    // ttyACM* before ttyUSB*, each in numeric order (ttyUSB2 before ttyUSB10).
    std::sort(g_ports.begin(), g_ports.end(), [](const SerialPort &a, const SerialPort &b) {
        int c = a.name.compare(0, 6, b.name, 0, 6);
        return c ? c < 0 : atoi(a.name.c_str() + 6) < atoi(b.name.c_str() + 6);
    });
    if (g_ports.size() > 32) g_ports.resize(32);   // ACT_PORTSEL_BASE range
}

void close_port_dialog()
{
    g_port_dlg = false;
    SDL_StopTextInput();
}

void open_port_dialog()
{
    refresh_ports();
    g_port_msg.clear();
    // Start the custom field on the current device when it's one the list
    // can't offer (a symlink, another driver's name, ...).
    g_port_text = g_eng->board_dev();
    for (const auto &p : g_ports)
        if (p.name == short_dev(g_port_text)) g_port_text.clear();
    if (g_port_text == "-") g_port_text.clear();
    g_port_dlg = true;
    SDL_StartTextInput();
}

// Reopen the board on `dev` ("" = no board). Success switches the output to it
// and closes the dialog; failure keeps the dialog up with the reason, and goes
// back to the board that was working (a typo shouldn't cost you the board).
void select_port(const std::string &dev)
{
    std::string prev = g_eng->board_dev();
    bool had_board = g_eng->board_available();

    if (g_eng->open_board(dev)) {
        g_outmode = OUT_BOARD;
        apply_output();
        close_port_dialog();
        return;
    }
    if (dev.empty()) {
        close_port_dialog();
    } else {
        g_port_msg = retrowave_last_error();
        if (had_board) g_eng->open_board(prev);
    }
    if (g_outmode == OUT_BOARD && !g_eng->board_available()) g_outmode = OUT_PC;
    apply_output();
}

void submit_custom_port()
{
    std::string d = g_port_text;
    size_t a = d.find_first_not_of(' '), b = d.find_last_not_of(' ');
    d = (a == std::string::npos) ? "" : d.substr(a, b - a + 1);
    if (d.empty()) { g_port_msg = "type a device first, e.g. /dev/ttyUSB1"; return; }
    select_port(d == "none" || d == "off" ? std::string() : d);
}

// Keyboard while the dialog is up: everything typed goes to the custom field.
void port_dialog_key(const SDL_Event &e)
{
    if (e.type == SDL_TEXTINPUT) {
        if (e.text.timestamp == g_port_open_ts) return;   // the key that opened us
        for (const char *c = e.text.text; *c; ++c)
            if (*c > ' ' && *c < 127 && g_port_text.size() < 200) g_port_text += *c;
        return;
    }
    switch (e.key.keysym.sym) {
    case SDLK_ESCAPE: close_port_dialog(); break;
    case SDLK_RETURN: case SDLK_KP_ENTER: submit_custom_port(); break;
    case SDLK_BACKSPACE: if (!g_port_text.empty()) g_port_text.pop_back(); break;
    case SDLK_u: if (e.key.keysym.mod & KMOD_CTRL) g_port_text.clear(); break;
    case SDLK_v:
        if (e.key.keysym.mod & KMOD_CTRL) {
            char *clip = SDL_GetClipboardText();
            for (const char *c = clip; c && *c; ++c)
                if (*c > ' ' && *c < 127 && g_port_text.size() < 200) g_port_text += *c;
            SDL_free(clip);
        }
        break;
    }
}

void do_action(int a)
{
    if (a >= ACT_ROW_BASE) {
        g_pl->select_display_index(a - ACT_ROW_BASE);
        play_current();
        return;
    }
    if (a >= ACT_PORTSEL_BASE) {
        size_t i = (size_t)(a - ACT_PORTSEL_BASE);
        if (i < g_ports.size()) select_port(g_ports[i].name);
        return;
    }
    if (a >= ACT_CHAN_BASE) {
        int ch = a - ACT_CHAN_BASE;
        g_eng->set_channel_muted(ch, !g_eng->channel_muted(ch));
        return;
    }
    switch (a) {
    case ACT_PREV:  if (g_pl->prev(true)) play_current(); break;
    case ACT_NEXT:  if (g_pl->next(true)) play_current(); break;
    case ACT_PLAY:
        // After STOP there is no song loaded: Play restarts the current track.
        if (!g_eng->has_song()) play_current();
        else g_eng->set_paused(!g_eng->paused());
        break;
    case ACT_UNMUTE_ALL: g_eng->set_mute_mask(0); break;
    case ACT_STOP:  g_eng->stop(); break;
    case ACT_LOOP:
        g_loopmode = (g_loopmode + 1) % 3;
        g_eng->set_loop(g_loopmode == LOOP_ONE);
        break;
    case ACT_SEEK:
        if (g_eng->has_song() && g_seek.w > 0) {
            double f = (double)(g_click_x - g_seek.x) / g_seek.w;
            g_eng->seek((f < 0 ? 0 : f > 1 ? 1 : f) * g_eng->length());
        }
        break;
    case ACT_SHUFFLE: g_pl->set_shuffle(!g_pl->shuffle()); break;
    case ACT_STYLE: style = (style + 1) % STYLE_COUNT; break;
    case ACT_TUP:   g_eng->set_tempo(g_eng->tempo() < 4.0 ? g_eng->tempo() * 1.25 : 4.0); break;
    case ACT_TDN:   g_eng->set_tempo(g_eng->tempo() > 0.25 ? g_eng->tempo() / 1.25 : 0.25); break;
    case ACT_OUTPUT: {
        // Cycle PC -> Board -> External, skipping any that isn't available.
        // Landing on External connects the device on demand (no -m needed).
        for (int step = 0; step < 3; ++step) {
            g_outmode = (g_outmode + 1) % 3;
            if (g_outmode == OUT_BOARD && !g_eng->board_available()) continue;
            if (g_outmode == OUT_EXT && !g_eng->gm_available()) {
                if (!g_eng->gm_connectable() || !g_eng->ensure_midi()) continue;
            }
            break;   // this mode is usable
        }
        if (g_outmode == OUT_EXT) { refresh_midi_ports(); sync_midi_idx(); }
        apply_output();
        break;
    }
    case ACT_BANK_PREV: if (g_bank > 0)                       { g_bank--; g_eng->set_bank(g_bank); } break;
    case ACT_BANK_NEXT: if (g_bank < adl_getBanksCount() - 1) { g_bank++; g_eng->set_bank(g_bank); } break;
    case ACT_MIDI_PREV: cycle_midi(-1); break;
    case ACT_MIDI_NEXT: cycle_midi(+1); break;
    case ACT_RECURSE:   g_recurse = !g_recurse; break;
    case ACT_OPEN: start_pick_folder(); break;
    case ACT_PORT: if (g_port_dlg) close_port_dialog(); else open_port_dialog(); break;
    case ACT_PORT_CLOSE:  close_port_dialog(); break;
    case ACT_PORT_RESCAN: refresh_ports(); g_port_msg.clear(); break;
    case ACT_PORT_NONE:   select_port(std::string()); break;
    case ACT_PORT_CUSTOM: submit_custom_port(); break;
    }
}

// ── Playlist panel ────────────────────────────────────────────────────────────
int g_row_h = 16;    // pixel height of one playlist row (font size 2 -> ~14)

void draw_playlist(int px, int py, int pw, int ph, int mx, int my)
{
    fill(px, py, pw, ph, RGB{ 8, 10, 18 });
    SDL_SetRenderDrawColor(ren, 60, 66, 96, 255);
    SDL_Rect frame = { px, py, pw, ph }; SDL_RenderDrawRect(ren, &frame);

    // Title strip.
    fill(px, py, pw, 20, RGB{ 26, 28, 46 });
    char hdr[64];
    snprintf(hdr, sizeof hdr, "PLAYLIST  %d", (int)g_pl->size());
    draw_text(px + 6, py + 4, 2, RGB{ 200, 210, 245 }, hdr);

    int list_y = py + 22, list_h = ph - 24;
    int rows = list_h / g_row_h;
    int total = (int)g_pl->size();
    int cur = g_pl->current_display_index();
    g_visible_rows = rows;   // let the keyboard/wheel handlers page by this

    // Auto-reveal the current track only when it *changes* (a new song started);
    // otherwise leave g_scroll where the user's wheel/keys put it, so the list
    // can be browsed freely while a track plays.
    if (cur != g_last_scrolled_cur) {
        if (cur >= 0) {
            if (cur < g_scroll)              g_scroll = cur;
            else if (cur >= g_scroll + rows) g_scroll = cur - rows + 1;
        }
        g_last_scrolled_cur = cur;
    }
    if (g_scroll > total - rows) g_scroll = total - rows;
    if (g_scroll < 0) g_scroll = 0;

    const auto &tracks = g_pl->tracks();
    for (int i = 0; i < rows; ++i) {
        int idx = g_scroll + i;
        if (idx >= total) break;
        int ry = list_y + i * g_row_h;
        SDL_Rect row = { px + 1, ry, pw - 2, g_row_h };
        bool hover = mx >= row.x && mx < row.x + row.w && my >= row.y && my < row.y + row.h;
        bool current = (idx == cur);

        if (current)   fill(row.x, row.y, row.w, row.h, RGB{ 40, 70, 120 });
        else if (hover) fill(row.x, row.y, row.w, row.h, RGB{ 24, 28, 44 });

        char num[16]; snprintf(num, sizeof num, "%d", idx + 1);
        RGB nc = current ? RGB{ 210, 225, 255 } : RGB{ 90, 100, 140 };
        draw_text(px + 6, ry + 1, 1, nc, num);

        std::string nm = ellipsize(tracks[idx].name, pw - 44, 2);
        RGB tc = current ? RGB{ 255, 255, 255 } : RGB{ 175, 185, 215 };
        draw_text(px + 34, ry + 1, 2, tc, nm.c_str());

        reg_button(row, ACT_ROW_BASE + idx);
    }

    // Slim scrollbar.
    if (total > rows) {
        int track_x = px + pw - 4;
        fill(track_x, list_y, 3, list_h, RGB{ 20, 22, 34 });
        int knob_h = list_h * rows / total; if (knob_h < 8) knob_h = 8;
        int knob_y = list_y + (list_h - knob_h) * g_scroll / (total - rows);
        fill(track_x, knob_y, 3, knob_h, RGB{ 80, 90, 130 });
    }
}

// ── Controls bar (two rows) ───────────────────────────────────────────────────
void draw_controls(int W, int Hh)
{
    int mx, my; SDL_GetMouseState(&mx, &my);
    char b[64];

    // Row 2 (transport) at the very bottom.
    int r2 = Hh - 34, bh = 26, bx = 12;
    bx = add_icon_button(bx, r2, bh, ICON_PREV, ACT_PREV, mx, my);
    bool playing = g_eng->has_song() && !g_eng->paused();
    bx = add_icon_button(bx, r2, bh, playing ? ICON_PAUSE : ICON_PLAY, ACT_PLAY, mx, my);
    bx = add_icon_button(bx, r2, bh, ICON_NEXT, ACT_NEXT, mx, my);
    bx = add_icon_button(bx, r2, bh, ICON_STOP, ACT_STOP, mx, my);
    bx += 6;
    snprintf(b, sizeof b, "LOOP:%s", LOOP_NAME[g_loopmode]);
    bx = add_button(bx, r2, bh, b, ACT_LOOP, mx, my, g_loopmode != LOOP_OFF);
    snprintf(b, sizeof b, "SHUF:%s", g_pl->shuffle() ? "ON" : "OFF");
    bx = add_button(bx, r2, bh, b, ACT_SHUFFLE, mx, my, g_pl->shuffle());
    bx = add_button(bx, r2, bh, STYLE_SHORT[style], ACT_STYLE, mx, my);
    bx = add_button(bx, r2, bh, "T-", ACT_TDN, mx, my);
    snprintf(b, sizeof b, "%.2fx", g_eng->tempo());
    draw_text(bx + 2, r2 + (bh - FONT_H * 2) / 2, 2, RGB{ 150, 170, 210 }, b);
    bx += text_w(b, 2) + 8;
    bx = add_button(bx, r2, bh, "T+", ACT_TUP, mx, my);

    // Board serial port: shows the device when there's room for it.
    bx += 6;
    std::string dev = short_dev(g_eng->board_dev());
    if (size_t sl = dev.rfind('/'); sl != std::string::npos) dev = dev.substr(sl + 1);
    std::string pl = "PORT:" + (dev.empty() ? std::string("NONE") : dev == "-" ? std::string("DRY-RUN") : dev);
    if (bx + text_w(pl.c_str(), 2) + 12 > W - 12) pl = "PORT";
    add_button(bx, r2, bh, pl.c_str(), ACT_PORT, mx, my, g_eng->board_available());

    // Row 1 (routing / bank / library) above it.
    int r1 = Hh - 66; bx = 12;
    const char *om = g_outmode == OUT_PC ? "PC" : g_outmode == OUT_BOARD ? "BOARD" : "EXT-MIDI";
    snprintf(b, sizeof b, "OUT:%s", om);
    bx = add_button(bx, r1, bh, b, ACT_OUTPUT, mx, my);
    bx += 4;

    // In External mode the FM bank is irrelevant, so this control becomes the
    // ALSA device picker; otherwise it's the FM bank picker.
    bool ext = (g_outmode == OUT_EXT);
    bx = add_button(bx, r1, bh, "<", ext ? ACT_MIDI_PREV : ACT_BANK_PREV, mx, my);
    std::string label;
    if (ext) label = "MIDI: " + (g_eng->gm_available() ? g_eng->gm_name() : std::string("(none)"));
    else     label = std::string("BANK: ") + bank_name(g_bank);
    draw_text(bx + 2, r1 + (bh - FONT_H * 2) / 2, 2, RGB{ 200, 210, 245 },
              ellipsize(label, 280, 2).c_str());
    bx += 284;
    bx = add_button(bx, r1, bh, ">", ext ? ACT_MIDI_NEXT : ACT_BANK_NEXT, mx, my);

    bx += 10;
    snprintf(b, sizeof b, "RECURSE:%s", g_recurse ? "ON" : "OFF");
    bx = add_button(bx, r1, bh, b, ACT_RECURSE, mx, my, g_recurse);
    bx = add_button(bx, r1, bh, g_picking ? "OPENING.." : "OPEN FOLDER", ACT_OPEN, mx, my, g_picking);
    if (uint32_t m = g_eng->mute_mask()) {        // only shown while something is muted
        snprintf(b, sizeof b, "UNMUTE %d", __builtin_popcount(m));
        add_button(bx, r1, bh, b, ACT_UNMUTE_ALL, mx, my, true);
    }
}

// Full-width list row of the port dialog: device name, then a dimmer note.
void port_row(int x, int y, int w, int h, const std::string &name,
              const std::string &note, int act, bool current, int mx, int my)
{
    bool hover = mx >= x && mx < x + w && my >= y && my < y + h;
    fill(x, y, w, h, current ? RGB{ 46, 96, 140 } : hover ? RGB{ 58, 64, 96 } : RGB{ 34, 36, 54 });
    SDL_SetRenderDrawColor(ren, 95, 105, 150, 255);
    SDL_Rect b = { x, y, w, h }; SDL_RenderDrawRect(ren, &b);
    int ty = y + (h - FONT_H * 2) / 2;
    draw_text(x + 8, ty, 2, RGB{ 235, 240, 255 }, name.c_str());
    int nx = x + 8 + text_w(name.c_str(), 2) + 16;
    if (!note.empty() && x + w - 8 - nx > 40)
        draw_text(nx, ty, 2, current ? RGB{ 190, 215, 245 } : RGB{ 130, 140, 180 },
                  ellipsize(note, x + w - 8 - nx, 2).c_str());
    reg_button(b, act);
}

// Modal: drawn last, over a dimmed window, and it replaces the frame's button
// table so nothing underneath can be clicked.
void draw_port_dialog(int W, int Hh)
{
    int mx, my; SDL_GetMouseState(&mx, &my);
    fill(0, 0, W, Hh, RGB{ 0, 0, 0 }, 185);
    g_nbtn = 0;

    const int rh = 26, gap = 4, pad = 14;
    const int fixed = pad + 24 + (rh + gap) + 12 + 14 + rh + 16 + 22 + rh + pad;
    int fit = (Hh - 24 - fixed) / (rh + gap); if (fit < 1) fit = 1;
    int shown = (int)g_ports.size() < fit ? (int)g_ports.size() : fit;
    int extra = (int)g_ports.size() - shown;
    int list_rows = shown > 0 ? shown : 1;           // "none found" takes a row

    int pw = W - 40 < 620 ? W - 40 : 620, ph = fixed + list_rows * (rh + gap);
    int px = (W - pw) / 2, py = (Hh - ph) / 2;
    fill(px, py, pw, ph, RGB{ 16, 18, 30 });
    SDL_SetRenderDrawColor(ren, 110, 125, 180, 255);
    SDL_Rect frame = { px, py, pw, ph }; SDL_RenderDrawRect(ren, &frame);

    int x = px + pad, w = pw - 2 * pad, y = py + pad;
    draw_text(x, y, 2, RGB{ 240, 240, 255 }, "OPL3 BOARD SERIAL PORT");
    y += 24;

    // Detected devices; the one the board is on is highlighted.
    std::string cur = short_dev(g_eng->board_dev());
    bool alive = g_eng->board_available();
    for (int i = 0; i < shown; ++i) {
        bool is_cur = (g_ports[i].name == cur);
        std::string note = g_ports[i].desc;
        if (is_cur) note = (alive ? "(IN USE) " : "(NOT RESPONDING) ") + note;
        port_row(x, y, w, rh, g_ports[i].name, note, ACT_PORTSEL_BASE + i,
                 is_cur && alive, mx, my);
        y += rh + gap;
    }
    if (shown == 0) {
        draw_text(x + 8, y + (rh - FONT_H * 2) / 2, 2, RGB{ 130, 140, 180 },
                  ellipsize("NO USB-SERIAL DEVICES FOUND - PLUG IN, THEN RESCAN", w - 16, 2).c_str());
        y += rh + gap;
    }
    port_row(x, y, w, rh, "NONE", extra > 0 ? "PC AUDIO ONLY  (MORE PORTS EXIST - TYPE BELOW)"
                                            : "PC AUDIO ONLY", ACT_PORT_NONE, cur.empty(), mx, my);
    y += rh + gap + 12;

    // Custom path. The font has no lowercase, and device names are case-
    // sensitive, so capitals are marked with a bar above them.
    draw_text(x, y, 1, RGB{ 150, 170, 210 },
              "OTHER DEVICE - TYPE ITS NAME OR FULL PATH, THEN ENTER:");
    y += 14;
    int ow = text_w("OPEN", 2) + 12, fw = w - ow - 6;
    bool custom_cur = !cur.empty() && cur == short_dev(g_port_text) && alive;
    fill(x, y, fw, rh, RGB{ 6, 7, 12 });
    SDL_SetRenderDrawColor(ren, custom_cur ? 90 : 95, custom_cur ? 170 : 105, custom_cur ? 230 : 150, 255);
    SDL_Rect box = { x, y, fw, rh }; SDL_RenderDrawRect(ren, &box);
    int cw = (FONT_W + 1) * 2, maxc = (fw - 16 - 44) / cw - 1;   // room for cursor + tag
    size_t from = (int)g_port_text.size() > maxc ? g_port_text.size() - maxc : 0;
    int cx = x + 8, ty = y + (rh - FONT_H * 2) / 2 + 1;
    for (size_t i = from; i < g_port_text.size(); ++i, cx += cw) {
        char one[2] = { g_port_text[i], 0 };
        draw_text(cx, ty, 2, RGB{ 235, 240, 255 }, one);
        if (one[0] >= 'A' && one[0] <= 'Z') fill(cx, ty - 4, cw - 2, 2, RGB{ 240, 200, 40 });
    }
    if (SDL_GetTicks() / 500 % 2 == 0) fill(cx, ty, 2, FONT_H * 2, RGB{ 235, 240, 255 });
    if (custom_cur)
        draw_text(x + fw - 8 - text_w("IN USE", 1), y + (rh - FONT_H) / 2, 1,
                  RGB{ 120, 190, 240 }, "IN USE");
    add_button(x + fw + 6, y, rh, "OPEN", ACT_PORT_CUSTOM, mx, my);
    y += rh + 4;
    draw_text(x, y, 1, RGB{ 110, 120, 160 }, "CAPITAL LETTERS ARE SHOWN WITH A BAR ON TOP");
    y += 12;

    // Result of the last attempt.
    if (!g_port_msg.empty())
        draw_text(x, y + 2, 1, RGB{ 240, 110, 110 }, ellipsize(g_port_msg, w, 1).c_str());
    y += 22;

    int bx = add_button(x, y, rh, "RESCAN", ACT_PORT_RESCAN, mx, my);
    add_button(bx, y, rh, "CLOSE", ACT_PORT_CLOSE, mx, my);

    reg_button(frame, ACT_NOP);                             // clicks on the panel
    reg_button(SDL_Rect{ 0, 0, W, Hh }, ACT_PORT_CLOSE);    // clicks outside it
}

int hit_button(int x, int y)
{
    for (int i = 0; i < g_nbtn; ++i)
        if (x >= g_btn[i].r.x && x < g_btn[i].r.x + g_btn[i].r.w &&
            y >= g_btn[i].r.y && y < g_btn[i].r.y + g_btn[i].r.h) return g_btn[i].act;
    return -1;
}

void handle_click(int x, int y, bool right)
{
    int a = hit_button(x, y);
    if (a < 0) return;
    g_click_x = x;
    if (right) {   // right-click only means something on a channel bar (solo)
        if (a >= ACT_CHAN_BASE && a < ACT_PORTSEL_BASE) solo_channel(a - ACT_CHAN_BASE);
        return;
    }
    do_action(a);
}

// ── Header: title, time, click-to-seek progress bar ──────────────────────────
void draw_header(int W)
{
    fill(0, 0, W, 40, RGB{ 22, 22, 38 });

    const Track *cur = g_pl->current();
    double pos = g_eng->position(), len = g_eng->length();
    char tm[32];
    snprintf(tm, sizeof tm, "%d:%02d/%d:%02d",
             (int)pos / 60, (int)pos % 60, (int)len / 60, (int)len % 60);
    int tw = text_w(tm, 2);
    draw_text(W - 12 - tw, 6, 2, RGB{ 150, 170, 210 }, tm);

    std::string title = cur ? cur->name : std::string("(nothing loaded)");
    if (!g_eng->has_song()) { if (cur) title += "  -STOPPED-"; }
    else if (g_eng->paused()) title += "  -PAUSED-";
    draw_text(12, 6, 2, RGB{ 240, 240, 255 }, ellipsize(title, W - 36 - tw, 2).c_str());

    // Progress bar; the clickable strip is taller than the drawn one.
    g_seek = SDL_Rect{ 12, 24, W - 24, 14 };
    int mx, my; SDL_GetMouseState(&mx, &my);
    bool hover = g_eng->has_song() && mx >= g_seek.x && mx < g_seek.x + g_seek.w &&
                 my >= g_seek.y && my < g_seek.y + g_seek.h;
    int by = 28, bh = 6;
    fill(g_seek.x, by, g_seek.w, bh, RGB{ 34, 36, 54 });
    if (len > 0) {
        int done = (int)(g_seek.w * (pos / len)); if (done > g_seek.w) done = g_seek.w;
        fill(g_seek.x, by, done, bh, hover ? RGB{ 110, 160, 230 } : RGB{ 70, 120, 200 });
    }
    if (hover) fill(mx - 1, by - 2, 2, bh + 4, RGB{ 230, 235, 255 });
    reg_button(g_seek, ACT_SEEK);
}

// Test hooks (parallel RWSHOT): at frame 20, inject a wheel scroll
// (RWSCROLLTEST=N), N output-cycle presses (RWOUTTEST=N), a channel-mute mask
// (RWMUTE=hex), a seek-bar click (RWSEEK=fraction), key presses (RWKEYS=chars)
// and/or the port dialog (RWPORT=dev opens it and submits dev as the custom
// path; empty just opens it), so headless screenshots can verify scrolling /
// output switching / muting / seeking / keyboard actions / port selection.
void run_test_hooks(long frame)
{
    if (frame != 20) return;
    if (const char *st = getenv("RWSCROLLTEST")) {
        SDL_Event we; SDL_zero(we);
        we.type = SDL_MOUSEWHEEL;
        we.wheel.y = -atoi(st);
        we.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
        SDL_PushEvent(&we);
    }
    if (const char *sk = getenv("RWSEEK")) {     // click the seek bar at a fraction
        SDL_Event ce; SDL_zero(ce);
        ce.type = SDL_MOUSEBUTTONDOWN;
        ce.button.button = SDL_BUTTON_LEFT;
        ce.button.x = g_seek.x + (int)(g_seek.w * atof(sk));
        ce.button.y = g_seek.y + g_seek.h / 2;
        SDL_PushEvent(&ce);
    }
    if (const char *mt = getenv("RWMUTE"))
        g_eng->set_mute_mask((uint32_t)strtoul(mt, nullptr, 16));
    if (const char *ot = getenv("RWOUTTEST"))
        for (int k = atoi(ot); k > 0; --k) do_action(ACT_OUTPUT);
    if (const char *pt = getenv("RWPORT")) {
        open_port_dialog();
        if (*pt) { g_port_text = pt; submit_custom_port(); }
    }
    if (const char *keys = getenv("RWKEYS"))
        for (const char *k = keys; *k; ++k) {
            SDL_Event ke; SDL_zero(ke);
            ke.type = SDL_KEYDOWN;
            ke.key.keysym.sym = (SDL_Keycode)*k;
            SDL_PushEvent(&ke);
        }
}

// Headless self-capture for testing: RWSHOT=path [RWSHOT_FRAME=N] saves the
// frame as a BMP and quits.
void maybe_screenshot(long frame, int W, int Hh)
{
    const char *shot = getenv("RWSHOT");
    if (!shot) return;
    long target = getenv("RWSHOT_FRAME") ? atol(getenv("RWSHOT_FRAME")) : 120;
    if (frame < target) return;
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, W, Hh, 32, SDL_PIXELFORMAT_ARGB8888);
    if (s && SDL_RenderReadPixels(ren, nullptr, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch) == 0)
        SDL_SaveBMP(s, shot);
    if (s) SDL_FreeSurface(s);
    g_quit = true;
}

} // namespace

// ── Public entry point ────────────────────────────────────────────────────────
bool gui_run(Engine &engine, Playlist &playlist, Config &cfg)
{
    g_eng = &engine;
    g_pl  = &playlist;
    g_recurse = cfg.recurse;
    g_bank    = engine.bank();
    g_loopmode = (cfg.loop >= LOOP_OFF && cfg.loop <= LOOP_ALL) ? cfg.loop : LOOP_OFF;

    style = cfg.style;
    if (style < 0 || style >= STYLE_COUNT) style = STYLE_LED;

    // Restore the saved output mode if it's usable now, else fall back.
    int want = cfg.outmode;
    if (want < 0) want = engine.board_available() ? OUT_BOARD : OUT_PC;   // auto
    if (want == OUT_BOARD && !engine.board_available()) want = OUT_PC;
    if (want == OUT_EXT) {
        if (engine.gm_available() || (engine.gm_connectable() && engine.ensure_midi())) {
            refresh_midi_ports(); sync_midi_idx();
        } else {
            want = engine.board_available() ? OUT_BOARD : OUT_PC;
        }
    }
    g_outmode = want;
    apply_output();

    font_init();
    if (SDL_WasInit(SDL_INIT_VIDEO) == 0 && SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "gui: SDL video init failed: %s\n", SDL_GetError());
        return false;
    }
    g_win = SDL_CreateWindow("OPL3 RetroWave MIDI Player",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            cfg.win_w < 720 ? 720 : cfg.win_w, cfg.win_h < 420 ? 420 : cfg.win_h,
            SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_win || !ren) { fprintf(stderr, "gui: window/renderer failed\n"); return false; }
    SDL_SetWindowMinimumSize(g_win, 720, 420);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_StopTextInput();   // only wanted while the port dialog is open

    if (!playlist.empty() && !engine.paused())
        play_current();

    while (!g_quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) { g_quit = true; }
            else if (e.type == SDL_MOUSEBUTTONDOWN &&
                     (e.button.button == SDL_BUTTON_LEFT || e.button.button == SDL_BUTTON_RIGHT))
                handle_click(e.button.x, e.button.y, e.button.button == SDL_BUTTON_RIGHT);
            else if (g_port_dlg) {   // modal: keys go to the dialog, nothing else
                if (e.type == SDL_KEYDOWN || e.type == SDL_TEXTINPUT) port_dialog_key(e);
            }
            else if (e.type == SDL_MOUSEWHEEL) {
                int dir = (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) ? -1 : 1;
                scroll_playlist(-e.wheel.y * dir * 3);
            }
            else if (e.type == SDL_KEYDOWN) {
                int page = g_visible_rows > 1 ? g_visible_rows - 1 : 1;
                switch (e.key.keysym.sym) {
                case SDLK_q: case SDLK_ESCAPE: g_quit = true; break;
                case SDLK_SPACE: do_action(ACT_PLAY); break;
                case SDLK_n: do_action(ACT_NEXT); break;   // next / prev *track*
                case SDLK_p: do_action(ACT_PREV); break;
                case SDLK_s: do_action(ACT_STOP); break;
                case SDLK_l: do_action(ACT_LOOP); break;
                case SDLK_h: do_action(ACT_SHUFFLE); break;
                case SDLK_v: do_action(ACT_STYLE); break;
                case SDLK_o: do_action(ACT_OUTPUT); break;
                case SDLK_d: g_port_open_ts = e.key.timestamp; do_action(ACT_PORT); break;
                case SDLK_u: do_action(ACT_UNMUTE_ALL); break;
                // Bank (or MIDI device, in External mode) previous / next.
                case SDLK_LEFTBRACKET:
                    do_action(g_outmode == OUT_EXT ? ACT_MIDI_PREV : ACT_BANK_PREV); break;
                case SDLK_RIGHTBRACKET:
                    do_action(g_outmode == OUT_EXT ? ACT_MIDI_NEXT : ACT_BANK_NEXT); break;
                case SDLK_EQUALS: case SDLK_PLUS: case SDLK_KP_PLUS: do_action(ACT_TUP); break;
                case SDLK_MINUS: case SDLK_KP_MINUS: do_action(ACT_TDN); break;
                // Playlist scrolling: up/down by a row, left/right & PgUp/PgDn by a page.
                case SDLK_UP:       scroll_playlist(-1);    break;
                case SDLK_DOWN:     scroll_playlist(+1);    break;
                case SDLK_LEFT:  case SDLK_PAGEUP:   scroll_playlist(-page); break;
                case SDLK_RIGHT: case SDLK_PAGEDOWN: scroll_playlist(+page); break;
                case SDLK_HOME:     scroll_playlist(-1000000000); break;
                case SDLK_END:      scroll_playlist(+1000000000); break;
                }
            }
        }

        // Auto-advance when the current song finishes (wrapping in LOOP:ALL).
        if (engine.consume_song_ended()) {
            bool wrap = (g_loopmode == LOOP_ALL);
            if (!playlist.next(wrap) || !play_from_playlist(engine, playlist, wrap, false))
                engine.stop();
        }

        // The board went away (unplugged / wedged): carry on through the PC.
        if (g_outmode == OUT_BOARD && !engine.board_available()) {
            fprintf(stderr, "gui: board lost — switching output to PC\n");
            g_outmode = OUT_PC;
            apply_output();
        }

        poll_pick_folder();
        static long frame = 0; ++frame;
        run_test_hooks(frame);

        viz_update();
        g_nbtn = 0;

        int W, Hh; SDL_GetRendererOutputSize(ren, &W, &Hh);
        fill(0, 0, W, Hh, RGB{ 12, 12, 20 });
        draw_header(W);

        // Layout: playlist panel on the right, visualizer fills the rest.
        int panel_w = W / 3; if (panel_w < 260) panel_w = 260; if (panel_w > 380) panel_w = 380;
        int px = W - panel_w - 12, py = 48;
        int bottom = Hh - 76;                    // above the two control rows
        int mx, my; SDL_GetMouseState(&mx, &my);
        draw_playlist(px, py, panel_w, bottom - py, mx, my);

        int ax = 16, ay = 50, aw = px - 16 - 16, ah = bottom - ay - 18;
        if (aw > 40) {
            fill(ax - 5, ay - 5, aw + 10, ah + 10 + 18, RGB{ 8, 8, 14 });
            draw_bars(ax, ay, aw, ah);
        }

        draw_controls(W, Hh);
        if (g_port_dlg) draw_port_dialog(W, Hh);
        SDL_RenderPresent(ren);

        maybe_screenshot(frame, W, Hh);
    }

    // Hand the user's final settings back for persistence.
    cfg.bank    = engine.bank();
    cfg.recurse = g_recurse;
    cfg.loop    = g_loopmode;
    cfg.shuffle = playlist.shuffle();
    cfg.style   = style;
    cfg.outmode = g_outmode;
    // Remember the board device (a dry run isn't a device worth remembering).
    if (engine.board_dev() != "-")
        cfg.serial_dev = engine.board_dev().empty() ? "none" : engine.board_dev();
    SDL_GetWindowSize(g_win, &cfg.win_w, &cfg.win_h);

    // Don't wait on an Open Folder dialog the user left open.
    if (g_pick_thread.joinable()) g_pick_thread.detach();

    if (ren)   SDL_DestroyRenderer(ren);
    if (g_win) SDL_DestroyWindow(g_win);
    ren = nullptr; g_win = nullptr;
    return true;
}
