#ifndef _CGAMESTATE
#define _CGAMESTATE

#include "types.h"
#ifdef TARGET_PC
#include "port_beam_doors.h"
#include "port_map_marks.h"
#endif

#include "MetroidPrime/Player/CGameOptions.hpp"
#include "MetroidPrime/Player/CHintOptions.hpp"
#include "MetroidPrime/Player/CPlayerState.hpp"
#include "MetroidPrime/Player/CSystemState.hpp"
#include "MetroidPrime/Player/CWorldState.hpp"
#include "MetroidPrime/Player/CWorldTransManager.hpp"
#include "MetroidPrime/TGameTypes.hpp"

#include "rstl/rc_ptr.hpp"
#include "rstl/reserved_vector.hpp"
#include "rstl/vector.hpp"

class CGameState {
public:
  CGameState();
  CGameState(CInputStream& in, int saveIdx);

  void ReadSystemOptions(CInputStream& in);
  void PutTo(COutputStream& out);
  void WriteSystemOptions(COutputStream& out);

  void SetCurrentWorldId(CAssetId);
  void InitializeMemoryStates();
  void SetDeferPowerupInit(bool);
  void SetTotalPlayTime(double);

  rstl::ncrc_ptr< CPlayerState >& PlayerState();
  rstl::rc_ptr< CPlayerState > GetPlayerState() const;
  CAssetId CurrentWorldAssetId() const;
  void WriteBackupBuf();

  CWorldState& StateForWorld(CAssetId mlvlId);
  CWorldState& CurrentWorldState();
  const CWorldState& GetCurrentWorldState() const;

  void ImportPersistentOptions(const CSystemState&);
  void ExportPersistentOptions(CSystemState&);

  CSystemState& SystemState() { return xa8_systemState; }
  CGameOptions& GameOptions() { return x17c_gameOptions; }
  CHintOptions& HintOptions() { return x1f8_hintOptions; }
  uint& SaveIdx() { return x20c_saveIdx; }
  u64& CardSerial() { return x210_cardSerial; }
  rstl::vector< uchar >& BackupBuf() { return x218_backupBuf; }
  u32 GetFileIdx() const { return x20c_saveIdx; }
  void SetFileIdx(u32 idx) { x20c_saveIdx = idx; }
  void SetCardSerial(u64 serial) { x210_cardSerial = serial; }
  u64 GetCardSerial() const { return x210_cardSerial; }
  bool GetHardMode() const { return x228_24_hardMode; }
  void SetHardMode(bool v);
  bool GetInitPowerupsAtFirstSpawn() const { return x228_25_initPowerupsAtFirstSpawn; }
  double GetTotalPlayTime() const { return xa0_playTime; }
  float GetHardModeDamageMultiplier() const;
  float GetHardModeWeaponMultiplier() const;
  rstl::ncrc_ptr< CWorldTransManager >& WorldTransitionManager();

  struct GameFileStateInfo {
    double x0_playTime;
    CAssetId x8_mlvlId;
    float xc_health;
    uint x10_energyTanks;
    uint x14_timestamp;
    uint x18_itemPercent;
    float x1c_scanPercent;
    bool x20_hardMode;
  };
  static GameFileStateInfo LoadGameFileState(const void* data);

#ifdef TARGET_PC
  // Archipelago: how many of the session's received items this state already
  // holds, so loading a save (or quitting without one) gives back exactly the
  // items the save is missing. Saved as a trailer after the retail data, which
  // leaves ~100 of the buffer's 940 bytes unused; retail readers ignore it.
  struct ApProgress {
    // False for a save written before the trailer existed: the client adopts
    // it as is, since there is no telling which items it has.
    bool recorded;
    // Hash of the seed and slot that granted the items, 0 when none has yet.
    uint identity;
    uint appliedIndex;
    // The built-in locations this game has collected, one bit per index in the
    // location table. Kept with the save so checks made while disconnected
    // reach the server on the next connection.
    uint checked[4];
    // The blast shields this game has broken, one bit per shielded doorway of
    // the seed (PortApWorld::DoorChange::shieldBit).
    uint shields[4];
    // Not saved: the client has lined its session up with this state.
    bool reconciled;
  };
  ApProgress& PortApProgress() { return xpc_apProgress; }
  // The map pickups this save has seen (PortMapPickups, "Seen" mode), one bit
  // per dot index. Saved as the "PKSN" trailer after the Archipelago one.
  static const int kPortSeenWords = 4;
  uint* PortSeenPickups() { return xpc_seenPickups; }
  // The rooms the player marked on the map (PortMapMarks), the "RMMK" trailer
  // after the "PKSN" one.
  PortMapMarks::List& PortMapMarkList() { return xpc_mapMarks; }
  // The beam doorways this save has had opened (PortBeamDoors), one bit per
  // PortApWorld::BeamDoorwayBit. Not in the slot: the card keeps them in a side
  // file (port_beam_doors.h), so a game state streamed in starts with none.
  uint* PortBeamDoorBits() { return xpc_beamDoors; }
  // The doorways as they were when the backup buffer was written: the slot
  // can't carry them, so CMain::RefreshGameState (a continue after death) puts
  // these back over the state it rebuilds from the buffer.
  uint* PortBackupBeamDoorBits() { return xpc_backupBeamDoors; }
  // After the live doorways were put back over a state just streamed in (whose
  // backup buffer that state wrote, with none): the backup takes them too.
  void PortBackUpBeamDoors() {
    for (int i = 0; i < PortBeamDoors::kWords; ++i)
      xpc_backupBeamDoors[i] = xpc_beamDoors[i];
  }
#endif

private:
  void InitializeMemoryWorlds();

  rstl::reserved_vector< uchar, 128 > x0_;
  CAssetId x84_mlvlId;
  rstl::vector< CWorldState > x88_worldStates;
  rstl::ncrc_ptr< CPlayerState > x98_playerState;
  rstl::ncrc_ptr< CWorldTransManager > x9c_transManager;
  double xa0_playTime;
  CSystemState xa8_systemState;
  CGameOptions x17c_gameOptions;
  CHintOptions x1f8_hintOptions;
  uint x20c_saveIdx;
  u64 x210_cardSerial;
  rstl::vector< uchar > x218_backupBuf;
  bool x228_24_hardMode : 1;
  bool x228_25_initPowerupsAtFirstSpawn : 1;
#ifdef TARGET_PC
  ApProgress xpc_apProgress;
  uint xpc_seenPickups[kPortSeenWords];
  PortMapMarks::List xpc_mapMarks;
  uint xpc_beamDoors[PortBeamDoors::kWords];
  uint xpc_backupBeamDoors[PortBeamDoors::kWords];
#endif
};
CHECK_SIZEOF(CGameState, 0x230)

extern CGameState* gpGameState;

#endif // _CGAMESTATE
