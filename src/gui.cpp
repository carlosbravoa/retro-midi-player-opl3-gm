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
 * or any typed path) and reopens the board on it without restarting. Also:
 * drag-and-drop of files/folders, find-as-you-type in the playlist ('/'), a PC
 * volume control, and automatic return to the board when it is plugged back in.
 *
 * The control bar is a flow layout: items wrap onto extra rows when the window
 * is too narrow, and the playlist/visualizer shrink to make room.
 */
#include "gui.hpp"
#include "engine.hpp"
#include "playlist.hpp"
#include "fanout_chip.hpp"
#include "config.hpp"
#include "serial_ports.hpp"
#include "durations.hpp"
#include "mpris.hpp"
#include "gm_names.h"

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
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#define NCH 18   // OPL3: 18 two-operator channels

// ── Shared drawing state ──────────────────────────────────────────────────────
namespace {

Engine   *g_eng = nullptr;
Playlist *g_pl  = nullptr;

// Mutually-exclusive output: FM to the OPL3 board, FM to PC speakers, or the
// raw MIDI stream to an external device. (No simultaneous output by design.)
enum { OUT_PC, OUT_BOARD, OUT_EXT };
int  g_outmode = OUT_PC;

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

// One SDL call per string: lit pixels are merged into horizontal runs and
// drawn as a single batch of rects (this is the bulk of every frame).
void draw_text(int x, int y, int s, RGB c, const char *txt)
{
    static std::vector<SDL_Rect> rects;
    rects.clear();
    int cx = x;
    for (const char *p = txt; *p; ++p) {
        const uint8_t *g = font_glyph(*p);
        if (g)
            for (int ry = 0; ry < FONT_H; ++ry)
                for (int rx = 0; rx < FONT_W; ) {
                    if (!(g[ry] & (1 << (FONT_W - 1 - rx)))) { ++rx; continue; }
                    int end = rx + 1;
                    while (end < FONT_W && (g[ry] & (1 << (FONT_W - 1 - end)))) ++end;
                    rects.push_back(SDL_Rect{ cx + rx * s, y + ry * s, (end - rx) * s, s });
                    rx = end;
                }
        cx += (FONT_W + 1) * s;
    }
    if (rects.empty()) return;
    SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, 255);
    SDL_RenderFillRects(ren, rects.data(), (int)rects.size());
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
    ACT_VDN, ACT_VUP,             // PC volume
    ACT_CLEAR,                    // empty the playlist
    ACT_BANK_DLG, ACT_BANK_AB,    // bank list dialog; flip to the previous bank
    ACT_HELP, ACT_MODAL_CLOSE,
    ACT_EXPORT, ACT_EXPORT_VGM, ACT_EXPORT_WAV, ACT_EXPORT_BOTH,
    ACT_FULLVIZ,                  // fullscreen visualizer on/off
    ACT_SCROLLBAR,                // grab the playlist scrollbar
    ACT_CHAN_BASE,                // ACT_CHAN_BASE + ch => toggle OPL3 channel ch
    ACT_PORTSEL_BASE = ACT_CHAN_BASE + NCH,      // + n => detected port n
    ACT_BANKSEL_BASE = ACT_PORTSEL_BASE + 32,    // + n => FM bank n
    ACT_ROW_BASE = ACT_BANKSEL_BASE + 512  // + n => playlist row n (must stay last)
};

struct Button { SDL_Rect r; int act; };
Button g_btn[1024];
int    g_nbtn = 0;
void reg_button(SDL_Rect b, int act) { if (g_nbtn < 1024) g_btn[g_nbtn++] = Button{ b, act }; }

// ── Visualizer ────────────────────────────────────────────────────────────────
// Two sources share the drawing code: the 18 OPL3 channels (what the board and
// the emulator play), or — when the output is External MIDI — the 16 MIDI
// channels the external synth is playing.
struct VizChan { float level, peak, hue; bool on; float note; };
VizChan viz[NCH];
int     g_viz_n = NCH;
bool    g_viz_midi = false;
uint8_t g_viz_held[16][128];      // MIDI view: velocity of each held key
int     g_hover_chan = -1;        // channel under the mouse (for the inspector)

int chan_a0(int ch) { return ch < 9 ? 0xA0 + ch : 0x1A0 + (ch - 9); }
int chan_b0(int ch) { return ch < 9 ? 0xB0 + ch : 0x1B0 + (ch - 9); }

void viz_update()
{
    g_viz_midi = (g_outmode == OUT_EXT);
    g_viz_n = g_viz_midi ? 16 : NCH;
    for (int ch = 0; ch < g_viz_n; ++ch) {
        float amp, pitch, note = 0.0f;
        if (g_viz_midi) {
            // Loudest held key; a note-on too short to catch held still shows
            // through its peak.
            g_eng->midi_held(ch, g_viz_held[ch]);
            int top = -1, vmax = 0;
            for (int k = 0; k < 128; ++k)
                if (g_viz_held[ch][k]) { top = k; if (g_viz_held[ch][k] > vmax) vmax = g_viz_held[ch][k]; }
            amp = vmax / 127.0f;
            float pk = g_eng->midi_take_peak(ch);
            if (pk > amp) amp = pk;
            pitch = top >= 0 ? top / 127.0f : viz[ch].hue;
            note = (float)top;
        } else {
            amp = powf((float)opl_channel_level(ch), 0.55f);   // perceptual
            int b0   = g_opl_regs[chan_b0(ch)];
            int fnum = g_opl_regs[chan_a0(ch)] | ((b0 & 3) << 8);
            int block = (b0 >> 2) & 7;
            pitch = (block * 1024 + fnum) / (8.0f * 1024.0f);
            // F-number -> Hz -> (fractional) MIDI note, so bends show too.
            if (fnum > 0)
                note = 69.0f + 12.0f * log2f(fnum * 49716.0f / (float)(1 << (20 - block)) / 440.0f);
        }

        VizChan *v = &viz[ch];
        v->on = amp > 0.02f;
        v->level += (amp - v->level) * (amp > v->level ? 0.6f : 0.20f);
        if (v->level < 0) v->level = 0;
        if (v->level > v->peak) v->peak = v->level;
        else v->peak -= 0.012f;
        if (v->peak < v->level) v->peak = v->level;
        if (v->on) { v->hue = pitch; v->note = note; }
    }
}

// Mutes follow the view: OPL3 channels for FM, MIDI channels for External.
uint32_t viz_mute_mask() { return g_viz_midi ? g_eng->midi_mute_mask() : g_eng->mute_mask(); }
void viz_set_mute_mask(uint32_t m)
{
    if (g_viz_midi) g_eng->set_midi_mute_mask(m);
    else            g_eng->set_mute_mask(m);
}
bool viz_muted(int ch) { return (viz_mute_mask() >> ch) & 1u; }

enum { STYLE_LED, STYLE_NEON, STYLE_SPECTRUM, STYLE_PIANO, STYLE_COUNT };
int style = STYLE_LED;
const char *STYLE_SHORT[] = { "LED", "NEON", "SPEC", "KEYS" };

