// SPDX-License-Identifier: GPL-3.0-or-later

#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif
#if defined(_WIN32)
#include <windows.h> // LARGE_INTEGER, QueryPerformanceCounter
#endif

#include "vr/openxr_integration.h"

#include "vr/frame_interpolation_pacing.h"

#include "vr/openxr_diagnostics.h"
#include "vr/openxr_screen_math.h"
#include "vr/prime_vr_policy.h"
#include "vr/virtual_screen_anchor.h"
#include "vr/vr_log.h"
#include "vr/vr_settings.h"
#include <aurora/gfx.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(MP_ENABLE_OPENXR)
#include "vr/openxr_backend.h"
#include "vr/openxr_input.h"
#include "vr/openxr_runtime.h"
#if defined(_WIN32)
#include "vr/openxr_d3d12.h"
#define MP_OPENXR_GRAPHICS_BACKEND 1
#elif defined(__ANDROID__)
#include "vr/openxr_android.h"
#include "vr/openxr_vulkan.h"
#include "vr/openxr_vulkan_direct.h"
#include <time.h>
#include <unistd.h>
#define XR_USE_TIMESPEC
#include <openxr/openxr_platform.h>
#define MP_OPENXR_GRAPHICS_BACKEND 1
#else
#define MP_OPENXR_GRAPHICS_BACKEND 0
#endif
#else
#define MP_OPENXR_GRAPHICS_BACKEND 0
#endif

namespace PortVr {
namespace {

inline constexpr float kDegreesToRadians = 0.01745329252f;

// What the presentation policy may show, from the VR settings. The game thread
// reports the game mode itself (PrimeVRPolicyPublishGameMode).
void ConfigurePolicy(bool enabled) noexcept {
    const PortVrSettings settings = GetVrSettings();
    PrimeVRPolicyConfig config{};
    config.enabled = enabled;
    // Replay off keeps everything on the virtual screen.
    config.immersive = settings.immersive_replay;
    config.cinematic_screen = settings.cinematic_screen_enabled;
    config.game_menu_screen = settings.game_menu_screen_enabled;
    config.message_box_immersive = settings.message_box_immersive;
    config.world_units_per_meter = settings.world_scale;
    config.screen_distance_meters = settings.screen_distance_meters;
    config.screen_width_meters = settings.screen_width_meters;
    PrimeVRPolicyConfigure(config);
}

#if MP_OPENXR_GRAPHICS_BACKEND

#if defined(_WIN32)
// TODO(phase 5): the Vulkan binding through the openxr_windows.h variant wrapper.
using GraphicsBackend = OpenXRD3D12Backend;
inline constexpr const char* kFallbackNote = "; continuing on the mirror output";

#else
// Direct presentation when Aurora's Dawn offers it, else the AHardwareBuffer bridge.
using GraphicsBackend = OpenXRQuestVulkanBackend;
// A standalone headset has no mirror window to fall back to (see OnRuntimeExitRequested).
inline constexpr const char* kFallbackNote = "";
inline constexpr const char* kGraphicsBackendName = "Vulkan";
#endif

struct Quaternion {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};

Quaternion Normalize(Quaternion value) noexcept {
    const float length_squared = value.x * value.x + value.y * value.y +
                                 value.z * value.z + value.w * value.w;
    if (!(length_squared > 1.0e-12f)) {
        return {};
    }
    const float inverse_length = 1.0f / std::sqrt(length_squared);
    value.x *= inverse_length;
    value.y *= inverse_length;
    value.z *= inverse_length;
    value.w *= inverse_length;
    return value;
}

Quaternion Conjugate(Quaternion value) noexcept {
    return {-value.x, -value.y, -value.z, value.w};
}

Quaternion Multiply(const Quaternion& left, const Quaternion& right) noexcept {
    return Normalize({
        left.w * right.x + left.x * right.w + left.y * right.z - left.z * right.y,
        left.w * right.y - left.x * right.z + left.y * right.w + left.z * right.x,
        left.w * right.z + left.x * right.y - left.y * right.x + left.z * right.w,
        left.w * right.w - left.x * right.x - left.y * right.y - left.z * right.z,
    });
}

std::array<float, 3> Rotate(const Quaternion& q, const std::array<float, 3>& value) noexcept {
    // Expanded q * [v,0] * conjugate(q), avoiding two temporary normalizations.
    const float tx = 2.0f * (q.y * value[2] - q.z * value[1]);
    const float ty = 2.0f * (q.z * value[0] - q.x * value[2]);
    const float tz = 2.0f * (q.x * value[1] - q.y * value[0]);
    return {
        value[0] + q.w * tx + (q.y * tz - q.z * ty),
        value[1] + q.w * ty + (q.z * tx - q.x * tz),
        value[2] + q.w * tz + (q.x * ty - q.y * tx),
    };
}

void RotationMatrix(const Quaternion& value, float matrix[9]) noexcept {
    const Quaternion q = Normalize(value);
    const float xx = q.x * q.x;
    const float yy = q.y * q.y;
    const float zz = q.z * q.z;
    const float xy = q.x * q.y;
    const float xz = q.x * q.z;
    const float yz = q.y * q.z;
    const float wx = q.w * q.x;
    const float wy = q.w * q.y;
    const float wz = q.w * q.z;
    matrix[0] = 1.0f - 2.0f * (yy + zz);
    matrix[1] = 2.0f * (xy - wz);
    matrix[2] = 2.0f * (xz + wy);
    matrix[3] = 2.0f * (xy + wz);
    matrix[4] = 1.0f - 2.0f * (xx + zz);
    matrix[5] = 2.0f * (yz - wx);
    matrix[6] = 2.0f * (xz - wy);
    matrix[7] = 2.0f * (yz + wx);
    matrix[8] = 1.0f - 2.0f * (xx + yy);
}

// Midpoint between the eyes: the head position the tracking origin is latched
// to. Callers check XR_VIEW_STATE_POSITION_VALID_BIT first.
std::array<float, 3> CenterPosition(const OpenXRFrame& frame) noexcept {
    const auto& left = frame.views[0].pose;
    const auto& right = frame.views[1].pose;
    return {
        (left.position.x + right.position.x) * 0.5f,
        (left.position.y + right.position.y) * 0.5f,
        (left.position.z + right.position.z) * 0.5f,
    };
}

void IdentityEye(AuroraStereoEye& eye) noexcept {
    std::fill(std::begin(eye.projection), std::end(eye.projection), 0.0f);
    eye.projection[0] = 1.0f;
    eye.projection[5] = 1.0f;
    eye.projection[10] = 1.0f;
    eye.projection[15] = 1.0f;
    std::fill(std::begin(eye.viewFromCenter), std::end(eye.viewFromCenter), 0.0f);
    eye.viewFromCenter[0] = 1.0f;
    eye.viewFromCenter[5] = 1.0f;
    eye.viewFromCenter[10] = 1.0f;
}

void ProjectionFromFov(const XrFovf& fov, float output[16]) noexcept {
    const float left = std::tan(fov.angleLeft);
    const float right = std::tan(fov.angleRight);
    const float down = std::tan(fov.angleDown);
    const float up = std::tan(fov.angleUp);
    const float inverse_width = 1.0f / (right - left);
    const float inverse_height = 1.0f / (up - down);
    std::fill(output, output + 16, 0.0f);
    output[0] = 2.0f * inverse_width;
    output[2] = (right + left) * inverse_width;
    output[5] = 2.0f * inverse_height;
    output[6] = (up + down) * inverse_height;
}

// The base is a position and nothing else. OpenXR keeps its reference spaces
// gravity-aligned, so handing the headset's rotation to the game camera as-is
// leaves the game's horizon level and its forward fixed to the reference space.
// Composing a latched head orientation in here instead would bake that instant's
// pitch and roll into the neutral and tilt the horizon for the rest of the session.
//
// lean_back_radians is the one deliberate exception: a fixed pitch of the game
// camera about the reference space's right axis, for a player sitting reclined.
// It multiplies in on the right, so it turns the world before the head rotation
// rather than after it, which is what makes it cancel a reclined head exactly
// and, when you then look sideways, roll the view the way a real recline would.
void ViewFromBase(const XrPosef& eye_pose, const std::array<float, 3>& base_position,
                  bool position_valid, float units_per_meter, float lean_back_radians,
                  float output[12]) noexcept {
    const Quaternion eye = Normalize({eye_pose.orientation.x, eye_pose.orientation.y,
                                      eye_pose.orientation.z, eye_pose.orientation.w});
    const Quaternion inverse_eye = Conjugate(eye);
    float rotation[9];
    if (lean_back_radians == 0.0f) {
        RotationMatrix(inverse_eye, rotation);
    } else {
        const float half_angle = 0.5f * lean_back_radians;
        const Quaternion lean{std::sin(half_angle), 0.0f, 0.0f, std::cos(half_angle)};
        RotationMatrix(Multiply(inverse_eye, lean), rotation);
    }

    std::array<float, 3> translation{};
    if (position_valid) {
        const std::array<float, 3> base_to_eye{
            base_position[0] - eye_pose.position.x,
            base_position[1] - eye_pose.position.y,
            base_position[2] - eye_pose.position.z,
        };
        translation = Rotate(inverse_eye, base_to_eye);
    }
    output[0] = rotation[0];
    output[1] = rotation[1];
    output[2] = rotation[2];
    output[3] = translation[0] * units_per_meter;
    output[4] = rotation[3];
    output[5] = rotation[4];
    output[6] = rotation[5];
    output[7] = translation[1] * units_per_meter;
    output[8] = rotation[6];
    output[9] = rotation[7];
    output[10] = rotation[8];
    output[11] = translation[2] * units_per_meter;
}

// Distinct names from openxr_runtime.cpp's helpers: both files can share a
// unity-build translation unit and the same anonymous namespace.
const char* DiagnosticSpaceName(XrReferenceSpaceType type) noexcept {
    switch (type) {
    case XR_REFERENCE_SPACE_TYPE_VIEW:
        return "VIEW";
    case XR_REFERENCE_SPACE_TYPE_LOCAL:
        return "LOCAL";
    case XR_REFERENCE_SPACE_TYPE_STAGE:
        return "STAGE";
    default:
        return "OTHER";
    }
}

const char* DiagnosticBlendModeName(XrEnvironmentBlendMode mode) noexcept {
    switch (mode) {
    case XR_ENVIRONMENT_BLEND_MODE_OPAQUE:
        return "OPAQUE";
    case XR_ENVIRONMENT_BLEND_MODE_ADDITIVE:
        return "ADDITIVE";
    case XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND:
        return "ALPHA_BLEND";
    default:
        return "OTHER";
    }
}

// What the runtime reported about the eyes this frame. The cant is the angle
// between the two eyes' forward axes: zero for parallel displays, and the
// headset's display tilt on canted ones (Pimax) unless the runtime is asked for
// parallel projections.
diagnostics::ViewGeometry DiagnosticViewGeometry(const OpenXRBackendFrame& frame) noexcept {
    constexpr float kRadiansToDegrees = 57.29577951f;
    diagnostics::ViewGeometry geometry{};
    std::array<std::array<float, 3>, kOpenXREyeCount> forward{};
    for (uint32_t eye = 0; eye < kOpenXREyeCount; ++eye) {
        const XrView& view = frame.xr_frame.views[eye];
        geometry.fov_degrees[eye] = {view.fov.angleLeft * kRadiansToDegrees, view.fov.angleRight * kRadiansToDegrees,
                                     view.fov.angleUp * kRadiansToDegrees, view.fov.angleDown * kRadiansToDegrees};
        const auto& q = view.pose.orientation;
        forward[eye] = Rotate(Normalize({q.x, q.y, q.z, q.w}), {0.0f, 0.0f, -1.0f});
        geometry.width[eye] = frame.render_width[eye];
        geometry.height[eye] = frame.render_height[eye];
    }
    const float dot = forward[0][0] * forward[1][0] + forward[0][1] * forward[1][1] + forward[0][2] * forward[1][2];
    geometry.cant_degrees = std::acos(std::clamp(dot, -1.0f, 1.0f)) * kRadiansToDegrees;
    if ((frame.xr_frame.view_state_flags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0) {
        const auto& left = frame.xr_frame.views[0].pose.position;
        const auto& right = frame.xr_frame.views[1].pose.position;
        const float dx = right.x - left.x;
        const float dy = right.y - left.y;
        const float dz = right.z - left.z;
        geometry.ipd_millimeters = std::sqrt(dx * dx + dy * dy + dz * dz) * 1000.0f;
    }
    return geometry;
}

class OpenXRIntegration final {
public:
    static OpenXRIntegration& Get() {
        static OpenXRIntegration integration;
        return integration;
    }

    OpenXRStartupResult Prepare(AuroraConfig& aurora_config) {
        Shutdown();
        {
            std::lock_guard lock(error_mutex_);
            last_error_.clear();
        }
        if (graphics_retained_) {
            SetError(std::string("OpenXR cannot be restarted after an unfenceable ") +
                     kGraphicsBackendName + " submission");
            return OpenXRStartupResult::Unavailable;
        }
        // The pacing thread is not running here (Shutdown above joined it), so
        // the sink may be replaced.
        diagnostics::SetLogSink(
            [](std::string_view line) { PORTVR_LOG() << line << std::endl; });
        diagnostics::SetEnabled(GetVrSettings().diagnostics_logging);
        requested_ = GetVrSettings().enabled;
        ConfigurePolicy(requested_);
        if (!requested_) {
            return OpenXRStartupResult::Disabled;
        }
        if (!BackendMatchesConfiguredGraphicsApi(aurora_config)) {
            return OpenXRStartupResult::Unavailable;
        }

        logger_ = [](OpenXRLogLevel level, std::string_view message) {
            const char* name = level == OpenXRLogLevel::Error ? "error" :
                               level == OpenXRLogLevel::Warning ? "warning" : "info";
            PORTVR_LOG() << "[openxr::" << name << "] " << message << std::endl;
        };
#if defined(__ANDROID__)
        {
            std::string loader_error;
            if (!OpenXRAndroidInitializeLoader(logger_, &loader_error)) {
                SetError("OpenXR Android loader initialization failed: " + loader_error);
                return OpenXRStartupResult::Unavailable;
            }
        }
#endif
        runtime_ = std::make_unique<OpenXRRuntime>(logger_);
        backend_ = std::make_unique<GraphicsBackend>(logger_);

        OpenXRConfig config{};
        config.application_name = aurora_config.appName != nullptr ? aurora_config.appName
                                                                    : "Metroid Prime";
        config.engine_name = "Aurora";
        config.resolution_scale = GetVrSettings().render_scale;
        // render_scale_ was read when this object was made, before the settings file
        // loaded; the pacing loop would size the eyes back to that default.
        SetRenderScale(config.resolution_scale);
#if defined(_WIN32)
        config.required_extensions = {kRequiredAuroraBackend == BACKEND_VULKAN ? "XR_KHR_vulkan_enable2" : "XR_KHR_D3D12_enable"};
        config.optional_extensions = {"XR_KHR_win32_convert_performance_counter_time",
                                      "XR_FB_display_refresh_rate", "XR_EXT_performance_settings"};
#else
        // Either Vulkan binding extension is acceptable; the backend picks
        // whichever the runtime enabled, preferring enable2.
        config.required_extensions = {"XR_KHR_android_create_instance"};
        // XR_FB_passthrough: the room around the virtual screen (OpenXRPassthrough), asked for
        // whatever [vr] passthrough says, since the setting is live.
        config.optional_extensions = {"XR_KHR_vulkan_enable2", "XR_KHR_vulkan_enable",
                                      "XR_KHR_convert_timespec_time",
                                      "XR_KHR_android_thread_settings",
                                      "XR_FB_display_refresh_rate", "XR_EXT_performance_settings",
                                      "XR_FB_passthrough"};
        config.instance_create_next = OpenXRAndroidInstanceCreateNext();
#endif
        config.optional_extensions.push_back("XR_VALVE_frame_controller_interaction");
        config.optional_extensions.push_back("XR_BD_controller_interaction");
        if (!runtime_->Initialize(config)) {
            SetError("OpenXR instance initialization failed: " + runtime_->LastError().message);
            ResetPreparedObjects();
            return OpenXRStartupResult::Unavailable;
        }
        const auto& extensions = runtime_->EnabledExtensions();
        const auto has_extension = [&](const char* name) {
            return std::find(extensions.begin(), extensions.end(), name) != extensions.end();
        };
#if defined(_WIN32)
        if (has_extension("XR_KHR_win32_convert_performance_counter_time")) {
            runtime_->LoadFunction("xrConvertTimeToWin32PerformanceCounterKHR", &convert_display_time_);
        }
#else
        if (has_extension("XR_KHR_convert_timespec_time")) {
            runtime_->LoadFunction("xrConvertTimeToTimespecTimeKHR", &convert_display_time_);
        }
#endif
        if (has_extension("XR_FB_display_refresh_rate")) {
            runtime_->LoadFunction("xrGetDisplayRefreshRateFB", &get_display_refresh_rate_);
            runtime_->LoadFunction("xrEnumerateDisplayRefreshRatesFB", &enumerate_display_refresh_rates_);
            runtime_->LoadFunction("xrRequestDisplayRefreshRateFB", &request_display_refresh_rate_);
        }
        if (has_extension("XR_EXT_performance_settings")) {
            runtime_->LoadFunction("xrPerfSettingsSetPerformanceLevelEXT", &set_performance_level_);
        }
        interpolation_available_.store(convert_display_time_ != nullptr, std::memory_order_release);
        if (!backend_->QueryGraphicsRequirements(*runtime_)) {
            SetError(backend_->LastError());
            ResetPreparedObjects();
            return OpenXRStartupResult::Unavailable;
        }

        ApplyGraphicsRequirements(aurora_config);
        prepared_ = true;
        return OpenXRStartupResult::Prepared;
    }

    bool Start(AuroraBackend active_backend) {
        if (!prepared_ || runtime_ == nullptr || backend_ == nullptr) {
            return !requested_;
        }
        if (active_backend != kRequiredAuroraBackend) {
            SetError(std::string("Aurora could not create the OpenXR-required ") +
                     kGraphicsBackendName + " backend");
            ResetPreparedObjects();
            return false;
        }
        if (!backend_->BindAurora(*runtime_)) {
            SetError(backend_->LastError());
            ResetPreparedObjects();
            return false;
        }
        {
            const OpenXRViewConfiguration& left = runtime_->ViewConfiguration()[0];
            std::lock_guard lock(eye_view_mutex_);
            eye_view_ = left.properties;
            eye_width_.store(left.render_width, std::memory_order_relaxed);
            eye_height_.store(left.render_height, std::memory_order_relaxed);
        }
        input_ = std::make_unique<OpenXRInput>(logger_);
        if (!input_->Create(*runtime_)) {
            PORTVR_LOG() << "OpenXR controller input unavailable: " << input_->LastError()
                                   << std::endl;
            input_.reset();
        }

#if defined(__ANDROID__)
        // The producer: SDL's main thread, which also carries every guest fiber.
        game_thread_id_ = static_cast<uint32_t>(gettid());
#endif
        stop_.store(false, std::memory_order_release);
        {
            std::lock_guard lock(interpolation_mutex_);
            interpolation_stopping_ = false;
        }
        teardown_requested_.store(false, std::memory_order_release);
        // How every eye is replayed, fixed: it ends at the frame's final GXCopyDisp, so it holds the
        // image the game presented; it keeps the EFB reset after a display copy, which can only be
        // an earlier one's and erases what that last copy did not show; and it is drawn in one
        // render pass.
        aurora_set_stereo_stop_at_display_copy(true);
        aurora_set_stereo_skip_copy_clears(false);
        aurora_set_stereo_single_pass_eyes(true);
        WithdrawPublishedFrame();
        aurora_set_stereo_frame_provider(&OpenXRIntegration::ProvideStereoFrame, this);
        provider_registered_ = true;
        if (convert_display_time_ != nullptr) {
            diagnostics::SetDisplayTimeConverter(
                [this](int64_t xr_time) { return static_cast<int64_t>(DisplayTimeNanos(xr_time)); });
        }
        running_.store(true, std::memory_order_release);
        try {
            pacing_thread_ = std::thread([this] { PacingThread(); });
        } catch (const std::exception& exception) {
            running_.store(false, std::memory_order_release);
            aurora_set_stereo_frame_provider(nullptr, nullptr);
            provider_registered_ = false;
            SetError(std::string("could not start the OpenXR pacing thread: ") + exception.what());
            ResetPreparedObjects();
            return false;
        }
        PORTVR_LOG() << "OpenXR asynchronous " << kGraphicsBackendName
                               << " presentation started" << std::endl;
        return true;
    }

    void Shutdown() noexcept {
        teardown_requested_.store(false, std::memory_order_release);
        // Called on the game thread: aurora's producer (the GX thread) must be
        // idle before the frame worker is quiesced.
        // Stop idle replays before draining; no new worker job may race provider removal.
        {
            std::lock_guard lock(interpolation_mutex_);
            interpolation_stopping_ = true;
            aurora_set_stereo_frame_interpolation(false);
        }
        if (pacing_thread_.joinable()) {
            // Registration changes are only safe while no sealed frame is in
            // flight. The caller invokes us before Aurora teardown.
            aurora_quiesce_frame_worker();
            aurora_set_stereo_frame_provider(nullptr, nullptr);
            provider_registered_ = false;
            WithdrawPublishedFrame();
            {
                // Pair the predicate update with the wait mutex. Otherwise a
                // terminal pacing thread can observe false, miss the notify,
                // and make join wait forever.
                std::lock_guard lock(stop_mutex_);
                stop_.store(true, std::memory_order_release);
            }
            stop_cv_.notify_all();
            pacing_thread_.join();
        } else {
            if (provider_registered_) {
                aurora_quiesce_frame_worker();
                aurora_set_stereo_frame_provider(nullptr, nullptr);
                provider_registered_ = false;
            }
            ShutdownOrRetainGraphicsObjects();
        }
        running_.store(false, std::memory_order_release);
        request_cv_.notify_all();
        PrimeVRPolicySetSessionActive(false);
        // The converter reads runtime_; the pacing thread has stopped using it.
        diagnostics::SetDisplayTimeConverter({});
        backend_.reset();
        runtime_.reset();
        prepared_ = false;
        convert_display_time_ = nullptr;
        get_display_refresh_rate_ = nullptr;
        enumerate_display_refresh_rates_ = nullptr;
        request_display_refresh_rate_ = nullptr;
        display_refresh_rate_requested_ = false;
        set_performance_level_ = nullptr;
        headset_hz_.store(0, std::memory_order_relaxed);
        rendered_fps_.store(0, std::memory_order_relaxed);
        interpolation_available_.store(false, std::memory_order_release);
        {
            std::lock_guard lock(eye_view_mutex_);
            eye_view_ = {XR_TYPE_VIEW_CONFIGURATION_VIEW};
            eye_width_.store(0, std::memory_order_relaxed);
            eye_height_.store(0, std::memory_order_relaxed);
        }
        ResetTrackingOrigin();
        applied_session_run_serial_ = 0;
        session_was_active_ = false;
    }

    bool IsRunning() const noexcept { return running_.load(std::memory_order_acquire); }

    void RequestRecenter() noexcept {
        recenter_requested_.store(true, std::memory_order_release);
    }

    void SetRecenterCallback(void (*callback)()) noexcept {
        recenter_callback_.store(callback, std::memory_order_release);
    }

    // The game thread draws once per XR frame; it waits here for the pacing
    // thread to publish the poses of the next packet. `request` carries the
    // serial of the last request the caller consumed.
    bool WaitForFrameRequest(OpenXRFrameRequest& request, uint32_t timeout_ms) {
        std::unique_lock lock(request_mutex_);
        const uint64_t last = request.serial;
        if (!request_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] {
                return request_.serial > last || !running_.load(std::memory_order_acquire);
            })) {
            return false;
        }
        if (request_.serial <= last) {
            return false;
        }
        request = request_;
        return true;
    }

