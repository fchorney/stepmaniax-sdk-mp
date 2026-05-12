// SMXPanelAnimation: GIF-based panel animation loading and playback.
//
// This implements SMX_LightsAnimation_Load and SMX_LightsAnimation_SetAuto.
// Animations are decoded from GIF files and played back at 30 FPS by calling
// SMX_SetLights2 internally.

#include "SMX.h"
#include "SMXProtocolConstants.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4244) // conversion from '__int64' to 'long' inside gif_load
#endif
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include "vendor/gif_load.h"
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
#ifdef _MSC_VER
#pragma warning(pop)
#endif

using namespace std;

// Declared in SMXHelpers.h
namespace SMX { double GetMonotonicTime(); }

namespace {

// --- GIF Decoding ---

struct DecodedFrame {
    vector<uint8_t> rgba; // width * height * 4 bytes (RGBA)
    int width = 0;
    int height = 0;
    float duration = 0; // seconds
};

struct GifDecodeState {
    vector<DecodedFrame> frames;
    vector<uint8_t> canvas; // composited RGBA canvas
    vector<uint8_t> prevCanvas; // for GIF_PREV disposal
    int width = 0;
    int height = 0;
};

void GifFrameCallback(void *data, struct GIF_WHDR *whdr)
{
    auto *state = static_cast<GifDecodeState*>(data);

    // Initialize canvas on first frame.
    if(whdr->ifrm == 0)
    {
        state->width = whdr->xdim;
        state->height = whdr->ydim;
        state->canvas.assign(whdr->xdim * whdr->ydim * 4, 0);
    }

    // Save canvas for GIF_PREV disposal.
    state->prevCanvas = state->canvas;

    // Render this frame's pixels onto the canvas.
    int frxd = whdr->frxd, fryd = whdr->fryd;
    int frxo = whdr->frxo, fryo = whdr->fryo;

    // Handle interlacing.
    for(int y = 0; y < fryd; y++)
    {
        int srcY = y;
        if(whdr->intr)
        {
            // Deinterlace: GIF interlace passes
            static const int starts[] = {0, 4, 2, 1};
            static const int steps[]  = {8, 8, 4, 2};
            int pass = 0, row = 0;
            for(pass = 0; pass < 4; pass++)
            {
                for(int r = starts[pass]; r < fryd; r += steps[pass])
                {
                    if(row == y) { srcY = r; goto found; }
                    row++;
                }
            }
            found:;
        }

        for(int x = 0; x < frxd; x++)
        {
            int srcIdx = srcY * frxd + x;
            int palIdx = whdr->bptr[srcIdx];

            // Skip transparent pixels.
            if(palIdx == whdr->tran)
                continue;

            int dstIdx = ((fryo + y) * state->width + (frxo + x)) * 4;
            state->canvas[dstIdx + 0] = whdr->cpal[palIdx].R;
            state->canvas[dstIdx + 1] = whdr->cpal[palIdx].G;
            state->canvas[dstIdx + 2] = whdr->cpal[palIdx].B;
            state->canvas[dstIdx + 3] = 255;
        }
    }

    // Store the composited frame.
    DecodedFrame frame;
    frame.rgba = state->canvas;
    frame.width = state->width;
    frame.height = state->height;

    // Convert GIF time (10ms units) to seconds. Snap 30ms/40ms to exactly 30 FPS.
    int ms = whdr->time * 10;
    if(ms == 30 || ms == 40)
        frame.duration = 1.0f / 30.0f;
    else if(ms <= 0)
        frame.duration = 1.0f / 30.0f; // default for 0-delay frames
    else
        frame.duration = ms / 1000.0f;

    state->frames.push_back(std::move(frame));

    // Apply disposal for next frame.
    if(whdr->mode == GIF_BKGD)
    {
        // Clear the frame region to transparent.
        for(int y = 0; y < fryd; y++)
            for(int x = 0; x < frxd; x++)
            {
                int idx = ((fryo + y) * state->width + (frxo + x)) * 4;
                state->canvas[idx + 0] = 0;
                state->canvas[idx + 1] = 0;
                state->canvas[idx + 2] = 0;
                state->canvas[idx + 3] = 0;
            }
    }
    else if(whdr->mode == GIF_PREV)
    {
        state->canvas = state->prevCanvas;
    }
    // GIF_NONE / GIF_CURR: leave canvas as-is.
}

bool DecodeGif(const char *data, int size, vector<DecodedFrame> &frames)
{
    GifDecodeState state;
    long result = GIF_Load(const_cast<void*>(static_cast<const void*>(data)),
                           static_cast<long>(size), GifFrameCallback, nullptr, &state, 0L);
    if(result == 0 && state.frames.empty())
        return false;
    frames = std::move(state.frames);
    return !frames.empty();
}

// --- Panel Extraction ---

struct PanelFrame {
    uint8_t rgb[25 * 3]; // Up to 25 LEDs × 3 bytes RGB
    int numLeds;         // 16 or 25
};

// Extract a single panel's LED data from a decoded RGBA frame.
// For 14×15 GIFs: 4×4 grid at (col*5, row*5), 16 LEDs.
// For 23×24 GIFs: outer 4×4 at even coords + inner 3×3 at odd coords, 25 LEDs.
PanelFrame ExtractPanel(const DecodedFrame &frame, int panel)
{
    PanelFrame pf;
    memset(&pf, 0, sizeof(pf));

    int col = panel % 3;
    int row = panel / 3;

    if(frame.width == 14)
    {
        // 14×15: each panel is 4×4 at (col*5, row*5)
        pf.numLeds = 16;
        int baseX = col * 5;
        int baseY = row * 5;
        int led = 0;
        for(int dy = 0; dy < 4; dy++)
        {
            for(int dx = 0; dx < 4; dx++)
            {
                int idx = ((baseY + dy) * frame.width + (baseX + dx)) * 4;
                pf.rgb[led * 3 + 0] = frame.rgba[idx + 0];
                pf.rgb[led * 3 + 1] = frame.rgba[idx + 1];
                pf.rgb[led * 3 + 2] = frame.rgba[idx + 2];
                led++;
            }
        }
    }
    else
    {
        // 23×24: outer 4×4 at even coords, inner 3×3 at odd coords
        pf.numLeds = 25;
        int baseX = col * 8;
        int baseY = row * 8;
        int led = 0;
        // Outer 4×4 grid (sampled at even positions)
        for(int dy = 0; dy < 4; dy++)
        {
            for(int dx = 0; dx < 4; dx++)
            {
                int idx = ((baseY + dy * 2) * frame.width + (baseX + dx * 2)) * 4;
                pf.rgb[led * 3 + 0] = frame.rgba[idx + 0];
                pf.rgb[led * 3 + 1] = frame.rgba[idx + 1];
                pf.rgb[led * 3 + 2] = frame.rgba[idx + 2];
                led++;
            }
        }
        // Inner 3×3 grid (sampled at odd positions)
        for(int dy = 0; dy < 3; dy++)
        {
            for(int dx = 0; dx < 3; dx++)
            {
                int idx = ((baseY + dy * 2 + 1) * frame.width + (baseX + dx * 2 + 1)) * 4;
                pf.rgb[led * 3 + 0] = frame.rgba[idx + 0];
                pf.rgb[led * 3 + 1] = frame.rgba[idx + 1];
                pf.rgb[led * 3 + 2] = frame.rgba[idx + 2];
                led++;
            }
        }
    }
    return pf;
}

// --- Animation State ---

struct PanelAnimationData {
    vector<PanelFrame> frames[9]; // Per-panel frames
    vector<float> durations;      // Frame durations (shared across panels)
    int loopFrame = 0;            // Frame to loop back to
    int numLeds = 25;             // 16 or 25
};

struct AnimationPlaybackState {
    int currentFrame = 0;
    float timeInFrame = 0;
    bool playing = false;

