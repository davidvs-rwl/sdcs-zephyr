/*
 * sdcs.h - eLichens Safety Device Communication Standard (SDCS) client
 *
 * Target sensor: eLichens Mulberry (MULBERRY-13 / MUL13442-Safety-CH4-5-vol)
 * Protocol ref:  eLichens sensors communication protocol rev2.5
 * Frame:         SOP(1) Ver(1) Len(1) Index(2) Cmd(1) Data(0..n) CRC(2) EOP(1)
 *                Len = total_frame_len - 3
 *                CRC = CRC-16/BUYPASS over SOP..Data, transmitted MSB first
 *
 * Redwood Labs Inc.
 */

#ifndef SDCS_H_
#define SDCS_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <zephyr/device.h>

#define SDCS_SOP                0x7Bu
#define SDCS_VER                0x59u
#define SDCS_EOP                0x7Du
#define SDCS_CMD_ERROR_SLAVE    0x71u

/* Commands (rev2.5 section 3) */
#define SDCS_CMD_GET_MODEL_NAME     0x10u
#define SDCS_CMD_GET_PROD_NAME      0x11u
#define SDCS_CMD_GET_FW_VER         0x12u
#define SDCS_CMD_GET_SEN_SN         0x13u
#define SDCS_CMD_GET_RUN_TIME       0x14u
#define SDCS_CMD_GET_APP_MODE       0x17u
#define SDCS_CMD_GET_SEN_NAME       0x35u
#define SDCS_CMD_GET_DATA_PACK      0x30u
#define SDCS_CMD_GET_SEN_DATA_FMT   0x31u
#define SDCS_CMD_GET_SEN_PARA       0x33u
#define SDCS_CMD_GET_SEN_PROD_DATE  0x37u
#define SDCS_CMD_GET_SEN_RTC        0x38u
#define SDCS_CMD_SET_SEN_PARA       0x80u
#define SDCS_CMD_SET_SEN_RTC        0x82u
#define SDCS_CMD_WRITE_PROTECT      0xA0u

/* GET_DATA_PACK request bitmap (DH[1]=high, DH[2]=low) */
#define SDCS_DP_STATUS      (1u << 0)
#define SDCS_DP_ALARM       (1u << 1)
#define SDCS_DP_ERROR       (1u << 2)
#define SDCS_DP_GAS         (1u << 3)
#define SDCS_DP_RAW_COUNTS  (1u << 4)
#define SDCS_DP_TEMPERATURE (1u << 5)
#define SDCS_DP_HUMIDITY    (1u << 6)   /* Bilberry only - do NOT set for Mulberry */
#define SDCS_DP_MULBERRY_ALL (SDCS_DP_STATUS | SDCS_DP_ALARM | SDCS_DP_ERROR | \
			      SDCS_DP_GAS | SDCS_DP_RAW_COUNTS | SDCS_DP_TEMPERATURE)

/* Sensor status byte */
#define SDCS_ST_WARMUP          (1u << 1)
#define SDCS_ST_IN_CALIBRATION  (1u << 3)
#define SDCS_ST_UNSTABLE_ENV    (1u << 4)

/* Sensor alarm byte */
#define SDCS_AL_OVER_RANGE      (1u << 0)
#define SDCS_AL_TIME_NOT_SYNC   (1u << 2)
#define SDCS_AL_HIGH            (1u << 3)
#define SDCS_AL_LOW             (1u << 4)
#define SDCS_AL_OVER_RANGE_HIGH (1u << 5)
#define SDCS_AL_DRIFT           (1u << 7)

/* Protocol error codes (Appendix I), returned as -SDCS_ESLAVE_BASE - code */
#define SDCS_ERR_FAIL_UNKNOWN       0x31u
#define SDCS_ERR_FAIL_INVALIDCMD    0x32u
#define SDCS_ERR_FAIL_DATASIZE      0x33u
#define SDCS_ERR_FAIL_INVALIDVALUE  0x34u
#define SDCS_ERR_FAIL_NOWRITE       0x39u
#define SDCS_ERR_FAIL_OPERATION     0x3Fu

#define SDCS_MAX_DATA 128
#define SDCS_MAX_ERRORS 10

struct sdcs_reading {
	bool     have_status;
	uint8_t  status;
	bool     have_alarm;
	uint8_t  alarm;
	bool     have_errors;
	uint8_t  n_errors;
	uint8_t  errors[SDCS_MAX_ERRORS];
	bool     have_gas;
	int32_t  gas_ppm_x100;   /* ppm * 100, signed */
	bool     have_raw;
	uint16_t raw_active;
	uint16_t raw_reference;
	bool     have_temp;
	bool     temp_sensor_error;  /* raw byte was 0xFF */
	int16_t  temp_c;             /* raw - 127 */
};

/* Bind the client to an already-ready UART device (9600 8N1). */
int sdcs_init(const struct device *uart_dev);

/* Raw request/response. Returns response data length, or negative errno.
 * A slave Error Packet returns -EPROTO and stores the code in *slave_err.
 * Pass slave_err = NULL if you do not care.
 */
int sdcs_transact(uint8_t cmd, const uint8_t *req, size_t req_len,
		  uint8_t *resp, size_t resp_max, uint8_t *slave_err);

/* ASCII getters (GET_MODEL_NAME, GET_PROD_NAME, GET_FW_VER, GET_SEN_SN). */
int sdcs_get_ascii(uint8_t cmd, char *out, size_t out_sz);

/* GET_RUN_TIME - total powered seconds, MSB first. */
int sdcs_get_run_time(uint32_t *seconds);

/* GET_DATA_PACK. bitmap is any OR of SDCS_DP_*. */
int sdcs_get_data_pack(uint16_t bitmap, struct sdcs_reading *out);

/* WRITE_PROTECT: enable=false unlocks for ~300 s, then it re-locks itself. */
int sdcs_write_protect(bool enable);

/* SET_SEN_RTC. Must be called after every sensor power-on or the
 * "time not synchronized" alarm bit stays set. Requires write protect off.
 * year is the full year (e.g. 2026); the wire format carries year-2000.
 */
int sdcs_set_rtc(uint16_t year, uint8_t month, uint8_t day,
		 uint8_t hour, uint8_t minute, uint8_t second);

/* CRC-16/BUYPASS: poly 0x8005, init 0x0000, no reflection, xorout 0x0000.
 * Check value for "123456789" is 0xFEE8.
 */
uint16_t sdcs_crc16(const uint8_t *buf, size_t len);

#endif /* SDCS_H_ */
