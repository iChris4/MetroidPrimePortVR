#include "port_actor_shadow.h"
#include "port_log.h"
#include "port_env.h"

#include "MetroidPrime/CActor.hpp"
#include "MetroidPrime/CSimpleShadow.hpp"
#include "MetroidPrime/CStateManager.hpp"

#include "Kyoto/Math/CTransform4f.hpp"

#include "dolphin/gx/GXExtra.h"
#include "port_debug.h"
#include "port_room_geo.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace PortActorShadow {
namespace {
std::vector<const CActor*> sActors;

Box ToBox(const float* v) { return {{v[0], v[1], v[2]}, {v[3], v[4], v[5]}}; }
} // namespace

bool Active() { return PortDebug::ProjectedShadows() && PortRoomGeo::GetMode() != PortRoomGeo::Mode::Off; }

void BeginFrame() { sActors.clear(); }

void Register(const CActor* actor) {
  if (sActors.size() < kMaxMaps) {
    sActors.push_back(actor);
  }
}

void RenderFrame(const CStateManager& mgr, const CTransform4f& view) {
  if (sActors.empty() || !Active()) {
    return;
  }
  // GX's view matrix (see PortSetupShadow): the camera's rows and their eye offset.
  const CVector3f eye = view.GetTranslation();
  const float axes[3][3] = {
      {view.Get00(), view.Get10(), view.Get20()},
      {view.Get02(), view.Get12(), view.Get22()},
      {-view.Get01(), -view.Get11(), -view.Get21()},
  };
  float w2v[3][4];
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      w2v[r][c] = axes[r][c];
    }
    w2v[r][3] = -(axes[r][0] * eye.GetX() + axes[r][1] * eye.GetY() + axes[r][2] * eye.GetZ());
  }
  static const bool debug = port::EnvFlag("MP_ACTOR_SHADOW_DEBUG");
  static unsigned frame = 0;
  if (debug && (++frame % 120) == 0) {
    for (size_t i = 0; i < sActors.size(); ++i) {
      const CSimpleShadow& s = *sActors[i]->GetShadow();
      const float* m = s.PortMapBox();
      const float* r = s.PortReceiverBox();
      PortLog::Write(
              "actorshadow[%zu] id=%x op=%.3f fade=%.3f map c=(%.2f %.2f %.2f) h=(%.2f %.2f %.2f) recv c=(%.2f %.2f %.2f) "
              "h=(%.2f %.2f %.2f) dyn=%d\n",
              i, sActors[i]->GetUniqueId().Value(), s.PortOpacity(), s.PortFadeStart(), m[0], m[1], m[2], m[3], m[4],
              m[5], r[0], r[1], r[2], r[3], r[4], r[5], s.PortDynamic() ? 1 : 0);
    }
  }
  for (size_t i = 0; i < sActors.size(); ++i) {
    const CActor& actor = *sActors[i];
    float clip[16];
    CasterClip(w2v, ToBox(actor.GetShadow()->PortMapBox()), clip);
    GXPortActorShadowCasters(static_cast<u32>(i), clip);
    CPortActorRenderScope presented(actor);
    actor.PortRenderActorShadowCaster(mgr);
  }
  GXPortActorShadowEnd();
  GXPortActorShadowRender();
  for (size_t i = 0; i < sActors.size(); ++i) {
    const CSimpleShadow& s = *sActors[i]->GetShadow();
    const Box rb = ToBox(s.PortReceiverBox());
    float params[kParamFloats];
    DecalParams(w2v, ToBox(s.PortMapBox()), rb, s.PortOpacity(), s.PortFadeStart(), static_cast<unsigned>(i), params);
    GXPortActorShadowDecal(static_cast<u32>(i), params);
    const float lo[3] = {rb.centre[0] - rb.half[0], rb.centre[1] - rb.half[1], rb.centre[2] - rb.half[2]};
    const float hi[3] = {rb.centre[0] + rb.half[0], rb.centre[1] + rb.half[1], rb.centre[2] + rb.half[2]};
    PortRoomGeo::DrawShadowReceivers(lo, hi);
    if (s.PortDynamic()) {
      // Filter 0x40 receivers: platforms inside the box are redrawn as decals too.
      TEntityList near;
      mgr.BuildNearList(near, CAABox(CVector3f(lo[0], lo[1], lo[2]), CVector3f(hi[0], hi[1], hi[2])),
                        CMaterialFilter::MakeInclude(CMaterialList(kMT_Platform)), nullptr);
      for (const TUniqueId id : near) {
        const CActor* platform = TCastToConstPtr< CActor >(mgr.GetObjectById(id));
        if (platform == nullptr || platform == sActors[i] || !platform->HasModelData() ||
            platform->NullModel() || !platform->IsDrawEnabled()) {
          continue;
        }
        CPortActorRenderScope presented(*platform);
        platform->PortRenderActorShadowCaster(mgr);
      }
    }
  }
  GXPortActorShadowEnd();
  // Each call covers the actors registered since the last one (one area's); the next area starts at layer 0.
  sActors.clear();
}

} // namespace PortActorShadow
