#ifndef _CSCRIPTMIDI
#define _CSCRIPTMIDI

#include "MetroidPrime/CEntity.hpp"

#include "Kyoto/Audio/CMidiManager.hpp"

class CScriptMidi : public CEntity {
  friend struct PortEntitySchema; // save-state field schema (platform/include/port_entity_schema.h)
public:
  CScriptMidi(TUniqueId id, const CEntityInfo& info, const rstl::string& name, bool active,
              CAssetId csng, float, float, int);
  ~CScriptMidi() override;

  void Stop(CStateManager& mgr, float fadeTime);
  void Play(CStateManager& mgr, float fadeTime);
  DECLARE_TYPES_MATCH_OR_ACCEPT;
  void AcceptScriptMsg(EScriptObjectMessage msg, TUniqueId objId, CStateManager& stateMgr) override;
#ifdef TARGET_PC
  // Port: whether this object's song is playing (a save state keeps it).
  bool PortPlaying() const { return x3c_handle != CSfxHandle::NullHandle(); }
#endif

private:
  TToken< CMidiManager::CMidiData > x34_song;
  CSfxHandle x3c_handle;
  float x40_fadeInTime;
  float x44_fadeOutTime;
  short x48_volume;

  void StopInternal(float fadeTime);
};
CHECK_SIZEOF(CScriptMidi, 0x4c)

#endif // _CSCRIPTMIDI
