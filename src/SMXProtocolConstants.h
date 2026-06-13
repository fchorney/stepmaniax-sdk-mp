#ifndef SMXProtocolConstants_h
#define SMXProtocolConstants_h

#include <cstdint>

#include "SMX.h"  // public source of truth for the hardware-shape constants below

// USB device identification (SMX_USB_VENDOR_ID / SMX_USB_PRODUCT_ID /
// SMX_USB_PRODUCT_STRING are public macros from SMX.h).

// Panel and LED geometry — internal aliases of the public SMX_* constants, so
// the existing unprefixed names keep working without duplicating the values.
static constexpr int NUM_PANELS             = SMX_NUM_PANELS;
static constexpr int LEDS_PER_PANEL_16      = SMX_LEDS_PER_PANEL_16;
static constexpr int LEDS_PER_PANEL_25      = SMX_LEDS_PER_PANEL_25;
static constexpr int PLATFORM_STRIP_LEDS    = SMX_PLATFORM_STRIP_LEDS;
static constexpr int BYTES_PER_PAD_16       = SMX_BYTES_PER_PAD_16;  // 432
static constexpr int BYTES_PER_PAD_25       = SMX_BYTES_PER_PAD_25;  // 675

// Color scaling applied to all LED values before sending to the device.
// Values above ~170 don't make LEDs brighter; this improves contrast and reduces power draw.
static constexpr float LED_COLOR_SCALE      = 0.6666f;

// Legacy lights-off command payload size (9 panels × 4 LEDs × 3 RGB).
static constexpr int LEGACY_LIGHTS_PAYLOAD_SIZE = 108;

// Timing constants.
static constexpr double CONFIG_WRITE_RATE_LIMIT_SECONDS = 1.0;  // Min interval between config writes
static constexpr double SENSOR_TEST_TIMEOUT_SECONDS     = 2.0;  // Timeout for sensor test response
static constexpr int    SENSOR_TEST_POLL_WAIT_MS        = 20;   // Main-loop wait while sensor test active (targets ~20Hz+)
static constexpr double PANEL_TEST_REFRESH_SECONDS      = 1.0;  // Resend panel test mode interval
static constexpr double ENUMERATION_INTERVAL_SECONDS    = 1.0;  // Min interval between HID enumerations
static constexpr double LIGHTS_FRAME_INTERVAL           = 1.0/30.0;  // 30 FPS rate limit for lights
static constexpr double LIGHTS_LEGACY_COMMAND_DELAY     = 1.0/60.0;  // Delay between commands (fw < 4)
static constexpr double ANIMATION_PAUSE_DURATION        = 0.1;  // Pause animation on direct lights call

#endif
