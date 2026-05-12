# USB Communication Protocol

This document describes the USB HID communication protocol used by StepManiaX dance pads, as understood from the SDK implementation. The pad firmware is not open source, so this is reverse-engineered from observed behavior and SDK code.

## Device Identification

| Property | Value |
|----------|-------|
| USB Vendor ID | `0x2341` (Arduino) |
| USB Product ID | `0x8037` |
| Product String | `"StepManiaX"` |
| Transport | USB HID (Human Interface Device) |
| Max Packet Size | 64 bytes |

The pad uses an Arduino-based microcontroller (hence the Arduino VID). Communication uses standard USB HID reports — no custom drivers are needed on any platform.

## HID Report Types

The protocol uses three HID report IDs for different data flows:

| Report ID | Direction | Name | Purpose |
|-----------|-----------|------|---------|
| `0x03` | Device → Host | Input State | Panel press/release bitmask |
| `0x05` | Host → Device | Command | Commands sent to the pad |
| `0x06` | Device → Host | Data | Command responses, config, sensor data |

## Report 3: Input State (Device → Host)

The primary real-time data channel. Sent by the device whenever panel state changes or on a periodic heartbeat.

### Packet Format

```
Byte 0: Report ID (0x03)
Byte 1: Input state low byte
Byte 2: Input state high byte
```

Total: 3 bytes.

### Input State Bitmask

The 16-bit value (little-endian) encodes which panels are currently pressed:

```
Bit:    0   1   2   3   4   5   6   7   8   9-15
Panel: ┌───┬───┬───┐
       │ 0 │ 1 │ 2 │  (top row: up-left, up, up-right)
       ├───┼───┼───┤
       │ 3 │ 4 │ 5 │  (middle row: left, center, right)
       ├───┼───┼───┤
       │ 6 │ 7 │ 8 │  (bottom row: down-left, down, down-right)
       └───┴───┴───┘
       Bits 9-15: unused (always 0)
```

A bit value of `1` means the panel is pressed; `0` means released.

### Timing Behavior

Based on observation (firmware is closed-source):

| State | Report Rate | Notes |
|-------|-------------|-------|
| Idle | ~10 packets/sec | Periodic heartbeat, state unchanged |
| Active input | ~50 packets/sec | One report per state transition |

The device appears to send reports on state change plus a low-frequency heartbeat, rather than continuously streaming at the USB endpoint's maximum rate.

## Report 5: Commands (Host → Device)

All commands from the host to the device are sent as Report 5 packets. Commands larger than 61 bytes are fragmented across multiple packets.

### Packet Format

```
Byte 0: Report ID (0x05)
Byte 1: Flags
Byte 2: Payload size (0-61)
Bytes 3-63: Payload data (padded with zeros to 64 bytes total)
```

Total: always 64 bytes (zero-padded).

### Fragmentation Flags

| Flag | Value | Meaning |
|------|-------|---------|
| `START_OF_COMMAND` | `0x04` | First packet of a command |
| `END_OF_COMMAND` | `0x01` | Last packet of a command |
| `DEVICE_INFO` | `0x80` | Special: device info request |

For single-packet commands (≤ 61 bytes), both `START_OF_COMMAND` and `END_OF_COMMAND` are set: flags = `0x05`.

For multi-packet commands:
- First packet: flags = `0x04` (START only)
- Middle packets: flags = `0x00` (no flags)
- Last packet: flags = `0x01` (END only)

### Command Sequencing

Commands are sent one at a time. The host must wait for the device to respond (via Report 6 with `HOST_CMD_FINISHED`) before sending the next command. If no response arrives within 2 seconds, the command is retried.

## Report 6: Data Responses (Device → Host)

All command responses, configuration data, and sensor readings arrive as Report 6 packets. Like outgoing commands, responses may be fragmented across multiple packets.

### Packet Format

```
Byte 0: Report ID (0x06)
Byte 1: Flags
Byte 2: Payload size (0-61)
Bytes 3-N: Payload data (N = 3 + payload size)
```

Total: variable length (3 to 64 bytes).

### Response Flags

