// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#if defined(MP_ENABLE_OPENXR) && defined(_WIN32)

#include "vr/openxr_backend.h"
#include "vr/openxr_d3d12.h"
#include "vr/openxr_runtime.h"
#include "vr/openxr_vulkan_direct.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace PortVr {

// PC VR's graphics backend, as the graphics_backend setting asks for Aurora's: D3D12 (also for
// Auto), or Vulkan through direct presentation on Dawn's own device (OpenXRVulkanDirectBackend,
// which needs PrimedGun's patched Dawn, tools/Build-WindowsDawn.ps1). The choice is made when this
// object is created, before Aurora picks its device, and kept for its life. Both backends share
// the method surface and threading rules documented on OpenXRD3D12Backend.
class OpenXRWindowsBackend final {
    // Forwards to whichever backend this object holds (defined first: the forwarders below
    // deduce their return types from these).
    template <typename F>
    decltype(auto) Call(F&& call) {
        return vulkan_ != nullptr ? call(*vulkan_) : call(*d3d12_);
    }
    template <typename F>
    decltype(auto) Call(F&& call) const {
        return vulkan_ != nullptr ? call(std::as_const(*vulkan_)) : call(std::as_const(*d3d12_));
    }

public:
    OpenXRWindowsBackend(OpenXRLogCallback logger, bool vulkan) {
        if (vulkan) {
            vulkan_ = std::make_unique<OpenXRVulkanDirectBackend>(std::move(logger));
        } else {
            d3d12_ = std::make_unique<OpenXRD3D12Backend>(std::move(logger));
        }
    }

    OpenXRWindowsBackend(const OpenXRWindowsBackend&) = delete;
    OpenXRWindowsBackend& operator=(const OpenXRWindowsBackend&) = delete;

    bool IsVulkan() const { return vulkan_ != nullptr; }

    // D3D12 only: the adapter the runtime wants Aurora's device on (Vulkan picks it through the
    // runtime instead, in Dawn's device hooks).
    const OpenXRD3D12GraphicsRequirements& D3D12GraphicsRequirements() const { return d3d12_->GraphicsRequirements(); }

    bool QueryGraphicsRequirements(OpenXRRuntime& runtime) { return Call([&](auto& b) { return b.QueryGraphicsRequirements(runtime); }); }
    bool BindAurora(OpenXRRuntime& runtime) { return Call([&](auto& b) { return b.BindAurora(runtime); }); }
    void SetRenderScale(float scale) { Call([&](auto& b) { b.SetRenderScale(scale); }); }

    OpenXRBeginStatus BeginFrame(const OpenXRPresentation& presentation, OpenXRBackendFrame& frame) {
        return Call([&](auto& b) { return b.BeginFrame(presentation, frame); });
    }
    OpenXRSubmissionStatus WaitForSubmission(const OpenXRBackendFrame& frame, uint32_t timeout_ms = UINT32_MAX) {
        return Call([&](auto& b) { return b.WaitForSubmission(frame, timeout_ms); });
    }
    bool TryCancelPendingFrame(OpenXRBackendFrame& frame) { return Call([&](auto& b) { return b.TryCancelPendingFrame(frame); }); }
    bool RepeatFrame(const OpenXRBackendFrame& frame) { return Call([&](auto& b) { return b.RepeatFrame(frame); }); }
    bool FinishFrame(OpenXRBackendFrame& frame, bool submit_layer) {
        return Call([&](auto& b) { return b.FinishFrame(frame, submit_layer); });
    }

    OpenXRBeginStatus PreparePacket(const OpenXRPresentation& presentation, OpenXRBackendFrame& packet) {
        return Call([&](auto& b) { return b.PreparePacket(presentation, packet); });
    }
    bool TryCancelPendingPacket(OpenXRBackendFrame& packet) { return Call([&](auto& b) { return b.TryCancelPendingPacket(packet); }); }
    OpenXRBeginStatus BeginFrameForPacket(const OpenXRBackendFrame& packet, OpenXRBackendFrame& frame) {
        return Call([&](auto& b) { return b.BeginFrameForPacket(packet, frame); });
    }
    OpenXRSubmissionStatus CopyRenderedEyes(const OpenXRBackendFrame& frame) {
        return Call([&](auto& b) { return b.CopyRenderedEyes(frame); });
    }
    OpenXRBeginStatus KeepAliveCycle() { return Call([&](auto& b) { return b.KeepAliveCycle(); }); }
    OpenXRBeginStatus LocatePacket(const OpenXRPresentation& presentation, OpenXRBackendFrame& packet,
                                   uint32_t periods_ahead) {
        return Call([&](auto& b) { return b.LocatePacket(presentation, packet, periods_ahead); });
    }
    OpenXRBeginStatus ArmPacket(OpenXRBackendFrame& packet) { return Call([&](auto& b) { return b.ArmPacket(packet); }); }
    bool SupportsPipelining() const { return Call([&](const auto& b) { return b.SupportsPipelining(); }); }
    bool EyesResizing() const { return Call([&](const auto& b) { return b.EyesResizing(); }); }
    bool Shutdown() { return Call([&](auto& b) { return b.Shutdown(); }); }

    bool IsBound() const { return Call([&](const auto& b) { return b.IsBound(); }); }
    bool PanelLayerAvailable() const { return Call([&](const auto& b) { return b.PanelLayerAvailable(); }); }
    int64_t SwapchainFormat() const { return Call([&](const auto& b) { return b.SwapchainFormat(); }); }
    const std::string& LastError() const { return Call([&](const auto& b) -> const std::string& { return b.LastError(); }); }

private:
    std::unique_ptr<OpenXRD3D12Backend> d3d12_;
    std::unique_ptr<OpenXRVulkanDirectBackend> vulkan_;
};

} // namespace PortVr

#endif // defined(MP_ENABLE_OPENXR) && defined(_WIN32)
