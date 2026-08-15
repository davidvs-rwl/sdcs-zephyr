# Engineering decision log — Mulberry bring-up on nRF9151-DK

Decisions taken during milestone 1 (prove the UART link, identify the sensor,
poll readings to console), with the evidence behind each one.

Toolchain: nRF Connect SDK v3.4.0, Zephyr 4.4.0, board target
`nrf9151dk/nrf9151/ns`.

Each entry records what we believed going in, what the evidence actually
showed, what we chose, and what the failure would have cost. Several entries
record us being wrong. Those are the useful ones.

---

## 1. Pin selection: D6/D7 (P0.06/P0.07) for the sensor UART

**What we believed.** That the nRF9160-DK is a close enough model for the
nRF9151-DK to reason from. It is not, and that assumption had already produced
one wrong pin assignment before this history begins.

**What the evidence showed.** Reading the board files for the version actually
being built is the only check that holds. Two things fell out of doing that:

- Only **P0.02, P0.03, P0.06, P0.07** are unclaimed and brought out to the
  Arduino digital header. Everything else is taken — P0.00/01/04/05 are LEDs,
  P0.08/09/18/19 buttons, P0.10–P0.13 `arduino_spi` (spi3), P0.14–P0.17
  uart0/uart1 RTS/CTS, P0.26/27 the console, P0.28/29 VCOM1, P0.30/31 i2c2.
- The `arduino_header` `gpio-map` resolves index 12 → P0.06 and index 13 →
  P0.07, confirming D6/D7 are those pins on *this* board revision rather than
  by analogy with another.

A related correction: the project notes recorded the board files as Zephyr
v4.2.1. NCS v3.4.0 actually ships **Zephyr 4.4.0**. The pin map happened to
survive the version difference, but it was verified against 4.4.0 rather than
assumed from the note.

**What we chose.** UARTE2 on D6 (P0.06, TX) and D7 (P0.07, RX) — adjacent
header pins with no pinctrl contention.

**Cost of getting it wrong.** Zephyr does not error on pinctrl contention. Two
peripherals assigned the same pin builds clean and fails on the bench, where
the symptom is silence or corruption and the suspicion lands on wiring.

---

## 2. i2c2 and uart2 share one peripheral instance at `@a000`

**What we believed.** That verifying the pins was sufficient. The pin audit was
clean: decoding every `psels` value in the resolved devicetree showed only
`uart2_default` and `uart2_sleep` referencing P0.06 or P0.07, and no `gpios`
property claiming either.

**What the evidence showed.** The conflict was one level up from the pins. On
the nRF91 series, UARTE2 / SPIM2 / SPIS2 / TWIM2 / TWIS2 are one peripheral,
not five. In the resolved devicetree:

```
uart2: uart@a000    reg = <0xa000 0x1000>   interrupts = <0xa 0x1>   status = "okay"
i2c2:  i2c@a000     reg = <0xa000 0x1000>                            status = "okay"
```

Same register block, same IRQ 10. The board enables `i2c2` as `arduino_i2c` on
D14/D15, so enabling `uart2` put two Zephyr drivers on the same registers. The
pins do not overlap — 30/31 versus 06/07 — so nothing in the pinctrl layer had
anything to complain about. The TF-M interrupt handler name for this instance,
`UARTE2_SPIM2_SPIS2_TWIM2_TWIS2_IRQHandler`, states the sharing outright.

**What we chose.** Disable `i2c2` in the board overlay (`dbbe6b3`). Nothing in
this milestone uses the Arduino I2C header.

**Cost of getting it wrong.** It builds clean and flashes clean. Whichever
driver initialises second reconfigures the peripheral out from under the first,
at runtime, with no diagnostic. This is the same class of silent failure as pin
contention, but pinctrl cannot see it — pinctrl compares pins, and these
conflict on registers.

**The same trap sits one instance over.** `spi3` and `uart3` are both at
`@b000` on IRQ 11. `uart3` is currently disabled, so there is no conflict
today. If uart2 ever becomes unusable and uart3 is the fallback, `arduino_spi`
must be disabled first.

