// SPDX-License-Identifier: GPL-3.0-or-later
//
// PlayStation VR2 Sense controllers for the visionOS OpenXR provider.
//
// visionOS 26 pairs the Sense controllers as GameController devices of the
// spatial controller category (GCProductCategorySpatialController): their
// buttons, triggers and sticks come from GCController.input, and their poses
// from ARKit's accessory tracking. Each controller is loaded as an ARKit
// accessory (ar_accessory_load_from_device) and tracked by an accessory
// tracking provider that runs in an ARKit session of its own: the provider's
// accessory set is fixed when it is created (changing it in place needs
// visionOS 27), so a controller that connects mid-game means a new provider
// and a new session, and keeping that apart from the compositor's session
// leaves the world tracking the head pose comes from untouched. All ARKit
// sessions of an app share its world origin.
//
// The actions read the controllers as Oculus Touch controllers, which Prime's
// input layer (openxr_input.cpp) already binds: grip and aim poses, trigger,
// squeeze, stick, A/B and X/Y, menu (xr_visionos_input.mm). The DualSense and
// other gamepads stay SDL's (Aurora's lib/input.cpp leaves the spatial
// controllers to this file).

#include "xr_visionos_internal.h"

#import <CoreHaptics/CoreHaptics.h>
#import <GameController/GameController.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace PortVr::visionos {
namespace {

struct Controller {
    GCController* device = nil;
    // Null until ARKit loaded it (asynchronous), or when it could not.
    ar_accessory_t accessory = nil;
    // The hand it is made for (the Sense controllers are a left and a right one).
    ar_accessory_chirality_t inherentChirality = ar_accessory_chirality_unspecified;
    CHHapticEngine* haptics = nil;
    bool hapticsFailed = false;
};

std::mutex g_mutex;
bool g_started = false;
std::vector<Controller> g_controllers;
ar_session_t g_session = nil;
ar_accessory_tracking_provider_t g_provider = nil;
// Scratch anchor the predictions are written into (the provider's anchors are its own).
ar_accessory_anchor_t g_predicted = nil;
std::atomic<uint64_t> g_generation{1};
id g_connectObserver = nil;
id g_disconnectObserver = nil;
dispatch_queue_t g_hapticsQueue = nil;

bool IsSpatialController(GCController* controller) noexcept {
    return controller != nil && [controller.productCategory isEqualToString:GCProductCategorySpatialController];
}

Controller* FindLocked(GCController* device) noexcept {
    for (Controller& controller : g_controllers) {
        if (controller.device == device) {
            return &controller;
        }
    }
    return nullptr;
}

// Replaces the accessory session with one that tracks every loaded controller.
// Called with g_mutex held.
void RestartSessionLocked() noexcept {
    if (g_session != nil) {
        ar_session_stop(g_session);
        g_session = nil;
        g_provider = nil;
    }
    ar_accessories_t accessories = ar_accessories_create();
    size_t count = 0;
    for (const Controller& controller : g_controllers) {
        if (controller.accessory != nil) {
            ar_accessories_add_accessory(accessories, controller.accessory);
            ++count;
        }
    }
    g_generation.fetch_add(1);
    if (count == 0) {
        return;
    }
    if (!ar_accessory_tracking_provider_is_supported()) {
        Log("accessory tracking is not supported here: the controllers' buttons work, their poses do not");
        return;
    }
    ar_accessory_tracking_configuration_t configuration = ar_accessory_tracking_configuration_create();
    ar_accessory_tracking_configuration_set_accessories(configuration, accessories);
    g_provider = ar_accessory_tracking_provider_create(configuration);
    g_session = ar_session_create();
    if (g_predicted == nil) {
        g_predicted = ar_accessory_anchor_create();
    }
    ar_session_set_data_provider_state_change_handler(
        g_session, nullptr,
        ^(ar_data_providers_t, ar_data_provider_state_t state, ar_error_t error, ar_data_provider_t) {
            if (error != nil) {
                Log("accessory tracking stopped: error %ld", static_cast<long>(ar_error_get_error_code(error)));
            } else if (state == ar_data_provider_state_running) {
                Log("accessory tracking running");
            }
        });
    // Asked once; while it is not granted the provider stays paused and only the poses are missing.
    ar_session_request_authorization(
        g_session, ar_authorization_type_accessory_tracking, ^(ar_authorization_results_t results, ar_error_t error) {
            if (error != nil || results == nil) {
                return;
            }
            ar_authorization_results_enumerate_results(results, ^bool(ar_authorization_result_t result) {
                if (ar_authorization_result_get_authorization_type(result) == ar_authorization_type_accessory_tracking) {
                    Log("accessory tracking %s", ar_authorization_result_get_status(result) ==
                                                         ar_authorization_status_allowed
                                                     ? "allowed"
                                                     : "not allowed");
                }
                return true;
            });
        });
    ar_data_providers_t providers = ar_data_providers_create();
    ar_data_providers_add_data_provider(providers, g_provider);
    ar_session_run(g_session, providers);
    Log("accessory tracking session started for %zu controller(s)", count);
}

void Load(GCController* device) {
    if (!IsSpatialController(device)) {
        return;
    }
    {
        std::lock_guard lock(g_mutex);
        if (!g_started || FindLocked(device) != nullptr) {
            return;
        }
        Controller controller;
        controller.device = device;
        g_controllers.push_back(controller);
        g_generation.fetch_add(1);
    }
    Log("spatial controller connected: %s", device.vendorName.UTF8String ?: "(unnamed)");
    ar_accessory_load_from_device(device, ^(id<GCDevice> loaded, bool successful, ar_error_t error,
                                            ar_accessory_t accessory) {
        std::lock_guard lock(g_mutex);
        Controller* controller = FindLocked(static_cast<GCController*>(loaded));
        if (controller == nullptr) {
            return; // disconnected meanwhile
        }
        if (!successful || accessory == nil) {
            Log("ARKit could not load %s as an accessory (error %ld): its pose will not be tracked",
                device.vendorName.UTF8String ?: "a controller",
                error != nil ? static_cast<long>(ar_error_get_error_code(error)) : 0L);
            return;
        }
        controller->accessory = accessory;
        controller->inherentChirality = ar_accessory_get_inherent_chirality(accessory);
        RestartSessionLocked();
    });
}

void Unload(GCController* device) {
    std::lock_guard lock(g_mutex);
    const auto it = std::find_if(g_controllers.begin(), g_controllers.end(),
                                 [device](const Controller& controller) { return controller.device == device; });
    if (it == g_controllers.end()) {
        return;
    }
    if (it->haptics != nil) {
        [it->haptics stopWithCompletionHandler:nil];
    }
    const bool tracked = it->accessory != nil;
    g_controllers.erase(it);
    Log("spatial controller disconnected");
    if (tracked) {
        RestartSessionLocked();
    } else {
        g_generation.fetch_add(1);
    }
}

// The anchor of `controller` among the provider's latest ones, or nil.
ar_accessory_anchor_t LatestAnchorLocked(const Controller& controller) noexcept {
    if (g_provider == nil || controller.accessory == nil) {
        return nil;
    }
    ar_accessory_anchors_t anchors = ar_accessory_tracking_provider_get_latest_anchors(g_provider);
    if (anchors == nil) {
        return nil;
    }
    __block ar_accessory_anchor_t found = nil;
    ar_accessory_t wanted = controller.accessory;
    ar_accessory_anchors_enumerate_anchors(anchors, ^bool(ar_accessory_anchor_t anchor) {
        if (ar_accessory_is_equal_to_accessory(ar_accessory_anchor_get_accessory(anchor), wanted)) {
            found = anchor;
            return false;
        }
        return true;
    });
    return found;
}

// 0 left, 1 right: the hand holding it when ARKit says, else the hand it is
// made for; 2 when neither is known.
uint32_t HandOfLocked(const Controller& controller) noexcept {
    ar_accessory_chirality_t chirality = ar_accessory_chirality_unspecified;
    if (ar_accessory_anchor_t anchor = LatestAnchorLocked(controller);
        anchor != nil && ar_accessory_anchor_is_held(anchor)) {
        chirality = ar_accessory_anchor_get_held_chirality(anchor);
    }
    if (chirality == ar_accessory_chirality_unspecified) {
        chirality = controller.inherentChirality;
    }
    if (chirality == ar_accessory_chirality_left) {
        return 0;
    }
    if (chirality == ar_accessory_chirality_right) {
        return 1;
    }
    return 2;
}

// The controller assigned to each hand. A controller of unknown hand takes the
// right hand when it is free, then the left (a single controller is a gun hand).
std::array<Controller*, 2> AssignLocked() noexcept {
    std::array<Controller*, 2> hands{};
    std::vector<Controller*> unknown;
    for (Controller& controller : g_controllers) {
        const uint32_t hand = HandOfLocked(controller);
        if (hand < 2 && hands[hand] == nullptr) {
            hands[hand] = &controller;
        } else {
            unknown.push_back(&controller);
        }
    }
    for (Controller* controller : unknown) {
        if (hands[1] == nullptr) {
            hands[1] = controller;
        } else if (hands[0] == nullptr) {
            hands[0] = controller;
        }
    }
    return hands;
}

float LinearValue(id<GCLinearInput> input) noexcept { return input != nil ? std::clamp(input.value, 0.0f, 1.0f) : 0.0f; }

void ReadButtons(GCController* device, ControllerSample& sample) noexcept {
    GCControllerLiveInput* input = device.input;
    if (input == nil) {
        return;
    }
    const auto button = [input](GCInputButtonName name) -> id<GCButtonElement> {
        return [input.buttons elementForAlias:name];
    };
    const auto pressed = [](id<GCButtonElement> element) { return element != nil && element.pressedInput.pressed; };
    const auto touched = [](id<GCButtonElement> element) {
        return element != nil && element.touchedInput != nil && element.touchedInput.touched;
    };
    id<GCButtonElement> trigger = button(GCInputTrigger);
    id<GCButtonElement> grip = button(GCInputGripButton);
    id<GCButtonElement> a = button(GCInputButtonA);
    id<GCButtonElement> b = button(GCInputButtonB);
    id<GCButtonElement> menu = button(GCInputButtonMenu);
    id<GCButtonElement> stickClick = button(GCInputThumbstickButton);
    sample.trigger = trigger != nil ? LinearValue(trigger.pressedInput) : 0.0f;
    if (trigger != nil && sample.trigger == 0.0f && trigger.pressedInput.pressed) {
        sample.trigger = 1.0f;
    }
    sample.triggerTouched = touched(trigger);
    sample.squeeze = grip != nil ? LinearValue(grip.pressedInput) : 0.0f;
    if (grip != nil && sample.squeeze == 0.0f && grip.pressedInput.pressed) {
        sample.squeeze = 1.0f;
    }
    sample.primary = pressed(a);
    sample.secondary = pressed(b);
    sample.primaryTouched = touched(a) || sample.primary;
    sample.secondaryTouched = touched(b) || sample.secondary;
    sample.menu = pressed(menu);
    sample.thumbstickClick = pressed(stickClick);
    sample.thumbstickTouched = touched(stickClick) || sample.thumbstickClick;
    if (id<GCDirectionPadElement> stick = [input.dpads elementForAlias:GCInputThumbstick]; stick != nil) {
        const GCPoint2 value = stick.xyAxes.value;
        sample.thumbstickX = std::clamp(value.x, -1.0f, 1.0f);
        sample.thumbstickY = std::clamp(value.y, -1.0f, 1.0f);
        if (sample.thumbstickX != 0.0f || sample.thumbstickY != 0.0f) {
            sample.thumbstickTouched = true;
        }
    }
}

} // namespace

