// SPDX-License-Identifier: GPL-3.0-or-later
#include "vr/vr_wrist_visor.h"
#include "vr/vr_view.h"
#include "vr/vr_pad.h"
#include "vr/vr_settings.h"
#include "GuiSys/CGuiModel.hpp"
#include "Kyoto/Graphics/CGraphics.hpp"
#include "Kyoto/Graphics/CModel.hpp"
#include "Kyoto/Graphics/CModelFlags.hpp"
#include "Kyoto/Math/CQuaternion.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/Cameras/CCameraManager.hpp"
#include "MetroidPrime/Cameras/CGameCamera.hpp"
#include "MetroidPrime/Player/CPlayer.hpp"
#include "MetroidPrime/Player/CPlayerState.hpp"
#include <algorithm>

namespace PortVr {
namespace {
// Retain HUD assets; registered widget pointers are cleared by CGuiModel destruction.
TCachedToken<CModel>* s_models[2][4]{};
CGuiModel* s_widgets[2][4]{};
}
void VrWristCaptureModels(CGuiModel* const* icons, CGuiModel* const* frames) noexcept {
    if (!GetVrSettings().wrist_visor_enabled) return;
    for (int layer = 0; layer < 2; ++layer) {
        auto* const* widgets = layer == 0 ? icons : frames;
        for (int i = 0; i < 4; ++i) {
            if (!widgets[i] || !widgets[i]->GetModel().valid()) continue;
            s_widgets[layer][i] = widgets[i];
            auto& source = const_cast<TCachedToken<CModel>&>(*widgets[i]->GetModel());
            if (!source.TryCache()) continue;
            auto*& dest = s_models[layer][i];
            if (!dest) dest = new TCachedToken<CModel>(source);
            else if (dest->GetObject() != source.GetObject()) *dest = source;
        }
    }
}
void VrWristForgetModel(const CGuiModel* widget) noexcept {
    for (auto& layer : s_widgets) for (auto*& current : layer)
        if (current == widget) current = nullptr;
}
void VrWristVisorDraw(const CStateManager& mgr) noexcept {
    const auto settings = GetVrSettings();
    const auto state = GetVrPadState();
    if (!settings.wrist_visor_enabled || !VrImmersive() || !state.wrist_visible ||
        !mgr.GetPlayer() || !mgr.GetCameraManager()) return;
    const auto snapshot = OpenXRGetInputSnapshot();
    const auto& off = snapshot.controllers[settings.use_right_hand ? 0 : 1];
    const auto& pose = WristVisor::WristPose(off);
    if (!snapshot.runtime_active || !off.connected || !pose.valid) return;
    const auto camera = mgr.GetCameraManager()->GetSimulationCameraTransform(mgr);
    const auto trackingToWorld = VrTrackingToWorldTransform(*mgr.GetPlayer(), camera);
    auto wristPose = CQuaternion(pose.orientation[3], pose.orientation[0], pose.orientation[1],
                                 pose.orientation[2]).BuildNormalized().BuildTransform4f();
    wristPose.SetTranslation(CVector3f(pose.position[0], pose.position[1], pose.position[2]));
    const WristVisor::Calibration calibration(settings);
    const auto panel = WristVisor::PanelRotation(settings.use_right_hand ? 1u : 0u, calibration);
    const auto panelPose = CQuaternion(panel[3], panel[0], panel[1], panel[2]).BuildTransform4f();
    // One rigid transform for the whole row: all faces remain parallel and coplanar.
    const auto wristModel = trackingToWorld * wristPose *
        CTransform4f::Translate(calibration.offset[0], calibration.offset[1], calibration.offset[2]) * panelPose;
    const auto oldModel = CGraphics::GetModelMatrix();
    const auto oldView = CGraphics::GetViewMatrix();
    const auto oldProjection = CGraphics::GetProjectionState();
    const auto& gameCamera = mgr.GetCameraManager()->GetCurrentCamera(mgr);
    CGraphics::SetPerspective(gameCamera.GetRenderFov(), gameCamera.GetAspectRatio(),
                              gameCamera.GetNearClipDistance(), gameCamera.GetFarClipDistance());
    CGraphics::SetViewPointMatrix(VrRoomScaleCameraTransform(camera));
    CGraphics::SetFog(kRFM_None, 0.f, 1.f, CColor::Black());
    CGraphics::SetCullMode(kCM_None);
    const auto symbol = WristVisor::SymbolRotation(settings.use_right_hand ? 1u : 0u);
    const auto symbolRotation = CQuaternion(symbol[3], symbol[0], symbol[1], symbol[2]).BuildTransform4f();
    for (int i = 0; i < 4; ++i) {
        const int visor = WristVisor::VisorForSlot[i];
        const bool owned = (state.wrist_owned & (1u << visor)) != 0;
        const auto& center = WristVisor::Centers[i];
        const float alpha = owned ? 1.f : 0.22f;
        for (int layer = 1; layer >= 0; --layer) {
            auto* widget = s_widgets[layer][visor];
            if (!widget) continue;
            auto* token = s_models[layer][visor];
            if (!token || !token->TryCache()) continue;
            auto* model = token->GetObject();
            if (!model || !model->IsLoaded(0)) continue;
            const auto& bounds = model->GetBoundingBox();
            const auto size = bounds.GetMaxPoint() - bounds.GetMinPoint();
            const float extent = std::max(size.GetX(), size.GetZ());
            if (extent < 0.00001f) continue;
            const auto middle = (bounds.GetMinPoint() + bounds.GetMaxPoint()) * 0.5f;
            const float hoverScale = state.wrist_hover == i ? 1.12f : 1.f;
            const float scale = (layer == 0 ? 0.036f : 0.046f) * hoverScale / extent;
            // HUD models use X/Z for their face and -Y towards the viewer.
            // Turn that face onto the back of the wrist without flattening its geometry.
            const auto local = CTransform4f::Translate(center[0], center[1] + (layer == 0 ? 0.002f : 0.f), center[2]) *
                (layer == 0 ? symbolRotation : CTransform4f::Identity()) *
                CTransform4f::Scale(scale, -scale, -scale) * CTransform4f::Translate(-middle);
            widget->DrawWrist(wristModel * local, alpha);
        }
    }
    CGraphics::SetModelMatrix(oldModel);
    CGraphics::SetViewPointMatrix(oldView);
    CGraphics::SetProjectionState(oldProjection);
    CGraphics::SetDepthWriteMode(true, kE_LEqual, true);
    CGraphics::SetCullMode(kCM_Front);
}
} // namespace PortVr
