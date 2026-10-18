#include "port_room_env_tiles.h"
#include "port_test.h"

#include <cstdio>
#include <cstring>

using namespace PortRoomEnvTiles;

namespace {
uint32_t Owner(const TileTable& t, const TileKey& k) {
  uint32_t n = 0xFFFFFFFF;
  return t.Active(k, &n) ? n : 0xFFFFFFFF;
}

// A set owning one layer-0 tile at block (bx,0,0): 2x2x2 points at the tile's far corner (or
// near one), every point's mean = `value` (half bits) in all three channels.
std::shared_ptr<TileSet> MakeSet(int bx, uint16_t value, int lox) {
  auto set = std::make_shared<TileSet>();
  Tile tile;
  tile.key = {0, bx, 0, 0};
  tile.lo[0] = lox;
  tile.size[0] = tile.size[1] = tile.size[2] = 2;
  tile.offset = 0;
  set->tiles.push_back(tile);
  set->data.resize(8 * 6);
  for (int i = 0; i < 24; ++i) {
    set->data[i * 2] = uint8_t(value);
    set->data[i * 2 + 1] = uint8_t(value >> 8);
  }
  set->box[0] = bx * 64 + lox;
  set->box[3] = bx * 64 + lox + 1;
  return set;
}
uint16_t PointMean(const ResolvedGrid& g, int64_t x0, int64_t gx, int64_t gy, int64_t gz) {
  (void)x0;
  const uint8_t* p = &g.points[((size_t(gz) * g.size[1] + size_t(gy)) * g.size[0] + size_t(gx)) * 24];
  return uint16_t(p[0] | (p[1] << 8));
}
} // namespace