| Flag | Value | Meaning |
|------|-------|---------|
| `START_OF_COMMAND` | `0x04` | First packet of a response |
| `END_OF_COMMAND` | `0x01` | Last packet of a response (queue for reading) |
| `HOST_CMD_FINISHED` | `0x02` | Device finished processing the host's command |
| `DEVICE_INFO` | `0x80` | This is a device info response |

Flags can be combined. A typical single-packet response has flags = `0x07` (START + END + HOST_CMD_FINISHED).

### Reassembly

The host reassembles fragmented responses:
1. `START_OF_COMMAND` → clear any partial buffer
2. Append payload to buffer
3. `END_OF_COMMAND` → complete packet ready for processing
4. `HOST_CMD_FINISHED` → invoke command completion callback

## Device Info Protocol

Device info is requested immediately upon connection and uses a special flag-based mechanism separate from the normal command protocol.

### Request

```
Report 5 packet:
  [0] = 0x05 (Report ID)
  [1] = 0x80 (DEVICE_INFO flag)
  [2] = 0x00 (no payload)
  [3-63] = 0x00 (padding)
```

### Response

```
Report 6 packet with DEVICE_INFO flag (0x80):
  Payload structure (22 bytes):
    [0]     Command byte
    [1]     Packet size
    [2]     Player ('0' = P1, '1' = P2)
    [3]     Unused
    [4-19]  Serial number (16 raw bytes)
    [20-21] Firmware version (uint16_t, little-endian)
    [22]    Unused
```

The player byte corresponds to the physical jumper on the PCB. The serial number is stored as 16 raw bytes and displayed as a 32-character hex string.

## Command Protocol

All application-level commands follow the same pattern: the host sends a command string via Report 5, and the device responds via Report 6. The first byte of the command string identifies the command type.

### Command Summary

| Command | Byte | Direction | Description |
|---------|------|-----------|-------------|
| Get Config (new) | `'G'` (0x47) | Host → Device | Request config (firmware v5+) |
| Get Config (old) | `'g'` + `'\n'` | Host → Device | Request config (firmware < v5) |
| Set Config (new) | `'W'` + size + data | Host → Device | Write config (firmware v5+) |
| Set Config (old) | `'w'` + size + data | Host → Device | Write config (firmware < v5) |
| Factory Reset | `'f'` + `'\n'` | Host → Device | Reset to defaults |
| Force Recalibration | `'C'` + `'\n'` | Host → Device | Trigger recalibration |
| Set Serial | `'s'` + serial(16) + `'\n'` | Host → Device | Assign serial number |
| Re-enable Auto Lights | `'S'` + `' 1'` + `'\n'` | Host → Device | Resume auto-lighting |
| Panel Lights (inner) | `'4'` + RGB data + `'\n'` | Host → Device | Inner 3×3 grid LEDs (fw v4+) |
| Panel Lights (top) | `'2'` + RGB data + `'\n'` | Host → Device | Top 2 rows of 4×4 grid |
| Panel Lights (bottom) | `'3'` + RGB data + `'\n'` | Host → Device | Bottom 2 rows of 4×4 grid |
| Lights Off (legacy) | `'l'` + 108 zeros + `'\n'` | Host → Device | Clear all panel LEDs |
| Platform Lights | `'L'` + strip + count + RGB | Host → Device | Set LED strip colors |
| Panel Test Mode | `'t'` + `' '` + mode + `'\n'` | Host → Device | Diagnostic lighting |
| Sensor Test Request | `'y'` + mode + `'\n'` | Host → Device | Request sensor data |
| Sensor Test Response | `'y'` + mode + size + data | Device → Host | Sensor test results |
| Config Response (new) | `'G'` + size + data | Device → Host | Config data (fw v5+) |
| Config Response (old) | `'g'` + size + data | Device → Host | Config data (fw < v5) |

### Get Configuration

**Request (firmware v5+):**
```
"G"  (1 byte)
```

**Request (firmware < v5):**
```
"g\n"  (2 bytes)
```

