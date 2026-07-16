/*
 * alsamidi.cpp — see alsamidi.hpp.
 */
#include "alsamidi.hpp"

#include <alsa/asoundlib.h>
#include <cstdio>
#include <cstring>

bool AlsaMidi::open(const std::string &port)
{
    close();

    snd_seq_t *seq = nullptr;
    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_OUTPUT, 0) < 0) {
        fprintf(stderr, "alsamidi: cannot open ALSA sequencer\n");
        return false;
    }
    snd_seq_set_client_name(seq, "OPL3 RetroWave MIDI Player");

    int myport = snd_seq_create_simple_port(seq, "Output",
        SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    if (myport < 0) {
        fprintf(stderr, "alsamidi: cannot create sequencer port\n");
        snd_seq_close(seq);
        return false;
    }

    // Resolve the destination.
    snd_seq_addr_t dest;
    if (!port.empty()) {
        if (snd_seq_parse_address(seq, &dest, port.c_str()) < 0) {
            fprintf(stderr, "alsamidi: bad MIDI port '%s' (try --list-midi)\n", port.c_str());
            snd_seq_close(seq);
            return false;
        }
    } else {
        // Auto-pick: first WRITE-capable port that isn't ours, the system
        // client, or "Midi Through".
        bool found = false;
        snd_seq_client_info_t *cinfo; snd_seq_client_info_alloca(&cinfo);
        snd_seq_port_info_t   *pinfo; snd_seq_port_info_alloca(&pinfo);
        int me = snd_seq_client_id(seq);
        snd_seq_client_info_set_client(cinfo, -1);
        while (!found && snd_seq_query_next_client(seq, cinfo) >= 0) {
            int client = snd_seq_client_info_get_client(cinfo);
            if (client == me || client == SND_SEQ_CLIENT_SYSTEM) continue;
            if (strstr(snd_seq_client_info_get_name(cinfo), "Through")) continue;
            snd_seq_port_info_set_client(pinfo, client);
            snd_seq_port_info_set_port(pinfo, -1);
            while (snd_seq_query_next_port(seq, pinfo) >= 0) {
                unsigned caps = snd_seq_port_info_get_capability(pinfo);
                if ((caps & SND_SEQ_PORT_CAP_WRITE) && !(caps & SND_SEQ_PORT_CAP_NO_EXPORT)) {
                    dest.client = client;
                    dest.port   = snd_seq_port_info_get_port(pinfo);
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            fprintf(stderr, "alsamidi: no external MIDI port found (try --list-midi / -m)\n");
            snd_seq_close(seq);
            return false;
        }
    }

    // Subscribe our source port to the destination.
    snd_seq_addr_t sender; sender.client = snd_seq_client_id(seq); sender.port = myport;
    snd_seq_port_subscribe_t *sub; snd_seq_port_subscribe_alloca(&sub);
    snd_seq_port_subscribe_set_sender(sub, &sender);
    snd_seq_port_subscribe_set_dest(sub, &dest);
    if (snd_seq_subscribe_port(seq, sub) < 0) {
        fprintf(stderr, "alsamidi: cannot connect to %d:%d\n", dest.client, dest.port);
        snd_seq_close(seq);
        return false;
    }

    // Cache a human-readable destination name.
    snd_seq_client_info_t *ci; snd_seq_client_info_alloca(&ci);
    m_dest_name.clear();
    if (snd_seq_get_any_client_info(seq, dest.client, ci) >= 0)
        m_dest_name = snd_seq_client_info_get_name(ci);
    char tail[16]; snprintf(tail, sizeof tail, " %d:%d", dest.client, dest.port);
    m_dest_name += tail;

    m_seq = seq; m_port = myport; m_dst_client = dest.client; m_dst_port = dest.port;
    fprintf(stderr, "alsamidi: connected to %s\n", m_dest_name.c_str());
    return true;
}

void AlsaMidi::close()
{
    if (m_seq) { panic(); snd_seq_close(m_seq); }
    m_seq = nullptr; m_port = -1; m_dst_client = m_dst_port = -1;
    m_dest_name.clear();
}

void AlsaMidi::send(uint8_t type, uint8_t channel, uint8_t d0, uint8_t d1)
{
    if (!m_seq) return;

    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_source(&ev, m_port);
    snd_seq_ev_set_subs(&ev);
    snd_seq_ev_set_direct(&ev);

    int ch = channel & 0x0f;
    switch (type) {
    case 0x9: snd_seq_ev_set_noteon(&ev, ch, d0, d1);   break;
    case 0x8: snd_seq_ev_set_noteoff(&ev, ch, d0, d1);  break;
    case 0xA: snd_seq_ev_set_keypress(&ev, ch, d0, d1); break;
    case 0xB: snd_seq_ev_set_controller(&ev, ch, d0, d1); break;
    case 0xC: snd_seq_ev_set_pgmchange(&ev, ch, d0);    break;
    case 0xD: snd_seq_ev_set_chanpress(&ev, ch, d0);    break;
    case 0xE: snd_seq_ev_set_pitchbend(&ev, ch, (int)(d0 | (d1 << 7)) - 8192); break;
    default:  return;
    }
    snd_seq_event_output_direct(m_seq, &ev);
}

void AlsaMidi::panic()
{
    if (!m_seq) return;
    for (int ch = 0; ch < 16; ++ch) {
        send(0xB, ch, 120, 0);   // all sound off
        send(0xB, ch, 123, 0);   // all notes off
        send(0xB, ch, 121, 0);   // reset all controllers
    }
    snd_seq_drain_output(m_seq);
}

std::string AlsaMidi::dest_addr() const
{
    if (!m_seq) return "";
    char a[16]; snprintf(a, sizeof a, "%d:%d", m_dst_client, m_dst_port);
    return a;
}

std::vector<MidiPort> AlsaMidi::enumerate()
{
    std::vector<MidiPort> out;
    snd_seq_t *seq = nullptr;
    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_OUTPUT, 0) < 0)
        return out;
    int me = snd_seq_client_id(seq);
    snd_seq_client_info_t *cinfo; snd_seq_client_info_alloca(&cinfo);
    snd_seq_port_info_t   *pinfo; snd_seq_port_info_alloca(&pinfo);
    snd_seq_client_info_set_client(cinfo, -1);
    while (snd_seq_query_next_client(seq, cinfo) >= 0) {
        int client = snd_seq_client_info_get_client(cinfo);
        if (client == me || client == SND_SEQ_CLIENT_SYSTEM) continue;
        const char *cname = snd_seq_client_info_get_name(cinfo);
        if (strstr(cname, "Through")) continue;
        snd_seq_port_info_set_client(pinfo, client);
        snd_seq_port_info_set_port(pinfo, -1);
        while (snd_seq_query_next_port(seq, pinfo) >= 0) {
            unsigned caps = snd_seq_port_info_get_capability(pinfo);
            if ((caps & SND_SEQ_PORT_CAP_WRITE) && !(caps & SND_SEQ_PORT_CAP_NO_EXPORT)) {
                int p = snd_seq_port_info_get_port(pinfo);
                char addr[16]; snprintf(addr, sizeof addr, "%d:%d", client, p);
                out.push_back(MidiPort{ addr, std::string(cname) + " " + addr });
            }
        }
    }
    snd_seq_close(seq);
    return out;
}

