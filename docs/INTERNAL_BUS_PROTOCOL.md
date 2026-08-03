# Internal Bus Protocol Analysis

Analysis of the MCU-to-panel communication protocol based on Saleae logic capture data.

## Capture Setup

- **Channels captured:** Data line (MCU→panels), Up Left, Up, Left, Center, Right, Down, Down Right
- **Missing channels:** Up Right, Down Left (not enough probes)
- **Capture duration:** ~39.5 seconds (service-menu capture); additional targeted captures per command type
- **Data bus:** UART 250000 baud, 8N1 (confirmed)
- **CSV export format:** Hex mode (0xNN values); ASCII mode produces '.' for non-printable bytes which are unrecoverable

## Data Line: UART at 250000 Baud (8N1)

The data bus is **standard UART at 250000 baud, 8N1** (8 data bits, no parity, 1 stop bit).

At this baud rate, one bit = 4µs. A UART frame is 10 bits (start + 8 data + stop) = 40µs.

### Idle State: Continuous 0x00 Transmission

When idle, the MCU continuously transmits `0x00` bytes. This produces the characteristic "clock-like" pattern:

```
UART 0x00 frame at 250kbaud:
Start  D0 D1 D2 D3 D4 D5 D6 D7  Stop
  0     0  0  0  0  0  0  0  0    1
  ├─────────── 36µs ───────────┤ 4µs │
  LOW LOW LOW LOW LOW LOW LOW LOW LOW HIGH
```

This looks like a 25 kHz clock (36µs LOW + 4µs HIGH) but is actually just UART transmitting zeros. The continuous 0x00 stream serves as:
- A timing reference for panels to synchronize to
- A "no command" indicator
- Clock pulses during sensor test data readback

## Data Line: Command Protocol

### Command Framing

**BREAK conditions follow each command as a terminator.** Each command on the bus is:

```
CMD BYTE(S) + PAYLOAD → BREAK (0x00 with framing error) → next CMD (immediately)
```

The BREAK appears as a single `0x00` byte with a UART framing error flag (stop bit was LOW instead of HIGH), always at the last byte of every command. Commands are sent back-to-back with no inter-command idle gap — the BREAK is the only delimiter.

Evidence from a single 935-byte captured chunk (bus_probe test 3):
```
[  0]: 0x34 = '4' command
[244]: 0x00 [BREAK]         ← end of '4'
[245]: 0x77 = 'w' command
[498]: 0x00 [BREAK]         ← end of 'w'
[499]: 0x32 = '2' command
[716]: 0x00 [BREAK]         ← end of '2'
[717]: 0x33 = '3' command
[934]: 0x00 [BREAK]         ← end of '3'
```

Four separate commands (`'4'`, `'w'`, `'2'`, `'3'`) concatenated back-to-back, each terminated by a BREAK.

> **Note on earlier captures:** A previous service-menu capture described BREAK as *preceding* commands. That was a misreading — the BREAK belongs to the end of the *prior* command, not the start of the *next* one. Across all bus-probe captures (12 tests, hundreds of command chunks), BREAK is always the final byte.

### Normal Operation Commands (Panel LED Lighting)

During normal operation, the MCU cycles through three commands at ~30 Hz to update panel LEDs:

| Command | Hex | Purpose | Payload bytes (total incl. cmd + BREAK) |
|---------|-----|---------|----------------------------------------|
| `'4'` | 0x34 | Inner 3×3 LED grid (firmware v4+ / 25-LED panels) | 9 panels × 9 LEDs × 3 bytes RGB + `'\n'` + BREAK = **245** |
| `'2'` | 0x32 | Top half of panel LEDs (outer rows 0-1 of 4×4 grid) | 9 panels × 8 LEDs × 3 bytes RGB + `'\n'` + BREAK = **218** |
| `'3'` | 0x33 | Bottom half of panel LEDs (outer rows 2-3 of 4×4 grid) | 9 panels × 8 LEDs × 3 bytes RGB + `'\n'` + BREAK = **218** |

Cycle order: `'4'` → `'2'` → `'3'` → `'4'` → ... at 30 FPS.

This matches `SMXManager::SetLights()` which splits panel LED data into three commands. The MCU receives the lighting command over USB and forwards it to the panels over this UART bus.

**Total cycle wire time:**