    void Reset() { currentFrame = 0; timeInFrame = 0; }
};

// Global animation state, protected by g_AnimMutex.
mutex g_AnimMutex;
PanelAnimationData g_Animations[2][2]; // [pad][type]
AnimationPlaybackState g_PlaybackState[2][2][9]; // [pad][type][panel]
atomic<bool> g_bAutoAnimating{false};
atomic<bool> g_bAnimThreadSending{false}; // true while animation thread is calling SetLights2
thread g_AnimThread;
atomic<bool> g_bAnimShutdown{false};
double g_fStopAnimatingUntil = 0;

// --- Animation Thread ---

void AnimationThreadMain()
{
    const int iFrameMs = 33; // ~30 FPS
    vector<char> lightData(2 * BYTES_PER_PAD_25, 0); // Reused each frame, max possible size

    while(!g_bAnimShutdown.load(memory_order_relaxed))
    {
        auto tFrameStart = chrono::steady_clock::now();

        // Check if temporarily paused (SMX_SetLights2 was called directly).
        if(SMX::GetMonotonicTime() < g_fStopAnimatingUntil)
        {
            this_thread::sleep_for(chrono::milliseconds(iFrameMs));
            continue;
        }

        // Build lights data for both pads.
        int iLedsPerPanel = 25;
        int iBytesPerPanel;
        int iBytesPerPad;

        {
            lock_guard<mutex> lock(g_AnimMutex);

            // Determine LED count from loaded animations.
            for(int pad = 0; pad < 2; pad++)
                for(int type = 0; type < 2; type++)
                    if(!g_Animations[pad][type].durations.empty())
                        iLedsPerPanel = g_Animations[pad][type].numLeds;

            iBytesPerPanel = iLedsPerPanel * 3;
            iBytesPerPad = 9 * iBytesPerPanel;
            memset(lightData.data(), 0, 2 * iBytesPerPad);

            for(int pad = 0; pad < 2; pad++)
            {
                // Get current input state for pressed animation.
                uint16_t iPadState = SMX_GetInputState(pad);

                for(int panel = 0; panel < 9; panel++)
                {
                    char *out = &lightData[pad * iBytesPerPad + panel * iBytesPerPanel];

                    // Apply released animation (always playing).
                    auto &relAnim = g_Animations[pad][SMX_LightsType_Released];
                    auto &relState = g_PlaybackState[pad][SMX_LightsType_Released][panel];
                    if(!relAnim.durations.empty())
                    {
                        relState.playing = true;
                        const PanelFrame &pf = relAnim.frames[panel][relState.currentFrame];
                        int copyBytes = min(iBytesPerPanel, pf.numLeds * 3);
                        memcpy(out, pf.rgb, copyBytes);
                    }

                    // Overlay pressed animation if panel is pressed.
                    bool bPressed = (iPadState & (1 << panel)) != 0;
                    auto &pressAnim = g_Animations[pad][SMX_LightsType_Pressed];
                    auto &pressState = g_PlaybackState[pad][SMX_LightsType_Pressed][panel];
                    if(!pressAnim.durations.empty())
                    {
                        if(bPressed)
                        {
                            pressState.playing = true;
                            const PanelFrame &pf = pressAnim.frames[panel][pressState.currentFrame];
                            // Overlay: only non-black pixels (transparency substitute)
                            for(int i = 0; i < pf.numLeds && i < iLedsPerPanel; i++)
                            {
                                if(pf.rgb[i*3] || pf.rgb[i*3+1] || pf.rgb[i*3+2])
                                {
                                    out[i*3+0] = pf.rgb[i*3+0];
                                    out[i*3+1] = pf.rgb[i*3+1];
                                    out[i*3+2] = pf.rgb[i*3+2];
                                }
                            }
                        }
                        else
                        {
                            // Reset pressed animation when released.
                            pressState.playing = false;
                            pressState.Reset();
                        }
                    }
                }

                // Advance animation timing.
                float dt = 1.0f / 30.0f;
                for(int type = 0; type < 2; type++)
                {
                    auto &anim = g_Animations[pad][type];
                    if(anim.durations.empty())
                        continue;
                    for(int panel = 0; panel < 9; panel++)
                    {
                        auto &ps = g_PlaybackState[pad][type][panel];
                        if(!ps.playing)
                            continue;
                        ps.timeInFrame += dt;
                        if(ps.timeInFrame >= anim.durations[ps.currentFrame])
                        {
                            ps.timeInFrame -= anim.durations[ps.currentFrame];
                            ps.currentFrame++;
                            if(ps.currentFrame >= (int)anim.durations.size())
                                ps.currentFrame = anim.loopFrame;
                        }
                    }
                }
            }
        }

        // Send lights. Set flag so TemporaryStop knows not to pause us.
        int totalSize = iLedsPerPanel == 25 ? 1350 : 864;
        g_bAnimThreadSending.store(true, memory_order_relaxed);
        SMX_SetLights2(lightData.data(), totalSize);
        g_bAnimThreadSending.store(false, memory_order_relaxed);

        // Sleep for remainder of frame.
        auto tFrameEnd = tFrameStart + chrono::milliseconds(iFrameMs);
        this_thread::sleep_until(tFrameEnd);
    }
}

} // anonymous namespace

