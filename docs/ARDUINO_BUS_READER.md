# Arduino: Reading the SMX Internal Data Bus

Guide for reading the StepManiaX pad's internal UART data bus with an Arduino to detect panel lighting commands and extract judgement colors during gameplay.

## Hardware Setup

- Connect the SMX data line to an Arduino UART RX pin
- The data line is 5V logic — use a 5V-tolerant Arduino (Uno, Mega, etc.) or a level shifter for 3.3V boards
- Connect GND between the Arduino and the pad
- No pull-up/pull-down resistors needed — the MCU drives the line

## Protocol Summary

| Parameter | Value |
|-----------|-------|
| Baud rate | 250000 |
| Format | 8N1 (8 data bits, no parity, 1 stop bit) |
| Idle state | Continuous 0x00 bytes (looks like 25kHz clock) |
| Command framing | BREAK condition (~184µs LOW) before each command |
| Byte time | 40µs (10 bits × 4µs) |

## Command Framing

Every command is preceded by a **BREAK condition** — the line is held LOW for ~184µs (46 bit times), which is longer than any valid UART frame (10 bits = 40µs). This is the unambiguous frame delimiter.

```
... 0x00 0x00 ║ BREAK (184µs LOW) ║ IDLE (HIGH gap) ║ CMD + DATA ║ BREAK ...
```

After the BREAK, the line goes HIGH (UART idle) for a variable duration (600µs to 4ms), then the command byte and payload follow as normal UART bytes with no gaps between them.

## Detecting BREAK on Arduino

Arduino's hardware UART reports a BREAK as a **framing error** (the stop bit is LOW instead of HIGH). On AVR Arduinos, you can detect this via the UART error flags.

### Method 1: Framing Error Detection (recommended)

```cpp
#include <Arduino.h>

// Use Serial1 on boards with multiple UARTs (Mega, Leonardo, etc.)
// Or SoftwareSerial on Uno (but 250kbaud may be too fast for SoftwareSerial)

#define SMX_SERIAL Serial1
#define SMX_BAUD   250000

// Buffer for one full lighting command
#define CMD_BUF_SIZE 260
uint8_t cmdBuf[CMD_BUF_SIZE];
int cmdLen = 0;
bool inCommand = false;

void setup()
{
    Serial.begin(115200);   // debug output
    SMX_SERIAL.begin(SMX_BAUD, SERIAL_8N1);
}

void loop()
{
    while (SMX_SERIAL.available()) {
        // Check for framing error (BREAK condition)
        // On AVR: UCSRnA & (1 << FEn) indicates framing error
        // On ARM-based boards: check platform-specific error register

        uint8_t b = SMX_SERIAL.read();

        if (hasFramingError()) {
            // BREAK detected — previous command is complete
            if (inCommand && cmdLen > 0) {
                processCommand(cmdBuf, cmdLen);
            }
            cmdLen = 0;
            inCommand = false;
            continue;
        }

        if (!inCommand) {
            // First non-zero byte after BREAK+IDLE is the command byte
            if (b != 0x00) {
                inCommand = true;
                cmdBuf[0] = b;
                cmdLen = 1;
            }
            // Skip 0x00 bytes (idle stream between BREAK and command)
            continue;
        }

        // Accumulate command payload
        if (cmdLen < CMD_BUF_SIZE) {
            cmdBuf[cmdLen++] = b;
        }
    }
}

// Platform-specific framing error detection
// For AVR (Uno, Mega):
bool hasFramingError()
{
#if defined(__AVR__)
    // For Serial1 on Mega: check UCSR1A register
    return (UCSR1A & (1 << FE1)) != 0;
#else
    // For other platforms, you may need to check differently
    // or use the timing-based method below
    return false;
#endif
}
```

### Method 2: Timing-Based Detection (more portable)

If framing error detection isn't available, you can detect BREAK by timing the gap between bytes. A normal byte takes 40µs. If you see no valid byte for > 100µs, it's a BREAK.

