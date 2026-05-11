# Internal Bus Protocol Analysis

Analysis of the MCU-to-panel communication protocol based on Saleae logic capture data.

## Capture Setup

- **Channels captured:** Data line (MCU→panels), Up Left, Up, Left, Center, Right, Down, Down Right
- **Missing channels:** Up Right, Down Left (not enough probes)
- **Capture duration:** ~39.5 seconds
- **Activity:** Idle → panel presses (Up, Left, Center, Right, Down) → sensor test data → center panel press at end
- **Data bus:** UART 250000 baud, 8N1 (confirmed)

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

Commands are sent by breaking out of the 0x00 stream with a specific framing sequence:

### Command Framing

```
... 0x00 0x00 │ BREAK │ IDLE GAP │ CMD BYTE(S) │ 0x00 0x00 ...
              │~184µs │ variable │             │
              │ LOW   │  HIGH    │             │
```

1. **BREAK condition** — line held LOW for ~184µs (46 bits). This is longer than any valid UART frame and signals "attention" to all panels.
2. **IDLE gap** — line goes HIGH (UART idle) for a variable duration (650µs to 5ms).
3. **Command byte(s)** — one or more UART bytes at 250kbaud.
4. **Resume 0x00 stream** — panels process the command during the subsequent 0x00 bytes.

### Normal Operation Commands (Panel LED Lighting)

During normal operation, the MCU cycles through three commands at ~30 Hz to update panel LEDs:

| Command | Hex | Purpose | Data |
|---------|-----|---------|------|
| `'2'` | 0x32 | Top half of panel LEDs (rows 0-1 of 4×4 grid) | 9 panels × 8 LEDs × 3 bytes RGB |
| `'3'` | 0x33 | Bottom half of panel LEDs (rows 2-3 of 4×4 grid) | 9 panels × 8 LEDs × 3 bytes RGB |
| `'4'` | 0x34 | Inner 3×3 LED grid (firmware v4+ / 25-LED panels) | 9 panels × 9 LEDs × 3 bytes RGB |

The cycle repeats: `'4'` → `'2'` → `'3'` → `'4'` → ...

This matches the original SDK's `SMXManager::SetLights()` which splits panel LED data into three commands sent at 30 FPS. The panel has a 4×4 outer grid (split into top/bottom halves) plus a 3×3 inner grid for 25-LED panels. The MCU receives the lighting command over USB and forwards it to the panels over this UART bus.

In this capture, the RGB data is all zeros (0x00) because the pad is in the service menu with no active lighting — the "idle clock" appearance is literally the MCU sending lighting commands with all-black pixel data.

### Sensor Test Command

To request sensor test data, the MCU sends a multi-byte command:

```
BREAK → IDLE (5.07ms) → 0x42 0x31 0x50 ('B' '1' 'P')
```

| Byte | Hex | ASCII | Possible meaning |
|------|-----|-------|-----------------|
| 0x42 | 66 | 'B' | Broadcast? Begin? |
| 0x31 | 49 | '1' | Test mode 1 (uncalibrated)? |
| 0x50 | 80 | 'P' | Poll? Request? |

After this command, the data line resumes the 0x00 stream and all panels respond simultaneously with 80-bit data frames on their signal wires (see below).

### Summary Table

| Pattern | Meaning |
|---------|---------|
| 36µs LOW + 4µs HIGH repeating | UART 0x00 stream (idle / black LED data) |
| ~184µs LOW | BREAK condition (command start) |
| Extended HIGH (variable) | UART idle (inter-command gap) |
| Non-zero byte after BREAK+IDLE | Command byte |

### Complete Internal Bus Command Reference

Decoded from logic captures (service menu + sensor test):

| Command | Hex | Purpose | Data Format |
|---------|-----|---------|-------------|
| `'2'` + data | 0x32 + RGB | Top half panel LEDs (rows 0-1 of 4×4) | 9 panels × 8 LEDs × 3 bytes RGB + `'\n'` |
| `'3'` + data | 0x33 + RGB | Bottom half panel LEDs (rows 2-3 of 4×4) | 9 panels × 8 LEDs × 3 bytes RGB + `'\n'` |
| `'4'` + data | 0x34 + RGB | Inner 3×3 LED grid (fw v4+) | 9 panels × 9 LEDs × 3 bytes RGB + `'\n'` |
| `'l'` | 0x6C | Lights off (clear all panel LEDs) | No data (or followed by zeros) |
| `'T0'` | 0x54 0x30 | Panel test mode OFF | |
| `'T1'` | 0x54 0x31 | Panel test mode ON (pressure test) | Resent every ~1s |
| `'w'` + size + data | 0x77 + ... | Config write (forwarded from USB `'W'` cmd) | See format below |
| `'B1P'` | 0x42 0x31 0x50 | Sensor test request (uncalibrated?) | Triggers 80-bit parallel response |
| `'U'` | 0x55 | Unknown (not in original SDK) | Possibly calibration/recalibration |
| `'R'` | 0x52 | Panel reset (boot only) | Sent once at start of boot sequence |
| `'G'` + 0xFF | 0x47 0xFF | Panel readiness poll (boot only) | 30Hz until all panels respond; extra byte = response bitmask |