void StartAccessoryTracking() noexcept {
    {
        std::lock_guard lock(g_mutex);
        if (g_started) {
            return;
        }
        g_started = true;
        g_hapticsQueue = dispatch_queue_create("org.primedgun.vision.haptics", DISPATCH_QUEUE_SERIAL);
    }
    NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
    g_connectObserver = [center addObserverForName:GCControllerDidConnectNotification
                                            object:nil
                                             queue:nil
                                        usingBlock:^(NSNotification* notification) {
                                            Load(static_cast<GCController*>(notification.object));
                                        }];
    g_disconnectObserver = [center addObserverForName:GCControllerDidDisconnectNotification
                                               object:nil
                                                queue:nil
                                           usingBlock:^(NSNotification* notification) {
                                               Unload(static_cast<GCController*>(notification.object));
                                           }];
    // Controllers paired before the game started are already in the list (the
    // notification only reports the later ones).
    for (GCController* controller in GCController.controllers) {
        Load(controller);
    }
    Log("watching for spatial controllers (accessory tracking %s)",
        ar_accessory_tracking_provider_is_supported() ? "supported" : "not supported");
}

void StopAccessoryTracking() noexcept {
    NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
    if (g_connectObserver != nil) {
        [center removeObserver:g_connectObserver];
        g_connectObserver = nil;
    }
    if (g_disconnectObserver != nil) {
        [center removeObserver:g_disconnectObserver];
        g_disconnectObserver = nil;
    }
    std::lock_guard lock(g_mutex);
    if (g_session != nil) {
        ar_session_stop(g_session);
        g_session = nil;
        g_provider = nil;
    }
    for (Controller& controller : g_controllers) {
        if (controller.haptics != nil) {
            [controller.haptics stopWithCompletionHandler:nil];
        }
    }
    g_controllers.clear();
    g_started = false;
    g_generation.fetch_add(1);
}

