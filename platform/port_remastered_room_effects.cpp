// Remastered's own particle effect actors as a room's .roomfx (port_room_effects.h).

#include "port_remastered_room_internal.h"
#include "port_room_effects.h"

namespace PortRemastered {
namespace RoomDetail {
namespace {

// EffectMP1 properties (SLdrEffectMP1::Load, 0xd9cd58), with the loader's defaults.
constexpr uint32_t kPropGenp = 0x88fc08e6;
constexpr uint32_t kPropHotInThermal = 0x7f176a6a;
constexpr uint32_t kPropNoTimer = 0x0baeccf6;
constexpr uint32_t kPropNoTimer2 = 0xff985718;  // CEffectMP1GOC::Create ORs it into the same flag
constexpr uint32_t kPropRebuild = 0x85963ef9;
constexpr uint32_t kPropUseRateInvCamDist = 0x40cff441;
constexpr uint32_t kPropRateInvCamDist = 0xcf6c4262;
constexpr uint32_t kPropRateInvCamDistRate = 0xed2add6d;
constexpr uint32_t kPropDuration = 0x4fca7292;
constexpr uint32_t kPropDurationResetWhileVisible = 0xeb2e007b;
constexpr uint32_t kPropUseCamDistRange = 0x86e4b31b;
constexpr uint32_t kPropCamDistRangeMin = 0x8dc7fd5e;
constexpr uint32_t kPropCamDistRangeMax = 0x45375a0b;
constexpr uint32_t kPropFarRate = 0x6d12ca92;
constexpr uint32_t kPropCombatVisor = 0x155fa3e4;
constexpr uint32_t kPropThermalVisor = 0x880e9b91;
constexpr uint32_t kPropXrayVisor = 0x2ada726b;
constexpr uint32_t kPropDieWhenDone = 0xf13f4e03;
constexpr uint32_t kPropExternalVariables = 0x3def0de6;  // CScriptEffectMP1::InitializeExternalVariables

constexpr uint8_t kRetailEffect = 7;
// A retail effect no object matched, with an added one this near, is the one it replaces.
constexpr double kReplaceDistance = 5.0;

// Rz * Ry * Rx of Remastered's angles in GameCube axes (the room geometry's `place`), then
// the area's rotation, as retail's editor angles in degrees.
void RetailAngles(const Vec3& rot, const Mat34& xf, float out[3]) {
  constexpr int kAxis[3] = {0, 2, 1};
  constexpr double kSign[3] = {-1, 1, 1};
  constexpr double kDeg = 3.14159265358979323846 / 180.0;
  double s[3], k[3];
  for (int i = 0; i < 3; ++i) {
    s[i] = std::sin(rot[size_t(i)] * kDeg);
    k[i] = std::cos(rot[size_t(i)] * kDeg);
  }
  const double m[3][3] = {
      {k[2] * k[1], k[2] * s[1] * s[0] - s[2] * k[0], k[2] * s[1] * k[0] + s[2] * s[0]},
      {s[2] * k[1], s[2] * s[1] * s[0] + k[2] * k[0], s[2] * s[1] * k[0] - k[2] * s[0]},
      {-s[1], k[1] * s[0], k[1] * k[0]},
  };
  double g[3][3], w[3][3];
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col) {
      g[row][col] = kSign[row] * kSign[col] * m[kAxis[row]][kAxis[col]];
    }
  }
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col) {
      w[row][col] = xf[size_t(row)][0] * g[0][col] + xf[size_t(row)][1] * g[1][col] + xf[size_t(row)][2] * g[2][col];
    }
  }
  const double sy = std::clamp(-w[2][0], -1.0, 1.0);
  double x, z;
  if (std::fabs(sy) > 0.99999) {
    x = 0;
    z = std::atan2(-w[0][1], w[1][1]);
  } else {
    x = std::atan2(w[2][1], w[2][2]);
    z = std::atan2(w[1][0], w[0][0]);
  }
  out[0] = float(x / kDeg);
  out[1] = float(std::asin(sy) / kDeg);
  out[2] = float(z / kDeg);
}

}  // namespace

