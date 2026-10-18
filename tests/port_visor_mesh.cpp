#include "port_visor_mesh.h"
#include "port_test.h"

#include <cstdio>

int main() {
  using namespace PortVisorMesh;
  // The flag word is unkC | unkE << 16; only bits 5, 16, 17 and 18 matter.
  REQUIRE(FromFlags(0) == 0);
  REQUIRE(FromFlags(0xfffe0000u & ~0x70000u) == 0);
  REQUIRE(FromFlags(0x0001ffdf) == kTag + kVisor16);  // all of unkC but bit 5, bit 16
  REQUIRE(FromFlags(0x20) == (kTag | kShadow));
  REQUIRE(FromFlags(1u << 17) == (kTag | kVisor17));
  REQUIRE(FromFlags(1u << 18) == (kTag | kVisor18));
  REQUIRE(Bits(FromFlags(0x20 | (7u << 16))) == (kShadow | kVisor16 | kVisor17 | kVisor18));
  REQUIRE(Bits(0) == 0 && Bits(0xf) == 0 && Bits(0x12340002) == 0);  // untagged words carry nothing

  // Combat and scan hide bit 16, thermal bit 17, X-ray bit 18 (EPlayerVisor order).
  const int combat = 0, xray = 1, scan = 2, thermal = 3;
  const uint32_t w16 = FromFlags(1u << 16), w17 = FromFlags(1u << 17), w18 = FromFlags(1u << 18);
  REQUIRE(SkipsVisor(w16, combat) && SkipsVisor(w16, scan) && !SkipsVisor(w16, thermal) && !SkipsVisor(w16, xray));
  REQUIRE(SkipsVisor(w17, thermal) && !SkipsVisor(w17, combat) && !SkipsVisor(w17, scan) && !SkipsVisor(w17, xray));
  REQUIRE(SkipsVisor(w18, xray) && !SkipsVisor(w18, combat) && !SkipsVisor(w18, scan) && !SkipsVisor(w18, thermal));
  REQUIRE(!SkipsVisor(0, combat) && !SkipsVisor(0, xray) && !SkipsVisor(0, thermal));

  // The shadow bit never hides a surface from a visor, and only the shadow decision reads it.
  const uint32_t wShadow = FromFlags(0x20);
  for (int v = 0; v < 4; ++v) {
    REQUIRE(!SkipsVisor(wShadow, v));
  }
  REQUIRE(SkipsShadow(wShadow) && !SkipsShadow(w16) && !SkipsShadow(0));
  REQUIRE(SkipsShadow(FromFlags(0x20 | (1u << 18))) && SkipsVisor(FromFlags(0x20 | (1u << 18)), xray));
  return 0;
}
