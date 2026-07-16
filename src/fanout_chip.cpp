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
