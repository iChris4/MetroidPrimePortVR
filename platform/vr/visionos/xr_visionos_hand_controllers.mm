// SPDX-License-Identifier: GPL-3.0-or-later
//
// Nintendo Switch Joy-Cons as tracked controllers for the visionOS OpenXR
// provider.
//
// visionOS pairs Joy-Cons as plain GameController gamepads: buttons and sticks,
// no pose. A pair the system combined into one controller (its product category
// "Nintendo Switch Joy-Con (L/R)"; holding Capture and Home toggles it) is
// still two pieces, one in each hand, and ARKit tracks the hands that hold
// them. So each half plays one hand's Touch controller, as a Sense controller
// does (xr_visionos_accessories.mm): its buttons from GameController
// (MapJoyConPair, platform/include/vr/vr_hand_controller.h, the Sense layout by
// the Nintendo labels), its aim and grip poses from that hand's skeleton,
// predicted by ARKit to the asked-for time, in the frame of a fist closed on an
// upright controller (HoldingFrame) and smoothed (PoseSmoother), its rumble
// through Core Haptics on that Joy-Con. The game then plays the Sense
// controllers' controls: the arm cannon follows the hand.
//
// The pair is claimed only while the setting vr_joycon_hand_tracking is on
// (xr_visionos_set_hand_tracked_gamepads) and hand tracking runs here, allowed
// by the wearer: without it the Joy-Cons stay the gamepad they were, the
// original controls (and in the simulator, which has no hands). While claimed,
// Aurora leaves the pair out of its gamepads (lib/input_visionos.mm), or every
// press would reach the game twice. A Sense controller in a hand comes first.
// A single Joy-Con is a sideways mini gamepad without its ZL/ZR and is left
// alone.

#include "xr_visionos_internal.h"

#include "vr/vr_hand_controller.h"

#import <CoreHaptics/CoreHaptics.h>
#import <GameController/GameController.h>

#include <algorithm>
#include <cmath>