**Response:**
```
Byte 0: 'G' or 'g' (echoes command)
Byte 1: Size of config data
Bytes 2+: Config data (SMXConfig struct, 250 bytes for v5)
```

### Set Configuration

**Request (firmware v5+):**
```
Byte 0: 'W'
Byte 1: Size (250 = sizeof(SMXConfig))
Bytes 2+: SMXConfig struct data (250 bytes)
```

**Request (firmware < v5):**
```
Byte 0: 'w'
Byte 1: Size
Bytes 2+: OldSMXConfig struct data (converted from SMXConfig)
```

Total command size for v5: 252 bytes → fragmented into 5 HID packets (61 + 61 + 61 + 61 + 8 bytes of payload).

After writing config, the SDK immediately sends a "G"/"g" command to read back and verify.

### Set Serial Number

```
Byte 0: 's'
Bytes 1-16: 16 random bytes (the new serial number)
Byte 17: '\n'
```

The serial is permanently stored in the device's non-volatile memory.

### Panel LED Control

Panel LEDs are updated via three separate commands sent in sequence. Each panel has 25 LEDs: a 4×4 outer grid (16 LEDs) plus a 3×3 inner grid (9 LEDs, firmware v4+ only).

**LED layout per panel (25-LED mode):**

```
Outer 4×4:          Inner 3×3:
00  01  02  03
   16  17  18       (between outer rows 0-1 and 2-3)
04  05  06  07
   19  20  21       (between outer rows 2-3 and 4-5)
08  09  10  11
   22  23  24       (between outer rows 4-5 and 6-7)
12  13  14  15
```

**Commands (sent in order for each update):**

| Command | Prefix | Payload | Size | Notes |
|---------|--------|---------|------|-------|
| Inner grid | `'4'` | 9 panels × 9 LEDs × 3 RGB | 244 bytes + `'\n'` | Firmware v4+ only |
| Top half | `'2'` | 9 panels × 8 LEDs × 3 RGB | 217 bytes + `'\n'` | Top 2 rows of 4×4 |
| Bottom half | `'3'` | 9 panels × 8 LEDs × 3 RGB | 217 bytes + `'\n'` | Bottom 2 rows of 4×4 |

**Color scaling:** All RGB values are multiplied by 0.6666 before sending. Values above ~170 don't make LEDs brighter; this improves contrast and reduces power draw.

**Rate limiting:** Updates are capped at 30 FPS. If updates arrive faster, the most recent data replaces pending data.

**Firmware version timing:**
- **Firmware < v4:** Commands `'2'` and `'3'` are sent with a 1/60s delay between them. The master controller needs time to relay data to panels. Command `'4'` is not sent.
- **Firmware ≥ v4:** All three commands are queued immediately. The firmware handles flow control internally, buffering commands until the previous one finishes sending to panels.

**Auto-lighting:** Panels return to automatic step lighting if no lights commands are received for a few seconds (controlled by `autoLightsTimeout` in config). Applications should send updates continuously, even if colors aren't changing.

**Panel test mode interaction:** Lights commands are silently dropped while a panel test mode is active.

### Lights Off (Legacy)

```
Byte 0: 'l'
Bytes 1-108: zeros (9 panels × 4 LEDs × 3 RGB)
Byte 109: '\n'
```

The `'l'` command is a legacy lights command that predates the `'2'`/`'3'`/`'4'` split. It is only used to blank all panel LEDs (e.g., before entering panel test mode). The 108 zero bytes is the minimum payload the firmware expects for this command to be valid.

### Platform LED Strip

```
Byte 0: 'L'
Byte 1: Strip index (0)
Byte 2: Number of LEDs (44)
Bytes 3+: RGB data (44 × 3 = 132 bytes)
```

Each pad has 44 LEDs on its platform edge strip. The SDK splits the 88-LED (264-byte) input buffer: first 132 bytes → pad 0, second 132 bytes → pad 1.

Requires firmware v4+ (`masterVersion >= 4` in config).

### Panel Test Mode

```
"t " + mode_char + "\n"
```

| Mode | Char | Effect |
|------|------|--------|
| Off | `'0'` | Disable test mode |
| Pressure Test | `'1'` | Panels light based on pressure |