| Segment | Bytes |
|---------|-------|
| `'4'` command | 245 |
| `'2'` command | 218 |
| `'3'` command | 218 |
| **Full cycle** | **681** |

At 250 kbaud / 8N1 (40 µs per byte): 681 × 40 µs = **27.24 ms minimum** per cycle. At 30 fps (33.33 ms) this leaves ~6 ms of idle time per cycle.

**RGB values are pre-scaled on the bus:**

The MCU applies `LED_COLOR_SCALE = 0.6666` to all RGB values before transmitting. A bus value of `0xAA` (170) represents original color 255. Multiply bus RGB by `1 / 0.6666 ≈ 1.5` to recover the original unscaled values.

**LED buffer layout (25-LED mode, per panel, 75 bytes):**

| Bytes in API buffer | LEDs | Bus command | Color in distinctive-pattern test |
|--------------------|------|-------------|----------------------------------|
| 0–23 (LEDs 0–7) | Outer top half | `'2'` | GREEN |
| 24–47 (LEDs 8–15) | Outer bottom half | `'3'` | BLUE |
| 48–74 (LEDs 16–24) | Inner 3×3 grid | `'4'` | RED |

### Platform Strip Lights

`SMX_SetPlatformLights()` sends USB command `'L' + strip_index + count + RGB`. The MCU **does not forward this to the panels over the data bus** — the platform strips are driven directly by the MCU via its own LED output and are invisible on the panel UART bus. Captures of `SMX_SetPlatformLights()` calls show only the normal `'4'`/`'2'`/`'3'` heartbeat on the bus.

### Command Parsing

Commands are back-to-back with no inter-command idle. In the heartbeat (500ms all-black lights), `'4'` and `'2'` are typically sent with < 1ms between them and appear as a single 463-byte chunk in Saleae:

- **463B** = `'4'`(245B) + `'2'`(218B) merged (< 1ms gap)
- **681B** = full `'4'`+`'2'`+`'3'` cycle merged
- **935B** = lighting cycle + `'w'` config write merged

**Command byte false-positive hazard:**

The command bytes `0x32`, `0x33`, `0x34` are valid mid-brightness RGB color values. Parsing these naively from the raw stream will produce false positives inside lighting payloads. Use byte-count tracking or BREAK detection to reliably find command boundaries.

## Complete Internal Bus Command Reference

Confirmed from logic captures with smx-bus-probe (all commands verified with BREAK at last byte):

| Command | Hex | Bus format | USB source | Notes |
|---------|-----|-----------|------------|-------|
| `'4'` + data | 0x34 | `0x34` + 243B RGB + `0x0A` + `0x00[BREAK]` | `SMX_SetLights2` | Inner 3×3 LEDs; 9 panels × 9 LEDs × 3B |
| `'2'` + data | 0x32 | `0x32` + 216B RGB + `0x0A` + `0x00[BREAK]` | `SMX_SetLights2` | Outer top LEDs; 9 panels × 8 LEDs × 3B |
| `'3'` + data | 0x33 | `0x33` + 216B RGB + `0x0A` + `0x00[BREAK]` | `SMX_SetLights2` | Outer bottom LEDs; 9 panels × 8 LEDs × 3B |
| `'l'` | 0x6C | `0x6C 0x00[BREAK]` (2B) | `SMX_SetPanelTestMode` | Lights off; sent before enabling panel test mode |
| `'T1'` | 0x54 0x31 | `0x54 0x31 0x00[BREAK]` (3B) | `SMX_SetPanelTestMode(PressureTest)` | Panel test ON; resent every ~1s by MCU |
| `'T0'` | 0x54 0x30 | `0x54 0x30 0x00[BREAK]` (3B) | `SMX_SetPanelTestMode(Off)` | Panel test OFF |
| `'B0P'` | 0x42 0x30 0x50 | `0x42 0x30 0x50 0x00[BREAK]` (4B) | `SMX_SetTestMode(UncalibratedValues)` | Sensor test; see format below |
| `'B1P'` | 0x42 0x31 0x50 | `0x42 0x31 0x50 0x00[BREAK]` (4B) | `SMX_SetTestMode(CalibratedValues)` | Sensor test calibrated |
| `'B2P'` | 0x42 0x32 0x50 | `0x42 0x32 0x50 0x00[BREAK]` (4B) | `SMX_SetTestMode(Noise)` | Sensor test noise |
| `'B3P'` | 0x42 0x33 0x50 | `0x42 0x33 0x50 0x00[BREAK]` (4B) | `SMX_SetTestMode(Tare)` | Sensor test tare |
| `'C'` | 0x43 | `0x43 0x00[BREAK]` (2B) | `SMX_ForceRecalibration` | Force recalibration; MCU follows with `'w'` config write |
| `'w'` + config | 0x77 | `0x77 0xFA 0xVV` + 250B SMXConfig + `0x00[BREAK]` (254B) | `SMX_SetConfig` / periodic MCU | Config write; see format below. Byte[2] varies by source. |
| `'G'` sync | 0x47 | 64B structured format + `0x00[BREAK]` | MCU, whenever nothing is driving lighting | Animation frame-sync broadcast; see below |
| `'R'` | 0x52 | `0x52 0x00[BREAK]` (2B) | — (not in SDK) | Effect unverified, see below. Seen at boot, but not boot-only. |
| `'U'` | 0x55 | `0x55 0x00[BREAK]` (2B) | — (not in SDK) | Purpose unknown; see below. |