    uint32_t FrameRequestTimeoutMs() const noexcept {
        return running_.load(std::memory_order_acquire) && session_running_.load(std::memory_order_acquire)
                   ? kFrameRequestWaitRunningMs
                   : kFrameRequestWaitIdleMs;
    }

    bool LatestFrameRequest(OpenXRFrameRequest& request) {
        std::lock_guard lock(request_mutex_);
        if (request_.serial == 0) {
            return false;
        }
        request = request_;
        return true;
    }

    void SetFrameInterpolationFps(uint32_t target) noexcept {
        frame_interpolation_fps_.store(NormalizeFrameInterpolationFps(target), std::memory_order_relaxed);
    }

    OpenXRFrameTiming FrameTiming() const noexcept {
        return {headset_hz_.load(std::memory_order_relaxed), rendered_fps_.load(std::memory_order_relaxed)};
    }

    bool FrameInterpolationAvailable() const noexcept {
        return interpolation_available_.load(std::memory_order_acquire);
    }

    void SetPassthrough(bool enabled) noexcept {
        passthrough_.store(enabled, std::memory_order_relaxed);
    }

    void SetLeanBackDegrees(float degrees) noexcept {
        lean_back_degrees_.store(
            std::clamp(degrees, -kVrLeanBackDegreesLimit,
                       kVrLeanBackDegreesLimit),
            std::memory_order_relaxed);
    }

    void SetRenderScale(float scale) noexcept {
        render_scale_.store(ClampRenderScale(scale), std::memory_order_relaxed);
    }

    void SetDisplayRefreshRate(float hz) noexcept {
        display_refresh_rate_.store(std::clamp(hz, 0.0f, 144.0f), std::memory_order_relaxed);
    }

    void ReapplyPerformanceLevel() noexcept { performance_level_dirty_.store(true, std::memory_order_release); }

    OpenXREyeResolution EyeResolution(float scale) const noexcept {
        OpenXREyeResolution resolution{};
        std::lock_guard lock(eye_view_mutex_);
        resolution.width = eye_width_.load(std::memory_order_relaxed);
        resolution.height = eye_height_.load(std::memory_order_relaxed);
        if (resolution.width != 0) {
            const OpenXREyeSize scaled = OpenXRScaledEyeSize(eye_view_, ClampRenderScale(scale));
            resolution.scaled_width = scaled.width;
            resolution.scaled_height = scaled.height;
        }
        return resolution;
    }

