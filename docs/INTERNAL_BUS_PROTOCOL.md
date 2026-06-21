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
| `'G'` poll | 0x47 | 65B structured format + `0x00[BREAK]` | `SMX_ReenableAutoLights` / boot | Panel readiness poll; see boot section |
| `'R'` | 0x52 | `0x52 ...` | — | Panel reset (boot only; not in SDK) |
| `'U'` | 0x55 | `0x55 0x00[BREAK]` (2B) | — (pad firmware only) | 1Hz heartbeat from SMX pad firmware; not SDK-triggered |

**`'B?P'` sensor test format confirmed:** The middle byte exactly mirrors the USB sensor test mode character — `'0'` uncalibrated, `'1'` calibrated, `'2'` noise, `'3'` tare. Captured and verified across all four test modes.

**`'C'` recalibration command:** The command is `'C'` (0x43). The USB-level command is `"C\n"` (from `SMXDevice::ForceRecalibration()`). After `'C'`, the MCU automatically sends a `'w'` config write.

**`'U'` command (0x55) resolved:** A **1-second firmware heartbeat** emitted by the SMX pad firmware itself, not triggered by any SDK API call. It appears at regular 1-second intervals during normal pad operation when the pad's own firmware is in charge (observed immediately after USB is swapped from the Mac SDK to the pad). Format: `0x55 0x00[BREAK]` (2B). It was absent from bus-probe captures because our SDK does not send or respond to it — the pad firmware sends it autonomously on the internal bus regardless of what is connected over USB.

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

**Important:** When panel test mode is enabled via `SMX_SetPanelTestMode`, the MCU stops accepting lighting commands and **re-enters its panel readiness polling loop** (`'G'` commands at 30Hz). This continues until `SMX_SetPanelTestMode(Off)` is called, after which the MCU polls for a short time then resumes normal lighting.

### Re-enable Auto Lights (`SMX_ReenableAutoLights`)

USB command: `"S 1\n"`. Effect on bus: MCU re-enters the `'G'` panel readiness polling loop at 30Hz, verifying all panels before switching back to the MCU's stored GIF auto-lighting mode. Roughly 10 `'G'` polls are sent before lighting resumes.

## Boot Sequence

### Boot Commands

| Command | Hex | Purpose |
|---------|-----|---------|
| `'R'` | 0x52 | Panel reset — sent once at start of boot |
| `'w'` + config | 0x77 | Config write — sent immediately after reset |
| `'G'` poll | 0x47 | Panel readiness poll — sent at 30Hz until panels respond |

**Boot sequence:**
```
t=0       Power on
t=2.4s    'R'              <- Reset all panels
t=2.4s    'w' + config     <- Write configuration to panels
t=2.4s    'G' poll         <- Begin polling for panel readiness (30Hz)
          ...              <- Repeat 'G' until all panels respond
t=???     '4','2','3'      <- Switch to lighting loop (normal operation)
```

### `'G'` Poll Format (Boot / Re-init)

The `'G'` command on the internal bus is **65 bytes**, not just `47 FF` as the USB perspective implies. The MCU sends a structured packet that includes per-panel readiness sub-records:

```
Bytes [0-3]:   47 00 00 00              = 'G' command + 3 prefix bytes
Bytes [4-10]:  FF FF CE 00 00 00 00     = header field (CE = pad-type/status byte)
Bytes [11-17]: SEQ FF 00 01 00 00 00    = panel entry 1
Bytes [18-24]: FF FF 02 6B 00 00 00     = separator (constant: 0x6B may be firmware version)
Bytes [25-31]: SEQ FF 00 01 00 00 00    = panel entry 2
Bytes [32-38]: SEQ FF 00 01 00 00 00    = panel entry 3
Bytes [39-45]: SEQ FF 00 01 00 00 00    = panel entry 4
Bytes [46-52]: FF FF VAR VAR 00 00 00   = separator (varying: status/sensor data?)
Bytes [53-59]: SEQ FF 00 01 00 00 00    = panel entry 5
Bytes [60-63]: FF FF VAR VAR            = trailer (varying)
Byte  [64]:    00 [BREAK]               = command terminator
```