**`'B?P'` sensor test format confirmed:** The middle byte exactly mirrors the USB sensor test mode character — `'0'` uncalibrated, `'1'` calibrated, `'2'` noise, `'3'` tare. Captured and verified across all four test modes.

**`'C'` recalibration command:** The command is `'C'` (0x43). The USB-level command is `"C\n"` (from `SMXDevice::ForceRecalibration()`). After `'C'`, the MCU automatically sends a `'w'` config write.

**`'U'` command (0x55): purpose unknown.** Format: `0x55 0x00[BREAK]` (2B).

An earlier version of this document described it as a 1-second heartbeat emitted
autonomously by the pad firmware. **That characterisation is withdrawn** — later captures
do not support it. What `'U'` is for has not been determined.

**`'R'` command (0x52): effect unverified.** Format: `0x52 0x00[BREAK]` (2B).

This was previously labelled "panel reset". That label is an **inference from position** — `'R'` is sent once at the start of the boot sequence, before the config write and the `'G'` broadcasts — and it has never been directly verified. It is a reasonable reading, but it is not an observation.

Two things have since been established:

- **It is not boot-only.** It also appears during normal operation.
- **It produced no observable effect across five separate occurrences.** In every case the animation frame counter continued incrementing on schedule, with no restart, and `'G'` cadence held at 32.7-32.9 ms against a 32.8 ms norm. In the three-event capture the panel signal lines showed **zero** transitions throughout, on lines that are known to respond to a sensor-test read, so they were being monitored correctly.

If `'R'` reset the panels, the most visible consequence would be the animation restarting from frame 0, and it does not. Either the reset is a no-op once the pad is already up, or the label is wrong.

One correlation worth not over-reading: a `'w'` config write follows each `'R'` by about a second. That is the MCU's own ~2-second autonomous config refresh, not a consequence of `'R'` — the offsets vary (1.12 s, 1.08 s, 1.23 s) and the writes continue on the same cadence with no `'R'` present.

### Config Write (`'w'`) Format

The internal bus config write has an extra byte compared to the USB protocol:

```
USB format:   'W' + size(1B) + SMXConfig(250B)
Bus format:   'w' + 0xFA   + VV + SMXConfig(250B) + 0x00[BREAK]
              (1B)  (1B)    (1B)  (250B)             (1B) = 254B total
```

The third byte (`VV`) **varies by software source**:

| Source | Observed byte[2] values |
|--------|------------------------|
| Mac SDK (stepmaniax-sdk-mp / rustmaniax-sdk) | `0x52` |
| Official SMX game software | `0x12`, `0x1E`, `0x6E` |

It is not a simple incrementing counter and is not a checksum over the config payload. It may be a software-version tag, a session token, or a firmware-internal routing byte. Its purpose is unknown.

**Decoded config from captures (hex mode, all values confirmed):**

