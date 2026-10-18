#include "port_map_pickups.h"
#include "port_test.h"

#include <cstdio>
#include <cstdlib>

int main() {
  size_t count = 0;
  const PortMapPickups::Dot* dots = PortMapPickups::Dots(count);
  REQUIRE(count > 0 && count <= PortMapPickups::kMaxDots);

  // Every dot finds itself, and a point a little off it too.
  for (size_t i = 0; i < count; ++i) {
    const float exact[3] = {dots[i].pos[0], dots[i].pos[1], dots[i].pos[2]};
    REQUIRE(PortMapPickups::DotIndex(dots[i].world, dots[i].area, exact) == static_cast< int >(i));
    const float near[3] = {exact[0] + 0.5f, exact[1] - 0.5f, exact[2] + 0.5f};
    REQUIRE(PortMapPickups::DotIndex(dots[i].world, dots[i].area, near) == static_cast< int >(i));
  }
  const float far[3] = {dots[0].pos[0] + 50.f, dots[0].pos[1], dots[0].pos[2]};
  REQUIRE(PortMapPickups::DotIndex(dots[0].world, dots[0].area, far) == -1);
  REQUIRE(PortMapPickups::DotIndex(0x12345678u, dots[0].area, dots[0].pos) == -1);

  // The seen bitset: each dot has its own bit, setting is idempotent.
  uint32_t words[PortMapPickups::kSeenWords] = {};
  for (size_t i = 0; i < count; ++i) {
    REQUIRE(!PortMapPickups::IsSeen(words, i));
  }
  for (size_t i = 0; i < count; i += 3) {
    PortMapPickups::SetSeen(words, i);
    PortMapPickups::SetSeen(words, i);
  }
  for (size_t i = 0; i < count; ++i) {
    REQUIRE(PortMapPickups::IsSeen(words, i) == (i % 3 == 0));
  }
  PortMapPickups::SetSeen(words, PortMapPickups::kMaxDots); // out of range: ignored
  REQUIRE(!PortMapPickups::IsSeen(words, PortMapPickups::kMaxDots));
  // The disc's kinds: 50 missile pickups (the launcher is one of them), 14
  // energy tanks and 4 power bomb expansions are minor; the main Power Bomb and
  // the rest are major.
  int missiles = 0, tanks = 0, bombMajor = 0, bombMinor = 0, minor = 0;
  for (size_t i = 0; i < count; ++i) {
    missiles += dots[i].item == 4 ? 1 : 0;
    tanks += dots[i].item == 24 ? 1 : 0;
    if (dots[i].item == 7) {
      (PortMapPickups::KindOfItem(7, dots[i].capacity) == PortMapPickups::kK_Major ? bombMajor
                                                                                  : bombMinor)++;
    }
    minor += PortMapPickups::KindOfItem(dots[i].item, dots[i].capacity) == PortMapPickups::kK_Minor;
  }
  REQUIRE(missiles == 50 && tanks == 14 && bombMajor == 1 && bombMinor == 4);
  REQUIRE(minor == 68 && count - minor == 32);
  REQUIRE(PortMapPickups::KindOfItem(0, 1) == PortMapPickups::kK_Major);
  REQUIRE(PortMapPickups::KindOfItem(29, 1) == PortMapPickups::kK_Major);
  std::puts("map pickup dot matching, seen bitset and kinds passed");
}
