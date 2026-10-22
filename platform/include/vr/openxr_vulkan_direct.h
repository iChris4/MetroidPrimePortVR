// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#if defined(MP_ENABLE_OPENXR) && defined(__ANDROID__)

#include "vr/openxr_backend.h"
#include "vr/openxr_runtime.h"
#include "vr/openxr_vulkan.h"

#include <cstdint>
#include <memory>
#include <string>

namespace PortVr {

// Same-device Dawn/OpenXR Vulkan backend for the Quest (direct presentation), after
// Wiicompiled_VR's Windows Vulkan backend. It needs PrimedGun's patched Dawn (quest/dawn).
//
// QueryGraphicsRequirements() runs after OpenXRRuntime::Initialize() and before
// aurora_initialize(): it installs the hooks through which Dawn creates its Vulkan instance and
// device with the runtime (XR_KHR_vulkan_enable2, aurora/vulkan_direct_interop.h).
// BindAurora() runs afterwards and binds the session to Dawn's own device and queue. Each frame's
// swapchain images are acquired before Aurora renders, and Aurora draws the eyes straight into
// them on Dawn's queue: no second device, no shared buffers, one pass per eye.
//
// The method surface and threading rules are OpenXRVulkanBackend's (openxr_vulkan.h): everything
// from BeginFrame()/PreparePacket() through FinishFrame() belongs to one XR pacing thread, and
// Aurora's frame worker only publishes a token that WaitForSubmission() consumes. The runtime's
// calls that use the queue are serialized with Dawn's submissions through Dawn's device lock.
class OpenXRVulkanDirectBackend final {
public:
    explicit OpenXRVulkanDirectBackend(OpenXRLogCallback logger = {});
    ~OpenXRVulkanDirectBackend();

    OpenXRVulkanDirectBackend(const OpenXRVulkanDirectBackend&) = delete;
    OpenXRVulkanDirectBackend& operator=(const OpenXRVulkanDirectBackend&) = delete;
    OpenXRVulkanDirectBackend(OpenXRVulkanDirectBackend&&) = delete;
    OpenXRVulkanDirectBackend& operator=(OpenXRVulkanDirectBackend&&) = delete;

    bool QueryGraphicsRequirements(OpenXRRuntime& runtime);
    bool BindAurora(OpenXRRuntime& runtime);
    void SetRenderScale(float scale);

    OpenXRBeginStatus BeginFrame(const OpenXRPresentation& presentation, OpenXRBackendFrame& frame);
    OpenXRSubmissionStatus WaitForSubmission(const OpenXRBackendFrame& frame,
                                             uint32_t timeout_ms = UINT32_MAX);
    bool TryCancelPendingFrame(OpenXRBackendFrame& frame);
    bool RepeatFrame(const OpenXRBackendFrame& frame);
    bool FinishFrame(OpenXRBackendFrame& frame, bool submit_layer);

    // Render-first pacing: PreparePacket acquires the images Aurora copies the eyes into while no
    // compositor frame is open; BeginFrameForPacket accepts only a completed packet, and
    // CopyRenderedEyes has nothing left to do (the eyes are already in the images).
    OpenXRBeginStatus PreparePacket(const OpenXRPresentation& presentation, OpenXRBackendFrame& packet);
    bool TryCancelPendingPacket(OpenXRBackendFrame& packet);
    OpenXRBeginStatus BeginFrameForPacket(const OpenXRBackendFrame& packet, OpenXRBackendFrame& frame);
    OpenXRSubmissionStatus CopyRenderedEyes(const OpenXRBackendFrame& frame);
    OpenXRBeginStatus KeepAliveCycle();
    // Pipelined pacing (openxr_integration.cpp PipelinedCycle), as openxr_d3d12.h's: a packet's
    // poses first, located `periods_ahead` display periods past the last compositor frame, its
    // images and Aurora targets later (ArmPacket), once the pending packet's eyes were submitted.
    // It then queues behind that packet on the other swapchain pair and becomes the pending one
    // when that packet's compositor frame ends. PreparePacket is both at once. Supported while
    // every eye and panel swapchain has two images or more: the queued packet's image comes from
    // the pair on display.
    OpenXRBeginStatus LocatePacket(const OpenXRPresentation& presentation, OpenXRBackendFrame& packet,
                                   uint32_t periods_ahead);
    OpenXRBeginStatus ArmPacket(OpenXRBackendFrame& packet);
    bool SupportsPipelining() const;
    // A new render resolution still waits for one of the two eye swapchain pairs, which is only
    // rebuilt with no packet in flight: pipelined pacing waits (vr/openxr_eye_resize.h).
    bool EyesResizing() const;

