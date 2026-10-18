#include "port_collision_view.h"
#include "port_env.h"

#include "Collision/CMaterialList.hpp"
#include "Kyoto/Graphics/CGX.hpp"
#include "Kyoto/Graphics/CGraphics.hpp"
#include "Kyoto/Math/CAABox.hpp"
#include "MetaRender/CCubeRenderer.hpp"
#include "MetroidPrime/Player/CPlayer.hpp"
#include "MetroidPrime/CGameArea.hpp"
#include "MetroidPrime/CObjectList.hpp"
#include "MetroidPrime/CPhysicsActor.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/TCastTo.hpp"
#include "WorldFormat/CAreaOctTree.hpp"

#include <cstdlib>
#include <cstring>

namespace PortCollisionView {
namespace {

const char* const kNames[] = {"off", "overlay", "only"};

Mode InitialMode() {
  Mode mode = Mode::Off;
  if (const char* env = port::EnvString("MP_COLLISION_VIEW")) {
    ParseMode(env, mode);
  }
  return mode;
}

Mode sMode = InitialMode();

constexpr uint Bit(EMaterialTypes type) { return 1u << uint(type); }

// 0xRRGGBB for a static triangle's material: what it is made of where that changes how it
// plays (lava, phazon, grates that shots pass), else which way the game says it faces.
uint SurfaceColor(uint material) {
  if (material & Bit(kMT_Lava)) {
    return 0xe0602a;
  }
  if (material & Bit(kMT_Phazon)) {
    return 0x30c0d0;
  }
  if (material & (Bit(kMT_ProjectilePassthrough) | Bit(kMT_SeeThrough))) {
    return 0xd8c860;
  }
  if (material & Bit(kMT_Floor)) {
    return 0x6c8cb4;
  }
  if (material & Bit(kMT_Ceiling)) {
    return 0xa86868;
  }
  return 0xa0a0a0;
}

// Packs for CGraphics::StreamColor (0xRRGGBBAA), with each channel scaled by shade.
uint Pack(uint rgb, float shade, uint alpha) {
  uint out = alpha;
  for (int shift = 8; shift <= 24; shift += 8) {
    const float channel = float((rgb >> (shift - 8)) & 0xff) * shade;
    out |= uint(channel > 255.f ? 255.f : channel) << shift;
  }
  return out;
}

// Brightness from a fixed light above and to one side, so neighbouring faces part.
float Shade(const CVector3f& a, const CVector3f& b, const CVector3f& c) {
  const CVector3f normal = CVector3f::Cross(b - a, c - a);
  if (normal.MagSquared() < 1e-12f) {
    return 0.6f;
  }
  static const CVector3f light = CVector3f(0.35f, 0.55f, 0.76f).AsNormalized();
  return 0.35f + 0.65f * (0.5f + 0.5f * CVector3f::Dot(normal.AsNormalized(), light));
}

void SetupStates(bool blend, bool depthWrite) {
  CGraphics::DisableAllLights();
  CGX::SetNumTexGens(0);
  CGX::SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
  CGraphics::SetTevOp(kTS_Stage0, CGraphics::kEnvPassthru);
  CGraphics::SetAlphaCompare(kAF_Always, 0, kAO_And, kAF_Always, 0);
  if (blend) {
    CGraphics::SetBlendMode(kBM_Blend, kBF_SrcAlpha, kBF_InvSrcAlpha, kLO_Clear);
  } else {
    CGraphics::SetBlendMode(kBM_Blend, kBF_One, kBF_Zero, kLO_Clear);
  }
  CGraphics::SetDepthWriteMode(true, kE_LEqual, depthWrite);
}

void DrawArea(const CAreaOctTree& tree, bool only) {
  const uint count = tree.PortTriangleCount();
  SetupStates(!only, only);
  CGraphics::StreamBegin(kP_Triangles);
  for (uint tri = 0; tri < count; ++tri) {
    ushort index[3];
    tree.GetTriangleVertexIndices(ushort(tri), index);
    const CVector3f& a = tree.GetVert(index[0]);
    const CVector3f& b = tree.GetVert(index[1]);
    const CVector3f& c = tree.GetVert(index[2]);
    const uint color = Pack(SurfaceColor(tree.GetTriangleMaterial(int(tri))), Shade(a, b, c), only ? 255 : 110);
    CGraphics::StreamColor(color);
    CGraphics::StreamVertex(a);
    CGraphics::StreamVertex(b);
    CGraphics::StreamVertex(c);
  }
  CGraphics::StreamEnd();

  // The edges, so flat walls show their triangles; drawn later, so a depth range pulled a
  // little toward the eye keeps them above the faces they lie on.
  SetupStates(true, false);
  CGraphics::StreamBegin(kP_Lines);
  CGraphics::StreamColor(only ? 0x202020ffu : 0x101010a0u);
  for (uint tri = 0; tri < count; ++tri) {
    ushort index[3];
    tree.GetTriangleVertexIndices(ushort(tri), index);
    for (int e = 0; e < 3; ++e) {
      CGraphics::StreamVertex(tree.GetVert(index[e]));
      CGraphics::StreamVertex(tree.GetVert(index[(e + 1) % 3]));
    }
  }
  CGraphics::StreamEnd();
}

void DrawBox(const CAABox& box) {
  const CVector3f& lo = box.GetMinPoint();
  const CVector3f& hi = box.GetMaxPoint();
  const CVector3f p[8] = {
      CVector3f(lo.GetX(), lo.GetY(), lo.GetZ()), CVector3f(hi.GetX(), lo.GetY(), lo.GetZ()),
      CVector3f(hi.GetX(), hi.GetY(), lo.GetZ()), CVector3f(lo.GetX(), hi.GetY(), lo.GetZ()),
      CVector3f(lo.GetX(), lo.GetY(), hi.GetZ()), CVector3f(hi.GetX(), lo.GetY(), hi.GetZ()),
      CVector3f(hi.GetX(), hi.GetY(), hi.GetZ()), CVector3f(lo.GetX(), hi.GetY(), hi.GetZ()),
  };
  static const int kFaces[6][4] = {{0, 1, 2, 3}, {4, 7, 6, 5}, {0, 4, 5, 1},
                                   {1, 5, 6, 2}, {2, 6, 7, 3}, {3, 7, 4, 0}};
  static const float kShade[6] = {0.55f, 1.f, 0.75f, 0.85f, 0.7f, 0.9f};
  SetupStates(true, false);
  CGraphics::StreamBegin(kP_Triangles);
  for (int f = 0; f < 6; ++f) {
    CGraphics::StreamColor(Pack(0xff8020, kShade[f], 120));
    const int* q = kFaces[f];
    const int order[6] = {q[0], q[1], q[2], q[0], q[2], q[3]};
    for (int i = 0; i < 6; ++i) {
      CGraphics::StreamVertex(p[order[i]]);
    }
  }
  CGraphics::StreamEnd();
  CGraphics::StreamBegin(kP_Lines);
  CGraphics::StreamColor(0xffa040ffu);
  for (int i = 0; i < 4; ++i) {
    const int edges[3][2] = {{i, (i + 1) % 4}, {4 + i, 4 + (i + 1) % 4}, {i, 4 + i}};
    for (int e = 0; e < 3; ++e) {
      CGraphics::StreamVertex(p[edges[e][0]]);
      CGraphics::StreamVertex(p[edges[e][1]]);
    }
  }
  CGraphics::StreamEnd();
}

// Active actors that are solid to Samus: their collision primitive's box. Platforms carry a
// triangle tree that this draws as its bounds.
void DrawActors(const CStateManager& mgr, const CGameArea& area) {
  const CObjectList* list = area.GetPostConstructed()->x10c0_areaObjectList.get();
  if (list == nullptr) {
    return;
  }
  const CObjectList& objects = *list;
  for (int idx = objects.GetFirstObjectIndex(); idx != -1; idx = objects.GetNextObjectIndex(idx)) {
    const CPhysicsActor* actor = TCastToConstPtr< CPhysicsActor >(objects[idx]);
    if (actor == nullptr || !actor->GetActive() || static_cast< const CActor* >(actor) == static_cast< const CActor* >(mgr.GetPlayer()) ||
        !actor->GetMaterialList().HasMaterial(kMT_Solid)) {
      continue;
    }
    DrawBox(actor->GetBoundingBox());
  }
}

} // namespace

Mode GetMode() { return sMode; }
void SetMode(Mode mode) { sMode = mode; }
const char* ModeName(Mode mode) { return kNames[int(mode)]; }
bool ParseMode(const char* name, Mode& mode) {
  for (int i = 0; i < 3; ++i) {
    if (std::strcmp(name, kNames[i]) == 0) {
      mode = Mode(i);
      return true;
    }
  }
  if (std::strcmp(name, "1") == 0 || std::strcmp(name, "on") == 0) {
    mode = Mode::Only;
    return true;
  }
  if (std::strcmp(name, "0") == 0) {
    mode = Mode::Off;
    return true;
  }
  return false;
}

void Draw(const CStateManager& mgr, const CGameArea* const* areas, int count) {
  if (sMode == Mode::Off) {
    return;
  }
  const bool only = sMode == Mode::Only;
  gpRender->SetModelMatrix(CTransform4f::Identity());
  gpRender->SetWorldFog(kRFM_None, 0.f, 1.f, CColor::Black());
  CGraphics::SetCullMode(kCM_None);
  // Pulled toward the eye so coplanar faces of the world (overlay) and the edges (both)
  // win the depth test.
  CGraphics::SetDepthRange(0.125f - 2e-5f, 1.f - 2e-5f);
  for (int i = 0; i < count; ++i) {
    const CGameArea& area = *areas[i];
    if (area.IsPostConstructed() && area.GetPostConstructed()->x0_collision.get() != nullptr) {
      DrawArea(area.GetOctTree(), only);
    }
  }
  for (int i = 0; i < count; ++i) {
    if (areas[i]->IsPostConstructed()) {
      DrawActors(mgr, *areas[i]);
    }
  }
  CGraphics::SetDepthRange(0.125f, 1.f);
  CGraphics::SetCullMode(kCM_Front);
  CGraphics::SetDepthWriteMode(true, kE_LEqual, true);
  CGraphics::SetBlendMode(kBM_None, kBF_One, kBF_Zero, kLO_Clear);
}

} // namespace PortCollisionView