The device times out after ~1 second without a refresh, so the SDK resends every second.

### Sensor Test Mode

**Request:**
```
"y" + mode_char + "\n"
```

| Mode | Char | Description |
|------|------|-------------|
| Uncalibrated | `'0'` | Raw ADC values |
| Calibrated | `'1'` | After calibration/tare |
| Noise | `'2'` | Variance of recent readings |
| Tare | `'3'` | Current baseline values |

**Response:**
```
Byte 0: 'y'
Byte 1: Mode char (echoed)
Byte 2: Size (number of uint16_t values / 2)
Bytes 3+: Interleaved uint16_t data (little-endian)
```

The response data is interleaved across all 9 panels at the bit level. Each uint16_t contains one bit per panel (bit 0 = panel 0, bit 8 = panel 8). The host de-interleaves by extracting the panel's bit from each uint16_t to reconstruct per-panel data.

**Per-panel data structure (after de-interleaving):**

```
Bits 0-2:   Signature (must be 0, 1, 0 to validate)
Bit 3:      Bad sensor flag [0]
Bit 4:      Bad sensor flag [1]
Bit 5:      Bad sensor flag [2]
Bit 6:      Bad sensor flag [3]
Bit 7:      Unused
Bytes 1-8:  Sensor levels [4] (int16_t each)
Bits 72-75: DIP switch setting (4 bits)
Bit 76:     Bad jumper [0]
Bit 77:     Bad jumper [1]
Bit 78:     Bad jumper [2]
Bit 79:     Bad jumper [3]
```

Each panel has 4 sensors. The sensor level meaning depends on the test mode.

### Factory Reset

```
"f\n"  (2 bytes)
```

Resets all configuration to factory defaults. The SDK follows up with a config read to update its cached state.

### Force Recalibration

```
"C\n"  (2 bytes)
```

Triggers an immediate sensor recalibration cycle.

### Re-enable Auto Lights

```
"S 1\n"  (4 bytes)
```

Resumes automatic panel lighting after it was disabled by sending lighting commands.

## Configuration Packet Structure

The configuration is a 250-byte packed struct (`SMXConfig`) that is sent/received as a binary blob.

### SMXConfig Layout (v5, current)

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 1 | `masterVersion` | Firmware version (read-only, write 0xFF) |
| 1 | 1 | `configVersion` | Config format version (0x05 = current) |
| 2 | 1 | `flags` | Bitfield (see SMXConfigFlags) |
| 3 | 2 | `debounceNodelayMilliseconds` | Debounce timing |
| 5 | 2 | `debounceDelayMilliseconds` | Debounce delay |
| 7 | 2 | `panelDebounceMicroseconds` | Per-panel debounce (default: 4000) |
| 9 | 1 | `autoCalibrationMaxDeviation` | Max calibration deviation |
| 10 | 1 | `badSensorMinimumDelaySeconds` | Bad sensor delay |
| 11 | 2 | `autoCalibrationAveragesPerUpdate` | Calibration averages |
| 13 | 2 | `autoCalibrationSamplesPerAverage` | Calibration samples |
| 15 | 2 | `autoCalibrationMaxTare` | Max tare value |
| 17 | 5 | `enabledSensors[5]` | Sensor enable bitmask |
| 22 | 1 | `autoLightsTimeout` | Auto-lights timeout (128ms units) |
| 23 | 27 | `stepColor[3×9]` | Per-panel RGB step colors |
| 50 | 3 | `platformStripColor[3]` | Default platform LED color |
| 53 | 2 | `autoLightPanelMask` | Which panels auto-light |
| 55 | 1 | `panelRotation` | Panel rotation (unused) |
| 56 | 144 | `panelSettings[9]` | Per-panel sensor settings (16 bytes each) |
| 200 | 1 | `preDetailsDelayMilliseconds` | Internal tunable |
| 201 | 49 | `padding` | Reserved (preserve when writing) |

Total: 250 bytes.