std::set<int> ConnectedEntities(const Room& room) {
  const std::vector<Component>& comps = room.Components();
  std::set<int> out;
  for (const Connection& c : ReadConnections(room)) {
    const int t = room.ByGuid(c.target);
    if (t >= 0) {
      out.insert(comps[size_t(t)].type == kEntity ? t : comps[size_t(t)].entity);
    }
  }
  return out;
}

std::string WriteRoomEffects(const std::string& name, const Room& room, uint32_t mrea, const Area& area,
                             const SceneryScripts& scripts, const RoomIO& io) {
  const std::vector<const Component*> effects = room.Of(kEffectMP1);
  if (!io.effect || !io.write || effects.empty()) {
    return {};
  }
  char line[384];
  const std::vector<ScriptObject>& objects = area.objects;
  if (objects.empty()) {
    return "  " + name + ": no retail objects, its effect actors left out";
  }
  for (const ScriptObject& o : objects) {
    if (PortRoomEffects::IsAddOnId(o.id)) {
      std::snprintf(line, sizeof line, "  %s: retail object %08X is in the add-on id range, its effect actors left out",
                    name.c_str(), o.id);
      return line;
    }
  }
  const uint32_t areaBits = (objects[0].id >> 16) & 0x3FF;
  const std::set<int> connected = ConnectedEntities(room);
  std::set<uint32_t> targeted;  // retail ids some retail connection reaches
  for (const ScriptObject& o : objects) {
    for (uint32_t t : o.targets) {
      targeted.insert(t & 0x3FFFFFF);
    }
  }
  std::set<size_t> matchedObjects;
  for (const auto& [entity, index] : scripts.retail) {
    matchedObjects.insert(index);
  }

  std::map<uint32_t, PortRoomEffects::Layer> layers;
  std::set<uint32_t> skips;
  std::vector<PortRoomEffects::Override> overrides;
  std::vector<Vec3> added;
  std::set<int> seen;
  size_t connectedCount = 0, noPart = 0, noLayer = 0, kept = 0, keptOwn = 0, overridden = 0, full = 0, badVars = 0;
  for (const Component* c : effects) {
    if (c->entity < 0 || !seen.insert(c->entity).second) {
      continue;
    }
    const auto f = room.Flat(*c);
    const auto genp = f.find(kPropGenp);
    if (genp == f.end() || genp->second.size != 16) {
      ++noPart;
      continue;
    }
    const std::array<uint8_t, 16> genpId = SwapUuid(room.Bytes(genp->second));
    uint32_t part = io.effect(genpId);
    // The retail PARTs the effects step wrote this GENP under by its table: one GENP can stand
    // for several, the lava spit's big and small. A retail object drawing one of them keeps its
    // own id, which is now the mod's conversion of that GENP.
    const std::vector<uint32_t> paired = io.matched != nullptr ? io.matched(genpId) : std::vector<uint32_t>();
    // One the matcher paired with a retail object is that object, which stays, whatever
    // connects to it; if Remastered gave it an effect of its own, or it has particle
    // variables, the object takes those (an override).
    const auto matched = scripts.retail.find(c->entity);
    if (matched != scripts.retail.end()) {
      const ScriptObject& o = objects[matched->second];
      if (o.type != kRetailEffect) {
        ++kept;
        continue;
      }
      std::vector<PortRoomEffects::Variable> vars;
      const auto ext = f.find(kPropExternalVariables);
      if (ext != f.end() && !PortRoomEffects::ReadExternalVariables(room.Bytes(ext->second), ext->second.size, vars)) {
        ++badVars;
        vars.clear();
      }
      // The variables are the GENP's: they go only with its PART, the Remastered-only one or
      // the one the mod gives the object's own retail id (its own of the GENP's, as big and
      // small spit share one).
      const uint32_t own = o.props.size() >= 40 ? ReadBE32(&o.props[36]) : 0;
      if (std::find(paired.begin(), paired.end(), own) != paired.end()) {
        part = own;
      }
      const bool ownPart = part != 0 && (part >> 24) == 0xFE && part != own;
      if (!ownPart && (part == 0 || part != own || vars.empty())) {
        ++keptOwn;
        continue;
      }
      overrides.push_back({o.id, part, std::move(vars)});
      ++overridden;
      continue;
    }
    if (connected.count(c->entity) != 0) {
      ++connectedCount;
      continue;
    }
    if (part == 0) {
      ++noPart;
      continue;
    }
    uint32_t retailLayer = 0;
    if (!c->baseLayer) {
      const auto layer = scripts.layer.find(c->entity);
      if (layer == scripts.layer.end()) {
        ++noLayer;  // a layer none of whose entities matched: which retail one it is is unknown
        continue;
      }
      retailLayer = layer->second;
    }
    Vec3 pos, rot, scale;
    if (!room.Xform(*c, pos, rot, scale)) {
      ++noPart;
      continue;
    }
    if (added.size() >= PortRoomEffects::kMaxAddOns) {
      ++full;
      continue;
    }
    PortRoomEffects::Effect e;
    e.id = PortRoomEffects::AddOnId(retailLayer, areaBits, uint32_t(added.size()));
    e.name = "Remastered effect " + UuidText(room.Bytes(genp->second));
    const Vec3 w = Apply(area.xf, MulR2G(pos));
    constexpr int kAxis[3] = {0, 2, 1};
    for (size_t i = 0; i < 3; ++i) {
      e.pos[i] = float(w[i]);
      e.scale[i] = float(scale[size_t(kAxis[i])]);
    }
    RetailAngles(rot, area.xf, e.rot);
    e.part = part;
    e.active = room.Active(*c);
    auto flag = [&](uint32_t id, bool fallback) {
      const auto it = f.find(id);
      return it != f.end() && it->second.size >= 1 ? room.Bytes(it->second)[0] != 0 : fallback;
    };
    auto real = [&](uint32_t id, float fallback) {
      const auto it = f.find(id);
      return it != f.end() && it->second.size >= 4 ? ReadLEFloat(room.Bytes(it->second)) : fallback;
    };
    e.hotInThermal = flag(kPropHotInThermal, false);
    e.noTimerUnlessAreaOccluded = flag(kPropNoTimer, false) || flag(kPropNoTimer2, false);
    e.rebuildSystemsOnActivate = flag(kPropRebuild, false);
    e.useRateInverseCamDist = flag(kPropUseRateInvCamDist, false);
    e.rateInverseCamDist = real(kPropRateInvCamDist, 5.0f);
    e.rateInverseCamDistRate = real(kPropRateInvCamDistRate, 0.5f);
    e.duration = real(kPropDuration, 0.2f);
    e.durationResetWhileVisible = real(kPropDurationResetWhileVisible, 0.1f);
    e.useRateCamDistRange = flag(kPropUseCamDistRange, false);
    e.rateCamDistRangeMin = real(kPropCamDistRangeMin, 20.0f);
    e.rateCamDistRangeMax = real(kPropCamDistRangeMax, 30.0f);
    e.rateCamDistRangeFarRate = real(kPropFarRate, 0.0f);
    e.combatVisorVisible = flag(kPropCombatVisor, true);
    e.thermalVisorVisible = flag(kPropThermalVisor, true);
    e.xrayVisorVisible = flag(kPropXrayVisor, true);
    e.dieWhenSystemsDone = flag(kPropDieWhenDone, false);
    std::vector<PortRoomEffects::Variable> vars;
    const auto ext = f.find(kPropExternalVariables);
    if (ext != f.end() && !PortRoomEffects::ReadExternalVariables(room.Bytes(ext->second), ext->second.size, vars)) {
      ++badVars;  // the effect still comes, with its system's defaults
      vars.clear();
    }
    PortRoomEffects::Layer& layer = layers[retailLayer];
    layer.layer = retailLayer;
    PortRoomEffects::AppendEffect(e, layer.objects);
    layer.vars.push_back(std::move(vars));
    ++layer.count;
    added.push_back(w);
  }
  // A retail effect no Remastered entity matched, that nothing sends a message to, next to an
  // added one: Remastered replaced it.
  size_t replacedNear = 0;
  for (size_t j = 0; j < objects.size(); ++j) {
    const ScriptObject& o = objects[j];
    if (o.type != kRetailEffect || !o.hasPos || matchedObjects.count(j) != 0 || skips.count(o.id) != 0 ||
        targeted.count(o.id & 0x3FFFFFF) != 0) {
      continue;
    }
    for (const Vec3& w : added) {
      if (Distance(w, o.pos) < kReplaceDistance) {
        skips.insert(o.id);
        ++replacedNear;
        break;
      }
    }
  }
  std::snprintf(line, sizeof line,
                "  %s: %zu effect actor(s) added, %zu retail effect(s) replaced, %zu retail effect(s) take "
                "Remastered's effect, %zu connected and left out, %zu matched a retail object (%zu of them still on "
                "the retail PART), %zu without a PART, %zu on an unknown layer%s%s",
                name.c_str(), added.size(), replacedNear, overridden, connectedCount, kept + keptOwn + overridden,
                keptOwn, noPart, noLayer, full != 0 ? ", the id range is full" : "",
                badVars != 0 ? ", unreadable particle variables" : "");
  if (added.empty() && overrides.empty() && skips.empty()) {
    return line;
  }
  PortRoomEffects::File file;
  file.skips.assign(skips.begin(), skips.end());
  file.connected = uint32_t(connectedCount);
  for (auto& [index, layer] : layers) {
    file.layers.push_back(std::move(layer));
  }
  file.overrides = std::move(overrides);
  char fileName[32];
  std::snprintf(fileName, sizeof fileName, "%08X.roomfx", mrea);
  if (!io.write(fileName, PortRoomEffects::Write(file))) {
    return "  " + name + ": could not write " + fileName;
  }
  return line;
}

