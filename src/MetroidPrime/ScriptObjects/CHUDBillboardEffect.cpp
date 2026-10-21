#include "MetroidPrime/ScriptObjects/CHUDBillboardEffect.hpp"

#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/CWorld.hpp"
#include "MetroidPrime/Cameras/CCameraManager.hpp"
#include "MetroidPrime/Cameras/CGameCamera.hpp"
#include "MetroidPrime/Player/CPlayer.hpp"

#include "MetaRender/CCubeRenderer.hpp"

#include "Kyoto/Particles/CElementGen.hpp"
#include "Kyoto/Particles/CParticleElectric.hpp"

#include "rstl/math.hpp"

#ifdef TARGET_PC
#include "Kyoto/Graphics/CGraphics.hpp"
#include "Kyoto/Math/CMath.hpp"
#include "Kyoto/Particles/CGenDescription.hpp"
#include "MetroidPrime/TCastTo.hpp"

#include <cmath>
#endif

#include "port_debug.h"
#ifdef TARGET_PC
#include "vr/vr_view.h"
#endif

int CHUDBillboardEffect::g_BillboardCount = 0;
int CHUDBillboardEffect::g_IndirectTexturedBillboardCount = 0;

float CHUDBillboardEffect::CalcGenRate() {
  float f1 = (g_BillboardCount + g_IndirectTexturedBillboardCount <= 4)
                 ? 0.f
                 : g_BillboardCount * 0.2f + g_IndirectTexturedBillboardCount * 0.1f;
  return 1.f - rstl::min_val(0.8f, f1);
}

const CVector3f& CHUDBillboardEffect::GetScaleForPOV(const CStateManager& mgr) {
  static CVector3f result(0.155f, 1.f, 0.155f);
  return result;
}

float CHUDBillboardEffect::GetNearClipDistance(const CStateManager& mgr) {
  return mgr.GetCameraManager()->GetCurrentCamera(mgr).GetNearClipDistance() + 0.01f;
}

CHUDBillboardEffect::CHUDBillboardEffect(
    const rstl::optional_object< TToken< CGenDescription > >& particle,
    const rstl::optional_object< TToken< CElectricDescription > >& electric, TUniqueId uid,
    bool active, const rstl::string& name, float dist, const CVector3f& scale0, const CColor& color,
    const CVector3f& scale1, const CVector3f& translation)
: CEffect(uid, CEntityInfo(kInvalidAreaId, CEntity::NullConnectionList), active, name,
          CTransform4f::Identity())

, xec_translation(translation.GetX(), translation.GetY() + dist, translation.GetZ())
, xf8_localScale(CVector3f::ByElementMultiply(scale1, scale0))
, x104_24_renderAsParticleGen(true)
, x104_25_enableRender(false)
, x104_26_isElementGen(false)
, x104_27_runIndefinitely(false)
, x108_timeoutTimer(0.f) {

  if (particle) {
    x104_26_isElementGen = true;
    xe8_generator = rs_new CElementGen(*particle);
    if (static_cast< CElementGen& >(*xe8_generator).IsIndirectTextured())
      ++g_IndirectTexturedBillboardCount;
  } else {
    xe8_generator = rs_new CParticleElectric(TToken< CElectricDescription >(*electric));
  }
  ++g_BillboardCount;
  xe8_generator->SetModulationColor(color);
  xe8_generator->SetLocalScale(xf8_localScale);
}

CHUDBillboardEffect::~CHUDBillboardEffect() {
  --g_BillboardCount;
  if (xe8_generator->Get4CharId() == 'PART')
    if (static_cast< CElementGen& >(*xe8_generator).IsIndirectTextured())
      --g_IndirectTexturedBillboardCount;
}

void CHUDBillboardEffect::AddToRenderer(const CFrustumPlanes& frustum,
                                        const CStateManager& mgr) const {
  if (x104_25_enableRender && x104_24_renderAsParticleGen) {
    gpRender->AddParticleGen(*xe8_generator);
  }
}

void CHUDBillboardEffect::PreRender(CStateManager& mgr, const CFrustumPlanes& frustum) {
  if (mgr.GetPlayer()->GetCameraState() == CPlayer::kCS_FirstPerson &&
      !PortDebug::HideVisorEffects()) {
    CTransform4f camXf = mgr.GetCameraManager()->GetCurrentCameraTransform(mgr);
#ifdef TARGET_PC
    camXf = PortVr::VrHeadViewTransform(camXf);
    if (xPortManagedType == 4) {
      // Remastered places the goo manager's effect in HUD space (CHUDVisorEffectData): x right
      // and y up on the plane D ahead of the eye, scaled per frame by GetHUDVisorEffectScale
      // (D * tan(fov / 2), times the aspect across), so a splat at (x, y) with size s lands at
      // (x, y) in normalized screen coordinates with half-extent s.
      const CGameCamera& cam = mgr.GetCameraManager()->GetCurrentCamera(mgr);
      const float d = xec_translation.GetY();
      const float halfHeight = d * std::tan(cam.GetRenderFov() * (M_PIF / 180.f) * 0.5f);
      const CTransform4f hudToCamera(1.f, 0.f, 0.f, 0.f, 0.f, 0.f, -1.f, 0.f, 0.f, 1.f, 0.f, 0.f);
      xe8_generator->SetGlobalTranslation(camXf * CVector3f(0.f, d, 0.f));
      xe8_generator->SetGlobalOrientation(camXf * hudToCamera);
      xe8_generator->SetLocalScale(
          CVector3f(cam.GetAspectRatio() * halfHeight, 1.f, halfHeight));
    } else
#endif
    {
      xe8_generator->SetGlobalTranslation(camXf * xec_translation);
      xe8_generator->SetGlobalOrientation(camXf);
    }
#ifdef TARGET_PC
    // camXf is the presented camera: blending it again would trail a frame behind.
    if (x104_26_isElementGen)
      static_cast< CElementGen& >(*xe8_generator).PortGlobalIsPresented();
#endif
    x104_25_enableRender = true;
  } else {
    x104_25_enableRender = false;
  }
  x104_24_renderAsParticleGen = !mgr.RenderLast(GetUniqueId());
}

