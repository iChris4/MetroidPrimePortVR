// Opt-in lifecycle driver for real-disc regression runs (MP_ENABLE_SMOKE_DRIVER).
#include "port_env.h"
#include "port_log.h"
#include "compat.h"
#include "port_debug.h"
#include "port_mouse.h"
#include "port_smoke.h"
#include "port_console.h"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/CWorld.hpp"
#include "MetroidPrime/CGameArea.hpp"
#include "Kyoto/CARAMToken.hpp"
#include "MetroidPrime/Cameras/CCameraManager.hpp"
#include "MetroidPrime/Cameras/CFirstPersonCamera.hpp"
#include "MetroidPrime/Player/CPlayer.hpp"
#include "MetroidPrime/Player/CPlayerGun.hpp"
#include "MetroidPrime/Player/CPlayerState.hpp"
#include "MetroidPrime/Player/CGameState.hpp"
#include "MetroidPrime/CMemoryCard.hpp"
#include "MetroidPrime/CSaveGameScreen.hpp"
#include "Kyoto/Graphics/CGraphics.hpp"
#include "Kyoto/Math/CQuaternion.hpp"
#include "Kyoto/Math/CRelAngle.hpp"
#include "MetroidPrime/ScriptObjects/CScriptWater.hpp"
#include "MetroidPrime/CFluidPlaneCPU.hpp"
#include "MetroidPrime/TCastTo.hpp"
#include <dolphin/pad.h>
#include <SDL3/SDL.h>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>

namespace aurora {
void request_screenshot() noexcept;
}

namespace {
unsigned sMouseTicks = 0;
unsigned sMouseShots = 0, sMouseChargedShots = 0, sMouseMissiles = 0, sMouseLocks = 0;
unsigned sGunViewChecks = 0;
bool sMouseJumped = false;
bool sMouseMorphed = false, sMouseResumed = false;
bool sStrafedBeforeUI = false, sStrafedAfterUI = false;
bool sPowerProjectileSeen = false, sMissileProjectileSeen = false, sBombSeen = false;
bool sMouseComplete = false;
unsigned sAreaReloads = 0;
bool AreaReloadEnabled() {
  static const bool enabled = port::EnvFlag("MP_SMOKE_AREA_RELOAD");
  return enabled;
}
bool sWasLocked = false;
CVector3f sLastLockedDirection(0.f, 1.f, 0.f);
CVector3f sInitialDirection(0.f, 1.f, 0.f);
void MouseCheck(bool valid, const char* message) {
  if (!valid) {
    PortLog::Write("[mouse-smoke] failed at tick %u: %s\n", sMouseTicks, message);
    std::abort();
  }
}
}

void PortSmokeAreaReload(CStateManager& mgr) {
  static const unsigned pauseAfterTicks = [] {
    const char* value = port::EnvString("MP_SMOKE_PAUSE");
    return value != nullptr ? static_cast<unsigned>(std::strtoul(value, nullptr, 10)) : 0;
  }();
  static unsigned sPauseTicks = 0;
  if (pauseAfterTicks != 0 && mgr.GetGameState() == CStateManager::kGS_Running &&
      mgr.GetCameraManager()->IsInFPCamera() &&
      !mgr.GetCameraManager()->IsInCinematicCamera()) {
    if (++sPauseTicks == pauseAfterTicks) {
      PortLog::Write("[smoke] entering pause screen\n");
      mgr.EnterPauseScreen();
    }
  }
  // MP_SMOKE_MAP=<ticks>: press Z to open the map screen. Z opens the map from
  // gameplay; in the pause screen it does nothing.
  static const unsigned mapAfterTicks = [] {
    const char* value = port::EnvString("MP_SMOKE_MAP");
    return value != nullptr ? static_cast<unsigned>(std::strtoul(value, nullptr, 10)) : 0;
  }();
  if (mapAfterTicks != 0 && mgr.GetGameState() == CStateManager::kGS_Running &&
      mgr.GetCameraManager()->IsInFPCamera() &&
      !mgr.GetCameraManager()->IsInCinematicCamera()) {
    static unsigned sMapTicks = 0;
    if (++sMapTicks == mapAfterTicks) {
      PortLog::Write("[smoke] pressing Z for the map screen\n");
    }
    if (sMapTicks >= mapAfterTicks && sMapTicks < mapAfterTicks + 20) {
      PADStatus status{};
      status.err = PAD_ERR_NONE;
      status.button = PAD_TRIGGER_Z;
      PADSetVirtualStatus(0, &status);
    }
  }
  static const bool morphEnabled = port::EnvFlag("MP_SMOKE_MORPH");
  static unsigned sMorphTicks = 0;
  static bool sMorphDone = false;
  if (morphEnabled && !sMorphDone && mgr.GetGameState() == CStateManager::kGS_Running &&
      mgr.GetCameraManager()->IsInFPCamera() &&
      !mgr.GetCameraManager()->IsInCinematicCamera()) {
    ++sMorphTicks;
    PADStatus status{};
    status.err = PAD_ERR_NONE;
    if ((sMorphTicks >= 120 && sMorphTicks < 126) || (sMorphTicks >= 360 && sMorphTicks < 366)) {
      status.button = PAD_BUTTON_X;
    }
    PADSetVirtualStatus(0, &status);
    if (sMorphTicks == 1) {
      PortLog::Write("[morph-smoke] begin\n");
    }
    if (sMorphTicks > 480) {
      sMorphDone = true;
      PADClearVirtualStatus(0);
      PortLog::Write("[morph-smoke] done\n");
    }
  }
  if (!AreaReloadEnabled() || sAreaReloads >= 3 || mgr.GetGameState() != CStateManager::kGS_Running ||
      mgr.GetCameraManager()->IsInCinematicCamera() || !mgr.GetCameraManager()->IsInFPCamera()) return;
  CWorld* world = mgr.World();
  if (!world || !world->DoesAreaExist(world->GetCurrentAreaId())) return;
  CGameArea* area = world->Area(world->GetCurrentAreaId());
  if (!area->IsLoaded() || area->GetOcclusionState() != CGameArea::kOS_Visible) return;
  GXDrawDone();
  PortLog::Write("[area-smoke] evict/restore area %d cycle %u\n", area->GetId().Value(), sAreaReloads + 1);
  area->SetOcclusionState(CGameArea::kOS_Occluded);
  const uint64_t deadline = SDL_GetTicksNS() + 5000000000ull;
  while (!area->TransferTokensToARAM()) {
    CARAMToken::UpdateAllDMAs();
    MouseCheck(SDL_GetTicksNS() < deadline, "area eviction stalled");
  }
  area->SetOcclusionState(CGameArea::kOS_Visible);
  ++sAreaReloads;
  if (sAreaReloads == 3) PortLog::Write("[area-smoke] passed: three geometry eviction/reload cycles\n");
}