```cpp
#include <Arduino.h>

#define SMX_SERIAL Serial1
#define SMX_BAUD   250000

#define CMD_BUF_SIZE 260
uint8_t cmdBuf[CMD_BUF_SIZE];
int cmdLen = 0;
bool inCommand = false;
unsigned long lastByteTime = 0;

// BREAK threshold: if no byte received for > 150µs, assume BREAK occurred
// (normal inter-byte gap is 0µs since bytes are back-to-back)
#define BREAK_TIMEOUT_US 150

void setup()
{
    Serial.begin(115200);
    SMX_SERIAL.begin(SMX_BAUD, SERIAL_8N1);
}

void loop()
{
    unsigned long now = micros();

    // Check for timeout (BREAK detection)
    if (inCommand && (now - lastByteTime) > BREAK_TIMEOUT_US) {
        processCommand(cmdBuf, cmdLen);
        cmdLen = 0;
        inCommand = false;
    }

    while (SMX_SERIAL.available()) {
        uint8_t b = SMX_SERIAL.read();
        lastByteTime = micros();

        if (!inCommand) {
            if (b != 0x00) {
                inCommand = true;
                cmdBuf[0] = b;
                cmdLen = 1;
            }
            continue;
        }

        if (cmdLen < CMD_BUF_SIZE) {
            cmdBuf[cmdLen++] = b;
        }
    }
}
```

### Method 3: Pin Interrupt (most reliable)

Monitor the RX pin with a pin-change interrupt to measure LOW duration directly.

```cpp
#include <Arduino.h>

#define SMX_SERIAL Serial1
#define SMX_BAUD   250000
#define RX_PIN     19  // Serial1 RX on Mega

volatile bool breakDetected = false;
volatile unsigned long lowStartTime = 0;

void setup()
{
    Serial.begin(115200);
    SMX_SERIAL.begin(SMX_BAUD, SERIAL_8N1);

    // Attach interrupt on RX pin to detect BREAK
    attachInterrupt(digitalPinToInterrupt(RX_PIN), rxPinChange, CHANGE);
}

void rxPinChange()
{
    if (digitalRead(RX_PIN) == LOW) {
        lowStartTime = micros();
    } else {
        // Rising edge — check how long it was LOW
        unsigned long duration = micros() - lowStartTime;
        if (duration > 100) {  // > 100µs = BREAK (normal byte LOW is max 36µs)
            breakDetected = true;
        }
    }
}

// In loop(), check breakDetected flag to know when a new command starts
```

## Processing Lighting Commands

```cpp
// Panel indices
enum Panel {
    PANEL_UL = 0, PANEL_U  = 1, PANEL_UR = 2,
    PANEL_L  = 3, PANEL_C  = 4, PANEL_R  = 5,
    PANEL_DL = 6, PANEL_D  = 7, PANEL_DR = 8
};

struct RGB {
    uint8_t r, g, b;
};

// Extract the dominant color for each panel from a '2' command
// '2' payload: 9 panels × 8 LEDs × 3 bytes RGB = 216 bytes
// The first LED of each panel is representative for solid judgement fills
RGB getPanelColor(const uint8_t *cmdBuf, int cmdLen, int panel)
{
    RGB color = {0, 0, 0};

    // Verify this is a '2' command with full payload
    if (cmdBuf[0] != '2' || cmdLen < 217)
        return color;

    int offset = 1 + (panel * 24);  // skip cmd byte, 24 bytes per panel
    color.r = cmdBuf[offset];
    color.g = cmdBuf[offset + 1];
    color.b = cmdBuf[offset + 2];
    return color;
}

void processCommand(const uint8_t *cmdBuf, int cmdLen)
{
    if (cmdLen < 1)
        return;

    uint8_t cmd = cmdBuf[0];

    switch (cmd) {
    case '2':  // Top half panel LEDs (contains judgement colors)
        if (cmdLen >= 217) {
            processLightingTop(cmdBuf, cmdLen);
        }
        break;
    case '3':  // Bottom half panel LEDs
        // Same structure as '2', 217 bytes
        break;
    case '4':  // Inner 3×3 grid (fw v4+)
        // 244 bytes: cmd + 9 panels × 27 bytes (9 LEDs × 3 RGB)
        break;
    default:
        break;
    }
}
```

## Detecting Judgement Colors

