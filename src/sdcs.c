/*
 * sdcs.c - eLichens SDCS client for Zephyr / nRF Connect SDK
 * Redwood Labs Inc.
 */

#include "sdcs.h"

#include <zephyr/kernel.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(sdcs, LOG_LEVEL_INF);

/* Protocol timing (rev2.5 s2.3): response within 250 ms, 3 timeouts = offline.
 * 400 ms gives margin for the sensor's 500 ms measure cycle boundary.
 */
#define SDCS_RESP_TIMEOUT_MS  400
#define SDCS_BYTE_GAP_MS      50
#define SDCS_RETRIES          2

#define RX_RING_SIZE 256

static const struct device *uart;
static uint8_t rx_ring_buf[RX_RING_SIZE];
static struct ring_buf rx_ring;
static K_SEM_DEFINE(rx_sem, 0, 1);
static K_MUTEX_DEFINE(bus_lock);
static uint16_t tx_index;

uint16_t sdcs_crc16(const uint8_t *buf, size_t len)
{
	uint16_t crc = 0x0000;

	for (size_t i = 0; i < len; i++) {
		crc ^= (uint16_t)buf[i] << 8;
		for (int b = 0; b < 8; b++) {
			crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x8005u)
					      : (uint16_t)(crc << 1);
		}
	}
	return crc;
}

static void uart_isr(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);
	uint8_t byte;

	if (!uart_irq_update(dev)) {
		return;
	}

	while (uart_irq_rx_ready(dev)) {
		if (uart_fifo_read(dev, &byte, 1) != 1) {
			break;
		}
		if (ring_buf_put(&rx_ring, &byte, 1) != 1) {
			LOG_WRN("rx ring overflow, byte dropped");
		}
		k_sem_give(&rx_sem);
	}
}

int sdcs_init(const struct device *uart_dev)
{
	if (!device_is_ready(uart_dev)) {
		return -ENODEV;
	}

	uart = uart_dev;
	ring_buf_init(&rx_ring, sizeof(rx_ring_buf), rx_ring_buf);

	int rc = uart_irq_callback_user_data_set(uart, uart_isr, NULL);

	if (rc != 0) {
		return rc;
	}
	uart_irq_rx_enable(uart);
	return 0;
}

static void rx_flush(void)
{
	ring_buf_reset(&rx_ring);
	k_sem_reset(&rx_sem);
}

/* Blocking read of exactly one byte with a deadline. */
static int rx_byte(uint8_t *out, k_timepoint_t deadline)
{
	while (true) {
		if (ring_buf_get(&rx_ring, out, 1) == 1) {
			return 0;
		}
		if (sys_timepoint_expired(deadline)) {
			return -ETIMEDOUT;
		}
		(void)k_sem_take(&rx_sem, sys_timepoint_timeout(deadline));
	}
}

static void tx_frame(const uint8_t *frame, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		uart_poll_out(uart, frame[i]);
	}
}

/* Read one well-formed frame. Returns total frame length in buf, or negative. */
static int rx_frame(uint8_t *buf, size_t buf_max, k_timepoint_t deadline)
{
	uint8_t b;
	int rc;

	/* Hunt for SOP. Anything before it is line noise or a stale tail. */
	do {
		rc = rx_byte(&b, deadline);
		if (rc) {
			return rc;
		}
	} while (b != SDCS_SOP);

	if (buf_max < 4) {
		return -ENOSPC;
	}
	buf[0] = b;

	rc = rx_byte(&buf[1], deadline);   /* Ver */
	if (rc) {
		return rc;
	}
	rc = rx_byte(&buf[2], deadline);   /* Len */
	if (rc) {
		return rc;
	}

	if (buf[1] != SDCS_VER) {
		LOG_WRN("bad version 0x%02x", buf[1]);
		return -EPROTO;
	}

	size_t total = (size_t)buf[2] + 3u;   /* Len counts everything after Len */

	if (total > buf_max || total < 6u) {
		LOG_WRN("bad length field 0x%02x", buf[2]);
		return -EPROTO;
	}

	for (size_t i = 3; i < total; i++) {
		rc = rx_byte(&buf[i], deadline);
		if (rc) {
			return rc;
		}
	}

	if (buf[total - 1] != SDCS_EOP) {
		LOG_WRN("bad EOP 0x%02x", buf[total - 1]);
		return -EPROTO;
	}

	uint16_t calc = sdcs_crc16(buf, total - 3);
	uint16_t got  = ((uint16_t)buf[total - 3] << 8) | buf[total - 2];

	if (calc != got) {
		LOG_WRN("CRC mismatch: calc %04x got %04x", calc, got);
		return -EBADMSG;
	}

	return (int)total;
}