std::vector<std::array<uint8_t, 16>> RoomEffectGenpsOf(const Room& room) {
  std::vector<std::array<uint8_t, 16>> out;
  const std::set<int> connected = ConnectedEntities(room);
  for (const Component* c : room.Of(kEffectMP1)) {
    if (c->entity < 0 || connected.count(c->entity) != 0) {
      continue;
    }
    const auto f = room.Flat(*c);
    const auto genp = f.find(kPropGenp);
    if (genp != f.end() && genp->second.size == 16) {
      out.push_back(SwapUuid(room.Bytes(genp->second)));
    }
  }
  return out;
}

}  // namespace RoomDetail

std::vector<std::array<uint8_t, 16>> RoomEffectGenps(const std::vector<RoomPak>& rooms) {
  std::vector<std::array<uint8_t, 16>> out;
  std::set<std::array<uint8_t, 16>> seen;
  for (const RoomPak& rp : rooms) {
    if (rp.pak == nullptr) {
      continue;
    }
    for (const PakAsset& asset : rp.pak->Assets()) {
      if (asset.type != port::FourCC("ROOM")) {
        continue;
      }
      std::vector<uint8_t> bytes;
      std::string error;
      RoomDetail::Room room;
      if (!rp.pak->ReadAsset(asset, bytes, error) || !room.Parse(bytes, error)) {
        continue;
      }
      for (const auto& genp : RoomDetail::RoomEffectGenpsOf(room)) {
        if (seen.insert(genp).second) {
          out.push_back(genp);
        }
      }
      break;
    }
  }
  return out;
}

}  // namespace PortRemastered