The game sends panel colors through the lighting commands. Judgement colors appear as solid fills on the stepped panel, then fade back to the idle animation.

### Known Judgement Colors (as seen on the bus, after 0.6666× scaling)

| Judgement | R | G | B | Notes |
|-----------|---|---|---|-------|
| Perfect!! | ~169 | ~169 | ~169 | White. Fades through grays. |
| Perfect! | ~169 | ~131-148 | ~3-11 | Yellow/gold. |
| Early | ~0 | ~169 | ~0 | Green. Fades through dimmer greens. |
| Late | ~11 | ~0 | ~169 | Deep blue/indigo. |
| Miss | ~169 | ~0 | ~0-1 | Red. |

These are approximate — the game uses animation interpolation so values vary slightly frame-to-frame. The idle/background animation color is typically (14,103,147) = teal/cyan.

### Color Detection Code

```cpp
enum Judgement {
    JUDGEMENT_NONE,
    JUDGEMENT_PERFECT_PLUS,  // Perfect!! (white)
    JUDGEMENT_PERFECT,       // Perfect!  (yellow)
    JUDGEMENT_EARLY,         // Early     (green)
    JUDGEMENT_LATE,          // Late      (blue)
    JUDGEMENT_MISS           // Miss      (red)
};

// Detect judgement from a panel's RGB color (bus values, already scaled)
Judgement detectJudgement(uint8_t r, uint8_t g, uint8_t b)
{
    // Black or very dim = no judgement active
    if (r < 20 && g < 20 && b < 20)
        return JUDGEMENT_NONE;

    // White: all channels high and similar
    if (r > 130 && g > 130 && b > 130 && abs(r - g) < 20 && abs(g - b) < 20)
        return JUDGEMENT_PERFECT_PLUS;

    // Yellow: R high, G medium-high, B low
    if (r > 130 && g > 90 && b < 40)
        return JUDGEMENT_PERFECT;

    // Green: G dominant, R and B low
    if (g > 80 && r < 30 && b < 30)
        return JUDGEMENT_EARLY;

    // Blue/indigo: B dominant, R and G low
    if (b > 80 && r < 30 && g < 30)
        return JUDGEMENT_LATE;

    // Red: R dominant, G and B low
    if (r > 80 && g < 30 && b < 30)
        return JUDGEMENT_MISS;

    return JUDGEMENT_NONE;  // animation/transition color
}

void processLightingTop(const uint8_t *cmdBuf, int cmdLen)
{
    for (int panel = 0; panel < 9; panel++) {
        RGB color = getPanelColor(cmdBuf, cmdLen, panel);
        Judgement j = detectJudgement(color.r, color.g, color.b);

        if (j != JUDGEMENT_NONE) {
            // A judgement was detected on this panel!
            onJudgement(panel, j);
        }
    }
}

void onJudgement(int panel, Judgement j)
{
    const char *names[] = {"", "Perfect!!", "Perfect!", "Early", "Late", "Miss"};
    const char *panelNames[] = {"UL","U","UR","L","C","R","DL","D","DR"};

    Serial.print(panelNames[panel]);
    Serial.print(": ");
    Serial.println(names[j]);
}
```

## Complete Minimal Example

