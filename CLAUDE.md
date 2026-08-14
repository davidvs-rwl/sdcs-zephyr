# CLAUDE.md

Project context for Claude Code sessions. Read this before making changes.

## What this is

Firmware for reading an **eLichens Mulberry** NDIR methane sensor over UART
from an **nRF9151-DK**, using eLichens' SDCS protocol.

Owner: Redwood Labs Inc. (independent gas sensor test & characterization lab).
Intended to become a public GitHub repository.

Current milestone: **bench bring-up only.** Prove the UART link, identify the
sensor, poll readings to console. No modem, no power gating, no storage.
Field deployment (solar-powered, outdoor, LTE-M backhaul) comes later and
should not drive design decisions yet.

## Hardware

| | |
|---|---|
| MCU board | nRF9151-DK (PCA10171) |
| Board target | `nrf9151dk/nrf9151/ns` |
| Sensor | eLichens Mulberry, MULBERRY-13 (MUL13442-Safety-CH4-5-vol) |
| Sensor serial | SN03160399, firmware V2.00F, produced 2024-01-26 |
| Range | 0–5 vol% CH4 (0–50000 ppm), resolution 500 ppm |
| Sensor UART | 9600 8N1, no parity, no flow control |
| Console UART | uart0, 115200, VCOM0 |
| Dev host | MacBook |

A Raspberry Pi 5 (`rl-pi-001`) runs a known-good Python implementation of the
same protocol (`mulberry_sdcs.py`). When nRF behavior is ambiguous, the Pi is
the reference — compare bytes on the wire, don't guess.

## Hard constraints — do not violate

1. **`VDD_GPIO` must be 3 V.** The nRF9151-DK has no voltage-select switch
   (unlike the nRF9160-DK) and defaults to 1.8 V. It is changed in nRF Connect
   for Desktop → Board Configurator. At 1.8 V the sensor's RX threshold
   (2.0 V min) is not met and the sensor's ~2.8 V TX exceeds the nRF's
   abs-max of VDD+0.3 V.

2. **Sensor VCC is 5 V, not 3 V.** Supply range is 3–5 V, but the PVD
   threshold is 2.85 V min; below it the sensor answers UART but performs no
   gas measurement. 3 V leaves no margin.

3. **Never commit the eLichens documentation.** The protocol spec
   (`eLichens_Sensors_CommunicationProtocol`) and the Mulberry user manual are
   marked `[CONFIDENTIAL]` and are property of eLichens SA. No verbatim
   tables, no CRC lookup table copied from the appendix, no scanned pages, no
   quoted paragraphs. Implementing the protocol is fine; republishing the
   document is not. Add both filenames to `.gitignore`.

4. **Do not fabricate timestamps.** `SET_SEN_RTC` writes a date the sensor
   stores against calibration records. Leave `SET_RTC_AT_BOOT` at 0 until
   there is a real time source (modem `AT+CCLK` or GNSS). The
   `TIME_NOT_SYNC` alarm bit staying set is the correct state, not a bug.

## Verified pin map (nRF9151-DK, Zephyr v4.2.1 board files)

Only **P0.02 (D2), P0.03 (D3), P0.06 (D6), P0.07 (D7)** are unclaimed and
brought out to the Arduino header. Everything else is taken:

```
P0.00 LED1 + pwm0      P0.14 uart0 RTS (A0)   P0.26 uart0 RX (console)
P0.01 LED2             P0.15 uart0 CTS (A1)   P0.27 uart0 TX (console)
P0.04 LED3             P0.16 uart1 RTS (A2)   P0.28 uart1 RX (VCOM1)
P0.05 LED4             P0.17 uart1 CTS (A3)   P0.29 uart1 TX (VCOM1)
P0.08 Button1 (D8)     P0.18 Button3   (A4)   P0.30 i2c2 SDA (D14)
P0.09 Button2 (D9)     P0.19 Button4   (A5)   P0.31 i2c2 SCL (D15)
P0.10 spi3 CS  (D10)   P0.20 spi3 CS2
P0.11 spi3 MOSI(D11)   P0.21 nRF5340 reset (disabled)
P0.12 spi3 MISO(D12)
P0.13 spi3 SCK (D13)
```

**We use UARTE2 on D6 (P0.06, TX) and D7 (P0.07, RX).**

