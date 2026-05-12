#include "SMXHelpers.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <random>
#include <string>

using namespace std;

namespace SMX {

static atomic<SMXLogCallback*> g_LogCallback{nullptr};

double GetMonotonicTime()
{
    static auto start = chrono::steady_clock::now();
    return chrono::duration<double>(chrono::steady_clock::now() - start).count();
}

void Log(const string &s)
{
    auto cb = g_LogCallback.load(memory_order_acquire);
    if(cb)
        cb(s.c_str());
    else
        printf("%6.3f: %s\n", GetMonotonicTime(), s.c_str());
}

void SetLogCallback(SMXLogCallback *callback)
{
    g_LogCallback.store(callback, memory_order_release);
}

string ssprintf(const char *fmt, ...)
{
    char buf[512];
    va_list va;
    va_start(va, fmt);
    const int n = vsnprintf(buf, sizeof(buf), fmt, va);
    va_end(va);
    if(n < 0) return string("Error formatting: ") + fmt;
    if(n < static_cast<int>(sizeof(buf)))
        return string(buf, n);

    string s(n, '\0');
    va_start(va, fmt);
    vsnprintf(&s[0], n + 1, fmt, va);
    va_end(va);
    return s;
}

string BinaryToHex(const void *pData, const int iNumBytes)
{
    static const char hex[] = "0123456789abcdef";
    const auto *p = static_cast<const unsigned char*>(pData);
    string s(iNumBytes * 2, '\0');
    for(int i = 0; i < iNumBytes; i++)
    {
        s[i*2]   = hex[p[i] >> 4];
        s[i*2+1] = hex[p[i] & 0x0F];
    }
    return s;
}

string BinaryToHex(const string &sString)
{
    return BinaryToHex(sString.data(), static_cast<int>(sString.size()));
}

void GenerateSerial(uint8_t *pOut)
{
    random_device rd;
    mt19937 gen(rd());
    uniform_int_distribution<int> dist(0, 255);
    for(int i = 0; i < SERIAL_SIZE; i++)
        pOut[i] = static_cast<uint8_t>(dist(gen));
}

} // namespace SMX
