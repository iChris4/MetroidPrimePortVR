// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#if defined(MP_ENABLE_OPENXR)

#include "vr/openxr_runtime.h"
#include "vr/openxr_settings_panel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace PortVr {

// Backend-neutral frame vocabulary shared by every graphics binding.
//
// The OpenXR pacing thread in openxr_integration.cpp is written against these
// types and the method surface documented on OpenXRD3D12Backend; each concrete
// backend (D3D12 on Windows, Vulkan on Android) implements that same surface so
// the pacing, retained-layer and policy logic is compiled once for both.

enum class OpenXRFrameMode {
    ImmersiveProjection,
    VirtualScreen,
};

enum class OpenXRBeginStatus {
    Ready,
    SessionNotRunning,
    ExitRequested,
    Error,
};

enum class OpenXRSubmissionStatus {
    Success,
    // GPU work may have touched the compositor image or the shared buffers with no completion
    // marker to wait on; the session cannot continue.
    Failed,
    // The eye copy was not submitted and nothing touched the compositor image or the shared
    // buffers, so the frame may end without a layer and the next one is tried normally.
    Skipped,
    Timeout,
    ShuttingDown,
};

// PrimedGun's VR menu (openxr_settings_panel.h) as compositor quad layers of
// its own, over the eyes or the menu screen, as PrimedGun showed it. Its image
// is copied in with the frame's eyes (Aurora's panel stereo target) into a
// swapchain of the menu image's own size; the laser and the hit dot are two
// more quads cut from that image's sprite strip. Nothing is allocated or
// copied until the menu first opens, and nothing is submitted while it is
// closed.
struct OpenXRPanelLayer {
    // The frame renders the panel's image: set by the pacing thread when the
    // panel is open, cleared by a backend that could not provide the layer.
    bool requested = false;
    // Where it hangs in the application space, once the hand or head pose is known.
    bool placed = false;
    XrPosef pose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
    float width_meters = 0.0f;
    float height_meters = 0.0f;
    // The image rows the panel shows from the top: the menu's canvas, or the
    // shader compilation toast's strip while the menu is closed.
    int32_t image_rows = VrMenu::kMenuHeight;
    // The pointer hand's laser, from its aim pose to the menu (or 8 m out).
    bool laser = false;
    XrPosef laser_pose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
    float laser_length_meters = 0.0f;
    // The dot where the laser meets the menu.
    bool dot = false;
    XrPosef dot_pose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
};

// The panel image's size: the menu canvas and its sprite strip.
inline constexpr uint32_t kOpenXRPanelLayerWidth = static_cast<uint32_t>(kSettingsPanelWidthPixels);
inline constexpr uint32_t kOpenXRPanelLayerHeight = static_cast<uint32_t>(kSettingsPanelHeightPixels);
inline constexpr uint32_t kOpenXRPanelMaxLayers = 3;

// How many more pacing cycles an eye swapchain pair replaced by a new render
// resolution lives on before it is destroyed. The pair was last shown by the
// xrEndFrame of the cycle before its replacement, and the compositor keeps
// reading that layer's images until a later one lands and its own GPU work on
// them is done; the spec allows a runtime to use the images after
// xrDestroySwapchain, but destroying a pair the compositor was still consuming
// lost the Vulkan device on a PC runtime. Each cycle ends a compositor frame,
// so this many later frames have replaced the pair's picture by then.
inline constexpr uint32_t kOpenXRRetiredSwapchainCycles = 8;