    void ServiceProducerFrameBoundary() noexcept {
        if (teardown_requested_.load(std::memory_order_acquire)) {
            Shutdown();
        }
    }

    std::string LastError() const {
        std::lock_guard lock(error_mutex_);
        return last_error_;
    }

private:
    struct PublishedFrame {
        AuroraStereoFrame frame{};
    };

#if defined(_WIN32)
    AuroraBackend kRequiredAuroraBackend = BACKEND_D3D12;
    const char* kGraphicsBackendName = "D3D12";
#else
    static constexpr AuroraBackend kRequiredAuroraBackend = BACKEND_VULKAN;
#endif
    // Skipped eye copies tolerated back to back before the session is given up: a few seconds
    // at the headset's refresh rate.
    static constexpr uint32_t kMaxConsecutiveSkips = 300;

    static float ClampRenderScale(float scale) noexcept {
        return std::clamp(scale, kVrRenderScaleMin, kVrRenderScaleMax);
    }

    // The size the eyes are rendered at now, for the settings: the pair being written, before the
    // immersive window may aim its eyes through the window.
    void NoteEyeSize(const OpenXRBackendFrame& frame) noexcept {
        eye_width_.store(frame.render_width[0], std::memory_order_relaxed);
        eye_height_.store(frame.render_height[0], std::memory_order_relaxed);
    }

    bool BackendMatchesConfiguredGraphicsApi(const AuroraConfig& aurora_config) {
#if defined(_WIN32)
        kRequiredAuroraBackend = BACKEND_D3D12;
        kGraphicsBackendName = "D3D12";
#endif
        if (aurora_config.desiredBackend == BACKEND_AUTO ||
            aurora_config.desiredBackend == kRequiredAuroraBackend) {
            return true;
        }
        SetError(std::string("OpenXR requires the ") + kGraphicsBackendName +
                 " graphics backend on this platform");
        return false;
    }

    void ApplyGraphicsRequirements(AuroraConfig& aurora_config) {
        aurora_config.desiredBackend = kRequiredAuroraBackend;
        aurora_config.xrInterop = true;
#if defined(__ANDROID__)
        // Foveated rendering: fragment density maps are decided with the device. They put a flag on
        // every render pipeline, so a session launched with foveation off does without them, and a
        // level chosen later applies at the next start. MP_FDM_DEVICE=0|1 forces the device's
        // choice for one run, to price the pipeline flag apart from the maps themselves.
        aurora_config.xrFragmentDensityMap = GetVrSettings().foveation != FoveationLevel::Off;
        if (const char* force = std::getenv("MP_FDM_DEVICE"); force != nullptr && *force != '\0') {
            aurora_config.xrFragmentDensityMap = *force == '1';
        }
#endif
#if defined(_WIN32)
        if (kRequiredAuroraBackend != BACKEND_D3D12) return;
        const auto& requirements = backend_->GraphicsRequirements();
        aurora_config.hasD3D12AdapterLuid = true;
        aurora_config.d3d12AdapterLuidLow = requirements.adapter_luid_low;
        aurora_config.d3d12AdapterLuidHigh = requirements.adapter_luid_high;
#endif
    }

    void ResetPreparedObjects() {
        diagnostics::SetDisplayTimeConverter({});
        ShutdownOrRetainGraphicsObjects();
        input_.reset();
        backend_.reset();
        runtime_.reset();
        prepared_ = false;
    }

    bool ShutdownOrRetainGraphicsObjects() noexcept {
        if (input_ != nullptr) {
            // Actions belong to the session and must go before it does.
            input_->Destroy();
            input_.reset();
        }
        if (backend_ != nullptr && !backend_->Shutdown()) {
            PORTVR_LOG()
                << "OpenXR " << kGraphicsBackendName
                << " queue completion is unknown; retaining the backend, "
                   "runtime, session, and graphics resources until process exit"
                << std::endl;
            (void)backend_.release();
            (void)runtime_.release();
            graphics_retained_ = true;
            return false;
        }
        if (runtime_ != nullptr) {
            runtime_->Shutdown();
        }
        return true;
    }

#if defined(__ANDROID__)
    // Aurora's frame worker publishes its native thread id once it runs; until then there is
    // nothing to hint. The hint itself may be refused by the runtime, which is only logged.
    bool RegisterAuroraFrameWorkerThread() {
        const uint32_t thread_id = aurora_get_frame_worker_native_thread_id();
        if (thread_id == 0 || runtime_ == nullptr) {
            return false;
        }
        const bool hinted = OpenXRAndroidRegisterThreadId(*runtime_, OpenXRAndroidThreadType::RendererMain, thread_id);
        PORTVR_LOG() << "OpenXR: Android thread hint for Aurora's frame worker "
                               << (hinted ? "set" : "refused") << std::endl;
        return true;
    }

    // Aurora's GX FIFO processor turns the game's GX commands into recorded draws on a thread
    // of its own, Wiicompiled's GX thread in this lineage; Wiicompiled went from 52-56 to 60
    // FPS on a Quest 3 once that thread had a renderer hint.
    bool RegisterAuroraGxWorkerThread() {
        const uint32_t thread_id = aurora_get_gx_worker_native_thread_id();
        if (thread_id == 0 || runtime_ == nullptr) {
            return false;
        }
        const bool hinted =
            OpenXRAndroidRegisterThreadId(*runtime_, OpenXRAndroidThreadType::RendererWorker, thread_id);
        PORTVR_LOG() << "OpenXR: Android thread hint for Aurora's GX processor "
                               << (hinted ? "set" : "refused") << std::endl;
        return true;
    }
#endif

    // Asks the runtime for the configured performance level in both domains. Standalone
    // headsets clock their cores by this: a Quest 3 held the game thread at CPU level 4
    // (2.2 GHz of a possible 2.36) and the GPU at level 3 with the runtime's own choice. A
    // refusal is logged and changes nothing; desktop runtimes rarely offer the extension.
    void ApplyPerformanceLevel() {
        if (runtime_ == nullptr || set_performance_level_ == nullptr || !runtime_->HasSession()) {
            return;
        }
        const std::string requested = GetVrSettings().performance_level;
        XrPerfSettingsLevelEXT level = XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT;
        if (requested == "default") {
            return;
        } else if (requested == "power_savings") {
            level = XR_PERF_SETTINGS_LEVEL_POWER_SAVINGS_EXT;
        } else if (requested == "sustained_low") {
            level = XR_PERF_SETTINGS_LEVEL_SUSTAINED_LOW_EXT;
        } else if (requested == "boost") {
            level = XR_PERF_SETTINGS_LEVEL_BOOST_EXT;
        }
        const XrResult cpu = set_performance_level_(runtime_->Session(), XR_PERF_SETTINGS_DOMAIN_CPU_EXT, level);
        const XrResult gpu = set_performance_level_(runtime_->Session(), XR_PERF_SETTINGS_DOMAIN_GPU_EXT, level);
        PORTVR_LOG() << "OpenXR: performance level \"" << requested << "\" CPU "
                               << (XR_SUCCEEDED(cpu) ? "set" : "refused") << " (" << cpu << "), GPU "
                               << (XR_SUCCEEDED(gpu) ? "set" : "refused") << " (" << gpu << ")" << std::endl;
    }

    // XR_FB_display_refresh_rate. The game's 60 Hz simulation is drawn once per XR
    // frame with interpolated presentation, so any rate the headset offers moves
    // smoothly; each costs one draw of the game and both eyes per display frame.
    // 0 leaves the runtime's choice, 72 Hz on a Quest unless its system says otherwise.
    void ApplyDisplayRefreshRate() {
        const float wanted = display_refresh_rate_.load(std::memory_order_relaxed);
        applied_display_refresh_rate_ = wanted;
        if (runtime_ == nullptr || !runtime_->HasSession() || request_display_refresh_rate_ == nullptr) {
            return;
        }
        if (!(wanted > 0.0f)) {
            if (display_refresh_rate_requested_) {
                display_refresh_rate_requested_ = false;
                const XrResult result = request_display_refresh_rate_(runtime_->Session(), 0.0f);
                PORTVR_LOG() << "OpenXR: display refresh rate left to the runtime (" << result << ")"
                             << std::endl;
            }
            return;
        }
        std::vector<float> rates;
        if (enumerate_display_refresh_rates_ != nullptr) {
            uint32_t count = 0;
            if (XR_SUCCEEDED(enumerate_display_refresh_rates_(runtime_->Session(), 0, &count, nullptr)) &&
                count != 0) {
                rates.resize(count);
                if (XR_SUCCEEDED(enumerate_display_refresh_rates_(runtime_->Session(), count, &count,
                                                                   rates.data()))) {
                    rates.resize(count);
                } else {
                    rates.clear();
                }
            }
        }
        float chosen = wanted;
        if (!rates.empty()) {
            chosen = rates.front();
            for (const float rate : rates) {
                if (std::fabs(rate - wanted) < std::fabs(chosen - wanted)) {
                    chosen = rate;
                }
            }
        }
        const XrResult result = request_display_refresh_rate_(runtime_->Session(), chosen);
        display_refresh_rate_requested_ = XR_SUCCEEDED(result);
        std::ostringstream offered;
        for (size_t i = 0; i < rates.size(); ++i) {
            offered << (i == 0 ? "" : ", ") << rates[i];
        }
        PORTVR_LOG() << "OpenXR: display refresh rate " << chosen << " Hz (asked for " << wanted
                     << ", offered " << (rates.empty() ? std::string("unknown") : offered.str()) << ") "
                     << (XR_SUCCEEDED(result) ? "requested" : "refused") << " (" << result << ")"
                     << std::endl;
    }

    static bool ProvideStereoFrame(uint32_t, AuroraStereoFrame* output, void* userdata) {
        auto* self = static_cast<OpenXRIntegration*>(userdata);
        if (self == nullptr || output == nullptr) {
            return false;
        }
        // The packet storage is reused by the XR thread. Claim and copy it
        // under one short lock so cancellation cannot begin the next packet
        // while this callback is preempted between exchange and copy.
        std::lock_guard lock(self->published_mutex_);
        PublishedFrame* frame = self->published_.exchange(nullptr, std::memory_order_acq_rel);
        if (frame == nullptr) {
            return false;
        }
        diagnostics::NotePacketConsumed();
        *output = frame->frame;
        {
            std::lock_guard pickup(self->pickup_mutex_);
            self->picked_up_token_ = frame->frame.frameToken;
        }
        self->pickup_cv_.notify_all();
        return true;
    }