### Per-Panel Sensor Settings (packed_sensor_settings_t, 16 bytes)

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 1 | `loadCellLowThreshold` | Load cell activation threshold |
| 1 | 1 | `loadCellHighThreshold` | Load cell release threshold |
| 2 | 4 | `fsrLowThreshold[4]` | FSR activation thresholds (per sensor) |
| 6 | 4 | `fsrHighThreshold[4]` | FSR release thresholds (per sensor) |
| 10 | 2 | `combinedLowThreshold` | Combined sensor activation |
| 12 | 2 | `combinedHighThreshold` | Combined sensor release |
| 14 | 2 | `reserved` | Must not be modified |

### Config Flags

| Flag | Value | Meaning |
|------|-------|---------|
| `PlatformFlags_AutoLightingUsePressedAnimations` | `0x01` | Use pressed animation vs solid color |
| `PlatformFlags_FSR` | `0x02` | Panels use FSR sensors (vs load cells) |

### Firmware Version Compatibility

| Feature | Minimum Version |
|---------|----------------|
| New config format ('G'/'W') | v5 |
| Platform LED strip | v4 |
| Per-panel thresholds | Config v2 |
| Debounce delay field | Config v3 |
| FSR sensor support | v5 |

## Connection Handshake Sequence

Complete sequence from device discovery to fully connected. Note: the device begins sending Report 3 (input state) immediately upon USB connection — there is no activation command. The SDK simply ignores input until the handshake is complete.

```
Time    Host                              Device
─────   ────                              ──────
  0     Enumerate HID devices
        Find VID=0x2341, PID=0x8037
        Verify product = "StepManiaX"
        Open HID connection
                                           Device immediately begins sending
                                           Report 3 input state packets
        ─────────────────────────────────────────
  1     Send Device Info Request
        [05][80][00][00...00]         →
                                      ←   [06][80][size][cmd][pkt_size]
                                           [player][unused][serial:16]
                                           [fw_version:2][unused]
        Parse: player, serial, firmware
        ─────────────────────────────────────────
  2     Send Get Config ("G" for fw≥5)
        [05][05][01]['G']             →
                                      ←   [06][07][size]['G'][config_size]
                                           [config data: 250 bytes]
                                           (fragmented across multiple Report 6 packets)
        Parse config → m_Config
        m_bHaveConfig = true
        ─────────────────────────────────────────
  3     Device fully connected
        Fire Connected callback
        SDK now processes Report 3 input
        (was being received since step 0, but ignored until now)
```

## Packet Fragmentation Example

Sending a 252-byte config write command (`'W'` + size + 250 bytes of config):

```
Packet 1: [05][04][3D] + bytes 0-60 of command    (START, 61 bytes payload)
Packet 2: [05][00][3D] + bytes 61-121             (continuation, 61 bytes)
Packet 3: [05][00][3D] + bytes 122-182            (continuation, 61 bytes)
Packet 4: [05][00][3D] + bytes 183-243            (continuation, 61 bytes)
Packet 5: [05][01][08] + bytes 244-251            (END, 8 bytes payload)
```

Each packet is zero-padded to 64 bytes for the HID write.

## Error Handling

| Condition | Behavior |
|-----------|----------|
| HID read returns -1 | Set `m_bHadReadError`, main thread closes device |
| HID write returns -1 | Return error, cancel command, close device |
| Command timeout (2s) | Retry: push command back to front of queue |
| Malformed Report 6 | Skip packet (size validation) |
| Invalid device info | Ignore packet |
| START without END | Previous partial data discarded |

## Rate Limiting and Timing

| Operation | Rate Limit | Reason |
|-----------|-----------|--------|
| Config writes | 1 per second | EEPROM wear protection |
| Device enumeration | 1 per second | Reduce syscall overhead |
| Panel test mode refresh | Every ~1 second | Device timeout |
| Sensor test requests | Wait for response or 2s timeout | Sequential |
| USB polling | Configurable (default 1000µs) | CPU vs latency tradeoff |
| Main thread cycle | Configurable (default 50ms) | Connection responsiveness |

## Internal Hardware Architecture

Understanding the internal hardware helps explain the sensor test data format and input detection.