// The menu's layers, submitted after (so over) the scene's: the menu, then its
// laser, then the hit dot. They all read the same swapchain image, each its
// own rectangle of it (VrMenu::kLaserRect, kDotRect). Returns how many of
// `quads` it filled.
inline uint32_t OpenXRPanelQuadLayers(const OpenXRPanelLayer& panel, XrSpace space, XrSwapchain swapchain,
                                      std::array<XrCompositionLayerQuad, kOpenXRPanelMaxLayers>& quads) noexcept {
    uint32_t count = 0;
    const auto append = [&](const XrPosef& pose, const VrMenu::Rect& rect, XrExtent2Df size) {
        screen_math::Pose normalized{{pose.position.x, pose.position.y, pose.position.z},
                                    {pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w}};
        if (!screen_math::NormalizePose(normalized) || !std::isfinite(size.width) ||
            !std::isfinite(size.height) || !(size.width > 0.f) || !(size.height > 0.f)) {
            return false;
        }
        XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
        // PrimedGun's menu image holds straight (unpremultiplied) alpha.
        quad.layerFlags =
            XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
        quad.space = space;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.subImage.swapchain = swapchain;
        quad.subImage.imageRect = {{rect.x, rect.y}, {rect.w, rect.h}};
        quad.subImage.imageArrayIndex = 0;
        quad.pose.position = {normalized.position[0], normalized.position[1], normalized.position[2]};
        quad.pose.orientation = {normalized.orientation[0], normalized.orientation[1],
                                 normalized.orientation[2], normalized.orientation[3]};
        quad.size = size;
        quads[count++] = quad;
        return true;
    };
    // Never let bad menu tracking tear down the whole VR session. A bad menu
    // is omitted for this frame; a bad laser/dot is omitted independently.
    if (!append(panel.pose, {0, 0, VrMenu::kWidth, panel.image_rows},
                {panel.width_meters, panel.height_meters})) return 0;
    if (panel.laser) {
        append(panel.laser_pose, VrMenu::kLaserRect, {VrMenu::kLaserWidth, panel.laser_length_meters});
    }
    if (panel.dot) {
        append(panel.dot_pose, VrMenu::kDotRect, {VrMenu::kDotSize, VrMenu::kDotSize});
    }
    return count;
}

// The part of the virtual screen's image that holds anything. Aurora
// letterboxes the desktop snapshot into that eye-sized image exactly like this
// (webgpu::calculate_present_viewport_for_aspect); the rest is black. An
// unknown content_aspect (0) keeps the whole image.
inline XrRect2Di OpenXRVirtualScreenContentRect(uint32_t width, uint32_t height, float content_aspect) noexcept {
    XrRect2Di rect{{0, 0}, {static_cast<int32_t>(width), static_cast<int32_t>(height)}};
    if (width == 0 || height == 0 || !(content_aspect > 0.0f)) {
        return rect;
    }
    uint32_t content_width = width;
    uint32_t content_height = std::min<uint32_t>(
        height, std::max<uint32_t>(1u, static_cast<uint32_t>(std::lround(
                                           static_cast<double>(width) * static_cast<double>(1.0f / content_aspect)))));
    if (content_height == height) {
        content_width = std::min<uint32_t>(
            width, std::max<uint32_t>(1u, static_cast<uint32_t>(std::lround(static_cast<double>(height) *
                                                                            static_cast<double>(content_aspect)))));
    }
    rect.offset = {static_cast<int32_t>((width - content_width) / 2),
                   static_cast<int32_t>((height - content_height) / 2)};
    rect.extent = {static_cast<int32_t>(content_width), static_cast<int32_t>(content_height)};
    return rect;
}

struct OpenXRPresentation {
    OpenXRFrameMode mode = OpenXRFrameMode::ImmersiveProjection;

    // Used only by VirtualScreen.
    float quad_distance_meters = 2.0f;
    float quad_width_meters = 2.4f;
    // The desktop snapshot's width over height, which Aurora letterboxes into
    // the screen's image (see OpenXRVirtualScreenContentRect); 0 while unknown.
    float quad_content_aspect = 0.0f;

    // When quad_anchored is set, the quad is placed at quad_pose in the
    // application reference space and stays put as the player looks around.
    // Otherwise it falls back to being head-locked in XR_VIEW_SPACE, centered
    // straight ahead at -Z, which is what happens until tracking has produced a
    // head pose good enough to anchor against.
    bool quad_anchored = false;
    XrPosef quad_pose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};

    // Used only by ImmersiveProjection: Aurora left each eye transparent outside
    // the race's 2D-layer screen (AuroraStereoFrame::window), so the projection
    // layer is blended by its alpha over whatever is under it.
    bool immersive_window = false;
    // The immersive window's eyes were aimed through the window itself, so each
    // is only the window: its image is render_width x render_height, the
    // top-left part of the eye's swapchain image the layer shows.
    bool window_eyes = false;

    // Show the room through the headset's cameras around the virtual screen or
    // the immersive window (OpenXRPassthrough). Taken when the presentation is
    // handed to the backend, which starts or pauses the view then; a backend
    // without one ignores it.
    bool passthrough = false;

    OpenXRPanelLayer panel;
};

struct OpenXRBackendFrame {
    OpenXRFrame xr_frame;
    OpenXRPresentation presentation;
    std::array<uint32_t, kOpenXREyeCount> render_width{};
    std::array<uint32_t, kOpenXREyeCount> render_height{};
    bool expects_gpu_submission = false;
};

} // namespace PortVr

#endif // defined(MP_ENABLE_OPENXR)