    void PacingThread() noexcept {
#if defined(__ANDROID__)
        // The runtime schedules hinted threads onto the fast cores. The game thread and Aurora's
        // frame worker, which submits the GPU work, are the ones that matter; this thread only
        // paces.
        bool worker_registered = false;
        bool gx_worker_registered = false;
        if (runtime_ != nullptr) {
            const bool pacing_hinted =
                OpenXRAndroidRegisterThread(*runtime_, OpenXRAndroidThreadType::RendererWorker);
            bool game_hinted = false;
            if (game_thread_id_ != 0) {
                game_hinted = OpenXRAndroidRegisterThreadId(*runtime_, OpenXRAndroidThreadType::ApplicationMain,
                                                            game_thread_id_);
            }
            PORTVR_LOG() << "OpenXR: Android thread hints: game " << (game_hinted ? "set" : "refused")
                                   << ", pacing " << (pacing_hinted ? "set" : "refused") << std::endl;
            worker_registered = RegisterAuroraFrameWorkerThread();
            gx_worker_registered = RegisterAuroraGxWorkerThread();
        }
#endif
        ApplyPerformanceLevel();
        exit_requested_.store(false, std::memory_order_release);
        bool fatal = false;
        uint32_t consecutive_skips = 0;
        bool store_gate_set = false;
        bool store_gate_racing = false;
        bool presentation_logged = false;
        VRPresentationMode logged_presentation = VRPresentationMode::Desktop;
        uint32_t presentation_log_count = 0;
        bool immersive_submission_logged = false;
        int last_pacing_mode = -1;
        while (!stop_.load(std::memory_order_acquire) && !fatal) {
#if defined(__ANDROID__)
            if (!worker_registered) {
                worker_registered = RegisterAuroraFrameWorkerThread();
            }
            if (!gx_worker_registered) {
                gx_worker_registered = RegisterAuroraGxWorkerThread();
            }
#endif
            const OpenXREventStatus events = diagnostics::Measure(diagnostics::Stage::PollEvents, [&] {
                return runtime_->PollEvents();
            });
            const bool session_active = runtime_->IsSessionRunning();
            session_running_.store(session_active, std::memory_order_release);
            PrimeVRPolicySetSessionActive(session_active);
            const uint64_t session_run_serial = runtime_->SessionRunSerial();
            if (session_run_serial != applied_session_run_serial_) {
                applied_session_run_serial_ = session_run_serial;
                ResetTrackingOrigin();
                diagnostics::OnSessionStarted();
                // A runtime may drop the request when the session stops (the headset
                // asleep, the system menu); every new session asks again.
                ApplyPerformanceLevel();
                applied_display_refresh_rate_ = -1.0f;
            }
            if (session_active &&
                display_refresh_rate_.load(std::memory_order_relaxed) != applied_display_refresh_rate_) {
                ApplyDisplayRefreshRate();
            }
            if (session_active && performance_level_dirty_.exchange(false, std::memory_order_acq_rel)) {
                ApplyPerformanceLevel();
            }
            if (session_active != session_was_active_) {
                session_was_active_ = session_active;
                if (!session_active) {
                    ResetTrackingOrigin();
                    // Nothing is displayed while the session is not running (the system menu,
                    // the headset taken off), so the stall of a cache store is invisible here.
                    // Waiting for it is deliberate: the process may be ended next.
                    aurora_store_pipeline_caches();
                }
            }
            if (events == OpenXREventStatus::ExitRequested) {
                OnRuntimeExitRequested();
                break;
            }
            if (events == OpenXREventStatus::Error) {
                SetError("OpenXR event processing failed: " + runtime_->LastError().message);
                break;
            }
            if (!session_active) {
                if (input_ != nullptr) {
                    input_->Idle();
                }
                SetInterpolationActive(false);
                interpolation_pacing_.Reset();
                rendered_fps_.store(0, std::memory_order_relaxed);
                WaitForStopOrDelay(std::chrono::milliseconds(5));
                continue;
            }

            const PrimeVRPolicySnapshot policy = PrimeVRPolicyGetSnapshot();
            aurora_set_stereo_motion_logging(diagnostics::Enabled());
            // Diagnostics lift the cap: a presentation flickering between the
            // race and the virtual screen is exactly what a report needs to show.
            if ((!presentation_logged || policy.presentation != logged_presentation) &&
                (presentation_log_count < 16 || diagnostics::Enabled())) {
                presentation_logged = true;
                logged_presentation = policy.presentation;
                ++presentation_log_count;
                PORTVR_LOG()
                    << "[vr] presentation="
                    << (policy.presentation == VRPresentationMode::Immersive
                            ? "immersive"
                            : policy.presentation == VRPresentationMode::VirtualScreen
                                  ? "virtual-screen"
                                  : "desktop")
                    << ", game-mode=" << static_cast<unsigned>(policy.game_mode)
                    << ", game-frame=" << policy.game_frame << std::endl;
            }
            OpenXRPresentation presentation{};
            const bool immersive = policy.presentation == VRPresentationMode::Immersive;
            presentation.mode = immersive ? OpenXRFrameMode::ImmersiveProjection
                                           : OpenXRFrameMode::VirtualScreen;
            presentation.quad_distance_meters = policy.config.screen_distance_meters;
            presentation.quad_width_meters = policy.config.screen_width_meters;
            if (float picture_aspect = 0.0f, snapshot_aspect = 0.0f;
                aurora_get_stereo_screen_aspects(&picture_aspect, &snapshot_aspect)) {
                presentation.quad_content_aspect = snapshot_aspect;
            }
            // The immersive window: the race's stereo view seen through its 2D layer's screen.
            // The flag travels with the packet, so the eyes Aurora masks and the layer the
            // backend blends always belong to the same frame.
            presentation.immersive_window = false;
            // The room around the menu screen and every other virtual screen, a Flat Screen
            // race included, and around the immersive window; a fully immersive race is
            // virtual all round, and the cameras are paused for it.
            presentation.passthrough =
                !immersive && passthrough_.load(std::memory_order_relaxed);
            // PrimedGun's VR menu gets compositor layers of its own while it is
            // open. A backend that could not make them cannot show it, and the
            // menu then never opens. While it is closed, the shader compilation
            // toast borrows the first.
            const bool panel_layer = backend_->PanelLayerAvailable();
            aurora_set_stereo_panel_layer(panel_layer);
            presentation.panel.requested =
                panel_layer && (OpenXRSettingsPanelOpen() || OpenXRCompileToastShown());

            // Pipeline caches are stored where their stall is least visible: once when a race
            // ends, and by the compiler itself while the headset shows the virtual screen. Never
            // mid-race, and a race on the virtual screen (Flat Screen mode) is still a race. The
            // race-exit store runs on Aurora's thread: this one keeps submitting frames while
            // Dawn holds its device to serialize, which is still a brief game stall.
            const bool racing = immersive;
            if (!store_gate_set || racing != store_gate_racing) {
                const bool left_race = store_gate_set && store_gate_racing && !racing;
                store_gate_set = true;
                store_gate_racing = racing;
                aurora_set_pipeline_cache_idle_store(!racing);
                if (left_race) {
                    aurora_request_pipeline_cache_store();
                }
            }

            // Updating this on the owner thread also confines retained replay to
            // validated race content. The provider checks policy tags again.
            const uint32_t interpolation_target = frame_interpolation_fps_.load(std::memory_order_relaxed);
            SetInterpolationActive(immersive && FrameInterpolationAvailable() && interpolation_target != 0);

            // With interpolation off, the eyes are rendered before
            // the compositor frame that shows them is begun, so that frame never waits for a
            // game frame. Interpolation keeps the frame-first order below: it renders for the
            // frame's own predicted display time.
            const bool render_first = !aurora_get_stereo_frame_interpolation();
            // Pipelined render-first pacing (vr_pipelined_rendering, PipelinedCycle), on a
            // backend that supports it. A new render scale (the eyes are rebuilt with no packet
            // pending), the setting going off or a switch to frame-first pacing first drains
            // the packet in flight with one plain cycle; a new scale then keeps the cycles
            // plain until both swapchain pairs have it (one is rebuilt at a time).
            const float scale = render_scale_.load(std::memory_order_relaxed);
            const bool pipelined = render_first && GetVrSettings().pipelined_rendering &&
                                   backend_->SupportsPipelining() && scale == pipelined_scale_ &&
                                   !backend_->EyesResizing();
            pipelined_scale_ = scale;
            const int pacing_mode = !render_first ? 0 : pipelined ? 2 : 1;
            if (last_pacing_mode != pacing_mode) {
                last_pacing_mode = pacing_mode;
                PORTVR_LOG() << "OpenXR " << kGraphicsBackendName << " pacing: "
                    << (pacing_mode == 0 ? "frame-first (VR interpolation)"
                        : pacing_mode == 2 ? "render-first, pipelined" : "render-first") << std::endl;
            }
            // A new scale rebuilds the eyes as the backend next prepares them.
            backend_->SetRenderScale(scale);
            if (pipelined || pipelined_packet_) {
                // With a packet in flight and the mode changed, this cycle only finishes it.
                if (!PipelinedCycle(presentation, policy, immersive, consecutive_skips,
                                    immersive_submission_logged, pipelined)) {
                    fatal = true;
                }
                continue;
            }
            if (render_first) {
                if (!RenderFirstCycle(presentation, policy, immersive, consecutive_skips,
                                      immersive_submission_logged)) {
                    fatal = true;
                }
                continue;
            }
            OpenXRBackendFrame frame{};
            const OpenXRBeginStatus begin = backend_->BeginFrame(presentation, frame);
            if (begin == OpenXRBeginStatus::SessionNotRunning) {
                PrimeVRPolicySetSessionActive(false);
                continue;
            }
            if (begin == OpenXRBeginStatus::ExitRequested) {
                OnRuntimeExitRequested();
                break;
            }
            if (begin == OpenXRBeginStatus::Error) {
                SetError(backend_->LastError());
                fatal = true;
                break;
            }
            NoteEyeSize(frame);

            UpdateFrameTiming(frame.xr_frame);
            if (diagnostics::Enabled()) {
                NoteFrameDiagnostics(frame, immersive);
            }
            // Both of these read this frame's located head pose and must run
            // before FinishFrame submits a layer built from it.
            ServiceRecenterRequest();
            UpdateVirtualScreenPose(frame, policy);
            // After the screen is placed, so the pointer aims at this frame's
            // screen rather than the previous one's.
            const OpenXRPointerScreen screen = PointerScreen(frame, policy, immersive);
            if (input_ != nullptr) {
                const diagnostics::ScopedStage input_timer(diagnostics::Stage::InputSync);
                input_->Sync(frame.xr_frame.predicted_display_time, screen, panel_layer);
            }
            PlacePanelLayer(frame, screen);

            if (!frame.expects_gpu_submission) {
                if (!backend_->FinishFrame(frame, false)) {
                    SetError(backend_->LastError());
                    fatal = true;
                }
                continue;
            }

            if (aurora_get_stereo_frame_interpolation() &&
                !interpolation_pacing_.ShouldRender(frame.xr_frame.predicted_display_time, interpolation_target)) {
                diagnostics::OnInterpolationSkip();
                if (!diagnostics::Measure(diagnostics::Stage::Cancel, [&] {
                    return backend_->TryCancelPendingFrame(frame);
                }) || !backend_->FinishFrame(frame, false)) {
                    SetError(backend_->LastError());
                    fatal = true;
                }
                continue;
            }

            {
                const diagnostics::ScopedStage publish_timer(diagnostics::Stage::Publish);
                std::lock_guard lock(published_mutex_);
                // First person renders at life-size scale, third person at the
                // configured diorama scale. Head translation and IPD are the
                // only things this multiplies, so a one-frame disagreement with
                // the camera's own switch is not observable.
                BuildPublishedFrame(frame, immersive, policy);
                PublishFrameRequest(frame, immersive, policy);
                diagnostics::OnPacketPublished();
                published_.store(&published_frame_, std::memory_order_release);
            }
            aurora_notify_stereo_frame();

            OpenXRSubmissionStatus submission = OpenXRSubmissionStatus::Timeout;
            bool canceled_before_encode = false;
            const auto cancel_after =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
            while (!stop_.load(std::memory_order_acquire) &&
                   submission == OpenXRSubmissionStatus::Timeout) {
                // Fresh rendering wakes us immediately. A 50 ms keep-alive
                // protects stalls without issuing eager repeats during GPU work.
                submission = diagnostics::Measure(diagnostics::Stage::SubmissionWait, [&] {
                    return backend_->WaitForSubmission(frame, 50);
                });
                if (submission == OpenXRSubmissionStatus::Timeout) {
                    // A pause, minimized window, or guest stall may leave no GX
                    // frame to consume this packet. Withdraw it, then cancel the
                    // matching bridge target only if Encode has not taken ownership.
                    if (std::chrono::steady_clock::now() >= cancel_after) {
                        diagnostics::Measure(diagnostics::Stage::Withdraw, [&] { WithdrawPublishedFrame(); });
                        canceled_before_encode = diagnostics::Measure(diagnostics::Stage::Cancel, [&] {
                            return backend_->TryCancelPendingFrame(frame);
                        });
                        if (canceled_before_encode) {
                            diagnostics::OnPacketCanceled();
                            break;
                        }
                    }
                    diagnostics::OnKeepaliveRepeat();
                    if (!backend_->RepeatFrame(frame)) {
                        SetError(backend_->LastError());
                        fatal = true;
                        break;
                    }
                }
            }
            diagnostics::Measure(diagnostics::Stage::Withdraw, [&] { WithdrawPublishedFrame(); });
            if (stop_.load(std::memory_order_acquire)) {
                // Aurora has been drained by Shutdown(); backend shutdown below
                // cancels its pending target, then either safely releases the
                // XR image or retains the entire graph if GPU completion is unknown.
                break;
            }
            if (canceled_before_encode) {
                if (!backend_->FinishFrame(frame, false)) {
                    SetError(backend_->LastError());
                    fatal = true;
                }
                continue;
            }
            if (fatal) {
                break;
            }
            const bool submit = submission == OpenXRSubmissionStatus::Success;
            diagnostics::OnSubmission(submit);
            if (!backend_->FinishFrame(frame, submit)) {
                SetError(backend_->LastError());
                fatal = true;
            } else if (submission == OpenXRSubmissionStatus::Skipped) {
                // No GPU work touched the compositor image or the shared buffers, so the frame
                // ended on the retained layer and the next one is tried normally. A long run of
                // skips means the copy path is broken for good.
                ++consecutive_skips;
                if (consecutive_skips == 1 || consecutive_skips % 60 == 0) {
                    PORTVR_LOG() << "OpenXR: eye copy skipped (" << consecutive_skips
                                           << " in a row): " << backend_->LastError() << std::endl;
                }
                if (consecutive_skips >= kMaxConsecutiveSkips) {
                    SetError(std::string("Aurora's ") + kGraphicsBackendName +
                             " stereo copy keeps failing" + kFallbackNote);
                    fatal = true;
                }
            } else if (!submit) {
                SetError(std::string("Aurora's ") + kGraphicsBackendName +
                         " stereo copy failed" + kFallbackNote);
                fatal = true;
            } else {
                consecutive_skips = 0;
                ++timing_submissions_;
            }
            if (submit && !fatal && immersive && !immersive_submission_logged) {
                immersive_submission_logged = true;
                PORTVR_LOG()
                    << "[vr] first immersive packet consumed and submitted as "
                       "an OpenXR projection layer"
                    << std::endl;
            }
        }

        SetInterpolationActive(false);
        aurora_set_pipeline_cache_idle_store(false);
        aurora_set_stereo_panel_layer(false);
        running_.store(false, std::memory_order_release);
        PrimeVRPolicySetSessionActive(false);
#if defined(__ANDROID__)
        // A standalone headset has no desktop to continue on: whatever ended the
        // session ends the app, and the launcher shows an error if there was one.
        if (!stop_.load(std::memory_order_acquire)) {
            OpenXRAndroidRequestQuit(exit_requested_.load(std::memory_order_acquire) ? std::string()
                                                                                      : LastError());
        }
#endif
        if (!stop_.load(std::memory_order_acquire)) {
            // A runtime/backend failure can happen while Aurora is submitting.
            // Ask the producer to reach a safe frame boundary, drain Aurora,
            // and unregister the provider before this XR owner destroys state.
            teardown_requested_.store(true, std::memory_order_release);
            std::unique_lock lock(stop_mutex_);
            stop_cv_.wait(lock, [this] { return stop_.load(std::memory_order_acquire); });
        }
        ShutdownOrRetainGraphicsObjects();
    }