int sdcs_transact(uint8_t cmd, const uint8_t *req, size_t req_len,
		  uint8_t *resp, size_t resp_max, uint8_t *slave_err)
{
	uint8_t frame[6 + SDCS_MAX_DATA + 3];
	uint8_t rbuf[6 + SDCS_MAX_DATA + 3];
	int rc = -EIO;

	if (uart == NULL) {
		return -ENODEV;
	}
	if (req_len > SDCS_MAX_DATA) {
		return -EINVAL;
	}
	if (slave_err) {
		*slave_err = 0;
	}

	k_mutex_lock(&bus_lock, K_FOREVER);

	for (int attempt = 0; attempt <= SDCS_RETRIES; attempt++) {
		size_t n = 0;
		size_t total = 6u + req_len + 3u;

		frame[n++] = SDCS_SOP;
		frame[n++] = SDCS_VER;
		frame[n++] = (uint8_t)(total - 3u);
		frame[n++] = (uint8_t)(tx_index >> 8);
		frame[n++] = (uint8_t)(tx_index & 0xFFu);
		frame[n++] = cmd;
		if (req_len) {
			memcpy(&frame[n], req, req_len);
			n += req_len;
		}

		uint16_t crc = sdcs_crc16(frame, n);

		frame[n++] = (uint8_t)(crc >> 8);
		frame[n++] = (uint8_t)(crc & 0xFFu);
		frame[n++] = SDCS_EOP;

		rx_flush();
		tx_frame(frame, n);

		k_timepoint_t deadline = sys_timepoint_calc(K_MSEC(SDCS_RESP_TIMEOUT_MS));
		int flen = rx_frame(rbuf, sizeof(rbuf), deadline);

		if (flen == -ETIMEDOUT) {
			LOG_WRN("cmd 0x%02x timeout (attempt %d)", cmd, attempt + 1);
			rc = -ETIMEDOUT;
			continue;
		}
		if (flen < 0) {
			rc = flen;
			continue;   /* framing/CRC error - a resend is reasonable */
		}

		tx_index++;

		uint8_t rcmd = rbuf[5];
		size_t  dlen = (size_t)flen - 9u;   /* SOP,Ver,Len,Idx*2,Cmd,CRC*2,EOP */

		if (rcmd == SDCS_CMD_ERROR_SLAVE) {
			uint8_t code = (dlen >= 1) ? rbuf[6] : 0;

			if (slave_err) {
				*slave_err = code;
			}
			/* Doc s2.5: do not retransmit, the answer will not change. */
			LOG_WRN("cmd 0x%02x -> error packet 0x%02x", cmd, code);
			rc = -EPROTO;
			break;
		}

		if (rcmd != cmd) {
			LOG_WRN("cmd echo mismatch: sent 0x%02x got 0x%02x", cmd, rcmd);
			rc = -EPROTO;
			continue;
		}

		if (dlen > resp_max) {
			rc = -ENOSPC;
			break;
		}
		if (dlen && resp) {
			memcpy(resp, &rbuf[6], dlen);
		}
		rc = (int)dlen;
		break;
	}

	k_mutex_unlock(&bus_lock);
	return rc;
}

int sdcs_get_ascii(uint8_t cmd, char *out, size_t out_sz)
{
	uint8_t data[SDCS_MAX_DATA];
	int n = sdcs_transact(cmd, NULL, 0, data, sizeof(data), NULL);

	if (n < 0) {
		return n;
	}

	size_t copy = MIN((size_t)n, out_sz - 1);

	memcpy(out, data, copy);
	out[copy] = '\0';

	/* Trim any trailing non-printables; some fields are NUL padded. */
	for (size_t i = 0; i < copy; i++) {
		if (out[i] < 0x20 || out[i] > 0x7E) {
			out[i] = '\0';
			break;
		}
	}
	return 0;
}

