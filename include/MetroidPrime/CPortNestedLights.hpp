#ifndef _CPORTNESTEDLIGHTS
#define _CPORTNESTEDLIGHTS

#include "types.h"

#include "MetroidPrime/CEntityInfo.hpp"
#include "MetroidPrime/TGameTypes.hpp"
#include "rstl/string.hpp"

#include <vector>

class CParticleGen;
class CStateManager;

// Port-only: the scene lights of a particle host's nested (PHDR) child generators. Remastered
// builds a light for every generator, embedded children included; retail hosts only light the
// root, so a host keeps one of these and calls Update every frame it ticks its generator, Delete
// when it drops or stops the generator, and Forward from its AcceptScriptMsg. Each light is a
// CGameLight owned by the host through this list.
class CPortNestedLights {
public:
  // Sync the lights to `generator`'s live nested children; with `active` false (or no PART
  // generator) every light is deleted. `sourceId` and `priority` go to the CGameLights.
  void Update(CStateManager& mgr, const CParticleGen* generator, bool active, TUniqueId host,
              TAreaId area, const rstl::string& name, uint sourceId, uint priority);
  void Delete(CStateManager& mgr, TUniqueId host);
  // Hands `msg` to the lights; Deleted, Deactivate and Activate then drop them.
  void Forward(EScriptObjectMessage msg, TUniqueId sender, TUniqueId host, CStateManager& mgr);
  bool Empty() const { return xLights.empty(); }
  size_t Count() const { return xLights.size(); }

private:
  struct Proxy {
    const CParticleGen* generator; // key only; may be stale after the generator tree updates
    TUniqueId light;
  };
  std::vector< Proxy > xLights;
};

#endif // _CPORTNESTEDLIGHTS