    // One compositor cycle on the retained layer, with no frame left active. False on a fatal
    // backend or runtime failure (the error is recorded).
    bool KeepAlive() {
        const OpenXRBeginStatus status = backend_->KeepAliveCycle();
        if (status == OpenXRBeginStatus::SessionNotRunning) {
            PrimeVRPolicySetSessionActive(false);
            return true;
        }
        if (status == OpenXRBeginStatus::ExitRequested) {
            OnRuntimeExitRequested();
            return false;
        }
        if (status == OpenXRBeginStatus::Error) {
            SetError(backend_->LastError());
            return false;
        }
        return true;
    }

    // Render-first pacing (see each backend's PreparePacket). Returns false on a fatal
    // failure; a cycle that ends without a layer returns true and the loop tries again.
    bool RenderFirstCycle(OpenXRPresentation presentation, const PrimeVRPolicySnapshot& policy, bool immersive,
                          uint32_t& consecutive_skips, bool& immersive_submission_logged) {
        OpenXRBackendFrame packet{};
        const OpenXRBeginStatus prepared = backend_->PreparePacket(presentation, packet);
        if (prepared == OpenXRBeginStatus::SessionNotRunning) {
            PrimeVRPolicySetSessionActive(false);
            return true;
        }
        if (prepared == OpenXRBeginStatus::ExitRequested) {
            OnRuntimeExitRequested();
            return false;
        }
        if (prepared == OpenXRBeginStatus::Error) {
            SetError(backend_->LastError());
            return false;
        }
        NoteEyeSize(packet);
        // The head pose this packet was located with places the screens and aims the pointer.
        ServiceRecenterRequest();
        UpdateVirtualScreenPose(packet, policy);
        const OpenXRPointerScreen screen = PointerScreen(packet, policy, immersive);
        if (input_ != nullptr) {
            const diagnostics::ScopedStage input_timer(diagnostics::Stage::InputSync);
            input_->Sync(packet.xr_frame.predicted_display_time, screen, backend_->PanelLayerAvailable());
        }
        PlacePanelLayer(packet, screen);
        if (!packet.expects_gpu_submission) {
            // Nothing to render (no rendering requested or no tracking): keep the compositor fed.
            return KeepAlive();
        }
        {
            const diagnostics::ScopedStage publish_timer(diagnostics::Stage::Publish);
            std::lock_guard lock(published_mutex_);
            BuildPublishedFrame(packet, immersive, policy);
            PublishFrameRequest(packet, immersive, policy);
            diagnostics::OnPacketPublished();
            published_.store(&published_frame_, std::memory_order_release);
        }
        aurora_notify_stereo_frame();

        // Aurora renders the eyes at its next seal. Meanwhile the compositor keeps showing the
        // retained layer; a 50 ms stall repeats it explicitly and withdraws the packet.
        OpenXRSubmissionStatus submission = OpenXRSubmissionStatus::Timeout;
        bool canceled_before_encode = false;
        const auto cancel_after = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
        while (!stop_.load(std::memory_order_acquire) && submission == OpenXRSubmissionStatus::Timeout) {
            submission = diagnostics::Measure(diagnostics::Stage::SubmissionWait, [&] {
                return backend_->WaitForSubmission(packet, 50);
            });
            if (submission == OpenXRSubmissionStatus::Timeout) {
                if (std::chrono::steady_clock::now() >= cancel_after) {
                    diagnostics::Measure(diagnostics::Stage::Withdraw, [&] { WithdrawPublishedFrame(); });
                    canceled_before_encode = diagnostics::Measure(diagnostics::Stage::Cancel, [&] {
                        return backend_->TryCancelPendingPacket(packet);
                    });
                    if (canceled_before_encode) {
                        diagnostics::OnPacketCanceled();
                        break;
                    }
                }
                diagnostics::OnKeepaliveRepeat();
                if (!KeepAlive()) {
                    return false;
                }
            }
        }
        diagnostics::Measure(diagnostics::Stage::Withdraw, [&] { WithdrawPublishedFrame(); });
        if (stop_.load(std::memory_order_acquire) ||
            submission == OpenXRSubmissionStatus::ShuttingDown) {
            return true;
        }
        if (canceled_before_encode) {
            // Refresh display timing after a canceled packet as well, so the
            // next estimate cannot remain stuck in the past during a game stall.
            return KeepAlive();
        }
        if (submission != OpenXRSubmissionStatus::Success) {
            // Nothing reached the shared buffers (Skipped) or Aurora failed after queuing GPU
            // work (Failed): same accounting as the frame-first path, on a keep-alive cycle.
            diagnostics::OnSubmission(false);
            if (submission == OpenXRSubmissionStatus::Failed) {
                SetError(std::string("Aurora's ") + kGraphicsBackendName +
                         " stereo copy failed" + kFallbackNote);
                return false;
            }
            ++consecutive_skips;
            if (consecutive_skips == 1 || consecutive_skips % 60 == 0) {
                PORTVR_LOG() << "OpenXR: eye copy skipped (" << consecutive_skips
                                       << " in a row): " << backend_->LastError() << std::endl;
            }
            if (consecutive_skips >= kMaxConsecutiveSkips) {
                SetError(std::string("Aurora's ") + kGraphicsBackendName +
                         " stereo copy keeps failing" + kFallbackNote);
                return false;
            }
            return KeepAlive();
        }

        // The eyes are rendered: begin the compositor frame, complete the backend copy, end.
        OpenXRBackendFrame frame{};
        const OpenXRBeginStatus begin = backend_->BeginFrameForPacket(packet, frame);
        if (begin == OpenXRBeginStatus::SessionNotRunning) {
            PrimeVRPolicySetSessionActive(false);
            return true;
        }
        if (begin == OpenXRBeginStatus::ExitRequested) {
            OnRuntimeExitRequested();
            return false;
        }
        if (begin == OpenXRBeginStatus::Error) {
            SetError(backend_->LastError());
            return false;
        }
        UpdateFrameTiming(frame.xr_frame);
        if (diagnostics::Enabled()) {
            NoteFrameDiagnostics(frame, immersive);
        }
        const OpenXRSubmissionStatus copy =
            frame.expects_gpu_submission ? backend_->CopyRenderedEyes(frame) : OpenXRSubmissionStatus::Skipped;
        const bool submit = copy == OpenXRSubmissionStatus::Success;
        diagnostics::OnSubmission(submit);
        if (!backend_->FinishFrame(frame, submit)) {
            SetError(backend_->LastError());
            return false;
        }
        if (copy == OpenXRSubmissionStatus::Failed) {
            SetError(std::string("Aurora's ") + kGraphicsBackendName +
                     " stereo copy failed" + kFallbackNote);
            return false;
        }
        if (!submit) {
            ++consecutive_skips;
            if (consecutive_skips == 1 || consecutive_skips % 60 == 0) {
                PORTVR_LOG() << "OpenXR: eye copy skipped (" << consecutive_skips
                                       << " in a row): " << backend_->LastError() << std::endl;
            }
            return consecutive_skips < kMaxConsecutiveSkips;
        }
        consecutive_skips = 0;
        ++timing_submissions_;
        if (immersive && !immersive_submission_logged) {
            immersive_submission_logged = true;
            PORTVR_LOG() << "[vr] first immersive packet consumed and submitted as "
                                      "an OpenXR projection layer"
                                   << std::endl;
        }
        return true;
    }


    // The packet's poses to the game: its screens and pointer, its input, its frame request.
    void PublishPacket(OpenXRBackendFrame& packet, const PrimeVRPolicySnapshot& policy, bool immersive) {
        NoteEyeSize(packet);
        // The head pose this packet was located with places the screens and aims the pointer.
        ServiceRecenterRequest();
        UpdateVirtualScreenPose(packet, policy);
        const OpenXRPointerScreen screen = PointerScreen(packet, policy, immersive);
        if (input_ != nullptr) {
            const diagnostics::ScopedStage input_timer(diagnostics::Stage::InputSync);
            input_->Sync(packet.xr_frame.predicted_display_time, screen, backend_->PanelLayerAvailable());
        }
        PlacePanelLayer(packet, screen);
        {
            const diagnostics::ScopedStage publish_timer(diagnostics::Stage::Publish);
            std::lock_guard lock(published_mutex_);
            BuildPublishedFrame(packet, immersive, policy);
            PublishFrameRequest(packet, immersive, policy);
            diagnostics::OnPacketPublished();
            published_.store(&published_frame_, std::memory_order_release);
        }
        aurora_notify_stereo_frame();
    }

    // Whether the game picked up the packet `token` (ProvideStereoFrame) within `timeout_ms`.
    bool WaitForPickup(uint64_t token, uint32_t timeout_ms) {
        std::unique_lock lock(pickup_mutex_);
        pickup_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                            [&] { return picked_up_token_ >= token || stop_.load(std::memory_order_acquire); });
        return picked_up_token_ >= token;
    }

