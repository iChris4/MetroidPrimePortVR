#ifndef METROID_PRIME_PORT_PORT_CONSOLE_PHYSICS_H
#define METROID_PRIME_PORT_PORT_CONSOLE_PHYSICS_H
#include "port_json_writer.h"

#include <string>

// JSON numbers for the `physics` object of `status --json` (issue #117 telemetry).
// The game runs with fast-math off for physics, but a component can still be
// non-finite: a velocity is NaN or infinite for a tick when a force blows up or a
// normalise divides by zero. `%.9g` would print `nan`/`inf`, which no JSON parser
// accepts, so a non-finite component is emitted as `null` and a finite one keeps
// the exact `%.9g` text a float round-trips through.
//
// Free of game types (the writer is in port_json.cpp) so the console's formatter
// can be tested without the game: tests/port_console_physics.cpp parses what this
// produces.
namespace PortConsolePhysics {

// One component: `%.9g` of a finite value, `null` otherwise; this is
// port::JsonWriter's Float rule, which these forward to.
inline std::string JsonNumber(float value) { return port::JsonWriter::FloatText(value); }

// `[x,y,z]` with each component through JsonNumber.
inline std::string JsonVector(float x, float y, float z) {
  port::JsonWriter json;
  json.BeginArray().Float(x).Float(y).Float(z).EndArray();
  return json.Take();
}

} // namespace PortConsolePhysics

#endif // METROID_PRIME_PORT_PORT_CONSOLE_PHYSICS_H
