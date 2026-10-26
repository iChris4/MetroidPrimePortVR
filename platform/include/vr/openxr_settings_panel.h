// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "vr/openxr_hand_inputs.h"
#include "vr/vr_menu.h"

#include <array>
#include <cstdint>

namespace PortVr {

// PrimedGun's in-headset settings menu (vr/vr_menu.h): its image, drawn by the
// game thread (vr_menu.cpp), is shown as an OpenXR quad layer of its own on the
// off hand or floating ahead, with the pointer hand's laser and hit dot as two
// more quads cut from the same image. The tracked controllers operate it and
// are withheld from the game while it is open; the game is not paused.
//
// The XR pacing thread owns the controllers: it opens and closes the menu,
// places it, aims the laser and publishes the pointer here. The game thread
// reads the pointer once per presented frame and may close the menu too.
// Nothing in this header depends on OpenXR, so the menu compiles the same in
// builds without it.

// The panel swapchain's image: PrimedGun's 1024x512 canvas plus the sprite strip.
inline constexpr float kSettingsPanelWidthPixels = static_cast<float>(VrMenu::kWidth);
inline constexpr float kSettingsPanelHeightPixels = static_cast<float>(VrMenu::kImageHeight);

struct OpenXRSettingsPanelPointer {
    bool valid = false;
    float x = 0.0f; // canvas pixels, +x right
    float y = 0.0f; // canvas pixels, +y down
    // Clicks since the last read, the last one where it landed: a press must
    // not be lost when the game thread misses the frames it was held.
    uint32_t clicks = 0;
    bool click_valid = false;
    float click_x = 0.0f;
    float click_y = 0.0f;
};

// Either thread.
void OpenXRSetSettingsPanelOpen(bool open) noexcept;
bool OpenXRSettingsPanelOpen() noexcept;
// XR thread: where the laser meets the canvas this frame, and whether a click
// started there.
void OpenXRPublishSettingsPanelPointer(bool valid, float x, float y, bool clicked) noexcept;
// Game thread: the latest pointer, taking the clicks made since the last call.
OpenXRSettingsPanelPointer OpenXRTakeSettingsPanelPointer() noexcept;
// The shader compilation toast (VrMenu::BuildCompileToastPixels) has the panel's
// image while the menu is closed: set by the game thread, which draws it, read by
// the XR thread, which places it. Either thread.
void OpenXRSetCompileToastShown(bool shown) noexcept;
bool OpenXRCompileToastShown() noexcept;

namespace settings_panel {

using PortVr::HandInputs;

// What opens the menu, besides the hands' buttons.
struct ToggleOptions {
    // Whether the menu can be shown at all: the VR overlays are on and the
    // backend has a layer for it. Off, it closes and stays closed.
    bool allowed = true;
    // PrimedGun points with the cannon hand, independently of menu activation.
    uint32_t pointing_hand = 1;
    // "LONGER HELD PRESS FOR VR MENU": opening takes kLongPressSeconds of
    // holding; closing is still one press.
    bool long_press = false;
    float long_press_seconds = static_cast<float>(VrMenu::kLongPressSeconds);
    // "MENU REQUIRES HAND NEAR HEAD": opening also needs the physical left hand in the
    // visor gesture's zone beside the head (VisorDpad::HandNearHead).
    bool needs_head_zone = false;
    bool hand_near_head = false;
};

// What the controllers do this frame.
struct Frame {
    bool open = false;     // the menu is showing
    bool withheld = false; // the game must not see the controllers
    uint32_t pointing_hand = 1;
    bool select = false;   // the pointer hand's trigger or A held
    bool clicked = false;  // ... and pressed this frame
};

// Whether the button that opens and closes the menu is held. PrimedGun: the
// physical left thumbstick click or menu button (right stick click is always
// "SET HEIGHT", regardless of handedness). As a gamepad every button means something to the game, so
// both thumbsticks clicked together stand in for it; with the controllers
// nothing to the game it is left Y.
inline bool ToggleHeld(const std::array<HandInputs, 2>& hands, OpenXRControllerMode mode) noexcept {
    switch (mode) {
    case OpenXRControllerMode::PrimedGun:
        return hands[0].thumbstick_click || hands[0].menu;
    case OpenXRControllerMode::Gamepad:
        return hands[0].thumbstick_click && hands[1].thumbstick_click;
    case OpenXRControllerMode::None:
        break;
    }
    return hands[0].secondary;
}

// The controller side of the menu, one Update per XR frame:
//
// - The toggle (ToggleHeld, with PrimedGun's long press and head zone options)
//   opens or closes it.
// - While it is open, the pointing hand's trigger or A / X selects. PrimedGun
//   points with the cannon hand; the other modes with the hand whose trigger
//   was pulled last.
// - The game gets the controllers back once menu buttons/triggers are released, so the
//   press that closed the menu never lands in the game as well.
class Controls {
public:
    // `open` is the shared flag: the toggle flips it, and the game thread may
    // have changed it since the last frame. `dt_seconds` is the time since the
    // previous frame; `mode` is how the game sees the controllers.
    Frame Update(const std::array<HandInputs, 2>& hands, bool& open, float dt_seconds, OpenXRControllerMode mode,
                 const ToggleOptions& options) noexcept {
        const bool primedgun = mode == OpenXRControllerMode::PrimedGun;
        const bool toggle = ToggleHeld(hands, mode) &&
                            (!options.needs_head_zone || open || options.hand_near_head || !primedgun);
        if (!options.allowed) {
            open = false;
            m_hold_seconds = 0.0f;
            m_hold_consumed = toggle;
        } else if (options.long_press && !open) {
            if (!toggle) {
                m_hold_seconds = 0.0f;
                m_hold_consumed = false;
            } else if (!m_hold_consumed) {
                m_hold_seconds += dt_seconds;
                if (m_hold_seconds >= std::clamp(options.long_press_seconds, 0.01f, 5.0f)) {
                    open = true;
                    m_hold_consumed = true;
                }
            }
        } else if (toggle && !m_toggle_held) {
            open = !open;
            // A press that closes the menu must not count towards reopening it.
            m_hold_consumed = true;
            m_hold_seconds = 0.0f;
        } else if (!toggle) {
            m_hold_consumed = false;
            m_hold_seconds = 0.0f;
        }
        m_toggle_held = toggle;

        // Outside PrimedGun's scheme the left menu button also closes it.
        if (!primedgun && open && hands[0].menu && !m_menu_held && m_was_open) {
            open = false;
        }
        m_menu_held = hands[0].menu;

        for (uint32_t hand = 0; hand < 2; ++hand) {
            const bool pulled = hands[hand].trigger > kPressThreshold;
            if (pulled && !m_trigger_held[hand]) {
                m_last_trigger_hand = hand;
            }
            m_trigger_held[hand] = pulled;
        }
        const uint32_t pointing = primedgun ? (options.pointing_hand & 1u) : m_last_trigger_hand;

        if (open != m_was_open) {
            // Whatever is held across the change belongs to that change.
            m_release_pending = true;
        }
        if (m_release_pending && !AnyHeld(hands)) {
            m_release_pending = false;
        }
        m_was_open = open;

        Frame frame{};
        frame.open = open;
        frame.withheld = open || m_release_pending;
        frame.pointing_hand = pointing;
        bool select = false;
        if (open && !m_release_pending) {
            // Opening held nothing that selects, but a trigger still down from
            // the game must not click the first thing under the pointer.
            select = primedgun ? hands[pointing].primary || m_trigger_held[pointing]
                               : m_trigger_held[0] || m_trigger_held[1] || hands[0].primary || hands[1].primary;
        }
        frame.select = select;
        frame.clicked = select && !m_select_held;
        m_select_held = select;
        return frame;
    }

    void Reset() noexcept { *this = Controls{}; }

    // Grips may stay held while pointing, selecting, and returning to gameplay.
    // Custom grips bound to menu actions arrive as logical buttons/triggers.
    static bool AnyHeld(const std::array<HandInputs, 2>& hands) noexcept {
        for (const HandInputs& hand : hands) {
            if (hand.primary || hand.secondary || hand.menu || hand.thumbstick_click ||
                hand.trigger > kPressThreshold) {
                return true;
            }
        }
        return false;
    }

private:
    bool m_toggle_held = false;
    float m_hold_seconds = 0.0f;
    bool m_hold_consumed = false;
    bool m_menu_held = false;
    std::array<bool, 2> m_trigger_held{};
    uint32_t m_last_trigger_hand = 1;
    bool m_was_open = false;
    bool m_release_pending = false;
    bool m_select_held = false;
};

} // namespace settings_panel

} // namespace PortVr