bool AlsaMidi::any_available()
{
    snd_seq_t *seq = nullptr;
    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_OUTPUT, 0) < 0)
        return false;
    int me = snd_seq_client_id(seq);
    bool found = false;
    snd_seq_client_info_t *cinfo; snd_seq_client_info_alloca(&cinfo);
    snd_seq_port_info_t   *pinfo; snd_seq_port_info_alloca(&pinfo);
    snd_seq_client_info_set_client(cinfo, -1);
    while (!found && snd_seq_query_next_client(seq, cinfo) >= 0) {
        int client = snd_seq_client_info_get_client(cinfo);
        if (client == me || client == SND_SEQ_CLIENT_SYSTEM) continue;
        if (strstr(snd_seq_client_info_get_name(cinfo), "Through")) continue;
        snd_seq_port_info_set_client(pinfo, client);
        snd_seq_port_info_set_port(pinfo, -1);
        while (snd_seq_query_next_port(seq, pinfo) >= 0) {
            unsigned caps = snd_seq_port_info_get_capability(pinfo);
            if ((caps & SND_SEQ_PORT_CAP_WRITE) && !(caps & SND_SEQ_PORT_CAP_NO_EXPORT)) {
                found = true; break;
            }
        }
    }
    snd_seq_close(seq);
    return found;
}

void AlsaMidi::list_ports()
{
    snd_seq_t *seq = nullptr;
    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_OUTPUT, 0) < 0) {
        fprintf(stderr, "alsamidi: cannot open ALSA sequencer\n");
        return;
    }
    printf("Available ALSA MIDI output ports (use with -m):\n");
    printf("  %-7s %-30s %s\n", "Port", "Client Name", "Port Name");
    printf("  %-7s %-30s %s\n", "-------", "------------------------------", "----------");

    snd_seq_client_info_t *cinfo; snd_seq_client_info_alloca(&cinfo);
    snd_seq_port_info_t   *pinfo; snd_seq_port_info_alloca(&pinfo);
    snd_seq_client_info_set_client(cinfo, -1);
    while (snd_seq_query_next_client(seq, cinfo) >= 0) {
        int client = snd_seq_client_info_get_client(cinfo);
        snd_seq_port_info_set_client(pinfo, client);
        snd_seq_port_info_set_port(pinfo, -1);
        while (snd_seq_query_next_port(seq, pinfo) >= 0) {
            unsigned caps = snd_seq_port_info_get_capability(pinfo);
            if ((caps & SND_SEQ_PORT_CAP_WRITE) && !(caps & SND_SEQ_PORT_CAP_NO_EXPORT)) {
                char addr[16];
                snprintf(addr, sizeof addr, "%d:%d", client, snd_seq_port_info_get_port(pinfo));
                printf("  %-7s %-30s %s\n", addr,
                       snd_seq_client_info_get_name(cinfo),
                       snd_seq_port_info_get_name(pinfo));
            }
        }
    }
    snd_seq_close(seq);
}
