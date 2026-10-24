// The world master's script resolver (port_remastered_room_scripts.cpp): FluidSender, which names
// the senders retail has no object for (the player in a fluid, the camera under water, and the
// InventoryActivator the camera water proxy fires), and MasterScripts, which follows a fog hint's
// links back through Relays to those and to camera-detecting TriggerMP1s. Also the rooms' own
// effect actors as a .roomfx (port_remastered_room_effects.cpp). The ROOM files are built here,
// with only the components and connections these read.

#include "port_remastered_room_internal.h"
#include "port_room_effects.h"
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

void Append(Bytes& to, const Bytes& from) {
  to.insert(to.end(), from.begin(), from.end());
}

// A chunk (24 byte header) or a form (32 byte header, `id` at 20).
Bytes Chunk(const char* id, const Bytes& body) {
  Bytes b(id, id + 4);
  PutLE(b, body.size(), 8);
  b.resize(24);
  Append(b, body);
  return b;
}

Bytes Form(const char* id, const Bytes& body) {
  Bytes b{'R', 'F', 'R', 'M'};
  PutLE(b, body.size(), 8);
  b.resize(20);
  b.insert(b.end(), id, id + 4);
  b.resize(32);
  Append(b, body);
  return b;
}

// A property list: u16 count, then per property u32 id, u16 size, the data.
struct Props {
  Bytes b{0, 0};
  Props& Add(uint32_t id, const Bytes& data) {
    PutLE(b, id, 4);
    PutLE(b, data.size(), 2);
    Append(b, data);
    ++b[0];
    return *this;
  }
  Props& AddU32(uint32_t id, uint32_t v) {
    Bytes d;
    PutLE(d, v, 4);
    return Add(id, d);
  }
};

Bytes Guid(size_t n) {
  Bytes g(16, 0);
  g[0] = uint8_t(n);
  g[15] = 0xA5;
  return g;
}

// A room of components, each with an entity of its own. Component n has the guid Guid(n) and
// the entity N + n (N components); an entity starts active unless `active` is false, at `pos`
// with `scale` and no rotation.
struct Fixture {
  struct Link {
    size_t target;  // component
    uint32_t event, action;
  };
  struct Comp {
    uint32_t type;
    Bytes props;
    std::vector<Link> links;
    bool active = true;
    float pos[3] = {0, 0, 0};
    float scale[3] = {1, 1, 1};
  };
  std::vector<Comp> comps;

  // Component n's index in the room, its entity's N + n.
  size_t Entity(size_t n) const { return comps.size() + n; }

  Bytes Build() const {
    const size_t n = comps.size();
    Bytes sden, idta, records;
    for (size_t i = 0; i < 2 * n; ++i) {
      const bool entity = i >= n;
      const Comp& c = comps[entity ? i - n : i];
      // A component's data starts with 4 bytes the reader skips. An entity's: the active flag,
      // a byte, then position, rotation and scale.
      Bytes raw(4, 0);
      if (entity) {
        raw.push_back(c.active ? 1 : 0);
        raw.push_back(0);
        for (const float f : {c.pos[0], c.pos[1], c.pos[2], 0.f, 0.f, 0.f, c.scale[0], c.scale[1], c.scale[2]}) {
          PutFloat(raw, f);
        }
      } else {
        Append(raw, c.props.empty() ? Bytes{0, 0} : c.props);
      }
      Append(sden, Chunk("SDEN", raw));
      // The id data: the guid, 8 bytes (not 0xffffffff first), the connection count and the
      // connections (event, action, target guid, 2 bytes, two empty optional strings, 19 bytes);
      // an entity names its component instead: 'PMOC' and its guid.
      Bytes id = Guid(i);
      PutLE(id, 0, 8);
      if (entity) {
        PutLE(id, 0, 2);
        id.insert(id.end(), {'P', 'M', 'O', 'C'});
        Append(id, Guid(i - n));
      } else {
        PutLE(id, c.links.size(), 2);
        for (const Link& l : c.links) {
          PutLE(id, l.event, 4);
          PutLE(id, l.action, 4);
          Append(id, Guid(l.target));
          id.resize(id.size() + 2);
          PutLE(id, 0, 4);
          PutLE(id, 0, 4);
          id.resize(id.size() + 19);
        }
      }
      Append(idta, Chunk("IDTA", id));
      PutLE(records, entity ? kEntity : c.type, 4);
      PutLE(records, i, 4);
      PutLE(records, i, 4);
    }
    Bytes body;
    Append(body, Form("SDTA", [&] {
             Bytes b = sden;
             Append(b, idta);
             return b;
           }()));
    Append(body, Form("LYRS", Form("LAYR", Form("SRIP", Chunk("COMP", records)))));
    return Form("ROOM", body);
  }
};