- **SEQ** (5 positions: bytes 11, 25, 32, 39, 53): A counter shared across all 5 panel entries. Cycles as 0x00 x ~27 polls, then 0x01 x 4, 0x02 x 4, 0x03 x 4, 0x04 x 4, 0x05 x 4, repeating. The 0x00 hold duration (~27 polls x 33ms ~= 891ms) matches `autoLightsTimeout = 7 x 128ms = 896ms`.
- **5 panel entries**: Matches `autoLightPanelMask = 0x00BA` (5 active panels). The MCU only polls enabled panels.
- **byte[6] = 0xCE in boot-idle** but **0x0A in panel-test context**. This byte appears to reflect MCU operating mode.

After calling `SMX_ReenableAutoLights()`, the `'G'` packet header changes (byte[6] varies), confirming different MCU states produce structurally similar but not identical `'G'` packets.

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

9. **Panel test mode causes the MCU to re-enter `'G'` polling.** When `'T1'` is sent, the MCU stops the lighting loop and begins `'G'` readiness polls at 30Hz until `'T0'` is received, then polls briefly before resuming lighting.

10. **`'G'` polls have 5 per-panel entries**, matching `autoLightPanelMask` (5 active panels on this pad). The MCU only polls active panels.

11. **The MCU writes config autonomously every ~2 seconds** regardless of host software. Even with no SDK connected, the MCU continuously sends `'G'` polls and periodic `'w'` config writes.

12. **Config fully decoded** (hex-mode CSV export required; ASCII mode corrupts non-printable bytes by substituting `'.'`). `autoLightsTimeout = 7 x 128ms = 896ms`. Center panel has higher load-cell thresholds (100/120) vs. other panels (70/80).

13. **`'U'` (0x55) is a 1Hz pad firmware heartbeat**, not an SDK command. It appears at exactly 1-second intervals on the bus during normal operation when the pad's own firmware is in control (visible immediately after USB is swapped from the Mac SDK connection to the pad itself). Our SDK does not send it and the official stepmaniax-sdk-mp does not handle it. Format: `0x55 0x00[BREAK]` (2B).

14. **`'w'` byte[2] varies by software source.** The third byte of the config write is `0x52` from the Mac SDK and `0x12` / `0x1E` / `0x6E` from the official SMX game. It is not a checksum or counter; purpose unknown.

15. **The data bus is physically daisy-chained through the panels in cable order.** Confirmed from physical inspection. Using panel numbering UL=0, U=1, UR=2, L=3, C=4, R=5, DL=6, D=7, DR=8, the chain runs: MCU → 0(UL) → 3(L) → 6(DL) → 7(D) → 4(C) → 1(U) → 2(UR) → 5(R) → 8(DR). This is a serpentine route: left column top-to-bottom, then center column bottom-to-top, then right column top-to-bottom -- consistent with minimizing cable runs across the pad. Whether panels consume/modify bytes in transit or treat the bus as a pure broadcast remains unconfirmed (see Open Questions).

## Open Questions

- **`'G'` poll exact field semantics.** The SEQ counter (5 synchronized bytes cycling 0-5), separator constants (`0x6B`, `0xCE`), and VAR bytes in separators are structurally documented but not semantically decoded. Firmware source would clarify. The 5 panel-entry sub-records are strongly correlated with `autoLightPanelMask`'s 5 enabled panels.

- **`'l'` command payload.** Observed as `0x6C 0x00[BREAK]` (2 bytes: command + 1 null byte). Is the trailing 0x00 a required operand or coincidental? The original SDK sends `"l" + zeros + "\n"` over USB before panel test mode; what arrives on the bus is just `0x6C 0x00[BREAK]`.

- **`'w'` third byte.** Varies by software source (`0x52` Mac SDK, `0x12`/`0x1E`/`0x6E` official SMX game). Not a checksum or simple counter. Purpose unknown.

- **Broadcast vs. consume-and-forward on the daisy chain.** The daisy-chain topology is now confirmed (see Finding 15). The remaining question is whether each panel passes the data line signal through unmodified (pure broadcast, all panels see identical bytes) or consumes/modifies bytes as the command propagates. To resolve this, tap the data line at multiple points in the chain simultaneously and compare the byte streams. If they are identical, it is a broadcast bus; if bytes are stripped or modified at each panel, it is a consume-and-forward protocol. The `'G'` poll's per-panel sub-records are consistent with either model.
