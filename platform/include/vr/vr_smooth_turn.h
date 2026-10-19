#pragma once

#include <algorithm>
#include <cmath>

namespace PortVr::SmoothTurn {

struct Output {
    float stick = 0.0f;
    float speed_scale = 1.0f;
};

// The virtual GameCube stick saturates at full deflection. Carry any extra
// yaw rate separately so sensitivity above 1 still increases turning speed.
inline Output Mix(float move, float look, float sensitivity) noexcept {
    sensitivity = std::clamp(sensitivity, 0.2f, 3.0f);
    const float limit = std::max(1.0f, sensitivity);
    const float turn = std::clamp(move + look * sensitivity, -limit, limit);
    return {std::clamp(turn, -1.0f, 1.0f), std::max(1.0f, std::abs(turn))};
}

} // namespace PortVr::SmoothTurn
