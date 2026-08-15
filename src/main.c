/*
 * main.c - Mulberry CH4 bring-up on nRF9151-DK
 *
 * Milestone 1: prove the UART link, identify the sensor, then poll
 * GET_DATA_PACK at 1 Hz and print a parsed line to the VCOM console.
 *
 * No modem, no power gating, no NVS. Those come after the link is proven.
 *
 * Redwood Labs Inc.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <stdio.h>
#include <string.h>

#include "sdcs.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

#define SENSOR_UART_NODE DT_NODELABEL(uart2)
static const struct device *const sensor_uart = DEVICE_DT_GET(SENSOR_UART_NODE);

/* Poll period. Doc s5.4: do not exceed 1 Hz if you care about the current
 * budget. The sensor's own measurement cycle is 500 ms, so 1 Hz never
 * misses a fresh value.
 */
#define POLL_PERIOD_MS 1000

/* Set to 1 once you have a real time source (modem AT+CCLK or GNSS).
 * Leaving it 0 means the ALARM "time not synchronized" bit stays set,
 * which is correct and honest rather than stamping a fictional time.
 */
#define SET_RTC_AT_BOOT 0

/* Set to 1 to run the pin/pinctrl verification instead of talking to the
 * sensor. Jumper D6 to D7 on the Arduino header, flash, and watch the
 * console. This proves the UARTE2 instance, the pinctrl assignment, the
 * physical header location and the 9600 baud setting all at once, with no
 * sensor attached and nothing that can be damaged by a wrong pin.
 *
 * Run this BEFORE the Mulberry is ever connected.
 */
#define LOOPBACK_TEST 0

#if LOOPBACK_TEST
static int run_loopback(void)
{
	static const uint8_t pattern[] = {
		0x7B, 0x59, 0x06, 0x00, 0x00, 0x10, 0xAA, 0x26, 0x7D
	};
	uint8_t got[sizeof(pattern)];
	size_t n = 0;

	printk("\n--- UART2 loopback test ---\n");
	printk("Jumper D6 (P0.06) to D7 (P0.07), then reset.\n");
	printk("Sending a real GET_MODEL_NAME frame at 9600 8N1...\n");

	/* Send one byte, read it back, repeat.
	 *
	 * The obvious version - transmit all nine bytes and then read the
	 * echo - does not work here. uart_poll_out() blocks until each byte
	 * is on the wire, so with TX looped to RX the whole frame arrives
	 * before anything drains it, and the result depends on how much the
	 * UARTE poll-mode RX path happens to buffer rather than on the
	 * wiring. That version dropped byte 8 of 9 reproducibly while every
	 * other byte came back correct.
	 *
	 * Interleaving keeps at most one byte in flight, so a failure here
	 * means the pins, the jumper or the baud rate - which is the only
	 * thing this test is meant to prove.
	 */
	size_t failed_at = sizeof(pattern);

	for (size_t i = 0; i < sizeof(pattern); i++) {
		/* One byte at 9600 8N1 is ~1.04 ms on the wire, plus the
		 * driver's turnaround. 50 ms is far more than enough and
		 * still fails fast when nothing is connected.
		 */
		int64_t deadline = k_uptime_get() + 50;
		bool echoed = false;

		uart_poll_out(sensor_uart, pattern[i]);

		while (k_uptime_get() < deadline) {
			uint8_t b;

			if (uart_poll_in(sensor_uart, &b) == 0) {
				got[n++] = b;
				echoed = true;
				break;
			}
			/* Busy-wait rather than k_sleep(): a tick is 10 ms
			 * by default, which would allow only a handful of
			 * polls inside the deadline.
			 */
			k_busy_wait(100);
		}

		if (!echoed) {
			failed_at = i;
			break;
		}
	}

	printk("sent: ");
	for (size_t i = 0; i < sizeof(pattern); i++) {
		printk("%02X ", pattern[i]);
	}
	printk("\nrecv: ");
	for (size_t i = 0; i < n; i++) {
		printk("%02X ", got[i]);
	}
	printk("(%u/%u bytes)\n", (unsigned)n, (unsigned)sizeof(pattern));

	if (n != sizeof(pattern)) {
		printk("\nRESULT: FAIL - no echo for byte %u (0x%02X).\n",
		       (unsigned)failed_at, pattern[failed_at]);
		printk("  failed on byte 0 -> jumper missing, or wrong "
		       "header pins\n"
		       "  failed later     -> intermittent contact, or "
		       "another peripheral on P0.06/P0.07\n");
		return -EIO;
	}
	if (memcmp(pattern, got, n) != 0) {
		printk("\nRESULT: FAIL - bytes differ. Suspect baud or "
		       "another peripheral driving these pins.\n");
		return -EIO;
	}

	printk("\nRESULT: PASS - UARTE2 is on D6/D7 at 9600 8N1.\n"
	       "Remove the jumper and wire the sensor.\n");
	return 0;
}
#endif /* LOOPBACK_TEST */

