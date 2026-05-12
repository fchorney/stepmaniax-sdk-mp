#include <doctest/doctest.h>
#include "SMX.h"

#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace std;

// --- Minimal GIF generation helpers ---
// These create valid GIF89a files in memory for testing.

// Append a byte to a buffer.
static void PushByte(vector<uint8_t> &buf, uint8_t b) { buf.push_back(b); }
static void PushLE16(vector<uint8_t> &buf, uint16_t v)
{
    buf.push_back(v & 0xFF);
    buf.push_back((v >> 8) & 0xFF);
}

// Create a minimal single-frame GIF with a solid color.
// width×height, all pixels set to the given RGB color.
static vector<uint8_t> MakeSolidGif(int width, int height, uint8_t r, uint8_t g, uint8_t b)
{
    vector<uint8_t> gif;

    // Header
    const char *hdr = "GIF89a";
    gif.insert(gif.end(), hdr, hdr + 6);

    // Logical Screen Descriptor
    PushLE16(gif, width);
    PushLE16(gif, height);
    PushByte(gif, 0x80); // GCT flag, 1 bit color resolution, 2 colors
    PushByte(gif, 0);    // background color index
    PushByte(gif, 0);    // pixel aspect ratio

    // Global Color Table (2 colors: index 0 = our color, index 1 = black)
    PushByte(gif, r); PushByte(gif, g); PushByte(gif, b);
    PushByte(gif, 0); PushByte(gif, 0); PushByte(gif, 0);

    // Image Descriptor
    PushByte(gif, 0x2C); // Image separator
    PushLE16(gif, 0);    // left
    PushLE16(gif, 0);    // top
    PushLE16(gif, width);
    PushLE16(gif, height);
    PushByte(gif, 0);    // no local color table, not interlaced

    // Image Data (LZW minimum code size = 2)
    PushByte(gif, 2); // LZW min code size

    // LZW compressed data: clear code, then all index-0 pixels, then end code.
    // For a 2-bit LZW with all-zero pixels, we can use a simple encoding.
    // Clear=4, End=5, data codes are 0.
    // We'll encode in sub-blocks.
    int totalPixels = width * height;

    // Simple approach: emit clear code, then raw pixel values, then end code.
    // With min code size 2: clear=4, end=5, initial code size=3 bits.
    // Each pixel 0 takes 3 bits. Pack into bytes.
    vector<uint8_t> lzwData;
    int bits = 0;
    int bitCount = 0;
    int codeSize = 3;

    auto emitCode = [&](int code) {
        bits |= (code << bitCount);
        bitCount += codeSize;
        while(bitCount >= 8)
        {
            lzwData.push_back(bits & 0xFF);
            bits >>= 8;
            bitCount -= 8;
        }
    };

    emitCode(4); // clear
    for(int i = 0; i < totalPixels; i++)
        emitCode(0); // pixel index 0
    emitCode(5); // end
    if(bitCount > 0)
        lzwData.push_back(bits & 0xFF);

    // Write as sub-blocks (max 255 bytes each)
    for(size_t i = 0; i < lzwData.size(); )
    {
        size_t blockSize = min((size_t)255, lzwData.size() - i);
        PushByte(gif, (uint8_t)blockSize);
        gif.insert(gif.end(), lzwData.begin() + i, lzwData.begin() + i + blockSize);
        i += blockSize;
    }
    PushByte(gif, 0); // block terminator

    // Trailer
    PushByte(gif, 0x3B);

    return gif;
}