    // Render-first pacing with one packet of overlap (vr_pipelined_rendering): the next
    // packet's poses are located and published as soon as the game has picked this one
    // up, so the game records N+1 while Aurora encodes N; N+1 gets its images once N's
    // eyes are submitted, before N's compositor frame. One frame of latency more, hidden
    // in part by locating N+1 one display period further ahead. `continue_pipeline` false
    // ends the pipeline after this packet (a drain). Returns false on a fatal failure.
    bool PipelinedCycle(OpenXRPresentation presentation, const PrimeVRPolicySnapshot& policy, bool immersive,
                        uint32_t& consecutive_skips, bool& immersive_submission_logged, bool continue_pipeline) {
        if (!pipelined_packet_) {
            // The pipeline's first packet, prepared whole.
            OpenXRBackendFrame packet{};
            const OpenXRBeginStatus prepared = backend_->PreparePacket(presentation, packet);
            if (prepared == OpenXRBeginStatus::SessionNotRunning) {
                PrimeVRPolicySetSessionActive(false);
                return true;
            }
            if (prepared == OpenXRBeginStatus::ExitRequested) {
                OnRuntimeExitRequested();
                return false;
            }
            if (prepared == OpenXRBeginStatus::Error) {
                SetError(backend_->LastError());
                return false;
            }
            if (!packet.expects_gpu_submission) {
                // Nothing to render (no rendering requested or no tracking): keep the compositor fed.
                NoteEyeSize(packet);
                return KeepAlive();
            }
            PublishPacket(packet, policy, immersive);
            pipelined_packet_ = packet;
            return true;
        }
        OpenXRBackendFrame packet = *pipelined_packet_;
        pipelined_packet_.reset();

        // The next packet, once the game has this one: located a period further ahead and
        // published, not yet armed (Aurora holds one packet's targets at a time).
        std::optional<OpenXRBackendFrame> next;
        if (continue_pipeline && WaitForPickup(packet.xr_frame.serial, kPickupWaitMs)) {
            OpenXRBackendFrame candidate{};
            const OpenXRBeginStatus located = backend_->LocatePacket(presentation, candidate, 3);
            if (located == OpenXRBeginStatus::ExitRequested) {
                OnRuntimeExitRequested();
                return false;
            }
            if (located == OpenXRBeginStatus::Error) {
                SetError(backend_->LastError());
                return false;
            }
            // A stopped session or nothing to render ends the pipeline with this packet.
            if (located == OpenXRBeginStatus::Ready && candidate.xr_frame.should_render &&
                candidate.xr_frame.views_valid) {
                PublishPacket(candidate, policy, immersive);
                next = candidate;
            }
        }
        // Whatever ends this cycle early takes the next packet's request back with it; eyes
        // the game already renders for it find no targets and are dropped once.
        const auto drop_next = [&] {
            if (next) {
                diagnostics::Measure(diagnostics::Stage::Withdraw, [&] { WithdrawPublishedFrame(); });
                next.reset();
            }
        };

        // This packet's eyes: as RenderFirstCycle, a 50 ms stall repeats the retained layer and
        // withdraws the packet.
        OpenXRSubmissionStatus submission = OpenXRSubmissionStatus::Timeout;
        bool canceled_before_encode = false;
        const auto cancel_after = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
        while (!stop_.load(std::memory_order_acquire) && submission == OpenXRSubmissionStatus::Timeout) {
            submission = diagnostics::Measure(diagnostics::Stage::SubmissionWait, [&] {
                return backend_->WaitForSubmission(packet, 50);
            });
            if (submission == OpenXRSubmissionStatus::Timeout) {
                if (std::chrono::steady_clock::now() >= cancel_after) {
                    drop_next();
                    diagnostics::Measure(diagnostics::Stage::Withdraw, [&] { WithdrawPublishedFrame(); });
                    canceled_before_encode = diagnostics::Measure(diagnostics::Stage::Cancel, [&] {
                        return backend_->TryCancelPendingPacket(packet);
                    });
                    if (canceled_before_encode) {
                        diagnostics::OnPacketCanceled();
                        break;
                    }
                }
                diagnostics::OnKeepaliveRepeat();
                if (!KeepAlive()) {
                    return false;
                }
            }
        }
        if (stop_.load(std::memory_order_acquire) || submission == OpenXRSubmissionStatus::ShuttingDown) {
            return true;
        }
        if (canceled_before_encode) {
            return KeepAlive();
        }
        if (submission != OpenXRSubmissionStatus::Success) {
            drop_next();
            diagnostics::OnSubmission(false);
            if (submission == OpenXRSubmissionStatus::Failed) {
                SetError(std::string("Aurora's ") + kGraphicsBackendName + " stereo copy failed" + kFallbackNote);
                return false;
            }
            ++consecutive_skips;
            if (consecutive_skips == 1 || consecutive_skips % 60 == 0) {
                PORTVR_LOG() << "OpenXR: eye copy skipped (" << consecutive_skips
                             << " in a row): " << backend_->LastError() << std::endl;
            }
            if (consecutive_skips >= kMaxConsecutiveSkips) {
                SetError(std::string("Aurora's ") + kGraphicsBackendName + " stereo copy keeps failing" +
                         kFallbackNote);
                return false;
            }
            return KeepAlive();
        }

        // The next packet's images and targets, before this packet's compositor frame.
        if (next) {
            const OpenXRBeginStatus armed = backend_->ArmPacket(*next);
            if (armed != OpenXRBeginStatus::Ready) {
                if (armed == OpenXRBeginStatus::Error) {
                    PORTVR_LOG() << "OpenXR: pipelined packet not armed: " << backend_->LastError() << std::endl;
                }
                drop_next();
            }
        }

        // This packet's compositor frame: begin, confirm the copy, end (which promotes the next).
        OpenXRBackendFrame frame{};
        const OpenXRBeginStatus begin = backend_->BeginFrameForPacket(packet, frame);
        if (begin == OpenXRBeginStatus::SessionNotRunning) {
            PrimeVRPolicySetSessionActive(false);
            drop_next();
            return true;
        }
        if (begin == OpenXRBeginStatus::ExitRequested) {
            OnRuntimeExitRequested();
            return false;
        }
        if (begin == OpenXRBeginStatus::Error) {
            SetError(backend_->LastError());
            return false;
        }
        UpdateFrameTiming(frame.xr_frame);
        if (diagnostics::Enabled()) {
            NoteFrameDiagnostics(frame, immersive);
        }
        const OpenXRSubmissionStatus copy =
            frame.expects_gpu_submission ? backend_->CopyRenderedEyes(frame) : OpenXRSubmissionStatus::Skipped;
        const bool submit = copy == OpenXRSubmissionStatus::Success;
        diagnostics::OnSubmission(submit);
        if (!backend_->FinishFrame(frame, submit)) {
            SetError(backend_->LastError());
            return false;
        }
        pipelined_packet_ = next;
        if (copy == OpenXRSubmissionStatus::Failed) {
            SetError(std::string("Aurora's ") + kGraphicsBackendName + " stereo copy failed" + kFallbackNote);
            return false;
        }
        if (!submit) {
            ++consecutive_skips;
            if (consecutive_skips == 1 || consecutive_skips % 60 == 0) {
                PORTVR_LOG() << "OpenXR: eye copy skipped (" << consecutive_skips
                             << " in a row): " << backend_->LastError() << std::endl;
            }
            return consecutive_skips < kMaxConsecutiveSkips;
        }
        consecutive_skips = 0;
        ++timing_submissions_;
        if (immersive && !immersive_submission_logged) {
            immersive_submission_logged = true;
            PORTVR_LOG() << "[vr] first immersive packet consumed and submitted as "
                            "an OpenXR projection layer"
                         << std::endl;
        }
        return true;
    }

