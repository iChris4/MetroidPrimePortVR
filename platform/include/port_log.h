#ifndef METROID_PRIME_PORT_PORT_LOG_H
#define METROID_PRIME_PORT_PORT_LOG_H
#include <cstdarg>
#include <cstdio>

#if defined(__ANDROID__)
#include <android/log.h>

#include "port_log_file.h"
#endif

// The port's own diagnostics: build id, chosen disc, randomizer and
// Archipelago messages. They used to be plain fprintf(stderr), which is right
// on a desktop and useless on Android, where nothing captures the process's
// stderr - an app that exits during startup leaves no trace of why, and there is
// no terminal to read. On Android these go to logcat instead, under the
// "metroidprime" tag; everywhere else they stay on stderr exactly as before.
namespace PortLog {

inline void WriteV(const char* format, va_list arguments) {
  char buffer[2048];
  const int written = std::vsnprintf(buffer, sizeof(buffer), format, arguments);
  if (written <= 0)
    return;
#if defined(__ANDROID__)
  __android_log_write(ANDROID_LOG_INFO, "metroidprime", buffer);
  PortLogFile::Write("metroidprime", buffer);
#else
  std::fputs(buffer, stderr);
#endif
}

inline void Write(const char* format, ...) {
  va_list arguments;
  va_start(arguments, format);
  WriteV(format, arguments);
  va_end(arguments);
}

} // namespace PortLog

#endif // METROID_PRIME_PORT_PORT_LOG_H
