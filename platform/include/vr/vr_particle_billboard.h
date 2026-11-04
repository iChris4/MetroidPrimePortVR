// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Kyoto/Graphics/CGraphics.hpp"
#include <dolphin/gx/GXGeometry.h>
#ifdef TARGET_PC
#include "vr/vr_billboard_math.h"
#include "vr/vr_view.h"
#endif

// Converts the existing camera-plane quad to a point-facing quad without
// changing its center, authored rotation, scale, UVs, or material. Cache one
// facing per sprite, shared by both eyes; never replace the world view matrix.
class VrParticleBillboard {
public:
    VrParticleBillboard() {
#ifdef TARGET_PC
        CVector3f direction;
        // Not in a visor draw: its camera space is already the head's (vr_view.h).
        active_ = PortVr::VrParticlesFaceHead() &&
                  PortVr::VrHeadGaze(CGraphics::GetViewMatrix(), viewer_, direction);
#endif
    }
    CVector3f Vertex(const CVector3f& center, const CVector3f& vertex) {
#ifdef TARGET_PC
        if (active_) {
            if (!modelReady_) {
                model_ = CGraphics::GetModelMatrix();
                inverse_ = model_.GetInverse();
                rightScale_ = model_.GetRight().Magnitude();
                upScale_ = model_.GetUp().Magnitude();
                forwardScale_ = model_.GetForward().Magnitude();
                modelReady_ = true;
            }
            if (!cached_ || center != center_) {
                center_ = center;
                worldCenter_ = model_ * center;
                const auto basis = PortVr::billboard_math::Facing(
                    {worldCenter_.GetX(), worldCenter_.GetY(), worldCenter_.GetZ()},
                    {viewer_.GetX(), viewer_.GetY(), viewer_.GetZ()}, {0.f, 0.f, 1.f});
                right_ = CVector3f(basis.right[0], basis.right[1], basis.right[2]) * rightScale_;
                up_ = CVector3f(basis.up[0], basis.up[1], basis.up[2]) * upScale_;
                forward_ = CVector3f(basis.forward[0], basis.forward[1], basis.forward[2]) * forwardScale_;
                cached_ = true;
            }
            const CVector3f offset = vertex - center;
            return inverse_ * (worldCenter_ + right_ * offset.GetX() +
                               forward_ * offset.GetY() + up_ * offset.GetZ());
        }
#endif
        return vertex;
    }
    void Emit(const CVector3f& center, float x, float y, float z) {
        const CVector3f vertex = Vertex(center, CVector3f(x, y, z));
        GXPosition3f32(vertex.GetX(), vertex.GetY(), vertex.GetZ());
    }
private:
#ifdef TARGET_PC
    bool active_ = false;
    bool cached_ = false;
    bool modelReady_ = false;
    float rightScale_ = 1.f, upScale_ = 1.f, forwardScale_ = 1.f;
    CVector3f viewer_;
    CVector3f center_;
    CVector3f worldCenter_;
    CVector3f right_, up_, forward_;
    CTransform4f inverse_ = CTransform4f::Identity();
    CTransform4f model_ = CTransform4f::Identity();
#endif
};
