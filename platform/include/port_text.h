#pragma once

#include <stdint.h>

// The text the port adds to the game's own screens (main menu, options, map,
// save station), translatable like the game's: it lives in a string table of
// its own, STRG kTableId, which no disc has. The port's English is built in; a
// mod that ships "504F5254.STRG" replaces it string by string, in whichever
// language sections it has (the language setting picks one as it does for the
// disc's tables). A string the mod's table lacks (an older table, an empty
// string) stays English. docs/MODS.md lists the strings by index.
namespace PortText {

constexpr uint32_t kTableId = 0x504F5254;  // 'PORT'

// The table's string indices. Append only: a mod's table is indexed by these.
enum Id {
  kExitGame,
  kWarpToStart,
  kMarkRoom,
  kUnmarkRoom,
  kLegendMajor,
  kLegendMinor,
  kLegendCollected,
  kLegendPickup,
  kLegendMarked,
  kOptAspectRatio,
  kOptWidescreenHud,
  kOptTwinStick,
  kOptAimSpeed,
  kOptFastMorph,
  kOptLockOnToggle,
  kOptGrappleShoot,
  kOptScanShoot,
  kOptGrappleToggle,
  kOptSpiderToggle,
  kOptAimLockOn,
  kOptStickyCharge,
  kOptRapidCharge,
  kOptFov,
  kOptBallFollowsFov,
  kOptGunFollowsFov,
  kOptGunFov,
  kOptAntiAliasing,
  kOptHudScale,
  kOptHideHelmet,
  kOptHideVisorEffects,
  kOptSpeedrunTimer,
  kOptRevealMap,
  kOptMapPickups,
  kOptCrosshairSize,
  kOptSkippableCutscenes,
  kOptElevatorRide,
  kOptOriginalExperience,
  kOptPhotoMode,
  kAspect4x3,
  kAspect16x9,
  kAspectWindow,
  kElevatorOriginal,
  kElevatorFast,
  kElevatorSkip,
  kPickupsOff,
  kPickupsSeen,
  kPickupsAll,
  kPickupsRooms,
  kCount
};

// The string in the current language. The pointer stays valid for the run
// (panes keep it; a table replaced by a mod reload is never freed).
const wchar_t* Get(Id id);
// The built-in English.
const wchar_t* English(Id id);

}  // namespace PortText