// Set once MP_SMOKE_WORLD has run all its restarts, so MP_SMOKE_WALK can start
// in the destination room instead of spending itself at the opening spawn.
static bool sWorldSmokeDone = false;
static bool WorldSmokePending() {
  static const bool requested = port::EnvString("MP_SMOKE_WORLD") != nullptr;
  return requested && !sWorldSmokeDone;
}

void PortSmokeWorldTeleport(CStateManager& mgr) {
  static uint32_t sTargetWorld = 0;
  static bool sTargetResolved = false;
  if (!sTargetResolved) {
    const char* value = port::EnvString("MP_SMOKE_WORLD");
    if (value == nullptr) {
      sTargetResolved = true;
      return;
    }
    if (std::strcmp(value, "auto") == 0) {
      if (gpGameState == nullptr || gpMemoryCard == nullptr) return;
      const rstl::vector< CMemoryCard::MemoryWorld >& worlds = gpMemoryCard->GetMemoryWorlds();
      const uint32_t current = gpGameState->CurrentWorldAssetId();
      for (int i = 0; i < worlds.size(); ++i) {
        if (worlds[i].first != current) {
          sTargetWorld = static_cast< uint32_t >(worlds[i].first);
          break;
        }
      }
      if (sTargetWorld == 0) return;
    } else {
      sTargetWorld = static_cast< uint32_t >(std::strtoul(value, nullptr, 16));
    }
    sTargetResolved = true;
  }
  if (sTargetWorld == 0) return;
  const uint32_t targetWorld = sTargetWorld;

  // Repeat the exact overlay restart for lifetime regressions. Do not compare
  // manager/world addresses: their allocations can be reused after a restart.
  static const unsigned restartCount = [] {
    const char* value = port::EnvString("MP_SMOKE_WORLD_RESTARTS");
    return value != nullptr ? static_cast< unsigned >(std::strtoul(value, nullptr, 10)) : 1u;
  }();
  static const uint32_t targetArea = [] {
    const char* value = port::EnvString("MP_SMOKE_WORLD_AREA");
    return value != nullptr ? static_cast< uint32_t >(std::strtoul(value, nullptr, 16)) : 0u;
  }();
  static const unsigned warmupTicks = [] {
    const char* value = port::EnvString("MP_SMOKE_WORLD_TICKS");
    return value != nullptr ? static_cast< unsigned >(std::strtoul(value, nullptr, 10)) : 120u;
  }();
  // An explicit tick delay also permits a restart during the opening camera,
  // including the first GUI tick before the mapper has locked its area STRG.
  static const bool waitForCamera = port::EnvString("MP_SMOKE_WORLD_TICKS") == nullptr;
  static unsigned sWarmupTicks = 0;
  static unsigned sRestarts = 0;
  static bool sWaiting = false;
  if (sRestarts >= restartCount || mgr.GetWantsToQuit()) return;

  if (!sWaiting) {
    if (mgr.GetGameState() != CStateManager::kGS_Running ||
        (waitForCamera && (!mgr.GetCameraManager()->IsInFPCamera() ||
                           mgr.GetCameraManager()->IsInCinematicCamera()))) {
      return;
    }
    if (++sWarmupTicks < warmupTicks) return;
    PortLog::Write("[world-smoke] requesting world %08X restart %u/%u\n", targetWorld,
                   sRestarts + 1, restartCount);
    PortDebug::RequestWorldTeleport(targetWorld, targetArea);
    sWaiting = true;
    sWarmupTicks = 0;
    return;
  }

  if (gpGameState == nullptr || gpGameState->CurrentWorldAssetId() != targetWorld) return;
  if (mgr.GetGameState() != CStateManager::kGS_Running || mgr.World() == nullptr) return;
  if (mgr.World()->IGetWorldAssetId() != targetWorld) return;
  sWaiting = false;
  ++sRestarts;
  sWorldSmokeDone = sRestarts >= restartCount;
  PortLog::Write("[world-smoke] passed: world %08X area %d restart %u/%u\n", targetWorld,
                 mgr.World()->GetCurrentAreaId().Value(), sRestarts, restartCount);
}

// MP_SMOKE_ELEVATOR=<ticks>: ride the last elevator teleporter the current world
// constructed, that many gameplay ticks after the world is running. It sends
// the teleporter the same Play and SetToZero messages the elevator's scripts
// do, so the model transition and the mid-update world switch both run, unlike
// MP_SMOKE_WORLD's debug restart. Reports `[elevator-smoke] passed` once the
// destination world is running.
namespace {
TUniqueId sElevatorUid = kInvalidUniqueId;
CAssetId sElevatorOwnerWorld = kInvalidAssetId;
CAssetId sElevatorDestWorld = kInvalidAssetId;
}

void PortSmokeElevatorLoaded(TUniqueId uid, CAssetId worldId, CAssetId areaId) {
  const CAssetId owner = gpGameState != nullptr ? gpGameState->CurrentWorldAssetId() : kInvalidAssetId;
  PortLog::Write("[elevator-smoke] world %08X loaded elevator %04X -> world %08X area %08X\n",
                 owner, uid.value, worldId, areaId);
  sElevatorUid = uid;
  sElevatorOwnerWorld = owner;
  sElevatorDestWorld = worldId;
}

void PortSmokeElevator(CStateManager& mgr) {
  static const unsigned delayTicks = [] {
    const char* value = port::EnvString("MP_SMOKE_ELEVATOR");
    return value != nullptr ? static_cast< unsigned >(std::strtoul(value, nullptr, 10)) : 0u;
  }();
  enum { kWaiting, kPlaying, kRiding, kDone };
  static int sPhase = kWaiting;
  static unsigned sTicks = 0;
  static CAssetId sTicksWorld = kInvalidAssetId;
  static CAssetId sDestWorld = kInvalidAssetId;
  static std::chrono::steady_clock::time_point sRideStart;
  if (delayTicks == 0 || sPhase == kDone || mgr.GetWantsToQuit() || mgr.World() == nullptr) return;
  const CAssetId world = mgr.World()->IGetWorldAssetId();
  if (world != sTicksWorld) {
    sTicksWorld = world;
    sTicks = 0;
  }
  const bool playing = mgr.GetGameState() == CStateManager::kGS_Running &&
                       mgr.GetCameraManager()->IsInFPCamera() &&
                       !mgr.GetCameraManager()->IsInCinematicCamera();
  switch (sPhase) {
  case kWaiting:
    if (!playing || ++sTicks < delayTicks) return;
    if (sElevatorOwnerWorld != world || mgr.GetObjectById(sElevatorUid) == nullptr) {
      if (sTicks == delayTicks)
        PortLog::Write("[elevator-smoke] no elevator in this world yet\n");
      return;
    }
    PortLog::Write("[elevator-smoke] riding elevator %04X to world %08X\n",
                   sElevatorUid.value, sElevatorDestWorld);
    sDestWorld = sElevatorDestWorld;
    mgr.SendScriptMsgAlways(sElevatorUid, kInvalidUniqueId, kSM_Play);
    sPhase = kPlaying;
    sTicks = 0;
    break;
  case kPlaying:
    if (++sTicks < 2) return;
    PortLog::Write("[elevator-smoke] sending SetToZero\n");
    sRideStart = std::chrono::steady_clock::now();
    sPhase = kRiding;
    mgr.SendScriptMsgAlways(sElevatorUid, kInvalidUniqueId, kSM_SetToZero);
    break;
  case kRiding:
    if (world != sDestWorld || !playing) return;
    PortLog::Write("[elevator-smoke] passed: world %08X area %d after %.2f s\n", world,
                   mgr.World()->GetCurrentAreaId().Value(),
                   std::chrono::duration< double >(std::chrono::steady_clock::now() - sRideStart).count());
    sPhase = kDone;
    break;
  }
}