// The fixture parsed; its bytes live as long as the object.
struct Parsed {
  Bytes bytes;
  Room room;
  explicit Parsed(const Fixture& f) : bytes(f.Build()) {
    std::string error;
    if (!room.Parse(bytes, error)) {
      std::fprintf(stderr, "fixture does not parse: %s\n", error.c_str());
      port_test::CountFailure();
    }
  }
};

// What FluidSender makes of the connection from component `sender` with `event`.
uint32_t Sender(const Fixture& f, size_t sender, uint32_t event, int& state) {
  const Parsed p(f);
  const std::vector<Component>& comps = p.room.Components();
  const std::vector<Connection> conns = ReadConnections(p.room);
  std::map<int, std::vector<const Connection*>> incoming;  // by target entity, as MasterScripts keeps it
  for (const Connection& c : conns) {
    const int t = p.room.ByGuid(c.target);
    const int entity = t < 0 ? -1 : comps[size_t(t)].type == kEntity ? t : comps[size_t(t)].entity;
    if (entity >= 0) {
      incoming[entity].push_back(&c);
    }
  }
  state = -1;
  for (const Connection& c : conns) {
    if (c.sender == sender && c.event == event) {
      return FluidSender(p.room, incoming, c, state);
    }
  }
  std::fprintf(stderr, "no connection from %zu with event %08x\n", sender, event);
  port_test::CountFailure();
  return 0;
}

Bytes U32(uint32_t v) {
  Bytes b;
  PutLE(b, v, 4);
  return b;
}

Fixture::Comp Plain(uint32_t type, std::vector<Fixture::Link> links = {}) {
  Fixture::Comp c;
  c.type = type;
  c.links = std::move(links);
  return c;
}

Fixture::Comp Special(uint32_t type, uint32_t item, std::vector<Fixture::Link> links) {
  Fixture::Comp c = Plain(kSpecialFunction, std::move(links));
  c.props = Props().AddU32(kPropSpecialFunctionType, type).AddU32(kPropSpecialFunctionItem, item).b;
  return c;
}

