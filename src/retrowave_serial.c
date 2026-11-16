/*
 * retrowave_serial.c — see retrowave_serial.h.
 *
 * Wire framing: each logical packet is 7-of-8 bit encoded and wrapped in a
 * [0x00 ... 0x02] frame (rw_pack).  Board address byte 0x42 (= 0x21 << 1)
 * selects the OPL3 board.  Register write, port 0:
 *   {0x42, 0x12, 0xe1, reg, 0xe3, val, 0xfb, val}
 * port 1:
 *   {0x42, 0x12, 0xe5, reg, 0xe7, val, 0xfb, val}
 * Reset: send {0x42,0x12,0xfe}, wait ~10 ms, send {0x42,0x12,0xff}, wait ~10 ms.
 */
// cfmakeraw(), CRTSCTS and B2000000 are GNU/BSD extensions, hidden under the
// project's strict -std=iso9899:1999.  Request them explicitly.
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif

#include "retrowave_serial.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

volatile bool retrowave_active = false;

static int rw_fd = -1;
static bool rw_dummy = false;  // dry-run: frame but don't write to hardware
static char rw_err[320] = "";  // why the last retrowave_open() failed

const char *retrowave_last_error(void)
{
	return rw_err;
}

// Record (and print) an open failure, and release the half-opened device.
static bool rw_open_fail(const char *what, const char *path)
{
	snprintf(rw_err, sizeof(rw_err), "%s %s: %s", what, path, strerror(errno));
	fprintf(stderr, "retrowave: %s\n", rw_err);
	if (rw_fd >= 0)
	{
		close(rw_fd);
		rw_fd = -1;
	}
	return false;
}

// Staging buffer for buffered mode (see retrowave_set_buffered).  It grows on
// demand: a burst (a long seek, a full register replay) must never fall back
// to writing from the staging thread, which would interleave with the writer
// thread's own writes and corrupt frames.  Past RW_STAGE_MAX (far more than
// any real burst) writes are dropped instead.
#define RW_STAGE_MAX ((size_t)8 << 20)
static bool     rw_buffered = false;
static uint8_t *rw_stage = NULL;
static size_t   rw_stage_cap = 0;
static size_t   rw_stage_len = 0;

static bool rw_stage_reserve(size_t need)
{
	if (need <= rw_stage_cap)
		return true;
	size_t cap = rw_stage_cap ? rw_stage_cap : 65536;
	while (cap < need)
		cap *= 2;
	if (cap > RW_STAGE_MAX)
		return false;
	uint8_t *nb = realloc(rw_stage, cap);
	if (nb == NULL)
		return false;
	rw_stage = nb;
	rw_stage_cap = cap;
	return true;
}

#define RW_BOARD_OPL3 0x42

// 7-of-8 bit pack: turn a logical packet into the wire frame [0x00 ... 0x02].
// out must hold at least len + len/7 + 4 bytes (64 is plenty for our packets).
static uint32_t rw_pack(const uint8_t *in, uint32_t len, uint8_t *out)
{
	uint32_t ic = 0, oc = 0;
	out[oc++] = 0x00;
	uint8_t shift = 0;
	while (ic < len)
	{
		uint8_t b = in[ic] >> shift;
		if (ic > 0)
			b |= in[ic - 1] << (8 - shift);
		b |= 0x01;
		out[oc++] = b;
		shift++; ic++;
		if (shift > 7) { shift = 0; ic--; }
	}
	if (shift)
		out[oc++] = (in[ic - 1] << (8 - shift)) | 0x01;
	out[oc++] = 0x02;
	return oc;
}

// Board lost: stop talking to it (once) so the caller can fall back to PC.
static void rw_fail(const char *why)
{
	if (retrowave_active)
		fprintf(stderr, "retrowave: board lost (%s) — board output disabled\n", why);
	retrowave_active = false;
}

