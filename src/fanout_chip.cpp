/*
 * fanout_chip.cpp — see fanout_chip.hpp.
 *
 * inject_fanout_chip() reaches into libADLMIDI's internals to swap its OPL3
 * chip for a FanoutOPL3, mirroring the exact per-chip setup libADLMIDI itself
 * does in OPL3::reset() (setChipId / setRate / setRunningAtPcmRate / initChip /
 * updateChannelCategories / silenceAll) so the replacement is indistinguishable
 * to the sequencer. This is the same technique SudoMaker's midi2vgm uses.
 */
#include "fanout_chip.hpp"

// libADLMIDI internals (from libADLMIDI/src, added to the include path).
#include <adlmidi_private.hpp>
#include <adlmidi_midiplay.hpp>
#include <adlmidi_opl3.hpp>

std::atomic<bool> g_board_out{true};
std::atomic<bool> g_pc_out{true};
std::atomic<bool> g_gm_out{false};
volatile uint8_t  g_opl_regs[512] = {0};
std::atomic<uint32_t> g_opl_mute{0};
std::atomic<bool>     g_opl_hold{false};

uint8_t fanout_mask(uint16_t addr, uint8_t data)
{
    uint32_t mute = g_opl_hold.load(std::memory_order_relaxed)
                  ? 0x3FFFFu : g_opl_mute.load(std::memory_order_relaxed);
    if (!mute) return data;
    unsigned lo = addr & 0xFF, set = addr >> 8;
    if (lo >= 0xB0 && lo <= 0xB8) {                 // key-on lives in bit 5
        if (mute & (1u << (set * 9 + (lo - 0xB0)))) data &= ~0x20;
    } else if (addr == 0xBD && (data & 0x20)) {     // rhythm mode: drum key bits
        if (mute & (1u << 6)) data &= ~0x10;        // ch6: bass drum
        if (mute & (1u << 7)) data &= ~0x09;        // ch7: snare + hi-hat
        if (mute & (1u << 8)) data &= ~0x06;        // ch8: tom + cymbal
    }
    return data;
}

void fanout_send(uint16_t addr, uint8_t data)
{
    if (retrowave_active && g_board_out.load(std::memory_order_relaxed))
        retrowave_write(addr, data);
    adlib_write(addr, data);
}

void fanout_rekey()
{
    for (unsigned set = 0; set < 2; ++set)
        for (unsigned c = 0; c < 9; ++c) {
            uint16_t a = (uint16_t)(set * 0x100 + 0xB0 + c);
            fanout_send(a, fanout_mask(a, g_opl_regs[a]));
        }
    fanout_send(0xBD, fanout_mask(0xBD, g_opl_regs[0xBD]));
}

void fanout_replay_to_board()
{
    if (!retrowave_active) return;
    auto put = [](uint16_t a) { retrowave_write(a, fanout_mask(a, g_opl_regs[a])); };

    put(0x105);                 // OPL3 mode enable first (else set 2 is ignored)
    put(0x104);                 // 4-op connection select
    put(0x001); put(0x008);     // waveform-select enable, CSM/note-sel
    for (unsigned set = 0; set < 2; ++set) {
        uint16_t b = (uint16_t)(set * 0x100);
        static const uint8_t opbase[] = { 0x20, 0x40, 0x60, 0x80, 0xE0 };
        for (uint8_t base : opbase)
            for (unsigned o = 0; o < 0x16; ++o) {
                if ((o & 7) >= 6) continue;          // gaps at 0x06/07, 0x0E/0F
                put((uint16_t)(b + base + o));
            }
        for (unsigned c = 0; c < 9; ++c) put((uint16_t)(b + 0xA0 + c));
        for (unsigned c = 0; c < 9; ++c) put((uint16_t)(b + 0xC0 + c));
    }
    put(0x0BD);                 // tremolo/vibrato depth + rhythm
    for (unsigned set = 0; set < 2; ++set)     // key-on last
        for (unsigned c = 0; c < 9; ++c) put((uint16_t)(set * 0x100 + 0xB0 + c));
}

void fanout_clear_keys()
{
    for (unsigned set = 0; set < 2; ++set)
        for (unsigned c = 0; c < 9; ++c) g_opl_regs[set * 0x100 + 0xB0 + c] &= ~0x20;
    g_opl_regs[0xBD] &= ~0x1F;
}

bool inject_fanout_chip(ADL_MIDIPlayer *player, unsigned sample_rate)
{
    if(!player || !player->adl_midiPlayer)
        return false;

    MIDIplay *mp = reinterpret_cast<MIDIplay *>(player->adl_midiPlayer);
    Synth *synth = mp->m_synth.get();
    if(!synth || synth->m_numChips != 1 || synth->m_chips.size != 1)
        return false;

    FanoutOPL3 *chip = new FanoutOPL3();
    chip->setChipId(0);
    chip->setRate(sample_rate);
    if(synth->m_runAtPcmRate)
        chip->setRunningAtPcmRate(true);

    // Hand ownership to libADLMIDI (frees the previous chip; frees ours later).
    synth->m_chips[0].reset(chip);

    // Re-run the initialisation libADLMIDI applies to a freshly created chip so
    // the OPL3 mode/timer/waveform registers are written (to the board too, via
    // FanoutOPL3::writeReg) and the panning/chip-type bookkeeping is refreshed.
    synth->initChip(0);
    synth->updateChannelCategories();
    synth->silenceAll();
    return true;
}
