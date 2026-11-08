/*
 * Host-side test harness for scriba's ch347_spi.c: a fake libusb that
 * emulates a CH347 at the USB packet level (512-byte high-speed bulk
 * packets, transfers end on a short packet or when the request is filled,
 * OVERFLOW when a packet exceeds the space left, TIMEOUT when the host waits
 * for data the device never sends, no ZLPs).
 *
 * The device frames read data at a configurable size, so one driver build is
 * checked against 510-byte frames (2022 firmware), 4096-byte frames (WCH's
 * Linux driver) and 1024-byte frames (exact multiples of the packet size).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libusb-1.0/libusb.h>

#include "spi_controller.h"

int debug_enabled = 0;
int trace_enabled = 0;

extern const struct spi_programmer ch347_programmer;

/* ---- fake device state ---- */
#define PKT 512
#define QMAX (1 << 16)

static struct { uint16_t len; uint8_t d[PKT]; } q[QMAX];
static int qh, qt;                 /* packet queue head/tail */
static int frame_payload = 507;    /* data bytes per read frame */
static int cs_low;
static int cfg_div = -1;
static uint8_t miso_next;          /* MISO stream: 0,1,2,... */
static uint8_t mosi_log[1 << 20];
static unsigned mosi_len;
static int stall_seen, overflow_seen, protocol_err;
static struct { int out, in, c1, c2, c3, c4, c2_cs_high; } cnt;   /* USB traffic counters */
static int fail_next_status;                          /* inject a bad C4 status */

/* Firmware models */
static int fw_first_only;   /* run only the first command of each OUT transfer */
static int fw_pack;         /* pack the replies of one OUT transfer into shared packets */
static int chip_present = 1;
static unsigned fw_version = 0x0544;   /* reported by the 0xCA chip-info read */
static int fw_echo_flags;              /* echo C2 CS flags in the reply header */
#define FW_HAS_CS_FLAGS (fw_version >= 0x0341)
static const uint8_t chip_id[3] = { 0xEF, 0x40, 0x18 };
static int pack_from = -1;  /* first queue slot this OUT transfer may pack into */

/* Current SPI transaction (CS low): first MOSI byte and bytes clocked */
static int tx_op = -1;
static unsigned tx_pos;

static uint8_t miso_at(void)
{
	unsigned p = tx_pos++;
	if (tx_op == 0x9F)
		return !chip_present ? 0xFF : p == 0 ? 0xFF : p <= 3 ? chip_id[p - 1] : 0x00;
	return miso_next++;
}

static void mosi_byte(uint8_t v)
{
	if (tx_pos == 0)
		tx_op = v;
}

static void queue_bytes(const uint8_t *b, int n)
{
	/* Split one device frame into packets; a frame that is a multiple of
	 * PKT ends on a full packet (no ZLP), the hard case for the host. With
	 * fw_pack, replies of the same OUT transfer share packets. */
	while (n > 0) {
		int last = (qt + QMAX - 1) % QMAX;
		if (fw_pack && pack_from >= 0 && qt != pack_from && q[last].len < PKT && qh != qt) {
			int c = PKT - q[last].len < n ? PKT - q[last].len : n;
			memcpy(q[last].d + q[last].len, b, c);
			q[last].len += c;
			b += c;
			n -= c;
			continue;
		}
		int c = n > PKT ? PKT : n;
		q[qt].len = c;
		memcpy(q[qt].d, b, c);
		qt = (qt + 1) % QMAX;
		b += c;
		n -= c;
	}
}

static void queue_status(uint8_t cmd)
{
	uint8_t s[4] = { cmd, 1, 0, 0 };
	queue_bytes(s, 4);
}

static void device_cmd(const uint8_t *b, unsigned len);

/* One OUT transfer: one or more commands back to back */
static void device_out(const uint8_t *b, int n)
{
	if (n > 510) { fprintf(stderr, "mock: OUT transfer of %d bytes exceeds 510\n", n); protocol_err++; }
	pack_from = qt;
	int off = 0;
	while (off < n) {
		if (n - off < 3) { protocol_err++; break; }
		/* New firmware reads the top nibble of a C2 length as CS flags; old
		 * firmware takes it as length, so stray flags overrun the transfer. */
		unsigned hi = (b[off] == 0xC2 && FW_HAS_CS_FLAGS) ? (b[off + 2] & 0x0F) : b[off + 2];
		unsigned len = b[off + 1] | (hi << 8);
		if (off + 3 + (int)len > n) { fprintf(stderr, "mock: command overruns transfer\n"); protocol_err++; break; }
		device_cmd(b + off, len);
		off += 3 + len;
		if (fw_first_only)
			break;
	}
	pack_from = -1;
}

