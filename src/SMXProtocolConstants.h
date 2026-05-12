#ifndef SMXProtocolConstants_h
#define SMXProtocolConstants_h

#include <cstdint>

// USB device identification.
static constexpr uint16_t SMX_USB_VENDOR_ID  = 0x2341;
static constexpr uint16_t SMX_USB_PRODUCT_ID = 0x8037;
#define SMX_USB_PRODUCT_STRING L"StepManiaX"

// Panel and LED geometry.
static constexpr int NUM_PANELS             = 9;    // Panels per pad (3×3 grid)
static constexpr int LEDS_PER_PANEL_16      = 16;   // Outer 4×4 grid only (legacy)
static constexpr int LEDS_PER_PANEL_25      = 25;   // Outer 4×4 + inner 3×3 (firmware v4+)
static constexpr int PLATFORM_STRIP_LEDS    = 44;   // LEDs in the platform edge strip per pad
static constexpr int BYTES_PER_PAD_16       = NUM_PANELS * LEDS_PER_PANEL_16 * 3;  // 432
static constexpr int BYTES_PER_PAD_25       = NUM_PANELS * LEDS_PER_PANEL_25 * 3;  // 675

// Color scaling applied to all LED values before sending to the device.
// Values above ~170 don't make LEDs brighter; this improves contrast and reduces power draw.
static constexpr float LED_COLOR_SCALE      = 0.6666f;

// Legacy lights-off command payload size (9 panels × 4 LEDs × 3 RGB).
static constexpr int LEGACY_LIGHTS_PAYLOAD_SIZE = 108;

// Timing constants.
static constexpr double CONFIG_WRITE_RATE_LIMIT_SECONDS = 1.0;  // Min interval between config writes
static constexpr double SENSOR_TEST_TIMEOUT_SECONDS     = 2.0;  // Timeout for sensor test response
static constexpr double PANEL_TEST_REFRESH_SECONDS      = 1.0;  // Resend panel test mode interval
static constexpr double ENUMERATION_INTERVAL_SECONDS    = 1.0;  // Min interval between HID enumerations
static constexpr double LIGHTS_FRAME_INTERVAL           = 1.0/30.0;  // 30 FPS rate limit for lights
static constexpr double LIGHTS_LEGACY_COMMAND_DELAY     = 1.0/60.0;  // Delay between commands (fw < 4)
static constexpr double ANIMATION_PAUSE_DURATION        = 0.1;  // Pause animation on direct lights call

#endif