```
masterVersion:              0x05 = 5
configVersion:              0x05 = 5
flags:                      0x03 = 0b00000011
debounceNodelayMs:          15
debounceDelayMs:            0
panelDebounceMicroseconds:  2000 µs
autoCalibrationMaxDeviation: 100
badSensorMinimumDelaySeconds: 15
autoCalibrationAveragesPerUpdate: 300
autoCalibrationSamplesPerAverage: 100
autoCalibrationMaxTare:     0xFFFF
enabledSensors[5]:          { 0x0F, 0x0F, 0xFF, 0x0F, 0x00 }
autoLightsTimeout:          0x07 = 7 × 128ms = 896ms
stepColor[9]:               all (170, 170, 170) = white (pre-scaled)
platformStripColor:         (0, 0, 128) = dark blue
autoLightPanelMask:         0x00BA = 5 panels active (bits 1,3,4,5,7)
panelRotation:              0

panelSettings[9] (loadCellLow/High, fsrLow[4], fsrHigh[4]):
  panels 0/2/6/8:  lc=70/80,    fsr_lo=217/217/217/217, fsr_hi=218/218/218/218
  panels 1/3/5/7:  lc=70/80,    fsr varies by panel (199-229 low, 200-230 high)
  panel 4 (center): lc=100/120, fsr_lo=217/217/217/217, fsr_hi=218/218/218/218
```

### Sensor Test Command and Response

**Command (sent at ~30 Hz):**
```
'B' mode_char 'P' 0x00[BREAK]     (4 bytes)
```

Where `mode_char` is: `'0'`=uncalibrated, `'1'`=calibrated, `'2'`=noise, `'3'`=tare.

After each `'B?P'` command, the MCU sends **81 null bytes** (`0x00`) as clock pulses, during which all panels transmit their sensor data simultaneously on the signal wires (see Signal Lines section below).

**Bus traffic pattern during sensor test:**

The `'B?P'` command is inserted between the `'3'` (end-of-cycle) and `'4'` (start-of-next-cycle) lighting commands. The clock burst that follows has **no BREAK of its own** — it runs directly into the next `'4'` command:

```
'4'[BREAK] '2'[BREAK] '3'[BREAK] 'B?P'[BREAK] 0x00×81 '4'[BREAK] '2'[BREAK] '3'[BREAK] ...
```

Because the clock burst and the following `'4'` command share the same BREAK terminator, a BREAK-delimited stream parser sees them as a single "command" of 325 bytes (81 clock bytes + 244 bytes of the `'4'` payload before its BREAK). This is expected — the 81-byte clock burst is not a real command and the 325-byte sequence should be discarded as a non-lighting command. The `'4'` data within it is not recoverable by a BREAK-delimited parser.

The 81-byte null burst is the MCU clocking out 80 bits of panel response data plus alignment; all bytes are 0x00 with no framing errors (clean clock signal).

### Panel Test Mode (`'T'` Command)

**Enable pressure test:**
1. `'l' 0x00[BREAK]` — clears panel lights (2B)
2. `'T1' 0x00[BREAK]` — enables pressure test mode (3B); MCU resends every ~1s

**Disable:**
1. `'T0' 0x00[BREAK]` — disables panel test mode (3B)

**Important:** When panel test mode is enabled via `SMX_SetPanelTestMode`, the MCU stops accepting lighting commands and **returns to emitting `'G'` frame-sync broadcasts** at 30Hz. This continues until `SMX_SetPanelTestMode(Off)` is called, after which the MCU polls for a short time then resumes normal lighting.

### Re-enable Auto Lights (`SMX_ReenableAutoLights`)

USB command: `"S 1\n"`. Effect on bus: the MCU stops accepting host lighting and returns to emitting `'G'` frame-sync broadcasts at 30Hz, at which point the panels resume playing their stored animation.

The switch is essentially immediate. Measured from the last host lighting frame to the first `'G'`: **9 ms with `"S 1\n"`, against 928 ms if nothing is sent.** That ~920 ms figure is the firmware's own timeout, which restores the stored animation on its own once host lighting stops. `"S 1\n"` simply skips the wait.

## Boot Sequence

### Boot Commands

| Command | Hex | Purpose |
|---------|-----|---------|
| `'R'` | 0x52 | Panel reset — sent once at start of boot |
| `'w'` + config | 0x77 | Config write — sent immediately after reset |
| `'G'` sync | 0x47 | Frame-sync broadcast — sent at 30Hz once panels are up |

**Boot sequence:**
```
t=0       Power on
t=2.4s    'R'              <- Reset all panels
t=2.4s    'w' + config     <- Write configuration to panels
t=2.4s    'G' sync         <- Begin frame-sync broadcasts (30Hz)
          ...              <- Continue until lighting takes over
t=???     '4','2','3'      <- Switch to lighting loop (normal operation)
```

