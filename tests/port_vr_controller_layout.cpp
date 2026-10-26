#include "vr/vr_controller_layout.h"
#include "vr/vr_settings.h"
#include <cstdio>
#include <cstdlib>

static void Check(bool good, const char* what) {
    if (!good) { std::fprintf(stderr, "Custom controller layout: %s\n", what); std::abort(); }
}
int main() {
    using namespace PortVr;
    using namespace ControllerLayout;
    std::array<OpenXRControllerState, 2> hands{};
    // Independent thresholds must affect analog travel even if the runtime's click is already down.
    PortVrSettings sensitivity;
    sensitivity.custom_trigger_threshold = 0.8f;
    sensitivity.custom_grip_threshold = 0.7f;
    sensitivity.custom_touchpad_threshold = 0.6f;
    sensitivity.custom_stick_threshold = 0.9f;
    hands[1].connected = true;
    hands[1].trigger_value = 0.75f; hands[1].trigger_click = true;
    Check(!Pressed(hands, 19, Thresholds(sensitivity)), "trigger click cannot bypass analog threshold");
    hands[1].trigger_analog = true; hands[1].trigger_value = 0.f;
    Check(!Pressed(hands, 19, Thresholds(sensitivity)), "active analog channel at zero does not fall back to click");
    hands[1].trigger_value = 0.85f;
    Check(Pressed(hands, 19, Thresholds(sensitivity)), "trigger activates above configured travel");
    hands[1].squeeze_value = 0.65f; hands[1].squeeze_click = true;
    Check(!Pressed(hands, 20, Thresholds(sensitivity)), "grip threshold applies independently");
    hands[1].squeeze_force = 0.75f;
    Check(Pressed(hands, 20, Thresholds(sensitivity)), "Index grip force can activate grip");
    hands[1].trackpad_force = 0.55f; hands[1].trackpad_click = true;
    Check(!Pressed(hands, 23, Thresholds(sensitivity)), "touchpad click cannot bypass pressure threshold");
    hands[1].trackpad_force = 0.65f;
    Check(Pressed(hands, 23, Thresholds(sensitivity)), "touchpad activates above configured pressure");
    hands[1].thumbstick_y = 0.85f; hands[1].primary = true;
    Check(!Pressed(hands, 24, Thresholds(sensitivity)) && Pressed(hands, 17, Thresholds(sensitivity)),
          "stick jump threshold does not affect additional button jump");
    hands[1].thumbstick_y = 0.95f;
    Check(Pressed(hands, 24, Thresholds(sensitivity)), "stick jump activates above configured deflection");
    hands[1].trigger_value = 0.75f;
    Check(PanelInputs(hands, 5, 19, 18, true, 1, Thresholds(sensitivity))[1].trigger == 0.f,
          "overlay confirm uses the same trigger threshold");
    sensitivity.custom_trigger_threshold = 0.1f;
    hands[1].trigger_click = false; hands[1].trigger_value = 0.2f;
    Check(PanelInputs(hands, 5, 1, 18, false, 1, Thresholds(sensitivity))[1].secondary,
          "lightly held trigger still gates input when closing the overlay");
    hands = {};
    PortVrSettings layout;
    Check(!layout.custom_layout_enabled, "legacy layout remains the default");
    hands[1].connected = true;
    hands[1].secondary = true;
    Check(Pressed(hands, layout.custom_beam), "default beam hold is the right upper button");
    Check(!Pressed(hands, layout.custom_jump), "beam hold does not imply jump");
    hands[1].thumbstick_y = 0.9f;
    Check(Pressed(hands, layout.custom_jump), "stick jump remains bindable");
    layout.custom_jump_extra = 17;
    hands[1].primary = true; hands[1].thumbstick_y = 0;
    Check(Pressed(hands, layout.custom_jump_extra) && !Pressed(hands, layout.custom_jump), "extra button jump is independent of stick jump");
    hands[0].connected = true; hands[0].dpad_left = true;
    Check(Pressed(hands, layout.custom_visor_left), "Steam Frame left D-pad reaches Scan visor");
    Check(!Pressed(hands, layout.custom_visor_right), "separate D-pad directions are not aliased");
    hands[0].dpad_left = false; hands[0].dpad_up = true;
    Check(Pressed(hands, layout.custom_visor_up), "physical D-pad up reaches Combat visor");
    layout.custom_visor_hold = 20; hands[1].squeeze_value = 0.8f;
    Check(Pressed(hands, layout.custom_visor_hold), "a grip can hold the visor selector");
    hands[1].squeeze_value = 0;
    Check(!Pressed(hands, layout.custom_visor_hold), "releasing the selector releases the hold");
    hands[0].squeeze_click = true;
    Check(Pressed(hands, 4), "Vive/WMR digital grips are accepted");
    hands[1].trackpad_force = 0.8f;
    Check(Pressed(hands, 23), "Index trackpad force is bindable");
    hands[1].trigger_click = true;
    Check(Pressed(hands, layout.custom_shoot), "digital controller triggers are accepted");
    Check(Pressed(hands, layout.custom_spider), "default Morph Ball spider hold uses the right trigger");
    Check(layout.custom_menu_pause == 2 && layout.custom_back == 18, "menu pause stays separate from Back");
    hands[1].tertiary = true; hands[1].quaternary = false;
    Check(Pressed(hands, 33) && !Pressed(hands, 34), "Steam Frame X and Y stay independent");
    hands[1].shoulder = true;
    Check(Pressed(hands, 32), "Steam Frame shoulder can be mapped");
    hands[1].connected = false;
    Check(!Pressed(hands, 33) && !Pressed(hands, 19), "disconnected controllers cannot leave held input");
    Check(!Pressed(hands, 0) && !Pressed(hands, -1) && !Pressed(hands, 37), "unbound/invalid controls are idle");
    hands = {};
    hands[0].connected = hands[1].connected = true;
    hands[0].squeeze_value = hands[1].squeeze_value = 1.f;
    hands[0].squeeze_force = hands[1].squeeze_force = 1.f;
    auto heldGripPanel = PanelInputs(hands, 5, 1, 34, true, 1);
    Check(!heldGripPanel[0].secondary && !heldGripPanel[1].secondary,
          "unmapped held grips do not block custom menu clicks or gameplay return");
    auto mappedGripPanel = PanelInputs(hands, 5, 20, 34, true, 1);
    Check(mappedGripPanel[1].trigger == 1.f && mappedGripPanel[1].secondary,
          "a grip deliberately bound to confirm remains a menu press");
    hands[0].squeeze_value = hands[1].squeeze_value = 0.f;
    hands[0].squeeze_force = hands[1].squeeze_force = 0.f;
    hands[1].trigger_value = 1.f;
    auto panel = PanelInputs(hands, 5, 1, 34, true, 1);
    Check(panel[1].trigger == 0.f && panel[1].secondary, "old trigger does not select; it still gates release");
    hands[0].primary = true;
    panel = PanelInputs(hands, 5, 1, 34, true, 1);
    Check(panel[1].trigger == 1.f, "custom confirm selects the overlay with the pointing hand");
    hands[1].quaternary = true;
    panel = PanelInputs(hands, 5, 1, 34, true, 1);
    Check(panel[0].thumbstick_click, "custom Back closes an open overlay");
    panel = PanelInputs(hands, 5, 1, 34, false, 1);
    Check(!panel[0].thumbstick_click && panel[1].secondary, "held Back cannot reopen or leak through after closing");
}
