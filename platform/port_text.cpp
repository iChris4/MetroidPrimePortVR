// The port's own in-game text (port_text.h).
//
// Built as part of mp_game, as it reads the table through the game's resource
// loader.

#include "port_text.h"

#include "Kyoto/CResFactory.hpp"
#include "Kyoto/SObjectTag.hpp"
#include "Kyoto/Streams/CInputStream.hpp"
#include "Kyoto/Text/CStringTable.hpp"
#include "rstl/auto_ptr.hpp"

#include "port_log.h"
#include "port_savestate.h"

#include <vector>

namespace PortText {
namespace {

const wchar_t* const kEnglish[] = {
    L"Exit Game",
    L"Warp to start",
    L"Mark room",
    L"Unmark room",
    L"Major",
    L"Minor",
    L"Collected",
    L"Pickup",
    L"Marked",
    L"Aspect Ratio",
    L"Widescreen HUD",
    L"Twin Stick Aim",
    L"Stick Aim Speed",
    L"Fast Morph",
    L"Toggle Lock-On",
    L"Shoot While Grappling",
    L"Shoot in Scan Visor",
    L"Toggle Grapple",
    L"Toggle Spider Ball",
    L"Aim Lock-On",
    L"Sticky Charge",
    L"Remastered Charge",
    L"Field of View",
    L"Morph Ball Follows FOV",
    L"Arm Cannon Follows FOV",
    L"Arm Cannon FOV",
    L"Anti-Aliasing",
    L"HUD Scale",
    L"Hide Helmet",
    L"Hide Visor Effects",
    L"In-Game Timer",
    L"Reveal Map",
    L"Pickup Dots",
    L"Crosshair Size",
    L"Skippable Cutscenes",
    L"Elevator Ride",
    L"Original Experience",
    L"Photo Mode",
    L"4:3",
    L"16:9",
    L"Window",
    L"Original",
    L"Fast",
    L"Skip",
    L"Off",
    L"Seen",
    L"All",
    L"Rooms",
};
static_assert(sizeof(kEnglish) / sizeof(kEnglish[0]) == kCount, "one English string per PortText::Id");

const CStringTable* sTable = nullptr;
// The mod reload the table was looked up after; -1 before the first lookup.
int sLookedUp = -1;

// A mod's table, else null. Loaded once per mod reload; replaced tables stay
// allocated, as panes keep pointers into them.
const CStringTable* Table() {
  const int reloads = PortSaveState::ModReloads();
  if (sLookedUp == reloads || gpResourceFactory == nullptr) {
    return sTable;
  }
  sLookedUp = reloads;
  sTable = nullptr;
  const SObjectTag tag('STRG', kTableId);
  if (gpResourceFactory->GetResLoader().ResourceExists(tag)) {
    rstl::auto_ptr< CInputStream > in = gpResourceFactory->GetResLoader().LoadNewResourceSync(tag, NULL);
    if (in.get() != NULL) {
      sTable = rs_new CStringTable(*in);
      PortLog::Write("text: the port's strings come from a mod (%d of %d)\n", sTable->GetStringCount(),
                     int(kCount));
    }
  }
  return sTable;
}

}  // namespace

const wchar_t* English(Id id) { return id >= 0 && id < kCount ? kEnglish[id] : L""; }

const wchar_t* Get(Id id) {
  const CStringTable* table = Table();
  if (table != nullptr && id >= 0 && id < table->GetStringCount()) {
    const wchar_t* text = table->GetString(id);
    if (text[0] != L'\0') {
      return text;
    }
  }
  return English(id);
}

}  // namespace PortText