// When the save screen's confirm press is due. Zero means not scheduled. Shared
// between the game-side hook that requests the screen and the frame loop that
// injects the press, because they run in different places at different times.
unsigned sSaveConfirmAt = 0;

// Schedules the A press that confirms the save screen. Called from
// PortSmokeFrame, not from the save hook, because a menu stops the game think
// loop that the hook lives in. $1 is the frame the request happened on.
// A request made while an earlier press is still pending is queued, not
// dropped: the "Save" dialog can come up during the hold that answered "file
// corrupt", and dropping it left the save unconfirmed.
unsigned sSaveConfirmQueued = 0;
void PortSmokeSaveConfirm() {
  if (sSaveConfirmAt != 0) {
    ++sSaveConfirmQueued;
    PortLog::Write("[save-smoke] will confirm again after the current press\n");
    return;
  }
  sSaveConfirmAt = PortSmokeCurrentFrame() + 90;
  PortLog::Write("[save-smoke] will confirm the save screen shortly\n");
}

// The frame loop's counter, so a hook that is not handed a frame can still ask
// what frame it is on.
namespace {
unsigned sSmokeFrame = 0;
}
unsigned PortSmokeCurrentFrame() { return sSmokeFrame; }
void PortSmokeCurrentFrameSet(unsigned frame) { sSmokeFrame = frame; }

// MP_SMOKE_SAVE=<ticks>: open the real in-game save screen, then confirm it.
//
// Saving in Metroid Prime happens at a save station, not from the pause menu -
// CPauseScreen's sub-screens are LogBook, Options, Inventory, ToGame and ToMap,
// with no save among them. A save station's trigger is a script special
// function that calls CStateManager::EnterSaveGameScreen(), deferring
// kSMT_SaveGame, which CMFGame::Think turns into CMFGame::SaveGame() and then a
// CSaveGameScreen(kSC_InGame). Calling EnterSaveGameScreen() takes exactly that
// path, so this exercises the real save - the screen, the file write, the card -
// and only skips the walk to the trigger, which is the part that needs a player.
void PortSmokeSave(CStateManager& mgr) {
  static const unsigned afterTicks = [] {
    const char* value = port::EnvString("MP_SMOKE_SAVE");
    return value != nullptr ? static_cast<unsigned>(std::strtoul(value, nullptr, 10)) : 0;
  }();
  static unsigned sTicks = 0;
  static unsigned sStage = 0;
  if (afterTicks == 0)
    return;
  if (sStage == 0) {
    // Only the *request* needs the game in its first-person state. Once the save
    // screen is up the game is paused and no longer in that state, so requiring
    // it afterwards is what stopped the confirm from ever happening - and a save
    // screen opened and never confirmed leaves an 8192-byte file of zeroes on
    // the card, which the game then quite correctly calls corrupt.
    const bool inGame = mgr.GetGameState() == CStateManager::kGS_Running &&
                        mgr.GetCameraManager()->IsInFPCamera() &&
                        !mgr.GetCameraManager()->IsInCinematicCamera();
    if (!inGame) {
      sTicks = 0;
      return;
    }
    if (++sTicks < afterTicks)
      return;
    // A save station checks this before opening the screen, and closes without
    // one when it is 0, as it was for every card while the GCI folder had no
    // serial. Calling EnterSaveGameScreen() directly skips that check, so repeat it.
    if (gpGameState->CardSerial() == 0) {
      PortLog::Write("[save-smoke] failed: card serial is 0, so a save station would close "
                     "without a save screen\n");
      std::abort();
    }
    PortLog::Write("[save-smoke] requesting the in-game save screen\n");
    mgr.EnterSaveGameScreen();
    // The confirm is not scheduled here: a press at a fixed delay landed on the
    // card's "file corrupt" dialog instead of "Save", so the file was repaired
    // with no game in it and the save itself was never confirmed. The save
    // screen reports its dialogs to PortSmokeSaveScreenUI, which confirms them.
    sStage = 1;
  }
}

// Called by CSaveGameScreen whenever its dialog changes, so the confirm lands on
// the dialog it is meant for. Every change is logged, since the dialog sequence
// is what tells a card repair apart from a save. Under MP_SMOKE_SAVE the card has
// just been wiped by the script, and fast boot leaves the file it created but
// never wrote behind, so "file corrupt" is expected once and is answered with
// its first choice (delete the bad file); "Save" is then confirmed once.
void PortSmokeSaveScreenUI(int saveCtx, int oldUiType, int uiType, int driverState) {
  PortLog::Write("[card] save screen (%s) ui type %d -> %d, driver state %d\n",
                 saveCtx == kSC_InGame ? "in game" : "front end", oldUiType,
                 uiType, driverState);
  if (saveCtx != kSC_InGame || port::EnvString("MP_SMOKE_SAVE") == nullptr)
    return;
  static bool sCorruptAnswered = false;
  static bool sSaveConfirmed = false;
  if (uiType == CSaveGameScreen::kUIT_SaveCorrupt && !sCorruptAnswered) {
    sCorruptAnswered = true;
    PortLog::Write("[save-smoke] deleting the unwritten file fast boot left behind\n");
    PortSmokeSaveConfirm();
  } else if (uiType == CSaveGameScreen::kUIT_SaveReady && !sSaveConfirmed) {
    sSaveConfirmed = true;
    PortSmokeSaveConfirm();
  }
}

