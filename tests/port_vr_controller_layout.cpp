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
}
