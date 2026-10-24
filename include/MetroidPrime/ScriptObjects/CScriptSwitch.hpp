#ifndef _CSCRIPTSWITCH
#define _CSCRIPTSWITCH

#include "types.h"

#include "MetroidPrime/CEntity.hpp"

class CScriptSwitch : public CEntity {
  friend struct PortEntitySchema; // save-state field schema (platform/include/port_entity_schema.h)
public:
  CScriptSwitch(TUniqueId uid, const rstl::string& name, const CEntityInfo& info, bool, bool, bool);

  DECLARE_ACCEPT;
  void AcceptScriptMsg(EScriptObjectMessage msg, TUniqueId uid, CStateManager& mgr);

private:
  bool mOpened;
  bool mCloseOnOpened;
};

#endif // _CSCRIPTSWITCH