bool retrowave_send(const uint8_t *buf, size_t len)
{
	if (!retrowave_active)
		return false;
	if (rw_dummy || rw_fd < 0)
		return true;

	size_t w = 0;
	while (w < len)
	{
		ssize_t rc = write(rw_fd, buf + w, len - w);
		if (rc > 0)
		{
			w += (size_t)rc;
			continue;
		}
		if (rc < 0 && errno == EINTR)
			continue;
		if (rc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
		{
			// USB buffer full: 2 Mbaud drains it quickly, so a long wait
			// means the device has wedged.
			struct pollfd pfd = { rw_fd, POLLOUT, 0 };
			int pr = poll(&pfd, 1, 500);
			if (pr > 0 && !(pfd.revents & (POLLERR | POLLHUP)))
				continue;
			rw_fail(pr == 0 ? "write timeout" : "device hung up");
			return false;
		}
		rw_fail(rc < 0 ? strerror(errno) : "short write");
		return false;
	}
	return true;
}

// Write a logical packet (gets framed via rw_pack before hitting the wire, or
// staged in buffered mode).
static void rw_raw(const uint8_t *buf, uint32_t len)
{
	uint8_t packed[64];
	uint32_t plen = rw_pack(buf, len, packed);
	if (rw_buffered)
	{
		if (rw_stage_reserve(rw_stage_len + plen))
		{
			memcpy(rw_stage + rw_stage_len, packed, plen);
			rw_stage_len += plen;
		}
		return;
	}
	retrowave_send(packed, plen);
}

void retrowave_set_buffered(bool on)
{
	rw_buffered = on;
}

size_t retrowave_take(uint8_t *out, size_t cap)
{
	size_t n = rw_stage_len < cap ? rw_stage_len : cap;
	if (n == 0)
		return 0;
	if (n < rw_stage_len)   // cut on a frame boundary (frames end with 0x02)
		while (n > 0 && rw_stage[n - 1] != 0x02)   // payload bytes all have bit0 set
			--n;
	memcpy(out, rw_stage, n);
	memmove(rw_stage, rw_stage + n, rw_stage_len - n);
	rw_stage_len -= n;
	return n;
}

void retrowave_discard(void)
{
	rw_stage_len = 0;
}

static void rw_sleep_ms(long ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
	nanosleep(&ts, NULL);
}

void retrowave_write(unsigned int idx, uint8_t val)
{
	if (!retrowave_active)
		return;

	uint8_t reg = (uint8_t)(idx & 0xff);
	if (idx & 0x100)
	{
		uint8_t b[] = { RW_BOARD_OPL3, 0x12, 0xe5, reg, 0xe7, val, 0xfb, val };
		rw_raw(b, sizeof(b));
	}
	else
	{
		uint8_t b[] = { RW_BOARD_OPL3, 0x12, 0xe1, reg, 0xe3, val, 0xfb, val };
		rw_raw(b, sizeof(b));
	}
}

void retrowave_reset(void)
{
	if (!retrowave_active)
		return;
	// Always sent directly (never staged): the sleeps are part of the reset.
	uint8_t lo[] = { RW_BOARD_OPL3, 0x12, 0xfe };
	uint8_t hi[] = { RW_BOARD_OPL3, 0x12, 0xff };
	uint8_t packed[16];
	retrowave_send(packed, rw_pack(lo, sizeof(lo), packed));
	rw_sleep_ms(10);
	retrowave_send(packed, rw_pack(hi, sizeof(hi), packed));
	rw_sleep_ms(10);
}

bool retrowave_open(const char *dev)
{
	if (dev == NULL || dev[0] == '\0')
		dev = "ttyACM0";
	rw_err[0] = '\0';

	if (strcmp(dev, "-") == 0)
	{
		rw_dummy = true;
		retrowave_active = true;
		fprintf(stderr, "retrowave: dry-run mode (no hardware)\n");
		return true;
	}

	char path[256];
	if (dev[0] == '/')   // any absolute path (e.g. a symlink elsewhere) as-is
		snprintf(path, sizeof(path), "%s", dev);
	else
		snprintf(path, sizeof(path), "/dev/%s", dev);

	// Non-blocking so a wedged device can't hang the writer (see retrowave_send).
	rw_fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (rw_fd < 0)
		return rw_open_fail("cannot open", path);
	// Exclusive: a second player (or anything else) opening this port now gets
	// EBUSY instead of interleaving its bytes with ours.  Released on close.
	ioctl(rw_fd, TIOCEXCL);

	struct termios tio;
	memset(&tio, 0, sizeof(tio));
	if (tcgetattr(rw_fd, &tio) != 0)   // e.g. ENOTTY: not a serial device
		return rw_open_fail("not a serial port?", path);
	cfmakeraw(&tio);
	tio.c_cflag |= (CLOCAL | CREAD);
	tio.c_cflag &= ~CRTSCTS;
	cfsetispeed(&tio, B2000000);  // 2 Mbaud — mandatory
	cfsetospeed(&tio, B2000000);
	if (tcsetattr(rw_fd, TCSANOW, &tio) != 0)
		return rw_open_fail("cannot set 2 Mbaud on", path);

	retrowave_active = true;
	retrowave_reset();
	if (!retrowave_active)   // the reset couldn't be written: wedged device
	{
		errno = EIO;
		return rw_open_fail("no response from", path);
	}
	fprintf(stderr, "retrowave: streaming OPL3 to %s @ 2000000 baud\n", path);
	return true;
}

void retrowave_close(void)
{
	if (rw_fd >= 0)
	{
		close(rw_fd);
		rw_fd = -1;
	}
	rw_dummy = false;
	rw_buffered = false;
	rw_stage_len = 0;
	retrowave_active = false;
}
