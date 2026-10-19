#include "vr/vr_smooth_turn.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {
void Check(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "VR smooth turn regression: %s\n", message);
        std::abort();
    }
}
bool Near(float a, float b) { return std::abs(a - b) < 0.0001f; }
} // namespace

int main() {
    using PortVr::SmoothTurn::Mix;
    for (float direction : {-1.0f, 1.0f}) {
        for (float sensitivity : {0.2f, 0.5f, 1.0f, 1.5f, 2.0f, 3.0f}) {
            const auto out = Mix(0.0f, direction, sensitivity);
            Check(Near(out.stick * out.speed_scale, direction * sensitivity),
                  "full look deflection retains sensitivity beyond stick saturation");
            Check(std::abs(out.stick) <= 1.0f, "virtual stick stays in range");
        }
    }
    const auto partial = Mix(0.0f, 0.5f, 3.0f);
    Check(Near(partial.stick, 1.0f) && Near(partial.speed_scale, 1.5f), "partial look can exceed native rate");
    const auto movement = Mix(0.75f, 0.0f, 3.0f);
    Check(Near(movement.stick, 0.75f) && Near(movement.speed_scale, 1.0f), "move stick alone is unchanged");
    const auto opposed = Mix(1.0f, -1.0f, 3.0f);
    Check(Near(opposed.stick, -1.0f) && Near(opposed.speed_scale, 2.0f), "opposing sticks retain the requested direction");
    const auto normal = Mix(1.0f, 1.0f, 1.0f);
    Check(Near(normal.stick, 1.0f) && Near(normal.speed_scale, 1.0f), "default combined input retains native limit");
    const auto centre = Mix(0.0f, 0.0f, 3.0f);
    Check(centre.stick == 0.0f && centre.speed_scale == 1.0f, "centred sticks clear the speed multiplier");
    return 0;
}