**Generalisation.** Checking pins is not checking peripherals. Compare `reg`
addresses across every enabled node, not just `psels`.

---

## 3. MCUboot is not optional on this target

**What we believed.** That a build with no DFU requirement needs no bootloader,
and that `SB_CONFIG_BOOTLOADER_MCUBOOT` being unset was therefore correct.

**What the evidence showed.** The `ns` board target lays out for the with-BL2
flash map whether or not BL2 is built. `nrf9151dk_nrf9151_ns.dts` sets
`zephyr,code-partition = &slot0_ns_partition`, pulling in
`nrf91xx_partition.dtsi`, which hardcodes:

```
0x00000000  BL2 - MCUboot             (64 KB)
0x00010000  secure image primary     (256 KB)
0x00050000  non-secure image primary (192 KB)
```

TF-M's own `flash_layout.h` documents both maps. Without BL2 it expects the
secure image at `0x00000000` and the non-secure image at `0x00078000`. Our
images landed at `0x10000` and `0x50000` — the **with-BL2** map — while
`SB_CONFIG_BOOTLOADER_MCUBOOT` was unset, `BL2:BOOL=FALSE`, and `domains.yaml`
listed only the application. The build reserved 64 KB for a bootloader and then
did not produce one.

Reading the device back showed the consequence. The flashed hex covered:

```
0x00010000 - 0x00020400   TF-M secure
0x00050000 - 0x0005AA70   non-secure app
```

Nothing at `0x00000000`. But flash there was not empty:

```
0x00000000: 20003FE8 00002009 000061D3 00001FF5
```

A valid vector table — SP `0x20003FE8`, reset handler `0x00002009` — that we
never flashed. `west flash` erases only the address ranges its own image
touches, so a stale image from earlier work survived untouched at the reset
vector. The CPU booted that instead.

Locating it required proving where execution actually was. **RTT was not
compiled in** (`# CONFIG_USE_SEGGER_RTT is not set`), so an RTT check would
have been silent whether or not the firmware ran — a guaranteed false negative.
Sampling the program counter over SWD instead:

```
PC: 0x0000005a50   (three samples)
PC: 0x0000005a50   after nrfutil device reset
```

Pinned, and below `0x10000` — inside the stale image, not our TF-M at
`0x10000` and not our app at `0x50000`.

**What we chose.** `nrfutil device erase --all` to clear the leftover, then
`sysbuild.conf` with `SB_CONFIG_BOOTLOADER_MCUBOOT=y` (`f2bb1c7`). After the
fix the layout closes:

```
mcuboot       0x00000000 - 0x00007954
app (signed)  0x00010000 - 0x0005AB08
```

TF-M moved from `0x10000` to `0x10200` — the `0x200` gap is the MCUboot image
header that was previously absent, which is what a bootloader at `0x0` would
have been looking for and failing to find.

**Cost of getting it wrong.** A build and flash that both report success,
followed by a device that does nothing, with a blank console and no other
symptom. The blank console invites a hunt through UART configuration, which is
where several hours can go before anyone checks the reset vector.

---

## 4. `west flash` exiting 0 does not mean the firmware runs

**What we believed.** That a successful flash — including nrfutil's own
"Verifying image" step and a silent `nrfutil device fw-verify` exit 0 — was
evidence the application was running.

**What the evidence showed.** Every one of those checks passed while the device
executed a stale image at `0x0`. They are all honest about what they measure:
the bytes we sent arrived at the addresses we sent them to. None of them makes
any claim about `0x0`, because our hex said nothing about `0x0`. `fw-verify`
prints nothing on success, so its silence is easy to read as more assurance
than it is.

**What we chose.** Treat flashing and executing as separate claims requiring
separate evidence:

1. **What is programmed** — read the device back (`nrfutil device read`), and
   enumerate the segments in the hex being flashed. Confirm they cover the
   reset vector.
