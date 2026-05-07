#include <doctest/doctest.h>
#include <SMX.h>
#include <cstring>

TEST_CASE("SMX_Version returns a valid version string") {
    const char *version = SMX_Version();
    REQUIRE(version != nullptr);
    CHECK(std::strlen(version) > 0);
    // Should match the project version set in CMakeLists.txt
    CHECK(std::strcmp(version, "0.1.0") == 0);
}