void CHUDBillboardEffect::Render(const CStateManager& mgr) const {
  if (x104_25_enableRender && !x104_24_renderAsParticleGen) {
    xe8_generator->Render();
  }
}

void CHUDBillboardEffect::Think(float dt, CStateManager& mgr) {
  if (GetActive()) {
    mgr.SetActorAreaId(*this, mgr.GetWorld()->GetCurrentAreaId());
    float oldGenRate = xe8_generator->GetGeneratorRate();
    xe8_generator->SetGeneratorRate(oldGenRate * CalcGenRate());
    xe8_generator->Update(dt);
    xe8_generator->SetGeneratorRate(oldGenRate);
    if (!x104_27_runIndefinitely) {
      x108_timeoutTimer += dt;
      if (x108_timeoutTimer > 30.f) {
        mgr.DeleteObjectRequest(GetUniqueId());
        return;
      }
    }
    if (xe8_generator->IsSystemDeletable())
      mgr.DeleteObjectRequest(GetUniqueId());
  }
}

#ifdef TARGET_PC
void CHUDBillboardEffect::AcceptScriptMsg(EScriptObjectMessage msg, TUniqueId uid,
                                          CStateManager& mgr) {
  // CHUDBillboardEffectMP1: a managed effect going away resets its manager.
  if (msg == kSM_Deleted && xPortManagedType == 4 &&
      mgr.PortVisorGooManager().effect == GetUniqueId()) {
    PortSetVisorGoo(mgr, kInvalidUniqueId, 0.f);
  }
  CEffect::AcceptScriptMsg(msg, uid, mgr);
}

bool CHUDBillboardEffect::PortHasVisorGoo(CStateManager& mgr) {
  return TCastToPtr< CHUDBillboardEffect >(mgr.ObjectById(mgr.PortVisorGooManager().effect));
}

// SetManagedEffect(4, id): the old effect is unmanaged, the new one is updated once and its size
// variable looked up; an invalid id resets the ring.
void CHUDBillboardEffect::PortSetVisorGoo(CStateManager& mgr, TUniqueId uid, float dt) {
  CStateManager::PortVisorGooState& goo = mgr.PortVisorGooManager();
  if (CHUDBillboardEffect* old = TCastToPtr< CHUDBillboardEffect >(mgr.ObjectById(goo.effect))) {
    old->xPortManagedType = -1;
  }
  goo.effect = uid;
  CHUDBillboardEffect* fx = TCastToPtr< CHUDBillboardEffect >(mgr.ObjectById(uid));
  if (!fx) {
    goo.ring.Reset();
    goo.sizeHandle = kPortNoVar;
    return;
  }
  fx->xPortManagedType = 4;
  fx->xe8_generator->Update(dt);
  goo.sizeHandle = kPortNoVar;
  if (fx->x104_26_isElementGen) {
    // 0c7177e8-ec26-4794-b8f3-64e9517fa245, as stored (the first three fields little-endian).
    PortGuid kSize;
    kSize.bytes = {0xe8, 0x77, 0x71, 0x0c, 0x26, 0xec, 0x94, 0x47,
                   0xb8, 0xf3, 0x64, 0xe9, 0x51, 0x7f, 0xa2, 0x45};
    goo.sizeHandle =
        static_cast< CElementGen& >(*fx->xe8_generator).PortGetRealHandle(kSize);
  }
}

void CHUDBillboardEffect::PortAddVisorGooSplat(CStateManager& mgr) {
  CStateManager::PortVisorGooState& goo = mgr.PortVisorGooManager();
  CHUDBillboardEffect* fx = TCastToPtr< CHUDBillboardEffect >(mgr.ObjectById(goo.effect));
  if (!fx || fx->xPortManagedType != 4 || !fx->x104_26_isElementGen || mgr.Random() == nullptr) {
    return;
  }
  CElementGen& gen = static_cast< CElementGen& >(*fx->xe8_generator);
  const CGenDescription* desc = gen.PortDesc();
  PortVisorGoo::Splat splat;
  if (!PortVisorGoo::Plan(goo.ring, *mgr.Random(), static_cast< int >(gen.GetParticleCountAll()),
                          desc->xPortVisorData, desc->xPortVisorMinScale,
                          desc->xPortVisorMaxScale,
                          static_cast< float >(CGraphics::GetViewportWidth()),
                          static_cast< float >(CGraphics::GetViewportHeight()), splat)) {
    return;
  }
  gen.PortBindReal(goo.sizeHandle, splat.size);
  gen.PortCreateParticlesAtPositionsIncludingChildren(1, &splat.pos);
  PortVisorGoo::Push(goo.ring, splat.budget, splat.pos);
}
#endif

ENTITY_ACCEPT_IMPL(CHUDBillboardEffect)
