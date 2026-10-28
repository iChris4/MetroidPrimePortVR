// SPDX-License-Identifier: GPL-3.0-or-later
#include "vr/vr_visibility.h"
#include "Kyoto/Math/CFrustumPlanes.hpp"
#include "Kyoto/Math/CAABox.hpp"
#include <cmath>
#include <initializer_list>
#include <cstdio>
#include <cstdlib>
static void Check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "VR visibility: %s\n", message); std::abort(); }
}
int main() {
    const CVector3f origin(6.f, -3.f, 2.f); // include physical translation
    for (float yaw : {0.f, 1.5707963f, -1.5707963f, 3.1415926f}) {
        for (float pitch : {-.7f, 0.f, .7f}) {
            const CVector3f forward(-std::sin(yaw) * std::cos(pitch),
                                   std::cos(yaw) * std::cos(pitch), std::sin(pitch));
            const auto view = CTransform4f::LookAt(origin, origin + forward);
            const auto plane = PortVr::FogClipNearPlane(view);
            Check(plane.GetHeight(origin + forward * 4.f) > 0.f,
                  "fog ahead of a side/rear/pitched HMD survives the cap");
            Check(plane.GetHeight(origin - forward) < 0.f,
                  "cap rejects the space behind the tracked head");
            Check(std::fabs(plane.GetHeight(origin + forward * .3f)) < .0001f,
                  "near cap follows roomscale position");
            const CFrustumPlanes frustum(view, 2.f, 1.f, .1f, false, 100.f);
            const auto center = origin + forward * 4.f;
            const CVector3f half(.1f, .1f, .1f);
            Check(frustum.BoxInFrustumPlanes(CAABox(center-half, center+half)),
                  "water patch in tracked view remains visible at any yaw/pitch");
            Check(CFrustumPlanes().BoxInFrustumPlanes(CAABox(center-half, center+half)),
                  "disabled/unknown tracking frustum retains water patches");
        }
    }
}