    // Also aims the immersive window's eyes through the window, in `source` itself, so that the layer
    // later built from it shows the eyes as they were rendered.
    void BuildPublishedFrame(OpenXRBackendFrame& source, bool immersive, const PrimeVRPolicySnapshot& policy) noexcept {
        const float units_per_meter = policy.EffectiveUnitsPerMeter();
        const uint64_t content_tag = policy.content_tag;
        ApplyPendingReferenceSpaceChange(source.xr_frame);
        auto& destination = published_frame_.frame;
        destination = {};
        destination.frameToken = source.xr_frame.serial;
        destination.contentTag = content_tag;
        destination.displayTimeNanos = DisplayTimeNanos(source.xr_frame.predicted_display_time);
        destination.mode = immersive ? AURORA_STEREO_FRAME_IMMERSIVE_REPLAY
                                      : AURORA_STEREO_FRAME_VIRTUAL_SCREEN;
        destination.window = false;
        for (uint32_t eye = 0; eye < kOpenXREyeCount; ++eye) {
            destination.eyes[eye].width = source.render_width[eye];
            destination.eyes[eye].height = source.render_height[eye];
            IdentityEye(destination.eyes[eye]);
        }
        if (!immersive) {
            last_immersive_ = false;
            return;
        }

        const bool position_valid =
            (source.xr_frame.view_state_flags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0;
        // Latch on the first immersive frame, after a recenter or an origin
        // change, and on re-entry from the virtual screen so a race start
        // recenters a player who shifted during the menus. Position only: the
        // heading and the horizon belong to the reference space, so no
        // transition here can tilt the view or redefine forward.
        if (position_valid && (!base_position_valid_ || !last_immersive_)) {
            base_position_ = CenterPosition(source.xr_frame);
            base_position_valid_ = true;
            ++tracking_origin_generation_;
        }
        last_immersive_ = true;
        // With the eyes' centre space it belongs to, after this frame's latch.
        destination.originGeneration = tracking_origin_generation_;
        // Read once so both eyes are built from the same angle even if the
        // settings slider moves between them.
        const float lean_back_radians =
            lean_back_degrees_.load(std::memory_order_relaxed) * kDegreesToRadians;
        for (uint32_t eye = 0; eye < kOpenXREyeCount; ++eye) {
            ProjectionFromFov(source.xr_frame.views[eye].fov,
                              destination.eyes[eye].projection);
            ViewFromBase(source.xr_frame.views[eye].pose, base_position_,
                         position_valid && base_position_valid_, units_per_meter,
                         lean_back_radians, destination.eyes[eye].viewFromCenter);
        }
    }

    // Runs once per located frame, before the virtual screen is placed and
    // before the immersive origin is latched, so a recenter reaches both from
    // this frame's head pose rather than the next one's.
    void ServiceRecenterRequest() noexcept {
        if (recenter_requested_.exchange(false, std::memory_order_acq_rel)) {
            InvokeRecenterCallback();
            ResetTrackingOrigin();
        }
    }

    // Each newly opened menu/cinema screen captures the latest HMD facing.
    // Keep tracking the head during gameplay too, for a transition on a frame
    // whose tracking is unavailable; the visible screen then stays anchored.
    void UpdateVirtualScreenPose(OpenXRBackendFrame& frame, const PrimeVRPolicySnapshot& policy) noexcept {
        constexpr XrViewStateFlags kPoseUsable =
            XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
        screen_math::Pose head{};
        const bool tracked = frame.xr_frame.views_valid &&
                             (frame.xr_frame.view_state_flags & kPoseUsable) == kPoseUsable;
        if (tracked) {
            head.position = CenterPosition(frame.xr_frame);
            const auto& q = frame.xr_frame.views[0].pose.orientation;
            head.orientation = {q.x, q.y, q.z, q.w};
        }
        virtual_screen_anchor_.Update(policy.presentation, policy.game_mode, tracked ? &head : nullptr,
                                      frame.presentation.quad_distance_meters,
                                      frame.xr_frame.predicted_display_time);
        const auto& pose = virtual_screen_anchor_.Pose();
        frame.presentation.quad_anchored = virtual_screen_anchor_.Valid();
        frame.presentation.quad_pose.orientation =
            {pose.orientation[0], pose.orientation[1], pose.orientation[2], pose.orientation[3]};
        frame.presentation.quad_pose.position = {pose.position[0], pose.position[1], pose.position[2]};
    }

    // The rectangle the game picture covers on the screen this frame shows, in
    // the application space, for the Wii Remote pointer to aim at.
    //
    // Menus: the quad layer UpdateVirtualScreenPose placed (or its head-locked
    // fallback), sized like the backends size it: hud_width_meters across with
    // the eye texture's aspect, the desktop snapshot letterboxed into it and the
    // picture into the snapshot.
    //
    // Races: the 2D layer's screen, which Aurora hangs hud_distance_meters
    // ahead in the recorded centre-eye space. ViewFromBase maps a point p of
    // that space (in metres) to base + lean * p in the application space, so the
    // screen sits at base + lean * (0, 0, -distance), turned by the lean, its
    // height following the picture aspect as stereo_hud_screen's does. With the
    // 2D layer stretched across the eyes there is no screen to point at, except
    // in the immersive window, which is that screen and always carries the layer.
    OpenXRPointerScreen PointerScreen(const OpenXRBackendFrame& frame, const PrimeVRPolicySnapshot& policy,
                                      bool immersive) const noexcept {
        OpenXRPointerScreen screen{};
        float picture_aspect = 0.0f;
        float snapshot_aspect = 0.0f;
        if (!aurora_get_stereo_screen_aspects(&picture_aspect, &snapshot_aspect)) {
            return screen;
        }

        if (immersive) {
            // The immersive window always carries the 2D layer.
            const bool on_screen = aurora_get_stereo_hud_screen_enabled();
            if (!on_screen || !(policy.config.screen_width_meters > 0.0f) || !RaceScreenPose(frame, policy, screen.pose)) {
                return screen;
            }
            screen.half_width_meters = 0.5f * policy.config.screen_width_meters;
            screen.half_height_meters = screen.half_width_meters / picture_aspect;
            screen.valid = true;
            return screen;
        }

        if (frame.render_width[0] == 0 || frame.render_height[0] == 0 || !MenuScreenPose(frame, screen.pose)) {
            return screen;
        }
        const float eye_aspect =
            static_cast<float>(frame.render_width[0]) / static_cast<float>(frame.render_height[0]);
        const std::array<float, 2> extents = screen_math::MenuPictureHalfExtents(
            std::max(0.25f, frame.presentation.quad_width_meters), eye_aspect, snapshot_aspect, picture_aspect);
        screen.half_width_meters = extents[0];
        screen.half_height_meters = extents[1];
        screen.valid = true;
        return screen;
    }

    // The VR menu's layers hang where the input layer placed the menu this
    // frame (on the off hand, or floating) and aimed the laser, the same poses
    // its pointer hits were tested against. While the menu is closed the
    // shader compilation toast may have the first layer (PlaceCompileToast).
    void PlacePanelLayer(OpenXRBackendFrame& frame, const OpenXRPointerScreen& screen) noexcept {
        OpenXRPanelLayer& panel = frame.presentation.panel;
        panel.image_rows = VrMenu::kMenuHeight;
        if (panel.requested && !OpenXRSettingsPanelOpen() && OpenXRCompileToastShown()) {
            PlaceCompileToast(frame, screen);
            return;
        }
        toast_floating_valid_ = false;
        const OpenXRMenuPlacement menu = input_ != nullptr ? input_->MenuPlacement() : OpenXRMenuPlacement{};
        panel.placed = panel.requested && menu.placed;
        if (!panel.placed) {
            panel.laser = panel.dot = false;
            return;
        }
        panel.pose = menu.pose;
        panel.width_meters = menu.width_meters;
        panel.height_meters = menu.height_meters;
        panel.laser = menu.laser;
        panel.laser_pose = menu.laser_pose;
        panel.laser_length_meters = menu.laser_length_meters;
        panel.dot = menu.dot;
        panel.dot_pose = menu.dot_pose;
    }

    // PrimedGun's compile progress panel, the image's top VrMenu::kToastHeight
    // rows: across the top of the screen the game picture is on (the virtual
    // screen, or the immersive window). With none up (immersive play) it floats
    // ahead of where the head looked when it appeared, upright, and stays there.
    void PlaceCompileToast(OpenXRBackendFrame& frame, const OpenXRPointerScreen& screen) noexcept {
        OpenXRPanelLayer& panel = frame.presentation.panel;
        panel.laser = panel.dot = false;
        panel.image_rows = VrMenu::kToastHeight;
        VrMenu::ToastPlacement toast;
        if (screen.valid) {
            toast_floating_valid_ = false;
            screen_math::Pose screen_pose;
            screen_pose.position = {screen.pose.position.x, screen.pose.position.y, screen.pose.position.z};
            screen_pose.orientation = {screen.pose.orientation.x, screen.pose.orientation.y,
                                       screen.pose.orientation.z, screen.pose.orientation.w};
            toast = VrMenu::CompileToastOnScreen(screen_pose, screen.half_width_meters, screen.half_height_meters);
        } else if (frame.presentation.mode == OpenXRFrameMode::VirtualScreen) {
            // The menu screen whose picture isn't sized yet (the first frames): wait for it
            // rather than float and then jump onto it.
            panel.placed = false;
            return;
        } else {
            if (!toast_floating_valid_) {
                constexpr XrViewStateFlags kPoseUsable =
                    XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
                const OpenXRFrame& xr_frame = frame.xr_frame;
                if (!xr_frame.views_valid || (xr_frame.view_state_flags & kPoseUsable) != kPoseUsable) {
                    panel.placed = false;
                    return;
                }
                screen_math::Pose head;
                head.position = CenterPosition(xr_frame);
                const auto& q = xr_frame.views[0].pose.orientation;
                head.orientation = {q.x, q.y, q.z, q.w};
                if (!VrMenu::CompileToastFloating(head, toast_floating_)) {
                    panel.placed = false;
                    return;
                }
                toast_floating_valid_ = true;
            }
            toast = toast_floating_;
        }
        panel.placed = true;
        panel.pose.position = {toast.pose.position[0], toast.pose.position[1], toast.pose.position[2]};
        panel.pose.orientation = {toast.pose.orientation[0], toast.pose.orientation[1], toast.pose.orientation[2],
                                  toast.pose.orientation[3]};
        panel.width_meters = toast.width_meters;
        panel.height_meters = toast.height_meters;
    }

    // Centre of the race's 2D screen. ViewFromBase maps a point p of the
    // recorded centre-eye space (in metres) to base + lean * p in the
    // application space, so the screen Aurora hangs hud_distance_meters ahead
    // sits at base + lean * (0, 0, -distance), turned by the lean.
    bool RaceScreenPose(const OpenXRBackendFrame& frame, const PrimeVRPolicySnapshot& policy,
                        XrPosef& pose) const noexcept {
        const OpenXRFrame& xr_frame = frame.xr_frame;
        const float distance = policy.config.screen_distance_meters;
        if (!(distance > 0.0f)) {
            return false;
        }
        std::array<float, 3> base{};
        if (base_position_valid_ && last_immersive_) {
            base = base_position_;
        } else if (xr_frame.views_valid &&
                   (xr_frame.view_state_flags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0 &&
                   (xr_frame.view_state_flags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0) {
            // BuildPublishedFrame latches exactly this for the frame.
            base = CenterPosition(xr_frame);
        } else {
            return false;
        }
        const float half_angle = 0.5f * lean_back_degrees_.load(std::memory_order_relaxed) * kDegreesToRadians;
        const Quaternion lean{std::sin(half_angle), 0.0f, 0.0f, std::cos(half_angle)};
        const std::array<float, 3> ahead = Rotate(lean, {0.0f, 0.0f, -distance});
        pose.orientation = {lean.x, lean.y, lean.z, lean.w};
        pose.position = {base[0] + ahead[0], base[1] + ahead[1], base[2] + ahead[2]};
        return true;
    }

    // Centre of the menu quad: where UpdateVirtualScreenPose anchored it, or
    // its head-locked fallback straight ahead of the head.
    bool MenuScreenPose(const OpenXRBackendFrame& frame, XrPosef& pose) const noexcept {
        if (frame.presentation.mode != OpenXRFrameMode::VirtualScreen) {
            return false;
        }
        if (frame.presentation.quad_anchored) {
            pose = frame.presentation.quad_pose;
            return true;
        }
        const OpenXRFrame& xr_frame = frame.xr_frame;
        if (!xr_frame.views_valid || (xr_frame.view_state_flags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) == 0 ||
            (xr_frame.view_state_flags & XR_VIEW_STATE_POSITION_VALID_BIT) == 0) {
            return false;
        }
        const auto& head = xr_frame.views[0].pose.orientation;
        const Quaternion orientation = Normalize({head.x, head.y, head.z, head.w});
        const std::array<float, 3> center = CenterPosition(xr_frame);
        const std::array<float, 3> ahead =
            Rotate(orientation, {0.0f, 0.0f, -std::max(0.25f, frame.presentation.quad_distance_meters)});
        pose.orientation = {orientation.x, orientation.y, orientation.z, orientation.w};
        pose.position = {center[0] + ahead[0], center[1] + ahead[1], center[2] + ahead[2]};
        return true;
    }

    void ApplyPendingReferenceSpaceChange(const OpenXRFrame& frame) noexcept {
        if (runtime_->ConsumeAppSpaceChangesThrough(frame.predicted_display_time)) {
            InvokeRecenterCallback();
            ResetTrackingOrigin();
        }
    }

    void InvokeRecenterCallback() noexcept {
        if (void (*callback)() = recenter_callback_.load(std::memory_order_acquire); callback != nullptr) {
            callback();
        }
    }

    // The poses this packet was built from, for the game thread's draw.
    void PublishFrameRequest(const OpenXRBackendFrame& source, bool immersive,
                             const PrimeVRPolicySnapshot& policy) noexcept {
        OpenXRFrameRequest request{};
        request.predicted_display_time = source.xr_frame.predicted_display_time;
        request.display_time_nanos = published_frame_.frame.displayTimeNanos;
        request.immersive = immersive;
        request.content_tag = policy.content_tag;
        request.units_per_meter = policy.EffectiveUnitsPerMeter();
        const XrViewStateFlags flags = source.xr_frame.view_state_flags;
        request.head_valid = source.xr_frame.views_valid &&
                             (flags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0 &&
                             (flags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0;
        if (request.head_valid) {
            request.head_position = CenterPosition(source.xr_frame);
            const auto& q = source.xr_frame.views[0].pose.orientation;
            request.head_orientation = {q.x, q.y, q.z, q.w};
            for (uint32_t eye = 0; eye < kOpenXREyeCount; ++eye) {
                const XrView& view = source.xr_frame.views[eye];
                request.eye_position[eye] = {view.pose.position.x, view.pose.position.y, view.pose.position.z};
                request.eye_orientation[eye] = {view.pose.orientation.x, view.pose.orientation.y,
                                                view.pose.orientation.z, view.pose.orientation.w};
                request.eye_fov[eye] = {view.fov.angleLeft, view.fov.angleRight, view.fov.angleUp,
                                        view.fov.angleDown};
            }
        }
        request.base_position = base_position_;
        request.base_valid = base_position_valid_;
        request.tracking_origin_generation = tracking_origin_generation_;
        {
            std::lock_guard lock(request_mutex_);
            request.serial = ++request_serial_;
            request_ = request;
        }
        request_cv_.notify_all();
    }

    void ResetTrackingOrigin() noexcept {
        base_position_ = {};
        base_position_valid_ = false;
        last_immersive_ = false;
        // The anchored menu screen is placed in the same space, so it is stale
        // for exactly the same reasons and is re-placed on the next frame.
        virtual_screen_anchor_.Reset();
        // And so is the floating compile toast.
        toast_floating_valid_ = false;
    }

    void SetInterpolationActive(bool active) noexcept {
        std::lock_guard lock(interpolation_mutex_);
        aurora_set_stereo_frame_interpolation(active && !interpolation_stopping_);
    }

    void UpdateFrameTiming(const OpenXRFrame& frame) noexcept {
        float hz = 0;
        if (get_display_refresh_rate_ == nullptr ||
            XR_FAILED(get_display_refresh_rate_(runtime_->Session(), &hz)) || !(hz > 0)) {
            if (frame.predicted_display_period > 0)
                hz = static_cast<float>(1.0e9 / static_cast<double>(frame.predicted_display_period));
        }
        headset_hz_.store(hz, std::memory_order_relaxed);
        const auto now = std::chrono::steady_clock::now();
        const float elapsed = std::chrono::duration<float>(now - timing_start_).count();
        if (elapsed >= 1.0f) {
            rendered_fps_.store(static_cast<float>(timing_submissions_) / elapsed, std::memory_order_relaxed);
            timing_start_ = now;
            timing_submissions_ = 0;
        }
    }

    // Pacing thread, only while diagnostics are on.
    void NoteFrameDiagnostics(const OpenXRBackendFrame& frame, bool immersive) {
        const XrViewStateFlags flags = frame.xr_frame.view_state_flags;
        diagnostics::OnFrameBegun(immersive, frame.xr_frame.should_render, frame.xr_frame.views_valid,
                                  (flags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0,
                                  (flags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0);
        if (diagnostics::ConsumeSessionInfoRequest()) {
            LogDiagnosticSession(frame);
        }
        // The immersive window's eyes are aimed through the window, so their fields of view follow
        // the head and are no longer the headset's.
        if (frame.xr_frame.should_render && frame.xr_frame.views_valid && !frame.presentation.window_eyes) {
            diagnostics::OnViewGeometry(DiagnosticViewGeometry(frame));
        }
    }

    // Everything about the headset and runtime that a pacing report is read
    // against. Written when logging starts and again for every new session.
    void LogDiagnosticSession(const OpenXRBackendFrame& frame) const {
        const auto& info = runtime_->RuntimeInfo();
        std::ostringstream line;
        line << "session: runtime '" << info.runtime_name << "' " << XR_VERSION_MAJOR(info.runtime_version) << '.'
             << XR_VERSION_MINOR(info.runtime_version) << '.' << XR_VERSION_PATCH(info.runtime_version)
             << ", system '" << info.system_name << "', vendor 0x" << std::hex << info.vendor_id << std::dec
             << ", orientation tracking " << info.supports_orientation_tracking << ", position tracking "
             << info.supports_position_tracking << ", max layers " << info.max_layer_count;
        diagnostics::Info(line.str());

        line.str({});
        line << "session: " << kGraphicsBackendName << " backend, reference space "
             << DiagnosticSpaceName(runtime_->AppSpaceType()) << ", blend mode "
             << DiagnosticBlendModeName(runtime_->EnvironmentBlendMode()) << ", extensions";
        for (const std::string& extension : runtime_->EnabledExtensions()) {
            line << ' ' << extension;
        }
        diagnostics::Info(line.str());

        line.str({});
        const auto& views = runtime_->ViewConfiguration();
        line << "session: recommended eye size " << views[0].properties.recommendedImageRectWidth << 'x'
             << views[0].properties.recommendedImageRectHeight << " / "
             << views[1].properties.recommendedImageRectWidth << 'x'
             << views[1].properties.recommendedImageRectHeight << ", max "
             << views[0].properties.maxImageRectWidth << 'x' << views[0].properties.maxImageRectHeight
             << ", render_scale " << runtime_->Config().resolution_scale << ", swapchains "
             << views[0].render_width << 'x' << views[0].render_height << " / " << views[1].render_width << 'x'
             << views[1].render_height;
        diagnostics::Info(line.str());

        line.str({});
        const uint32_t interpolation = frame_interpolation_fps_.load(std::memory_order_relaxed);
        line << "session: display period ";
        if (frame.xr_frame.predicted_display_period > 0) {
            const double period_ms = static_cast<double>(frame.xr_frame.predicted_display_period) / 1.0e6;
            line << period_ms << " ms (" << 1000.0 / period_ms << " Hz)";
        } else {
            line << "unknown";
        }
        line << ", refresh-rate extension " << (get_display_refresh_rate_ != nullptr ? "yes" : "no")
             << ", display-time conversion " << (convert_display_time_ != nullptr ? "yes" : "no")
             << ", VR frame interpolation "
             << (interpolation == 0 ? std::string("off")
                 : interpolation == 1 ? std::string("auto")
                                      : std::to_string(interpolation))
             << (FrameInterpolationAvailable() ? "" : " (unavailable)");
        diagnostics::Info(line.str());
    }

    // Converts the compositor's predicted display time onto the runtime's
    // steady clock, which is what Aurora's interpolation deadlines are paced by.
    uint64_t DisplayTimeNanos(XrTime display_time) noexcept {
        if (convert_display_time_ == nullptr) return 0;
        const auto now = std::chrono::steady_clock::now();
        const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
#if defined(_WIN32)
        LARGE_INTEGER display_counter{}, counter{}, frequency{};
        if (XR_FAILED(convert_display_time_(runtime_->Instance(), display_time, &display_counter)) ||
            !QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0 ||
            !QueryPerformanceCounter(&counter)) return 0;
        const auto delta = static_cast<int64_t>(
            (static_cast<double>(display_counter.QuadPart) - static_cast<double>(counter.QuadPart)) *
            1.0e9 / static_cast<double>(frequency.QuadPart));
#else
        // XR_KHR_convert_timespec_time yields CLOCK_MONOTONIC, the clock behind
        // libc++'s steady_clock, so the delta is measured on that clock too.
        timespec display_spec{};
        timespec now_spec{};
        if (XR_FAILED(convert_display_time_(runtime_->Instance(), display_time, &display_spec)) ||
            clock_gettime(CLOCK_MONOTONIC, &now_spec) != 0) return 0;
        const int64_t display_ns = static_cast<int64_t>(display_spec.tv_sec) * 1'000'000'000ll + display_spec.tv_nsec;
        const int64_t monotonic_ns = static_cast<int64_t>(now_spec.tv_sec) * 1'000'000'000ll + now_spec.tv_nsec;
        const int64_t delta = display_ns - monotonic_ns;
#endif
        return now_ns + delta > 0 ? static_cast<uint64_t>(now_ns + delta) : 0;
    }

    void WaitForStopOrDelay(std::chrono::milliseconds delay) {
        std::unique_lock lock(stop_mutex_);
        stop_cv_.wait_for(lock, delay,
                          [this] { return stop_.load(std::memory_order_acquire); });
    }

    void WithdrawPublishedFrame() noexcept {
        std::lock_guard lock(published_mutex_);
        published_.store(nullptr, std::memory_order_release);
    }

    void SetError(std::string message) {
        {
            std::lock_guard lock(error_mutex_);
            last_error_ = std::move(message);
        }
        PORTVR_LOG() << "OpenXR: " << LastError() << std::endl;
    }

    // The runtime ended the session: Quit in the system menu, or the session was lost.
    // The desktop keeps playing in its window; on a standalone headset the app ends
    // with it (PacingThread), and that is no error.
    void OnRuntimeExitRequested() {
        exit_requested_.store(true, std::memory_order_release);
        SetError(std::string("OpenXR runtime requested session exit") + kFallbackNote);
    }

    OpenXRLogCallback logger_;
    std::unique_ptr<OpenXRRuntime> runtime_;
    std::unique_ptr<GraphicsBackend> backend_;
#if defined(__ANDROID__)
#endif
    std::unique_ptr<OpenXRInput> input_;
    std::thread pacing_thread_;
    std::atomic_bool stop_{false};
    std::atomic_bool running_{false};
    std::atomic_bool teardown_requested_{false};
    std::atomic_bool recenter_requested_{false};
    std::atomic<void (*)()> recenter_callback_{nullptr};
    std::mutex request_mutex_;
    std::condition_variable request_cv_;
    OpenXRFrameRequest request_{};
    uint64_t request_serial_ = 0;
    std::atomic<float> lean_back_degrees_{GetVrSettings().lean_back_degrees};
    std::atomic<float> render_scale_{GetVrSettings().render_scale};
    // Pipelined pacing (PipelinedCycle): the packet in flight, the scale it runs at, and the
    // game's pickup of the latest packet (ProvideStereoFrame), which the cycle waits for.
    std::optional<OpenXRBackendFrame> pipelined_packet_;
    float pipelined_scale_ = -1.0f;
    std::mutex pickup_mutex_;
    std::condition_variable pickup_cv_;
    uint64_t picked_up_token_ = 0;
    static constexpr uint32_t kPickupWaitMs = 50;
    std::atomic<float> display_refresh_rate_{GetVrSettings().display_refresh_rate};
    // Pacing thread only: the rate last asked for (-1 forces a request), and
    // whether the runtime accepted one this session.
    float applied_display_refresh_rate_ = -1.0f;
    // The performance level setting changed while a session ran (OpenXRReapplyPerformanceLevel).
    std::atomic_bool performance_level_dirty_{false};
    bool display_refresh_rate_requested_ = false;
    // The left eye for OpenXRGetEyeResolution: the runtime's description of it, set while a
    // session runs, and the size it is rendered at now (0 without a session).
    mutable std::mutex eye_view_mutex_;
    XrViewConfigurationView eye_view_{XR_TYPE_VIEW_CONFIGURATION_VIEW};
    std::atomic_uint32_t eye_width_{0};
    std::atomic_uint32_t eye_height_{0};
    std::atomic_bool passthrough_{GetVrSettings().passthrough};
    std::atomic_uint32_t frame_interpolation_fps_{GetVrSettings().frame_interpolation_fps};
    std::atomic_bool interpolation_available_{false};
    std::mutex interpolation_mutex_;
    bool interpolation_stopping_ = true;
    FrameInterpolationPacing interpolation_pacing_;
    std::atomic<float> headset_hz_{0};
    std::atomic<float> rendered_fps_{0};
    std::chrono::steady_clock::time_point timing_start_ = std::chrono::steady_clock::now();
    uint32_t timing_submissions_ = 0;
    PFN_xrGetDisplayRefreshRateFB get_display_refresh_rate_ = nullptr;
    PFN_xrEnumerateDisplayRefreshRatesFB enumerate_display_refresh_rates_ = nullptr;
    PFN_xrRequestDisplayRefreshRateFB request_display_refresh_rate_ = nullptr;
    PFN_xrPerfSettingsSetPerformanceLevelEXT set_performance_level_ = nullptr;
#if defined(_WIN32)
    using ConvertDisplayTime = XrResult (XRAPI_PTR*)(XrInstance, XrTime, LARGE_INTEGER*);
#else
    using ConvertDisplayTime = XrResult (XRAPI_PTR*)(XrInstance, XrTime, struct timespec*);
#endif
    ConvertDisplayTime convert_display_time_ = nullptr;
    std::atomic<PublishedFrame*> published_{nullptr};
    PublishedFrame published_frame_{};
    std::mutex published_mutex_;
    std::mutex stop_mutex_;
    std::condition_variable stop_cv_;
    mutable std::mutex error_mutex_;
    std::string last_error_;
    std::array<float, 3> base_position_{};
    bool base_position_valid_ = false;
    uint64_t tracking_origin_generation_ = 0;
    VirtualScreenAnchor virtual_screen_anchor_{};
    // The shader compilation toast's floating pose (PlaceCompileToast), latched while no screen is up.
    VrMenu::ToastPlacement toast_floating_{};
    bool toast_floating_valid_ = false;
    bool last_immersive_ = false;
    uint64_t applied_session_run_serial_ = 0;
    bool session_was_active_ = false;
    bool requested_ = false;
    bool prepared_ = false;
    bool provider_registered_ = false;
    bool graphics_retained_ = false;
    std::atomic<bool> exit_requested_{false};
    // Whether the XR session runs, for the game loop's wait (FrameRequestTimeoutMs).
    std::atomic<bool> session_running_{false};
    // The pacing thread gives a packet 50 ms (RenderFirstCycle); a shorter wait in the
    // game loop drew frames between requests that no packet asked for: on a Quest at
    // 72 Hz nearly every other draw, which took the GPU from the headset.
    static constexpr uint32_t kFrameRequestWaitRunningMs = 50;
    static constexpr uint32_t kFrameRequestWaitIdleMs = 16;
    uint32_t game_thread_id_ = 0;
};

#endif // MP_OPENXR_GRAPHICS_BACKEND

} // namespace

OpenXRStartupResult OpenXRPrepareAurora(AuroraConfig& config) {
#if !defined(MP_ENABLE_OPENXR)
    (void)config;
    ConfigurePolicy(false);
    return GetVrSettings().enabled ? OpenXRStartupResult::Unavailable
                                                           : OpenXRStartupResult::Disabled;
#elif MP_OPENXR_GRAPHICS_BACKEND
    return OpenXRIntegration::Get().Prepare(config);
#else
    (void)config;
    ConfigurePolicy(GetVrSettings().enabled);
    if (!GetVrSettings().enabled) {
        return OpenXRStartupResult::Disabled;
    }
    PORTVR_LOG() << "OpenXR is not wired to a graphics backend on this platform" << std::endl;
    return OpenXRStartupResult::Unavailable;
#endif
}

bool OpenXRStartAfterAurora(AuroraBackend active_backend) {
#if MP_OPENXR_GRAPHICS_BACKEND
    return OpenXRIntegration::Get().Start(active_backend);
#else
    (void)active_backend;
    return !GetVrSettings().enabled;
#endif
}

void OpenXRShutdownBeforeAurora() noexcept {
#if MP_OPENXR_GRAPHICS_BACKEND
    OpenXRIntegration::Get().Shutdown();
#else
    PrimeVRPolicySetSessionActive(false);
#endif
}

void OpenXRServiceProducerFrameBoundary() noexcept {
#if MP_OPENXR_GRAPHICS_BACKEND
    OpenXRIntegration::Get().ServiceProducerFrameBoundary();
#endif
}

bool OpenXRIsRunning() noexcept {
#if MP_OPENXR_GRAPHICS_BACKEND
    return OpenXRIntegration::Get().IsRunning();
#else
    return false;
#endif
}

namespace {
bool g_headset_input_focus = false; // game thread
} // namespace

void OpenXRApplyControllerState() noexcept {
#if MP_OPENXR_GRAPHICS_BACKEND
    OpenXRApplyVirtualGamepad();
    // SDL drops joystick presses while no window of ours has keyboard focus,
    // virtual joysticks included; the port turns background events off. While
    // the headset has the input focus they are let through, from the next pump.
    const bool focus = OpenXRIsRunning() && OpenXRGetInputSnapshot().runtime_active;
    if (focus != g_headset_input_focus) {
        g_headset_input_focus = focus;
        aurora_set_background_input(focus);
    }
#endif
}

bool OpenXRHeadsetHasInputFocus() noexcept { return g_headset_input_focus; }

void OpenXRRequestRecenter() noexcept {
#if MP_OPENXR_GRAPHICS_BACKEND
    OpenXRIntegration::Get().RequestRecenter();
#endif
}

void OpenXRSetLeanBackDegrees(float degrees) noexcept {
#if MP_OPENXR_GRAPHICS_BACKEND
    OpenXRIntegration::Get().SetLeanBackDegrees(degrees);
#else
    (void)degrees;
#endif
}

void OpenXRSetPassthrough(bool enabled) noexcept {
#if MP_OPENXR_GRAPHICS_BACKEND
    OpenXRIntegration::Get().SetPassthrough(enabled);
#else
    (void)enabled;
#endif
}

void OpenXRSetRenderScale(float scale) noexcept {
#if MP_OPENXR_GRAPHICS_BACKEND
    OpenXRIntegration::Get().SetRenderScale(scale);
#else
    (void)scale;
#endif
}

void OpenXRSetDisplayRefreshRate(float hz) noexcept {
#if MP_OPENXR_GRAPHICS_BACKEND
    OpenXRIntegration::Get().SetDisplayRefreshRate(hz);
#else
    (void)hz;
#endif
}

void OpenXRReapplyPerformanceLevel() noexcept {
#if MP_OPENXR_GRAPHICS_BACKEND
    OpenXRIntegration::Get().ReapplyPerformanceLevel();
#endif
}

OpenXREyeResolution OpenXRGetEyeResolution(float scale) noexcept {
#if MP_OPENXR_GRAPHICS_BACKEND
    return OpenXRIntegration::Get().EyeResolution(scale);
#else
    (void)scale;
    return {};
#endif
}

void OpenXRSetFrameInterpolationFps(uint32_t target) noexcept {
#if MP_OPENXR_GRAPHICS_BACKEND
    OpenXRIntegration::Get().SetFrameInterpolationFps(target);
#else
    (void)target;
#endif
}

OpenXRFrameTiming OpenXRGetFrameTiming() noexcept {
#if MP_OPENXR_GRAPHICS_BACKEND
    return OpenXRIntegration::Get().FrameTiming();
#else
    return {};
#endif
}

bool OpenXRFrameInterpolationAvailable() noexcept {
#if MP_OPENXR_GRAPHICS_BACKEND
    return OpenXRIntegration::Get().FrameInterpolationAvailable();
#else
    return false;
#endif
}

void OpenXRSetRecenterCallback(void (*callback)()) noexcept {
#if MP_OPENXR_GRAPHICS_BACKEND
    OpenXRIntegration::Get().SetRecenterCallback(callback);
#else
    (void)callback;
#endif
}

uint32_t OpenXRFrameRequestTimeoutMs() noexcept {
#if MP_OPENXR_GRAPHICS_BACKEND
    return OpenXRIntegration::Get().FrameRequestTimeoutMs();
#else
    return 16;
#endif
}

bool OpenXRWaitForFrameRequest(OpenXRFrameRequest& request, uint32_t timeout_ms) noexcept {
#if MP_OPENXR_GRAPHICS_BACKEND
    if (!OpenXRIntegration::Get().IsRunning()) {
        return false;
    }
    return OpenXRIntegration::Get().WaitForFrameRequest(request, timeout_ms);
#else
    (void)request;
    (void)timeout_ms;
    return false;
#endif
}

bool OpenXRLatestFrameRequest(OpenXRFrameRequest& request) noexcept {
#if MP_OPENXR_GRAPHICS_BACKEND
    return OpenXRIntegration::Get().LatestFrameRequest(request);
#else
    (void)request;
    return false;
#endif
}

std::string OpenXRLastError() {
#if !defined(MP_ENABLE_OPENXR)
    return "this build was compiled without OpenXR support";
#elif MP_OPENXR_GRAPHICS_BACKEND
    return OpenXRIntegration::Get().LastError();
#else
    return "OpenXR is not wired to a graphics backend on this platform";
#endif
}

bool OpenXRHeadsetIsOnlyDisplay() noexcept {
#if defined(MP_ENABLE_OPENXR) && defined(__ANDROID__)
    return true;
#else
    return false;
#endif
}

void OpenXRRequestAppQuit(const std::string& error) {
#if defined(MP_ENABLE_OPENXR) && defined(__ANDROID__)
    OpenXRAndroidRequestQuit(error);
#else
    (void)error;
#endif
}

} // namespace PortVr