void TestFluidSenders() {
  int state = -1;
  // 0 ProxyPlayer and 1 CameraWaterProxy, each telling the hint (2) in and out.
  Fixture f;
  f.comps = {Plain(kProxyPlayer, {{2, kEventPlayerFluidIn, kActionHintOn}, {2, kEventPlayerFluidOut, kActionHintOff}}),
             Plain(kCameraWaterProxy, {{2, kEventCameraWaterIn, kActionHintOn}, {2, kEventCameraWaterOut, kActionHintOff}}),
             Plain(kColorGradeHint)};
  CHECK_MSG(Sender(f, 0, kEventPlayerFluidIn, state) == PortRoomEnv::kSenderPlayerFluid && state == 0, "player in a fluid");
  CHECK_MSG(Sender(f, 0, kEventPlayerFluidOut, state) == PortRoomEnv::kSenderPlayerFluid && state == 1, "player out of it");
  CHECK_MSG(Sender(f, 1, kEventCameraWaterIn, state) == PortRoomEnv::kSenderCameraWater && state == 0, "camera under water");
  CHECK_MSG(Sender(f, 1, kEventCameraWaterOut, state) == PortRoomEnv::kSenderCameraWater && state == 1, "camera out of it");

  // 0 CameraWaterProxy -> 1 InventoryActivator (item 21) -> 2 hint, the shape of the masters'
  // water fogs.
  const Fixture::Link feed{1, kEventCameraWaterIn, kActionAction};
  const auto water = [&](uint32_t function, uint32_t item, std::vector<Fixture::Link> feeds) {
    Fixture w;
    w.comps = {Plain(kCameraWaterProxy, std::move(feeds)),
               Special(function, item,
                       {{2, kEventSpecialFunctionZero, kActionFogHintOn}, {2, kEventSpecialFunctionMaxReached, kActionFogHintOff}}),
               Plain(kVolumetricFogHint)};
    return w;
  };
  const uint32_t item21 = PortRoomEnv::kSenderCameraWaterItem + 21;
  const Fixture ok = water(kSpecialFunctionInventoryActivator, 21, {feed});
  CHECK_MSG(Sender(ok, 1, kEventSpecialFunctionZero, state) == item21 && state == 0, "item held: state 0");
  CHECK_MSG(Sender(ok, 1, kEventSpecialFunctionMaxReached, state) == item21 && state == 1, "item missing: state 1");
  CHECK_MSG(Sender(water(kSpecialFunctionInventoryActivator, 0, {feed}), 1, kEventSpecialFunctionZero, state) ==
                    PortRoomEnv::kSenderCameraWaterItem &&
                state == 0,
            "item 0 is an item");
  CHECK_MSG(Sender(water(4, 21, {feed}), 1, kEventSpecialFunctionZero, state) == 0, "another function type");
  CHECK_MSG(Sender(water(kSpecialFunctionInventoryActivator, PortRoomEnv::kCameraWaterItems, {feed}), 1,
                   kEventSpecialFunctionZero, state) == 0,
            "an item past the table");
  CHECK_MSG(Sender(water(kSpecialFunctionInventoryActivator, PortRoomEnv::kCameraWaterItems - 1, {feed}), 1,
                   kEventSpecialFunctionMaxReached, state) == PortRoomEnv::kSenderCameraWaterItem + 63,
            "the last item of the table");
  // A second feed that is no camera water proxy, then one with another event or action.
  Fixture extra = water(kSpecialFunctionInventoryActivator, 21, {feed});
  extra.comps.push_back(Plain(kRelay, {{1, kEventRelayFired, kActionAction}}));
  CHECK_MSG(Sender(extra, 1, kEventSpecialFunctionZero, state) == 0, "a feed that is not a camera water proxy");
  CHECK_MSG(Sender(water(kSpecialFunctionInventoryActivator, 21, {feed, {1, kEventCameraWaterOut, kActionAction}}), 1,
                   kEventSpecialFunctionZero, state) == 0,
            "a feed with another event");
  CHECK_MSG(Sender(water(kSpecialFunctionInventoryActivator, 21, {{1, kEventCameraWaterIn, kActionActivate}}), 1,
                   kEventSpecialFunctionZero, state) == 0,
            "a feed with another action");
  // A function nothing feeds, and an event it does not send.
  Fixture unfed = water(kSpecialFunctionInventoryActivator, 21, {});
  CHECK_MSG(Sender(unfed, 1, kEventSpecialFunctionZero, state) == 0, "nothing feeds it");
  Fixture other = ok;
  other.comps[1].links.push_back({2, kEventEntered, kActionFogHintOn});
  CHECK_MSG(Sender(other, 1, kEventEntered, state) == 0, "an event it does not send");
}

bool Same(const PortRoomGeo::Link& l, uint32_t sender, uint8_t state, uint8_t action) {
  return l.sender == sender && l.state == state && l.action == action;
}