### Config Write (`'w'`) Format

The internal bus config write has an extra byte compared to the USB protocol:

```
USB format:   'W' + size(1) + SMXConfig(250 bytes)
Bus format:   'w' + 0xFA   + UNKNOWN_BYTE + SMXConfig(249+ bytes)
```

The `UNKNOWN_BYTE` (observed values: 0x12, 0x6e, 0x1e, 0x3f) does not appear in the USB protocol. Its purpose is unclear — possibly a sequence number, checksum, or panel routing byte. It changes between different config writes in the same session.

When parsed with the extra byte skipped, the config decodes correctly:

```
Example decoded config (from menuing capture at boot):
  masterVersion:       5 (firmware v5)
  configVersion:       5 (current)
  flags:               0x03 (AutoLightingUsePressedAnimations + FSR)
  panelDebounceMicroseconds: 4000
  autoCalibrationMaxDeviation: 100
  badSensorMinimumDelaySeconds: 15
  autoLightsTimeout:   7 (≈ 896ms)
  stepColor:           all (170,170,170) = white
  platformStripColor:  (0, 0, 128) = blue

  Panel thresholds (LC_Lo/LC_Hi, FSR_Lo[0]):
    Corners (0,2,6,8): 255/255, 217  (disabled)
    Arrows  (1,3,5,7):  60/80,  217  (sensitive)
    Center  (4):         70/150, 217  (less sensitive)
```

Multiple config variants were sent at boot with different sensitivity levels (thresholds ranging from 25-80 for arrows, 40-150 for center), suggesting the game cycles through presets during initialization.

**Notes on `'U'` command:** Seen 19 times at the start of the capture (t=0.55s), not present in the original SDK source. May be a firmware-internal command for auto-calibration, panel initialization, or something the game's service menu triggers directly. Also seen once as `'U4'` (0x55 0x34).

**Notes on `'T'` command:** The USB-level command is `"t 1\n"` (with a space), but the internal bus command is `'T1'` (no space, uppercase). The MCU translates between the two formats.

**Notes on `'w'` command:** The USB-level config write is `'W'` + size + data, but the internal bus uses lowercase `'w'`. The second byte 0xFA = 250 = `sizeof(SMXConfig)`, confirming this is the MCU forwarding config data to the panels.

### Boot Sequence Commands

From an unsuccessful bootup capture (game stalled on splash screen), additional boot-only commands were observed:

| Command | Hex | Purpose | Notes |
|---------|-----|---------|-------|
| `'R'` | 0x52 | Panel reset | Sent once at very start of boot |
| `'G'` + 0xFF | 0x47 0xFF | Panel readiness poll | Runs at 30Hz until all panels respond |
| `'G'` + 0xFF + bitmask | 0x47 0xFF 0xNN | Panel status with response bitmask | Indicates which panels have responded |

**Boot sequence:**
```
t=0       Power on
t=2.4s    'R'              ← Reset all panels
t=2.4s    'w' + config     ← Write configuration to panels
t=2.4s    'G' 0xFF         ← Begin polling for panel readiness (30Hz)
          ...              ← Repeat 'G' until all panels respond
t=???     '4','2','3'      ← Switch to lighting loop (normal operation)
```

In the unsuccessful boot, the `'G'` poll ran for the entire 102-second capture without ever transitioning to the lighting loop. The variant `'G' 0xFF 0xF7` was observed 23 times — 0xF7 = `11110111` (bit 3 cleared), suggesting the Left panel (panel 3) was not responding. The logic probe attached to that panel's signal wire likely interfered with its response.

**Comparison: successful vs unsuccessful boot:**

| Phase | Successful (menuing capture) | Unsuccessful |
|-------|------------------------------|--------------|
| Reset | Not captured (already booted) | `'R'` at t=2.4s |
| Config | `'w'` commands at start | `'w'` commands at start |
| Panel poll | Presumably passed quickly | `'G'` 0xFF stuck for 102s |
| Normal operation | `'4'`/`'2'`/`'3'` lighting at 30Hz | Never reached |
| Other | `'U'` commands, test modes | None |

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

The signal transitions are clean and immediate — no debouncing visible at this timescale. The MCU can detect a press within one clock cycle (40µs = 0.04ms).