int main() {
  const TileKey a{0, 0, 0, 0}, b{0, 1, 0, 0}, c{3, 0, 0, 0};
  TileTable t;
  // Load order: the first copy stays mapped, the later copy waits.
  auto ch = t.Register(1, {a, b});
  CHECK_MSG(ch.size() == 2, "first load maps both");
  ch = t.Register(2, {a, c});
  CHECK_MSG(ch.size() == 1 && ch[0] == c, "second load maps only the new key");
  CHECK_MSG(Owner(t, a) == 1 && Owner(t, b) == 1 && Owner(t, c) == 2, "first loaded copy stays active");
  // A third copy queues behind; a layer is its own key.
  t.Register(3, {a});
  CHECK_MSG(Owner(t, a) == 1, "third load stays standby");
  // Unload of the mapped copy promotes the most recent standby (3, registered after 2).
  ch = t.Unregister(1);
  CHECK_MSG(Owner(t, a) == 3, "most recent standby promoted");
  CHECK_MSG(ch.size() == 2, "unload reports both changed keys");
  CHECK_MSG(!t.Active(b, nullptr), "tile with no standby is unmapped");
  // Priority is not re-resolved: reloading area 1 queues behind.
  t.Register(1, {a, b});
  CHECK_MSG(Owner(t, a) == 3 && Owner(t, b) == 1, "reload does not preempt");
  // Unloading a standby copy changes nothing.
  ch = t.Unregister(2);
  CHECK_MSG(Owner(t, a) == 3 && ch.size() == 1 && ch[0] == c, "standby unload keeps the active copy");
  CHECK_MSG(!t.Active(c, nullptr), "c unmapped");
  // A higher priority preempts.
  t.Register(4, {a}, 5);
  CHECK_MSG(Owner(t, a) == 4, "higher priority maps");
  t.Unregister(4);
  CHECK_MSG(Owner(t, a) == 1, "most recent of remaining standbys after priority node leaves");
  t.Unregister(1);
  t.Unregister(3);
  CHECK_MSG(t.KeyCount() == 0, "empty after all unload");

  // Resolving: the mapped copy of a tile supplies the points, whoever the sampling area is.
  {
    constexpr uint16_t kOne = 0x3C00, kTwo = 0x4000;
    const auto a = MakeSet(0, kOne, 0);
    const auto b = MakeSet(0, kTwo, 0);
    std::shared_ptr<const TileSet> mapped = a;
    const Lookup lookup = [&](const TileKey& k) -> std::shared_ptr<const TileSet> {
      return mapped != nullptr && mapped->Find(k) != nullptr ? mapped : nullptr;
    };
    ResolvedGrid grid;
    std::string note;
    CHECK_MSG(ResolveGrid(*b, lookup, 0, grid, note), "resolves with the first copy mapped");
    CHECK_MSG(PointMean(grid, 0, 1, 1, 1) == kOne, "the mapped (first) copy supplies the points, not the area's own");
    mapped = b;
    CHECK_MSG(ResolveGrid(*b, lookup, 0, grid, note) && PointMean(grid, 0, 1, 1, 1) == kTwo, "after promotion the standby copy");
    mapped = nullptr;
    CHECK_MSG(!ResolveGrid(*b, lookup, 0, grid, note), "an unmapped tile leaves the grid empty");
  }
  // Neighbouring blocks owned by different areas meet in one grid; an unmapped neighbour reads zero.
  {
    constexpr uint16_t kOne = 0x3C00, kTwo = 0x4000;
    const auto left = MakeSet(0, kOne, 62);  // points x 62,63 of block 0
    const auto right = MakeSet(1, kTwo, 0);  // points x 64,65 of block 1
    auto own = std::make_shared<TileSet>(*left);
    own->tiles.push_back(right->tiles[0]);
    own->tiles.back().offset = uint32_t(own->data.size());
    own->data.insert(own->data.end(), right->data.begin(), right->data.end());
    bool rightMapped = true;
    const Lookup lookup = [&](const TileKey& k) -> std::shared_ptr<const TileSet> {
      if (k.x == 0 && left->Find(k)) return left;
      if (k.x == 1 && rightMapped && right->Find(k)) return right;
      return nullptr;
    };
    ResolvedGrid grid;
    std::string note;
    CHECK_MSG(ResolveGrid(*own, lookup, 0, grid, note), "cross-tile grid resolves");
    // c0 = 61; grid x 1,2 = 62,63 (left); x 3,4 = 64,65 (right): contiguous across the border.
    CHECK_MSG(grid.size[0] == 6, "grid spans both tiles");
    CHECK_MSG(PointMean(grid, 0, 2, 1, 1) == kOne && PointMean(grid, 0, 3, 1, 1) == kTwo, "values meet at the tile border");
    rightMapped = false;
    CHECK_MSG(ResolveGrid(*own, lookup, 0, grid, note) && grid.size[0] == 4, "unmapped neighbour is not in the grid");
  }
  // The world texture's indirection: a point's tile and place in it, by position alone.
  {
    int32_t local[3];
    TileKey k = TileOf(0, -1, 64, 16, local);
    CHECK_MSG(k.layer == 0 && k.x == -1 && k.y == 1 && k.z == 1 && local[0] == 63 && local[1] == 0 && local[2] == 0,
              "layer 0 point (-1,64,16) is block (-1,1,1) at (63,0,0)");
    k = TileOf(2, 130, 5, 31, local);
    CHECK_MSG(k.layer == 2 && k.x == 1 && k.y == 0 && k.z == 1 && local[0] == 2 && local[1] == 5 && local[2] == 15,
              "layer 2 tiles are 128 wide");
    k = TileOf(0, 63, 63, 15, local);
    CHECK_MSG(k.x == 0 && k.y == 0 && k.z == 0 && local[0] == 63 && local[1] == 63 && local[2] == 15,
              "the last point of a block stays in it");
  }
  // ReadPoint: unmapped and out-of-crop points read zero (the border), mapped ones their bytes.
  {
    constexpr uint16_t kOne = 0x3C00;
    const auto set = MakeSet(0, kOne, 62); // x 62,63 of block 0, y/z 0..1
    const Lookup lookup = [&](const TileKey& key) -> std::shared_ptr<const TileSet> {
      return set->Find(key) != nullptr ? set : nullptr;
    };
    uint8_t p[6];
    CHECK_MSG(ReadPoint(lookup, 0, 63, 1, 1, p) && p[0] == (kOne & 0xFF) && p[1] == (kOne >> 8), "mapped point reads its bytes");
    CHECK_MSG(!ReadPoint(lookup, 0, 64, 1, 1, p) && p[0] == 0 && p[5] == 0, "unmapped tile reads zero");
    CHECK_MSG(!ReadPoint(lookup, 0, 61, 1, 1, p) && p[1] == 0, "a point outside the tile's crop reads zero");
    CHECK_MSG(!ReadPoint(lookup, 0, 63, 2, 1, p), "a point past the crop in y reads zero");
  }
  // A surface straddling a door seam: the two sides belong to different areas, both loaded; the
  // world grid holds both sides in one point space with no gap, and taps across the seam read the
  // neighbour's points. With only one side mapped the other side is the black border.
  {
    constexpr uint16_t kOne = 0x3C00, kTwo = 0x4000;
    const auto west = MakeSet(0, kOne, 62); // x 62,63
    const auto east = MakeSet(1, kTwo, 0);  // x 64,65
    bool eastMapped = true;
    const Lookup lookup = [&](const TileKey& k) -> std::shared_ptr<const TileSet> {
      if (k.x == 0 && west->Find(k)) return west;
      if (k.x == 1 && eastMapped && east->Find(k)) return east;
      return nullptr;
    };
    ResolvedGrid grid;
    std::string note;
    const float m[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    CHECK_MSG(ResolveWorld(m, {west->tiles[0].key, east->tiles[0].key}, lookup, 0, grid, note), "world grid resolves");
    CHECK_MSG(grid.size[0] == 6, "the world grid spans both areas' tiles");
    CHECK_MSG(PointMean(grid, 0, 2, 1, 1) == kOne && PointMean(grid, 0, 3, 1, 1) == kTwo,
              "the points either side of the seam are contiguous");
    eastMapped = false;
    CHECK_MSG(ResolveWorld(m, {west->tiles[0].key, east->tiles[0].key}, lookup, 0, grid, note) && grid.size[0] == 4,
              "an unmapped side is the empty border, not the other area's grid");
    CHECK_MSG(!ResolveWorld(m, {}, lookup, 0, grid, note), "no keys: no grid");
  }
  return PORT_TEST_RESULT();
}
