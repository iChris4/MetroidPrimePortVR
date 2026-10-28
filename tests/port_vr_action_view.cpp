// SPDX-License-Identifier: GPL-3.0-or-later
#include "vr/vr_action_view.h"

#include <cstdio>
#include <cstdlib>
#include <initializer_list>

namespace {
void Check(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "VR action view regression: %s\n", message);
        std::abort();
    }
}
bool Near(float a, float b) { return std::fabs(std::remainder(a - b, 6.28318530718f)) < .0001f; }
}

int main() {
    using PortVr::ActionView::State;
    using PortVr::ActionView::ReleaseLostTarget;
    Check(ReleaseLostTarget(true, false, true, true), "combat corpse/point transition releases lock");
    Check(!ReleaseLostTarget(false, false, true, true), "desktop corpse orbit is unchanged");
    Check(!ReleaseLostTarget(true, true, true, true), "scan orbit is unchanged");
    Check(!ReleaseLostTarget(true, false, false, true), "intentional empty point lock is unchanged");
    Check(!ReleaseLostTarget(true, false, true, false), "valid target and grapple transitions stay active");
    for (float targetYaw : {1.5707963f, -1.5707963f, 3.1415926f}) {
        State lost;
        lost.Update(true, true, false, targetYaw, .2f, 42);
        const bool action = !ReleaseLostTarget(true, false, true, true);
        const auto released = lost.Update(true, action, false, targetYaw, .2f);
        Check(released.released && Near(released.yaw, .2f),
              "target death while held restores view basis for side/rear locks");
        Check(!lost.Update(true, action, false, .2f, .2f).released,
              "holding lock after deletion does not repeatedly rebase movement");
    }
    constexpr float pi = 3.14159265359f;
    State view;
    Check(Near(view.Update(true, false, false, .3f, 0.f).yaw, .3f), "free view is unchanged");
    auto frame = view.Update(true, true, false, pi, 0.f);
    Check(Near(frame.yaw, 0.f), "locking behind does not turn the view base");
    Check(Near(frame.yaw + pi, pi), "stereo applies the behind-facing HMD yaw exactly once");
    frame = view.Update(true, true, false, pi - .2f, 0.f);
    Check(Near(frame.yaw, -.2f), "later target motion follows without the acquisition snap");
    const float orbitYaw = pi - .2f;
    const float swingYaw = pi - .2f;
    for (float headYaw : {0.f, pi / 2.f, -pi / 2.f, pi}) {
        frame = view.Update(true, true, false, orbitYaw, -.2f);
        Check(Near(frame.yaw, -.2f), "head turns do not rotate the orbit view base");
        Check(Near(frame.yaw + headYaw, headYaw - .2f), "head turns only change the displayed direction");
        frame = view.Update(true, true, false, swingYaw, -.2f);
        Check(Near(frame.yaw, -.2f), "changing from orbit to grapple retains the same view basis");
    }
    frame = view.Update(true, false, false, orbitYaw, -.15f);
    Check(frame.released && Near(frame.yaw, -.15f), "release preserves the actual smoothed view");
    Check(!view.Update(true, false, false, -.15f, -.15f).released, "release occurs only once");
    frame = view.Update(true, true, true, -pi + .1f, pi - .1f);
    Check(Near(frame.yaw, pi - .1f), "rear wraparound keeps the acquisition direction");
    Check(Near(view.Update(true, true, true, 1.f, pi - .1f).yaw, pi - .1f),
          "no camera turn holds the view while swing physics turns");
    frame = view.Update(true, true, false, -1.f, pi - .1f, 7);
    Check(Near(frame.yaw, pi - .1f), "selecting another target captures the current view without a spin");
    Check(Near(view.Update(true, true, false, -.8f, pi - .1f, 7).yaw, pi + .1f),
          "the new target's subsequent motion uses its new acquisition offset");
    view.Update(false, true, true, 1.f, 2.f);
    Check(!view.active && !view.Update(true, false, false, 1.f, 2.f).released,
          "cinematics and leaving VR clear the action without rewriting body heading");
    view.Update(true, true, false, 2.f, 1.f);
    view.Reset();
    Check(Near(view.Update(true, true, false, -.5f, .7f).yaw, .7f), "a new morph/session captures a fresh base");
    std::puts("port_vr_action_view_tests: ok");
}
