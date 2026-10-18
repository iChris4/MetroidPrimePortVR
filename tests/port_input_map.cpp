#include "port_input_map.h"
#include "port_test.h"

#include <cstdio>
#include <cstdlib>

int main() {
  using namespace PortInputMap;

  // Names round-trip, and an unknown one is rejected.
  for (int i = 0; i < kMA_Count; ++i) {
    REQUIRE_MSG(MouseActionFromName(MouseActionInfo(i).name) == i, "action name round trip");
  }
  REQUIRE_MSG(MouseActionFromName("fire") == -1, "unknown action name");
  REQUIRE_MSG(MouseActionInfo(99).padButton == 0 && MouseActionInfo(-1).padButton == 0, "action range");

  // The defaults are the old fixed buttons: left fire, middle missile, right lock-on.
  int actions[kMouseButtonCount];
  for (int i = 0; i < kMouseButtonCount; ++i) {
    actions[i] = DefaultMouseAction(i);
  }
  REQUIRE_MSG(MouseActions(actions, 1u << 0).buttons == kPadA, "left is A");
  REQUIRE_MSG(MouseActions(actions, 1u << 1).buttons == kPadY, "middle is Y");
  REQUIRE_MSG(MouseActions(actions, 1u << 2).buttons == kPadL, "right is L");
  REQUIRE_MSG(MouseActions(actions, (1u << 3) | (1u << 4)).buttons == 0, "side buttons unbound");
  REQUIRE_MSG(MouseActions(actions, 0x7).buttons == (kPadA | kPadY | kPadL), "all three");

  // Rebound, with a shift; the menu path lets only its allowed buttons through.
  actions[3] = kMA_B;
  actions[4] = kMA_Shift;
  const SMouseResult both = MouseActions(actions, (1u << 3) | (1u << 4));
  REQUIRE_MSG(both.buttons == kPadB && both.shift, "side buttons rebound");
  const SMouseResult menu = MouseActions(actions, 0x1f, kPadA | kPadB);
  REQUIRE_MSG(menu.buttons == (kPadA | kPadB) && !menu.shift, "menu buttons");
  REQUIRE_MSG(MouseButtonsFor(actions, kPadA | kPadB) == ((1u << 0) | (1u << 3)), "buttons for A and B");
  REQUIRE_MSG(MouseButtonsFor(actions, 0) == 0, "buttons for nothing");

  std::puts("input map tests passed");
  return 0;
}