namespace PortVr::visionos {
namespace {

namespace HC = PortVr::HandController;

NSString* const kJoyConCategoryPrefix = @"Nintendo Switch Joy-Con";
NSString* const kJoyConPairCategory = @"Nintendo Switch Joy-Con (L/R)";

// The aim's tilt above the holding frame's forward (wrist to index knuckle),
// in degrees. A start value: the VR menu's cannon calibration fine-tunes it.
constexpr float kHandControllerAimPitchDegrees = 0.0f;
// How long a hand ARKit lost keeps its last pose, as xrSyncActions keeps a
// hand: through a blink of the cameras, not through a hand gone from view.
constexpr int64_t kHoldNanos = 250'000'000;
// The Joy-Cons' motors are strong next to the Sense controllers' at the same
// amplitude: full strength was far too much on a headset, so the rumble setting
// is scaled to this share of it.
constexpr float kRumbleScale = 0.3f;

std::mutex g_mutex;
bool g_started = false;
// The combined pair, while one is connected.
GCController* g_pair = nil;
std::array<CHHapticEngine*, 2> g_haptics{};
std::array<bool, 2> g_hapticsFailed{};
id g_connectObserver = nil;
id g_disconnectObserver = nil;
dispatch_queue_t g_hapticsQueue = nil;

std::atomic_bool g_enabled{true};
std::atomic_bool g_claimed{false};
std::atomic<uint64_t> g_generation{0};

// Each hand's pose filter. Read and written from xrLocateSpace (under the
// instance mutex) and reset from Start/Stop.
struct PoseState {
    HC::PoseSmoother smoother;
    int64_t lastSeenNanos = 0;
    bool measured = false;
    // The output before the latest, for the velocity.
    HC::HandPose previous{};
    int64_t previousNanos = 0;
};
std::mutex g_poseMutex;
std::array<PoseState, 2> g_poses{};

bool IsJoyCon(GCController* controller) noexcept {
    return controller != nil && [controller.productCategory hasPrefix:kJoyConCategoryPrefix];
}

bool IsJoyConPair(GCController* controller) noexcept {
    return controller != nil && [controller.productCategory isEqualToString:kJoyConPairCategory];
}

// Every element the controller offers, for checking the layout on a headset.
void LogElements(GCController* controller) {
    GCPhysicalInputProfile* profile = controller.physicalInputProfile;
    if (profile == nil) {
        return;
    }
    NSArray<NSString*>* buttons = [profile.buttons.allKeys sortedArrayUsingSelector:@selector(compare:)];
    NSArray<NSString*>* dpads = [profile.dpads.allKeys sortedArrayUsingSelector:@selector(compare:)];
    Log("  buttons: %s", [buttons componentsJoinedByString:@", "].UTF8String ?: "");
    Log("  sticks and pads: %s", [dpads componentsJoinedByString:@", "].UTF8String ?: "");
    Log("  extended gamepad profile: %s", controller.extendedGamepad != nil ? "yes" : "no");
}

void ResetPoses() noexcept {
    std::lock_guard lock(g_poseMutex);
    for (PoseState& state : g_poses) {
        state = PoseState{};
    }
}

void StopHapticsLocked() noexcept {
    for (size_t hand = 0; hand < g_haptics.size(); ++hand) {
        if (g_haptics[hand] != nil) {
            [g_haptics[hand] stopWithCompletionHandler:nil];
        }
        g_haptics[hand] = nil;
    }
    g_hapticsFailed = {};
}

void Connected(GCController* device) {
    if (!IsJoyCon(device)) {
        return;
    }
    Log("Joy-Con connected: %s (%s)", device.productCategory.UTF8String ?: "",
        device.vendorName.UTF8String ?: "unnamed");
    LogElements(device);
    if (!IsJoyConPair(device)) {
        Log("a single Joy-Con stays a gamepad: hold Capture and Home together for a few seconds to combine the "
            "pair, which plays as tracked controllers");
        return;
    }
    std::lock_guard lock(g_mutex);
    if (!g_started || g_pair == device) {
        return;
    }
    StopHapticsLocked();
    g_pair = device;
    g_generation.fetch_add(1);
}

void Disconnected(GCController* device) {
    std::lock_guard lock(g_mutex);
    if (device == nil || device != g_pair) {
        return;
    }
    StopHapticsLocked();
    g_pair = nil;
    g_generation.fetch_add(1);
    Log("Joy-Con pair disconnected");
}

// Whether the pair plays as controllers now; logs and bumps the generation when
// that changes. Called with g_mutex held.
bool UpdateClaimLocked() noexcept {
    const Compositor& compositor = Compositor::Get();
    const bool handsReady = compositor.HandTrackingSupported() && compositor.HandTrackingAuthorized();
    const bool claimed = g_started && g_pair != nil && g_enabled.load() && handsReady;
    if (claimed != g_claimed.load()) {
        g_claimed.store(claimed);
        g_generation.fetch_add(1);
        if (claimed) {
            Log("the Joy-Con pair plays as hand-tracked controllers");
        } else if (g_pair != nil) {
            Log("the Joy-Con pair is a gamepad (%s)", !g_enabled.load() ? "setting off"
                                                      : !handsReady     ? "no hand tracking"
                                                                        : "stopped");
        }
        ResetPoses();
    }
    return claimed;
}

HC::JoyConPairButtons ReadPair(GCController* device) noexcept {
    HC::JoyConPairButtons out;
    GCExtendedGamepad* pad = device.extendedGamepad;
    if (pad == nil) {
        return out;
    }
    const auto down = [](GCControllerButtonInput* button) { return button != nil && button.isPressed; };
    out.a = down(pad.buttonA);
    out.b = down(pad.buttonB);
    out.x = down(pad.buttonX);
    out.y = down(pad.buttonY);
    out.dpadUp = down(pad.dpad.up);
    out.dpadDown = down(pad.dpad.down);
    out.dpadLeft = down(pad.dpad.left);
    out.dpadRight = down(pad.dpad.right);
    out.l = down(pad.leftShoulder);
    out.r = down(pad.rightShoulder);
    out.zl = down(pad.leftTrigger);
    out.zr = down(pad.rightTrigger);
    out.minus = down(pad.buttonOptions);
    out.plus = down(pad.buttonMenu);
    out.leftStickClick = down(pad.leftThumbstickButton);
    out.rightStickClick = down(pad.rightThumbstickButton);
    out.leftStickX = std::clamp(pad.leftThumbstick.xAxis.value, -1.0f, 1.0f);
    out.leftStickY = std::clamp(pad.leftThumbstick.yAxis.value, -1.0f, 1.0f);
    out.rightStickX = std::clamp(pad.rightThumbstick.xAxis.value, -1.0f, 1.0f);
    out.rightStickY = std::clamp(pad.rightThumbstick.yAxis.value, -1.0f, 1.0f);
    return out;
}

HC::Vec3 ToVec(simd_float3 v) noexcept { return {v.x, v.y, v.z}; }
simd_float3 ToSimd(HC::Vec3 v) noexcept { return simd_make_float3(v.x, v.y, v.z); }

// The unsmoothed pose of a hand holding a Joy-Con upright. Estimated joints
// serve while the hand itself is tracked (fingers closed on a controller hide
// some of it); `measured` says whether ARKit sees the joints the frame is made
// of.
bool RawPose(const HandSample& sample, HC::HandPose& pose, bool& measured) noexcept {
    if (!sample.tracked) {
        return false;
    }
    const HandJointSample& wrist = sample.wrist();
    const HandJointSample& index = sample.indexKnuckle();
    const HandJointSample& middle = sample.middleKnuckle();
    const HandJointSample& little = sample.joint(XR_HAND_JOINT_LITTLE_PROXIMAL_EXT);
    HC::Frame frame;
    if (!HC::HoldingFrame(ToVec(wrist.position), ToVec(index.position), ToVec(little.position), 0.0f, frame)) {
        return false;
    }
    pose.rotation = HC::QuatFromBasis(frame.right, frame.up, frame.back);
    pose.aimOrigin = ToVec(index.position);
    pose.gripOrigin = ToVec((wrist.position + middle.position) * 0.5f);
    measured = wrist.tracked && index.tracked && little.tracked;
    return true;
}

simd_quatf ToSimd(HC::Quat q) noexcept { return simd_quaternion(q.x, q.y, q.z, q.w); }

CHHapticEngine* EngineLocked(uint32_t hand) noexcept {
    if (g_pair == nil || g_hapticsFailed[hand]) {
        return nil;
    }
    if (g_haptics[hand] == nil) {
        GCDeviceHaptics* haptics = g_pair.haptics;
        CHHapticEngine* engine = nil;
        if (haptics != nil) {
            GCHapticsLocality locality = hand == 0 ? GCHapticsLocalityLeftHandle : GCHapticsLocalityRightHandle;
            if ([haptics.supportedLocalities containsObject:locality]) {
                engine = [haptics createEngineWithLocality:locality];
            }
            if (engine == nil) {
                engine = [haptics createEngineWithLocality:GCHapticsLocalityDefault];
            }
        }
        if (engine == nil) {
            g_hapticsFailed[hand] = true;
            Log("the Joy-Con pair has no haptics here (%s hand)", hand == 0 ? "left" : "right");
            return nil;
        }
        engine.playsHapticsOnly = YES;
        engine.autoShutdownEnabled = YES;
        g_haptics[hand] = engine;
    }
    return g_haptics[hand];
}

} // namespace

void StartHandControllers() noexcept {
    {
        std::lock_guard lock(g_mutex);
        if (g_started) {
            return;
        }
        g_started = true;
        g_hapticsQueue = dispatch_queue_create("org.primedgun.vision.joycon-haptics", DISPATCH_QUEUE_SERIAL);
    }
    ResetPoses();
    NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
    g_connectObserver = [center addObserverForName:GCControllerDidConnectNotification
                                            object:nil
                                             queue:nil
                                        usingBlock:^(NSNotification* notification) {
                                            Connected(static_cast<GCController*>(notification.object));
                                        }];
    g_disconnectObserver = [center addObserverForName:GCControllerDidDisconnectNotification
                                               object:nil
                                                queue:nil
                                           usingBlock:^(NSNotification* notification) {
                                               Disconnected(static_cast<GCController*>(notification.object));
                                           }];
    // Controllers paired before the game started are already in the list.
    for (GCController* controller in GCController.controllers) {
        Connected(controller);
    }
    Log("watching for Joy-Cons (hand-tracked controllers %s)", g_enabled.load() ? "on" : "off");
}

void StopHandControllers() noexcept {
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
    StopHapticsLocked();
    g_pair = nil;
    g_started = false;
    g_claimed.store(false);
    g_generation.fetch_add(1);
    ResetPoses();
}

uint64_t HandControllerGeneration() noexcept { return g_generation.load(); }

void SampleHandControllers(std::array<ControllerSample, 2>& controllers) noexcept {
    std::lock_guard lock(g_mutex);
    if (!UpdateClaimLocked()) {
        return;
    }
    const std::array<HC::HandButtons, 2> hands = HC::MapJoyConPair(ReadPair(g_pair));
    for (uint32_t hand = 0; hand < 2; ++hand) {
        ControllerSample& sample = controllers[hand];
        if (sample.connected) {
            continue; // a Sense controller is in this hand
        }
        const HC::HandButtons& in = hands[hand];
        sample.connected = true;
        sample.source = ControllerSource::HandTracked;
        sample.trigger = in.trigger;
        sample.triggerTouched = in.trigger > 0.0f;
        sample.squeeze = in.squeeze;
        sample.primary = in.primary;
        sample.secondary = in.secondary;
        sample.primaryTouched = in.primary;
        sample.secondaryTouched = in.secondary;
        sample.menu = in.menu;
        sample.thumbstickClick = in.thumbstickClick;
        sample.thumbstickX = in.thumbstickX;
        sample.thumbstickY = in.thumbstickY;
        sample.thumbstickTouched = in.thumbstickClick || in.thumbstickX != 0.0f || in.thumbstickY != 0.0f;
    }
}

bool HandControllerInHand(uint32_t hand) noexcept {
    if (hand > 1) {
        return false;
    }
    std::lock_guard lock(g_mutex);
    return g_claimed.load() && g_pair != nil;
}

bool HandControllerPose(Session& session, uint32_t hand, bool aim, int64_t timeNanos, simd_float4x4& worldFromPose,
                        XrSpaceLocationFlags& flags, simd_float3* linearVelocity) noexcept {
    flags = 0;
    if (hand > 1) {
        return false;
    }
    const int64_t now = timeNanos > 0 ? timeNanos : NowNanos();
    // The skeleton ARKit predicts for the asked-for time, so a fast swing does
    // not trail; the one the last sync read when it cannot answer.
    std::array<HandSample, 2> predicted{};
    const HandSample* sample = nullptr;
    if (timeNanos > 0 && Compositor::Get().HandsAt(timeNanos, predicted) && predicted[hand].tracked) {
        sample = &predicted[hand];
    } else if (session.hands[hand].tracked) {
        sample = &session.hands[hand];
    }

    std::lock_guard lock(g_poseMutex);
    PoseState& state = g_poses[hand];
    HC::HandPose raw;
    bool measured = false;
    HC::HandPose pose;
    bool fresh = false;
    if (sample != nullptr && RawPose(*sample, raw, measured)) {
        fresh = true;
        const int64_t before = state.smoother.LastTimeNanos();
        const HC::HandPose last = state.smoother.Last();
        const bool primed = state.smoother.Primed();
        pose = state.smoother.Update(raw, now);
        if (primed && state.smoother.LastTimeNanos() != before) {
            state.previous = last;
            state.previousNanos = before;
        }
        state.lastSeenNanos = std::max(state.lastSeenNanos, now);
        state.measured = measured;
    } else if (state.smoother.Primed() && now - state.lastSeenNanos <= kHoldNanos) {
        // Lost a moment ago: the last pose stands, no longer tracked.
        pose = state.smoother.Last();
        measured = false;
    } else {
        state = PoseState{};
        return false;
    }

    simd_float4x4 matrix = simd_matrix4x4(ToSimd(pose.rotation));
    if (aim && kHandControllerAimPitchDegrees != 0.0f) {
        const float radians = kHandControllerAimPitchDegrees * static_cast<float>(M_PI / 180.0);
        matrix = simd_mul(matrix, simd_matrix4x4(simd_quaternion(radians, simd_make_float3(1.0f, 0.0f, 0.0f))));
    }
    const HC::Vec3 origin = aim ? pose.aimOrigin : pose.gripOrigin;
    matrix.columns[3] = simd_make_float4(ToSimd(origin), 1.0f);
    worldFromPose = matrix;
    flags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    if (measured) {
        flags |= XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    }
    if (linearVelocity != nullptr) {
        *linearVelocity = simd_make_float3(0.0f, 0.0f, 0.0f);
        const int64_t dtNanos = state.smoother.LastTimeNanos() - state.previousNanos;
        if (fresh && state.previousNanos != 0 && dtNanos > 1'000'000) {
            const HC::Vec3 previous = aim ? state.previous.aimOrigin : state.previous.gripOrigin;
            const HC::Vec3 current = aim ? state.smoother.Last().aimOrigin : state.smoother.Last().gripOrigin;
            *linearVelocity = ToSimd(current - previous) * (1.0e9f / static_cast<float>(dtNanos));
        }
    }
    return true;
}

void HandControllerHaptic(uint32_t hand, float amplitude, int64_t durationNanos) noexcept {
    if (hand > 1 || amplitude <= 0.0f) {
        return;
    }
    CHHapticEngine* engine = nil;
    dispatch_queue_t queue = nil;
    {
        std::lock_guard lock(g_mutex);
        if (!g_claimed.load()) {
            return;
        }
        engine = EngineLocked(hand);
        queue = g_hapticsQueue;
    }
    if (engine == nil || queue == nil) {
        return;
    }
    const double seconds = std::clamp(durationNanos > 0 ? NanosToSeconds(durationNanos) : 0.02, 0.01, 2.0);
    const float intensity = std::clamp(amplitude, 0.0f, 1.0f) * kRumbleScale;
    // Core Haptics may block briefly; the pacing thread must not.
    dispatch_async(queue, ^{
        NSError* error = nil;
        if (![engine startAndReturnError:&error]) {
            return;
        }
        CHHapticEventParameter* strength =
            [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity value:intensity];
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

void HandControllerHapticStop(uint32_t hand) noexcept {
    if (hand > 1) {
        return;
    }
    CHHapticEngine* engine = nil;
    dispatch_queue_t queue = nil;
    {
        std::lock_guard lock(g_mutex);
        engine = g_haptics[hand];
        queue = g_hapticsQueue;
    }
    if (engine != nil && queue != nil) {
        dispatch_async(queue, ^{
          [engine stopWithCompletionHandler:nil];
        });
    }
}

} // namespace PortVr::visionos

void xr_visionos_set_hand_tracked_gamepads(bool enabled) { PortVr::visionos::g_enabled.store(enabled); }

bool xr_visionos_hand_tracked_gamepads_claimed(void) { return PortVr::visionos::g_claimed.load(); }