### `'G'` Format: Animation Frame Sync

`'G'` was previously described here as a panel readiness poll that verifies the panels.
**That was wrong.** Nothing ever replies to `'G'` — across 376 consecutive `'G'` frames the
panel signal lines showed no activity at all — and a protocol that verifies nothing cannot
be a readiness check.

`'G'` is an **animation frame-sync broadcast**. It is 64 bytes plus the BREAK, and tiles
exactly as a command byte followed by **9 records of 7 bytes, one per panel**:

```
47                          'G'
00 00 00 FF FF 0A A6        panel 0  UpLeft      FF FF = panel not present
00 00 00 0B FF 00 01        panel 1  Up
00 00 00 FF FF 01 21        panel 2  UpRight
00 00 00 0B FF 00 01        panel 3  Left
00 00 00 0B FF 00 01        panel 4  Center
00 00 00 0B FF 00 01        panel 5  Right
00 00 00 FF FF 22 F9        panel 6  DownLeft
00 00 00 0B FF 00 01        panel 7  Down
00 00 00 FF FF 10 00        panel 8  DownRight
00 [BREAK]
```

63 payload bytes = 9 x 7 exactly. The five populated panels share an identical record; the
four unpopulated corners are flagged `FF FF`. Byte 3 of each populated record is a
**counter**.

**The counter is the animation frame index.** Uploading an 8-frame animation changed its
cycle from 0..23 to **0..7**, tracking the frame count exactly:

| | Counter range | Distinct values |
|---|---|---|
| previous animation | 0..23 | 24 |
| after uploading 8 frames | 0..7 | 8 |

Supporting observations, all from the same captures:

- **`'G'` and lighting commands are perfectly mutually exclusive.** Across three
  host-lighting windows, `'G'` frames inside them: 0, 0, 0. `'G'` runs exactly when the
  panels are animating themselves and stops the moment anything drives lighting.
- **The counter pauses rather than free-running.** Across each ~2 s lighting gap it moved
  by 0 or 1, where a free-running clock would have advanced ~20.

### Frame Timing: Quantised to the `'G'` Tick

`'G'` is emitted at a fixed **30.47 Hz (32.82 ms per tick)**, and a frame always lasts a
whole number of ticks. The firmware rounds each authored frame delay **up**:

```
ticks = ceil(delay_ms / 32.82)
```

Measured with two uploads, each ruling out the alternatives:

| Animation asks for | Ticks | Actual frame period | `ceil` | `round` | `floor` |
|---|---|---|---|---|---|
| 100 ms | 4 | 132.1 ms | **4** | 3 | 3 |
| 70 ms | 3 | 99.3 ms | **3** | 2 | 2 |

So a panel animation **never plays faster than authored, and usually plays slower**, by up
to a full tick. Effective playback rates are limited to **30.47 / k** fps: 30.5, 15.2,
10.2, 7.6, 6.1, 5.1 and so on. Anything authored between those lands on the slower one.

### Auto-Lighting Is Played By The Panels

Across 35.8 s of capture spanning three host-lighting runs, **every one of the 519 lighting
commands on the bus carried an all-zero payload** — all of them the host's own black
frames. No animation content ever crossed the bus, yet the pad visibly animates whenever
the host stops driving it.

The stored animation is therefore **played by the panels themselves**, not streamed by the
MCU. That matches the upload path (`SMX_LightsUpload_*`, bus command `'m'`): animation data
is written into panel storage once, and panels play it locally.

This explains several things at once. `'G'` needs no reply because it only has to keep the
panels' playback in step. `SMX_ReenableAutoLights` needs no bus command because it only has
to make the MCU *stop* sending lighting. And the firmware's own ~920 ms timeout achieves
the same thing by simply giving up on absent host lighting. The `autoLightsTimeout` config
field (7 x 128 ms = 896 ms) is consistent with the measured 915-928 ms.

### Mac-Idle Capture (No Software Running)

With no SDK software connected (pads powered, no USB HID open), the MCU stays in boot phase forever:

- **~30Hz `'G'` polls** continuously
- **`'w'` config writes every ~2 seconds** (MCU refreshes panel config autonomously)
- Never transitions to lighting mode