uint64_t ControllerGeneration() noexcept { return g_generation.load(); }

void SampleControllers(std::array<ControllerSample, 2>& controllers) noexcept {
    controllers = {};
    std::lock_guard lock(g_mutex);
    const std::array<Controller*, 2> hands = AssignLocked();
    for (uint32_t hand = 0; hand < 2; ++hand) {
        if (hands[hand] == nullptr) {
            continue;
        }
        controllers[hand].connected = true;
        ReadButtons(hands[hand]->device, controllers[hand]);
    }
}

bool ControllerPose(uint32_t hand, bool aim, int64_t timeNanos, simd_float4x4& worldFromPose,
                    XrSpaceLocationFlags& flags, simd_float3* linearVelocity) noexcept {
    flags = 0;
    if (hand > 1) {
        return false;
    }
    std::lock_guard lock(g_mutex);
    const std::array<Controller*, 2> hands = AssignLocked();
    if (hands[hand] == nullptr) {
        return false;
    }
    ar_accessory_anchor_t anchor = LatestAnchorLocked(*hands[hand]);
    if (anchor == nil || !ar_accessory_anchor_is_tracked(anchor)) {
        return false;
    }
    const ar_accessory_anchor_tracking_state_t state = ar_accessory_anchor_get_tracking_state(anchor);
    if (state == ar_accessory_anchor_tracking_state_untracked) {
        return false;
    }
    // ARKit predicts from its latest inertial data; the latest anchor serves when it cannot.
    if (timeNanos > 0 && g_predicted != nil &&
        ar_accessory_tracking_provider_predict_anchor_at_timestamp(g_provider, anchor, NanosToSeconds(timeNanos),
                                                                   g_predicted)) {
        anchor = g_predicted;
    }
    // The pose as the wearer sees the controller (the correction the system's own rendering uses).
    const simd_float4x4 originFromAnchor =
        ar_accessory_anchor_get_origin_from_anchor_transform_with_correction(anchor, ar_transform_correction_rendered);
    const simd_float4x4 anchorFromLocation = ar_accessory_anchor_get_anchor_from_location_transform_with_correction(
        anchor, aim ? ar_accessory_location_name_aim : ar_accessory_location_name_grip, ar_transform_correction_rendered);
    worldFromPose = simd_mul(originFromAnchor, anchorFromLocation);
    flags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
    if (state != ar_accessory_anchor_tracking_state_orientation_tracked) {
        flags |= XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    }
    if (linearVelocity != nullptr) {
        // ARKit gives the velocity in the anchor's own axes.
        const simd_float3 local = ar_accessory_anchor_get_velocity(anchor);
        const simd_float3x3 rotation =
            simd_matrix(originFromAnchor.columns[0].xyz, originFromAnchor.columns[1].xyz, originFromAnchor.columns[2].xyz);
        *linearVelocity = simd_mul(rotation, local);
    }
    return true;
}

