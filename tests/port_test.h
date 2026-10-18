// The checks the standalone port tests (tests/*.cpp) share: one failure counter per test
// executable and a few macros that report where a check failed and what it checked.
//
//   CHECK(cond)              counts and reports a failure, then carries on
//   CHECK_MSG(cond, what)    the same, reporting `what` (a C string or std::string) as well
//   CHECK_EQ(a, b)           CHECK(a == b) that also prints both values when it can
//   REQUIRE(cond)            reports and aborts: for checks the rest of the test depends on
//   REQUIRE_MSG(cond, what)  the same, with a description
//   PORT_TEST_RESULT()       the exit code for main: 0 when nothing failed, else 1
//
// A failure prints "file:line: FAIL: <expression>" (plus " - <what>" when given) to stderr.
// Header-only and dependency-free, and C++17 so the exact-IEEE tests built at that standard
// can use it. REQUIRE does not depend on NDEBUG, unlike assert().
#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <type_traits>

namespace port_test {

inline int& FailureCount() {
  static int count = 0;
  return count;
}

inline int Failures() { return FailureCount(); }

// For a test that reports a failure in its own words (a diff, a table) and only needs it
// counted.
inline void CountFailure() { ++FailureCount(); }

// __FILE__ is often an absolute path; the file name is enough to find the check.
inline const char* BaseName(const char* path) {
  const char* name = path;
  for (const char* p = path; *p != '\0'; ++p) {
    if (*p == '/' || *p == '\\') {
      name = p + 1;
    }
  }
  return name;
}

inline void Report(const char* file, int line, const char* expr, const char* what) {
  // Keep the report in order with whatever the test printed to stdout before it.
  std::fflush(stdout);
  if (what != nullptr && *what != '\0') {
    std::fprintf(stderr, "%s:%d: FAIL: %s - %s\n", BaseName(file), line, expr, what);
  } else {
    std::fprintf(stderr, "%s:%d: FAIL: %s\n", BaseName(file), line, expr);
  }
  std::fflush(stderr);
}

inline bool Check(bool ok, const char* file, int line, const char* expr,
                  const char* what = nullptr) {
  if (!ok) {
    Report(file, line, expr, what);
    ++FailureCount();
  }
  return ok;
}

inline bool Check(bool ok, const char* file, int line, const char* expr, const std::string& what) {
  return Check(ok, file, line, expr, what.c_str());
}

inline void Require(bool ok, const char* file, int line, const char* expr,
                    const char* what = nullptr) {
  if (!ok) {
    Report(file, line, expr, what);
    std::abort();
  }
}

inline void Require(bool ok, const char* file, int line, const char* expr,
                    const std::string& what) {
  Require(ok, file, line, expr, what.c_str());
}

// A printable form of a CHECK_EQ operand: numbers, enums, bools and strings; anything else
// prints as "?" (the expression text still says what was compared).
template <typename T>
std::string Describe(const T& value) {
  using U = std::decay_t<T>;
  char buf[64];
  if constexpr (std::is_same_v<U, bool>) {
    return value ? "true" : "false";
  } else if constexpr (std::is_enum_v<U>) {
    return Describe(static_cast<std::underlying_type_t<U>>(value));
  } else if constexpr (std::is_floating_point_v<U>) {
    std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(value));
    return buf;
  } else if constexpr (std::is_integral_v<U> && std::is_signed_v<U>) {
    std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(value));
    return buf;
  } else if constexpr (std::is_integral_v<U>) {
    std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(value));
    return buf;
  } else if constexpr (std::is_convertible_v<const T&, std::string_view>) {
    return "\"" + std::string(std::string_view(value)) + "\"";
  } else {
    return "?";
  }
}

template <typename A, typename B>
bool CheckEq(const A& a, const B& b, const char* file, int line, const char* expr) {
  const bool ok = a == b;
  if (!ok) {
    const std::string values = Describe(a) + " vs " + Describe(b);
    Check(false, file, line, expr, values);
  }
  return ok;
}

// Prints a summary when something failed and returns main's exit code.
inline int Finish() {
  if (FailureCount() != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", FailureCount());
    return 1;
  }
  return 0;
}

} // namespace port_test

#define CHECK(cond) ::port_test::Check(static_cast<bool>(cond), __FILE__, __LINE__, #cond)
#define CHECK_MSG(cond, what)                                                                      \
  ::port_test::Check(static_cast<bool>(cond), __FILE__, __LINE__, #cond, (what))
#define CHECK_EQ(a, b) ::port_test::CheckEq((a), (b), __FILE__, __LINE__, #a " == " #b)
#define REQUIRE(cond) ::port_test::Require(static_cast<bool>(cond), __FILE__, __LINE__, #cond)
#define REQUIRE_MSG(cond, what)                                                                    \
  ::port_test::Require(static_cast<bool>(cond), __FILE__, __LINE__, #cond, (what))
#define PORT_TEST_RESULT() ::port_test::Finish()