int sdcs_get_run_time(uint32_t *seconds)
{
	uint8_t d[8];
	int n = sdcs_transact(SDCS_CMD_GET_RUN_TIME, NULL, 0, d, sizeof(d), NULL);

	if (n < 4) {
		return (n < 0) ? n : -EPROTO;
	}
	*seconds = ((uint32_t)d[0] << 24) | ((uint32_t)d[1] << 16) |
		   ((uint32_t)d[2] << 8) | d[3];
	return 0;
}

int sdcs_get_data_pack(uint16_t bitmap, struct sdcs_reading *out)
{
	uint8_t req[3] = { 0x00, (uint8_t)(bitmap >> 8), (uint8_t)(bitmap & 0xFFu) };
	uint8_t d[SDCS_MAX_DATA];

	int n = sdcs_transact(SDCS_CMD_GET_DATA_PACK, req, sizeof(req),
			      d, sizeof(d), NULL);
	if (n < 0) {
		return n;
	}

	memset(out, 0, sizeof(*out));
	size_t i = 0;

	/* Fields arrive in bit order B0..B5, only for bits that were requested. */
	if (bitmap & SDCS_DP_STATUS) {
		if (i >= (size_t)n) {
			return -EPROTO;
		}
		out->status = d[i++];
		out->have_status = true;
	}
	if (bitmap & SDCS_DP_ALARM) {
		if (i >= (size_t)n) {
			return -EPROTO;
		}
		out->alarm = d[i++];
		out->have_alarm = true;
	}
	if (bitmap & SDCS_DP_ERROR) {
		if (i >= (size_t)n) {
			return -EPROTO;
		}
		out->n_errors = d[i++];
		if (out->n_errors > SDCS_MAX_ERRORS ||
		    i + out->n_errors > (size_t)n) {
			return -EPROTO;
		}
		for (uint8_t e = 0; e < out->n_errors; e++) {
			out->errors[e] = d[i++];
		}
		out->have_errors = true;
	}
	if (bitmap & SDCS_DP_GAS) {
		if (i + 4 > (size_t)n) {
			return -EPROTO;
		}
		out->gas_ppm_x100 = (int32_t)(((uint32_t)d[i] << 24) |
					      ((uint32_t)d[i + 1] << 16) |
					      ((uint32_t)d[i + 2] << 8) |
					      d[i + 3]);
		i += 4;
		out->have_gas = true;
	}
	if (bitmap & SDCS_DP_RAW_COUNTS) {
		if (i >= (size_t)n) {
			return -EPROTO;
		}
		uint8_t nch = d[i++];

		if (nch != 2 || i + 4 > (size_t)n) {
			return -EPROTO;
		}
		out->raw_active    = ((uint16_t)d[i] << 8) | d[i + 1];
		out->raw_reference = ((uint16_t)d[i + 2] << 8) | d[i + 3];
		i += 4;
		out->have_raw = true;
	}
	if (bitmap & SDCS_DP_TEMPERATURE) {
		if (i >= (size_t)n) {
			return -EPROTO;
		}
		uint8_t t = d[i++];

		if (t == 0xFF) {
			out->temp_sensor_error = true;
		} else {
			out->temp_c = (int16_t)t - 127;
		}
		out->have_temp = true;
	}

	return 0;
}

int sdcs_write_protect(bool enable)
{
	uint8_t req = enable ? 0x01u : 0x00u;
	uint8_t err = 0;
	int rc = sdcs_transact(SDCS_CMD_WRITE_PROTECT, &req, 1, NULL, 0, &err);

	return (rc < 0) ? rc : 0;
}

int sdcs_set_rtc(uint16_t year, uint8_t month, uint8_t day,
		 uint8_t hour, uint8_t minute, uint8_t second)
{
	if (year < 2000 || year > 2099) {
		return -EINVAL;
	}

	uint8_t req[6] = {
		(uint8_t)(year - 2000), month, day, hour, minute, second
	};
	uint8_t err = 0;

	int rc = sdcs_transact(SDCS_CMD_SET_SEN_RTC, req, sizeof(req), NULL, 0, &err);

	if (rc == -EPROTO && err == SDCS_ERR_FAIL_NOWRITE) {
		LOG_ERR("SET_SEN_RTC refused: write protect is on");
	}

	/* Sensor stalls its UART while committing to NVM. Doc s4.4: wait 1 s. */
	k_sleep(K_MSEC(1100));

	return (rc < 0) ? rc : 0;
}