// MP_SMOKE_SCRIPT=<at>:<buttons>[:<hold>],... : press buttons at a given frame.
//
// The menus cannot be walked any other way. Every other hook either calls the
// game into a screen or presses one button for a fixed window, and reaching
// "Save Game" needs a sequence: leave the Inventory with Z, take a menu row with
// A, then confirm. The format is a comma-separated list of
// <frame>:<buttons>[:<hold frames>], where <buttons> is a sum of the names
// below, and a press lasts <hold> frames (default 8) so a menu registers it as
// a press rather than a tap it can miss.
//
// A button set that is still held from an earlier step wins over a later
// release, so overlapping steps are not a way to release a button early.
void PortSmokeScript(unsigned frame) {
  static const char* script = port::EnvString("MP_SMOKE_SCRIPT");
  if (script == nullptr)
    return;
  static unsigned sHoldUntil = 0;
  static unsigned sHeld = 0;
  static unsigned sNext = 0;
  // A small fixed table, parsed once. The list is a test script, not user input,
  // so a bound is enough and refusing to overrun it is better than growing an
  // allocation.
  static const int kMaxSteps = 32;
  static int sFrame[kMaxSteps];
  static unsigned sButtons[kMaxSteps];
  static unsigned sHold[kMaxSteps];
  static int sCount = -1;
  if (sCount < 0) {
    sCount = 0;
    char* p = const_cast< char* >(script);
    while (*p != '\0' && sCount < kMaxSteps) {
      const long at = std::strtol(p, &p, 10);
      if (*p != ':')
        break;
      ++p;
      long mask = 0;
      while (*p != '\0' && *p != ',' && *p != ':') {
        if (std::strncmp(p, "start", 5) == 0)
          mask |= PAD_BUTTON_START;
        else if (std::strncmp(p, "a", 1) == 0)
          mask |= PAD_BUTTON_A;
        else if (std::strncmp(p, "b", 1) == 0)
            mask |= PAD_BUTTON_B;
          else if (std::strncmp(p, "x", 1) == 0)
            mask |= PAD_BUTTON_X;
          else if (std::strncmp(p, "y", 1) == 0)
            mask |= PAD_BUTTON_Y;
          else if (std::strncmp(p, "up", 2) == 0)
            mask |= PAD_BUTTON_UP;
          else if (std::strncmp(p, "down", 4) == 0)
            mask |= PAD_BUTTON_DOWN;
          else if (std::strncmp(p, "left", 4) == 0)
            mask |= PAD_BUTTON_LEFT;
          else if (std::strncmp(p, "right", 5) == 0)
            mask |= PAD_BUTTON_RIGHT;
          else if (std::strncmp(p, "z", 1) == 0)
            mask |= PAD_TRIGGER_Z;
          else if (std::strncmp(p, "l", 1) == 0)
            mask |= PAD_TRIGGER_L;
          else if (std::strncmp(p, "r", 1) == 0)
            mask |= PAD_TRIGGER_R;
          else
            break;
          while (*p != '\0' && *p != ',' && *p != ':')
            ++p;
          if (*p == '+')
            ++p;
        }
        unsigned hold = 8;
        if (*p == ':') {
          ++p;
          hold = static_cast< unsigned >(std::strtoul(p, &p, 10));
        }
        sFrame[sCount] = static_cast< int >(at);
        sButtons[sCount] = static_cast< unsigned >(mask);
        sHold[sCount] = hold;
        ++sCount;
      if (*p == ',')
        ++p;
      else
        break;
    }
    // Anything past the bound is dropped rather than half-applied.
    if (*p != '\0')
      PortLog::Write("[smoke] script truncated at %d steps\n", kMaxSteps);
    PortLog::Write("[smoke] script parsed %d step(s)\n", sCount);
  }
  // Steps run in order and each fires once, at or after its frame. This has to
  // be straight-line rather than inside the parse block: with the loop guard
  // still on sNext, the second and later steps were never reached at all.
  if (sNext < static_cast<unsigned>(sCount) && static_cast< int >(frame) >= sFrame[sNext]) {
    sHeld = sButtons[sNext];
    sHoldUntil = frame + sHold[sNext];
    PortLog::Write("[smoke] frame %u: pressing 0x%04x for %u frame(s)\n", frame, sHeld, sHold[sNext]);
    ++sNext;
  }
  // The virtual status is sticky, so the release has to be written too; without
  // it a step's buttons stayed held until the next step, and two steps with the
  // same button never produced a second press.
  if (frame < sHoldUntil) {
    PADStatus status{};
    status.err = PAD_ERR_NONE;
    status.button = static_cast< u16 >(sHeld);
    // The game reads L/R as analog triggers (the pause screen pages on them),
    // so the digital bit alone does nothing there.
    if ((sHeld & PAD_TRIGGER_L) != 0)
      status.triggerLeft = 0xFF;
    if ((sHeld & PAD_TRIGGER_R) != 0)
      status.triggerRight = 0xFF;
    PADSetVirtualStatus(0, &status);
  } else if (sHeld != 0) {
    sHeld = 0;
    PADStatus status{};
    status.err = PAD_ERR_NONE;
    PADSetVirtualStatus(0, &status);
  }
}

// MP_SMOKE_CONTINUE=1: CFrontEndUI drives itself to Continue from its own
// screen state (see CFrontEndUI::Update), rather than from frame numbers that
// drift with machine speed. These are the press and screenshot primitives it
// uses; both are applied from PortSmokeFrame.
namespace {
unsigned sPressButtons = 0;
unsigned sPressUntil = 0;
bool sPressActive = false;
const int kMaxPendingShots = 16;
unsigned sPendingShots[kMaxPendingShots];
int sPendingShotCount = 0;
} // namespace

bool PortSmokeContinueEnabled() {
  static const bool enabled = port::EnvFlag("MP_SMOKE_CONTINUE");
  return enabled;
}

bool PortSmokePressPending() { return sPressActive; }

void PortSmokePress(unsigned buttons, unsigned hold) {
  sPressButtons = buttons;
  sPressUntil = PortSmokeCurrentFrame() + hold;
  sPressActive = true;
}

void PortSmokeShotAfter(unsigned frames, const char* why) {
  const unsigned at = PortSmokeCurrentFrame() + frames;
  PortLog::Write("[continue] screenshot at frame %u: %s\n", at, why);
  if (sPendingShotCount < kMaxPendingShots)
    sPendingShots[sPendingShotCount++] = at;
}

static void ApplyContinuePressAndShots(unsigned frame) {
  if (sPressActive) {
    PADStatus status{};
    status.err = PAD_ERR_NONE;
    if (frame < sPressUntil) {
      status.button = static_cast< u16 >(sPressButtons);
    } else {
      sPressActive = false;
    }
    PADSetVirtualStatus(0, &status);
  }
  for (int i = 0; i < sPendingShotCount;) {
    if (frame >= sPendingShots[i]) {
      aurora::request_screenshot();
      PortLog::Write("[smoke] screenshot requested at frame %u\n", frame);
      sPendingShots[i] = sPendingShots[--sPendingShotCount];
    } else {
      ++i;
    }
  }
}

// MP_SMOKE_STICK=1: hold the right stick and report the aim yaw change, to
// verify twin-stick aiming (run with MP_TWIN_STICK=1).
void PortSmokeStick(CStateManager& mgr) {
  static const bool enabled = port::EnvFlag("MP_SMOKE_STICK");
  if (!enabled) return;
  static unsigned sTicks = 0;
  static float sStartYaw = 0.f;
  static bool sDone = false;
  if (sDone || mgr.GetGameState() != CStateManager::kGS_Running ||
      !mgr.GetCameraManager()->IsInFPCamera() || mgr.GetCameraManager()->IsInCinematicCamera()) {
    return;
  }
  ++sTicks;
  if (sTicks == 120) {
    sStartYaw = PortDebug::AimYaw();
  }
  if (sTicks >= 120 && sTicks < 240) {
    PADStatus status{};
    status.err = PAD_ERR_NONE;
    status.substickX = 127;
    PADSetVirtualStatus(0, &status);
    return;
  }
  PADClearVirtualStatus(0);
  sDone = true;
  PortLog::Write("[stick-smoke] passed: yaw %.4f -> %.4f\n", sStartYaw, PortDebug::AimYaw());
}

