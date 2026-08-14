# Mulberry CH4 bring-up on nRF9151-DK

Milestone 1 firmware: prove the UART link to the eLichens Mulberry
(MULBERRY-13 / MUL13442-Safety-CH4-5-vol) and stream parsed readings to the
VCOM console. No modem, no power gating, no storage yet.

## Before you flash

1. **Set VDD_GPIO to 3 V.** The nRF9151-DK has *no* voltage-select switch
   (unlike the nRF9160-DK). It boots at **1.8 V** by default. Change it in
   **nRF Connect for Desktop -> Board Configurator -> VDD_GPIO -> 3 V**.
   At 1.8 V the sensor's RX threshold (2.0 V min) is not met, and the
   sensor's ~2.8 V TX output would exceed the nRF's abs-max of VDD+0.3 V.

2. **Meter check before connecting anything.**
   - DK VDD/GPIO rail reads ~3.0 V
   - Mulberry TX (pin 3) idles high at ~2.8 V with the sensor powered
     from 5 V and TX not yet connected to the DK

3. **Socket the sensor.** Never solder it. Mill-Max
   0364-0-15-15-13-27-10-0 (per eLichens support) or equivalent.

## Wiring

| Mulberry pin | Signal | nRF9151-DK        | Note                                  |
| ------------ | ------ | ----------------- | ------------------------------------- |
| 1            | VCC    | 5V                | 3-5 V range; 5 V keeps you clear of the 2.85 V PVD threshold |
| 2            | GND    | GND               | common ground is mandatory            |
| 3            | TX     | D7 = P0.07 (nRF RX) | crossover                           |
| 4            | RX     | D6 = P0.06 (nRF TX) | crossover                           |
| 5            | IO/CS  | leave floating    | internal pull-down; only needed to share one UART across sensors |

## Verify the pins before wiring

Jumper **D6 to D7** on the Arduino digital header, set `LOOPBACK_TEST 1` in
`src/main.c`, build and flash. A PASS proves the UARTE2 instance, the pinctrl
assignment, the physical header position and the 9600 baud rate in one shot,
with no sensor attached. Then set it back to 0.

D6/D7 (P0.06/P0.07) are the only adjacent pins on the header that no stock
board peripheral claims. P0.10-P0.13 belong to `arduino_spi` (spi3), which the
board enables by default; P0.14-P0.17 are uart0/uart1 RTS/CTS.

## Build

```
west build -b nrf9151dk/nrf9151/ns
west flash
```

Console: 115200 8N1 on the first VCOM port.

## Expected output

```
CRC-16/BUYPASS self test: FEE8 (expect FEE8) OK

--- identity ---
model      : MULBERRY-13
product    : eLichens NDIR
serial     : SN03160399
firmware   : V2.00F
run time   : ... s

--- polling at 1000 ms ---
  ppm     act    ref   T(C)  flags
    0.00  15520   6149    25  WARMUP,TIME_NOT_SYNC
```

`WARMUP` clears about 30 s after sensor power-on; gas reads 0 until then.
`TIME_NOT_SYNC` stays set until `SET_SEN_RTC` runs, which needs a real time
source - see `SET_RTC_AT_BOOT` in `main.c`.

## Protocol notes worth keeping

- Frame: `SOP(0x7B) Ver(0x59) Len Index(2) Cmd Data CRC(2) EOP(0x7D)`
- `Len` = total frame length - 3
- CRC-16/BUYPASS (poly 0x8005, init 0x0000, no reflection, xorout 0x0000),
  computed over SOP..Data, transmitted **MSB first**.
  Verified against all ten worked examples in Appendices VI and VII of
  the protocol document rev2.5.
- The sensor never speaks unprompted. Silence on the line means either
  wiring, baud, or a malformed request - not a sleeping sensor.
- Response deadline is 250 ms; three consecutive timeouts means power-cycle,
  soft reset will not help.
- Do not set bit 6 (humidity) in the GET_DATA_PACK bitmap - Bilberry only.
- Gas reading is a signed 32-bit big-endian integer of ppm x 100.
- Temperature byte is `actual_C + 127`; 0xFF means temperature sensor error.
