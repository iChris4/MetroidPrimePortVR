// The `physics` components of the console's `status --json` (port_console_physics.h),
// exercising the production helper itself: a non-finite component must come out as
// JSON null, because `%.9g` would print nan/inf and no parser accepts that. Finite
// components keep the exact %.9g text a float round-trips through. Every string this
// produces is checked by parsing it with the port's own strict JSON reader (which
// itself rejects NaN/Infinity), not by comparing it to text written here: a broken
// number would parse as something else or not at all.
#include "port_console_physics.h"
#include "port_json.h"
#include "port_test.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

namespace {

bool Parse(const std::string& text, PortJson::Value& value) {
  size_t offset = 0;
  const char* reason = nullptr;
  return PortJson::Parse(text, value, offset, &reason);
}

// A finite value's text must be exactly what %.9g prints and must round-trip to the
// float through the parser.
void CheckFinite(float value, const char* what) {
  const std::string text = PortConsolePhysics::JsonNumber(value);
  char reference[32];
  std::snprintf(reference, sizeof(reference), "%.9g", static_cast< double >(value));
  CHECK_MSG(text == reference, what);
  CHECK_MSG(text.find("nan") == std::string::npos && text.find("inf") == std::string::npos,
            "finite text holds no nan/inf");
  char* end = nullptr;
  const float parsed = std::strtof(text.c_str(), &end);
  CHECK_MSG(end != nullptr && *end == '\0' && parsed == value, "the text parses back to the float");
  PortJson::Value json;
  // The reader holds a double, so the check narrows it back to the float: the text only
  // has to round-trip to the float, not to the decimal the float widens to.
  CHECK_MSG(Parse(text, json) && json.IsNumber() &&
                static_cast< float >(json.AsNumber()) == value,
            "the strict JSON reader accepts it as that number");
}

void CheckNull(float value, const char* what) {
  const std::string text = PortConsolePhysics::JsonNumber(value);
  CHECK_MSG(text == "null", what);
  PortJson::Value json;
  CHECK_MSG(Parse(text, json) && json.IsNull(), "null is JSON null to the strict reader");
}

} // namespace

int main() {
  using PortConsolePhysics::JsonNumber;
  using PortConsolePhysics::JsonVector;

  // Finite, ordinary game values: exactly %.9g, not rounded to %.3f.
  CheckFinite(0.f, "0");
  CheckFinite(-0.f, "-0");
  CheckFinite(1.f, "1");
  CheckFinite(-48.1011543f, "-48.1011543");
  CheckFinite(99.2702255f, "99.2702255");
  CheckFinite(5.83333302f, "5.83333302");
  CheckFinite(0.001f, "0.001");
  CheckFinite(1e-7f, "1e-07");
  // Extremes: the largest finite float, the smallest normal and a subnormal. All are
  // finite, so all must stay numbers a parser accepts; inf is the failure mode.
  CheckFinite(std::numeric_limits< float >::max(), "FLT_MAX stays a number");
  CheckFinite(-std::numeric_limits< float >::max(), "-FLT_MAX stays a number");
  CheckFinite(std::numeric_limits< float >::min(), "FLT_MIN stays a number");
  CheckFinite(std::numeric_limits< float >::denorm_min(), "a subnormal stays a number");
  CheckFinite(std::numeric_limits< float >::epsilon(), "FLT_EPSILON stays a number");

  // Non-finite: null, whatever the sign.
  CheckNull(std::numeric_limits< float >::quiet_NaN(), "quiet NaN is null");
  CheckNull(-std::numeric_limits< float >::quiet_NaN(), "-NaN is null");
  CheckNull(std::numeric_limits< float >::infinity(), "+Inf is null");
  CheckNull(-std::numeric_limits< float >::infinity(), "-Inf is null");

  // Vectors, including the mixed case the brief calls out: a valid position with a
  // velocity that went non-finite on one axis.
  PortJson::Value parsed;
  const std::string ground = JsonVector(99.2702255f, -48.1011543f, 5.04225063f);
  CHECK_MSG(ground == "[99.2702255,-48.1011543,5.04225063]", "finite vector text");
  CHECK_MSG(Parse(ground, parsed) && parsed.IsArray() && parsed.Size() == 3 &&
                parsed.AsArray()[0].IsNumber() &&
                static_cast< float >(parsed.AsArray()[0].AsNumber()) == 99.2702255f &&
                parsed.AsArray()[2].IsNumber(),
            "the finite vector parses as an array of 3 numbers");

  const std::string mixed = JsonVector(1.f, std::numeric_limits< float >::quiet_NaN(),
                                       std::numeric_limits< float >::infinity());
  CHECK_MSG(mixed == "[1,null,null]", "NaN and Inf components in a vector are null");
  CHECK_MSG(Parse(mixed, parsed) && parsed.IsArray() && parsed.Size() == 3 &&
                parsed.AsArray()[0].IsNumber() && parsed.AsArray()[1].IsNull() &&
                parsed.AsArray()[2].IsNull(),
            "a mixed vector parses with the null members intact");
  CHECK_MSG(Parse(JsonVector(std::numeric_limits< float >::quiet_NaN(),
                             -std::numeric_limits< float >::infinity(),
                             std::numeric_limits< float >::denorm_min()),
                  parsed) &&
                parsed.IsArray() && parsed.AsArray()[0].IsNull() && parsed.AsArray()[1].IsNull() &&
                parsed.AsArray()[2].IsNumber(),
            "only the non-finite components are null");

  // No fixed aggregate buffer: the longest %.9g text (FLT_MAX is 14 characters) comes
  // back whole, and a whole object with a null component stays valid JSON.
  CHECK_MSG(JsonNumber(std::numeric_limits< float >::max()).size() == std::strlen("3.40282347e+38"),
            "a long float is not cut off");
  const std::string object = "{\"pos\":" + ground + ",\"vel\":" + mixed + "}";
  const bool objectParses =
      Parse(object, parsed) && parsed.IsObject() && parsed.Size() == 2;
  const PortJson::Value* posMember = objectParses ? parsed.Find("pos") : nullptr;
  const PortJson::Value* velMember = objectParses ? parsed.Find("vel") : nullptr;
  CHECK_MSG(objectParses && posMember != nullptr && posMember->IsArray() &&
                velMember != nullptr && velMember->IsArray() &&
                velMember->AsArray()[1].IsNull(),
            "a physics-like object with a null component parses as JSON");

  std::printf(port_test::Failures() == 0 ? "[console-physics-tests] all checks passed\n"
                      : "[console-physics-tests] FAILURES\n");
  return PORT_TEST_RESULT();
}