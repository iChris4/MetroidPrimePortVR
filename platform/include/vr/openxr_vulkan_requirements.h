// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#if defined(MP_ENABLE_OPENXR)

#include "vr/openxr_runtime.h"

namespace PortVr {

// What the OpenXR runtime asks of a Vulkan binding, for both Vulkan backends: the Quest's
// AHardwareBuffer bridge (openxr_vulkan.h) and direct presentation (openxr_vulkan_direct.h, the
// Quest and PC VR on Vulkan).
struct OpenXRVulkanGraphicsRequirements {
    // XR_MAKE_VERSION-encoded bounds reported by xrGetVulkanGraphicsRequirements2KHR.
    XrVersion min_api_version = 0;
    XrVersion max_api_version = 0;
    // Which binding extension was negotiated: XR_KHR_vulkan_enable2 when the
    // runtime offers it, otherwise the original XR_KHR_vulkan_enable.
    bool uses_enable2 = false;
};

} // namespace PortVr

#endif // defined(MP_ENABLE_OPENXR)
