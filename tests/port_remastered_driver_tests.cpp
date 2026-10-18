// The Remastered float drivers of material variables (the Waterfalls' GRDU): the importer finds
// the RealNumberDriver -> RealValueRelay -> MaterialVariableDriver -> actor chain in a room, and
// binds the variable to a parameter the port drives only through the model's material.
// port_remastered_room.cpp is included for its file-local helpers; the rest of the writer's
// internals (port_remastered_room_internal.h, in PortRemastered::RoomDetail) are linked in.

#include "../platform/port_remastered_room.cpp"

#include "Kyoto/Graphics/CCubeModel.hpp"
#include "port_test.h"

#include <cstdio>

using namespace PortRemastered;
using namespace PortRemastered::RoomDetail;

namespace {

using Bytes = std::vector<uint8_t>;

void PutLE(Bytes& b, uint64_t v, int n) {
  for (int i = 0; i < n; ++i) {
    b.push_back(uint8_t(v >> (8 * i)));
  }
}

void PutFloat(Bytes& b, float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  PutLE(b, u, 4);
}

void PutBE(Bytes& b, const char* id) {
  b.insert(b.end(), id, id + 4);
}

// A chunk (24 byte header) or a form (32 byte header, `id` at 20).
Bytes Chunk(const char* id, const Bytes& body) {
  Bytes b;
  PutBE(b, id);
  PutLE(b, body.size(), 8);
  b.resize(24);
  b.insert(b.end(), body.begin(), body.end());
  return b;
}

Bytes Form(const char* id, const Bytes& body) {
  Bytes b;
  b.insert(b.end(), {'R', 'F', 'R', 'M'});
  PutLE(b, body.size(), 8);
  b.resize(20);
  PutBE(b, id);
  b.resize(32);
  b.insert(b.end(), body.begin(), body.end());
  return b;
}

void Append(Bytes& to, const Bytes& from) {
  to.insert(to.end(), from.begin(), from.end());
}

// A property list: u16 count, then per property u32 id, u16 size, the data.
struct Props {
  Bytes b{0, 0};
  void Add(uint32_t id, const Bytes& data) {
    PutLE(b, id, 4);
    PutLE(b, data.size(), 2);
    Append(b, data);
    ++b[0];
  }
};

Bytes Guid(int n) {
  Bytes g(16, 0);
  g[0] = uint8_t(n);
  g[15] = 0xA5;
  return g;
}

// One room: components in a layer, each with its own entity. Component n has the guid Guid(n)
// and the entity component N + n; `links[n]` are the guids n links to.
struct Fixture {
  struct Comp {
    uint32_t type;
    Bytes props;
    std::vector<int> links;
  };
  std::vector<Comp> comps;
  Bytes strings;