static void device_cmd(const uint8_t *b, unsigned len)
{
	switch (b[0]) {
	case 0xC0:
		if (len != 26) protocol_err++;
		cfg_div = (b[15] >> 3) & 7;
		queue_status(0xC0);
		break;
	case 0xC1:
		cnt.c1++;
		if (len != 10) protocol_err++;
		if (b[3] & 0x80) {
			int low = !(b[3] & 0x40);
			if (low && !cs_low) { tx_op = -1; tx_pos = 0; }
			cs_low = low;
		}
		break;
	case 0xC4:
		cnt.c4++;
		if (!cs_low) { fprintf(stderr, "mock: write with CS high\n"); protocol_err++; }
		memcpy(mosi_log + mosi_len, b + 3, len);
		mosi_len += len;
		for (unsigned i = 0; i < len; i++) { mosi_byte(b[3 + i]); tx_pos++; }
		if (fail_next_status) {
			uint8_t s[4] = { 0xC4, 1, 0, 0x01 };
			fail_next_status = 0;
			queue_bytes(s, 4);
		} else
			queue_status(0xC4);
		break;
	case 0xCA: {                    /* chip info: [CA 04 00 ver_lo ver_hi 00 00] */
		uint8_t r[7] = { 0xCA, 4, 0, fw_version & 0xFF, fw_version >> 8, 0, 0 };
		queue_bytes(r, sizeof r);
		break;
	}
	case 0xC2: {                    /* full duplex: echo the MISO stream */
		cnt.c2++;
		uint8_t flags = FW_HAS_CS_FLAGS ? (b[2] & 0xC0) : 0;
		if (flags & 0x80) {     /* assert CS0 before the frame */
			if (!cs_low) { tx_op = -1; tx_pos = 0; }
			cs_low = 1;
		}
		if (!cs_low)
			cnt.c2_cs_high++;   /* the batching probe's sentinel does this on purpose */
		else {
			memcpy(mosi_log + mosi_len, b + 3, len);
			mosi_len += len;
		}
		static uint8_t fr[3 + 4096];
		unsigned left = len, i = 0;
		while (left) {
			unsigned c = left > (unsigned)frame_payload ? (unsigned)frame_payload : left;
			fr[0] = 0xC2; fr[1] = c & 0xFF; fr[2] = (c >> 8) | (fw_echo_flags ? flags : 0);
			for (unsigned k = 0; k < c; k++, i++) {
				if (cs_low) mosi_byte(b[3 + i]);
				fr[3 + k] = cs_low ? miso_at() : 0xFF;
			}
			queue_bytes(fr, c + 3);
			left -= c;
		}
		if (flags & 0x40)       /* release CS0 after the frame */
			cs_low = 0;
		break;
	}
	case 0xC3: {
		cnt.c3++;
		if (len != 4) protocol_err++;
		if (!cs_low) { fprintf(stderr, "mock: read with CS high\n"); protocol_err++; }
		uint32_t cnt = b[3] | (b[4] << 8) | (b[5] << 16) | ((uint32_t)b[6] << 24);
		static uint8_t fr[3 + 65536];
		while (cnt) {
			unsigned c = cnt > (unsigned)frame_payload ? (unsigned)frame_payload : cnt;
			fr[0] = 0xC3; fr[1] = c & 0xFF; fr[2] = c >> 8;
			for (unsigned i = 0; i < c; i++) fr[3 + i] = miso_at();
			queue_bytes(fr, c + 3);
			cnt -= c;
		}
		break;
	}
	default:
		fprintf(stderr, "mock: unknown cmd 0x%02X\n", b[0]);
		protocol_err++;
	}
}