void ControllerHaptic(uint32_t hand, float amplitude, int64_t durationNanos) noexcept {
    if (hand > 1 || amplitude <= 0.0f) {
        return;
    }
    CHHapticEngine* engine = nil;
    {
        std::lock_guard lock(g_mutex);
        const std::array<Controller*, 2> hands = AssignLocked();
        Controller* controller = hands[hand];
        if (controller == nullptr || controller->hapticsFailed) {
            return;
        }
        if (controller->haptics == nil) {
            GCDeviceHaptics* haptics = controller->device.haptics;
            controller->haptics = haptics != nil ? [haptics createEngineWithLocality:GCHapticsLocalityDefault] : nil;
            if (controller->haptics == nil) {
                controller->hapticsFailed = true;
                Log("controller haptics are not available");
                return;
            }
            controller->haptics.playsHapticsOnly = YES;
            controller->haptics.autoShutdownEnabled = YES;
        }
        engine = controller->haptics;
    }
    // OpenXR's minimum duration is -1; a pulse that short still has to be felt.
    const double seconds = std::clamp(durationNanos > 0 ? NanosToSeconds(durationNanos) : 0.02, 0.01, 2.0);
    const float intensity = std::clamp(amplitude, 0.0f, 1.0f);
    // Core Haptics may block briefly; the pacing thread must not.
    dispatch_async(g_hapticsQueue, ^{
        NSError* error = nil;
        if (![engine startAndReturnError:&error]) {
            return;
        }
        CHHapticEventParameter* strength = [[CHHapticEventParameter alloc]
            initWithParameterID:CHHapticEventParameterIDHapticIntensity
                          value:intensity];
        CHHapticEventParameter* sharpness =
            [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticSharpness value:0.5f];
        CHHapticEvent* event = [[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous
                                                             parameters:@[ strength, sharpness ]
                                                           relativeTime:0
                                                               duration:seconds];
        CHHapticPattern* pattern = [[CHHapticPattern alloc] initWithEvents:@[ event ] parameters:@[] error:&error];
        if (pattern == nil) {
            return;
        }
        id<CHHapticPatternPlayer> player = [engine createPlayerWithPattern:pattern error:&error];
        [player startAtTime:CHHapticTimeImmediate error:&error];
    });
}

void ControllerHapticStop(uint32_t hand) noexcept {
    if (hand > 1) {
        return;
    }
    CHHapticEngine* engine = nil;
    {
        std::lock_guard lock(g_mutex);
        const std::array<Controller*, 2> hands = AssignLocked();
        if (hands[hand] != nullptr) {
            engine = hands[hand]->haptics;
        }
    }
    if (engine != nil) {
        dispatch_async(g_hapticsQueue, ^{
          [engine stopWithCompletionHandler:nil];
        });
    }
}

bool HandHasController(uint32_t hand) noexcept {
    if (hand > 1) {
        return false;
    }
    std::lock_guard lock(g_mutex);
    return AssignLocked()[hand] != nullptr;
}

} // namespace PortVr::visionos

bool xr_visionos_hand_has_controller(uint32_t hand) { return PortVr::visionos::HandHasController(hand); }
