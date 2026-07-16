/*
 * alsamidi.hpp — send the original MIDI event stream to an external MIDI device
 * via the ALSA sequencer (e.g. a hardware General MIDI module on a USB-MIDI
 * cable).
 *
 * This is a *different kind of output* from the OPL3 board and PC audio: the
 * board and PC play libADLMIDI's FM synthesis, whereas this forwards the raw
 * MIDI note/program/controller events so an external synth plays them with its
 * own voices. The events come from libADLMIDI's raw-event hook (see
 * Engine::on_raw_event), so they stay on the same playback clock.
 *
 * ALSA sequencer usage adapted from the sibling descent-hmp-player/hmpplay.cpp.
 */
#ifndef ALSAMIDI_HPP
#define ALSAMIDI_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct _snd_seq;   // opaque; avoids leaking <alsa/asoundlib.h> into callers

// One selectable external MIDI destination.
struct MidiPort {
    std::string addr;   // ALSA address "client:port" (pass to AlsaMidi::open)
    std::string name;   // human-readable "Client — Port  client:port"
};

class AlsaMidi {
public:
    // Open the sequencer and connect to `port`. `port` accepts ALSA address
    // syntax ("20:0" or a client name). If empty, auto-connect to the first
    // external MIDI destination (skipping "Midi Through" and system ports).
    // Returns false (and stays closed) on any failure.
    bool open(const std::string &port);
    void close();
    bool is_open() const { return m_seq != nullptr; }
    const std::string &dest_name() const { return m_dest_name; }
    std::string dest_addr() const;   // "client:port" of the connection, or ""

    // Forward one channel-voice event. `type` is the MIDI status high nibble
    // (0x8 note-off … 0xE pitch-wheel); `channel` 0–15; d0/d1 the data bytes
    // (d1 ignored for 1-byte messages). No-op when closed.
    void send(uint8_t type, uint8_t channel, uint8_t d0, uint8_t d1);

    // All-sound-off + all-notes-off + reset-controllers on every channel, to
    // clear hanging notes on stop / track change / when switching away.
    void panic();

    // Print available MIDI destination ports (like `aplaymidi -l`).
    static void list_ports();

    // True if the system currently has at least one external MIDI destination
    // (excludes "Midi Through" and system ports). Lets the GUI offer the
    // External output and connect on demand, with no -m flag.
    static bool any_available();

    // Enumerate selectable external MIDI destinations (same filter as
    // any_available). For the GUI device picker.
    static std::vector<MidiPort> enumerate();

private:
    _snd_seq   *m_seq  = nullptr;
    int         m_port = -1;      // our source port id
    int         m_dst_client = -1;
    int         m_dst_port   = -1;
    std::string m_dest_name;
};

#endif /* ALSAMIDI_HPP */