  Bytes Build() const {
    const size_t n = comps.size();
    Bytes sden, idta;
    Bytes records;
    std::vector<Comp> all = comps;
    for (size_t i = 0; i < n; ++i) {
      all.push_back({kEntity, Bytes{0, 0}, {}});
    }
    for (size_t i = 0; i < all.size(); ++i) {
      Bytes raw{0, 0, 0, 0};
      Append(raw, all[i].props.empty() ? Bytes{0, 0} : all[i].props);
      Append(sden, Chunk("SDEN", raw));
      Bytes id = Guid(int(i));
      PutLE(id, 0, 8);  // no skipped block
      PutLE(id, 0, 2);  // no connections
      if (i >= n) {
        PutBE(id, "PMOC");
        Append(id, Guid(int(i - n)));
      } else {
        PutLE(id, all[i].links.size(), 2);
        for (int target : all[i].links) {
          PutLE(id, 1, 4);
          Append(id, Guid(target));
          id.resize(id.size() + 22);
        }
      }
      Append(idta, Chunk("IDTA", id));
      PutLE(records, all[i].type, 4);
      PutLE(records, i, 4);
      PutLE(records, i, 4);
    }
    Bytes layer = Form("SRIP", Chunk("COMP", records));
    Bytes pool;
    PutLE(pool, 16, 4);
    PutLE(pool, 1, 4);
    PutLE(pool, strings.size(), 4);
    Append(pool, strings);
    Bytes body;
    Append(body, Form("SDTA", [&] {
             Bytes b = sden;
             Append(b, idta);
             return b;
           }()));
    Append(body, Form("LYRS", Form("LAYR", layer)));
    Append(body, Chunk("STRP", pool));
    return Form("ROOM", body);
  }
};

// The Ice Shorelines spline: (0, -2) linear, (2, -0.13) and (12, -0.13) flat, (16.5, -2) linear.
Bytes Spline() {
  const float knots[4][2] = {{0.f, -2.f}, {2.f, -0.13f}, {12.f, -0.13f}, {16.5f, -2.f}};
  const uint8_t type[4] = {0, 1, 1, 0};
  Bytes b;
  PutLE(b, 4, 4);
  for (int i = 0; i < 4; ++i) {
    PutFloat(b, knots[i][0]);
    PutFloat(b, knots[i][1]);
    b.push_back(type[i]);
    b.push_back(type[i]);
  }
  PutFloat(b, -2.f);
  PutFloat(b, 0.f);
  b.insert(b.end(), {0, 0, 0});
  return b;
}

Bytes Strp(uint32_t offset, uint32_t size) {
  Bytes b;
  PutLE(b, offset, 4);
  PutLE(b, size, 4);
  return b;
}

// RND 0 -> relay 1 -> MVD 2 -> actor 3. `set` and `variable` are the MVD's names.
Fixture Chain() {
  Fixture f;
  f.strings = {'W', 'a', 't', 'e', 'r', 'f', 'a', 'l', 'l', 's', 0, 'G', 'R', 'D', 'U', 0};
  Props rnd;
  rnd.Add(kPropDriverSpline, Spline());
  Props mvd, group;
  group.Add(kPropMaterialSet[1], Strp(0, 10));
  group.Add(kPropMaterialVariable[1], Strp(11, 4));
  mvd.Add(kPropMaterialSet[0], group.b);
  f.comps = {{kRealNumberDriver, rnd.b, {1}},
             {kRealValueRelay, {}, {2}},
             {kMaterialVariableDriver, mvd.b, {3}},
             {kActorMP1, {}, {}}};
  return f;
}

std::map<int, std::vector<SceneryScripts::Driver>> Find(const Fixture& f, std::set<int>& starts) {
  static Bytes keep;
  keep = f.Build();
  static Room room;
  std::string error;
  starts.clear();
  if (!room.Parse(keep, error)) {
    std::fprintf(stderr, "fixture does not parse: %s\n", error.c_str());
    port_test::CountFailure();
    return {};
  }
  return FindMaterialDrivers(room, starts);
}

void TestChain() {
  std::set<int> starts;
  const auto found = Find(Chain(), starts);
  CHECK_MSG(found.size() == 1 && found.count(7) == 1, "the actor's entity is driven");
  if (found.count(7) != 0) {
    const std::vector<SceneryScripts::Driver>& d = found.at(7);
    CHECK_MSG(d.size() == 1 && d[0].set == "Waterfalls" && d[0].variable == "GRDU", "set and variable named");
    CHECK_MSG(d.size() == 1 && d[0].spline == Spline() && d[0].start == 4, "spline and start entity");
  }
  CHECK_MSG(starts == std::set<int>{4}, "the RealNumberDriver's entity is a start");
}

// The room files link to the entity that owns a component (its guid), not to the component.
void TestEntityLinks() {
  std::set<int> starts;
  Fixture f = Chain();
  const int n = int(f.comps.size());
  for (Fixture::Comp& c : f.comps) {
    for (int& t : c.links) {
      t += n;
    }
  }
  const auto found = Find(f, starts);
  CHECK_MSG(found.size() == 1 && found.count(7) == 1 && starts == std::set<int>{4}, "links to entities");
}

// Each break in the chain, one at a time, drives nothing.
void TestFailClosed() {
  std::set<int> starts;
  Fixture f = Chain();
  f.comps[0].links.clear();  // the driver does not feed the relay
  CHECK_MSG(Find(f, starts).empty() && starts.empty(), "missing driver -> relay link");
  f = Chain();
  f.comps[1].links.clear();  // the relay does not feed the MVD
  CHECK_MSG(Find(f, starts).empty(), "missing relay -> MVD link");
  f = Chain();
  f.comps[2].links.clear();  // the MVD feeds nothing
  CHECK_MSG(Find(f, starts).empty(), "MVD without an actor");
  f = Chain();
  f.comps[3].type = kEntity;  // not an actor
  CHECK_MSG(Find(f, starts).empty(), "target is not an ActorMP1");
  f = Chain();
  f.comps.push_back({kRealValueRelay, {}, {2}});  // a second relay into the MVD
  CHECK_MSG(Find(f, starts).empty(), "two relays into one MVD");
  f = Chain();
  f.comps.push_back({kRealNumberDriver, f.comps[0].props, {1}});  // a second driver into the relay
  CHECK_MSG(Find(f, starts).empty(), "two drivers into one relay");
  f = Chain();
  Props bad;
  bad.Add(kPropDriverSpline, Bytes{1, 0, 0, 0});  // truncated spline
  f.comps[0].props = bad.b;
  CHECK_MSG(Find(f, starts).empty(), "unreadable spline");
  f = Chain();
  f.strings = {'W', 'a', 't', 'e', 'r', 'f', 'a', 'l', 'l', 's', 0};  // names outside the pool
  CHECK_MSG(Find(f, starts).empty(), "names outside the string pool");
}

constexpr uint32_t FourCC(char a, char b, char c, char d) {
  return (uint32_t(uint8_t(a)) << 24) | (uint32_t(uint8_t(b)) << 16) | (uint32_t(uint8_t(c)) << 8) | uint8_t(d);
}

ModelMaterialData TextureData(uint32_t usage, uint8_t id) {
  ModelMaterialData d;
  d.usage = usage;
  d.kind = ModelMaterialData::Kind::Texture;
  d.texture.usage = usage;
  d.texture.hasUsage = true;
  d.texture.id[0] = id;
  d.texture.id[15] = uint8_t(id + 1);
  d.texture.wrapX = d.texture.wrapY = 1;
  return d;
}

ModelMaterialData ColorData(uint32_t usage, float r, float g, float b, float a) {
  ModelMaterialData d;
  d.usage = usage;
  d.kind = ModelMaterialData::Kind::Color;
  d.color[0] = r, d.color[1] = g, d.color[2] = b, d.color[3] = a;
  return d;
}

// A Waterfalls material (shader 50412FE7, kind 7) binding GRDU to CCH0.z.
ModelMaterial WaterfallMaterial() {
  ModelMaterial m;
  m.name = "Waterfalls";
  m.unk1 = 0x10;
  m.shaderId[0] = 0x50, m.shaderId[1] = 0x41, m.shaderId[2] = 0x2F, m.shaderId[3] = 0xE7;
  m.data = {TextureData(FourCC('T', 'C', 'H', '0'), 0x10), TextureData(FourCC('T', 'C', 'H', '1'), 0x20),
            ColorData(FourCC('C', 'C', 'H', '0'), 1.f, 0.5f, 0.25f, 1.f), ColorData(FourCC('C', 'C', 'H', '1'), 0.f, 0.1f, 0.f, 0.f),
            ColorData(FourCC('C', 'C', 'H', '2'), 0.f, 0.2f, 0.f, 0.f), ColorData(FourCC('C', 'C', 'H', '3'), 1.f, 1.f, 1.f, 1.f)};
  m.renderTypes.push_back({FourCC('G', 'R', 'D', 'U'), FourCC('C', 'C', 'H', '0'), 1, 2});
  return m;
}

void TestSlot() {
  SceneryScripts::Driver d;
  d.set = "Waterfalls";
  d.variable = "GRDU";
  const ModelMaterial water = WaterfallMaterial();
  CHECK_MSG(DriverSlot({water}, d) == 3, "GRDU on a Waterfalls material is kind parameter 3");

  SceneryScripts::Driver other = d;
  other.variable = "XXXX";
  CHECK_MSG(DriverSlot({water}, other) == -1, "a variable the material does not bind");
  other = d;
  other.variable = "GRD";
  CHECK_MSG(DriverSlot({water}, other) == -1, "a variable name that is not four characters");
  other = d;
  other.set = "Other";
  CHECK_MSG(DriverSlot({water}, other) == -1, "a set the model lacks");

  ModelMaterial component = water;
  component.renderTypes[0].flag2 = 0;  // CCH0.x: not what the port carries
  ModelMaterial second = water;
  second.name = "Waterfalls2";
  CHECK_MSG(DriverSlot({water, second}, d) == -1, "a model with a second kind 7 material is not driven");
  ModelMaterial plain = water;
  plain.name = "Rock";
  plain.shaderId[0] = 0;
  CHECK_MSG(DriverSlot({plain, water}, d) == 3, "other kinds beside the driven material do not count");
  CHECK_MSG(DriverSlot({component}, d) == -1, "an unsupported component");
  ModelMaterial channel = water;
  channel.renderTypes[0].dataType = FourCC('C', 'C', 'H', '1');
  CHECK_MSG(DriverSlot({channel}, d) == -1, "an unsupported channel");
  ModelMaterial notWater = water;
  notWater.shaderId[0] = 0x01;  // another shader: not kind 7
  CHECK_MSG(DriverSlot({notWater}, d) == -1, "a material that is not a waterfall");
}

void TestStart() {
  PortRoomGeo::Link a{0x0c1a01b0, 9, PortRoomGeo::kShow, 1.5f}, b = a;
  PortRoomGeo::Link first;
  CHECK_MSG(SingleDriverStart({a}, first) && first.sender == a.sender && first.delay == 1.5f, "one start");
  CHECK_MSG(SingleDriverStart({a, b}, first), "the same start twice");
  b.delay = 2.f;
  CHECK_MSG(!SingleDriverStart({a, b}, first), "starts at two delays");
  b = a;
  b.sender = 1;
  CHECK_MSG(!SingleDriverStart({a, b}, first), "starts from two senders");
  b = a;
  b.action = PortRoomGeo::kHide;
  CHECK_MSG(!SingleDriverStart({a, b}, first), "a hide among the starts");
  CHECK_MSG(!SingleDriverStart({}, first), "no start");
}

// The override a driven instance's draws set: kind 7's parameter only, and nothing once cleared
// (port_room_geo.cpp sets it around the driven instance's draws and clears it after).
void TestOverride() {
  float water[19] = {}, other[19] = {};
  water[13] = 7.f;
  other[13] = 5.f;
  for (int i = 15; i < 19; ++i) {
    water[i] = other[i] = 0.25f;
  }
  CHECK_MSG(CCubeModel::PortApplyKindParam(3, -0.75f, water) && water[18] == -0.75f && water[15] == 0.25f &&
                water[16] == 0.25f && water[17] == 0.25f,
            "kind 7: slot 3 only");
  CHECK_MSG(!CCubeModel::PortApplyKindParam(3, -0.75f, other) && other[18] == 0.25f, "another kind is left alone");
  CHECK_MSG(!CCubeModel::PortApplyKindParam(-1, 1.f, water) && water[18] == -0.75f, "cleared: no change");
  CHECK_MSG(!CCubeModel::PortApplyKindParam(4, 1.f, water), "a slot past 3 is refused");
}
} // namespace

int main() {
  TestChain();
  TestEntityLinks();
  TestFailClosed();
  TestSlot();
  TestStart();
  TestOverride();
  if (port_test::Failures() == 0) {
    std::printf("port_remastered_driver_tests: ok\n");
  }
  return PORT_TEST_RESULT();
}
