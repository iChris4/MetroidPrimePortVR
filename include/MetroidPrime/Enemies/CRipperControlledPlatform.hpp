#ifndef _CRIPPERCONTROLLEDPLATFORM
#define _CRIPPERCONTROLLEDPLATFORM

#include "MetroidPrime/ScriptObjects/CScriptPlatform.hpp"

class CRipperControlledPlatform : public CScriptPlatform {
  friend struct PortEntitySchema; // save-state field schema (platform/include/port_entity_schema.h)

public:
  CRipperControlledPlatform(
      TUniqueId, TUniqueId, const rstl::string&, const CEntityInfo&, const CTransform4f&,
      const CAABox&, bool, const rstl::optional_object< TLockedToken< CCollidableOBBTreeGroupContainer > >&);
  ~CRipperControlledPlatform();

  CQuaternion Move(float, CStateManager&) override;

private:
  TUniqueId x358_owner;
  float x35c_yaw;
};

#endif // _CRIPPERCONTROLLEDPLATFORM
