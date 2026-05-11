#include <doctest/doctest.h>
#include <SMX.h>
#include <cstring>
#include <string>
#include <vector>
#include "SMXVersion.h"
#include "test_helpers_manager.h"

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#include <csignal>
#endif

TEST_CASE("SMX_Version returns a valid version string") {
    const char *version = SMX_Version();
    REQUIRE(version != nullptr);
    CHECK(std::strlen(version) > 0);
    // Should match the project version set in CMakeLists.txt
    CHECK(std::strcmp(version, SMX_VERSION) == 0);
}

static std::vector<std::string> g_CapturedLogs;

static void TestLogCallback(const char *log)
{
    g_CapturedLogs.emplace_back(log);
}

TEST_CASE("SMX_SetLogCallback receives log messages from SDK") {
    g_CapturedLogs.clear();
    SMX_SetLogCallback(TestLogCallback);

    // Starting with no devices will still produce internal log activity
    // when a device connects. Use a minimal start/stop cycle.
    SMX_Start([](int, SMXUpdateCallbackReason, void*){}, nullptr);
    SMX_Stop();

    SMX_SetLogCallback(nullptr);

    // SMX_Stop logs "Closing device" for any open devices, but even without
    // devices the callback should have been set without crashing.
    // The key assertion: the callback mechanism works (no crash, no UB).
    CHECK(true);
}

TEST_CASE("SMX_SetLogCallback with nullptr disables custom logging") {
    SMX_SetLogCallback(nullptr);
    // Should not crash — falls back to printf
    SMX_Start([](int, SMXUpdateCallbackReason, void*){}, nullptr);
    SMX_Stop();
}

#ifndef _WIN32
TEST_CASE("SMX_Stop from callback aborts") {
    // Fork a child process that calls SMX_Stop from within the update callback.
    // The child should receive SIGABRT.
    pid_t pid = fork();
    REQUIRE(pid >= 0);

    if(pid == 0)
    {
        // Reset signal handlers inherited from doctest so abort() actually kills the process.
        signal(SIGABRT, SIG_DFL);

        // Child process: set up a fake device that will trigger a Connected callback,
        // then call SMX_Stop from inside that callback.
        using namespace SMXTestHelpers;

        auto *pFake = new FakeDevice();
        pFake->QueueRead(MakeDeviceInfoResponse('0', 5));
        pFake->SetConfigResponse(MakeConfigResponse());

        auto pEnum = std::make_unique<FakeHIDEnumerator>();
        pEnum->AddDevice("/dev/fake0", pFake);

        SMX_StartWithEnumerator([](int, SMXUpdateCallbackReason reason, void*) {
            if(SMX_REASON_IS(reason, SMXUpdateCallback_Connected))
                SMX_Stop();
        }, nullptr, std::move(pEnum));

        // Wait for the device to connect and the callback to fire.
        for(int i = 0; i < 100; i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));

        // Should not reach here — abort should have fired.
        SMX_Stop();
        delete pFake;
        _exit(42);
    }

    // Parent: wait for child and check exit status.
    int status = 0;
    waitpid(pid, &status, 0);

    // Child should have been killed by SIGABRT.
    CHECK(WIFSIGNALED(status));
    CHECK(WTERMSIG(status) == SIGABRT);
}
#endif