## Signal Lines: Sensor Test Data Mode

Starting at **t ≈ 19.8498s**, the signal lines switch from simple press detection to a parallel data transmission mode. This is triggered by a command on the data bus (visible as a burst at t ≈ 19.8444s with different timing characteristics).

### Trigger Command

At t = 19.8444s, the data line shows a burst with non-standard timing:
```
19.844427: LOW for 8µs
19.844435: HIGH for 4µs
19.844439: LOW for 16µs
19.844455: HIGH for 4µs
19.844459: LOW for 4µs
... (more rapid transitions)
19.844547: LOW for 181µs (extended)
19.844728: HIGH (idle for 5.07ms until test data begins)
```

This appears to be the MCU sending a "request sensor test data" command over the data bus.

### Test Data Frame Format

After the command, the data line resumes its 25kHz clock, and the signal lines transmit data **synchronously with the clock**. Each rising edge of the data line clock is one bit period.

**Frame: 80 bits (clocked at 25 kHz = 3.2ms per frame)**

The data is transmitted in parallel across all signal lines simultaneously. Each clock cycle, all panels output one bit on their respective signal lines. The MCU reads all lines in parallel.

### Decoded Test Data Frame (t = 19.8499s)

Reading signal line values at each data line clock edge (0 = LOW, 1 = HIGH):

```
Bit   UL  U   L   C   R   Dn  DR    Notes
───   ──  ──  ──  ──  ──  ──  ──    ─────
 0    0   0   0   0   0   0   0     ← Signature bit 0 (all zeros)
 1    1   1   1   1   1   1   1     ← Signature bit 1 (all ones)
 2    0   0   0   0   0   0   0     ← Signature bit 2 (all zeros)
 3    1   0   0   0   0   0   1     ← Bad sensor [0]
 4    1   0   0   0   0   0   1     ← Bad sensor [1]
 5    1   0   0   0   0   0   1     ← Bad sensor [2]
 6    1   0   0   0   0   0   1     ← Bad sensor [3]
 7    0   0   0   0   0   0   0     ← Dummy/unused
 8    0   1   1   1   1   1   0     ← Sensor 0, bit 0
 9    0   0   1   1   0   0   0     ← Sensor 0, bit 1
10    0   0   0   0   0   0   0     ← Sensor 0, bit 2
11    0   0   0   0   1   0   0     ← Sensor 0, bit 3
12    0   0   0   0   1   0   0     ← Sensor 0, bit 4
13    0   0   0   0   1   0   0     ← Sensor 0, bit 5
14    0   0   0   0   1   0   0     ← Sensor 0, bit 6
15    0   0   0   0   1   0   0     ← Sensor 0, bit 7
16    0   0   0   0   1   0   0     ← Sensor 0, bit 8
17    0   0   0   0   1   0   0     ← Sensor 0, bit 9
18    0   0   0   0   1   0   0     ← Sensor 0, bit 10
19    0   0   0   0   1   0   0     ← Sensor 0, bit 11
20    0   0   0   0   1   0   0     ← Sensor 0, bit 12
21    0   0   0   0   1   0   0     ← Sensor 0, bit 13
22    0   0   0   0   1   0   0     ← Sensor 0, bit 14
23    0   0   0   0   1   0   0     ← Sensor 0, bit 15 (sign)
24    0   0   1   0   0   0   0     ← Sensor 1, bit 0
25    0   0   0   1   1   0   0     ← Sensor 1, bit 1
26    0   0   0   1   0   0   0     ← Sensor 1, bit 2
27-39 0   0   0   1   0   0   0     ← Sensor 1, bits 3-15
40    0   1   0   0   0   0   0     ← Sensor 2, bit 0
41    0   0   0   1   1   1   0     ← Sensor 2, bit 1
42-55 0   0   0   0   1   0   0     ← Sensor 2, bits 2-15
56    0   0   0   1   1   1   0     ← Sensor 3, bit 0
57    0   1   0   1   1   1   0     ← Sensor 3, bit 1
58-71 0   1   0   1   0   0   0     ← Sensor 3, bits 2-15
72    0   1   1   0   1   1   0     ← DIP switch bits
73    0   0   1   0   0   1   0     ← DIP / bad jumper
74    0   0   0   1   1   1   0     ← Bad jumper bits
75    0   0   0   0   0   0   1     ← Bad jumper bits
76-79 0   0   0   0   0   0   0     ← Padding/end
```

### Validation: Signature Bits Match SDK

The SDK's `detail_data` struct expects signature bits `sig1=0, sig2=1, sig3=0` — and that's exactly what we see in bits 0-2: all panels output 0, then 1, then 0. This confirms the frame alignment is correct.

### Sensor Value Interpretation

