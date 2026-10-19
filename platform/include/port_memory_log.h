#ifndef METROID_PRIME_PORT_PORT_MEMORY_LOG_H
#define METROID_PRIME_PORT_PORT_MEMORY_LOG_H
#include <cstddef>
#include <cstdint>

// Memory lines for the log (issue #102: an Android low-memory kill leaves no
// crash report, only a log that stops). Linux and Android only; elsewhere no-ops.
namespace PortMemoryLog {

// Once, after the log starts: how the previous run ended (Android 11+, from the
// system's exit records) and the memory the device has now.
void LogStartup();
// Each frame, on the main thread: samples once a second and writes a line every
// 30 s, or sooner when the process or the system's free memory moved a lot.
void Tick(uint64_t nowNs);
// Android only (false elsewhere): whether the device is short on RAM (under 4096 MiB in
// total, or ActivityManager.isLowRamDevice()); `why` says which. Issue #162.
bool LowRamDevice(char* why, size_t whySize);

} // namespace PortMemoryLog

#endif