void TestMasterTrigger() {
  // 0 TriggerMP1 (sees the camera) Entered -> 1 Relay -> 3 hint On, Exited -> 2 Relay -> hint Remove.
  Fixture f;
  Fixture::Comp trigger = Plain(kTriggerMP1, {{1, kEventEntered, kActionRelayFire}, {2, kEventExited, kActionRelayFire}});
  trigger.props = Props().AddU32(kPropTriggerFlags, kTriggerDetectCamera | 1).b;
  trigger.pos[0] = 10.f;
  trigger.pos[1] = 20.f;
  trigger.pos[2] = 30.f;
  trigger.scale[0] = trigger.scale[1] = trigger.scale[2] = 8.f;
  f.comps = {trigger, Plain(kRelay, {{3, kEventRelayFired, kActionFogHintOn}}),
             Plain(kRelay, {{3, kEventRelayFired, kActionFogHintRemove}}), Plain(kVolumetricFogHint)};
  const Parsed p(f);
  const int hint = int(f.Entity(3));

  std::vector<PortRoomEnv::WorldVolume> at0, shifted;
  const SceneryScripts base = MasterScripts(p.room, {0, 0, 0}, at0);
  const SceneryScripts moved = MasterScripts(p.room, {100, 200, 300}, shifted);
  CHECK_MSG(base.links.size() == 1 && base.links.count(hint) == 1, "the hint's entity has links");
  CHECK_MSG(base.unresolved.empty(), "nothing left unresolved");
  if (base.links.count(hint) != 0) {
    const auto& links = base.links.at(hint);
    CHECK_MSG(links.size() == 2, "two links");
    bool in = false, out = false;
    for (const PortRoomGeo::Link& l : links) {
      in = in || Same(l, PortRoomEnv::kSenderWorldVolume + 0, 0, PortRoomGeo::kShow);
      out = out || Same(l, PortRoomEnv::kSenderWorldVolume + 0, 1, PortRoomGeo::kHide);
    }
    CHECK_MSG(in, "entering the volume turns the hint on");
    CHECK_MSG(out, "leaving it takes the hint out");
  }
  CHECK_MSG(at0.size() == 1 && shifted.size() == 1, "one volume, shared by both links");
  if (at0.size() == 1 && shifted.size() == 1) {
    // CameraBox's box in retail axes is (-x, z, y) of the entity; the world's shift is added.
    CHECK_MSG(at0[0].centre[0] == -10.f && at0[0].centre[1] == 30.f && at0[0].centre[2] == 20.f, "the box's centre");
    CHECK_MSG(shifted[0].centre[0] == 90.f && shifted[0].centre[1] == 230.f && shifted[0].centre[2] == 320.f,
              "the shift is added to the centre");
    CHECK_MSG(shifted[0].half[0] == at0[0].half[0], "and not to its size");
  }
  // An inactive trigger sees no camera: no volume, and the links are left unresolved.
  Fixture off = f;
  off.comps[0].active = false;
  const Parsed po(off);
  std::vector<PortRoomEnv::WorldVolume> none;
  const SceneryScripts idle = MasterScripts(po.room, {0, 0, 0}, none);
  CHECK_MSG(none.empty() && idle.links.empty() && idle.unresolved.count(hint) == 1, "an inactive trigger has no volume");
  // And one that does not detect the camera.
  Fixture blind = f;
  blind.comps[0].props = Props().AddU32(kPropTriggerFlags, 1).b;
  const Parsed pb(blind);
  const SceneryScripts deaf = MasterScripts(pb.room, {0, 0, 0}, none);
  CHECK_MSG(none.empty() && deaf.links.empty(), "a trigger that misses the camera has no volume");
}

void TestMasterCameraWater() {
  // 0 CameraWaterProxy -> 1 InventoryActivator (item 21): Zero -> 2 Relay -> 4 hint On, MaxReached
  // -> 3 Relay -> hint Remove.
  Fixture f;
  f.comps = {Plain(kCameraWaterProxy, {{1, kEventCameraWaterIn, kActionAction}}),
             Special(kSpecialFunctionInventoryActivator, 21,
                     {{2, kEventSpecialFunctionZero, kActionRelayFire}, {3, kEventSpecialFunctionMaxReached, kActionRelayFire}}),
             Plain(kRelay, {{4, kEventRelayFired, kActionFogHintOn}}),
             Plain(kRelay, {{4, kEventRelayFired, kActionFogHintRemove}}), Plain(kVolumetricFogHint)};
  const Parsed p(f);
  const int hint = int(f.Entity(4));
  std::vector<PortRoomEnv::WorldVolume> volumes;
  const SceneryScripts r = MasterScripts(p.room, {0, 0, 0}, volumes);
  CHECK_MSG(volumes.empty(), "no trigger, no volume");
  CHECK_MSG(r.unresolved.count(hint) == 0 && r.unresolvedHow.count(hint) == 0, "the hint is fully resolved");
  CHECK_MSG(r.links.count(hint) == 1 && r.links.at(hint).size() == 2, "two links");
  if (r.links.count(hint) != 0 && r.links.at(hint).size() == 2) {
    const uint32_t sender = PortRoomEnv::kSenderCameraWaterItem + 21;
    bool held = false, missing = false;
    for (const PortRoomGeo::Link& l : r.links.at(hint)) {
      held = held || Same(l, sender, 0, PortRoomGeo::kShow);
      missing = missing || Same(l, sender, 1, PortRoomGeo::kHide);
    }
    CHECK_MSG(held, "with the item the fog comes on");
    CHECK_MSG(missing, "without it the fog is taken out");
  }
  // The same chain on another function is not resolved.
  Fixture wrong = f;
  wrong.comps[1] = Special(4, 21, f.comps[1].links);
  const Parsed pw(wrong);
  const SceneryScripts u = MasterScripts(pw.room, {0, 0, 0}, volumes);
  CHECK_MSG(u.links.empty() && u.unresolved.count(hint) == 1 && u.unresolved.at(hint) == 2, "another function stays unresolved");
}

