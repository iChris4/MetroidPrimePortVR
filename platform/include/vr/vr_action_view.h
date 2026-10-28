// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cmath>

namespace PortVr::ActionView {

// A lost combat target must release its simulation orbit too. Keeping the
// vanilla corpse/point orbit leaves walking in the old target-facing basis
// until the trigger is released. Intentional point locks and scans stay intact.
constexpr bool ReleaseLostTarget(bool immersive, bool scanVisor,
                                 bool wasTargetOrbit, bool becomesTargetless) noexcept {
    return immersive && !scanVisor && wasTargetOrbit && becomesTargetless;
}

// Only simulation/view-base yaw enters this state. Tracked head yaw is applied
// by stereo replay and must never steer an established orbit or swing.
struct Result {
    float yaw;
    bool released;
};

struct State {
    bool active = false;
    float offset = 0.f;
    float anchor = 0.f;
    unsigned target = 0;

    void Reset() noexcept { active = false; offset = anchor = 0.f; }

    Result Update(bool immersive, bool action, bool freeze, float simulationYaw,
                  float previousViewYaw, unsigned actionTarget = 0) noexcept {
        if (!immersive || !std::isfinite(simulationYaw) || !std::isfinite(previousViewYaw)) {
            Reset();
            return {simulationYaw, false};
        }
        if (!action) {
            const bool released = active;
            Reset();
            return {released ? previousViewYaw : simulationYaw, released};
        }
        constexpr float tau = 6.283185307179586f;
        if (!active || target != actionTarget) {
            active = true;
            target = actionTarget;
            anchor = previousViewYaw;
            offset = std::remainder(previousViewYaw - simulationYaw, tau);
        }
        return {freeze ? anchor : std::remainder(simulationYaw + offset, tau), false};
    }
};

} // namespace PortVr::ActionView
