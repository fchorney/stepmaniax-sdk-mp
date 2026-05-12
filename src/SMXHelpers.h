#ifndef SMXHelpers_h
#define SMXHelpers_h

#include <string>
#include <cstdint>

#include "SMX.h"

namespace SMX {

/// Returns the elapsed time in seconds since program start using a high-resolution
/// monotonic clock. Used for timing commands and logging timestamps.
/// @return Elapsed time in seconds as a double.
double GetMonotonicTime();

/// Logs a message with a timestamp prefix. If a custom log callback is set,
/// it will be used; otherwise, logs to stdout with the current monotonic time.
/// @param s The message to log.
void Log(const std::string &s);

/// Sets a custom callback function to handle all log messages from the SDK.
/// Thread-safe: can be called from any thread at any time.
/// @param callback Function that receives log strings. Pass nullptr to disable.
void SetLogCallback(SMXLogCallback *callback);

/// Formatted string printing using printf-style arguments. Returns a std::string
/// instead of printing directly, useful for building log messages and debug output.
/// @param fmt Printf-style format string.
/// @param ... Variable arguments to format.
/// @return The formatted string.
std::string ssprintf(const char *fmt, ...);

/// Converts binary data to a hexadecimal string representation.
/// Each byte is converted to two hex digits (lowercase).
/// @param pData Pointer to the binary data.
/// @param iNumBytes Number of bytes to convert.
/// @return Hexadecimal string representation of the binary data.
std::string BinaryToHex(const void *pData, int iNumBytes);

/// Converts a string's binary data to a hexadecimal string representation.
/// @param sString The string whose bytes to convert.
/// @return Hexadecimal string representation.
std::string BinaryToHex(const std::string &sString);

/// Generates a random serial number (16 random bytes).
/// Used to assign unique identifiers to devices that don't have a serial number.
/// @param pOut Pointer to a 16-byte buffer to receive the generated serial.
void GenerateSerial(uint8_t *pOut);

} // namespace SMX

#endif
