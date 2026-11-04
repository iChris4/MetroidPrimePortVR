// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#if defined(MP_ENABLE_OPENXR) && defined(__ANDROID__)

#include "vr/openxr_backend.h"
#include "vr/openxr_runtime.h"
#include "vr/openxr_vulkan_requirements.h"

#include <cstdint>
#include <memory>
#include <string>

namespace PortVr {

// Two-device Dawn/OpenXR Vulkan backend for Android (Meta Quest).
//
// Method surface and threading rules are identical to OpenXRD3D12Backend so the
// pacing thread in openxr_integration.cpp is shared. The difference is behind
// BindAurora: instead of binding Dawn's device to the session, this backend
// creates its own VkInstance/VkDevice through the OpenXR runtime, owns a small
// ring of AHardwareBuffers per eye, and hands them to Aurora's Vulkan stereo
// bridge (aurora/vulkan_interop.h). When Aurora reports a completed copy, the
// backend copies the buffer into the acquired XrSwapchain image on the
// session's queue, so the compositor sees ordinary same-queue rendering.
//
// QueryGraphicsRequirements() runs after OpenXRRuntime::Initialize() and before
// aurora_initialize(); BindAurora() runs afterwards, while Aurora's frame worker
// is idle. Everything from BeginFrame() through FinishFrame() belongs to one XR
// pacing thread; the Aurora callback only records a small copy on this
// backend's private queue and publishes a token.
class OpenXRVulkanBackend final {
public:
    explicit OpenXRVulkanBackend(OpenXRLogCallback logger = {});
    ~OpenXRVulkanBackend();

    OpenXRVulkanBackend(const OpenXRVulkanBackend&) = delete;
    OpenXRVulkanBackend& operator=(const OpenXRVulkanBackend&) = delete;
    OpenXRVulkanBackend(OpenXRVulkanBackend&&) = delete;
    OpenXRVulkanBackend& operator=(OpenXRVulkanBackend&&) = delete;

    bool QueryGraphicsRequirements(OpenXRRuntime& runtime);
    bool BindAurora(OpenXRRuntime& runtime);

    // As OpenXRD3D12Backend::SetRenderScale. The shared eye buffers are
    // reallocated along with the first pair rebuilt at the new size.
    void SetRenderScale(float scale);

    OpenXRBeginStatus BeginFrame(const OpenXRPresentation& presentation, OpenXRBackendFrame& frame);
    OpenXRSubmissionStatus WaitForSubmission(const OpenXRBackendFrame& frame,
                                             uint32_t timeout_ms = UINT32_MAX);
    bool TryCancelPendingFrame(OpenXRBackendFrame& frame);
    bool RepeatFrame(const OpenXRBackendFrame& frame);
    bool FinishFrame(OpenXRBackendFrame& frame, bool submit_layer);

    // Render-first pacing, used while VR interpolation is off. A packet is prepared with the
    // views located for an estimated display time and handed to Aurora without a compositor
    // frame open; once Aurora has rendered the eyes into the shared buffers, the compositor frame
    // is begun, the eyes are copied into its swapchain images and it is ended at once. A headset
    // frame therefore never waits for a game frame: it stays open for the copy alone.
    OpenXRBeginStatus PreparePacket(const OpenXRPresentation& presentation, OpenXRBackendFrame& packet);
    bool TryCancelPendingPacket(OpenXRBackendFrame& packet);
    OpenXRBeginStatus BeginFrameForPacket(const OpenXRBackendFrame& packet, OpenXRBackendFrame& frame);
    OpenXRSubmissionStatus CopyRenderedEyes(const OpenXRBackendFrame& frame);
    // One compositor cycle that resubmits the retained layer (or nothing), with no frame left
    // active: keeps the runtime fed while the eyes are still being rendered and learns the
    // display timing the next packet is located for.
    OpenXRBeginStatus KeepAliveCycle();
    // Pipelined pacing (openxr_d3d12.h): not supported yet, LocatePacket prepares the
    // packet whole and ArmPacket is a no-op.
    OpenXRBeginStatus LocatePacket(const OpenXRPresentation& presentation, OpenXRBackendFrame& packet,
                                   uint32_t periods_ahead);
    OpenXRBeginStatus ArmPacket(OpenXRBackendFrame& packet);
    bool SupportsPipelining() const;
    // No pipelining here: every cycle may rebuild a pair (openxr_d3d12.h).
    bool EyesResizing() const;

    // Drains this backend's own queue before tearing down. Returns false only
    // when the private device could not be waited on, in which case the caller
    // retains the backend and runtime for the process lifetime.
    bool Shutdown();

    bool IsBound() const;
    // False once the settings panel's own layer could not be set up; the panel
    // is then drawn into the eyes again.
    bool PanelLayerAvailable() const;
    const OpenXRVulkanGraphicsRequirements& GraphicsRequirements() const;
    int64_t SwapchainFormat() const;
    const std::string& LastError() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace PortVr

#endif // defined(MP_ENABLE_OPENXR) && defined(__ANDROID__)