void PortSmokeVisor(CStateManager& mgr) {  static const bool enabled = port::EnvFlag("MP_SMOKE_VISOR");
  if (!enabled) return;
  static unsigned sTicks = 0;
  static bool sRequested = false;
  static bool sPassed = false;
  if (sPassed) return;
  if (mgr.GetGameState() != CStateManager::kGS_Running ||
      !mgr.GetCameraManager()->IsInFPCamera() || mgr.GetCameraManager()->IsInCinematicCamera()) {
    return;
  }
  CPlayerState* ps = mgr.PlayerState();
  if (ps == nullptr) return;
  // MP_SMOKE_VISOR=xray selects the X-ray visor; any other value, thermal.
  static const bool xray = std::strcmp(port::EnvString("MP_SMOKE_VISOR"), "xray") == 0;
  const char* name = xray ? "x-ray" : "thermal";
  if (!sRequested) {
    if (++sTicks < 120) return;
    const CPlayerState::EItemType item =
        xray ? CPlayerState::kIT_XRayVisor : CPlayerState::kIT_ThermalVisor;
    ps->SetPowerUp(item, 1);
    ps->SetPickup(item, 1);
    PortLog::Write("[visor-smoke] switching to the %s visor\n", name);
    ps->StartTransitionToVisor(xray ? CPlayerState::kPV_XRay : CPlayerState::kPV_Thermal);
    sRequested = true;
    return;
  }
  if (++sTicks > 900) {
    sPassed = true;
    PortLog::Write("[visor-smoke] passed: %s visor stable\n", name);
  }
}

bool PortSmokeMouseEnabled() {
  static const bool enabled = port::EnvFlag("MP_SMOKE_MOUSE");
  return enabled;
}

// Whether scripted input is in use, which also implies the window has focus.
//
// CDolphinController::ReadDevices zeroes the whole pad status when
// SDL_GetKeyboardFocus() is null, and keeps the error code, so the controller
// still reports *present* while every button is discarded. A scripted press is
// then silently dropped and the screen never advances, which is indistinguishable
// from the button doing nothing. The mouse hook already claims focus for itself
// (PortSmokeMouseEnabled is OR'd into inputFocused); the script and the console
// (MP_CONSOLE press/stick) need the same,
// since a press that is thrown away before it is read cannot test anything.
bool PortSmokeScriptedInput() {
  static const bool enabled = [] {
    // MP_CONSOLE carries a port, so it is on by the console's own rule (PortConsole::Start):
    // set, not empty and not 0.
    const char* const console = port::EnvString("MP_CONSOLE");
    return port::EnvString("MP_SMOKE_SCRIPT") != nullptr ||
           port::EnvString("MP_SMOKE_FRONTEND") != nullptr || port::EnvFlag("MP_SMOKE_DASH") ||
           (console != nullptr && console[0] != '\0' && std::strcmp(console, "0") != 0) ||
           PortSmokeContinueEnabled();
  }();
  return enabled;
}

unsigned PortSmokeMouseButtons(unsigned realButtons) {
  if (!PortSmokeMouseEnabled()) return realButtons;
  if (sMouseComplete) return 0;
  if ((sMouseTicks >= 10 && sMouseTicks < 15) ||
      (sMouseTicks >= 25 && sMouseTicks < 115) ||
      (sMouseTicks >= 335 && sMouseTicks < 380) ||
      (sMouseTicks >= 488 && sMouseTicks < 493)) return SDL_BUTTON_LMASK;
  if ((sMouseTicks >= 135 && sMouseTicks < 140) ||
      (sMouseTicks >= 155 && sMouseTicks < 160) ||
      (sMouseTicks >= 175 && sMouseTicks < 180)) return SDL_BUTTON_MMASK;
  if (sMouseTicks >= 210 && sMouseTicks < 270) return SDL_BUTTON_RMASK;
  return 0;
}

void PortSmokeMouseBeforeUpdate(CStateManager& mgr) {
  if (!PortSmokeMouseEnabled() || sMouseComplete) return;
  // Real scripted cameras can interrupt the sequence (e.g. frigate tutorials).
  // Do not spend the planned morph/unmorph button presses while input is disabled.
  if (mgr.GetCameraManager()->IsInCinematicCamera() ||
      mgr.GetPlayer()->GetCameraState() == CPlayer::kCS_Spawned ||
      mgr.GetPlayer()->GetDisableInput() || mgr.GetGameState() != CStateManager::kGS_Running) {
    PADClearVirtualStatus(0);
    PortDebug::SetMouseCaptured(false);
    return;
  }
  if (sMouseTicks == 0 && !mgr.GetPlayer()->MouseControlsAllowed(mgr)) return;
  ++sMouseTicks;
  PortDebug::SetMouseCaptured(true);
  float dx = 0.f, dy = 0.f;
  if (sMouseTicks == 1) {
    PortDebug::ResetMouseAim();
    sInitialDirection = mgr.GetCameraManager()->GetFirstPersonCamera()->GetGunFollowTransform().GetForward();
  } else if (sMouseTicks == 2) {
    dx = 30.f; dy = -100.f;
  } else if (sMouseTicks == 3) {
    dx = -60.f; dy = 200.f;
  } else if (sMouseTicks == 4) {
    dy = -2000.f;
  } else if (sMouseTicks == 5) {
    dy = 4000.f;
  } else if (sMouseTicks == 7) {
    PortDebug::SynchronizeMouseAim(sInitialDirection.GetX(), sInitialDirection.GetY(), sInitialDirection.GetZ());
  }
  if (sMouseTicks == 80) PortDebug::SetFrameLimitEnabled(false);
  if (sMouseTicks == 200) PortDebug::SetFrameLimitEnabled(true);
  // Exercise moving aim while uncapped, then deliberately move the mouse while locked.
  if (sMouseTicks >= 85 && sMouseTicks < 100) { dx = 20.f; dy = -3.f; }
  if (sMouseTicks >= 100 && sMouseTicks < 115) { dx = -20.f; dy = 3.f; }
  if (sMouseTicks >= 220 && sMouseTicks < 260) { dx = 60.f; dy = 30.f; }
  if (sMouseTicks == 295) dy = -40.f;
  if (sMouseTicks >= 430 && sMouseTicks < 495) { dx = 100.f; dy = -100.f; }
  if (sMouseTicks == 365 || sMouseTicks == 370) PortDebug::Toggle();
  PADStatus jump{};
  jump.err = PAD_ERR_NONE;
  if (sMouseTicks >= 300 && sMouseTicks < 310) jump.button = PAD_BUTTON_B;
  if ((sMouseTicks >= 420 && sMouseTicks < 425) ||
      (sMouseTicks >= 500 && sMouseTicks < 505)) jump.button = PAD_BUTTON_X;
  if ((sMouseTicks >= 20 && sMouseTicks < 25) ||
      (sMouseTicks >= 390 && sMouseTicks < 395)) jump.stickX = 80;
  if ((sMouseTicks >= 25 && sMouseTicks < 30) ||
      (sMouseTicks >= 395 && sMouseTicks < 400)) jump.stickX = -80;
  PADSetVirtualStatus(0, &jump);
  PortDebug::AddMouseDelta(dx, dy);
  PortDebug::BeginFrameMouse();
}