void draw_bars(int ax, int ay, int aw, int ah)
{
    const int n = g_viz_n;
    int gap = aw / (n * 5); if (gap < 1) gap = 1;
    int slot = (aw - gap) / n, bw = slot - gap;

    int mx, my; SDL_GetMouseState(&mx, &my);
    for (int ch = 0; ch < n; ++ch) {
        int x = ax + gap + ch * slot;
        bool muted = viz_muted(ch);
        SDL_Rect hit = { x - gap / 2, ay, slot, ah + 22 };   // bar + its labels
        bool hover = mx >= hit.x && mx < hit.x + hit.w && my >= hit.y && my < hit.y + hit.h;
        if (hover) { fill(hit.x, hit.y, hit.w, hit.h, RGB{ 30, 34, 56 }); g_hover_chan = ch; }
        reg_button(hit, ACT_CHAN_BASE + ch);

        if (muted) {
            // Silenced: draw a greyed-out slot (the unlit meter, desaturated)
            // and a red number + "M" tag.
            RGB grey = { 70, 72, 84 };
            if (style == STYLE_LED) {
                int segs = 18, seg_h = ah / segs;
                for (int s = 0; s < segs; ++s)
                    fill(x, ay + ah - (s + 1) * seg_h + 1, bw, seg_h - 1, grey, 40);
            } else {
                fill(x, ay, bw, ah, grey, 40);
            }
            char lbl[16]; snprintf(lbl, sizeof lbl, "%d", ch + 1);
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

        char lbl[16]; snprintf(lbl, sizeof lbl, "%d", ch + 1);
        draw_text(x + bw / 2 - text_w(lbl, 1) / 2, ay + ah + 4, 1,
                  viz[ch].on ? RGB{ 220, 220, 220 } : RGB{ 80, 80, 100 }, lbl);
    }
}

// KEYS style: one keyboard strip per channel, C1..C8, with the sounding
// note(s) lit — a single (bendable) pitch per OPL3 channel, or every held key
// of a MIDI channel in the External view.
const int PIANO_LO = 24, PIANO_HI = 108;

void draw_piano(int ax, int ay, int aw, int ah)
{
    int mx, my; SDL_GetMouseState(&mx, &my);
    const int n = g_viz_n, label_w = 24;
    const int kx = ax + label_w, kw_total = aw - label_w, keys = PIANO_HI - PIANO_LO;
    if (kw_total < keys) return;
    const float kw = (float)kw_total / keys, rh = (float)ah / n;
    auto key_x = [&](float note) { return kx + (note - PIANO_LO) * kw; };

    // The key columns are the same for every row: draw them full height.
    static std::vector<SDL_Rect> white, black, cs;
    white.clear(); black.clear(); cs.clear();
    for (int k = 0; k < keys; ++k) {
        int note = PIANO_LO + k, pc = note % 12;
        int x0 = (int)key_x((float)note), x1 = (int)key_x((float)note + 1);
        bool blk = pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
        (blk ? black : white).push_back(SDL_Rect{ x0, ay, x1 - x0 - (kw >= 4 ? 1 : 0), ah });
        if (pc == 0) cs.push_back(SDL_Rect{ x0 - 1, ay, 1, ah });   // octave line at each C
    }
    SDL_SetRenderDrawColor(ren, 34, 36, 52, 255);
    SDL_RenderFillRects(ren, white.data(), (int)white.size());
    SDL_SetRenderDrawColor(ren, 15, 16, 24, 255);
    SDL_RenderFillRects(ren, black.data(), (int)black.size());
    SDL_SetRenderDrawColor(ren, 78, 86, 124, 255);
    SDL_RenderFillRects(ren, cs.data(), (int)cs.size());

    for (int ch = 0; ch < n; ++ch) {
        int y0 = ay + (int)(ch * rh), y1 = ay + (int)((ch + 1) * rh), h = y1 - y0;
        SDL_Rect hit = { ax, y0, aw, h };
        bool hover = mx >= hit.x && mx < hit.x + hit.w && my >= hit.y && my < hit.y + hit.h;
        if (hover) { fill(hit.x, hit.y, hit.w, hit.h, RGB{ 255, 255, 255 }, 16); g_hover_chan = ch; }
        reg_button(hit, ACT_CHAN_BASE + ch);
        fill(ax, y1 - 1, aw, 1, RGB{ 8, 8, 14 });               // row separator

        bool muted = viz_muted(ch);
        char lbl[16]; snprintf(lbl, sizeof lbl, muted ? "%dM" : "%d", ch + 1);
        draw_text(ax + 2, y0 + (h - FONT_H) / 2, 1,
                  muted ? RGB{ 220, 70, 80 } : viz[ch].on ? RGB{ 220, 220, 220 } : RGB{ 80, 80, 100 }, lbl);
        if (muted) { fill(kx, y0, kw_total, h - 1, RGB{ 8, 8, 14 }, 175); continue; }

        RGB col = hsv((float)ch / n, 0.70f, 1.0f);              // one colour per channel
        auto lit = [&](float note, float strength) {
            if (note < PIANO_LO) note = PIANO_LO;
            if (note > PIANO_HI - 1) note = PIANO_HI - 1;
            if (strength > 1) strength = 1;
            int x = (int)key_x(note), w = kw < 3 ? 3 : (int)kw;
            Uint8 a = (Uint8)(90 + 165 * strength);
            fill(x - 2, y0, w + 4, h - 1, col, (Uint8)(a / 4));   // glow
            fill(x, y0 + 1, w, h - 3, col, a);
        };
        if (g_viz_midi) {
            for (int k = 0; k < 128; ++k)
                if (g_viz_held[ch][k]) lit((float)k, g_viz_held[ch][k] / 127.0f);
        } else if (viz[ch].level > 0.04f) {   // fades out with the note's release
            lit(viz[ch].note, viz[ch].level);
        }
    }
}

void draw_viz(int ax, int ay, int aw, int ah)
{
    g_hover_chan = -1;
    if (style == STYLE_PIANO) draw_piano(ax, ay, aw, ah + 18);   // no label strip below
    else                      draw_bars(ax, ay, aw, ah);
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
bool g_recurse = false;
bool g_quit    = false;

// LOOP cycles OFF -> ONE (repeat the track, done by libADLMIDI) -> ALL (the
// playlist wraps around at the end).
enum { LOOP_OFF, LOOP_ONE, LOOP_ALL };
int  g_loopmode = LOOP_OFF;
const char *LOOP_NAME[] = { "OFF", "ONE", "ALL" };

int  g_click_x = 0, g_click_y = 0;   // the click being handled (seek bar, scrollbar)
SDL_Rect g_seek = { 0, 0, 0, 0 };   // seek bar rect (set each frame)

// Open Folder runs the (blocking) zenity/kdialog on a worker thread so the
// window keeps repainting; the main loop picks up the result.
std::thread       g_pick_thread;
std::atomic<bool> g_picking{false};
std::atomic<bool> g_pick_done{false};
std::mutex        g_pick_mx;
std::string       g_pick_result;   // "" = cancelled / no dialog available

// The board dropped out (or wasn't there at startup) while it was the wanted
// output: go back to it by ourselves when it reappears. Cleared as soon as the
// user picks an output or a port themselves.
bool g_resume_board = false;

std::string g_last_folder;            // most recent folder added (saved on exit)
std::vector<std::string> g_dropped;   // files/folders of a drop in progress

// Text typed while a text field is up (port dialog, playlist find) must not
// include the key press that opened it.
Uint32 g_text_open_ts = 0;

// Modal dialogs: drawn over a dimmed window, they take the keyboard and the
// frame's buttons. At most one at a time.
enum { M_NONE, M_PORT, M_BANK, M_HELP, M_EXPORT };
int  g_modal = M_NONE;

// Transient message over the visualizer ("Saved ...", "Board is back", ...).
std::string g_toast;
Uint32      g_toast_until = 0;
void toast(const std::string &msg, Uint32 ms = 4000) { g_toast = msg; g_toast_until = SDL_GetTicks() + ms; }

bool g_fullviz = false;            // fullscreen, visualizer only
int  g_windowed_w = 0, g_windowed_h = 0;

// Mouse held down on something draggable.
enum { DRAG_NONE, DRAG_SEEK, DRAG_SCROLL };
int      g_drag = DRAG_NONE;
SDL_Rect g_sb_track = { 0, 0, 0, 0 };   // playlist scrollbar track (set each frame)
int      g_sb_knob_h = 0;
int      g_hover_row = -1;              // playlist row under the mouse (display index)

int  g_export_pending = 0;         // ACT_EXPORT_* to run after this frame is shown
DurationScanner g_durations;

int  g_bank = 0;      // current bank ordinal (mirrors the engine's, per frame)
int  g_bank_prev = -1;             // the bank before the last change (for A/B)
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

// Right-click on a channel: solo it, or if it is already the only one
// playing, bring every channel back.
void solo_channel(int ch)
{
    uint32_t all = (1u << g_viz_n) - 1, solo = ~(1u << ch) & all;
    viz_set_mute_mask(viz_mute_mask() == solo ? 0 : solo);
}

// The user picks a bank: switch to it, keep the old one for A/B, and remember
// the choice for the song that is playing (it comes back with that song).
void set_bank_user(int n)
{
    if (n < 0 || n >= adl_getBanksCount() || n == g_eng->bank()) return;
    g_bank_prev = g_eng->bank();
    g_eng->set_bank(n);
    g_bank = n;
    const Track *cur = g_pl->current();
    if (cur && g_eng->has_song()) song_bank_set(cur->path, bank_name(n));
}

// Step the song position by `delta` seconds (arrow keys, media controls).
void seek_by(double delta)
{
    if (!g_eng->has_song()) return;
    double t = g_eng->position() + delta, len = g_eng->length();
    g_eng->seek(t < 0 ? 0 : (len > 0 && t > len - 1) ? len - 1 : t);
}

void set_fullviz(bool on)
{
    if (on == g_fullviz) return;
    if (on) SDL_GetWindowSize(g_win, &g_windowed_w, &g_windowed_h);
    g_fullviz = on;
    SDL_SetWindowFullscreen(g_win, on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
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

// Add folders (scanned) and files to the playlist — from Open Folder or a
// drag-and-drop. An empty playlist starts playing what was added.
void add_music(const std::vector<std::string> &paths)
{
    bool was_empty = g_pl->empty();
    std::vector<std::string> files;
    for (const std::string &p : paths) {
        std::error_code ec;
        if (std::filesystem::is_directory(p, ec)) {
            g_pl->add_folder(p, g_recurse);
            g_last_folder = p;
        } else {
            files.push_back(p);
        }
    }
    g_pl->add_files(files);
    if (was_empty && !g_pl->empty()) {
        g_pl->reshuffle(false);   // nothing was playing: random start in shuffle
        play_current();
    }
}

// A finished drag-and-drop: add everything that was dropped.
void finish_drop()
{
    if (g_dropped.empty()) return;
    add_music(g_dropped);
    g_dropped.clear();
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
    add_music({ folder });
}

// ── Board serial-port dialog ──────────────────────────────────────────────────
// PORT opens a modal list of the USB-serial devices present (ttyACM*/ttyUSB*,
// with their USB product names) plus a text field for any other path. Picking
// one reopens the board there and switches the output to it; the choice is
// saved in the config.
std::vector<SerialPort> g_ports;
std::string g_port_text;          // the custom-path field
std::string g_port_msg;           // why the last attempt failed

// A device as the user thinks of it: without the "/dev/" prefix.
std::string short_dev(const std::string &d)
{
    return d.rfind("/dev/", 0) == 0 ? d.substr(5) : d;
}

// The kernel name ("ttyUSB1") the board's device resolves to right now, ""
// with no board configured. Cached: this is asked every frame.
std::string board_tty()
{
    static std::string for_dev, tty;
    static Uint32 at = 0;
    const std::string &dev = g_eng->board_dev();
    Uint32 now = SDL_GetTicks();
    if (dev != for_dev || now - at > 1000) {
        for_dev = dev; at = now;
        tty = (dev.empty() || dev == "-") ? dev : serial_dev_tty(dev);
    }
    return tty;
}

void refresh_ports()
{
    g_ports = serial_ports_enumerate();
    if (g_ports.size() > 32) g_ports.resize(32);   // ACT_PORTSEL_BASE range
}

void close_modal()
{
    g_modal = M_NONE;
    SDL_StopTextInput();
}
void close_port_dialog() { close_modal(); }

void open_port_dialog()
{
    refresh_ports();
    g_port_msg.clear();
    // Start the custom field on the current device when it's one the list
    // can't offer (a symlink, another driver's name, ...).
    g_port_text = g_eng->board_dev();
    std::string tty = g_port_text.empty() ? std::string() : serial_dev_tty(g_port_text);
    for (const auto &p : g_ports)
        if (p.name == tty) g_port_text.clear();
    if (g_port_text == "-") g_port_text.clear();
    g_modal = M_PORT;
    SDL_StartTextInput();
}

// Reopen the board on `dev` ("" = no board). Success switches the output to it
// and closes the dialog; failure keeps the dialog up with the reason, and goes
// back to the board that was working (a typo shouldn't cost you the board).
// `alt` is a second name for the same device to try (the plain tty name, when
// `dev` is its stable by-id path and that isn't reachable, e.g. sandboxed).
void select_port(const std::string &dev, const std::string &alt = std::string())
{
    std::string prev = g_eng->board_dev();
    bool had_board = g_eng->board_available();
    g_resume_board = false;   // the user is choosing: no automatic switching

    if (g_eng->open_board(dev) || (!alt.empty() && g_eng->open_board(alt))) {
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
        if (e.text.timestamp == g_text_open_ts) return;   // the key that opened us
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

// ── Playlist find-as-you-type ─────────────────────────────────────────────────
// '/' (or 'f') starts it; typing jumps to the first track whose name contains
// the text, Up/Down (or Tab) step through matches, Enter plays, Esc cancels.
bool        g_find = false;
std::string g_find_text;
int         g_find_idx = -1;      // display index of the match, -1 = none

bool find_matches(int idx)
{
    auto lower = [](std::string s) { for (char &c : s) c = (char)tolower((unsigned char)c); return s; };
    return lower(g_pl->tracks()[idx].name).find(lower(g_find_text)) != std::string::npos;
}

// Look for a match starting at display index `start`, stepping by `dir`.
void find_from(int start, int dir)
{
    int n = (int)g_pl->size();
    g_find_idx = -1;
    if (n == 0 || g_find_text.empty()) return;
    for (int k = 0; k < n; ++k) {
        int idx = (((start + dir * k) % n) + n) % n;
        if (!find_matches(idx)) continue;
        g_find_idx = idx;
        if (idx < g_scroll || idx >= g_scroll + g_visible_rows)   // bring it into view
            g_scroll = idx - g_visible_rows / 2;
        return;
    }
}

void open_find()
{
    g_find = true;
    g_find_text.clear();
    g_find_idx = -1;
    SDL_StartTextInput();
}

void close_find()
{
    g_find = false;
    g_find_idx = -1;
    SDL_StopTextInput();
}

void find_key(const SDL_Event &e)
{
    int at = g_find_idx < 0 ? 0 : g_find_idx;
    if (e.type == SDL_TEXTINPUT) {
        if (e.text.timestamp == g_text_open_ts) return;   // the key that opened us
        for (const char *c = e.text.text; *c; ++c)
            if (*c >= ' ' && *c < 127 && g_find_text.size() < 60) g_find_text += *c;
        find_from(at, +1);   // stay on the current match while it still fits
        return;
    }
    switch (e.key.keysym.sym) {
    case SDLK_ESCAPE: close_find(); break;
    case SDLK_BACKSPACE:
        if (!g_find_text.empty()) g_find_text.pop_back();
        find_from(at, +1);
        break;
    case SDLK_DOWN: case SDLK_TAB: find_from(at + 1, +1); break;
    case SDLK_UP:                  find_from(at - 1, -1); break;
    case SDLK_RETURN: case SDLK_KP_ENTER:
        if (g_find_idx >= 0) {
            g_pl->select_display_index(g_find_idx);
            play_current();
        }
        close_find();
        break;
    }
}

// ── FM bank dialog ────────────────────────────────────────────────────────────
// The whole bank list, filtered as you type. Clicking a bank (or stepping with
// Up/Down) switches to it at once and leaves the dialog open, so banks can be
// auditioned against the playing song.
std::string g_bank_filter;
int         g_bank_scroll = 0;
int         g_bank_rows = 1;      // rows the list shows (set each frame)

std::vector<int> bank_filtered()
{
    auto lower = [](std::string s) { for (char &c : s) c = (char)tolower((unsigned char)c); return s; };
    std::string f = lower(g_bank_filter);
    std::vector<int> out;
    for (int i = 0, n = adl_getBanksCount(); i < n; ++i)
        if (f.empty() || lower(bank_name(i)).find(f) != std::string::npos) out.push_back(i);
    return out;
}

void bank_reveal()   // scroll the list so the current bank is in view
{
    std::vector<int> list = bank_filtered();
    for (int pos = 0; pos < (int)list.size(); ++pos) {
        if (list[pos] != g_bank) continue;
        if (pos < g_bank_scroll)                     g_bank_scroll = pos;
        else if (pos >= g_bank_scroll + g_bank_rows) g_bank_scroll = pos - g_bank_rows + 1;
        return;
    }
}

void open_bank_dialog()
{
    g_bank_filter.clear();
    g_bank_scroll = g_bank - 4;   // clamped when drawn
    g_modal = M_BANK;
    SDL_StartTextInput();
}

void bank_dialog_key(const SDL_Event &e)
{
    if (e.type == SDL_TEXTINPUT) {
        if (e.text.timestamp == g_text_open_ts) return;   // the key that opened us
        for (const char *c = e.text.text; *c; ++c)
            if (*c >= ' ' && *c < 127 && g_bank_filter.size() < 40) g_bank_filter += *c;
        g_bank_scroll = 0;
        return;
    }
    std::vector<int> list = bank_filtered();
    int pos = -1;
    for (int i = 0; i < (int)list.size(); ++i) if (list[i] == g_bank) pos = i;
    int last = (int)list.size() - 1;
    switch (e.key.keysym.sym) {
    case SDLK_ESCAPE: case SDLK_RETURN: case SDLK_KP_ENTER: close_modal(); break;
    case SDLK_BACKSPACE:
        if (!g_bank_filter.empty()) g_bank_filter.pop_back();
        g_bank_scroll = 0;
        break;
    case SDLK_DOWN:
        if (last >= 0) { set_bank_user(list[pos < 0 ? 0 : pos < last ? pos + 1 : last]); bank_reveal(); }
        break;
    case SDLK_UP:
        if (last >= 0) { set_bank_user(list[pos <= 0 ? 0 : pos - 1]); bank_reveal(); }
        break;
    case SDLK_PAGEDOWN: g_bank_scroll += g_bank_rows - 1; break;
    case SDLK_PAGEUP:   g_bank_scroll -= g_bank_rows - 1; break;
    }
}

// The mouse is down on the seek bar or the playlist scrollbar: follow it.
void drag_update(int x, int y)
{
    if (g_drag == DRAG_SEEK) {
        // A seek re-reads the song up to that point, so don't do it per pixel.
        static Uint32 last = 0;
        static int    last_x = -1;
        Uint32 now = SDL_GetTicks();
        if (!g_eng->has_song() || g_seek.w <= 0 || (x == last_x && now - last < 500)) return;
        if (now - last < 40) return;
        last = now; last_x = x;
        double f = (double)(x - g_seek.x) / g_seek.w;
        g_eng->seek((f < 0 ? 0 : f > 1 ? 1 : f) * g_eng->length());
    } else if (g_drag == DRAG_SCROLL) {
        int total = (int)g_pl->size(), span = g_sb_track.h - g_sb_knob_h;
        if (total <= g_visible_rows || span <= 0) return;
        double f = (double)(y - g_sb_track.y - g_sb_knob_h / 2) / span;
        g_scroll = (int)((f < 0 ? 0 : f > 1 ? 1 : f) * (total - g_visible_rows) + 0.5);
    }
}

// Render the current track to "<name>.opl3.vgm" / ".opl3.wav" beside it (or in
// the home folder if that one isn't writable). Blocks for a moment.
void run_export(int kind)
{
    const Track *cur = g_pl->current();
    if (!cur) return;
    namespace fs = std::filesystem;
    fs::path src(cur->path), dir = src.parent_path();
    if (access(dir.c_str(), W_OK) != 0) {
        const char *home = getenv("HOME");
        dir = (home && *home) ? home : ".";
    }
    std::string base = (dir / (src.stem().string() + ".opl3")).string();
    std::string vgm = (kind != ACT_EXPORT_WAV) ? base + ".vgm" : std::string();
    std::string wav = (kind != ACT_EXPORT_VGM) ? base + ".wav" : std::string();
    std::string err;
    if (g_eng->export_song(cur->path, vgm, wav, err)) {
        const char *what = kind == ACT_EXPORT_VGM ? ".opl3.vgm" : kind == ACT_EXPORT_WAV ? ".opl3.wav"
                                                                : ".opl3.vgm + .wav";
        toast("Saved " + src.stem().string() + what + "  in  " + dir.string(), 8000);
    } else {
        toast("Export failed: " + err, 8000);
    }
}

// Remove the playlist row under the mouse (Del). If it was playing, move on.
void remove_hovered_row()
{
    if (g_hover_row < 0) return;
    bool was_playing = g_eng->has_song();
    if (!g_pl->remove_display_index(g_hover_row)) return;   // not the current track
    if (was_playing && !g_pl->empty()) play_current();
    else                               g_eng->stop();
}

void do_action(int a)
{
    if (a >= ACT_ROW_BASE) {
        g_pl->select_display_index(a - ACT_ROW_BASE);
        play_current();
        return;
    }
    if (a >= ACT_BANKSEL_BASE) {
        set_bank_user(a - ACT_BANKSEL_BASE);   // the dialog stays open, to compare
        return;
    }
    if (a >= ACT_PORTSEL_BASE) {
        size_t i = (size_t)(a - ACT_PORTSEL_BASE);
        // Prefer the stable by-id name, so the choice survives renumbering.
        if (i < g_ports.size())
            select_port(g_ports[i].device(),
                        g_ports[i].stable.empty() ? std::string() : g_ports[i].name);
        return;
    }
    if (a >= ACT_CHAN_BASE) {
        viz_set_mute_mask(viz_mute_mask() ^ (1u << (a - ACT_CHAN_BASE)));
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
    case ACT_UNMUTE_ALL: viz_set_mute_mask(0); break;
    case ACT_STOP:  g_eng->stop(); break;
    case ACT_LOOP:
        g_loopmode = (g_loopmode + 1) % 3;
        g_eng->set_loop(g_loopmode == LOOP_ONE);
        break;
    case ACT_SEEK: g_drag = DRAG_SEEK; drag_update(g_click_x, g_click_y); break;
    case ACT_SHUFFLE: g_pl->set_shuffle(!g_pl->shuffle()); break;
    case ACT_STYLE: style = (style + 1) % STYLE_COUNT; break;
    case ACT_TUP:   g_eng->set_tempo(g_eng->tempo() < 4.0 ? g_eng->tempo() * 1.25 : 4.0); break;
    case ACT_TDN:   g_eng->set_tempo(g_eng->tempo() > 0.25 ? g_eng->tempo() / 1.25 : 0.25); break;
    case ACT_OUTPUT: {
        g_resume_board = false;   // the user is choosing: no automatic switching
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
    case ACT_BANK_PREV: set_bank_user(g_bank - 1); break;
    case ACT_BANK_NEXT: set_bank_user(g_bank + 1); break;
    case ACT_BANK_AB:   if (g_bank_prev >= 0) set_bank_user(g_bank_prev); break;
    case ACT_BANK_DLG:  open_bank_dialog(); break;
    case ACT_HELP:      g_modal = (g_modal == M_HELP) ? M_NONE : M_HELP; break;
    case ACT_MODAL_CLOSE: close_modal(); break;
    case ACT_EXPORT:
        if (g_pl->current()) g_modal = M_EXPORT;
        else toast("Nothing to export: the playlist is empty");
        break;
    case ACT_EXPORT_VGM: case ACT_EXPORT_WAV: case ACT_EXPORT_BOTH:
        close_modal();
        g_export_pending = a;       // runs once "Exporting..." is on screen
        toast("Exporting...", 60000);
        break;
    case ACT_FULLVIZ: set_fullviz(!g_fullviz); break;
    case ACT_SCROLLBAR: g_drag = DRAG_SCROLL; drag_update(g_click_x, g_click_y); break;
    case ACT_MIDI_PREV: cycle_midi(-1); break;
    case ACT_MIDI_NEXT: cycle_midi(+1); break;
    case ACT_RECURSE:   g_recurse = !g_recurse; break;
    case ACT_OPEN: start_pick_folder(); break;
    case ACT_PORT: if (g_modal == M_PORT) close_modal(); else open_port_dialog(); break;
    case ACT_PORT_CLOSE:  close_port_dialog(); break;
    case ACT_PORT_RESCAN: refresh_ports(); g_port_msg.clear(); break;
    case ACT_PORT_NONE:   select_port(std::string()); break;
    case ACT_PORT_CUSTOM: submit_custom_port(); break;
    case ACT_VUP: case ACT_VDN: {   // 10% steps, 0..200%
        int pct = (int)lroundf(g_eng->volume() * 100.0f) + (a == ACT_VUP ? 10 : -10);
        g_eng->set_volume((pct < 0 ? 0 : pct > 200 ? 200 : pct) / 100.0f);
        break;
    }
    case ACT_CLEAR:
        g_eng->stop();
        g_pl->clear();
        g_last_folder.clear();   // don't bring it back on the next launch
        g_scroll = 0;
        if (g_find) close_find();
        break;
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
    if (g_find) {
        // The find field. Red while nothing matches.
        bool miss = !g_find_text.empty() && g_find_idx < 0;
        std::string t = g_find_text;
        int maxc = (pw - 12 - text_w("FIND: _", 2)) / ((FONT_W + 1) * 2);
        if (maxc > 0 && (int)t.size() > maxc) t = t.substr(t.size() - maxc);   // keep the tail
        t = "FIND: " + t + (SDL_GetTicks() / 500 % 2 == 0 ? "_" : "");
        draw_text(px + 6, py + 4, 2, miss ? RGB{ 240, 110, 110 } : RGB{ 240, 220, 120 }, t.c_str());
    } else {
        char hdr[64];
        snprintf(hdr, sizeof hdr, "PLAYLIST  %d", (int)g_pl->size());
        draw_text(px + 6, py + 4, 2, RGB{ 200, 210, 245 }, hdr);
        const char *hint = "/ FIND";
        draw_text(px + pw - 8 - text_w(hint, 1), py + 7, 1, RGB{ 90, 100, 140 }, hint);
    }

    g_hover_row = -1;
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
        if (hover) g_hover_row = idx;

        if (current)   fill(row.x, row.y, row.w, row.h, RGB{ 40, 70, 120 });
        else if (hover) fill(row.x, row.y, row.w, row.h, RGB{ 24, 28, 44 });
        if (g_find && idx == g_find_idx) {            // the find match
            SDL_SetRenderDrawColor(ren, 240, 220, 120, 255);
            SDL_RenderDrawRect(ren, &row);
        }

        char num[16]; snprintf(num, sizeof num, "%d", idx + 1);
        RGB nc = current ? RGB{ 210, 225, 255 } : RGB{ 90, 100, 140 };
        draw_text(px + 6, ry + 1, 1, nc, num);

        // Length, once the background scan has it.
        int right = px + pw - 14;
        double secs = g_durations.get(tracks[idx].path);
        if (secs > 0) {
            char d[16]; snprintf(d, sizeof d, "%d:%02d", (int)secs / 60, (int)secs % 60);
            right -= text_w(d, 1);
            draw_text(right, ry + 5, 1, current ? RGB{ 190, 205, 240 } : RGB{ 100, 110, 150 }, d);
            right -= 6;
        }
        std::string nm = ellipsize(tracks[idx].name, right - (px + 34), 2);
        RGB tc = current ? RGB{ 255, 255, 255 } : RGB{ 175, 185, 215 };
        draw_text(px + 34, ry + 1, 2, tc, nm.c_str());

        reg_button(row, ACT_ROW_BASE + idx);
    }

    // Scrollbar: click or drag anywhere on its track.
    g_sb_track = SDL_Rect{ 0, 0, 0, 0 };
    if (total > rows) {
        g_sb_track = SDL_Rect{ px + pw - 9, list_y, 8, list_h };
        g_sb_knob_h = list_h * rows / total; if (g_sb_knob_h < 12) g_sb_knob_h = 12;
        int knob_y = list_y + (list_h - g_sb_knob_h) * g_scroll / (total - rows);
        bool hot = g_drag == DRAG_SCROLL ||
                   (mx >= g_sb_track.x && mx < g_sb_track.x + g_sb_track.w &&
                    my >= g_sb_track.y && my < g_sb_track.y + g_sb_track.h);
        fill(g_sb_track.x, list_y, g_sb_track.w, list_h, RGB{ 20, 22, 34 });
        fill(g_sb_track.x + 1, knob_y, g_sb_track.w - 2, g_sb_knob_h,
             hot ? RGB{ 130, 145, 200 } : RGB{ 80, 90, 130 });
        reg_button(g_sb_track, ACT_SCROLLBAR);
    }
}

// ── Controls bar (flow layout) ────────────────────────────────────────────────
// The controls are built as a list of items and laid out left to right,
// wrapping onto a new row when the window is too narrow. build_controls() runs
// first each frame (the rest of the layout needs the row count); draw_controls()
// draws what it laid out.
enum { IT_BTN, IT_ICON, IT_TEXT };
struct Item {
    int         kind = IT_BTN;
    std::string label, alt;       // alt: shorter label to try before wrapping
    int         icon = 0, act = ACT_NOP;
    bool        active = false;
    int         w = 0, pre = 0;   // width; extra gap before (not at a row start)
    bool        glue = false;     // never wrap between the previous item and this
    bool        newrow = false;   // always starts a row
    RGB         col = { 200, 210, 245 };
    int         x = 0, row = 0;   // set by the layout
};
std::vector<Item> g_items;
int g_ctrl_rows = 2;
const int CTRL_BH = 26, CTRL_PITCH = 32;

int btn_w(const std::string &label) { return text_w(label.c_str(), 2) + 12; }

void build_controls(int W)
{
    g_items.clear();
    auto btn = [](const std::string &l, int act, bool active = false) -> Item & {
        Item it; it.label = l; it.act = act; it.active = active; it.w = btn_w(l);
        g_items.push_back(it); return g_items.back();
    };
    auto icon = [](int type, int act) -> Item & {
        Item it; it.kind = IT_ICON; it.icon = type; it.act = act; it.w = CTRL_BH + 6;
        g_items.push_back(it); return g_items.back();
    };
    auto text = [](const std::string &l, int w, RGB col) -> Item & {
        Item it; it.kind = IT_TEXT; it.label = l; it.w = w; it.col = col; it.glue = true;
        g_items.push_back(it); return g_items.back();
    };
    char b[64];

    // Routing / bank / library.
    const char *om = g_outmode == OUT_PC ? "PC" : g_outmode == OUT_BOARD ? "BOARD" : "EXT-MIDI";
    btn(std::string("OUT:") + om, ACT_OUTPUT);

    // In External mode the FM bank is irrelevant, so this control becomes the
    // ALSA device picker; otherwise it's the FM bank picker.
    bool ext = (g_outmode == OUT_EXT);
    btn("<", ext ? ACT_MIDI_PREV : ACT_BANK_PREV).pre = 4;
    std::string label;
    if (ext) label = "MIDI: " + (g_eng->gm_available() ? g_eng->gm_name() : std::string("(none)"));
    else {
        // '*' = this is the bank remembered for the song that is playing.
        const Track *cur = g_pl->current();
        bool mine = cur && g_eng->has_song() && song_bank_get(cur->path) == bank_name(g_bank);
        label = std::string(mine ? "BANK*: " : "BANK: ") + bank_name(g_bank);
    }
    Item &bank = text(label, W < 900 ? 196 : 284, RGB{ 200, 210, 245 });
    if (!ext) bank.act = ACT_BANK_DLG;   // click the name for the full list
    btn(">", ext ? ACT_MIDI_NEXT : ACT_BANK_NEXT).glue = true;

    btn(std::string("RECURSE:") + (g_recurse ? "ON" : "OFF"), ACT_RECURSE, g_recurse).pre = 10;
    btn(g_picking ? "OPENING.." : "OPEN", ACT_OPEN, g_picking);
    btn("CLEAR", ACT_CLEAR);
    btn("EXPORT", ACT_EXPORT);
    btn("?", ACT_HELP);

    // Transport.
    bool playing = g_eng->has_song() && !g_eng->paused();
    icon(ICON_PREV, ACT_PREV).newrow = true;
    icon(playing ? ICON_PAUSE : ICON_PLAY, ACT_PLAY);
    icon(ICON_NEXT, ACT_NEXT);
    icon(ICON_STOP, ACT_STOP);
    btn(std::string("LOOP:") + LOOP_NAME[g_loopmode], ACT_LOOP, g_loopmode != LOOP_OFF).pre = 6;
    btn(std::string("SHUF:") + (g_pl->shuffle() ? "ON" : "OFF"), ACT_SHUFFLE, g_pl->shuffle());
    btn(STYLE_SHORT[style], ACT_STYLE);

    btn("T-", ACT_TDN);
    snprintf(b, sizeof b, "%.2fx", g_eng->tempo());
    text(b, text_w(b, 2) + 4, RGB{ 150, 170, 210 });
    btn("T+", ACT_TUP).glue = true;

    btn("V-", ACT_VDN).pre = 6;
    snprintf(b, sizeof b, "%3d%%", (int)lroundf(g_eng->volume() * 100.0f));
    text(b, text_w(b, 2) + 4, RGB{ 150, 170, 210 });
    btn("V+", ACT_VUP).glue = true;

    // Board serial port: names the device when there's room for it.
    std::string dev = board_tty();
    Item &port = btn("PORT:" + (dev.empty() ? std::string("NONE") : dev == "-" ? std::string("DRY-RUN") : dev),
                     ACT_PORT, g_eng->board_available());
    port.pre = 6;
    port.alt = "PORT";

    // Flow.
    const int left = 12, right = W - 12, gap = 6;
    int x = left, row = 0;
    for (size_t i = 0; i < g_items.size(); ++i) {
        Item &it = g_items[i];
        if (it.newrow && x != left) { ++row; x = left; }
        int pre = (x == left) ? 0 : it.pre;
        if (!it.glue && x != left) {
            int need = it.w;                         // this item + what's glued to it
            for (size_t j = i + 1; j < g_items.size() && g_items[j].glue; ++j)
                need += gap + g_items[j].w;
            if (x + pre + need > right) {
                if (!it.alt.empty() && x + pre + btn_w(it.alt) <= right) {
                    it.label = it.alt; it.w = btn_w(it.alt);
                } else {
                    ++row; x = left; pre = 0;
                }
            }
        }
        x += pre;
        it.x = x; it.row = row;
        x += it.w + gap;
    }
    g_ctrl_rows = row + 1;
}

int controls_height() { return g_ctrl_rows * CTRL_PITCH + 12; }

void draw_controls(int Hh)
{
    int mx, my; SDL_GetMouseState(&mx, &my);
    for (const Item &it : g_items) {
        int y = Hh - 34 - (g_ctrl_rows - 1 - it.row) * CTRL_PITCH;
        if (it.kind == IT_BTN)
            add_button(it.x, y, CTRL_BH, it.label.c_str(), it.act, mx, my, it.active);
        else if (it.kind == IT_ICON)
            add_icon_button(it.x, y, CTRL_BH, it.icon, it.act, mx, my);
        else {
            RGB col = it.col;
            if (it.act != ACT_NOP) {   // clickable text (the bank name)
                SDL_Rect r = { it.x, y, it.w, CTRL_BH };
                if (mx >= r.x && mx < r.x + r.w && my >= r.y && my < r.y + r.h) {
                    fill(r.x, r.y, r.w, r.h, RGB{ 34, 36, 54 });
                    col = RGB{ 255, 255, 255 };
                }
                reg_button(r, it.act);
            }
            draw_text(it.x + 2, y + (CTRL_BH - FONT_H * 2) / 2, 2, col,
                      ellipsize(it.label, it.w - 4, 2).c_str());
        }
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
    reg_button(SDL_Rect{ 0, 0, W, Hh }, ACT_PORT_CLOSE);    // clicks outside it

    const int rh = 26, gap = 4, pad = 14;
    const int fixed = pad + 24 + (rh + gap) + 12 + 14 + rh + 6 + 22 + rh + pad;
    int fit = (Hh - 24 - fixed) / (rh + gap); if (fit < 1) fit = 1;
    int shown = (int)g_ports.size() < fit ? (int)g_ports.size() : fit;
    int extra = (int)g_ports.size() - shown;
    int list_rows = shown > 0 ? shown : 1;           // "none found" takes a row

    int pw = W - 40 < 620 ? W - 40 : 620, ph = fixed + list_rows * (rh + gap);
    int px = (W - pw) / 2, py = (Hh - ph) / 2;
    fill(px, py, pw, ph, RGB{ 16, 18, 30 });
    SDL_SetRenderDrawColor(ren, 110, 125, 180, 255);
    SDL_Rect frame = { px, py, pw, ph }; SDL_RenderDrawRect(ren, &frame);
    reg_button(frame, ACT_NOP);                             // clicks on the panel

    int x = px + pad, w = pw - 2 * pad, y = py + pad;
    draw_text(x, y, 2, RGB{ 240, 240, 255 }, "OPL3 BOARD SERIAL PORT");
    y += 24;

    // Detected devices; the one the board is on is highlighted.
    std::string cur = short_dev(g_eng->board_dev());
    std::string cur_tty = board_tty();
    bool alive = g_eng->board_available();
    for (int i = 0; i < shown; ++i) {
        bool is_cur = (g_ports[i].name == cur_tty);
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

    // Custom path.
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
    }
    if (SDL_GetTicks() / 500 % 2 == 0) fill(cx, ty, 2, FONT_H * 2, RGB{ 235, 240, 255 });
    if (custom_cur)
        draw_text(x + fw - 8 - text_w("IN USE", 1), y + (rh - FONT_H) / 2, 1,
                  RGB{ 120, 190, 240 }, "IN USE");
    add_button(x + fw + 6, y, rh, "OPEN", ACT_PORT_CUSTOM, mx, my);
    y += rh + 6;

    // Result of the last attempt.
    if (!g_port_msg.empty())
        draw_text(x, y + 2, 1, RGB{ 240, 110, 110 }, ellipsize(g_port_msg, w, 1).c_str());
    y += 22;

    int bx = add_button(x, y, rh, "RESCAN", ACT_PORT_RESCAN, mx, my);
    add_button(bx, y, rh, "CLOSE", ACT_PORT_CLOSE, mx, my);
}

// Dimmed backdrop + centred panel shared by the dialogs. Resets the frame's
// buttons (modal) and returns the panel rect.
SDL_Rect modal_panel(int W, int Hh, int pw, int ph, const char *title)
{
    fill(0, 0, W, Hh, RGB{ 0, 0, 0 }, 185);
    g_nbtn = 0;
    reg_button(SDL_Rect{ 0, 0, W, Hh }, ACT_MODAL_CLOSE);   // clicks outside it
    if (pw > W - 40) pw = W - 40;
    if (ph > Hh - 24) ph = Hh - 24;
    SDL_Rect r = { (W - pw) / 2, (Hh - ph) / 2, pw, ph };
    fill(r.x, r.y, r.w, r.h, RGB{ 16, 18, 30 });
    SDL_SetRenderDrawColor(ren, 110, 125, 180, 255);
    SDL_RenderDrawRect(ren, &r);
    reg_button(r, ACT_NOP);                                 // clicks on the panel
    draw_text(r.x + 14, r.y + 14, 2, RGB{ 240, 240, 255 }, title);
    return r;
}

void draw_bank_dialog(int W, int Hh)
{
    int mx, my; SDL_GetMouseState(&mx, &my);
    SDL_Rect r = modal_panel(W, Hh, 620, 560, "FM INSTRUMENT BANK");
    const int pad = 14, rh = 20;
    int x = r.x + pad, w = r.w - 2 * pad, y = r.y + pad + 24;

    std::string f = "FIND: " + g_bank_filter + (SDL_GetTicks() / 500 % 2 == 0 ? "_" : "");
    std::vector<int> list = bank_filtered();
    draw_text(x, y, 2, list.empty() ? RGB{ 240, 110, 110 } : RGB{ 240, 220, 120 }, f.c_str());
    y += 20;
    draw_text(x, y, 1, RGB{ 110, 120, 160 },
              ellipsize("TYPE TO FILTER  -  CLICK OR UP/DOWN TO SWITCH  -  ENTER TO CLOSE", w, 1).c_str());
    y += 14;

    int list_h = r.y + r.h - pad - CTRL_BH - 8 - y;
    int rows = list_h / rh; if (rows < 1) rows = 1;
    g_bank_rows = rows;
    int total = (int)list.size();
    if (g_bank_scroll > total - rows) g_bank_scroll = total - rows;
    if (g_bank_scroll < 0) g_bank_scroll = 0;

    const Track *cur = g_pl->current();
    std::string mine = (cur && g_eng->has_song()) ? song_bank_get(cur->path) : std::string();
    fill(x, y, w, rows * rh, RGB{ 8, 10, 18 });
    for (int i = 0; i < rows && g_bank_scroll + i < total; ++i) {
        int b = list[g_bank_scroll + i], ry = y + i * rh;
        SDL_Rect row = { x, ry, w - 10, rh };
        bool hover = mx >= row.x && mx < row.x + row.w && my >= row.y && my < row.y + row.h;
        if (b == g_bank) fill(row.x, row.y, row.w, row.h, RGB{ 40, 70, 120 });
        else if (hover)  fill(row.x, row.y, row.w, row.h, RGB{ 24, 28, 44 });
        char num[8]; snprintf(num, sizeof num, "%d", b);
        draw_text(x + 6, ry + 7, 1, RGB{ 90, 100, 140 }, num);
        std::string nm = bank_name(b);
        if (!mine.empty() && mine == nm) nm = "* " + nm;   // this song's remembered bank
        draw_text(x + 30, ry + 3, 2, b == g_bank ? RGB{ 255, 255, 255 } : RGB{ 175, 185, 215 },
                  ellipsize(nm, w - 46, 2).c_str());
        reg_button(row, ACT_BANKSEL_BASE + b);
    }
    if (total > rows) {
        int knob_h = rows * rh * rows / total; if (knob_h < 12) knob_h = 12;
        int knob_y = y + (rows * rh - knob_h) * g_bank_scroll / (total - rows);
        fill(x + w - 7, y, 6, rows * rh, RGB{ 20, 22, 34 });
        fill(x + w - 6, knob_y, 4, knob_h, RGB{ 80, 90, 130 });
    }
    int by = r.y + r.h - pad - CTRL_BH;
    int bx = add_button(x, by, CTRL_BH, "CLOSE", ACT_MODAL_CLOSE, mx, my);
    if (g_bank_prev >= 0) {
        std::string ab = std::string("A/B: ") + bank_name(g_bank_prev);
        add_button(bx, by, CTRL_BH, ellipsize(ab, w - (bx - x) - 12, 2).c_str(), ACT_BANK_AB, mx, my);
    }
}

void draw_help_dialog(int W, int Hh)
{
    static const char *const keys[][2] = {
        { "Space",        "Play / pause" },
        { "N  P",         "Next / previous track" },
        { "S",            "Stop" },
        { "Left  Right",  "Seek 5 s (Shift: 30 s)" },
        { "L  H",         "Loop mode / shuffle" },
        { "+  -",         "Tempo" },
        { "9  0",         "Volume (PC output)" },
        { "O",            "Output: PC / board / external MIDI" },
        { "D",            "Board serial port" },
        { "B",            "Bank list (or click the bank name)" },
        { "[  ]",         "Previous / next bank (or MIDI device)" },
        { "A",            "Flip to the previous bank (A/B)" },
        { "V",            "Visualizer style" },
        { "F11",          "Fullscreen visualizer" },
        { "Click bar",    "Mute channel (right-click: solo)" },
        { "U",            "Unmute all" },
        { "/  F",         "Find a track by name" },
        { "Up Down PgUp PgDn", "Scroll the playlist" },
        { "Del",          "Remove the track under the mouse" },
        { "E",            "Export the track to VGM / WAV" },
        { "?  F1",        "This help" },
        { "Q  Esc",       "Quit" },
    };
    const int n = (int)(sizeof keys / sizeof keys[0]), lh = 18;
    SDL_Rect r = modal_panel(W, Hh, 700, 14 + 28 + n * lh + 30, "KEYS");
    int y = r.y + 14 + 28;
    bool small = r.h < 14 + 28 + n * lh + 30;      // short window: tighter lines
    int step = small ? (r.h - 14 - 28 - 26) / n : lh;
    for (int i = 0; i < n; ++i, y += step) {
        draw_text(r.x + 14, y, small ? 1 : 2, RGB{ 240, 220, 120 }, keys[i][0]);
        draw_text(r.x + (small ? 130 : 232), y, small ? 1 : 2, RGB{ 200, 210, 245 },
                  ellipsize(keys[i][1], r.w - (small ? 144 : 246), small ? 1 : 2).c_str());
    }
    draw_text(r.x + 14, r.y + r.h - 18, 1, RGB{ 110, 120, 160 },
              "Drag files or folders onto the window to add them.  Any key closes this.");
}

void draw_export_dialog(int W, int Hh)
{
    int mx, my; SDL_GetMouseState(&mx, &my);
    const int rh = 26, gap = 4, pad = 14;
    SDL_Rect r = modal_panel(W, Hh, 620, pad + 24 + 16 + 3 * (rh + gap) + 8 + rh + pad, "EXPORT THIS TRACK");
    int x = r.x + pad, w = r.w - 2 * pad, y = r.y + pad + 24;
    const Track *cur = g_pl->current();
    draw_text(x, y, 1, RGB{ 150, 170, 210 },
              ellipsize(std::string("Rendered start to end with the current bank and mutes:  ") +
                        (cur ? cur->name : std::string()), w, 1).c_str());
    y += 16;
    port_row(x, y, w, rh, "VGM", "OPL3 register log, for hardware players", ACT_EXPORT_VGM, false, mx, my);
    y += rh + gap;
    port_row(x, y, w, rh, "WAV", "Audio, as the PC output plays it", ACT_EXPORT_WAV, false, mx, my);
    y += rh + gap;
    port_row(x, y, w, rh, "BOTH", "", ACT_EXPORT_BOTH, false, mx, my);
    y += rh + gap + 8;
    add_button(x, y, rh, "CANCEL", ACT_MODAL_CLOSE, mx, my);
}

// ── Channel inspector (tooltip on the visualizer) ─────────────────────────────
// One cycle of OPL3 waveform `ws` (0..7) at phase t in [0,1), in -1..1.
float opl_wave(int ws, float t)
{
    const float two_pi = 6.2831853f;
    float s = sinf(two_pi * t);
    switch (ws & 7) {
    case 0: return s;                                         // sine
    case 1: return s > 0 ? s : 0;                             // half sine
    case 2: return fabsf(s);                                  // rectified sine
    case 3: return fmodf(t, 0.5f) < 0.25f ? fabsf(s) : 0;     // quarter pulses
    case 4: return t < 0.5f ? sinf(two_pi * 2 * t) : 0;       // sine, double rate, half the time
    case 5: return t < 0.5f ? fabsf(sinf(two_pi * 2 * t)) : 0;
    case 6: return t < 0.5f ? 1.0f : -1.0f;                   // square
    default: return t < 0.5f ? expf(-6.0f * t) : -expf(-6.0f * (1.0f - t));   // "derived square"
    }
}

void draw_wave(int x, int y, int w, int h, int ws, RGB c)
{
    fill(x, y, w, h, RGB{ 6, 7, 12 });
    fill(x, y + h / 2, w, 1, RGB{ 40, 44, 66 });
    static std::vector<SDL_Rect> pts; pts.clear();
    int prev = 0;
    for (int i = 0; i < w; ++i) {
        int py = y + h / 2 - (int)lroundf(opl_wave(ws, (float)i / w) * (h / 2 - 1));
        int a = (i == 0) ? py : (prev < py ? prev : py), b = (i == 0) ? py : (prev > py ? prev : py);
        pts.push_back(SDL_Rect{ x + i, a, 1, b - a + 1 });    // join to the previous sample
        prev = py;
    }
    SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, 255);
    SDL_RenderFillRects(ren, pts.data(), (int)pts.size());
}

std::string note_name(int note)
{
    static const char *const names[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    if (note < 0 || note > 127) return "-";
    return std::string(names[note % 12]) + std::to_string(note / 12 - 1);
}

void draw_inspector(int W, int Hh)
{
    int ch = g_hover_chan;
    int mx, my; SDL_GetMouseState(&mx, &my);
    if (const char *t = getenv("RWINSPECT")) sscanf(t, "%d,%d,%d", &ch, &mx, &my);   // test hook
    if (ch < 0 || ch >= g_viz_n) return;

    char line[160];
    std::string head, sub;
    bool fm = !g_viz_midi;
    int set = ch / 9 * 0x100, c = ch % 9;
    static const int opoff[9] = { 0, 1, 2, 8, 9, 10, 16, 17, 18 };
    if (fm) {
        int ins = g_eng->channel_instrument(ch);
        const char *nm = gm_name(ins);
        snprintf(line, sizeof line, "OPL3 ch %d   %s%s", ch + 1,
                 ins < 0 ? "(nothing played yet)" : ins >= 128 ? "Drum: " : "", ins < 0 ? "" : nm);
        head = line;
        int c0 = g_opl_regs[set + 0xC0 + c];
        // 4-op: register 0x104 pairs channels 1-3 with 4-6 and 10-12 with 13-15.
        std::string mode = "2-op ";
        mode += (c0 & 1) ? "additive" : "FM";
        if (c < 6) {
            int bit = (ch >= 9 ? 3 : 0) + (c % 3);
            if (g_opl_regs[0x104] & (1 << bit)) {
                snprintf(line, sizeof line, "4-op with ch %d", ch + 1 + (c < 3 ? 3 : -3));
                mode = line;
            }
        }
        if (ch >= 6 && ch <= 8 && (g_opl_regs[0xBD] & 0x20)) mode = "rhythm-mode drums";
        snprintf(line, sizeof line, "%s   feedback %d   note %s%s", mode.c_str(), (c0 >> 1) & 7,
                 viz[ch].on ? note_name((int)lroundf(viz[ch].note)).c_str() : "-",
                 viz_muted(ch) ? "   MUTED" : "");
        sub = line;
    } else {
        int prog = g_eng->midi_program(ch);
        snprintf(line, sizeof line, "MIDI ch %d   %s", ch + 1, ch == 9 ? "Drums" : GM_INSTRUMENT[prog & 127]);
        head = line;
        std::string held;
        for (int k = 0; k < 128 && held.size() < 60; ++k)
            if (g_viz_held[ch][k]) held += (held.empty() ? "" : " ") + (ch == 9 && *gm_name(128 + k)
                                           ? std::string(gm_name(128 + k)) + "," : note_name(k));
        sub = (ch == 9 ? "" : "program " + std::to_string(prog + 1) + "   ") +
              (held.empty() ? std::string("silent") : "playing " + held) + (viz_muted(ch) ? "   MUTED" : "");
    }

    int w = 460, h = fm ? 14 + 18 + 14 + 2 * 22 + 12 + 8 : 14 + 18 + 14 + 8;
    if (w > W - 16) w = W - 16;
    int x = mx + 16, y = my + 16;
    if (x + w > W - 8) x = mx - 16 - w;
    if (x < 8) x = 8;
    if (y + h > Hh - 8) y = my - 12 - h;
    if (y < 8) y = 8;
    fill(x, y, w, h, RGB{ 16, 18, 30 }, 240);
    SDL_SetRenderDrawColor(ren, 110, 125, 180, 255);
    SDL_Rect box = { x, y, w, h }; SDL_RenderDrawRect(ren, &box);
    draw_text(x + 8, y + 8, 2, RGB{ 240, 240, 255 }, ellipsize(head, w - 16, 2).c_str());
    draw_text(x + 8, y + 28, 1, RGB{ 150, 170, 210 }, ellipsize(sub, w - 16, 1).c_str());
    if (!fm) return;

    // The two operators of this channel, straight from the register shadow.
    for (int op = 0; op < 2; ++op) {
        int o = set + opoff[c] + op * 3, oy = y + 44 + op * 22;
        int r20 = g_opl_regs[o + 0x20], r40 = g_opl_regs[o + 0x40], r60 = g_opl_regs[o + 0x60],
            r80 = g_opl_regs[o + 0x80], ws = g_opl_regs[o + 0xE0] & 7;
        draw_wave(x + 8, oy, 34, 18, ws, op ? RGB{ 120, 220, 140 } : RGB{ 240, 200, 90 });
        char mult[8];                                 // multiplier 0 means x0.5
        if (r20 & 15) snprintf(mult, sizeof mult, "x%d", r20 & 15);
        else          snprintf(mult, sizeof mult, "x.5");
        snprintf(line, sizeof line, "%s %-3s lvl %2d  A%-2d D%-2d S%-2d R%-2d %s%s%s",
                 op ? "CAR" : "MOD", mult, 63 - (r40 & 63),
                 r60 >> 4, r60 & 15, 15 - (r80 >> 4), r80 & 15,
                 (r20 & 0x80) ? "trem " : "", (r20 & 0x40) ? "vib " : "", (r20 & 0x20) ? "sus" : "");
        draw_text(x + 50, oy + 6, 1, RGB{ 200, 210, 245 }, ellipsize(line, w - 58, 1).c_str());
    }
    draw_text(x + 8, y + h - 14, 1, RGB{ 110, 120, 160 }, "click: mute   right-click: solo");
}

// The lyric line being sung, across the bottom of the visualizer.
void draw_lyrics(int ax, int ay, int aw, int ah)
{
    std::string ly = g_eng->lyric();
    if (ly.empty()) return;
    int s = aw > 900 ? 3 : 2;
    ly = ellipsize(ly, aw - 24, s);
    int th = FONT_H * s + 12, y = ay + ah - th - 4;
    fill(ax, y, aw, th, RGB{ 8, 8, 14 }, 215);
    draw_text(ax + (aw - text_w(ly.c_str(), s)) / 2, y + 6, s, RGB{ 255, 240, 170 }, ly.c_str());
}

void draw_toast(int W, int y)
{
    if (g_toast.empty() || (Sint32)(g_toast_until - SDL_GetTicks()) <= 0) return;
    std::string t = ellipsize(g_toast, W - 80, 2);
    int tw = text_w(t.c_str(), 2), x = (W - tw) / 2 - 12;
    fill(x, y, tw + 24, 28, RGB{ 30, 60, 100 }, 235);
    SDL_SetRenderDrawColor(ren, 110, 160, 230, 255);
    SDL_Rect b = { x, y, tw + 24, 28 }; SDL_RenderDrawRect(ren, &b);
    draw_text(x + 12, y + 7, 2, RGB{ 240, 245, 255 }, t.c_str());
}

// Topmost wins: whatever was drawn (registered) last is on top.
int hit_button(int x, int y)
{
    for (int i = g_nbtn - 1; i >= 0; --i)
        if (x >= g_btn[i].r.x && x < g_btn[i].r.x + g_btn[i].r.w &&
            y >= g_btn[i].r.y && y < g_btn[i].r.y + g_btn[i].r.h) return g_btn[i].act;
    return -1;
}

void handle_click(int x, int y, bool right)
{
    int a = hit_button(x, y);
    if (a < 0) return;
    if (g_find) close_find();   // a click ends the find
    if (g_modal == M_HELP) { close_modal(); return; }   // help: anything dismisses it
    g_click_x = x; g_click_y = y;
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
    std::string meta = g_eng->title();          // the title stored in the file, if any
    if (!meta.empty() && meta != title) title += "  |  " + meta;
    if (!g_eng->has_song()) { if (cur) title += "  -STOPPED-"; }
    else if (g_eng->paused()) title += "  -PAUSED-";
    draw_text(12, 6, 2, RGB{ 240, 240, 255 }, ellipsize(title, W - 36 - tw, 2).c_str());

    // Progress bar; the clickable strip is taller than the drawn one.
    g_seek = SDL_Rect{ 12, 24, W - 24, 14 };
    int mx, my; SDL_GetMouseState(&mx, &my);
    bool hover = g_drag == DRAG_SEEK || (g_eng->has_song() && mx >= g_seek.x && mx < g_seek.x + g_seek.w &&
                 my >= g_seek.y && my < g_seek.y + g_seek.h);
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
// the port dialog (RWPORT=dev opens it and submits dev as the custom path;
// empty just opens it), a playlist find (RWFIND=text) and/or a drag-and-drop
// (RWDROP=path), so headless screenshots can verify scrolling / output
// switching / muting / seeking / keyboard actions / port selection / find /
// drops. RWINSPECT=ch,x,y shows the channel inspector as if the mouse were
// there; RWMODAL=bank|help|export opens a dialog; RWEXPORT=vgm|wav|both runs an
// export; RWFULL=1 goes fullscreen.
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
    if (const char *md = getenv("RWMODAL")) {    // bank / help / export dialog
        if (!strcmp(md, "bank"))        open_bank_dialog();
        else if (!strcmp(md, "help"))   g_modal = M_HELP;
        else if (!strcmp(md, "export")) do_action(ACT_EXPORT);
    }
    if (const char *ex = getenv("RWEXPORT"))     // vgm / wav / both
        do_action(!strcmp(ex, "vgm") ? ACT_EXPORT_VGM : !strcmp(ex, "wav") ? ACT_EXPORT_WAV : ACT_EXPORT_BOTH);
    if (const char *hv = getenv("RWHOVER")) {    // park the mouse at "x,y"
        int hx = 0, hy = 0;
        if (sscanf(hv, "%d,%d", &hx, &hy) == 2) SDL_WarpMouseInWindow(g_win, hx, hy);
    }
    if (getenv("RWFULL")) set_fullviz(true);
    if (const char *ft = getenv("RWFIND")) {     // find-as-you-type for this text
        open_find();
        g_find_text = ft;
        find_from(0, +1);
    }
    if (const char *dp = getenv("RWDROP")) {     // as if this path were dropped
        g_dropped.push_back(dp);
        finish_drop();
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
bool gui_run(Engine &engine, Playlist &playlist, Config &cfg, bool autoplay)
{
    g_eng = &engine;
    g_pl  = &playlist;
    g_recurse = cfg.recurse;
    g_last_folder = cfg.last_folder;
    g_bank    = engine.bank();
    g_loopmode = (cfg.loop >= LOOP_OFF && cfg.loop <= LOOP_ALL) ? cfg.loop : LOOP_OFF;

    style = cfg.style;
    if (style < 0 || style >= STYLE_COUNT) style = STYLE_LED;

    // Restore the saved output mode if it's usable now, else fall back.
    int want = cfg.outmode;
    if (want < 0) want = engine.board_available() ? OUT_BOARD : OUT_PC;   // auto
    if (want == OUT_BOARD && !engine.board_available()) {
        want = OUT_PC;
        g_resume_board = true;   // it was the choice: take it up when it's plugged in
    }
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
    SDL_StopTextInput();   // only wanted while a text field is up

    if (autoplay && !playlist.empty() && !engine.paused())
        play_current();

    bool have_mpris = mpris_start();
    std::vector<MprisCommand> mpris_cmds;

    Uint32 last_reconnect_poll = SDL_GetTicks();
    while (!g_quit) {
        Uint32 frame_start = SDL_GetTicks();
        g_bank = engine.bank();   // a remembered bank may have come in with a new track
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) { g_quit = true; }
            else if (e.type == SDL_DROPFILE) {       // one per dropped file/folder
                if (e.drop.file) { g_dropped.push_back(e.drop.file); SDL_free(e.drop.file); }
            }
            else if (e.type == SDL_DROPCOMPLETE) finish_drop();
            else if (e.type == SDL_MOUSEBUTTONDOWN &&
                     (e.button.button == SDL_BUTTON_LEFT || e.button.button == SDL_BUTTON_RIGHT))
                handle_click(e.button.x, e.button.y, e.button.button == SDL_BUTTON_RIGHT);
            else if (e.type == SDL_MOUSEBUTTONUP) g_drag = DRAG_NONE;
            else if (e.type == SDL_MOUSEMOTION) {
                if (g_drag != DRAG_NONE) drag_update(e.motion.x, e.motion.y);
            }
            else if (g_modal != M_NONE) {   // modal: keys go to the dialog, nothing else
                if (e.type == SDL_MOUSEWHEEL && g_modal == M_BANK) {
                    int dir = (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) ? -1 : 1;
                    g_bank_scroll -= e.wheel.y * dir * 3;
                }
                else if (g_modal == M_PORT && (e.type == SDL_KEYDOWN || e.type == SDL_TEXTINPUT)) port_dialog_key(e);
                else if (g_modal == M_BANK && (e.type == SDL_KEYDOWN || e.type == SDL_TEXTINPUT)) bank_dialog_key(e);
                else if (e.type == SDL_KEYDOWN &&
                         (g_modal == M_HELP || e.key.keysym.sym == SDLK_ESCAPE)) close_modal();
            }
            else if (g_find && (e.type == SDL_KEYDOWN || e.type == SDL_TEXTINPUT)) find_key(e);
            else if (e.type == SDL_MOUSEWHEEL) {
                int dir = (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) ? -1 : 1;
                scroll_playlist(-e.wheel.y * dir * 3);
            }
            else if (e.type == SDL_KEYDOWN) {
                int page = g_visible_rows > 1 ? g_visible_rows - 1 : 1;
                bool shift = (e.key.keysym.mod & KMOD_SHIFT) != 0;
                switch (e.key.keysym.sym) {
                case SDLK_ESCAPE:   // leaves fullscreen first, quits otherwise
                    if (g_fullviz) set_fullviz(false); else g_quit = true;
                    break;
                case SDLK_q: g_quit = true; break;
                case SDLK_SPACE: case SDLK_AUDIOPLAY: do_action(ACT_PLAY); break;
                case SDLK_n: case SDLK_AUDIONEXT: do_action(ACT_NEXT); break;   // next / prev *track*
                case SDLK_p: case SDLK_AUDIOPREV: do_action(ACT_PREV); break;
                case SDLK_s: case SDLK_AUDIOSTOP: do_action(ACT_STOP); break;
                case SDLK_l: do_action(ACT_LOOP); break;
                case SDLK_h: do_action(ACT_SHUFFLE); break;
                case SDLK_v: do_action(ACT_STYLE); break;
                case SDLK_o: do_action(ACT_OUTPUT); break;
                case SDLK_d: g_text_open_ts = e.key.timestamp; do_action(ACT_PORT); break;
                case SDLK_b:
                    if (g_outmode != OUT_EXT) { g_text_open_ts = e.key.timestamp; open_bank_dialog(); }
                    break;
                case SDLK_a: do_action(ACT_BANK_AB); break;
                case SDLK_e: do_action(ACT_EXPORT); break;
                case SDLK_F1: do_action(ACT_HELP); break;
                case SDLK_F11: do_action(ACT_FULLVIZ); break;
                case SDLK_SLASH:
                    if (shift) { do_action(ACT_HELP); break; }   // '?'
                    g_text_open_ts = e.key.timestamp; open_find(); break;
                case SDLK_QUESTION: do_action(ACT_HELP); break;
                case SDLK_f: g_text_open_ts = e.key.timestamp; open_find(); break;
                case SDLK_DELETE: remove_hovered_row(); break;
                case SDLK_9: do_action(ACT_VDN); break;
                case SDLK_0: do_action(ACT_VUP); break;
                case SDLK_u: do_action(ACT_UNMUTE_ALL); break;
                // Bank (or MIDI device, in External mode) previous / next.
                case SDLK_LEFTBRACKET:
                    do_action(g_outmode == OUT_EXT ? ACT_MIDI_PREV : ACT_BANK_PREV); break;
                case SDLK_RIGHTBRACKET:
                    do_action(g_outmode == OUT_EXT ? ACT_MIDI_NEXT : ACT_BANK_NEXT); break;
                case SDLK_EQUALS: case SDLK_PLUS: case SDLK_KP_PLUS: do_action(ACT_TUP); break;
                case SDLK_MINUS: case SDLK_KP_MINUS: do_action(ACT_TDN); break;
                // Left/right seek; up/down & PgUp/PgDn scroll the playlist.
                case SDLK_LEFT:     seek_by(shift ? -30 : -5); break;
                case SDLK_RIGHT:    seek_by(shift ? +30 : +5); break;
                case SDLK_UP:       scroll_playlist(-1);    break;
                case SDLK_DOWN:     scroll_playlist(+1);    break;
                case SDLK_PAGEUP:   scroll_playlist(-page); break;
                case SDLK_PAGEDOWN: scroll_playlist(+page); break;
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
            g_resume_board = true;
            toast("Board lost - playing through the PC");
        }
        // ...and come back when it does (plugged back in, or in at last).
        if (frame_start - last_reconnect_poll >= 1000) {
            last_reconnect_poll = frame_start;
            if (engine.poll_board_reconnect() && g_resume_board) {
                fprintf(stderr, "gui: board is back — switching output to it\n");
                g_resume_board = false;
                g_outmode = OUT_BOARD;
                apply_output();
                toast("Board is back");
            }
        }

        // Desktop media controls (media keys, the desktop's player widget).
        if (have_mpris) {
            MprisState st;
            const Track *cur = playlist.current();
            st.status = !engine.has_song() ? MprisState::STOPPED
                      : engine.paused()    ? MprisState::PAUSED : MprisState::PLAYING;
            st.title    = cur ? cur->name : std::string();
            st.length   = engine.length();
            st.position = engine.position();
            st.volume   = engine.volume();
            st.can_go   = !playlist.empty();
            mpris_cmds.clear();
            mpris_update(st, mpris_cmds);
            for (const MprisCommand &c : mpris_cmds) {
                switch (c.type) {
                case MprisCommand::PLAY:
                    if (!engine.has_song()) play_current(); else engine.set_paused(false);
                    break;
                case MprisCommand::PAUSE:      engine.set_paused(true); break;
                case MprisCommand::PLAY_PAUSE: do_action(ACT_PLAY); break;
                case MprisCommand::STOP:       do_action(ACT_STOP); break;
                case MprisCommand::NEXT:       do_action(ACT_NEXT); break;
                case MprisCommand::PREVIOUS:   do_action(ACT_PREV); break;
                case MprisCommand::RAISE:      SDL_RaiseWindow(g_win); break;
                case MprisCommand::QUIT:       g_quit = true; break;
                case MprisCommand::SEEK_BY:    seek_by(c.value); break;
                case MprisCommand::SEEK_TO:    if (engine.has_song()) engine.seek(c.value); break;
                case MprisCommand::VOLUME:     engine.set_volume((float)c.value); break;
                }
            }
        }

        poll_pick_folder();
        static long frame = 0; ++frame;
        run_test_hooks(frame);

        viz_update();
        g_nbtn = 0;

        int W, Hh; SDL_GetRendererOutputSize(ren, &W, &Hh);
        int mx, my; SDL_GetMouseState(&mx, &my);
        fill(0, 0, W, Hh, RGB{ 12, 12, 20 });
        draw_header(W);

        // Layout: playlist panel on the right, visualizer fills the rest; in
        // fullscreen the visualizer has the window to itself.
        int ax = 16, ay = 50, aw = W - 32, ah = Hh - ay - 12 - 18;
        if (!g_fullviz) {
            build_controls(W);   // first: how many rows it needs sets the layout
            int panel_w = W / 3; if (panel_w < 260) panel_w = 260; if (panel_w > 380) panel_w = 380;
            int px = W - panel_w - 12, py = 48;
            int bottom = Hh - controls_height();     // above the control rows
            draw_playlist(px, py, panel_w, bottom - py, mx, my);
            aw = px - 16 - 16; ah = bottom - ay - 18;
        } else {
            g_hover_row = -1;
        }
        g_hover_chan = -1;
        if (aw > 40) {
            fill(ax - 5, ay - 5, aw + 10, ah + 10 + 18, RGB{ 8, 8, 14 });
            draw_viz(ax, ay, aw, ah);
            draw_lyrics(ax, ay, aw, ah + 18);
            if (uint32_t m = viz_mute_mask()) {      // only while something is muted
                char b[32]; snprintf(b, sizeof b, "UNMUTE %d", __builtin_popcount(m));
                add_button(ax + aw - btn_w(b), ay, CTRL_BH, b, ACT_UNMUTE_ALL, mx, my, true);
            }
        }

        if (!g_fullviz) draw_controls(Hh);
        draw_toast(W, 52);
        if (g_modal == M_NONE) draw_inspector(W, Hh);
        else if (g_modal == M_PORT)   draw_port_dialog(W, Hh);
        else if (g_modal == M_BANK)   draw_bank_dialog(W, Hh);
        else if (g_modal == M_HELP)   draw_help_dialog(W, Hh);
        else if (g_modal == M_EXPORT) draw_export_dialog(W, Hh);
        SDL_RenderPresent(ren);

        // An export blocks, so it runs here: "Exporting..." is on screen now.
        if (g_export_pending) {
            int kind = g_export_pending;
            g_export_pending = 0;
            run_export(kind);
        }

        maybe_screenshot(frame, W, Hh);

        // Present normally blocks for vsync. Where it doesn't (software
        // renderer, some hidden windows) don't spin a core: hold ~60 fps.
        Uint32 took = SDL_GetTicks() - frame_start;
        if (took < 15) SDL_Delay(15 - took);
    }
    mpris_stop();
    g_durations.stop();

    // Hand the user's final settings back for persistence.
    cfg.bank    = engine.default_bank();   // not a bank that came with one song
    cfg.recurse = g_recurse;
    cfg.loop    = g_loopmode;
    cfg.shuffle = playlist.shuffle();
    cfg.style   = style;
    // A board we're only waiting on is still the user's choice of output.
    cfg.outmode = (g_resume_board && g_outmode == OUT_PC) ? (int)OUT_BOARD : g_outmode;
    cfg.volume  = (int)lroundf(engine.volume() * 100.0f);
    cfg.last_folder = g_last_folder;
    if (!engine.gm_spec().empty()) cfg.midi_dev = engine.gm_spec();
    // Remember the board device (a dry run isn't a device worth remembering).
    if (engine.board_dev() != "-")
        cfg.serial_dev = engine.board_dev().empty() ? "none" : engine.board_dev();
    if (g_fullviz) { cfg.win_w = g_windowed_w; cfg.win_h = g_windowed_h; }   // not the screen's
    else           SDL_GetWindowSize(g_win, &cfg.win_w, &cfg.win_h);

    // Don't wait on an Open Folder dialog the user left open.
    if (g_pick_thread.joinable()) g_pick_thread.detach();

    if (ren)   SDL_DestroyRenderer(ren);
    if (g_win) SDL_DestroyWindow(g_win);
    ren = nullptr; g_win = nullptr;
    return true;
}