```cpp
// SMX Data Bus Reader — Judgement Color Detector
// Connect SMX data line to Serial1 RX (pin 19 on Mega)

#define SMX_SERIAL Serial1
#define SMX_BAUD   250000
#define CMD_BUF_SIZE 260
#define BREAK_TIMEOUT_US 150

uint8_t cmdBuf[CMD_BUF_SIZE];
int cmdLen = 0;
bool inCommand = false;
unsigned long lastByteTime = 0;

enum Judgement { J_NONE, J_PERFECT_PLUS, J_PERFECT, J_EARLY, J_LATE, J_MISS };
const char *jNames[] = {"", "Perfect!!", "Perfect!", "Early", "Late", "Miss"};
const char *panelNames[] = {"UL","U","UR","L","C","R","DL","D","DR"};

void setup()
{
    Serial.begin(115200);
    SMX_SERIAL.begin(SMX_BAUD, SERIAL_8N1);
    Serial.println("SMX Judgement Detector ready");
}

void loop()
{
    unsigned long now = micros();

    if (inCommand && (now - lastByteTime) > BREAK_TIMEOUT_US) {
        if (cmdBuf[0] == '2' && cmdLen >= 217) {
            for (int p = 0; p < 9; p++) {
                int off = 1 + p * 24;
                Judgement j = detect(cmdBuf[off], cmdBuf[off+1], cmdBuf[off+2]);
                if (j != J_NONE) {
                    Serial.print(panelNames[p]);
                    Serial.print(": ");
                    Serial.println(jNames[j]);
                }
            }
        }
        cmdLen = 0;
        inCommand = false;
    }

    while (SMX_SERIAL.available()) {
        uint8_t b = SMX_SERIAL.read();
        lastByteTime = micros();

        if (!inCommand) {
            if (b != 0x00) {
                inCommand = true;
                cmdBuf[0] = b;
                cmdLen = 1;
            }
            continue;
        }

        if (cmdLen < CMD_BUF_SIZE)
            cmdBuf[cmdLen++] = b;
    }
}

Judgement detect(uint8_t r, uint8_t g, uint8_t b)
{
    if (r > 130 && g > 130 && b > 130 && abs((int)r-g) < 20 && abs((int)g-b) < 20)
        return J_PERFECT_PLUS;
    if (r > 130 && g > 90 && b < 40)
        return J_PERFECT;
    if (g > 80 && r < 30 && b < 30)
        return J_EARLY;
    if (b > 80 && r < 30 && g < 30)
        return J_LATE;
    if (r > 80 && g < 30 && b < 30)
        return J_MISS;
    return J_NONE;
}
```

## Known Internal Bus Commands

For reference, all commands observed on the data bus:

| Command | Hex | Payload Size | Purpose |
|---------|-----|-------------|---------|
| `'2'` + data | 0x32 | 217 total | Top half panel LEDs (8 LEDs × 9 panels × RGB) |
| `'3'` + data | 0x33 | 217 total | Bottom half panel LEDs |
| `'4'` + data | 0x34 | 244 total | Inner 3×3 grid LEDs (9 LEDs × 9 panels × RGB) |
| `'l'` | 0x6C | 1 | Lights off |
| `'T0'` | 0x54 0x30 | 2 | Panel test mode OFF |
| `'T1'` | 0x54 0x31 | 2 | Panel test mode ON (pressure test) |
| `'w'` + data | 0x77 | 253 | Config write to panels |
| `'B1P'` | 0x42 0x31 0x50 | 3 | Sensor test data request |
| `'U'` | 0x55 | varies | Unknown (calibration?) |
| `'R'` | 0x52 | 1 | Panel reset (boot only) |
| `'G'` + 0xFF | 0x47 0xFF | 2+ | Panel readiness poll (boot only) |

## Timing Considerations

- Full lighting cycle: `'4'` → `'2'` → `'3'` at 30 FPS = ~33ms per cycle
- Each `'2'`/`'3'` frame: 217 bytes × 40µs = 8.68ms transmission time
- Each `'4'` frame: 244 bytes × 40µs = 9.76ms transmission time
- BREAK + IDLE gap: ~1-4ms between commands
- Total cycle: ~30ms (matches 30 FPS)

At 250kbaud, the Arduino UART buffer (64 bytes on AVR) will overflow if you don't read fast enough. Use the largest buffer available or read bytes in a tight loop. On ARM-based Arduinos (Due, Teensy, etc.) this is less of a concern.

## Notes

- The colors on the bus are pre-scaled by 0.6666× from the game's original values. To recover the original color, multiply by 1.5 (but for detection purposes, just match against the bus values directly).
- During gameplay, panels show a mix of judgement colors and background animations. Judgement colors appear as brief solid fills that fade out over several frames.
- Only the arrow panels (U, D, L, R) and center will show judgement colors during normal gameplay. Corners are typically disabled.
- The `'2'` command contains the top 2 rows of each panel's 4×4 LED grid. For solid judgement fills, all LEDs in the panel will be the same color, so reading just the first LED (bytes 1-3 for panel 0) is sufficient.