// Create a 2-frame animated GIF (for testing animation).
static vector<uint8_t> MakeAnimatedGif(int width, int height,
    uint8_t r1, uint8_t g1, uint8_t b1,
    uint8_t r2, uint8_t g2, uint8_t b2,
    int delayCs = 3) // delay in centiseconds (3 = 30ms)
{
    vector<uint8_t> gif;

    // Header
    const char *hdr = "GIF89a";
    gif.insert(gif.end(), hdr, hdr + 6);

    // Logical Screen Descriptor
    PushLE16(gif, width);
    PushLE16(gif, height);
    PushByte(gif, 0x80); // GCT flag, 2 colors
    PushByte(gif, 0);
    PushByte(gif, 0);

    // Global Color Table (2 colors)
    PushByte(gif, r1); PushByte(gif, g1); PushByte(gif, b1);
    PushByte(gif, r2); PushByte(gif, g2); PushByte(gif, b2);

    int totalPixels = width * height;

    auto addFrame = [&](int colorIdx, int delay) {
        // Graphic Control Extension
        PushByte(gif, 0x21); // extension
        PushByte(gif, 0xF9); // GCE
        PushByte(gif, 4);    // block size
        PushByte(gif, 0);    // no transparency, disposal=none
        PushLE16(gif, delay); // delay in centiseconds
        PushByte(gif, 0);    // transparent color (unused)
        PushByte(gif, 0);    // block terminator

        // Image Descriptor
        PushByte(gif, 0x2C);
        PushLE16(gif, 0); PushLE16(gif, 0);
        PushLE16(gif, width); PushLE16(gif, height);
        PushByte(gif, 0);

        // LZW data
        PushByte(gif, 2); // min code size

        vector<uint8_t> lzwData;
        int bits = 0, bitCount = 0, codeSize = 3;
        auto emitCode = [&](int code) {
            bits |= (code << bitCount);
            bitCount += codeSize;
            while(bitCount >= 8) { lzwData.push_back(bits & 0xFF); bits >>= 8; bitCount -= 8; }
        };
        emitCode(4); // clear
        for(int i = 0; i < totalPixels; i++) emitCode(colorIdx);
        emitCode(5); // end
        if(bitCount > 0) lzwData.push_back(bits & 0xFF);

        for(size_t i = 0; i < lzwData.size(); ) {
            size_t bs = min((size_t)255, lzwData.size() - i);
            PushByte(gif, (uint8_t)bs);
            gif.insert(gif.end(), lzwData.begin() + i, lzwData.begin() + i + bs);
            i += bs;
        }
        PushByte(gif, 0);
    };

    // NETSCAPE extension for looping
    PushByte(gif, 0x21); PushByte(gif, 0xFF);
    PushByte(gif, 11);
    const char *ns = "NETSCAPE2.0";
    gif.insert(gif.end(), ns, ns + 11);
    PushByte(gif, 3); PushByte(gif, 1); PushLE16(gif, 0); // loop forever
    PushByte(gif, 0);

    addFrame(0, delayCs); // frame 1: color 1
    addFrame(1, delayCs); // frame 2: color 2

    PushByte(gif, 0x3B); // trailer
    return gif;
}

// =========================================================================
// SMX_LightsAnimation_Load tests
// =========================================================================

TEST_CASE("SMX_LightsAnimation_Load rejects null data") {
    const char *error = nullptr;
    CHECK_FALSE(SMX_LightsAnimation_Load(nullptr, 0, 0, SMX_LightsType_Released, &error));
    CHECK(error != nullptr);
    CHECK(string(error).find("No GIF") != string::npos);
}

TEST_CASE("SMX_LightsAnimation_Load rejects invalid pad") {
    auto gif = MakeSolidGif(14, 15, 255, 0, 0);
    const char *error = nullptr;
    CHECK_FALSE(SMX_LightsAnimation_Load((const char*)gif.data(), gif.size(), 2, SMX_LightsType_Released, &error));
    CHECK(string(error).find("Invalid pad") != string::npos);
}

TEST_CASE("SMX_LightsAnimation_Load rejects wrong dimensions") {
    auto gif = MakeSolidGif(10, 10, 255, 0, 0);
    const char *error = nullptr;
    CHECK_FALSE(SMX_LightsAnimation_Load((const char*)gif.data(), gif.size(), 0, SMX_LightsType_Released, &error));
    CHECK(string(error).find("14x15 or 23x24") != string::npos);
}

TEST_CASE("SMX_LightsAnimation_Load accepts 14x15 GIF") {
    auto gif = MakeSolidGif(14, 15, 255, 0, 0);
    const char *error = nullptr;
    CHECK(SMX_LightsAnimation_Load((const char*)gif.data(), gif.size(), 0, SMX_LightsType_Released, &error));
}

TEST_CASE("SMX_LightsAnimation_Load accepts 23x24 GIF") {
    auto gif = MakeSolidGif(23, 24, 0, 255, 0);
    const char *error = nullptr;
    CHECK(SMX_LightsAnimation_Load((const char*)gif.data(), gif.size(), 0, SMX_LightsType_Released, &error));
}

TEST_CASE("SMX_LightsAnimation_Load accepts animated GIF") {
    auto gif = MakeAnimatedGif(14, 15, 255, 0, 0, 0, 0, 255, 3);
    const char *error = nullptr;
    CHECK(SMX_LightsAnimation_Load((const char*)gif.data(), gif.size(), 0, SMX_LightsType_Released, &error));
}

