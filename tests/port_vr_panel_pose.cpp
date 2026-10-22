#include "vr/openxr_backend.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "VR panel pose: %s\n", message);
        std::abort();
    }
}
}

int main() {
    using namespace PortVr;
    OpenXRPanelLayer panel{};
    panel.width_meters = 1.f;
    panel.height_meters = 0.7f;
    panel.laser = panel.dot = true;
    panel.laser_length_meters = 2.f;
    panel.pose.orientation.w = 1.02f;
    panel.laser_pose.orientation = {0.4f, 0.8f, -0.6f, 1.2f};
    panel.dot_pose.orientation.w = 0.98f;
    std::array<XrCompositionLayerQuad, kOpenXRPanelMaxLayers> quads{};
    const auto build = [&] {
        return OpenXRPanelQuadLayers(panel, XR_NULL_HANDLE, XR_NULL_HANDLE, quads);
    };
    Check(build() == 3, "valid menu, laser and dot all submitted");
    for (const auto& quad : quads) {
        const auto& q = quad.pose.orientation;
        const float length = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
        Check(std::isfinite(length) && std::fabs(length - 1.f) < 1.e-6f, "every layer quaternion is normalized");
    }
    panel.laser_pose.orientation = {};
    Check(build() == 2, "zero laser quaternion omits laser, retaining menu and dot");
    Check(quads[1].subImage.imageRect.offset.x == VrMenu::kDotRect.x, "dot remains after omitted laser");
    panel.dot_pose.position.x = std::numeric_limits<float>::quiet_NaN();
    Check(build() == 1, "nonfinite dot position omits only dot");
    panel.pose.orientation.w = std::numeric_limits<float>::infinity();
    Check(build() == 0, "nonfinite menu rotation omits menu and pointers");
    panel.pose.orientation = {};
    Check(build() == 0, "zero menu rotation is rejected");
    panel.pose.orientation.w = 1.f;
    panel.width_meters = -1.f;
    Check(build() == 0, "negative menu size is rejected");
    panel.width_meters = 1.f;
    panel.laser_pose.orientation.w = 1.f;
    panel.laser_length_meters = std::numeric_limits<float>::infinity();
    Check(build() == 1, "nonfinite laser length is omitted");
    panel.dot_pose.position.x = 0.f;
    panel.laser_length_meters = 2.f;
    Check(build() == 3, "layers resume on next valid tracking sample");

    screen_math::Pose tracked{};
    tracked.orientation = {0.f, 1.f, 0.f, 1.f};
    Check(screen_math::NormalizePose(tracked), "tracking quaternion normalized before menu placement");
    tracked.position[2] = std::numeric_limits<float>::quiet_NaN();
    Check(!screen_math::NormalizePose(tracked), "invalid tracked position rejected");
    std::puts("VR menu layer pose validation passed");
}
