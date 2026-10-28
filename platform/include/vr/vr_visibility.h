// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "Kyoto/Math/CPlane.hpp"
#include "Kyoto/Math/CTransform4f.hpp"
#include "Kyoto/Math/CUnitVector3f.hpp"

namespace PortVr {
// Match retail's cap distance, but pass the tracked view in immersive rendering.
inline CPlane FogClipNearPlane(const CTransform4f& view) {
    const CUnitVector3f forward(view.GetForward());
    return CPlane(CVector3f::Dot(view.GetTranslation(), forward) + 0.3f, forward);
}
}
