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
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

bool retrowave_active = false;

static int rw_fd = -1;
static bool rw_dummy = false;  // dry-run: frame but don't write to hardware

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

// Write a logical packet (gets framed via rw_pack before hitting the wire).
static void rw_raw(const uint8_t *buf, uint32_t len)
{
	if (rw_dummy || rw_fd < 0)
		return;

	uint8_t packed[64];
	uint32_t plen = rw_pack(buf, len, packed);
	uint32_t w = 0;
	while (w < plen)
	{
		ssize_t rc = write(rw_fd, packed + w, plen - w);
		if (rc > 0)
			w += (uint32_t)rc;
		else if (rc < 0 && errno == EINTR)
			continue;
		else
		{
			fprintf(stderr, "retrowave: serial write failed: %s\n", strerror(errno));
			return;
		}
	}
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

	uint8_t lo[] = { RW_BOARD_OPL3, 0x12, 0xfe };
	rw_raw(lo, sizeof(lo));
	rw_sleep_ms(10);
	uint8_t hi[] = { RW_BOARD_OPL3, 0x12, 0xff };
	rw_raw(hi, sizeof(hi));
	rw_sleep_ms(10);
}

bool retrowave_open(const char *dev)
{
	if (dev == NULL || dev[0] == '\0')
		dev = "ttyACM0";

	if (strcmp(dev, "-") == 0)
	{
		rw_dummy = true;
		retrowave_active = true;
		fprintf(stderr, "retrowave: dry-run mode (no hardware)\n");
		return true;
	}

	char path[256];
	if (strncmp(dev, "/dev/", 5) == 0)
		snprintf(path, sizeof(path), "%s", dev);
	else
		snprintf(path, sizeof(path), "/dev/%s", dev);

	rw_fd = open(path, O_RDWR | O_NOCTTY);
	if (rw_fd < 0)
	{
		fprintf(stderr, "retrowave: cannot open %s: %s\n", path, strerror(errno));
		return false;
	}

	struct termios tio;
	memset(&tio, 0, sizeof(tio));
	if (tcgetattr(rw_fd, &tio) != 0)
	{
		fprintf(stderr, "retrowave: tcgetattr failed: %s\n", strerror(errno));
		close(rw_fd);
		rw_fd = -1;
		return false;
	}
	cfmakeraw(&tio);
	tio.c_cflag |= (CLOCAL | CREAD);
	tio.c_cflag &= ~CRTSCTS;
	cfsetispeed(&tio, B2000000);  // 2 Mbaud — mandatory
	cfsetospeed(&tio, B2000000);
	if (tcsetattr(rw_fd, TCSANOW, &tio) != 0)
	{
		fprintf(stderr, "retrowave: tcsetattr failed: %s\n", strerror(errno));
		close(rw_fd);
		rw_fd = -1;
		return false;
	}

	retrowave_active = true;
	retrowave_reset();
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
	retrowave_active = false;
}
