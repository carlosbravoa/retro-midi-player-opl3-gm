/*
 * fanout_chip.hpp — a libADLMIDI OPL3 "chip" that fans register writes out to
 * both a real RetroWave OPL3 board and a software DOSBox OPL3 emulator.
 *
 * libADLMIDI is the sequencer and FM voice allocator: it turns a Standard MIDI
 * File into a stream of YMF262 register writes. Normally those writes drive an
 * internal software emulator. We replace the emulator with this class (see
 * inject_fanout_chip) so that every register write is simultaneously:
 *
 *   1. framed and streamed to the RetroWave OPL3 Express over USB-serial
 *      (retrowave_serial.*), when board output is enabled and a board is open;
 *   2. applied to an embedded DOSBox OPL3 emulator (emu/opl.c), which produces
 *      the PC-audio samples and the per-channel levels the visualizer reads.
 *
 * Because there is exactly one register stream feeding both sinks, the board
 * and the PC audio are sample-accurately in sync. The whole thing is clocked
 * by libADLMIDI's adl_play() (called from the SDL audio callback): each frame
 * it renders advances the sequencer, fires these writes, and returns one
 * emulator frame for the speakers.
 *
 * midi2vgm (SudoMaker, AGPLv3) demonstrated this custom-OPLChipBase technique;
 * this reuses it to target real hardware + local audio instead of a VGM file.
 */
#ifndef FANOUT_CHIP_HPP
#define FANOUT_CHIP_HPP

#include <atomic>
#include <cstdint>

// libADLMIDI internal API (headers live in libADLMIDI/src, not the install).
#include <adlmidi.h>
#include <chips/opl_chip_base.h>

extern "C" {
#include "emu/opl.h"
#include "retrowave_serial.h"
}

// Runtime output routing, flipped by the GUI/engine at any time.
//   g_board_out : forward register writes to the RetroWave board.
//   g_pc_out    : (read by the audio callback) send emulator audio to speakers.
// Board writes additionally require retrowave_active (a board is actually open).
extern std::atomic<bool> g_board_out;
extern std::atomic<bool> g_pc_out;
// External MIDI output is routed from libADLMIDI's raw-event hook (Engine),
// not from this chip; the flag lives here with the other routing flags.
extern std::atomic<bool> g_gm_out;

// Shadow of the last value written to each OPL3 register (two register sets,
// 0x000-0x1FF). Updated on every writeReg so the GUI can read note pitch/block
// for the visualizer without tapping the stream. Benign cross-thread reads.
extern volatile uint8_t g_opl_regs[512];

class FanoutOPL3 final : public OPLChipBaseT<FanoutOPL3>
{
public:
    FanoutOPL3() { }
    ~FanoutOPL3() override { }

    bool canRunAtPcmRate() const override { return true; }

    // (Re)initialise the embedded DOSBox emulator whenever libADLMIDI sets the
    // output rate. effectiveRate() is the rate our nativeGenerate() must match.
    void setRate(uint32_t rate) override
    {
        OPLChipBaseT<FanoutOPL3>::setRate(rate);
        adlib_init(effectiveRate());
    }

    void reset() override
    {
        OPLChipBaseT<FanoutOPL3>::reset();
        adlib_init(effectiveRate());
        // libADLMIDI's own reset writes a full silence-all register sequence
        // through writeReg() below, so the board is reset via that same path.
    }

    // The heart of the fan-out: one register write, two sinks.
    void writeReg(uint16_t addr, uint8_t data) override
    {
        if (addr < 512) g_opl_regs[addr] = data;
        if (retrowave_active && g_board_out.load(std::memory_order_relaxed))
            retrowave_write(addr, data);
        adlib_write(addr, data);
    }

    void nativePreGenerate()  override { }
    void nativePostGenerate() override { }

    // One stereo frame (L,R) from the DOSBox tee -> PC speakers.
    void nativeGenerate(int16_t *frame) override
    {
        adlib_getsample(frame, 1);
    }

    const char *emulatorName() override { return "RetroWave OPL3 (DOSBox tee)"; }
    ChipType    chipType()     override { return CHIPTYPE_OPL3; }

    // Real OPL3 only has hardware L/C/R panning via the Cx register bits, which
    // libADLMIDI writes for us — it does not support fine soft-panning on
    // hardware, so report no full panning and leave soft-pan disabled.
    bool hasFullPanning() override { return false; }
};

// Swap libADLMIDI's internal chip for a FanoutOPL3. Call once, after
// adl_setBank()/adl_setNumChips(1) and before playback. sample_rate must equal
// the value passed to adl_init(). Returns false if the player isn't in the
// expected single-chip state. The FanoutOPL3 is owned by libADLMIDI afterwards
// (freed on adl_close()).
bool inject_fanout_chip(ADL_MIDIPlayer *player, unsigned sample_rate);

#endif /* FANOUT_CHIP_HPP */