/* ---- fake libusb ---- */
static int fake_dev;
int libusb_init(libusb_context **c) { (void)c; return 0; }
void libusb_exit(libusb_context *c) { (void)c; }
libusb_device_handle *libusb_open_device_with_vid_pid(libusb_context *c, uint16_t v, uint16_t p)
{ (void)c; return (v == 0x1A86 && p == 0x55DB) ? (libusb_device_handle *)&fake_dev : NULL; }
int libusb_set_auto_detach_kernel_driver(libusb_device_handle *h, int e) { (void)h; (void)e; return 0; }
int libusb_claim_interface(libusb_device_handle *h, int i) { (void)h; return i == 2 ? 0 : LIBUSB_ERROR_BUSY; }
int libusb_release_interface(libusb_device_handle *h, int i) { (void)h; (void)i; return 0; }
void libusb_close(libusb_device_handle *h) { (void)h; }
libusb_device *libusb_get_device(libusb_device_handle *h) { (void)h; return NULL; }
int libusb_get_device_descriptor(libusb_device *d, struct libusb_device_descriptor *x) { (void)d; (void)x; return -1; }
int libusb_clear_halt(libusb_device_handle *h, unsigned char ep) { (void)h; (void)ep; return 0; }
const char *libusb_error_name(int e) { static char b[16]; snprintf(b, sizeof b, "ERR%d", e); return b; }
const char *get_libusb_version(void) { return "mock"; }
unsigned int spi_controller_get_speed_khz(void) { return 0; }
static unsigned int mock_opts;
unsigned int spi_controller_get_options(void) { return mock_opts; }

int libusb_bulk_transfer(libusb_device_handle *h, unsigned char ep, unsigned char *data,
			 int length, int *transferred, unsigned int timeout)
{
	(void)h; (void)timeout;
	int got = 0;
	if (transferred) *transferred = 0;

	if (!(ep & 0x80)) {
		if (ep != 0x06) return LIBUSB_ERROR_PIPE;
		cnt.out++;
		device_out(data, length);
		if (transferred) *transferred = length;
		return 0;
	}
	if (ep != 0x86) return LIBUSB_ERROR_PIPE;
	cnt.in++;
	for (;;) {
		if (qh == qt) {                     /* device has nothing more */
			if (transferred) *transferred = got;
			if (got == 0 || got % PKT == 0) {
				if (got) stall_seen++;
				return LIBUSB_ERROR_TIMEOUT;
			}
			return 0;
		}
		int pl = q[qh].len;
		if (pl > length - got) {             /* packet does not fit */
			overflow_seen++;
			if (transferred) *transferred = got;
			return LIBUSB_ERROR_OVERFLOW;
		}
		memcpy(data + got, q[qh].d, pl);
		got += pl;
		qh = (qh + 1) % QMAX;
		if (pl < PKT || got == length) {     /* short packet or filled */
			if (transferred) *transferred = got;
			return 0;
		}
	}
}

/* ---- tests ---- */
static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); } } while (0)

static void run_read(int fp, unsigned n)
{
	static uint8_t buf[1 << 20];
	frame_payload = fp;
	uint8_t start = miso_next;
	memset(buf, 0xEE, n);
	stall_seen = overflow_seen = 0;

	ch347_programmer.cs_select();
	int r = ch347_programmer.send_command(0, n, NULL, buf);
	ch347_programmer.cs_deselect();

	int ok = (r == 0);
	for (unsigned i = 0; ok && i < n; i++)
		if (buf[i] != (uint8_t)(start + i)) ok = 0;
	CHECK(ok, "read n=%u frame=%d ret=%d", n, fp, r);
	CHECK(qh == qt, "read n=%u frame=%d left %d packets queued", n, fp, (qt - qh + QMAX) % QMAX);
	CHECK(!stall_seen && !overflow_seen, "read n=%u frame=%d stall=%d overflow=%d", n, fp, stall_seen, overflow_seen);
	qh = qt;
	miso_next = start + n;
}