TEST_CASE("SMX_LightsAnimation_Load accepts pressed animation") {
    auto gif = MakeSolidGif(14, 15, 0, 0, 255);
    const char *error = nullptr;
    CHECK(SMX_LightsAnimation_Load((const char*)gif.data(), gif.size(), 1, SMX_LightsType_Pressed, &error));
}

TEST_CASE("SMX_LightsAnimation_Load rejects corrupt data") {
    const char garbage[] = "not a gif at all";
    const char *error = nullptr;
    CHECK_FALSE(SMX_LightsAnimation_Load(garbage, sizeof(garbage), 0, SMX_LightsType_Released, &error));
    CHECK(string(error).find("couldn't be read") != string::npos);
}

// =========================================================================
// SMX_LightsAnimation_SetAuto tests
// =========================================================================

TEST_CASE("SMX_LightsAnimation_SetAuto can be enabled and disabled") {
    // Load an animation first
    auto gif = MakeSolidGif(14, 15, 100, 100, 100);
    const char *error = nullptr;
    REQUIRE(SMX_LightsAnimation_Load((const char*)gif.data(), gif.size(), 0, SMX_LightsType_Released, &error));

    // Enable — should not crash
    SMX_LightsAnimation_SetAuto(true);
    this_thread::sleep_for(chrono::milliseconds(100));

    // Disable — should not crash
    SMX_LightsAnimation_SetAuto(false);
}

TEST_CASE("SMX_LightsAnimation_SetAuto double enable is safe") {
    SMX_LightsAnimation_SetAuto(true);
    SMX_LightsAnimation_SetAuto(true); // should be no-op
    this_thread::sleep_for(chrono::milliseconds(50));
    SMX_LightsAnimation_SetAuto(false);
}

TEST_CASE("SMX_LightsAnimation_SetAuto double disable is safe") {
    SMX_LightsAnimation_SetAuto(false);
    SMX_LightsAnimation_SetAuto(false); // should be no-op
}

// =========================================================================
// SMX_LightsUpload_PrepareUpload tests
// =========================================================================

TEST_CASE("SMX_LightsUpload_PrepareUpload rejects null data") {
    const char *error = nullptr;
    CHECK_FALSE(SMX_LightsUpload_PrepareUpload(nullptr, 0, 0, SMX_LightsType_Released, &error));
    CHECK(string(error).find("No GIF") != string::npos);
}

TEST_CASE("SMX_LightsUpload_PrepareUpload rejects 14x15 GIF") {
    auto gif = MakeSolidGif(14, 15, 255, 0, 0);
    const char *error = nullptr;
    CHECK_FALSE(SMX_LightsUpload_PrepareUpload((const char*)gif.data(), gif.size(), 0, SMX_LightsType_Released, &error));
    CHECK(string(error).find("23x24") != string::npos);
}

TEST_CASE("SMX_LightsUpload_PrepareUpload accepts 23x24 GIF") {
    auto gif = MakeSolidGif(23, 24, 255, 0, 0);
    const char *error = nullptr;
    CHECK(SMX_LightsUpload_PrepareUpload((const char*)gif.data(), gif.size(), 0, SMX_LightsType_Released, &error));
}

TEST_CASE("SMX_LightsUpload_PrepareUpload accepts pressed type") {
    auto gif = MakeSolidGif(23, 24, 0, 255, 0);
    const char *error = nullptr;
    CHECK(SMX_LightsUpload_PrepareUpload((const char*)gif.data(), gif.size(), 1, SMX_LightsType_Pressed, &error));
}

TEST_CASE("SMX_LightsUpload_PrepareUpload rejects invalid pad") {
    auto gif = MakeSolidGif(23, 24, 255, 0, 0);
    const char *error = nullptr;
    CHECK_FALSE(SMX_LightsUpload_PrepareUpload((const char*)gif.data(), gif.size(), 2, SMX_LightsType_Released, &error));
}

TEST_CASE("SMX_LightsUpload_BeginUpload calls callback with 100 when no data prepared") {
    int progress = -1;
    SMX_LightsUpload_BeginUpload(0, [](int p, void *pUser) {
        *static_cast<int*>(pUser) = p;
    }, &progress);
    // With no SDK running and no prepared data, should immediately call with 100
    CHECK(progress == 100);
}