// --- Public API ---

SMX_API bool SMX_LightsAnimation_Load(const char *gif, int size, int pad, SMX_LightsType type, const char **error)
{
    static const char *sNoError = "";
    if(!error) { static const char *dummy; error = &dummy; }
    *error = sNoError;

    if(!gif || size <= 0) { *error = "No GIF data provided."; return false; }
    if(pad < 0 || pad > 1) { *error = "Invalid pad index."; return false; }
    if(type != SMX_LightsType_Released && type != SMX_LightsType_Pressed)
    { *error = "Invalid animation type."; return false; }

    // Decode GIF.
    vector<DecodedFrame> frames;
    if(!DecodeGif(gif, size, frames))
    { *error = "The GIF couldn't be read."; return false; }

    // Validate dimensions.
    int w = frames[0].width, h = frames[0].height;
    if((w != 14 || h != 15) && (w != 23 || h != 24))
    { *error = "The GIF must be 14x15 or 23x24."; return false; }

    // Extract per-panel frames.
    PanelAnimationData anim;
    anim.numLeds = (w == 14) ? 16 : 25;
    anim.loopFrame = 0;

    for(int f = 0; f < (int)frames.size(); f++)
    {
        // Check for loop frame marker (bottom-left pixel is white).
        int markerIdx = ((h - 1) * w + 0) * 4;
        if(frames[f].rgba[markerIdx + 3] == 255 && frames[f].rgba[markerIdx + 0] >= 128)
        {
            if(anim.loopFrame == 0)
                anim.loopFrame = f;
        }

        anim.durations.push_back(frames[f].duration);
        for(int panel = 0; panel < 9; panel++)
            anim.frames[panel].push_back(ExtractPanel(frames[f], panel));
    }

    // Commit to global state.
    lock_guard<mutex> lock(g_AnimMutex);
    g_Animations[pad][type] = std::move(anim);
    // Reset playback state for this animation.
    for(int panel = 0; panel < 9; panel++)
        g_PlaybackState[pad][type][panel] = AnimationPlaybackState();

    return true;
}