2. **What is executing** — sample the PC over SWD and resolve it against the
   ELF.

**Cost of getting it wrong.** Trusting exit codes puts the search in the wrong
subsystem entirely. The console was blank because nothing was running; every
minute spent on UART configuration was spent on a working subsystem.

---

## 5. The loopback test measured its own buffering, not the pins

**What we believed.** That the pin-verification test was sound: transmit a real
9-byte `GET_MODEL_NAME` frame with TX jumpered to RX, then read the echo and
compare.

**What the evidence showed.** It failed, reproducibly, in a way that looked
like a wiring fault and was not:

```
sent: 7B 59 06 00 00 10 AA 26 7D
recv: 7B 59 06 00 00 10 AA    7D (8/9 bytes)
```

Eight of nine bytes byte-exact and in order. The dropped byte was `0x26` —
position 8 of 9, the CRC low byte — and it was the same byte on every one of
four iterations.

That pattern excludes the causes the test itself suggested. A missing jumper
gives zero bytes. A baud mismatch corrupts bit patterns, so the surviving bytes
would be garbage rather than perfect. Pin contention had already been
eliminated in the devicetree. Seven correct bytes followed by a correct `0x7D`
can only happen if the pins, the jumper and the baud rate are all right.

The fault was in the test's structure. `uart_poll_out()` blocks until each byte
is on the wire, so transmitting all nine before reading any meant the entire
frame arrived before anything drained it. The result therefore depended on how
much the UARTE poll-mode RX path happens to buffer — a property of the driver,
not of the wiring the test exists to check.

**What we chose.** Send one byte, read it back, then send the next, keeping at
most one byte in flight (`ba00d54`). Same jumper, same pins, same baud, same
frame: **9/9 byte-exact**, repeatedly.

Two supporting changes. The echo poll uses `k_busy_wait(100)` rather than
`k_sleep(K_MSEC(1))`, because the default tick is 10 ms and would have allowed
only a handful of polls inside the per-byte deadline. And the failure message
now names the byte index that got no echo, so byte 0 (jumper or wrong header
pins) is distinguishable from a later byte (intermittent contact). The previous
hint text — "partial/garbage → baud mismatch or pin contention" — pointed at
the wrong cause for exactly the failure this test produced.

**Cost of getting it wrong.** A self-test that fails on good hardware is worse
than no self-test. It sends you to re-seat connectors and re-measure a link
that was working, and it discredits the test for the case where the wiring
genuinely is bad.

**Generalisation.** A test intended to measure one layer must not depend on the
behaviour of another. Bounding the frame to one byte in flight removed the
buffering variable entirely.

---

## 6. Voltage: VDD_GPIO at 3.3 V, sensor VCC at 5 V

**What we believed.** Nothing initially — this DK behaves differently enough
from the nRF9160-DK that it needs stating explicitly.

**What the evidence showed.** The nRF9151-DK has **no voltage-select switch**,
unlike the nRF9160-DK. It defaults to **1.8 V**, and the level is changed in
nRF Connect for Desktop → Board Configurator → VDD_GPIO. Two independent
constraints bracket the choice:

- **nRF drives sensor RX.** The Mulberry's RX threshold is **2.0 V minimum**.
  At VDD_GPIO 1.8 V the nRF cannot drive high enough to register, so commands
  are never received.
- **Sensor drives nRF RX.** The Mulberry's TX is **2.9 V maximum** (2.8 V
  measured on this unit) against the nRF's absolute maximum of **VDD + 0.3 V**.
  At 1.8 V that ceiling is 2.1 V, so the sensor overdrives the nRF pin — not a
  signalling error but an abs-max violation.

The 1.8 V default therefore fails in both directions at once.

**What we chose.** VDD_GPIO at **3.3 V**, measured and confirmed before the
sensor was connected. Both 3.0 V and 3.3 V satisfy the constraints; 3.3 V has
more headroom on the side that damages hardware rather than merely
miscommunicating:

