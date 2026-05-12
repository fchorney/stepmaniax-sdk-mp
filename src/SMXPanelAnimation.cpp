// SMXPanelAnimation: GIF-based panel animation loading and playback.
//
// This implements SMX_LightsAnimation_Load and SMX_LightsAnimation_SetAuto.
// Animations are decoded from GIF files and played back at 30 FPS by calling
// SMX_SetLights2 internally.

#include "SMX.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "vendor/gif_load.h"

using namespace std;

// Declared in SMX.cpp
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
thread g_AnimThread;
atomic<bool> g_bAnimShutdown{false};
double g_fStopAnimatingUntil = 0;

// --- Animation Thread ---

void AnimationThreadMain()
{
    const int iFrameMs = 33; // ~30 FPS

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
        {
            lock_guard<mutex> lock(g_AnimMutex);

            // Determine LED count from loaded animations.
            for(int pad = 0; pad < 2; pad++)
                for(int type = 0; type < 2; type++)
                    if(!g_Animations[pad][type].durations.empty())
                        iLedsPerPanel = g_Animations[pad][type].numLeds;
        }

        const int iBytesPerPanel = iLedsPerPanel * 3;
        const int iBytesPerPad = 9 * iBytesPerPanel;
        vector<char> lightData(2 * iBytesPerPad, 0);

        {
            lock_guard<mutex> lock(g_AnimMutex);

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

        // Send lights.
        int totalSize = iLedsPerPanel == 25 ? 1350 : 864;
        SMX_SetLights2(lightData.data(), totalSize);

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
    if(g_bAutoAnimating.load(memory_order_relaxed))
        g_fStopAnimatingUntil = SMX::GetMonotonicTime() + 0.1; // 100ms
}