    // Call on the XR owner thread after Aurora's worker is idle and before aurora_shutdown().
    // False means Dawn's queue could not be drained: the caller retains the backend and runtime.
    bool Shutdown();

    bool IsBound() const;
    bool PanelLayerAvailable() const;
    const OpenXRVulkanGraphicsRequirements& GraphicsRequirements() const;
    int64_t SwapchainFormat() const;
    const std::string& LastError() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

// The Quest's graphics backend: direct presentation (OpenXRVulkanDirectBackend) when the
// direct_present setting asks for it and Aurora's Dawn offers it, otherwise the AHardwareBuffer
// bridge (OpenXRVulkanBackend). The choice is made in QueryGraphicsRequirements(), before Aurora
// creates its device, and kept for this object's life.
class OpenXRQuestVulkanBackend final {
public:
    explicit OpenXRQuestVulkanBackend(OpenXRLogCallback logger = {});
    ~OpenXRQuestVulkanBackend();

    OpenXRQuestVulkanBackend(const OpenXRQuestVulkanBackend&) = delete;
    OpenXRQuestVulkanBackend& operator=(const OpenXRQuestVulkanBackend&) = delete;
    OpenXRQuestVulkanBackend(OpenXRQuestVulkanBackend&&) = delete;
    OpenXRQuestVulkanBackend& operator=(OpenXRQuestVulkanBackend&&) = delete;

    bool QueryGraphicsRequirements(OpenXRRuntime& runtime);
    bool BindAurora(OpenXRRuntime& runtime);
    void SetRenderScale(float scale);

    OpenXRBeginStatus BeginFrame(const OpenXRPresentation& presentation, OpenXRBackendFrame& frame);
    OpenXRSubmissionStatus WaitForSubmission(const OpenXRBackendFrame& frame,
                                             uint32_t timeout_ms = UINT32_MAX);
    bool TryCancelPendingFrame(OpenXRBackendFrame& frame);
    bool RepeatFrame(const OpenXRBackendFrame& frame);
    bool FinishFrame(OpenXRBackendFrame& frame, bool submit_layer);
    OpenXRBeginStatus PreparePacket(const OpenXRPresentation& presentation, OpenXRBackendFrame& packet);
    bool TryCancelPendingPacket(OpenXRBackendFrame& packet);
    OpenXRBeginStatus BeginFrameForPacket(const OpenXRBackendFrame& packet, OpenXRBackendFrame& frame);
    OpenXRSubmissionStatus CopyRenderedEyes(const OpenXRBackendFrame& frame);
    OpenXRBeginStatus KeepAliveCycle();
    // Pipelined pacing: direct presentation's (above); the AHardwareBuffer bridge runs
    // render-first without it.
    OpenXRBeginStatus LocatePacket(const OpenXRPresentation& presentation, OpenXRBackendFrame& packet,
                                   uint32_t periods_ahead);
    OpenXRBeginStatus ArmPacket(OpenXRBackendFrame& packet);
    bool SupportsPipelining() const;
    bool EyesResizing() const;
    bool Shutdown();

    bool IsBound() const;
    bool PanelLayerAvailable() const;
    const OpenXRVulkanGraphicsRequirements& GraphicsRequirements() const;
    int64_t SwapchainFormat() const;
    const std::string& LastError() const;

private:
    OpenXRLogCallback logger_;
    std::unique_ptr<OpenXRVulkanDirectBackend> direct_;
    std::unique_ptr<OpenXRVulkanBackend> shared_;
};

} // namespace PortVr

#endif // defined(MP_ENABLE_OPENXR) && defined(__ANDROID__)