void PortSmokeMouseAfterUpdate(CStateManager& mgr) {
  if (!PortSmokeMouseEnabled() || sMouseComplete || sMouseTicks == 0) return;
  const CPlayer& player = *mgr.GetPlayer();
  sPowerProjectileSeen |= mgr.GetWeaponIdCount(player.GetUniqueId(), kWT_Power) > 0;
  sMissileProjectileSeen |= mgr.GetWeaponIdCount(player.GetUniqueId(), kWT_Missile) > 0;
  sBombSeen |= mgr.GetWeaponIdCount(player.GetUniqueId(), kWT_Bomb) > 0;
  if (player.GetMorphballTransitionState() == CPlayer::kMS_Morphed) sMouseMorphed = true;
  if (!player.MouseControlsAllowed(mgr)) {
    MouseCheck(!PortDebug::AimInitialized(), "inactive camera kept mouse ownership");
    MouseCheck(sMouseTicks < 620, "did not return from morph ball");
    sWasLocked = false;
    return;
  }
  if (sMouseMorphed) sMouseResumed = true;
  const float sidewaysSpeed = CVector3f::Dot(player.GetVelocityWR(), player.GetTransform().GetColumn(kDX));
  if (player.GetOrbitState() == CPlayer::kOS_NoOrbit && std::fabs(sidewaysSpeed) > 0.03f) {
    if (sMouseTicks >= 21 && sMouseTicks <= 31) sStrafedBeforeUI = true;
    if (sMouseTicks >= 391 && sMouseTicks <= 401) sStrafedAfterUI = true;
  }
  MouseCheck(!PortDebug::AimCrosshair() || !player.MouseLookIsFree(mgr) || player.IsCrosshairsOpen(),
             "mouse-aim crosshair was not requested");
  const CVector3f forward = mgr.GetCameraManager()->GetFirstPersonCamera()->GetGunFollowTransform().GetForward();
  const float yaw = PortDebug::AimYaw(), pitch = PortDebug::AimPitch();
  const CVector3f aim(-std::sin(yaw) * std::cos(pitch), std::cos(yaw) * std::cos(pitch), std::sin(pitch));
  MouseCheck(CVector3f::Dot(forward, aim) > 0.9999f, "camera did not reach mouse/locked aim in this tick");
  MouseCheck(std::fabs(pitch) <= PortMouse::kMaxPitch + 0.0001f, "pitch limit");
  const bool locked = !player.MouseLookIsFree(mgr);
  if (locked) {
    ++sMouseLocks;
    sLastLockedDirection = forward;
  } else if (sWasLocked) {
    MouseCheck(CVector3f::Dot(forward, sLastLockedDirection) > 0.9999f, "lock release snapped to stale aim");
  }
  sWasLocked = locked;
  if (sMouseTicks >= 300 && sMouseTicks < 330 && player.GetPlayerMovementState() != NPlayer::kMS_OnGround)
    sMouseJumped = true;
  if (sMouseTicks >= 620) {
    MouseCheck(sMouseShots > 0 && sMouseChargedShots > 0 && sMouseMissiles > 0, "fire/charge/missile input was not delivered");
    MouseCheck(sPowerProjectileSeen && sMissileProjectileSeen, "weapon inputs did not create live projectiles");
    MouseCheck(sMouseLocks > 0 && sMouseJumped && sGunViewChecks > 50, "lock/jump/viewmodel coverage incomplete");
    MouseCheck(sMouseMorphed && sMouseResumed, "morph-ball handoff coverage incomplete");
    MouseCheck(sBombSeen, "a left click in morph ball did not lay a bomb");
    MouseCheck(sStrafedBeforeUI && sStrafedAfterUI, "free strafe failed before/after F1");
    PADClearVirtualStatus(0);
    sMouseComplete = true;
    PortLog::Write("[mouse-smoke] passed: shots=%u charged=%u missiles=%u lockedTicks=%u gunViews=%u jump=1 morph=1 bomb=1 strafe-before/after-F1=1\n",
                   sMouseShots, sMouseChargedShots, sMouseMissiles, sMouseLocks, sGunViewChecks);
  }
}

void PortSmokeMouseShot(bool charged, bool secondary) {
  if (!PortSmokeMouseEnabled() || sMouseComplete) return;
  MouseCheck(sMouseTicks < 365 || sMouseTicks > 385, "UI/capture transition fired a held charge");
  if (secondary) ++sMouseMissiles;
  else if (charged) ++sMouseChargedShots;
  else ++sMouseShots;
}

void PortSmokeMouseGunView(const CStateManager& mgr, const CPlayerGun& gun, const CTransform4f& worldView) {
  if (!PortSmokeMouseEnabled() || !mgr.GetPlayer()->MouseControlsAllowed(mgr) ||
      mgr.GetPlayer()->GetGunHolsterState() != CPlayer::kGH_Drawn) return;
  const CVector3f gunForward = gun.GetTransform().GetForward();
  MouseCheck(CVector3f::Dot(CGraphics::GetViewMatrix().GetForward(), gunForward) > 0.9999f,
             "held cannon and its render camera use different orientations");
  if (mgr.GetPlayer()->MouseLookIsFree(mgr))
    MouseCheck(CVector3f::Dot(worldView.GetForward(), gunForward) > 0.9999f, "visible aim lags simulation aim");
  ++sGunViewChecks;
}