The `'w'` config write is sent even without host software, confirming it originates from MCU firmware, not the SDK.

## Signal Lines: Normal Operation (Press Detection)

During normal operation, all signal lines are held **HIGH (5V)**. When a panel detects a press, its signal line is pulled **LOW (GND)**.

### Observed Panel Presses

| Time (s) | Panel | Signal | Duration |
|----------|-------|--------|----------|
| 3.021 | Up | Goes LOW | ~0.88s |
| 4.632 | Left | Goes LOW | ~0.61s |
| 5.764 | Center | Goes LOW | ~0.54s |
| 6.682 | Right | Goes LOW | ~0.60s |
| 7.860 | Down | Goes LOW | ~0.64s |

## Signal Lines: Sensor Test Data Mode

Starting at **t ~= 19.8498s** in the service-menu capture, the signal lines switch from simple press detection to parallel data transmission. Triggered by a `'B?P'` command on the data bus.

### Test Data Frame Format

After `'B?P'`, the MCU sends 81 null bytes on the data line. During these pulses, the signal lines transmit data **synchronously with the UART clock**. Each rising edge of the data line clock = one bit.

**Frame: 80 bits (clocked at ~25 kHz ~= 3.2ms per frame)**

All panels transmit simultaneously in parallel on their individual signal wires.

### Decoded Test Data Frame (t = 19.8499s)

```
Bit   UL  U   L   C   R   Dn  DR    Notes
---   --  --  --  --  --  --  --    -----
 0    0   0   0   0   0   0   0     <- Signature bit 0 (all zeros)
 1    1   1   1   1   1   1   1     <- Signature bit 1 (all ones)
 2    0   0   0   0   0   0   0     <- Signature bit 2 (all zeros)
 3    1   0   0   0   0   0   1     <- Bad sensor [0]
 4    1   0   0   0   0   0   1     <- Bad sensor [1]
 5    1   0   0   0   0   0   1     <- Bad sensor [2]
 6    1   0   0   0   0   0   1     <- Bad sensor [3]
 7    0   0   0   0   0   0   0     <- Dummy/unused
 8    0   1   1   1   1   1   0     <- Sensor 0, bit 0
 9    0   0   1   1   0   0   0     <- Sensor 0, bit 1
10    0   0   0   0   0   0   0     <- Sensor 0, bit 2
11    0   0   0   0   1   0   0     <- Sensor 0, bit 3
12    0   0   0   0   1   0   0     <- Sensor 0, bit 4
13    0   0   0   0   1   0   0     <- Sensor 0, bit 5
14    0   0   0   0   1   0   0     <- Sensor 0, bit 6
15    0   0   0   0   1   0   0     <- Sensor 0, bit 7
16    0   0   0   0   1   0   0     <- Sensor 0, bit 8
17    0   0   0   0   1   0   0     <- Sensor 0, bit 9
18    0   0   0   0   1   0   0     <- Sensor 0, bit 10
19    0   0   0   0   1   0   0     <- Sensor 0, bit 11
20    0   0   0   0   1   0   0     <- Sensor 0, bit 12
21    0   0   0   0   1   0   0     <- Sensor 0, bit 13
22    0   0   0   0   1   0   0     <- Sensor 0, bit 14
23    0   0   0   0   1   0   0     <- Sensor 0, bit 15 (sign)
24    0   0   1   0   0   0   0     <- Sensor 1, bit 0
25    0   0   0   1   1   0   0     <- Sensor 1, bit 1
26    0   0   0   1   0   0   0     <- Sensor 1, bit 2
27-39 0   0   0   1   0   0   0     <- Sensor 1, bits 3-15
40    0   1   0   0   0   0   0     <- Sensor 2, bit 0
41    0   0   0   1   1   1   0     <- Sensor 2, bit 1
42-55 0   0   0   0   1   0   0     <- Sensor 2, bits 2-15
56    0   0   0   1   1   1   0     <- Sensor 3, bit 0
57    0   1   0   1   1   1   0     <- Sensor 3, bit 1
58-71 0   1   0   1   0   0   0     <- Sensor 3, bits 2-15
72    0   1   1   0   1   1   0     <- DIP switch bits
73    0   0   1   0   0   1   0     <- DIP / bad jumper
74    0   0   0   1   1   1   0     <- Bad jumper bits
75    0   0   0   0   0   0   1     <- Bad jumper bits
76-79 0   0   0   0   0   0   0     <- Padding/end
```