`arduino_spi` (spi3) is `status = "okay"` in the stock board devicetree and
owns P0.10–P0.13. Zephyr does **not** error on pinctrl contention — two
peripherals on the same pin builds clean and fails silently on the bench.
Always check the board `.dtsi` and `-pinctrl.dtsi` before assigning a pin,
and confirm against generated `build/*/zephyr/zephyr.dts` after building.

UARTE0 is the console and UARTE1 is VCOM1 / potentially claimed by TF-M, so
neither is available. If UARTE2 turns out to be secure-assigned on the `ns`
target, uart3 is the fallback.

## Wiring

| Mulberry pin | Signal | nRF9151-DK |
|---|---|---|
| 1 | VCC | 5V |
| 2 | GND | GND |
| 3 | TX | D7 = P0.07 (nRF RX) |
| 4 | RX | D6 = P0.06 (nRF TX) |
| 5 | IO/CS | leave floating (internal pull-down) |

Sensor must be socketed, never soldered. Mill-Max 0364-0-15-15-13-27-10-0
(confirmed by eLichens support; the user manual lists 0372-… — eLichens'
answer wins).

## SDCS protocol facts (verified, do not re-derive)

Frame: `SOP(0x7B) Ver(0x59) Len Index(2) Cmd Data CRC(2) EOP(0x7D)`

- `Len` = total frame length − 3 (counts everything after the Len byte)
- CRC is **CRC-16/BUYPASS**: poly `0x8005`, init `0x0000`, no input or output
  reflection, xorout `0x0000`. Check value for `"123456789"` is `0xFEE8`.
- CRC covers SOP through Data inclusive, and is transmitted **MSB first**.
- All multi-byte fields are big-endian.
- Verified against all ten worked example frames in the protocol document
  Appendices VI and VII. If a frame fails, suspect wiring or baud, not the
  CRC implementation.

Behavioral notes:

- The sensor is a pure slave. It **never** transmits unsolicited. Silence on
  the line means wiring, baud, or a malformed request — not a sleeping sensor.
- Response deadline is 250 ms. Three consecutive timeouts means the sensor
  must be power-cycled; soft reset does not work.
- On an Error Packet (`Cmd == 0x71`), do **not** retransmit — the same request
  produces the same error.
- Warm-up is 30 s from power-on; gas reads 0 and status bit B1 is set.
- Gas reading is a signed 32-bit big-endian integer of **ppm × 100**.
- Temperature byte is `actual_°C + 127`; `0xFF` means temperature sensor error.
- Do **not** set bit 6 (humidity) in the `GET_DATA_PACK` bitmap — Bilberry only.
- Write protection auto-re-enables after 300 s.
- After setting parameters, the sensor stalls its UART while committing to
  NVM. Wait ~1 s before the next command or it is silently lost.
- Poll at 1 Hz. The measure cycle is 500 ms, and >1 Hz wastes current budget.

## Build and run

```bash
west build -b nrf9151dk/nrf9151/ns
west flash
```

Console: 115200 8N1 on the first VCOM port (`/dev/tty.usbmodem*` on macOS).

Pin verification before wiring the sensor: set `LOOPBACK_TEST 1` in
`src/main.c`, jumper D6 to D7, flash. PASS confirms the UARTE2 instance,
pinctrl assignment, physical header position and baud rate with nothing
attached. Set it back to 0 afterwards.

## Layout

```
src/sdcs.h     protocol constants and API
src/sdcs.c     framing, CRC, transaction layer
src/main.c     application: identify, then poll
boards/nrf9151dk_nrf9151_ns.overlay   UARTE2 pin assignment
```

`sdcs.c/h` is a reusable client library and should stay free of
application logic and free of nRF-specific assumptions beyond the Zephyr
UART API.

## Conventions

- Zephyr/Linux kernel C style: tabs, 80 columns, `snake_case`.
- Return negative errno on failure; never swallow an error silently.
- Comments explain *why*, and cite the protocol section when a magic number
  comes from the spec.
- Prefer failing loudly at bring-up over defensive fallbacks that mask a
  wiring fault.

## Working style

- Verify against primary sources rather than reasoning by analogy from
  similar boards. The nRF9160-DK is **not** a reliable model for the
  nRF9151-DK — that assumption already produced one wrong pin assignment.
- State uncertainty explicitly rather than guessing plausibly.
- One milestone at a time, finished, before starting the next.