static void log_identity(void)
{
	char buf[32];
	uint32_t runtime;

	if (sdcs_get_ascii(SDCS_CMD_GET_MODEL_NAME, buf, sizeof(buf)) == 0) {
		printk("model      : %s\n", buf);
	} else {
		printk("model      : <no response>\n");
	}
	if (sdcs_get_ascii(SDCS_CMD_GET_PROD_NAME, buf, sizeof(buf)) == 0) {
		printk("product    : %s\n", buf);
	}
	if (sdcs_get_ascii(SDCS_CMD_GET_SEN_SN, buf, sizeof(buf)) == 0) {
		printk("serial     : %s\n", buf);
	}
	if (sdcs_get_ascii(SDCS_CMD_GET_FW_VER, buf, sizeof(buf)) == 0) {
		printk("firmware   : %s\n", buf);
	}
	if (sdcs_get_run_time(&runtime) == 0) {
		printk("run time   : %u s (%u h)\n", runtime, runtime / 3600u);
	}
}

static void describe_flags(const struct sdcs_reading *r, char *out, size_t sz)
{
	out[0] = '\0';
	size_t used = 0;

#define APPEND(cond, text)                                                  \
	do {                                                                \
		if ((cond) && used < sz - 1) {                              \
			int _w = snprintk(out + used, sz - used, "%s%s",     \
					  used ? "," : "", text);           \
			if (_w > 0) {                                       \
				used += (size_t)_w;                         \
			}                                                   \
		}                                                           \
	} while (0)

	APPEND(r->status & SDCS_ST_WARMUP, "WARMUP");
	APPEND(r->status & SDCS_ST_IN_CALIBRATION, "IN_CAL");
	APPEND(r->status & SDCS_ST_UNSTABLE_ENV, "UNSTABLE_ENV");
	APPEND(r->alarm & SDCS_AL_OVER_RANGE, "OVER_RANGE");
	APPEND(r->alarm & SDCS_AL_TIME_NOT_SYNC, "TIME_NOT_SYNC");
	APPEND(r->alarm & SDCS_AL_HIGH, "HIGH");
	APPEND(r->alarm & SDCS_AL_LOW, "LOW");
	APPEND(r->alarm & SDCS_AL_OVER_RANGE_HIGH, "OVER_RANGE_HIGH");
	APPEND(r->alarm & SDCS_AL_DRIFT, "DRIFT");
#undef APPEND

	if (out[0] == '\0') {
		strncpy(out, "-", sz);
	}
}

int main(void)
{
	printk("\n=== Redwood Labs / Mulberry CH4 bring-up ===\n");

	if (!device_is_ready(sensor_uart)) {
		printk("FATAL: sensor UART not ready\n");
		return -ENODEV;
	}

#if LOOPBACK_TEST
	/* Runs before sdcs_init() on purpose: the SDCS client enables
	 * interrupt-driven RX, which would consume the bytes that
	 * uart_poll_in() is waiting for.
	 */
	while (true) {
		(void)run_loopback();
		k_sleep(K_SECONDS(5));
	}
#endif

	int rc = sdcs_init(sensor_uart);

	if (rc) {
		printk("FATAL: sdcs_init failed (%d)\n", rc);
		return rc;
	}

	/* Self test of the CRC before we blame the wiring for anything. */
	uint16_t check = sdcs_crc16((const uint8_t *)"123456789", 9);

	printk("CRC-16/BUYPASS self test: %04x (expect FEE8) %s\n",
	       check, (check == 0xFEE8u) ? "OK" : "FAIL");

	/* Doc s5.1: comms ready within 100 ms of power-on, first gas value
	 * after 30 s of warm-up. Give the sensor a moment if we booted together.
	 */
	k_sleep(K_MSEC(500));

	printk("\n--- identity ---\n");
	log_identity();

#if SET_RTC_AT_BOOT
	printk("\n--- RTC sync ---\n");
	if (sdcs_write_protect(false) == 0) {
		/* Replace with a real time source before trusting this. */
		if (sdcs_set_rtc(2026, 8, 14, 12, 0, 0) == 0) {
			printk("RTC set\n");
		}
		sdcs_write_protect(true);
	}
#endif

	printk("\n--- polling at %d ms ---\n", POLL_PERIOD_MS);
	printk("  ppm     act    ref   T(C)  flags\n");

	uint32_t consecutive_failures = 0;

	while (true) {
		struct sdcs_reading r;
		char flags[128];

		rc = sdcs_get_data_pack(SDCS_DP_MULBERRY_ALL, &r);
		if (rc < 0) {
			consecutive_failures++;
			printk("read failed (%d), %u in a row\n",
			       rc, consecutive_failures);
			if (consecutive_failures >= 3) {
				/* Doc s5.5: 3 in a row means power-cycle the
				 * sensor. Soft reset will not help.
				 */
				printk("sensor considered offline - "
				       "hard reset required\n");
			}
			k_sleep(K_MSEC(POLL_PERIOD_MS));
			continue;
		}
		consecutive_failures = 0;

		describe_flags(&r, flags, sizeof(flags));

		printk("%8.2f %6u %6u %5d  %s",
		       (double)r.gas_ppm_x100 / 100.0,
		       r.raw_active, r.raw_reference,
		       r.temp_sensor_error ? -999 : r.temp_c,
		       flags);

		if (r.n_errors) {
			printk("  ERR[");
			for (uint8_t e = 0; e < r.n_errors; e++) {
				printk("%s%u", e ? "," : "", r.errors[e]);
			}
			printk("]");
		}
		printk("\n");

		k_sleep(K_MSEC(POLL_PERIOD_MS));
	}

	return 0;
}
