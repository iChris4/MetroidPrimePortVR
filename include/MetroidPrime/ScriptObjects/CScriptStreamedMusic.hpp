#ifndef _CSCRIPTSTREAMEDMUSIC
#define _CSCRIPTSTREAMEDMUSIC

#include "MetroidPrime/CEntity.hpp"

#include "Kyoto/Audio/CStreamAudioManager.hpp"

class CScriptStreamedMusic : public CEntity {
  friend struct PortEntitySchema; // save-state field schema (platform/include/port_entity_schema.h)
public:
  ~CScriptStreamedMusic() {}
  DECLARE_TYPES_MATCH_OR_ACCEPT;
  void AcceptScriptMsg(EScriptObjectMessage msg, TUniqueId objId, CStateManager& stateMgr) override;

  CScriptStreamedMusic(TUniqueId id, const CEntityInfo& info, const rstl::string& name, bool active,
                       const rstl::string& fileName, bool noStopOnDeactivate, float fadeIn,
                       float fadeOut, uint volume, bool loop, bool music);

  void Stop(CStateManager& mgr);
  void Play(CStateManager& mgr);
  static bool IsAudioTrackNameSoftware(const rstl::string& fileName);
#ifdef TARGET_PC
  // Port: a room's background track, as opposed to a jingle or the "SW"
  // placeholder that only fades the software channel.
  bool IsLoopingMusic() const {
    return x46_loop && x47_music && x34_fileName.size() > 2 && x45_fileIsDsp;
  }
  // Port: a looping software track this object started that is still playing,
  // and its file (after the tweak override Play applied).
  bool PortPlayingLoop() const {
    return x46_loop && x45_fileIsDsp &&
           CStreamAudioManager::PortIsPlaying(CStreamAudioManager::kSC_Default, x34_fileName);
  }
  const rstl::string& PortFileName() const { return x34_fileName; }
#endif

private:
  rstl::string x34_fileName;
  bool x44_noStopOnDeactivate;
  bool x45_fileIsDsp; // As opposed to .adp for DTK streaming
  bool x46_loop;
  bool x47_music;
  float x48_fadeIn;
  float x4c_fadeOut;
  uint x50_volume;

  static CStreamAudioManager::ESoftwareChannel IsOneShot(bool);
  void StopStream(CStateManager& mgr);
  void StartStream(CStateManager& mgr);
  void TweakOverride(CStateManager& mgr);

#ifdef TARGET_PC
  void PortApplyPickupJingle(CStateManager& mgr);
#endif
  void sub_8020c3f0(CStateManager& mgr);
  void sub_8020c414(CStateManager& mgr);
  void sub_8020be90();
};
CHECK_SIZEOF(CScriptStreamedMusic, 0x54)

#endif // _CSCRIPTSTREAMEDMUSIC
