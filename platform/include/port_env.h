#ifndef METROID_PRIME_PORT_PORT_ENV_H
#define METROID_PRIME_PORT_PORT_ENV_H
#include <cstdlib>
#include <initializer_list>

// One rule for the MP_* environment variables. A flag is off when unset (the
// caller's fallback), empty, or "0"/"false"/"off"/"no" in any case; every other
// value turns it on. Numbers fall back when unset, empty or not entirely a number.
namespace port {

// The raw value, or nullptr when unset (an empty value is returned as is).
inline const char* EnvString(const char* name) { return std::getenv(name); }

inline bool EnvFlag(const char* name, bool fallback = false) {
  const char* value = std::getenv(name);
  if (value == nullptr) {
    return fallback;
  }
  if (value[0] == '\0') {
    return false;
  }
  for (const char* off : {"0", "false", "off", "no"}) {
    const char* a = value;
    const char* b = off;
    while (*a != '\0' && *b != '\0' && ((*a | 0x20) == *b || *a == *b)) {
      ++a;
      ++b;
    }
    if (*a == '\0' && *b == '\0') {
      return false;
    }
  }
  return true;
}

inline int EnvInt(const char* name, int fallback) {
  const char* value = std::getenv(name);
  if (value == nullptr || value[0] == '\0') {
    return fallback;
  }
  char* end = nullptr;
  const long parsed = std::strtol(value, &end, 10);
  return end != value && *end == '\0' ? static_cast<int>(parsed) : fallback;
}

inline float EnvFloat(const char* name, float fallback) {
  const char* value = std::getenv(name);
  if (value == nullptr || value[0] == '\0') {
    return fallback;
  }
  char* end = nullptr;
  const float parsed = std::strtof(value, &end);
  return end != value && *end == '\0' ? parsed : fallback;
}

// The user's home folder, or nullptr when the environment does not name one:
// HOME, else USERPROFILE (Windows, where HOME is usually unset). An empty
// value counts as unset. Read by hand rather than through SDL, which would
// create its folders.
inline const char* HomeFolder() {
  for (const char* name : {"HOME", "USERPROFILE"}) {
    const char* value = std::getenv(name);
    if (value != nullptr && value[0] != '\0') {
      return value;
    }
  }
  return nullptr;
}

} // namespace port

#endif