SMX_API void SMX_LightsAnimation_SetAuto(bool enable)
{
    if(enable)
    {
        if(g_bAutoAnimating.load())
            return; // Already running.
        g_bAnimShutdown = false;
        g_bAutoAnimating = true;
        g_AnimThread = thread(AnimationThreadMain);
    }
    else
    {
        if(!g_bAutoAnimating.load())
            return; // Not running.
        g_bAnimShutdown = true;
        if(g_AnimThread.joinable())
            g_AnimThread.join();
        g_bAutoAnimating = false;
    }
}

// Called from SMX_SetLights2 to temporarily pause animation.
void SMXLightsAnimation_TemporaryStop()
{
    // Don't pause if the animation thread itself is sending lights.
    if(g_bAnimThreadSending.load(memory_order_relaxed))
        return;
    if(g_bAutoAnimating.load(memory_order_relaxed))
        g_fStopAnimatingUntil = SMX::GetMonotonicTime() + ANIMATION_PAUSE_DURATION;
}

// ---------------------------------------------------------------------------
// Animation Upload
// ---------------------------------------------------------------------------
// Converts GIF animations to the firmware's 4-bit paletted format and generates
// the upload command sequence for writing to panel EEPROM.

namespace {

// --- Firmware data structures ---

#pragma pack(push, 1)
struct fw_color_t { uint8_t rgb[3]; };
struct fw_palette_t { fw_color_t colors[15]; };  // 45 bytes
struct fw_graphic_t { uint8_t data[13]; };       // 25 pixels packed as 4-bit nibbles

struct fw_panel_animation_data_t {
    fw_graphic_t graphics[64];  // 832 bytes
    fw_palette_t palettes[2];   // 90 bytes
};                              // total: 922 bytes

struct fw_animation_timing_t {
    uint8_t loop_animation_frame;
    uint8_t frames[64];  // graphic indices (0xFF = end/loop)
    uint8_t delay[64];   // duration in 30FPS frame counts
};                       // total: 129 bytes

struct fw_upload_packet {
    uint8_t cmd = 'm';
    uint8_t panel = 0;
    uint8_t animation_idx = 0;
    uint8_t final_packet = 0;
    uint16_t offset = 0;
    uint8_t size = 0;
    uint8_t data[240] = {};
};

struct fw_delay_packet {
    uint8_t cmd = 'd';
    uint16_t milliseconds = 0;
};
#pragma pack(pop)

// --- Palette quantization ---

// Find a color's index in the palette, or 0xFF if not found.
static uint8_t FindColorInPalette(const fw_palette_t &pal, uint8_t r, uint8_t g, uint8_t b)
{
    for(int i = 0; i < 15; i++)
        if(pal.colors[i].rgb[0] == r && pal.colors[i].rgb[1] == g && pal.colors[i].rgb[2] == b)
            return i;
    return 0xFF;
}

// Build a 15-color palette from all frames of a single panel's animation.
// Returns false if more than 15 unique colors are used.
static bool BuildPalette(const vector<PanelFrame> &frames, fw_palette_t &palette)
{
    memset(&palette, 0, sizeof(palette));
    int nextColor = 0;

    for(const auto &frame : frames)
    {
        for(int i = 0; i < frame.numLeds; i++)
        {
            uint8_t r = frame.rgb[i*3+0];
            uint8_t g = frame.rgb[i*3+1];
            uint8_t b = frame.rgb[i*3+2];

            // Transparent (alpha=0 in the original) maps to index 15, skip it.
            // We treat fully black as transparent for the upload path.
            if(r == 0 && g == 0 && b == 0)
                continue;

            if(FindColorInPalette(palette, r, g, b) != 0xFF)
                continue;

            if(nextColor >= 15)
                return false;

            palette.colors[nextColor].rgb[0] = r;
            palette.colors[nextColor].rgb[1] = g;
            palette.colors[nextColor].rgb[2] = b;
            nextColor++;
        }
    }
    return true;
}

// Pack a single frame into a 4-bit graphic using the given palette.
static void PackGraphic(const PanelFrame &frame, const fw_palette_t &palette, fw_graphic_t &out)
{
    memset(&out, 0, sizeof(out));
    for(int i = 0; i < 25; i++)
    {
        uint8_t palIdx;
        if(i >= frame.numLeds)
        {
            palIdx = 15; // transparent
        }
        else
        {
            uint8_t r = frame.rgb[i*3+0];
            uint8_t g = frame.rgb[i*3+1];
            uint8_t b = frame.rgb[i*3+2];
            if(r == 0 && g == 0 && b == 0)
                palIdx = 15; // transparent
            else
            {
                palIdx = FindColorInPalette(palette, r, g, b);
                if(palIdx == 0xFF) palIdx = 0; // shouldn't happen if BuildPalette succeeded
            }
        }

        // Pack into nibbles: high nibble first.
        if(i & 1)
            out.data[i/2] |= (palIdx & 0x0F);
        else
            out.data[i/2] |= (palIdx & 0x0F) << 4;
    }
}

// Convert frame durations to 30 FPS delay counts.
static vector<uint8_t> ComputeFrameDelays(const vector<float> &durations)
{
    vector<uint8_t> delays;
    for(float dur : durations)
    {
        int frames = max(1, (int)(dur * 30.0f + 0.5f));
        delays.push_back((uint8_t)min(frames, 255));
    }
    return delays;
}

// Create upload packets for a block of data.
static void CreateUploadPackets(vector<fw_upload_packet> &packets,
    const void *data, int startOffset, int dataSize, uint8_t panel, uint8_t animIdx)
{
    const uint8_t *buf = (const uint8_t*)data;
    for(int offset = 0; offset < dataSize; )
    {
        fw_upload_packet pkt;
        pkt.panel = panel;
        pkt.animation_idx = animIdx;
        pkt.offset = startOffset + offset;
        pkt.size = (uint8_t)min((int)sizeof(pkt.data), dataSize - offset);
        memcpy(pkt.data, buf + offset, pkt.size);
        packets.push_back(pkt);
        offset += pkt.size;
    }
}

// --- Upload state ---

static vector<string> g_UploadCommands[2]; // [pad]
static mutex g_UploadMutex;

} // anonymous namespace