// An EffectMP1 with GENP Guid(100 + genp) and, if `vars`, one external variable (a real).
Fixture::Comp EffectActor(int genp, bool vars = false) {
  Fixture::Comp c = Plain(kEffectMP1);
  Props props;
  if (genp >= 0) {
    props.Add(0x88fc08e6, Guid(size_t(100 + genp)));
  }
  if (vars) {
    Props guid;
    guid.Add(0x82dde61c, Guid(200));
    Bytes value;
    PutFloat(value, 0.65f);
    Props entry;
    entry.Add(0xae893172, guid.b).Add(0xfd41dc4b, value);
    Bytes list = U32(1);
    Append(list, entry.b);
    props.Add(0x3def0de6, list);
  }
  c.props = props.b;
  return c;
}

// An Effect object's properties as the room reader hands them over: nine four-byte ones the
// import does not read, then the object's own PART at byte 36 (big endian), which is where it
// compares a matched actor's GENP with it (port_remastered_room_effects.cpp).
Bytes ObjectProps(uint32_t part) {
  Bytes props(36, 0);
  for (int shift = 24; shift >= 0; shift -= 8) {
    props.push_back(uint8_t(part >> shift));
  }
  return props;
}

ScriptObject RetailObject(uint32_t id, uint8_t type, Vec3 pos, std::vector<uint32_t> targets = {},
                          uint32_t ownPart = 0) {
  ScriptObject o;
  o.id = id;
  o.type = type;
  o.hasPos = true;
  o.pos = pos;
  o.targets = std::move(targets);
  if (ownPart != 0) {
    o.props = ObjectProps(ownPart);
  }
  return o;
}

