#include "port_hints.h"

#include "port_ap_metroidprime.h"
#include "port_apclient.h"
#include "port_custom_res.h"
#include "port_randomizer.h"

#include <map>
#include <mutex>

namespace PortHints {
namespace {

namespace Prime = PortAp::MetroidPrime;

// randomprime's "Totem N" STRGs (Metroid4.pak), Truth to Newborn, which is
// also item type order.
constexpr uint32_t kTotemStrings[] = {
    0xFAE3D58Eu, 0x7C77A720u, 0xB72B7485u, 0xAA2E443Du, 0x61729798u, 0xE7E6E536u,
    0x2CBA3693u, 0xDDEC8446u, 0x16B057E3u, 0x8E9C7387u, 0x45C0A022u, 0xC354D28Cu,
};

// A pickup whose scan this file wrote, by the STRG id of that scan.
struct Pickup {
  uint32_t world = 0, area = 0, entity = 0;
  int itemType = 0;
  bool randomized = false;
};

struct Watched {
  std::mutex mutex;
  std::map<uint32_t, Pickup> pickups;
};

Watched& GetWatched() {
  static Watched watched;
  return watched;
}

// The offline seed's hint, in the AP world's words.
bool SeedHint(int itemType, std::string& text) {
  uint32_t world = 0, area = 0, entity = 0;
  if (!PortRandomizer::FindItem(itemType, world, area, entity))
    return false;
  const Prime::Location* location = Prime::FindPickup(world, area, entity);
  const char* name = Prime::ItemName(Prime::kItemBase + itemType);
  if (location == nullptr || name == nullptr)
    return false;
  text = std::string("The &push;&main-color=#c300ff;") + name +
         "&pop; can be found in &push;&main-color=#89a1ff;" + location->name + "&pop;.";
  return true;
}

// Archipelago's view of the location first (it can change as scouts and
// names arrive), then the offline seed's item. An Archipelago location not
// scouted yet gives true and no text.
bool PickupText(const Pickup& pickup, std::string& text) {
  if (PortAp::PickupScanText(pickup.world, pickup.area, pickup.entity, text))
    return true;
  const char* name = pickup.randomized ? Prime::ItemName(Prime::kItemBase + pickup.itemType) : nullptr;
  if (name == nullptr)
    return false;
  text = name;
  return true;
}

bool TotemText(uint32_t strgId, std::string& text) {
  for (int i = 0; i < 12; ++i) {
    if (kTotemStrings[i] != strgId)
      continue;
    const int itemType = Prime::kArtifactTruth + i;
    return PortAp::ArtifactHint(itemType, text) || SeedHint(itemType, text);
  }
  return false;
}

bool IsTotem(uint32_t strgId) {
  for (uint32_t id : kTotemStrings) {
    if (id == strgId)
      return true;
  }
  return false;
}

} // namespace

bool PickupScan(uint32_t world, uint32_t area, uint32_t entity, int itemType, bool randomized,
                uint32_t& scanId) {
  const Pickup pickup{world, area, entity, itemType, randomized};
  std::string text;
  if (!PickupText(pickup, text))
    return false;
  // Still a check, not the retail item; the scan updates once scouted.
  if (text.empty())
    text = "Archipelago item";
  // Keyed by location, so the scan keeps its id while its text changes.
  scanId = PortCustomRes::TextScan((uint64_t(area) << 32) | entity, text);
  if (scanId == 0)
    return false;
  Watched& watched = GetWatched();
  std::lock_guard<std::mutex> lock(watched.mutex);
  watched.pickups[scanId + 1] = pickup;
  return true;
}

void ScanCompleted(uint32_t strgId) {
  for (int i = 0; i < 12; ++i) {
    if (kTotemStrings[i] == strgId) {
      PortAp::ArtifactScanned(Prime::kArtifactTruth + i);
      return;
    }
  }
}

bool IsWatched(uint32_t strgId) {
  if (IsTotem(strgId))
    return true;
  Watched& watched = GetWatched();
  std::lock_guard<std::mutex> lock(watched.mutex);
  return watched.pickups.count(strgId) != 0;
}

bool WatchedText(uint32_t strgId, std::u16string& text) {
  std::string utf8;
  if (IsTotem(strgId)) {
    if (!TotemText(strgId, utf8))
      return false;
  } else {
    Pickup pickup;
    {
      Watched& watched = GetWatched();
      std::lock_guard<std::mutex> lock(watched.mutex);
      const auto found = watched.pickups.find(strgId);
      if (found == watched.pickups.end())
        return false;
      pickup = found->second;
    }
    // Not scouted (or no longer, after a reconnect): keep what it said.
    if (!PickupText(pickup, utf8) || utf8.empty())
      return false;
  }
  text = PortCustomRes::Utf16(utf8);
  return true;
}

} // namespace PortHints