| VDD_GPIO | abs-max ceiling (VDD+0.3) | margin over 2.9 V sensor TX |
| --- | --- | --- |
| 1.8 V | 2.1 V | **−0.8 V — violation** |
| 3.0 V | 3.3 V | 0.4 V |
| 3.3 V | 3.6 V | 0.7 V |

Sensor RX threshold is cleared with room to spare at either level.

**Sensor VCC at 5 V, not 3 V.** The supply range is 3–5 V, but the PVD
threshold is **2.85 V minimum**. Below it the sensor still answers UART and
still returns well-formed frames — it simply performs no gas measurement. A
3 V supply leaves 0.15 V of margin against that threshold, which any wiring
drop or transient can erode. 5 V leaves 2.15 V.

**Cost of getting it wrong.** In the 1.8 V case, either no communication at all
or an over-voltage on an nRF input pin. In the 3 V supply case, something
worse: a sensor that responds correctly to every command and reports plausible
frames while measuring nothing. That failure is invisible at the protocol layer
and would be discovered, if at all, only by disbelieving the readings.

Note the loopback test cannot check any of this. The nRF drives and reads its
own pin through the jumper, so it passes at 1.8 V exactly as happily as at
3.3 V. Voltages are a meter check, not a firmware check.

---

## 7. The −2000 ppm reading is a sensor offset, not a decode fault

**What we believed.** Uncertain, and deliberately held so. The first readings
were `-2000.00` ppm on every poll, bit-identical, while the raw counts beneath
them moved. Two explanations fit equally well from the console: a genuine
negative zero offset in the sensor, or a scaling or offset error in our decode.
Both produce a stable negative number with healthy-looking neighbours.

**What the evidence showed.** Dumping the response frame before parsing settled
it. One frame, with the field walk done by hand rather than by the parser:

```
7B 59 13 03 A8 30 | 00 04 00 FF FC F2 C0 02 62 65 44 3D 95 | 85 F9 7D
SOP Ver Len Idx   Cmd        payload (13 B)                   CRC   EOP
```

| Offset | Bytes | Field | Value |
| --- | --- | --- | --- |
| 0 | `00` | status | no WARMUP / IN_CAL / UNSTABLE_ENV |
| 1 | `04` | alarm | bit 2 = `TIME_NOT_SYNC` |
| 2 | `00` | error count | 0 — no error bytes follow |
| 3–6 | `FF FC F2 C0` | gas | −200000 → **−2000.00 ppm** |
| 7 | `02` | channels | 2 |
| 8–9 | `62 65` | raw active | 25189 |
| 10–11 | `44 3D` | raw reference | 17469 |
| 12 | `95` | temperature | 149 − 127 = 22 °C |

The accounting closes exactly. `Len` = `0x13` = 19 = total − 3. Payload length
= 22 − 9 = 13. The six fields consume 1+1+1+4+1+4+1 = **13 bytes**, with
nothing left over and nothing short. A misaligned gas field could not land on
that boundary, and the neighbouring fields could not simultaneously produce
25189 / 17469 / 22 °C. CRC-16/BUYPASS was recomputed independently over three
different frames — data pack, serial, model — all matching.

`FF FC F2 C0` is −200000 as a signed 32-bit big-endian integer, and the field
is ppm × 100. The sensor is transmitting −2000.00 ppm. Our decode is correct.

**The quantization argument.** The remaining oddity was that the gas bytes were
bit-identical on every poll while the raw counts jittered by ±1–2. That is
consistent rather than suspicious: the sensor's resolution is **500 ppm**, and
−2000 ppm is exactly −4 resolution steps. Raw jitter of a couple of counts is
far below one step, so the quantized output stays locked while the underlying
signal moves. A value that moved with the raws would in fact have been the
surprising result.

**What we chose.** Record the reading as a sensor zero offset — roughly four
resolution steps below true zero, on a unit produced 2024-01-26 with 308 hours
of runtime — and leave calibration alone. Zeroing writes to NVM behind write
protection, and is not something to do speculatively. The diagnostic that
settled it is kept in the transaction layer behind `SDCS_LOG_RAW_FRAMES`,
default 0 (`0c96c62`).