void TestRoomEffects() {
  // 0 added (layer 1, with a variable), 1 matched to a retail object, 2 connected (3 links to it),
  // 4 no GENP, 5 a GENP with no PART, 6 on a layer no entity matched, 7 matched with particle
  // variables (its retail object takes them).
  Fixture f;
  f.comps = {EffectActor(0, true), EffectActor(1), EffectActor(0), Plain(kRelay, {{2, kEventRelayFired, kActionAction}}),
             EffectActor(-1), EffectActor(2), EffectActor(0), EffectActor(3, true)};
  f.comps[0].pos[0] = 1.f;
  f.comps[0].pos[1] = 2.f;
  f.comps[0].pos[2] = 3.f;
  const Parsed p(f);
  Area area;
  area.mrea = 0x1A2B3C4D;
  area.xf = {{{1, 0, 0, 10}, {0, 1, 0, 20}, {0, 0, 1, 30}}};
  const Vec3 w = Apply(area.xf, MulR2G({1, 2, 3}));
  const Vec3 near{w[0] + 1, w[1], w[2]};
  // 0 near the added one: replaced. 1 far. 2 near but a retail connection reaches it. 3 sends
  // that. 4 the one actor 1 matched, whose GENP maps to its own PART, so it keeps it. 5 the one
  // actor 7 matched, which has no PART of its own and takes Remastered's.
  constexpr uint32_t kOwnRetailPart = 0x12345678;  // the PART actor 1's GENP maps to
  area.objects = {RetailObject(0x041A0010, 7, near), RetailObject(0x041A0011, 7, {w[0] + 50, w[1], w[2]}),
                  RetailObject(0x041A0012, 7, near), RetailObject(0x041A0013, 0x15, near, {0x041A0012}),
                  RetailObject(0x041A0014, 7, near, {}, kOwnRetailPart),
                  RetailObject(0x041A0015, 7, {w[0] + 50, w[1], w[2]})};
  SceneryScripts scripts;
  scripts.layer[int(f.Entity(0))] = 1;
  scripts.retail[int(f.Entity(1))] = 4;
  scripts.retail[int(f.Entity(7))] = 5;
  std::map<std::string, Bytes> written;
  RoomIO io;
  io.write = [&](const std::string& name, const Bytes& data) {
    written[name] = data;
    return true;
  };
  io.effect = [](const std::array<uint8_t, 16>& genp) -> uint32_t {
    if (genp == SwapUuid(Guid(101).data())) {
      return kOwnRetailPart;  // the object's own PART: it keeps its retail effect
    }
    return genp == SwapUuid(Guid(102).data()) ? 0 : 0xFE000000u | genp[0];
  };
  const std::string line = WriteRoomEffects("room", p.room, area.mrea, area, scripts, io);
  CHECK_MSG(line == "  room: 1 effect actor(s) added, 1 retail effect(s) replaced, 1 retail effect(s) take "
                    "Remastered's effect, 1 connected and left out, 2 matched a retail object (1 of them still on the "
                    "retail PART), 2 without a PART, 1 on an unknown layer",
            line.c_str());
  REQUIRE_MSG(written.size() == 1 && written.count("1A2B3C4D.roomfx") == 1, "one .roomfx, named by the MREA");
  PortRoomEffects::File file;
  std::string error;
  REQUIRE_MSG(PortRoomEffects::Parse(written.begin()->second, file, error), error.c_str());
  CHECK_MSG(file.skips == std::vector<uint32_t>{0x041A0010}, "the near unmatched, untargeted one is skipped");
  CHECK(file.connected == 1);
  REQUIRE(file.layers.size() == 1 && file.layers[0].layer == 1 && file.layers[0].count == 1);
  PortRoomEffects::Effect e;
  size_t used = 0;
  REQUIRE(PortRoomEffects::ReadEffect(file.layers[0].objects.data(), file.layers[0].objects.size(), e, used));
  CHECK(e.id == PortRoomEffects::AddOnId(1, 0x1A, 0));
  CHECK(e.part == (0xFE000000u | SwapUuid(Guid(100).data())[0]));
  CHECK(e.pos[0] == float(w[0]) && e.pos[1] == float(w[1]) && e.pos[2] == float(w[2]));
  CHECK(e.active && e.scale[0] == 1.f && e.duration == 0.2f && e.xrayVisorVisible);
  REQUIRE(file.layers[0].vars.size() == 1 && file.layers[0].vars[0].size() == 1);
  const PortRoomEffects::Variable& v = file.layers[0].vars[0][0];
  CHECK(v.real == 0.65f && std::equal(v.guid.begin(), v.guid.end(), Guid(200).begin()));

  // The object actor 7 matched takes the actor's PART (it has no PART of its own) and its
  // particle variable, and is left out of the add-ons.
  REQUIRE_MSG(file.overrides.size() == 1, "one override");
  const PortRoomEffects::Override& over = file.overrides[0];
  CHECK(over.id == 0x041A0015 && over.part == (0xFE000000u | SwapUuid(Guid(103).data())[0]));
  REQUIRE(over.vars.size() == 1);
  CHECK(over.vars[0].real == 0.65f && std::equal(over.vars[0].guid.begin(), over.vars[0].guid.end(), Guid(200).begin()));

  // A retail object already in the add-on range: nothing written.
  written.clear();
  Area taken = area;
  taken.objects.push_back(RetailObject(PortRoomEffects::AddOnId(0, 0x1A, 3), 7, near));
  const std::string refused = WriteRoomEffects("room", p.room, area.mrea, taken, scripts, io);
  CHECK_MSG(refused.find("is in the add-on id range") != std::string::npos && written.empty(), refused.c_str());
  // A GENP several retail PARTs share: the object matched to the actor keeps its own retail id
  // (the mod converted the GENP under it) and takes the actor's particle variables, which set
  // what differs between them (the lava spit's big and small).
  {
    Fixture g;
    g.comps = {EffectActor(0, true)};
    const Parsed q(g);
    constexpr uint32_t kSharedPart = 0x99998888;  // the object's own PART, one the GENP is shared with
    Area shared = area;
    shared.objects = {RetailObject(0x041A0016, 7, Apply(area.xf, MulR2G({0, 0, 0})), {}, kSharedPart)};
    SceneryScripts sharedScripts;
    sharedScripts.retail[int(g.Entity(0))] = 0;
    std::map<std::string, Bytes> out;
    RoomIO io2;
    io2.write = [&](const std::string& name, const Bytes& data) {
      out[name] = data;
      return true;
    };
    io2.effect = [](const std::array<uint8_t, 16>&) -> uint32_t { return 0; };  // no PART under the GENP's id
    io2.matched = [](const std::array<uint8_t, 16>& genp) -> std::vector<uint32_t> {
      return genp == SwapUuid(Guid(100).data()) ? std::vector<uint32_t>{kSharedPart} : std::vector<uint32_t>{};
    };
    const std::string l = WriteRoomEffects("room", q.room, area.mrea, shared, sharedScripts, io2);
    CHECK_MSG(l.find("1 retail effect(s) take Remastered's effect, 0 connected") != std::string::npos, l.c_str());
    PortRoomEffects::File f2;
    std::string e2;
    REQUIRE_MSG(out.count("1A2B3C4D.roomfx") == 1 && PortRoomEffects::Parse(out["1A2B3C4D.roomfx"], f2, e2), e2.c_str());
    REQUIRE_MSG(f2.overrides.size() == 1, "one override");
    CHECK(f2.overrides[0].id == 0x041A0016 && f2.overrides[0].part == kSharedPart);
    REQUIRE(f2.overrides[0].vars.size() == 1 && f2.overrides[0].vars[0].real == 0.65f);
    CHECK_MSG(f2.layers.empty() && f2.skips.empty(), "nothing added, nothing skipped");
  }
  // Without an effect lookup the stage writes nothing and says nothing.
  io.effect = nullptr;
  CHECK(WriteRoomEffects("room", p.room, area.mrea, area, scripts, io).empty() && written.empty());
}

