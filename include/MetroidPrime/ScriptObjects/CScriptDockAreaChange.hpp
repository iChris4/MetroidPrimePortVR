#ifndef _CSCRIPTDOCKAREACHANGE
#define _CSCRIPTDOCKAREACHANGE

#include "MetroidPrime/CEntity.hpp"

class CScriptDockAreaChange : public CEntity {
  friend struct PortEntitySchema; // save-state field schema (platform/include/port_entity_schema.h)
  int x34_dockReference;

public:
  CScriptDockAreaChange(const TUniqueId, const rstl::string&, const CEntityInfo&, int, const bool);

  void AcceptScriptMsg(EScriptObjectMessage msg, TUniqueId objId, CStateManager& stateMgr) override;
  DECLARE_ACCEPT;
};

#endif // _CSCRIPTDOCKAREACHANGE
