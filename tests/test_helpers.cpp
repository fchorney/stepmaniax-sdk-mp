#include <doctest/doctest.h>

#include <cstdint>
#include <string>

// These are defined in SMX.cpp in the SMX namespace
namespace SMX {
    std::string ssprintf(const char *fmt, ...);
    std::string BinaryToHex(const void *pData, int iNumBytes);
    std::string BinaryToHex(const std::string &sString);
}

using namespace std;

TEST_CASE("BinaryToHex converts bytes to lowercase hex") {
    uint8_t data[] = {0x00, 0x0A, 0xFF, 0x42};
    CHECK(SMX::BinaryToHex(data, 4) == "000aff42");
}

TEST_CASE("BinaryToHex with empty data returns empty string") {
    CHECK(SMX::BinaryToHex(nullptr, 0).empty());
}

TEST_CASE("BinaryToHex string overload") {
    string s("\x01\x02\x03", 3);
    CHECK(SMX::BinaryToHex(s) == "010203");
}

TEST_CASE("ssprintf formats integers") {
    CHECK(SMX::ssprintf("%d", 42) == "42");
    CHECK(SMX::ssprintf("%05d", 7) == "00007");
}

TEST_CASE("ssprintf formats strings") {
    CHECK(SMX::ssprintf("hello %s", "world") == "hello world");
}

TEST_CASE("ssprintf formats hex") {
    CHECK(SMX::ssprintf("%02x", 255) == "ff");
    CHECK(SMX::ssprintf("%02x", 0) == "00");
}

TEST_CASE("ssprintf handles empty format") {
    CHECK(SMX::ssprintf("") == "");
}

TEST_CASE("ssprintf handles multiple args") {
    CHECK(SMX::ssprintf("%d %s %x", 10, "hi", 0xAB) == "10 hi ab");
}

TEST_CASE("ssprintf handles strings longer than 256 chars") {
    string padding(300, 'x');
    string result = SMX::ssprintf("prefix_%s_suffix", padding.c_str());
    CHECK(result.size() == 7 + 300 + 7);  // "prefix_" + 300x + "_suffix"
    CHECK(result.substr(0, 7) == "prefix_");
    CHECK(result.substr(result.size() - 7) == "_suffix");
    CHECK(result.substr(7, 300) == padding);
}