Signature bits 0-2 = `0, 1, 0` -- matches the SDK's `detail_data` struct (`sig1=0, sig2=1, sig3=0`). Frame alignment confirmed.

## Protocol Summary

```
+---------------------------------------------------------------------+
|                    MCU (Master Controller)                            |
|                                                                      |
|  Data Bus Out --> P0(UL) --> P3(L) --> P6(DL) --> P7(D) -->         |
|  (UART 250kbaud)    --> P4(C) --> P1(U) --> P2(UR) --> P5(R) -->    |
|                                                     --> P8(DR)       |
|  (daisy-chained in physical cable order; whether panels consume/     |
|   modify bytes in transit or treat the bus as broadcast is unknown)  |
|                                                                      |
|  Signal Wire In <-- Panel 0 (Up Left)     [dedicated wire]           |
|  Signal Wire In <-- Panel 1 (Up)          [dedicated wire]           |
|  Signal Wire In <-- Panel 2 (Up Right)    [dedicated wire]           |
|  Signal Wire In <-- Panel 3 (Left)        [dedicated wire]           |
|  Signal Wire In <-- Panel 4 (Center)      [dedicated wire]           |
|  Signal Wire In <-- Panel 5 (Right)       [dedicated wire]           |
|  Signal Wire In <-- Panel 6 (Down Left)   [dedicated wire]           |
|  Signal Wire In <-- Panel 7 (Down)        [dedicated wire]           |
|  Signal Wire In <-- Panel 8 (Down Right)  [dedicated wire]           |
+---------------------------------------------------------------------+

Normal mode:  Signal wires = 5V (idle) or GND (pressed)
              Data bus = CMD bytes BREAK CMD bytes BREAK ...
              (0x00 idle between command cycles)

Sensor test:  MCU sends 'B?P'[BREAK] then 81 x 0x00 (clock)
              Panels respond with 80-bit frames on signal wires
              All 9 panels transmit simultaneously (parallel)
```

## Key Findings

1. **The data bus IS standard UART** at 250000 baud, 8N1. The "clock-like" idle pattern is UART continuously transmitting 0x00 bytes (RGB data for all-black panel LEDs, or sensor clock pulses).

2. **BREAK is a command TERMINATOR, not a preamble.** Every command ends with a `0x00` byte flagged as a UART framing error. Commands are transmitted back-to-back with no inter-command idle -- the BREAK is the only delimiter. Earlier captures that appeared to show BREAK *before* commands were misread: the BREAK belongs to the end of the *previous* command.

3. **`'4'`/`'2'`/`'3'` are panel LED updates** at 30 Hz. `'4'` = inner 3x3 grid, `'2'` = outer top, `'3'` = outer bottom. RGB values are pre-scaled by 0.6666 (255 -> 170 on wire).

4. **`'B?P'` sensor test confirmed across all four modes.** Middle byte = USB mode char verbatim (`'0'`/`'1'`/`'2'`/`'3'`). `'B'` = broadcast, `'P'` = poll. After each `'B?P'[BREAK]`, the MCU sends 81 null bytes as clock pulses.

5. **Sensor test data is truly parallel.** All panels transmit simultaneously on individual signal wires, synchronized to the 0x00 clock on the data bus. The USB protocol sends data bit-interleaved because the hardware reads it that way.

6. **The frame is exactly 80 bits** -- 3 signature + 5 flags + 64 sensor data + 8 DIP/jumper = 80 bits, matching the SDK's `detail_data` struct.

7. **Force recalibration uses `'C'`** (0x43), not `'U'` (0x55). The USB command is `"C\n"` from `SMXDevice::ForceRecalibration()`. The MCU follows with a `'w'` config write.

8. **Platform strip lights are NOT on the panel bus.** `SMX_SetPlatformLights()` triggers USB command `'L'`, but the MCU drives the platform strips directly. No novel commands appear on the data bus during platform strip updates.

9. **Panel test mode causes the MCU to return to `'G'` broadcasts.** When `'T1'` is sent, the MCU stops the lighting loop and emits `'G'` at 30Hz until `'T0'` is received, then continues briefly before resuming lighting.

