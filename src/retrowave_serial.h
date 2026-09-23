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
#include <stddef.h>
#include <stdint.h>

// When true, OPL register writes are mirrored to the board.  Set by a
// successful retrowave_open(); cleared by retrowave_close() or when the device
// fails (e.g. unplugged mid-song) — poll it to notice a lost board.
extern volatile bool retrowave_active;

// Open the board.  dev may be given with or without the "/dev/" prefix; pass
// "-" for a dry run (frames are discarded, useful for testing without hardware).
// Performs a full chip reset on success.  Returns true on success; on failure
// prints an error, leaves retrowave_active false, and returns false.
bool retrowave_open(const char *dev);

// One OPL register write.  idx 0x000-0x0FF -> first register set (port 0);
// idx 0x100-0x1FF -> second register set (port 1).  No-op if not active.
// Unbuffered (default): framed and written to the device immediately.
// Buffered: the framed bytes are appended to a staging buffer instead, for the
// caller to collect with retrowave_take() and send later (paced) with
// retrowave_send().  The staging buffer is not thread-safe: the caller
// serialises retrowave_write / retrowave_take.
void retrowave_write(unsigned int idx, uint8_t val);

void retrowave_set_buffered(bool on);

// Move up to `cap` staged bytes (whole frames only) into `out`; returns the
// count.  Call until it returns 0 to drain everything.
size_t retrowave_take(uint8_t *out, size_t cap);

// Discard anything staged but not yet taken.
void retrowave_discard(void);

// Write already-framed bytes to the device.  Waits for a short while if the
// USB-serial buffer is full; on a hard error or timeout (board unplugged or
// wedged) prints once, clears retrowave_active and returns false.
bool retrowave_send(const uint8_t *buf, size_t len);

// Full chip reset (0xfe, settle, 0xff, settle).  No-op if not active.
void retrowave_reset(void);

// Close the device and clear retrowave_active.
void retrowave_close(void);

#endif /* RETROWAVE_SERIAL_H */