int main(void)
{
	fw_version = 0x0140;            /* first suite: old firmware, plain CS commands */
	CHECK(ch347_programmer.init() == 0, "init");
	CHECK(cfg_div == 2, "default divisor %d (want 2 = 15 MHz)", cfg_div);
	CHECK(!cs_low, "CS high after init");

	static const unsigned sizes[] = {
		1, 2, 3, 4, 5, 256, 505, 506, 507, 508, 509, 510, 511, 512, 513,
		1014, 1018, 1021, 1022, 1024, 1536, 2042, 2048, 3581, 4093, 4094,
		4096, 4097, 8186, 8192, 65536, 100003, 262144,
	};
	static const int frames[] = { 507, 4093, 1021, 509, 1 };
	for (size_t f = 0; f < sizeof frames / sizeof *frames; f++)
		for (size_t s = 0; s < sizeof sizes / sizeof *sizes; s++) {
			if (frames[f] < 64 && sizes[s] > 8192)
				continue;   /* one packet per byte: keep the queue bounded */
			run_read(frames[f], sizes[s]);
		}

	/* write: data arrives intact in <=510-byte frames, each acknowledged */
	static uint8_t w[5000];
	for (unsigned i = 0; i < sizeof w; i++) w[i] = (uint8_t)(i * 7 + 3);
	mosi_len = 0;
	ch347_programmer.cs_select();
	CHECK(ch347_programmer.send_command(sizeof w, 0, w, NULL) == 0, "write");
	ch347_programmer.cs_deselect();
	CHECK(mosi_len == sizeof w && !memcmp(mosi_log, w, sizeof w), "write data mismatch (%u bytes)", mosi_len);
	CHECK(qh == qt, "write left status packets queued");

	/* JEDEC-ID style: write 1, read 3 in one call -> one 0xC2 frame carrying
	 * the opcode plus 3 filler bytes; the reply's last 3 bytes are the ID. */
	frame_payload = 507;
	uint8_t id[3], op = 0x9F;
	uint8_t start = miso_next;
	mosi_len = 0;
	memset(&cnt, 0, sizeof cnt);
	ch347_programmer.cs_select();
	CHECK(ch347_programmer.send_command(1, 3, &op, id) == 0, "rdid");
	ch347_programmer.cs_deselect();
	CHECK(mosi_len == 4 && mosi_log[0] == 0x9F && mosi_log[1] == 0xFF && mosi_log[3] == 0xFF,
	      "rdid MOSI: opcode then 3 x FF (got %u bytes, %02X %02X)", mosi_len, mosi_log[0], mosi_log[1]);
	CHECK(!memcmp(id, chip_id, 3), "rdid data %02X %02X %02X", id[0], id[1], id[2]);
	CHECK(cnt.c2 == 1 && cnt.c3 == 0 && cnt.c4 == 0, "rdid used c2=%d c3=%d c4=%d", cnt.c2, cnt.c3, cnt.c4);

	/* Status poll as the NOR layer issues it: opcode write, then a 1-byte
	 * read -> CS low, one C2 round trip, CS high = 3 OUT + 1 IN. */
	memset(&cnt, 0, sizeof cnt);
	uint8_t rdsr = 0x05, sr = 0;
	start = miso_next;
	ch347_programmer.cs_select();
	CHECK(ch347_programmer.send_command(1, 0, &rdsr, NULL) == 0, "rdsr write");
	CHECK(ch347_programmer.send_command(0, 1, NULL, &sr) == 0, "rdsr read");
	ch347_programmer.cs_deselect();
	CHECK(sr == (uint8_t)(start + 1), "rdsr value");
	CHECK(cnt.out == 3 && cnt.in == 1 && cnt.c2 == 1 && cnt.c4 == 0,
	      "rdsr traffic out=%d in=%d c2=%d c4=%d (want 3/1/1/0)", cnt.out, cnt.in, cnt.c2, cnt.c4);
	miso_next = start + 2;

	/* Page program as the NOR layer issues it: opcode, 3 address bytes one
	 * at a time, then 256 data bytes -> one 260-byte C4 frame at CS high. */
	static uint8_t page[256];
	for (int i = 0; i < 256; i++) page[i] = (uint8_t)(255 - i);
	uint8_t pp = 0x02, a[3] = { 0x12, 0x34, 0x00 };
	memset(&cnt, 0, sizeof cnt);
	mosi_len = 0;
	ch347_programmer.cs_select();
	ch347_programmer.send_command(1, 0, &pp, NULL);
	for (int i = 0; i < 3; i++) ch347_programmer.send_command(1, 0, &a[i], NULL);
	ch347_programmer.send_command(256, 0, page, NULL);
	CHECK(cnt.c4 == 0, "page program sent before CS high (c4=%d)", cnt.c4);
	CHECK(ch347_programmer.cs_deselect() == 0, "page program flush");
	CHECK(cnt.c4 == 1 && mosi_len == 260 && mosi_log[0] == 0x02 && mosi_log[1] == 0x12 &&
	      mosi_log[3] == 0x00 && !memcmp(mosi_log + 4, page, 256),
	      "page program frame (c4=%d, %u bytes)", cnt.c4, mosi_len);
	CHECK(cnt.out == 3 && cnt.in == 1, "page program traffic out=%d in=%d (want 3/1)", cnt.out, cnt.in);
	CHECK(!cs_low, "CS high after page program");

	/* Read command: 4 held bytes + a 4096-byte read do not fit one C2
	 * frame -> held bytes flushed as C4, then the data streams via C3. */
	static uint8_t rbuf[4096];
	uint8_t rd[4] = { 0x03, 0, 0, 0 };
	memset(&cnt, 0, sizeof cnt);
	mosi_len = 0;
	start = miso_next;
	ch347_programmer.cs_select();
	ch347_programmer.send_command(4, 0, rd, NULL);
	CHECK(ch347_programmer.send_command(0, sizeof rbuf, NULL, rbuf) == 0, "bulk read");
	ch347_programmer.cs_deselect();
	CHECK(cnt.c4 == 1 && cnt.c3 == 1 && cnt.c2 == 0 && mosi_len == 4, "bulk read path c4=%d c3=%d c2=%d", cnt.c4, cnt.c3, cnt.c2);
	CHECK(rbuf[0] == start && rbuf[4095] == (uint8_t)(start + 4095), "bulk read data");
	miso_next = start + 4096;

	/* A failed held write (bad C4 status at CS high) makes every later call
	 * fail until reinit, so the flash layer's next command sees the error. */
	fail_next_status = 1;
	ch347_programmer.cs_select();
	ch347_programmer.send_command(1, 0, &pp, NULL);
	CHECK(ch347_programmer.cs_deselect() != 0, "failed flush reported by cs_deselect");
	uint8_t x;
	CHECK(ch347_programmer.cs_select() != 0, "cs_select after failure");
	CHECK(ch347_programmer.send_command(0, 1, NULL, &x) != 0, "send_command after failure");
	qh = qt;
	protocol_err = 0;               /* the failure test sends with CS state mid-change */
	CHECK(ch347_programmer.reinit() == 0, "reinit clears the failure");
	ch347_programmer.cs_select();
	CHECK(ch347_programmer.send_command(1, 1, &rdsr, &x) == 0, "works again after reinit");
	ch347_programmer.cs_deselect();

	/* a bad reply header must be rejected, not copied */
	ch347_programmer.cs_select();
	uint8_t bogus[7] = { 0xAA, 0x04, 0x00, 1, 2, 3, 4 };
	queue_bytes(bogus, 7);
	uint8_t tmp[16];
	/* queue a garbage frame ahead of the real reply */
	CHECK(ch347_programmer.send_command(0, 4, NULL, tmp) != 0, "bad header accepted");
	ch347_programmer.cs_deselect();
	qh = qt;

	CHECK(protocol_err == 0, "%d protocol errors", protocol_err);
	CHECK(ch347_programmer.shutdown() == 0, "shutdown");

	/* ---------------- chip-select flags ---------------- */
	struct { const char *name; unsigned ver; int echo, opts, want_out, want_in, want_out_end; } modes[] = {
		/* firmware 0544: WREN and page program are one C2 frame each with
		 * both CS flags, the status poll one C2 frame with the start flag;
		 * only the poll's CS release is a separate C1 */
		{ "csflags",        0x0544, 0, 0,                   3, 3, 4 },
		/* same, firmware echoing the flags in its reply headers */
		{ "csflags+echo",   0x0544, 1, 0,                   3, 3, 4 },
		/* exactly the minimum version */
		{ "csflags@0341",   0x0341, 0, 0,                   3, 3, 4 },
		/* too old: plain CS commands */
		{ "old-fw",         0x0340, 0, 0,                   8, 3, 9 },
		/* capable but turned off */
		{ "disabled",       0x0544, 0, SPI_OPT_NO_CS_FLAGS, 8, 3, 9 },
	};
	for (size_t m = 0; m < sizeof modes / sizeof *modes; m++) {
		const char *L = modes[m].name;
		qh = qt; protocol_err = 0; cs_low = 0; fail_next_status = 0;
		fw_version = modes[m].ver;
		fw_echo_flags = modes[m].echo;
		mock_opts = modes[m].opts;
		memset(&cnt, 0, sizeof cnt);

		CHECK(ch347_programmer.init() == 0, "%s: init", L);
		CHECK(!cs_low, "%s: CS high after init", L);
		CHECK(qh == qt, "%s: init left replies queued", L);

		/* WREN, page program (opcode, 3 address bytes, 256 data), status poll */
		static uint8_t pg[256];
		for (int i = 0; i < 256; i++) pg[i] = (uint8_t)(i ^ 0x5A);
		uint8_t wren = 0x06, ppo = 0x02, ad[3] = { 0x01, 0x02, 0x00 }, rs = 0x05, srv = 0xAA;
		memset(&cnt, 0, sizeof cnt);
		mosi_len = 0;
		ch347_programmer.cs_select();
		ch347_programmer.send_command(1, 0, &wren, NULL);
		CHECK(ch347_programmer.cs_deselect() == 0 && !cs_low, "%s: CS high after WREN", L);
		ch347_programmer.cs_select();
		ch347_programmer.send_command(1, 0, &ppo, NULL);
		for (int i = 0; i < 3; i++) ch347_programmer.send_command(1, 0, &ad[i], NULL);
		ch347_programmer.send_command(256, 0, pg, NULL);
		CHECK(ch347_programmer.cs_deselect() == 0 && !cs_low, "%s: CS high after page program", L);
		start = miso_next;      /* C2 write frames clock MISO too */
		ch347_programmer.cs_select();
		ch347_programmer.send_command(1, 0, &rs, NULL);
		CHECK(ch347_programmer.send_command(0, 1, NULL, &srv) == 0, "%s: status read", L);
		CHECK(cnt.out == modes[m].want_out && cnt.in == modes[m].want_in,
		      "%s: page sequence traffic out=%d in=%d (want %d/%d)", L, cnt.out, cnt.in,
		      modes[m].want_out, modes[m].want_in);
		CHECK(srv == (uint8_t)(start + 1), "%s: status value", L);
		CHECK(mosi_len == 263 && mosi_log[0] == 0x06 && mosi_log[1] == 0x02 && mosi_log[2] == 0x01 &&
		      !memcmp(mosi_log + 5, pg, 256) && mosi_log[261] == 0x05 && mosi_log[262] == 0xFF,
		      "%s: MOSI order (%u bytes)", L, mosi_len);
		CHECK(ch347_programmer.cs_deselect() == 0, "%s: release after status read", L);
		CHECK(cnt.out == modes[m].want_out_end, "%s: transfers incl. release %d (want %d)",
		      L, cnt.out, modes[m].want_out_end);
		CHECK(!cs_low && qh == qt, "%s: CS high and nothing queued", L);
		miso_next = start + 2;

		/* NOR read pattern: opcode + address held, then a large read ->
		 * held bytes go out first (C2 with the start flag, or C1 + C4),
		 * then C3 streams the data */
		uint8_t rdc[4] = { 0x03, 0, 0, 0 };
		static uint8_t big[8192];
		ch347_programmer.cs_select();
		ch347_programmer.send_command(4, 0, rdc, NULL);
		uint8_t s0 = miso_next + 4 * (modes[m].want_out == 3);   /* C2 clocks 4 MISO bytes */
		CHECK(ch347_programmer.send_command(0, sizeof big, NULL, big) == 0, "%s: NOR read", L);
		CHECK(ch347_programmer.cs_deselect() == 0 && !cs_low, "%s: CS high after NOR read", L);
		CHECK(big[0] == s0 && big[sizeof big - 1] == (uint8_t)(s0 + sizeof big - 1),
		      "%s: NOR read data %02X (want %02X)", L, big[0], s0);
		miso_next = s0 + sizeof big;

		/* an empty CS window sends nothing in flag mode */
		memset(&cnt, 0, sizeof cnt);
		ch347_programmer.cs_select();
		ch347_programmer.cs_deselect();
		CHECK(cnt.out == (modes[m].want_out == 3 ? 0 : 2), "%s: empty CS window sent %d", L, cnt.out);

		/* reads and a large write stay byte-exact */
		static const unsigned bsz[] = { 1, 507, 508, 1021, 4093, 4096, 65536, 100003 };
		for (int fp = 0; fp < 2; fp++)
			for (size_t s = 0; s < sizeof bsz / sizeof *bsz; s++)
				run_read(fp ? 4093 : 507, bsz[s]);
		mosi_len = 0;
		ch347_programmer.cs_select();
		CHECK(ch347_programmer.send_command(sizeof w, 0, w, NULL) == 0, "%s: write", L);
		CHECK(ch347_programmer.cs_deselect() == 0 && !cs_low, "%s: CS high after write", L);
		CHECK(mosi_len == sizeof w && !memcmp(mosi_log, w, sizeof w), "%s: write data (%u bytes)", L, mosi_len);

		CHECK(protocol_err == 0, "%s: %d protocol errors", L, protocol_err);
		CHECK(ch347_programmer.shutdown() == 0, "%s: shutdown", L);
		CHECK(!cs_low, "%s: CS high after shutdown", L);
	}

	printf("%s (%d failure%s)\n", failures ? "FAILED" : "all checks passed",
	       failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