// --- Public API ---

SMX_API bool SMX_LightsUpload_PrepareUpload(const char *gif, int size, int pad, SMX_LightsType type, const char **error)
{
    static const char *sNoError = "";
    if(!error) { static const char *dummy; error = &dummy; }
    *error = sNoError;

    if(!gif || size <= 0) { *error = "No GIF data provided."; return false; }
    if(pad < 0 || pad > 1) { *error = "Invalid pad index."; return false; }
    if(type != SMX_LightsType_Released && type != SMX_LightsType_Pressed)
    { *error = "Invalid animation type."; return false; }

    // Decode GIF.
    vector<DecodedFrame> frames;
    if(!DecodeGif(gif, size, frames))
    { *error = "The GIF couldn't be read."; return false; }

    // Only 23×24 (25-LED) is supported for firmware upload.
    if(frames[0].width != 23 || frames[0].height != 24)
    { *error = "Upload GIFs must be 23x24."; return false; }

    // Max 32 frames per animation type.
    if((int)frames.size() > 32)
    { *error = "The animation has too many frames (max 32)."; return false; }

    // Extract per-panel frames and detect loop frame.
    int loopFrame = 0;
    vector<PanelFrame> panelFrames[9];
    vector<float> durations;

    for(int f = 0; f < (int)frames.size(); f++)
    {
        // Check loop marker.
        int markerIdx = (23 * 23 + 0) * 4; // bottom-left pixel
        if(frames[f].rgba[markerIdx + 3] == 255 && frames[f].rgba[markerIdx + 0] >= 128)
            if(loopFrame == 0) loopFrame = f;

        durations.push_back(frames[f].duration);
        for(int panel = 0; panel < 9; panel++)
            panelFrames[panel].push_back(ExtractPanel(frames[f], panel));
    }

    // Build palette and pack graphics for each panel.
    fw_panel_animation_data_t allPanelData[9];
    memset(allPanelData, 0xFF, sizeof(allPanelData));

    int firstGraphic = (type == SMX_LightsType_Released) ? 0 : 32;

    for(int panel = 0; panel < 9; panel++)
    {
        fw_palette_t &palette = allPanelData[panel].palettes[type];
        if(!BuildPalette(panelFrames[panel], palette))
        {
            static char sErrBuf[128];
            snprintf(sErrBuf, sizeof(sErrBuf), "Panel %d uses too many colors (max 15).", panel);
            *error = sErrBuf;
            return false;
        }

        for(int f = 0; f < (int)panelFrames[panel].size(); f++)
            PackGraphic(panelFrames[panel][f], palette, allPanelData[panel].graphics[firstGraphic + f]);

        // Apply color scaling to palette (same as SetLights).
        for(auto &color : palette.colors)
            for(int c = 0; c < 3; c++)
                color.rgb[c] = uint8_t(color.rgb[c] * LED_COLOR_SCALE);
    }

    // Build master timing data.
    fw_animation_timing_t masterTiming;
    memset(&masterTiming, 0xFF, sizeof(masterTiming));
    masterTiming.loop_animation_frame = (uint8_t)loopFrame;
    for(int f = 0; f < (int)frames.size(); f++)
        masterTiming.frames[f] = firstGraphic + f;
    vector<uint8_t> delays = ComputeFrameDelays(durations);
    memset(masterTiming.delay, 0, sizeof(masterTiming.delay));
    for(int f = 0; f < (int)delays.size(); f++)
        masterTiming.delay[f] = delays[f];

    // --- Generate upload command sequence ---

    vector<string> commands;

    auto addUploadCmd = [&](const fw_upload_packet &pkt) {
        commands.emplace_back((const char*)&pkt, sizeof(pkt));
    };
    auto addDelay = [&](int ms) {
        fw_delay_packet pkt;
        pkt.milliseconds = (uint16_t)ms;
        commands.emplace_back((const char*)&pkt, sizeof(pkt));
    };

    // Create per-panel upload packets.
    vector<fw_upload_packet> packetsPerPanel[9];
    for(int panel = 0; panel < 9; panel++)
    {
        // Upload the 32 graphics for this type.
        const fw_graphic_t *graphics = &allPanelData[panel].graphics[firstGraphic];
        int graphicsOffset = (int)(firstGraphic * sizeof(fw_graphic_t));
        CreateUploadPackets(packetsPerPanel[panel], graphics, graphicsOffset,
            sizeof(fw_graphic_t) * 32, panel, type);

        // Upload the palette for this type.
        const fw_palette_t *palette = &allPanelData[panel].palettes[type];
        int paletteOffset = (int)(sizeof(fw_graphic_t) * 64 + type * sizeof(fw_palette_t));
        CreateUploadPackets(packetsPerPanel[panel], palette, paletteOffset,
            sizeof(fw_palette_t), panel, type);
    }

    // Interleave packets across panels with EEPROM write delays.
    while(true)
    {
        bool addedAny = false;
        int maxSize = 0;
        for(int panel = 0; panel < 9; panel++)
        {
            if(packetsPerPanel[panel].empty())
                continue;
            fw_upload_packet pkt = packetsPerPanel[panel].back();
            packetsPerPanel[panel].pop_back();
            addUploadCmd(pkt);
            maxSize = max(maxSize, (int)pkt.size);
            addedAny = true;
        }
        if(!addedAny) break;
        addDelay((int)(maxSize * 3.4f + 0.5f));
    }

    // Send panel data twice for reliability.
    size_t panelDataEnd = commands.size();
    commands.insert(commands.end(), commands.begin(), commands.begin() + panelDataEnd);

    // Append master timing data with final_packet flag on last packet.
    vector<fw_upload_packet> masterPackets;
    CreateUploadPackets(masterPackets, &masterTiming, 0, sizeof(masterTiming), 0xFF, type);
    masterPackets.back().final_packet = 1;
    for(const auto &pkt : masterPackets)
        addUploadCmd(pkt);

    // Append to existing commands for this pad (allows preparing both released
    // and pressed before a single BeginUpload call).
    lock_guard<mutex> lock(g_UploadMutex);
    g_UploadCommands[pad].insert(g_UploadCommands[pad].end(), commands.begin(), commands.end());

    return true;
}

// Declared in SMX.cpp
void SMX_SendCommandForPad(int pad, const std::string &cmd, std::function<void(std::string)> pComplete);

SMX_API void SMX_LightsUpload_BeginUpload(int pad, SMX_LightsUploadCallback callback, void *pUser)
{
    if(pad < 0 || pad > 1) return;
    if(!callback) return;

    vector<string> commands;
    {
        lock_guard<mutex> lock(g_UploadMutex);
        commands = std::move(g_UploadCommands[pad]);
        g_UploadCommands[pad].clear();
    }

    if(commands.empty())
    {
        callback(100, pUser);
        return;
    }

    int iTotalCommands = (int)commands.size();

    // Use a shared counter to track progress across async callbacks.
    auto pCompleted = make_shared<atomic<int>>(0);

    for(int i = 0; i < iTotalCommands; i++)
    {
        SMX_SendCommandForPad(pad, commands[i],
            [i, iTotalCommands, pCompleted, callback, pUser](string) {
                int iDone = pCompleted->fetch_add(1) + 1;
                int progress;
                if(iDone >= iTotalCommands)
                    progress = 100;
                else
                    progress = min((iDone * 100) / iTotalCommands, 99);
                callback(progress, pUser);
            });
    }
}