bool PortSmokeFrame(unsigned frame) {
  if (PortSmokeMouseEnabled() && frame == 1) PortDebug::SetMouseAim(true);
  PortSmokeCurrentFrameSet(frame);
  if (PortConsoleFrame(frame)) return true;
  // Before the other input hooks, so a script step and a hook press cannot
  // fight over the same virtual pad status in one frame.
  PortSmokeScript(frame);
  ApplyContinuePressAndShots(frame);
  // The save screen's confirm, held long enough for the screen to register a
  // press rather than a tap it can miss.
  if (sSaveConfirmAt != 0 && frame >= sSaveConfirmAt) {
    if (frame == sSaveConfirmAt)
      PortLog::Write("[save-smoke] confirming the save screen\n");
    if (frame < sSaveConfirmAt + 12) {
      PADStatus status{};
      status.err = PAD_ERR_NONE;
      status.button = PAD_BUTTON_A;
      PADSetVirtualStatus(0, &status);
    } else {
      // The virtual status is sticky; release A so the next press is an edge.
      PADStatus status{};
      status.err = PAD_ERR_NONE;
      PADSetVirtualStatus(0, &status);
      sSaveConfirmAt = 0;
      if (sSaveConfirmQueued != 0) {
        --sSaveConfirmQueued;
        sSaveConfirmAt = frame + 90;
      }
    }
  }
  static const unsigned limit = [] {
    const char* value = port::EnvString("MP_SMOKE_FRAMES");
    return value != nullptr ? static_cast<unsigned>(std::strtoul(value, nullptr, 10)) : 0;
  }();
  static const char* resizeSpec = port::EnvString("MP_SMOKE_RESIZE");
  if (resizeSpec != nullptr && frame == 300) {
    int rw = 0;
    int rh = 0;
    if (std::sscanf(resizeSpec, "%dx%d", &rw, &rh) == 2 && rw > 0 && rh > 0) {
      int count = 0;
      SDL_Window** windows = SDL_GetWindows(&count);
      if (windows != nullptr && count > 0) {
        SDL_SetWindowSize(windows[0], rw, rh);
        PortLog::Write("[smoke] resized window to %dx%d\n", rw, rh);
      }
      SDL_free(windows);
    }
  }
  static const char* shotList = port::EnvString("MP_SMOKE_SHOT");
  if (shotList != nullptr) {
    for (const char* p = shotList; *p != '\0';) {
      char* end = nullptr;
      const unsigned shotFrame = static_cast<unsigned>(std::strtoul(p, &end, 10));
      if (end == p) {
        break;
      }
      if (frame == shotFrame) {
        aurora::request_screenshot();
        PortLog::Write("[smoke] screenshot requested at frame %u\n", frame);
      }
      p = *end == ',' ? end + 1 : end;
    }
  }
  // MP_SMOKE_FRONTEND=<frame>: tap Start every 300 frames from that frame so
  // the front-end screens (and their button prompts) are reached without a
  // player. Start alone leaves dialogs such as the save check on screen.
  static const unsigned frontEndFrame = [] {
    const char* value = port::EnvString("MP_SMOKE_FRONTEND");
    return value != nullptr ? static_cast<unsigned>(std::strtoul(value, nullptr, 10)) : 0;
  }();
  if (frontEndFrame != 0 && frame >= frontEndFrame) {
    PADStatus status{};
    status.err = PAD_ERR_NONE;
    if ((frame - frontEndFrame) % 300 < 8) {
      status.button = PAD_BUTTON_START;
    }
    PADSetVirtualStatus(0, &status);
  }
  // MP_SMOKE_BIND_A=<scancode>: rebind the A action once, so the prompt's
  // binding-aware icon can be checked without going through the Controls tab.
  static const bool bindApplied = [] {
    const char* value = port::EnvString("MP_SMOKE_BIND_A");
    if (value == nullptr || value[0] == '\0') {
      return false;
    }
    PADKeyButtonBinding binding{};
    binding.scancode = static_cast< s32 >(std::strtol(value, nullptr, 10));
    binding.padButton = PAD_BUTTON_A;
    const bool ok = PADSetKeyButtonBinding(PAD_CHAN0, binding) != FALSE;
    PortLog::Write("[smoke] rebind A to scancode %d: %s\n", binding.scancode, ok ? "ok" : "failed");
    return ok;
  }();
  (void)bindApplied;
  if (limit == 0) return false;
  static SDL_Window* window = nullptr;
  if (window == nullptr) {
    int count = 0;
    SDL_Window** windows = SDL_GetWindows(&count);
    if (windows != nullptr && count > 0) window = windows[0];
    SDL_free(windows);
  }
  if (port::EnvFlag("MP_SMOKE_LIFECYCLE") && limit >= 240) {
    if (frame == limit / 4) {
      if (window != nullptr) SDL_HideWindow(window);
      PortDebug::SetAiAudioEnabled(false);
      PortDebug::SetMusyxAudioEnabled(false);
      PortLog::Write("[smoke] hide and mute\n");
    } else if (frame == limit / 4 + 30) {
      if (window != nullptr) SDL_ShowWindow(window);
      PortDebug::SetAiAudioEnabled(true);
      PortDebug::SetMusyxAudioEnabled(true);
      PortDebug::SetFrameLimitEnabled(false);
      PortLog::Write("[smoke] restore, unmute and uncap\n");
    } else if (frame == limit / 2) {
      PortDebug::SetFrameLimitEnabled(true);
      PortDebug::RequestReset();
      PortDebug::ResetMouseAim();
      PortLog::Write("[smoke] reset to menu\n");
    }
  }
  if (frame < limit || (PortSmokeMouseEnabled() && !sMouseComplete) ||
      (AreaReloadEnabled() && sAreaReloads < 3)) return false;
  PortLog::Write("[smoke] clean exit requested\n");
  return true;
}

// MP_SMOKE_WALK=<ticks>: hold the stick full forward and report the ground
// speed, so a tick-rate change can be checked to stay real-time.
void PortSmokeWalk(CStateManager& mgr) {
  static const unsigned walkTicks = [] {
    const char* value = port::EnvString("MP_SMOKE_WALK");
    return value != nullptr ? static_cast< unsigned >(std::strtoul(value, nullptr, 10)) : 0u;
  }();
  if (walkTicks == 0 || WorldSmokePending()) return;
  static unsigned sTicks = 0;
  static bool sStarted = false;
  static CVector3f sStart;
  static bool sDone = false;
  static float sMaxSpeed = 0.f;
  if (sDone || mgr.GetGameState() != CStateManager::kGS_Running ||
      !mgr.GetCameraManager()->IsInFPCamera() || mgr.GetCameraManager()->IsInCinematicCamera()) {
    return;
  }
  const CPlayer* player = mgr.GetPlayer();
  if (player == nullptr) return;
  if (!sStarted) {
    sStarted = true;
    sStart = player->GetTranslation();
    PortLog::Write("[walk-smoke] begin\n");
  }
  {
    const CVector3f vel = player->GetVelocityWR();
    const CVector3f flat(vel.GetX(), vel.GetY(), 0.f);
    sMaxSpeed = rstl::max_val(sMaxSpeed, flat.Magnitude());
  }
  PADStatus status{};
  status.err = PAD_ERR_NONE;
  status.stickY = 127;
  PADSetVirtualStatus(0, &status);
  if (++sTicks < walkTicks) return;
  PADClearVirtualStatus(0);
  sDone = true;
  const float dist = (player->GetTranslation() - sStart).Magnitude();
  const double seconds = static_cast< double >(sTicks) * PortDebug::TickPeriod();
  PortLog::Write("[walk-smoke] passed: ticks=%u seconds=%.3f dist=%.3f maxFlatSpeed=%.4f speed=%.4f/s\n",
                 sTicks, seconds, dist, sMaxSpeed, seconds > 0.0 ? dist / seconds : 0.0);
}