### Master Controller Unit (MCU)

The pad has a single master controller (MCU) that handles USB communication with the host. It is responsible for:
- Sending Report 3 input state to the host (immediately upon USB connection — no activation command needed)
- Receiving and executing commands from the host
- Communicating with the 9 individual panel PCBs

### Panel PCBs

Each of the 9 panels has its own circuit board with sensors and a microcontroller. Communication between the MCU and panels uses two separate channels:

**1. Data Bus (MCU → Panels, one-way):**

A single-pin UART daisy-chained through all 9 panels in this order:

```
MCU → Panel 0 → Panel 3 → Panel 6 → Panel 7 → Panel 4 → Panel 1 → Panel 2 → Panel 5 → Panel 8
```

This is a one-way broadcast channel. The MCU sends commands (e.g., sensor test requests, lighting data, configuration) to all panels simultaneously over this bus.

**2. Signal Wires (Panels → MCU, one per panel):**

Each panel has a dedicated signal wire running directly back to the MCU. This wire serves dual purposes:

- **Normal operation:** Held high at 5V. Pulled to ground when the panel detects a press. This gives the MCU immediate, per-panel press/release detection with minimal latency.
- **Sensor test data:** When the MCU requests sensor test data via the data bus, each panel responds individually through its own signal wire. The data is interleaved with the normal press/release signaling — the panel can still detect presses while transmitting test data.

### Why Sensor Test Data is Bit-Interleaved

The bit-level interleaving in sensor test responses (where each uint16_t contains one bit per panel) directly reflects the hardware: all 9 panels transmit their data simultaneously on their individual signal wires. The MCU reads all 9 signal lines in parallel, producing one bit per panel per sample. This parallel readout is then packed into uint16_t values (one bit per panel) and sent to the host as-is.

```
Signal wires (parallel):     USB packet (serialized):

Panel 0: ─────╥╥╥╥╥─────    uint16_t[0]: bit 0 = panel 0 bit 0
Panel 1: ─────╥╥╥╥╥─────                 bit 1 = panel 1 bit 0
Panel 2: ─────╥╥╥╥╥─────                 bit 2 = panel 2 bit 0
Panel 3: ─────╥╥╥╥╥─────                 ...
Panel 4: ─────╥╥╥╥╥─────                 bit 8 = panel 8 bit 0
Panel 5: ─────╥╥╥╥╥─────
Panel 6: ─────╥╥╥╥╥─────    uint16_t[1]: bit 0 = panel 0 bit 1
Panel 7: ─────╥╥╥╥╥─────                 bit 1 = panel 1 bit 1
Panel 8: ─────╥╥╥╥╥─────                 ...
              ↑ ↑ ↑ ↑ ↑
              Simultaneous bit samples → packed into uint16_t array
```

The SDK de-interleaves this on the host side to reconstruct per-panel sensor data.

## Unknown / Unconfirmed Details

The following aspects are inferred from code behavior but not confirmed by firmware documentation:

- **Report 3 timing**: The ~10 Hz idle / ~50 Hz active rates are observed, not specified. The firmware may behave differently in other conditions.
- **EEPROM write limits**: The 1-second rate limit is a conservative SDK-side protection. The actual EEPROM endurance is unknown.
- **Panel test mode timeout**: The ~1 second timeout is observed behavior. The exact timeout may vary by firmware version.
- **Panel signal wire protocol**: The exact protocol used to transmit sensor test data over the signal wires (interleaved with the normal 5V/GND press detection) is not fully documented. It works alongside press detection without interfering.
- **Data bus protocol**: The single-pin UART protocol used on the daisy-chained data bus is not documented. Likely a simple serial protocol at a fixed baud rate.
- **Command response ordering**: The protocol appears strictly sequential (one command in flight at a time). Whether the firmware supports pipelining is unknown.
- **Maximum command size**: The largest known command is config write at 252 bytes. The theoretical maximum is unlimited (fragmentation handles any size), but firmware buffer limits are unknown.
- **Undocumented commands**: There may be vendor-internal commands not exposed in the public SDK.