Fixture::Comp Density(float intensity) {
  Fixture::Comp c = Plain(kEnvFxDensityController);
  Bytes v;
  PutFloat(v, intensity);
  c.props = Props().Add(kPropEnvFxIntensity, v).b;
  return c;
}

// The density controller a RelayAutoFire reaches through Relays sets the intensity; one only a
// trigger fires, or nothing, does not.
void TestEnvFxIntensity() {
  Fixture f;
  // 0 controller 3.0 (a Relay's trigger fires it), 1 controller 5.0 (autofire 3 -> relay 2 -> 1),
  // 2 Relay, 3 RelayAutoFire, 4 a TriggerMP1 -> relay 5 -> controller 0, 5 Relay, 6 inert
  // controller 7.0.
  f.comps = {Density(3.f),
             Density(5.f),
             Plain(kRelay, {{1, kEventRelayFired, kActionEnvFxApply}}),
             Plain(kRelayAutoFire, {{2, 0x9a06ae6f, kActionRelayFire}}),
             Plain(kTriggerMP1, {{5, kEventEntered, kActionRelayFire}}),
             Plain(kRelay, {{0, kEventRelayFired, kActionEnvFxApply}}),
             Density(7.f)};
  {
    const Parsed p(f);
    std::vector<std::string> notes;
    CHECK_MSG(EnvFxIntensity(p.room, notes) == 5.f, "envfx: the autofire chain's controller");
    REQUIRE(notes.size() == 2);
    CHECK_MSG(notes[0].find("#0") != std::string::npos && notes[0].find("not fired at load") != std::string::npos &&
                  notes[0].find("F526FC2A") != std::string::npos,
              notes[0].c_str());
    CHECK_MSG(notes[1].find("#6") != std::string::npos && notes[1].find("nothing fires it") != std::string::npos,
              notes[1].c_str());
  }
  // A wrong action on the first hop breaks the chain: nothing fires at load.
  f.comps[3].links[0].action = kActionCounterIncrement;
  {
    const Parsed p(f);
    std::vector<std::string> notes;
    CHECK_MSG(EnvFxIntensity(p.room, notes) == 1.f && notes.size() == 3, "envfx: no chain, intensity 1");
  }
  // Two Relays deep.
  f.comps[3].links[0].action = kActionRelayFire;
  f.comps.push_back(Plain(kRelay, {{2, kEventRelayFired, kActionRelayFire}}));
  f.comps[3].links[0].target = 7;
  {
    const Parsed p(f);
    std::vector<std::string> notes;
    CHECK_MSG(EnvFxIntensity(p.room, notes) == 5.f, "envfx: two relays deep");
  }
  // A cycle of relays ends.
  f.comps[2].links.push_back({7, kEventRelayFired, kActionRelayFire});
  {
    const Parsed p(f);
    std::vector<std::string> notes;
    CHECK_MSG(EnvFxIntensity(p.room, notes) == 5.f, "envfx: relay cycle");
  }
}

}  // namespace

int main() {
  TestFluidSenders();
  TestMasterTrigger();
  TestMasterCameraWater();
  TestRoomEffects();
  TestEnvFxIntensity();
  if (port_test::Failures() == 0) {
    std::printf("port_remastered_room_scripts_tests: ok\n");
  }
  return PORT_TEST_RESULT();
}
