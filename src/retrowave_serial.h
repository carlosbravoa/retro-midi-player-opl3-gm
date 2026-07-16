/*
 * retrowave_serial.h — Stream OPL register writes to a RetroWave OPL3 Express.
 *
 * The board is a real Yamaha YMF262 (OPL3) on a USB-serial bridge, default
 * /dev/ttyACM0 at 2,000,000 baud.  You stream OPL register writes to it; it is
 * an FM chip, not a MIDI synth.  Tyrian's music is OPL2 (single register set),
 * so every write goes to the first register set (port 0).
 *
 * This is a C port of the framing used by the RetroWave reference players
 * (hmpplay_opl3, droplay, hmisandbox).  2 Mbaud is mandatory — a wrong baud
 * rate fails silently (garbled / no sound).
 *
 * Protocol: SudoMaker RetroWave (AGPLv3).  Keep that attribution if you ship.
 */
#ifndef RETROWAVE_SERIAL_H
#define RETROWAVE_SERIAL_H

#include <stdbool.h>
#include <stdint.h>

// When true, OPL register writes are mirrored to the board.  Set by a
// successful retrowave_open(); checked on the hot path in adlib_write().
extern bool retrowave_active;

// Open the board.  dev may be given with or without the "/dev/" prefix; pass
// "-" for a dry run (frames are discarded, useful for testing without hardware).
// Performs a full chip reset on success.  Returns true on success; on failure
// prints an error, leaves retrowave_active false, and returns false.
bool retrowave_open(const char *dev);

// One OPL register write.  idx 0x000-0x0FF -> first register set (port 0);
// idx 0x100-0x1FF -> second register set (port 1).  No-op if not active.
void retrowave_write(unsigned int idx, uint8_t val);

// Full chip reset (0xfe, settle, 0xff, settle).  No-op if not active.
void retrowave_reset(void);

// Close the device and clear retrowave_active.
void retrowave_close(void);

#endif /* RETROWAVE_SERIAL_H */