10. **`'G'` is an animation frame-sync broadcast, not a readiness poll.** It carries 9 records of 7 bytes, one per panel, with unpopulated corners flagged `FF FF`. The per-panel counter is the **animation frame index**: uploading an 8-frame animation changed its cycle from 0..23 to 0..7. Nothing ever replies to `'G'`, and it runs only while the panels are animating themselves.

11. **The MCU writes config autonomously every ~2 seconds** regardless of host software. Even with no SDK connected, the MCU continuously sends `'G'` broadcasts and periodic `'w'` config writes.

11b. **Frame timing is quantised to the `'G'` tick, rounding up.** `'G'` runs at a fixed 30.47Hz (32.82ms), and a frame lasts `ceil(delay_ms / 32.82)` ticks. A 100ms authored delay plays at 132.1ms; 70ms plays at 99.3ms. Animations never play faster than authored, only slower, and effective rates are limited to 30.47/k fps.

11c. **Auto-lighting is played by the panels, not streamed by the MCU.** Across 35.8s of capture every one of 519 bus lighting commands carried an all-zero payload (the host's own black frames); no animation content ever crosses the bus, yet the pad animates. Animation data is written into panel storage by the upload path and played locally.

12. **Config fully decoded** (hex-mode CSV export required; ASCII mode corrupts non-printable bytes by substituting `'.'`). `autoLightsTimeout = 7 x 128ms = 896ms`. Center panel has higher load-cell thresholds (100/120) vs. other panels (70/80).

13. **`'U'` (0x55): purpose unknown.** Previously recorded here as an autonomous 1Hz pad firmware heartbeat; that characterisation is withdrawn as unsupported. Format: `0x55 0x00[BREAK]` (2B).

13b. **`'R'` (0x52) "panel reset" is an unverified label.** It comes from `'R'` appearing once at the start of boot, not from any observed reset. It is not boot-only, and across five occurrences the animation frame counter kept incrementing with no restart, `'G'` cadence held at 32.7-32.9ms, and the panel signal lines showed no transitions. Either the reset is a no-op once the pad is up, or the label is wrong.

14. **`'w'` byte[2] varies by software source.** The third byte of the config write is `0x52` from the Mac SDK and `0x12` / `0x1E` / `0x6E` from the official SMX game. It is not a checksum or counter; purpose unknown.

15. **The data bus is physically daisy-chained through the panels in cable order.** Confirmed from physical inspection. Using panel numbering UL=0, U=1, UR=2, L=3, C=4, R=5, DL=6, D=7, DR=8, the chain runs: MCU → 0(UL) → 3(L) → 6(DL) → 7(D) → 4(C) → 1(U) → 2(UR) → 5(R) → 8(DR). This is a serpentine route: left column top-to-bottom, then center column bottom-to-top, then right column top-to-bottom -- consistent with minimizing cable runs across the pad. Whether panels consume/modify bytes in transit or treat the bus as a pure broadcast remains unconfirmed (see Open Questions).

## Open Questions

- **`'G'` poll exact field semantics.** The SEQ counter (5 synchronized bytes cycling 0-5), separator constants (`0x6B`, `0xCE`), and VAR bytes in separators are structurally documented but not semantically decoded. Firmware source would clarify. The 5 panel-entry sub-records are strongly correlated with `autoLightPanelMask`'s 5 enabled panels.

- **`'l'` command payload.** Observed as `0x6C 0x00[BREAK]` (2 bytes: command + 1 null byte). Is the trailing 0x00 a required operand or coincidental? The original SDK sends `"l" + zeros + "\n"` over USB before panel test mode; what arrives on the bus is just `0x6C 0x00[BREAK]`.

- **`'w'` third byte.** Varies by software source (`0x52` Mac SDK, `0x12`/`0x1E`/`0x6E` official SMX game). Not a checksum or simple counter. Purpose unknown.

- **Broadcast vs. consume-and-forward on the daisy chain.** The daisy-chain topology is now confirmed (see Finding 15). The remaining question is whether each panel passes the data line signal through unmodified (pure broadcast, all panels see identical bytes) or consumes/modifies bytes as the command propagates. To resolve this, tap the data line at multiple points in the chain simultaneously and compare the byte streams. If they are identical, it is a broadcast bus; if bytes are stripped or modified at each panel, it is a consume-and-forward protocol. The `'G'` poll's per-panel sub-records are consistent with either model.