**Cost of getting it wrong.** In either direction. Assuming a decode bug means
rewriting correct parsing code until the symptom moves. Assuming a sensor
offset means calibrating around a software fault and baking the error into the
calibration, where it becomes much harder to find.

**Generalisation.** When a parsed value is suspect, the cheapest decisive test
is to log the bytes before parsing and walk the fields by hand. Field accounting
that closes exactly on the declared payload length is strong evidence of
alignment; a parser reporting its own offsets is not.

---

## Verification methods

Three checks that each caught something in this bring-up.

### Confirming a devicetree resolved as intended

Read the **generated** tree, not the sources: `build/<image>/zephyr/zephyr.dts`.
It is fully resolved and every property carries a comment naming the file and
line it came from, so overlay precedence is visible rather than inferred.

- **Pin assignment.** Decode `psels` values. The encoding is
  `(fun << 24) | (port * 32 + pin)` — `0x6` is `UART_TX` on P0.06, `0x1000007`
  is `UART_RX` on P0.07. Decode *every* pinctrl state in the tree, not just the
  one being added, and check no other state names the same pin.
- **GPIO claims.** Pinctrl is not the only thing that takes pins. Decode every
  `gpios` property against the `gpio0` phandle — LEDs, buttons and chip selects
  live there.
- **Header positions.** Resolve the `arduino_header` `gpio-map` rather than
  trusting a silkscreen-to-GPIO table from another board.
- **Peripheral instances.** Compare `reg` addresses across all enabled nodes.
  Nodes at the same address are the same hardware regardless of node name or
  pins, and this is invisible to pinctrl. `uart@a000` and `i2c@a000` are one
  peripheral.
- **Chosen nodes.** Confirm `zephyr,console` and friends still point where you
  expect after an overlay lands.

### Proving firmware is executing, without RTT

RTT requires `CONFIG_USE_SEGGER_RTT`. If it is not set, RTT silence proves
nothing — it is a false negative, not a diagnosis. Check the config before
relying on it.

Sample the program counter over SWD instead:

```
nrfutil device cpu-register-read --register PC --serial-number <SN>
```

Then interpret it against the ELF, because a constant PC is not by itself a
hang:

```
arm-zephyr-eabi-addr2line -f -e build/<image>/zephyr/zephyr.elf <PC>
arm-zephyr-eabi-nm -nC build/<image>/zephyr/zephyr.elf     # enclosing symbol
```

Both a hang and a healthy idle loop show a stationary PC. The distinction is
*where*: `0x5A50`, below the image range, was a hang in stale flash; `0x57AA4`
resolving to `arch_cpu_idle` was the application sleeping between polls. Check
the address falls inside the running image's range, then resolve the symbol.

Pair it with a readback of what is actually programmed, including the reset
vector:

```
nrfutil device read --address 0x0 --bytes 32 --serial-number <SN> --direct
```

### Testing a UART link before connecting anything

Jumper TX to RX on the header and echo a frame back to yourself. This proves
the peripheral instance, the pinctrl assignment, the physical header position
and the baud rate in one shot, with nothing attached that a wrong pin could
damage. In this project: `LOOPBACK_TEST 1` in `src/main.c`, jumper D6 to D7.

Two requirements for the test to mean anything:

- **Keep at most one byte in flight.** Send a byte, read it back, then send the
  next. Transmitting a whole frame before reading makes the result depend on
  driver buffering rather than on the wiring — see decision 5.
- **Report *which* byte failed.** "No echo for byte 0" is a jumper or a wrong
  pin. A failure partway through is intermittent contact or contention. A byte
  count alone does not separate them.

What it does **not** test: signal levels. The nRF drives and reads its own pin,
so the test passes at any VDD_GPIO setting. Levels are a meter check before
anything is connected.
