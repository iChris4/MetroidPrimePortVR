#include "MetroidPrime/CPortNestedLights.hpp"

#include "MetroidPrime/CGameLight.hpp"
#include "MetroidPrime/CStateManager.hpp"

#include "Kyoto/Particles/CElementGen.hpp"

#include "port_env.h"

void CPortNestedLights::Delete(CStateManager& mgr, TUniqueId host) {
  for (const Proxy& proxy : xLights) {
    mgr.SendScriptMsgAlways(proxy.light, host, kSM_Deactivate);
    mgr.DeleteObjectRequest(proxy.light);
  }
  xLights.clear();
}

void CPortNestedLights::Forward(EScriptObjectMessage msg, TUniqueId sender, TUniqueId host,
                                CStateManager& mgr) {
  if (xLights.empty()) {
    return;
  }
  for (const Proxy& proxy : xLights) {
    mgr.SendScriptMsgAlways(proxy.light, sender, msg);
  }
  if (msg == kSM_Deleted || msg == kSM_Deactivate || msg == kSM_Activate) {
    Delete(mgr, host);
  }
}

void CPortNestedLights::Update(CStateManager& mgr, const CParticleGen* generator, bool active,
                               TUniqueId host, TAreaId area, const rstl::string& name,
                               uint sourceId, uint priority) {
  const CElementGen* root = generator != nullptr && generator->Get4CharId() == 'PART'
                                ? static_cast< const CElementGen* >(generator)
                                : nullptr;
  // MP_NESTED_LIGHTS=0: no child particle lights (debug).
  static const bool sOn = port::EnvFlag("MP_NESTED_LIGHTS", true);
  if (!sOn || !active || root == nullptr) {
    Delete(mgr, host);
    return;
  }

  std::vector< CElementGen::CPortNestedLightSnapshot > snapshots;
  root->PortCollectNestedLightSnapshots(snapshots);
  for (size_t i = 0; i < xLights.size();) {
    const bool present = CElementGen::PortHasNestedLightSnapshotFor(snapshots, xLights[i].generator);
    if (!present || mgr.ObjectById(xLights[i].light) == nullptr) {
      const TUniqueId lightId = xLights[i].light;
      mgr.SendScriptMsgAlways(lightId, host, kSM_Deactivate);
      mgr.DeleteObjectRequest(lightId);
      xLights.erase(xLights.begin() + i);
    } else {
      ++i;
    }
  }

  for (const CElementGen::CPortNestedLightSnapshot& snapshot : snapshots) {
    Proxy* proxy = nullptr;
    for (Proxy& candidate : xLights) {
      if (candidate.generator == snapshot.generator) {
        proxy = &candidate;
        break;
      }
    }
    const CTransform4f transform = CTransform4f::Translate(snapshot.origin);
    if (proxy == nullptr) {
      const TUniqueId lightId = mgr.AllocateUniqueId();
      mgr.AddObject(rs_new CGameLight(lightId, area, active, name, transform, host, snapshot.light,
                                      sourceId, priority, 0.f));
      xLights.push_back({snapshot.generator, lightId});
    } else {
      CGameLight* light = TCastToPtr< CGameLight >(mgr.ObjectById(proxy->light));
      if (light != nullptr) {
        light->SetTransform(transform);
        light->SetLight(snapshot.light);
      }
    }
  }
}