Looking at the Right panel (column R) as an example:
- Bad sensor flags: bits 3-6 = `0000` (no bad sensors)
- Sensor 0: bits 8-23 = `1111111111111100` = reading as int16_t with bit 23 as sign → large negative value or large positive depending on endianness

The Center panel shows sensor 1 bits 26-39 all as `1`, suggesting a sustained reading (possibly because the center panel was being pressed during the test, or it has a high baseline).

## Protocol Summary

```
┌─────────────────────────────────────────────────────────────────────┐
│                    MCU (Master Controller)                            │
│                                                                      │
│  Data Bus Out ──────────────────────────────────────────────────────→│
│  (25kHz clock + commands)                                            │
│                                                                      │
│  Signal Wire In ←── Panel 0 (Up Left)     [dedicated wire]           │
│  Signal Wire In ←── Panel 1 (Up)          [dedicated wire]           │
│  Signal Wire In ←── Panel 2 (Up Right)    [dedicated wire]           │
│  Signal Wire In ←── Panel 3 (Left)        [dedicated wire]           │
│  Signal Wire In ←── Panel 4 (Center)      [dedicated wire]           │
│  Signal Wire In ←── Panel 5 (Right)       [dedicated wire]           │
│  Signal Wire In ←── Panel 6 (Down Left)   [dedicated wire]           │
│  Signal Wire In ←── Panel 7 (Down)        [dedicated wire]           │
│  Signal Wire In ←── Panel 8 (Down Right)  [dedicated wire]           │
└─────────────────────────────────────────────────────────────────────┘

Normal mode:  Signal wires = 5V (idle) or GND (pressed)
              Data bus = 25kHz clock (idle)

Test mode:    MCU sends command on data bus
              Panels respond with 80-bit frames on signal wires
              Clocked by the data bus 25kHz signal
              All 9 panels transmit simultaneously (parallel)
```

## Key Findings

1. **The data bus IS standard UART** at 250000 baud, 8N1. The "clock-like" idle pattern is just UART continuously transmitting 0x00 bytes (which happen to be the RGB data for all-black panel LEDs).

2. **Command framing uses UART BREAK** — an extended LOW (~184µs = 46 bits) signals the start of a new command. This is a standard UART break condition (LOW held longer than one frame).

3. **The `'2'`, `'3'`, `'4'` commands are panel LED lighting updates** — confirmed from the original SDK's `SMXManager::SetLights()`. `'2'` = top half of 4×4 LED grid, `'3'` = bottom half, `'4'` = inner 3×3 grid (fw v4+). They cycle at 30 Hz, matching the SDK's 30 FPS lighting rate. The 0x00 bytes following each command are the RGB pixel data (all black in this capture since the pad is in the service menu).

4. **Sensor test data is truly parallel.** All panels transmit simultaneously on their individual signal wires, synchronized to the 0x00 byte stream on the data bus. This is why the USB protocol sends the data bit-interleaved — it's a direct representation of how the hardware reads it.

5. **The frame is exactly 80 bits** matching the SDK's `detail_data` struct (3 signature + 5 flags + 64 sensor data + 8 DIP/jumper = 80 bits).

6. **Press detection coexists with test data.** The signal wires can carry both — the MCU likely samples the lines at specific clock edges for test data while also detecting sustained LOW for press events.

7. **The sensor test command is `'B' '1' 'P'`** (0x42 0x31 0x50) — a multi-byte internal command that the MCU translates from the USB sensor test request (`"y0\n"`) into the panel bus format.

## Open Questions

- **What is the `'B1P'` command format?** Does `'1'` correspond to the sensor test mode number? Captures with different test modes (calibrated `'0'`, noise `'2'`, tare `'3'`) would confirm whether the middle byte changes. `'B'` might be "broadcast" and `'P'` might be "poll."
- **What is the `'U'` command?** Not present in the original SDK. Seen 19 times at startup in the menuing capture. Possibly auto-calibration, panel reset, or a game-specific firmware command. `'U4'` variant seen once.
- **How does the daisy chain affect command delivery?** Do panels consume/modify bytes, or is it a pure broadcast where all panels see all commands? The config write (`'w'`) sends the full 250-byte config — do all panels receive and apply it, or is there addressing?
- **What determines the inter-command timing?** The gap between `'4'` and the next `'2'` is much longer (~3.5-4ms) than between `'2'`→`'3'` or `'3'`→`'4'` (~0.8ms). The original SDK notes that firmware v4+ waits for panel TX to flush before processing the next command.
- **What are the extended HIGH gaps (~88-117µs) within the lighting data?** They appear 4 times after each lighting command within the 0x00 data stream. Possibly panel acknowledgment windows or daisy-chain propagation delays.