// MP_SMOKE_DASH=1: turn until the game offers an orbit target, lock on with L,
// then hold the stick right and press B, and report whether a sideways dash
// started. Run it with MP_MOUSE_AIM=1 or MP_TWIN_STICK=1 to cover those modes.
void PortSmokeDash(CStateManager& mgr) {
  static const bool enabled = port::EnvFlag("MP_SMOKE_DASH");
  if (!enabled || WorldSmokePending()) return;
  static unsigned sTicks = 0, sLockedTicks = 0, sDashTicks = 0, sTurnTicks = 0;
  static bool sFound = false, sDone = false;
  static float sYaw = 0.f;
  if (sDone || mgr.GetGameState() != CStateManager::kGS_Running ||
      !mgr.GetCameraManager()->IsInFPCamera() || mgr.GetCameraManager()->IsInCinematicCamera()) {
    return;
  }
  CPlayer* player = mgr.Player();
  if (player == nullptr || player->GetDisableInput()) return;
  if (PortDebug::MouseAim()) PortDebug::SetMouseCaptured(true);
  ++sTicks;
  // MP_SMOKE_DASH=scan locks onto scan points instead, for rooms without enemies.
  if (sTicks == 30 && std::strcmp(port::EnvString("MP_SMOKE_DASH"), "scan") == 0) {
    mgr.PlayerState()->StartTransitionToVisor(CPlayerState::kPV_Scan);
  }
  if (sTicks < 60) return;
  PADStatus status{};
  status.err = PAD_ERR_NONE;
  if (!sFound) {
    if (sTicks == 60) {
      const CVector3f fwd = player->GetTransform().GetForward();
      sYaw = std::atan2(-fwd.GetX(), fwd.GetY());
    }
    if (player->GetOrbitNextTargetId() != kInvalidUniqueId) {
      sFound = true;
      PortLog::Write("[dash-smoke] target %04x after %u turn ticks\n",
                     player->GetOrbitNextTargetId().Value(), sTurnTicks);
    } else if (++sTurnTicks > 240) {
      PADClearVirtualStatus(0);
      sDone = true;
      PortLog::Write("[dash-smoke] failed: no orbit target in a full turn\n");
      return;
    } else {
      // A slow turn, so the camera keeps up and the orbit zone sees each yaw.
      sYaw += 1.5f * M_PIF / 180.f;
      if (PortDebug::DirectAim()) {
        PortDebug::SynchronizeMouseAim(-std::sin(sYaw), std::cos(sYaw), 0.f);
      } else {
        player->SetTransform(CQuaternion::ZRotation(CRelAngle(sYaw))
                                 .BuildTransform4f(player->GetTranslation()));
      }
      PADSetVirtualStatus(0, &status);
      return;
    }
  }
  status.button = PAD_TRIGGER_L;
  status.triggerLeft = 0xFF;
  const bool locked = player->GetOrbitState() == CPlayer::kOS_OrbitObject;
  if (locked) ++sLockedTicks;
  if (sLockedTicks == 0 && sTicks % 10 == 0) {
    PortLog::Write("[dash-smoke] tick %u orbit %d next %04x\n", sTicks,
                   static_cast< int >(player->GetOrbitState()), player->GetOrbitNextTargetId().Value());
  }
  if (sLockedTicks >= 20) {
    status.stickX = 127;
    if (sLockedTicks == 22) status.button |= PAD_BUTTON_B;
  }
  PADSetVirtualStatus(0, &status);
  if (player->IsSidewaysDashing()) ++sDashTicks;
  if (sLockedTicks >= 20 && sLockedTicks < 40) {
    const CVector3f vel = player->GetVelocityWR();
    PortLog::Write("[dash-smoke] tick %u orbit %d dashing %d vel %.2f %.2f %.2f\n",
                   sLockedTicks, static_cast< int >(player->GetOrbitState()),
                   player->IsSidewaysDashing() ? 1 : 0, vel.GetX(), vel.GetY(), vel.GetZ());
  }
  if (sLockedTicks >= 60 || sTicks >= 600) {
    PADClearVirtualStatus(0);
    sDone = true;
    PortLog::Write("[dash-smoke] %s: lockedTicks=%u dashTicks=%u orbit=%d\n",
                   sDashTicks > 0 ? "passed" : "failed", sLockedTicks, sDashTicks,
                   static_cast< int >(player->GetOrbitState()));
  }
}

// MP_SMOKE_WATER=<cycles>: put the player into the loaded area's first water
// volume (just under the surface), walk forward there for two seconds, pull
// them back out, and repeat, so enter/exit effects and damage can be checked.
void PortSmokeWater(CStateManager& mgr) {
  static const unsigned cycles = [] {
    const char* value = port::EnvString("MP_SMOKE_WATER");
    return value != nullptr ? static_cast< unsigned >(std::strtoul(value, nullptr, 10)) : 0u;
  }();
  if (cycles == 0 || WorldSmokePending()) return;
  static unsigned sTicks = 0, sCycle = 0;
  static bool sDone = false, sHaveWater = false;
  static CVector3f sDry, sWet;
  if (sDone || mgr.GetGameState() != CStateManager::kGS_Running ||
      !mgr.GetCameraManager()->IsInFPCamera() || mgr.GetCameraManager()->IsInCinematicCamera()) {
    return;
  }
  CPlayer* player = mgr.Player();
  if (player == nullptr) return;
  if (!sHaveWater) {
    if (++sTicks < 60) return;
    const CObjectList& objList = mgr.GetObjectListById(kOL_All);
    for (int idx = objList.GetFirstObjectIndex(); idx != -1; idx = objList.GetNextObjectIndex(idx)) {
      CScriptWater* water = TCastToPtr< CScriptWater >(const_cast< CEntity* >(objList[idx]));
      if (water == nullptr || !water->GetActive()) continue;
      const CAABox box = water->GetTriggerBoundsWR();
      const CVector3f center = box.GetCenterPoint();
      sWet = CVector3f(center.GetX(), center.GetY(), box.GetMaxPoint().GetZ() - 1.5f);
      sHaveWater = true;
      PortLog::Write("[water-smoke] water %04x type %d surface %.2f\n", water->GetUniqueId().Value(),
                     static_cast< int >(water->GetFluidPlane().GetFluidType()), box.GetMaxPoint().GetZ());
      break;
    }
    if (!sHaveWater) {
      sDone = true;
      PortLog::Write("[water-smoke] failed: no active water in the area\n");
      return;
    }
    sDry = player->GetTranslation();
    sTicks = 0;
  }
  PADStatus status{};
  status.err = PAD_ERR_NONE;
  const unsigned phase = sTicks++ % 240;
  if (phase == 0 || phase == 120) {
    const bool in = phase == 0;
    player->Teleport(CTransform4f::Translate(in ? sWet : sDry), mgr, true);
    PortLog::Write("[water-smoke] cycle %u %s, health %.1f\n", sCycle, in ? "in" : "out",
                   mgr.GetPlayerState()->GetHealthInfo().GetHP());
    if (!in && ++sCycle >= cycles) {
      PADClearVirtualStatus(0);
      sDone = true;
      PortLog::Write("[water-smoke] passed: %u cycles\n", sCycle);
      return;
    }
  }
  if (phase < 120) status.stickY = 100;
  PADSetVirtualStatus(0, &status);
}
