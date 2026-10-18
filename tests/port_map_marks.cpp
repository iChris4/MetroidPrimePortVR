#include "port_map_marks.h"
#include "port_test.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

struct Bits {
  std::vector< bool > bits;
  size_t pos = 0;
  void Write(uint32_t value, unsigned n) {
    for (int i = static_cast< int >(n) - 1; i >= 0; --i) {
      bits.push_back(((value >> i) & 1) != 0);
    }
  }
  uint32_t Read(unsigned n) {
    uint32_t v = 0;
    for (unsigned i = 0; i < n; ++i) {
      v = (v << 1) | (pos < bits.size() && bits[pos] ? 1u : 0u);
      ++pos;
    }
    return v;
  }
};
} // namespace

int main() {
  using namespace PortMapMarks;

  // Pack/unpack: every legal world and area survives.
  for (unsigned w = 0; w < 8; ++w) {
    for (unsigned a = 0; a < 128; ++a) {
      REQUIRE_MSG(Valid(w, a) && WorldOf(Pack(w, a)) == w && AreaOf(Pack(w, a)) == a, "pack");
    }
  }
  REQUIRE_MSG(!Valid(8, 0) && !Valid(0, 128), "range");

  // Toggle adds, removes, keeps order and refuses past the cap.
  List list;
  REQUIRE_MSG(list.Toggle(Pack(1, 5)) == 1 && list.Toggle(Pack(4, 60)) == 1 && list.Toggle(Pack(0, 0)) == 1, "add");
  REQUIRE_MSG(list.count == 3 && list.Has(Pack(4, 60)), "has");
  REQUIRE_MSG(list.Toggle(Pack(4, 60)) == -1 && list.count == 2 && !list.Has(Pack(4, 60)), "remove");
  REQUIRE_MSG(list.packed[0] == Pack(1, 5) && list.packed[1] == Pack(0, 0), "order kept");
  List full;
  for (unsigned i = 0; i < kMaxMarks; ++i) {
    REQUIRE_MSG(full.Toggle(Pack(i % 7, i)) == 1, "fill");
  }
  REQUIRE_MSG(full.Toggle(Pack(7, 100)) == 0 && full.count == kMaxMarks, "cap");
  REQUIRE_MSG(full.Toggle(Pack(0, 0)) == -1, "a full list can still unmark");

  // Encode and decode round trip, the magic first. Only what Fit() allows
  // (never more than kTrailerMarks) is handed to Put(): that is the point of
  // the 5-bit count field, which an older build reads too.
  Bits bits;
  const size_t trailer = Fit(full.count, 940 * 8, 7084 + 160);
  REQUIRE_MSG(trailer == kTrailerMarks && trailer < full.count, "a USA trailer of 64 takes 20");
  Put(full, trailer, [&bits](uint32_t v, unsigned n) { bits.Write(v, n); });
  REQUIRE_MSG(bits.bits.size() == kHeaderBits + trailer * kMarkBits, "encoded size");
  REQUIRE_MSG(bits.Read(32) == kMagic, "magic");
  const List back = Get([&bits](unsigned n) { return bits.Read(n); });
  REQUIRE_MSG(back.count == trailer, "decoded count");
  for (uint32_t i = 0; i < trailer; ++i) {
    REQUIRE_MSG(back.packed[i] == full.packed[i], "decoded mark");
  }
  // The first build's trailer: a 5-bit count of 20 and nothing else changes.
  REQUIRE_MSG(kCountBits == 5 && kTrailerMarks == 20 && kMaxMarks == 64,
              "the trailer's fields and cap stay as first shipped, the side file holds 64");
  Bits legacy;
  legacy.Write(kMagic, 32);
  REQUIRE_MSG(legacy.Read(32) == kMagic, "legacy trailer magic");
  legacy.Write(kTrailerMarks, kCountBits);
  for (unsigned i = 0; i < kTrailerMarks; ++i) {
    legacy.Write(Pack(i % 7, i), kMarkBits);
  }
  const List legacyBack = Get([&legacy](unsigned n) { return legacy.Read(n); });
  REQUIRE_MSG(legacyBack.count == kTrailerMarks && legacyBack.packed[19] == Pack(5, 19),
              "an older build's 20-mark trailer reads whole");
  // A count above the trailer's cap (garbage, a cut trailer) reads as empty;
  // repeats drop.
  Bits junk;
  junk.Write(kTrailerMarks + 1, kCountBits);
  REQUIRE_MSG(Get([&junk](unsigned n) { return junk.Read(n); }).count == 0, "count too big");
  Bits allOnes;
  allOnes.Write(0x1F, kCountBits);
  REQUIRE_MSG(Get([&allOnes](unsigned n) { return allOnes.Read(n); }).count == 0, "all ones too big");
  Bits twice;
  twice.Write(3, kCountBits);
  twice.Write(Pack(2, 9), kMarkBits);
  twice.Write(Pack(2, 9), kMarkBits);
  twice.Write(Pack(3, 1), kMarkBits);
  REQUIRE_MSG(Get([&twice](unsigned n) { return twice.Read(n); }).count == 2, "repeat dropped");
  Bits empty;
  REQUIRE_MSG(Get([&empty](unsigned n) { return empty.Read(n); }).count == 0, "zeros read empty");

  // Budget: the USA slot (940 bytes), 7084 retail bits + PKSN 160 = 7244 used
  // holds 20 of the 64 marks with the spare kept (the side file keeps the
  // rest); with the 416-bit Archipelago trailer too, nothing fits.
  const size_t usaSlot = 940 * 8;
  const size_t fit = Fit(kMaxMarks, usaSlot, 7084 + 160);
  REQUIRE_MSG(fit == kTrailerMarks, "usa without archipelago fits 20 marks (the side file keeps the rest)");
  REQUIRE_MSG(usaSlot - (7084 + 160) - (kHeaderBits + fit * kMarkBits) >= kSpareBits, "spare kept");
  REQUIRE_MSG(Fit(kTrailerMarks + 1, usaSlot, 7084 + 160) == kTrailerMarks, "one more than fits still fits 20");
  REQUIRE_MSG(Fit(kMaxMarks, usaSlot, 7084 + 416 + 160) == 0, "usa with archipelago fits none");
  REQUIRE_MSG(Fit(0, usaSlot, 0) == 0, "no marks, nothing written");
  // Fit is exact at the edge: one fewer bit of room drops a mark.
  const size_t edge = 7084 + 160 + kSpareBits + kHeaderBits + kMarkBits * 3;
  REQUIRE_MSG(Fit(10, edge, 7084 + 160) == 3 && Fit(10, edge - 1, 7084 + 160) == 2, "fit edge");
  REQUIRE_MSG(Fit(5, 0xA7Fu * 8, 7084 + 416 + 160) == 5, "pal holds five");
  REQUIRE_MSG(Fit(kMaxMarks, 0xA7Fu * 8, 7084 + 160) == kTrailerMarks,
              "pal has room for 64 but the trailer keeps to 20");

  std::puts("port_map_marks_tests ok");
  return 0;
}
