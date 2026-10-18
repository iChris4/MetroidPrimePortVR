#include "MetroidPrime/CMapWorld.hpp"

#include "MetroidPrime/CMemoryDrawEnum.hpp"

#include "Kyoto/CFactoryFnReturn.hpp"
#include "Kyoto/Graphics/CGraphics.hpp"
#include "Kyoto/Math/CMath.hpp"
#include "Kyoto/Math/CMatrix3f.hpp"
#include "Kyoto/Math/CVector2f.hpp"
#include "Kyoto/Streams/CInputStream.hpp"
#include "MetaRender/CCubeRenderer.hpp"
#include "MetroidPrime/CGameArea.hpp"
#include "MetroidPrime/CMapWorldInfo.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/CWorld.hpp"
#include "MetroidPrime/Tweaks/CTweakAutoMapper.hpp"
#include "rstl/algorithm.hpp"
#include "rstl/math.hpp"
#include <float.h>
#include <math.h>
#include <stdlib.h>

#ifdef TARGET_PC
#include "Kyoto/Graphics/CGX.hpp"
#include "MetroidPrime/Player/CGameState.hpp"
#include "port_map_marks.h"
#include "port_map_pickups.h"
#include "port_map_batch.h"
#include <dolphin/gx/GXGet.h>

namespace {
// Rooms mode: true when the area still holds a pickup whose memory relay is not
// in the world's mailbox.
bool PortRoomHoldsPickup(const IWorld& world, int areaIdx) {
  const CAssetId worldId = world.IGetWorldAssetId();
  const CScriptMailbox& mailbox = *gpGameState->StateForWorld(worldId).Mailbox().GetPtr();
  size_t count = 0;
  const PortMapPickups::Dot* dots = PortMapPickups::Dots(count);
  for (size_t i = 0; i < count; ++i) {
    if (dots[i].world == worldId && world.IGetAreaId(dots[i].area).Value() == areaIdx &&
        !mailbox.HasMsg(TEditorId(dots[i].relay))) {
      return true;
    }
  }
  return false;
}
} // namespace
#endif

struct CMapObjectSortInfoGreaterThan {
  CMapObjectSortInfoGreaterThan() {}
  bool operator()(const CMapWorld::CMapObjectSortInfo& a,
                  const CMapWorld::CMapObjectSortInfo& b) const {
    if (a.GetObjectCode() == b.GetObjectCode()) {
      return a.GetZDistance() > b.GetZDistance();
    }
    return a.GetObjectCode() > b.GetObjectCode();
  }
};

class Circle2 {
  CVector2f x0_center;
  float x8_radius;

public:
  Circle2() : x0_center(CVector2f::Zero()), x8_radius(0.f) {}
  CVector2f& Center() { return x0_center; }
  float& Radius() { return x8_radius; }
  const CVector2f& GetCenter() const { return x0_center; }
  float GetRadius() const { return x8_radius; }
};
CHECK_SIZEOF(Circle2, 0xc)

struct Support {
  int x0_count;
  int x4_indices[3];

  bool Contains(int idx, CVector2f** points) {
    for (int i = 0; i < x0_count; ++i) {
      const CVector2f delta = *points[idx] - *points[x4_indices[i]];
      if (delta.MagSquared() < 0.01f) {
        return true;
      }
    }
    return false;
  }
};
CHECK_SIZEOF(Support, 0x10)

static Circle2 MinCircle(int count, const CVector2f* points);

CMapWorld::CMapObjectSortInfo::CMapObjectSortInfo(float zDist, int areaIdx, EObjectCode type,
                                                  int idx, CColor surfColor, CColor outlineColor)
: x0_zDist(zDist)
, x4_areaIdx(areaIdx)
, x8_typeAndIdx(static_cast< int >(type) | idx)
, xc_surfColor(surfColor)
, x10_outlineColor(outlineColor) {}

CMapWorld::CMapAreaData::CMapAreaData(CAssetId areaRes, EMapAreaList list, CMapAreaData* next)
: x0_areaRes(areaRes)
, x4_area(gpSimplePool->GetObj(SObjectTag('MAPA', areaRes)))
, x10_list(list)
, x14_next(next) {}

void CMapWorld::CMapAreaData::Lock() { x4_area.Lock(); }

void CMapWorld::CMapAreaData::Unlock() { x4_area.Unlock(); }

bool CMapWorld::CMapAreaData::IsLoaded() const {
  if (!x4_area.GetToken().IsLocked()) {
    return false;
  }
  return x4_area.TryCache();
}

CMapArea* CMapWorld::CMapAreaData::GetMapArea() const {
  x4_area.TryCache();
  return x4_area.GetObject();
}

CMapWorld::CMapWorldDrawParms::CMapWorldDrawParms(
    float alphaSurfVisited, float alphaOlVisited, float alphaSurfUnvisited, float alphaOlUnvisited,
    float alpha, const CStateManager& mgr, const CTransform4f& modelXf, const CTransform4f& viewXf,
    const IWorld& wld, const CMapWorldInfo& mwInfo, float outlineWidthScale, bool sortDoorSurfs,
    float playerFlash, float hintFlash, float objectScale)
: x0_alphaSurfVisited(alphaSurfVisited)
, x4_alphaOlVisited(alphaOlVisited)
, x8_alphaSurfUnvisited(alphaSurfUnvisited)
, xc_alphaOlUnvisited(alphaOlUnvisited)
, x10_alpha(alpha)
, x14_outlineWidthScale(outlineWidthScale)
, x18_mgr(mgr)
, x1c_modelXf(modelXf)
, x20_viewXf(viewXf)
, x24_wld(wld)
, x28_mwInfo(mwInfo)
, x2c_playerFlashIntensity(playerFlash)
, x30_hintFlashIntensity(hintFlash)
, x34_objectScale(objectScale)
, x38_sortDoorSurfs(sortDoorSurfs) {}

CMapWorld::CMapAreaBFSInfo::CMapAreaBFSInfo(int areaIdx, int depth, float surfDepth,
                                            float outlineDepth)
: x0_areaIdx(areaIdx)
, x4_depth(depth)
, x8_surfDrawDepth(surfDepth)
, xc_outlineDrawDepth(outlineDepth) {}

CMapWorld::CMapWorld(CInputStream& in)
: x10_listHeads(3, nullptr)
, x30_worldSpherePoint(CVector3f::Zero())
, x3c_worldSphereRadius(0.f)
, x40_worldSphereHalfDepth(0.f) {
  in.ReadLong();
  in.ReadLong();
  int areaCount = in.Get< int >();
  x0_areas.reserve(areaCount);
  x20_traversed = rstl::vector< bool >(areaCount, false);
  for (int i = 0; i < areaCount; ++i) {
    CAssetId areaRes = in.ReadLong();
    x0_areas.push_back(CMapAreaData(areaRes, kMAL_Unloaded, i == 0 ? nullptr : &x0_areas[i - 1]));
  }
  x10_listHeads[kMAL_Unloaded] = &x0_areas.back();
  CMemoryDrawEnum::AddWorldMemory(x0_areas.capacity() * sizeof(CMapAreaData) + sizeof(*this));
}

CMapWorld::~CMapWorld() {
  CMemoryDrawEnum::SubtractWorldMemory(x0_areas.capacity() * sizeof(CMapAreaData) + sizeof(*this));
}

CMapArea* CMapWorld::GetMapArea(int aid) const { return x0_areas[aid].GetMapArea(); }

bool CMapWorld::IsMapAreaInBFSInfoVector(const CMapAreaData* area,
                                         const rstl::vector< CMapAreaBFSInfo >& vec) const {
  for (AUTO(it, vec.begin()); it != vec.end(); ++it) {
    if (area == &x0_areas[it->GetAreaIndex()]) {
      return true;
    }
  }
  return false;
}

void CMapWorld::SetWhichMapAreasLoaded(const IWorld& wld, int start, int count) {
  ClearTraversedFlags();
  rstl::vector< CMapAreaBFSInfo > bfsInfos;
  bfsInfos.reserve(x0_areas.size());
  DoBFS(wld, start, count, 9999.f, 9999.f, false, bfsInfos);
  for (int i = 0; i < 2; ++i) {
    CMapAreaData* data = x10_listHeads[i == 0 ? kMAL_Loaded : kMAL_Loading];
    while (data != nullptr) {
      CMapAreaData* next = data->NextMapAreaData();
      if (!IsMapAreaInBFSInfoVector(data, bfsInfos)) {
        data->Unlock();
        MoveMapAreaToList(data, kMAL_Unloaded);
      }
      data = next;
    }
  }
  for (AUTO(it, bfsInfos.begin()); it != bfsInfos.end(); ++it) {
    CMapAreaData& data = x0_areas[it->GetAreaIndex()];
    data.Lock();
    if (data.GetContainingList() == kMAL_Unloaded) {
      MoveMapAreaToList(&data, kMAL_Loading);
    }
  }
}

bool CMapWorld::IsMapAreasStreaming() const {
  bool streaming = false;
  CMapAreaData* data = x10_listHeads[kMAL_Loading];
  while (data != nullptr) {
    CMapAreaData* next = data->NextMapAreaData();
    if (data->IsLoaded()) {
      const_cast< CMapWorld* >(this)->MoveMapAreaToList(data, kMAL_Loaded);
    } else {
      streaming = true;
    }
    data = next;
  }
  const bool result = streaming;
  return result;
}

void CMapWorld::MoveMapAreaToList(CMapAreaData* data, EMapAreaList list) {
  CMapAreaData* previous = nullptr;
  for (CMapAreaData* current = x10_listHeads[data->GetContainingList()];;
       previous = current, current = current->NextMapAreaData()) {
    if (current == data) {
      CMapAreaData* next = current->NextMapAreaData();
      if (previous == nullptr) {
        x10_listHeads[data->GetContainingList()] = next;
      } else {
        previous->SetNextMapArea(next);
      }
      break;
    }
  }
  data->SetNextMapArea(x10_listHeads[list]);
  data->SetContainingList(list);
  x10_listHeads[list] = data;
}

int CMapWorld::GetCurrentMapAreaDepth(const IWorld& wld, int aid) const {
  ClearTraversedFlags();
  rstl::vector< CMapAreaBFSInfo > bfsInfos;
  bfsInfos.reserve(x0_areas.size());
  DoBFS(wld, aid, 9999, 9999.f, 9999.f, false, bfsInfos);
  if (bfsInfos.empty()) {
    return 0;
  }
  return bfsInfos.back().GetDepth();
}

rstl::vector< int > CMapWorld::GetVisibleAreas(const IWorld& wld,
                                               const CMapWorldInfo& mwInfo) const {
  rstl::vector< int > areas;
  areas.reserve(x0_areas.size());
  for (int i = 0; i < x0_areas.size(); ++i) {
    if (!IsMapAreaValid(wld, i, true)) {
      continue;
    }
    if (GetMapArea(i)->GetIsVisibleToAutoMapper(mwInfo.IsWorldVisible(i),
                                                mwInfo.IsAreaVisible(i))) {
      areas.push_back(i);
    }
  }
  return areas;
}

void CMapWorld::Draw(const CMapWorldDrawParms& parms, int curArea, int otherArea, float depth1,
                     float depth2, bool inMapScreen) const {
  if (depth1 == 0.f && depth2 == 0.f) {
    return;
  }
  ClearTraversedFlags();
  const IWorld& wld = parms.GetWorld();
  int areaDepth = CMath::CeilingF(rstl::max_val(depth1, depth2));
  rstl::vector< CMapAreaBFSInfo > bfsInfos;
  bfsInfos.reserve(x0_areas.size());
  if (curArea != otherArea) {
    x20_traversed[otherArea] = true;
    DoBFS(wld, curArea, areaDepth, depth1, depth2, true, bfsInfos);
    float lowDepth1 = CMath::CeilingF(depth1 - 1.f);
    float newDepth1 =
        lowDepth1 +
        (depth1 == CMath::FloorF(depth1) ? 0.f : 1.f - static_cast< float >(fmod(depth1, 1.0)));
    float lowDepth2 = CMath::CeilingF(depth2 - 1.f);
    float newDepth2 =
        lowDepth2 +
        (depth2 == CMath::FloorF(depth2) ? 0.f : 1.f - static_cast< float >(fmod(depth2, 1.0)));
    int otherDepth = CMath::CeilingF(rstl::max_val(newDepth1, newDepth2));
    if (wld.IGetAreaAlways(otherArea)->IIsActive()) {
      x20_traversed[otherArea] = false;
      DoBFS(wld, otherArea, otherDepth, newDepth1, newDepth2, true, bfsInfos);
    }
  } else {
    DoBFS(wld, curArea, areaDepth, depth1, depth2, true, bfsInfos);
  }
  DrawAreas(parms, curArea, bfsInfos, inMapScreen);
}

void CMapWorld::DoBFS(const IWorld& wld, int startArea, int areaCount, float surfDepth,
                      float outlineDepth, bool checkLoad,
                      rstl::vector< CMapAreaBFSInfo >& bfsInfos) const {
  if (areaCount > 0 && IsMapAreaValid(wld, startArea, checkLoad)) {
    int idx = bfsInfos.size();
    bfsInfos.push_back(CMapAreaBFSInfo(startArea, 1, surfDepth, outlineDepth));
    x20_traversed[startArea] = true;
    for (;; ++idx) {
      if (idx == bfsInfos.size()) {
        break;
      }
      int areaIdx = bfsInfos[idx].GetAreaIndex();
      int depth = bfsInfos[idx].GetDepth();
      float surfaceDepth = bfsInfos[idx].GetSurfaceDrawDepth() - 1.f;
      float outlineDepth = bfsInfos[idx].GetOutlineDrawDepth() - 1.f;
      if (depth == areaCount) {
        continue;
      }
      const IGameArea* area = wld.IGetAreaAlways(areaIdx);
      for (int i = 0; i < static_cast< int >(area->IGetNumAttachedAreas()); ++i) {
        int attached = area->IGetAttachedAreaId(i).Value();
        if (IsMapAreaValid(wld, attached, checkLoad) && !x20_traversed[attached]) {
          bfsInfos.push_back(CMapAreaBFSInfo(attached, depth + 1, surfaceDepth, outlineDepth));
          x20_traversed[attached] = true;
        }
      }
    }
  }
}

bool CMapWorld::IsMapAreaValid(const IWorld& wld, const int areaIdx, bool checkLoad) const {
  if (!wld.IGetAreaAlways(areaIdx)->IIsActive()) {
    return false;
  }
  const CMapArea* area = GetMapArea(areaIdx);
  if (checkLoad && area == nullptr) {
    return false;
  }
  return true;
}

void CMapWorld::DrawAreas(const CMapWorldDrawParms& parms, int selArea,
                          const rstl::vector< CMapAreaBFSInfo >& bfsInfos, bool inMapScreen) const {
  gpRender->SetBlendMode_AlphaBlended();
  CGraphics::SetLineWidth(1.f, kTO_One);
  rstl::vector< CMapObjectSortInfo > sortInfos;
  int surfaceCount = 0;
  int objectCount = 0;
  for (int i = 0; i < bfsInfos.size(); ++i) {
    const CMapArea* area = GetMapArea(bfsInfos[i].GetAreaIndex());
    surfaceCount += area->GetNumSurfaces();
    objectCount += area->GetNumMappableObjects();
  }
  sortInfos.reserve(surfaceCount + objectCount +
                    (parms.GetIsSortDoorSurfaces() ? objectCount * 6 : 0));
  const CStateManager& mgr = parms.GetStateManager();
  const CMapWorldInfo& mwInfo = parms.GetMapWorldInfo();
  int playerArea = mgr.GetNextAreaId().Value();
  for (int i = 0; i < bfsInfos.size(); ++i) {
    const CMapAreaBFSInfo& bfsInfo = bfsInfos[i];
    int areaIdx = bfsInfo.GetAreaIndex();
    CMapArea* area = GetMapArea(areaIdx);
    if (!area->GetIsVisibleToAutoMapper(mwInfo.IsWorldVisible(areaIdx),
                                        mwInfo.IsAreaVisible(areaIdx))) {
      continue;
    }
    const float surfaceDepth = bfsInfo.GetSurfaceDrawDepth();
    const float outlineDepth = bfsInfo.GetOutlineDrawDepth();
    float alphaSurfVisited = parms.GetAlphaSurfaceVisited();
    float alphaOutlineVisited = parms.GetAlphaOutlineVisited();
    float alphaSurfUnvisited = parms.GetAlphaSurfaceUnvisited();
    float alphaOutlineUnvisited = parms.GetAlphaOutlineUnvisited();
    const CTransform4f& cameraXf = parms.GetCameraTransform();
    float surfaceFrac = surfaceDepth - CMath::FloorF(surfaceDepth);
    float surfaceAlpha = surfaceDepth >= 1.f ? 1.f : surfaceDepth >= 0.f ? surfaceFrac : 0.f;
    float outlineFrac = outlineDepth - CMath::FloorF(outlineDepth);
    float outlineAlpha = outlineDepth >= 1.f ? 1.f : outlineDepth >= 0.f ? outlineFrac : 0.f;
    bool visited = mwInfo.IsAreaVisited(areaIdx);
    float alphaSurf = visited ? alphaSurfVisited : alphaSurfUnvisited;
    float alphaOutline = visited ? alphaOutlineVisited : alphaOutlineUnvisited;
    CColor surfaceColor = visited ? gpTweakAutoMapper->x3c_surfColorVisited
                                  : gpTweakAutoMapper->x44_surfColorUnvisited;
    CColor outlineColor = visited ? gpTweakAutoMapper->x40_outlineColorVisited
                                  : gpTweakAutoMapper->x48_outlineColorUnvisited;
    CColor surfaceSelect = visited ? gpTweakAutoMapper->x4c_surfaceSelectColorVisited
                                   : gpTweakAutoMapper->x7c_surfaceSelectColorUnvisited;
    CColor outlineSelect = visited ? gpTweakAutoMapper->x50_outlineSelectColorVisited
                                   : gpTweakAutoMapper->x80_outlineSelectColorUnvisited;
    CColor hintFlash =
        CColor::Lerp(CColor(0u), CColor(uchar(255), uchar(255), uchar(255), uchar(0)),
                     parms.GetHintAreaFlashIntensity());
    CColor finalSurface = areaIdx == selArea && inMapScreen
                              ? CColor::Add(surfaceSelect, hintFlash)
                              : surfaceColor.WithAlphaOf(surfaceAlpha * alphaSurf);
    CColor finalOutline = areaIdx == selArea && inMapScreen
                              ? CColor::Add(outlineSelect, hintFlash)
                              : outlineColor.WithAlphaOf(outlineAlpha * alphaOutline);
    if ((selArea != playerArea || parms.GetHintAreaFlashIntensity() == 0.f) &&
        playerArea == areaIdx && this == mgr.GetWorld()->GetMapWorld()) {
      float pulse = parms.GetPlayerAreaFlashIntensity();
      const CColor& flashColor = gpTweakAutoMapper->xf4_areaFlashPulseColor;
      finalSurface = CColor::Lerp(finalSurface, flashColor, pulse);
      finalOutline = CColor::Lerp(finalOutline, flashColor, pulse);
    }
#ifdef TARGET_PC
    if (PortMapMarks::IsMarked(parms.GetWorld().IGetWorldAssetId(), areaIdx)) {
      // A room the player marked: cyan, over the Rooms amber.
      const CColor cyan(0x40e0ffffu);
      finalSurface = CColor::Lerp(finalSurface, cyan, 0.6f).WithAlphaOf(finalSurface.GetAlpha());
      finalOutline = CColor::Lerp(finalOutline, cyan, 0.8f).WithAlphaOf(finalOutline.GetAlpha());
    } else if (PortMapPickups::Mode() == PortDebug::kMapPickups_Rooms &&
               PortRoomHoldsPickup(parms.GetWorld(), areaIdx)) {
      // Amber half way, alpha kept.
      const CColor amber(0xffb030ffu);
      finalSurface = CColor::Lerp(finalSurface, amber, 0.5f).WithAlphaOf(finalSurface.GetAlpha());
      finalOutline = CColor::Lerp(finalOutline, amber, 0.5f).WithAlphaOf(finalOutline.GetAlpha());
    }
#endif
    const CTransform4f modelView =
        cameraXf.GetQuickInverse() * area->GetAreaPostTransform(parms.GetWorld(), areaIdx);
    int j;
    int count = area->GetNumSurfaces();
    for (j = 0; j < count; ++j) {
      const CMapArea::CMapAreaSurface& surface = area->GetSurface(j);
      const CVector3f center = surface.GetCenterPosition();
      const CVector3f pos = modelView * center;
      sortInfos.push_back(CMapObjectSortInfo(pos.GetY(), areaIdx, CMapObjectSortInfo::kOC_Surface,
                                             j, finalSurface, finalOutline));
    }
    int surfaceBase = 0;
    for (j = 0; j < area->GetNumMappableObjects(); surfaceBase += 6, ++j) {
      const CMappableObject& object = area->GetMappableObject(j);
      if (!object.GetIsVisibleToAutoMapper(mwInfo.IsWorldVisible(areaIdx), mwInfo)) {
        continue;
      }
      bool door = CMappableObject::IsDoorType(object.GetType());
      if (door) {
        if (!mwInfo.IsAreaVisible(areaIdx)) {
          continue;
        }
        if (parms.GetIsSortDoorSurfaces()) {
          int objectFace, face;
          for (face = 0, objectFace = surfaceBase; face < 6; ++face, ++objectFace) {
            const CVector3f& center = object.BuildSurfaceCenterPoint(face);
            const CVector3f translated =
                CMapArea::GetAreaPostTranslate(parms.GetWorld(), areaIdx) + center;
            const CVector3f pos = modelView * translated;
            sortInfos.push_back(CMapObjectSortInfo(pos.GetY(), areaIdx,
                                                   CMapObjectSortInfo::kOC_DoorSurface, objectFace,
                                                   CColor(), CColor()));
          }
          continue;
        }
      }
      const CVector3f origin = object.GetTransform().GetTranslation();
      const CVector3f translated =
          CMapArea::GetAreaPostTranslate(parms.GetWorld(), areaIdx) + origin;
      const CVector3f pos = modelView * translated;
      CMapObjectSortInfo::EObjectCode code =
          door ? CMapObjectSortInfo::kOC_Door : CMapObjectSortInfo::kOC_Object;
      sortInfos.push_back(CMapObjectSortInfo(pos.GetY(), areaIdx, code, j, CColor(), CColor()));
    }
  }

  const CTransform4f& modelXf = parms.GetPlaneProjectionTransform();
  const CTransform4f& cameraXf = parms.GetCameraTransform();
  float outlineWidth = parms.GetOutlineWidthScale();
  if (sortInfos.size() > 0) {
    rstl::sort(sortInfos.begin(), sortInfos.end(), CMapObjectSortInfoGreaterThan());
    enum EDrawMode { kDM_Invalid = -1, kDM_Surface, kDM_Door, kDM_DoorSurface, kDM_Object };
    EDrawMode lastMode = kDM_Invalid;
    int lastArea = -1;
    CMapArea::CMapAreaSurface::SetupGXMaterial();
#ifdef TARGET_PC
    // Port: the HUD minimap and the map screen. The full map drew a GX draw per room
    // surface, outline and door face, ~5,000 at the Chozo Ruins plaza: 36 FPS on the
    // Quest 3 on the virtual screen, far less as the 3D map in the headset's view.
    const bool batchEnabled =
        inMapScreen ? PortMapBatch::EnabledInMapScreen() : PortMapBatch::Enabled();
    // Kept across frames (game thread): the full map's ~5 MB of records made again in a
    // fresh vector each frame grew it by copies and gave the memory back to the system,
    // page faults and all, nearly half the Quest's game thread.
    static PortMapBatch::Geometry sBatch;
    PortMapBatch::Geometry& batch = sBatch;
    bool batchPending = false;
    const auto flushBatch = [&] {
      if (!batchPending) { return; }
      PortMapBatch::Draw(batch, modelXf);
      CMapArea::CMapAreaSurface::SetupGXMaterial();
      batchPending = false;
      lastMode = kDM_Invalid;
      lastArea = -1;
    };
    // A new batch starts from the line width the GX state has
    const auto beginBatch = [&] {
      if (batchPending) { return; }
      u8 width;
      GXTexOffset offset;
      GXGetLineWidth(&width, &offset);
      batch.Reset(width);
    };
#endif
    for (int i = 0; i < sortInfos.size(); ++i) {
      const CMapObjectSortInfo& info = sortInfos[i];
      CMapObjectSortInfo::EObjectCode type = info.GetObjectCode();
      int areaIdx = info.GetAreaIndex();
      int idx = info.GetLocalObjectIndex();
      const CColor& surfaceColor = info.GetSurfaceColor();
      const CColor& outlineColor = info.GetOutlineColor();
      CMapArea* area = GetMapArea(areaIdx);
      const CTransform4f& areaXf = area->GetAreaPostTransform(parms.GetWorld(), areaIdx);
#ifdef TARGET_PC
      // Doors join the batch too; only icons interrupt it.
      const bool batchesDoor = batchEnabled && (type == CMapObjectSortInfo::kOC_Door ||
                                                type == CMapObjectSortInfo::kOC_DoorSurface);
      if (type != CMapObjectSortInfo::kOC_Surface && !batchesDoor) { flushBatch(); }
#endif
      if (type == CMapObjectSortInfo::kOC_Surface) {
        const CMapArea::CMapAreaSurface& surface = area->GetSurface(idx);
        float linear = gpTweakAutoMapper->x54_mapSurfaceNormColorLinear;
        float constant = gpTweakAutoMapper->x58_mapSurfaceNormColorConstant;
        float shade =
            linear * rstl::max_val(0.f, CVector3f::Dot(-1.f * cameraXf.GetForward(),
                                                       areaXf.Rotate(surface.GetNormal())));
        shade = constant + shade;
        const CColor normalColor(shade, shade, shade, 1.f);
        const CColor color = CColor::Modulate(surfaceColor, normalColor);
#ifdef TARGET_PC
        if (batchEnabled) {
          beginBatch();
          if (surface.AppendToBatch(batch, area->GetVertices(), area->GetVertexCount(),
                                    areaXf, color, outlineColor, outlineWidth)) {
            batchPending = true;
            continue;
          }
          flushBatch();
        }
#endif
        bool needsVertices = lastArea != areaIdx || lastMode != kDM_Surface;
        if (needsVertices) {
          gpRender->SetModelMatrix(modelXf * areaXf);
        }
        const CVector3f* vertices = needsVertices ? area->GetVertices() : nullptr;
        surface.Draw(vertices, area->GetVertexCount(), color, outlineColor, outlineWidth);
        lastMode = kDM_Surface;
      } else if (type == CMapObjectSortInfo::kOC_Door || type == CMapObjectSortInfo::kOC_Object) {
        EDrawMode mode = type == CMapObjectSortInfo::kOC_Door ? kDM_Door : kDM_Object;
        const CMappableObject& object = area->GetMappableObject(idx);
        bool needsVertices = lastMode != mode;
        const CTransform4f objXf =
            CTransform4f::Translate(CMapArea::GetAreaPostTranslate(parms.GetWorld(), areaIdx)) *
            object.GetTransform();
#ifdef TARGET_PC
        if (batchesDoor) {
          beginBatch();
          object.AppendDoorToBatch(batch, objXf, selArea, mwInfo, parms.GetAlpha());
          batchPending = true;
          continue;
        }
#endif
        gpRender->SetModelMatrix(
            type == CMapObjectSortInfo::kOC_Door
                ? modelXf * objXf
                : modelXf * objXf *
                      CTransform4f(cameraXf.BuildMatrix3f() *
                                       CMatrix3f::Scale(parms.GetObjectScale()),
                                   CVector3f::Zero()));
        object.Draw(selArea, mwInfo, parms.GetAlpha(), needsVertices);
        lastMode = mode;
      } else if (type == CMapObjectSortInfo::kOC_DoorSurface) {
        const CMappableObject& object = area->GetMappableObject(idx / 6);
#ifdef TARGET_PC
        if (batchesDoor) {
          beginBatch();
          object.AppendDoorToBatch(
              batch,
              CTransform4f::Translate(CMapArea::GetAreaPostTranslate(parms.GetWorld(), areaIdx)) *
                  object.GetTransform(),
              selArea, mwInfo, parms.GetAlpha(), idx % 6);
          batchPending = true;
          continue;
        }
#endif
        gpRender->SetModelMatrix(
            modelXf *
            CTransform4f::Translate(CMapArea::GetAreaPostTranslate(parms.GetWorld(), areaIdx)) *
            object.GetTransform());
        object.DrawDoorSurface(selArea, mwInfo, parms.GetAlpha(), idx % 6,
                               lastMode != kDM_DoorSurface);
        lastMode = kDM_DoorSurface;
      }
      lastArea = areaIdx;
    }
#ifdef TARGET_PC
    flushBatch();
#endif
  }
#ifdef TARGET_PC
  if (PortMapPickups::Mode() == PortDebug::kMapPickups_All ||
      PortMapPickups::Mode() == PortDebug::kMapPickups_Seen) {
    DrawPortPickups(parms, bfsInfos);
  }
#endif
}

#ifdef TARGET_PC
// Port: a white dot for each pickup not yet collected (PortMapPickups), in the
// rooms this pass drew (Seen mode: only those the player has seen, or that a
// map station mapped); tracker colours in an Archipelago game. With pickup
// kinds on, a minor item is a ring instead of a dot and a collected pickup a
// grey cross. Map objects sort after everything else, so drawing the dots last
// keeps retail's order. A pickup counts as collected once its memory relay is
// in the world's mailbox, which is what keeps it gone after a reload.
void CMapWorld::DrawPortPickups(const CMapWorldDrawParms& parms,
                                const rstl::vector< CMapAreaBFSInfo >& bfsInfos) const {
  const IWorld& world = parms.GetWorld();
  const CAssetId worldId = world.IGetWorldAssetId();
  const CMapWorldInfo& mwInfo = parms.GetMapWorldInfo();
  const CScriptMailbox& mailbox = *gpGameState->StateForWorld(worldId).Mailbox().GetPtr();
  const CTransform4f& modelXf = parms.GetPlaneProjectionTransform();
  // Faces the camera and scales with the map like the retail icons.
  const CTransform4f billboard(parms.GetCameraTransform().BuildMatrix3f() *
                                   CMatrix3f::Scale(parms.GetObjectScale()),
                               CVector3f::Zero());
  const CColor white = CColor(0xffffffff).WithAlphaOf(parms.GetAlpha());
  static const int kSegments = 12;
  static const float kRadius = 1.3f;
  static const float kRimRadius = 1.7f;
  // A minor item's ring keeps the dot's outline with a rim-coloured hole.
  static const float kHoleRadius = 0.7f;
  // The cross's bars: half length and half width, then the rim's.
  static const float kBarLength = 1.4f;
  static const float kBarWidth = 0.3f;
  static const float kRimBarLength = 1.8f;
  static const float kRimBarWidth = 0.7f;
  static const float kHalfSqrt2 = 0.70710678f;
  const CColor rim = CColor(0x202020ff).WithAlphaOf(parms.GetAlpha());
  bool setUp = false;
  size_t count = 0;
  const PortMapPickups::Dot* dots = PortMapPickups::Dots(count);
  // An Archipelago game colours the dots like a tracker, and keeps the
  // collected ones as grey dots.
  static const uint kTrackerColors[] = {0xffffffff, 0x40e060ff, 0xffd830ff,
                                        0x4090ffff, 0xf04040ff, 0x808080ff};
  const unsigned char* tracker = nullptr;
  if (!PortMapPickups::Colors(tracker)) {
    tracker = nullptr;
  }
  const unsigned char* kinds = PortDebug::MapPickupKinds() ? PortMapPickups::Kinds() : nullptr;
  const bool seenOnly = PortMapPickups::Mode() == PortDebug::kMapPickups_Seen;
  const bool stationShows = PortDebug::MapStationPickups();
  for (size_t i = 0; i < count; ++i) {
    const PortMapPickups::Dot& dot = dots[i];
    if (dot.world != worldId) {
      continue;
    }
    const int areaIdx = world.IGetAreaId(dot.area).Value();
    if (seenOnly && !PortMapPickups::Seen(i) &&
        !(stationShows && mwInfo.IsWorldVisible(areaIdx))) {
      continue;
    }
    const bool collected = (tracker != nullptr && tracker[i] == PortMapPickups::kC_Grey) ||
                           mailbox.HasMsg(TEditorId(dot.relay));
    if (collected && tracker == nullptr && kinds == nullptr) {
      continue;
    }
    CColor color = white;
    if (collected) {
      color = CColor(kTrackerColors[PortMapPickups::kC_Grey]).WithAlphaOf(parms.GetAlpha());
    } else if (tracker != nullptr) {
      color = CColor(kTrackerColors[tracker[i]]).WithAlphaOf(parms.GetAlpha());
    }
    bool drawn = false;
    for (int j = 0; j < bfsInfos.size() && !drawn; ++j) {
      drawn = bfsInfos[j].GetAreaIndex() == areaIdx;
    }
    if (!drawn || !GetMapArea(areaIdx)->GetIsVisibleToAutoMapper(mwInfo.IsWorldVisible(areaIdx),
                                                                 mwInfo.IsAreaVisible(areaIdx))) {
      continue;
    }
    if (!setUp) {
      CGX::SetChanCtrl(CGX::Channel0, false, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE,
                       GX_AF_NONE);
      CGraphics::SetTevOp(kTS_Stage0, CGraphics::kEnvPassthru);
      setUp = true;
    }
    const CVector3f pos = CMapArea::GetAreaPostTranslate(world, areaIdx) +
                          CVector3f(dot.pos[0], dot.pos[1], dot.pos[2]);
    gpRender->SetModelMatrix(modelXf * CTransform4f::Translate(pos) * billboard);
    // A dark rim first, so the mark also reads on the pale explored rooms.
    if (collected && kinds != nullptr) {
      for (int pass = 0; pass < 2; ++pass) {
        const float length = pass == 0 ? kRimBarLength : kBarLength;
        const float width = pass == 0 ? kRimBarWidth : kBarWidth;
        for (int bar = 0; bar < 2; ++bar) {
          // Along (1, 1) or (1, -1), turned 45 degrees.
          const float side = bar == 0 ? 1.f : -1.f;
          const float ax = length * kHalfSqrt2;
          const float az = side * length * kHalfSqrt2;
          const float wx = -side * width * kHalfSqrt2;
          const float wz = width * kHalfSqrt2;
          CGraphics::StreamBegin(kP_TriangleStrip);
          CGraphics::StreamColor(pass == 0 ? rim : color);
          // Counter-clockwise in x/z like the dots' fans, or culling drops the bar.
          CGraphics::StreamVertex(-ax + wx, 0.f, -az + wz);
          CGraphics::StreamVertex(-ax - wx, 0.f, -az - wz);
          CGraphics::StreamVertex(ax + wx, 0.f, az + wz);
          CGraphics::StreamVertex(ax - wx, 0.f, az - wz);
          CGraphics::StreamEnd();
        }
      }
      continue;
    }
    const bool ring = kinds != nullptr && kinds[i] == PortMapPickups::kK_Minor;
    for (int pass = 0; pass < (ring ? 3 : 2); ++pass) {
      const float radius = pass == 0 ? kRimRadius : pass == 1 ? kRadius : kHoleRadius;
      CGraphics::StreamBegin(kP_TriangleFan);
      CGraphics::StreamColor(pass == 1 ? color : rim);
      CGraphics::StreamVertex(0.f, 0.f, 0.f);
      for (int k = 0; k <= kSegments; ++k) {
        const float angle = k * (M_2PIF / kSegments);
        CGraphics::StreamVertex(radius * CMath::FastCosR(angle), 0.f,
                                radius * CMath::FastSinR(angle));
      }
      CGraphics::StreamEnd();
    }
  }
  if (setUp) {
    // Put back the state the retail map draws expect.
    CMapArea::CMapAreaSurface::SetupGXMaterial();
  }
}
#endif

void CMapWorld::RecalculateWorldSphere(const CMapWorldInfo& mwInfo, const IWorld& wld) const {
  rstl::vector< CVector2f > coords;
  coords.reserve(x0_areas.size() * 8);
  float zMin = FLT_MAX;
  float zMax = -FLT_MAX;
  for (int i = 0; i < x0_areas.size(); ++i) {
    if (IsMapAreaValid(wld, i, true)) {
      CMapArea* area = GetMapArea(i);
      if (area->GetIsVisibleToAutoMapper(mwInfo.IsWorldVisible(i), mwInfo.IsAreaVisible(i))) {
        CAABox box = area->GetBoundingBox().GetTransformedAABox(area->GetAreaPostTransform(wld, i));
        for (int j = 0; j < 8; ++j) {
          const CVector3f point = box.GetPoint(j);
          coords.push_back(CVector2f(point.GetX(), point.GetY()));
          zMin = rstl::min_val(zMin, point.GetZ());
          zMax = rstl::max_val(zMax, point.GetZ());
        }
      }
    }
  }
  const Circle2 circle = MinCircle(coords.size(), coords.data());
  x3c_worldSphereRadius = circle.GetRadius();
  x30_worldSpherePoint =
      CVector3f(circle.GetCenter().GetX(), circle.GetCenter().GetY(), 0.5f * (zMax + zMin));
  x40_worldSphereHalfDepth = 0.5f * (zMax - zMin);
}

CVector3f CMapWorld::ConstrainToWorldVolume(const CVector3f& point,
                                            const CVector3f& lookVec) const {
  CVector3f result = point;
  if (CMath::AbsF(lookVec.GetZ()) > FLT_EPSILON) {
    float above = point.GetZ() - (x40_worldSphereHalfDepth + x30_worldSpherePoint.GetZ());
    float below = point.GetZ() - (x30_worldSpherePoint.GetZ() - x40_worldSphereHalfDepth);
    if (above > 0.f) {
      result = point + (-above / lookVec.GetZ()) * lookVec;
    } else if (below < 0.f) {
      result = point + (-below / lookVec.GetZ()) * lookVec;
    }
  } else {
    result.SetZ(CMath::Clamp(x30_worldSpherePoint.GetZ() - x40_worldSphereHalfDepth, result.GetZ(),
                             x40_worldSphereHalfDepth + x30_worldSpherePoint.GetZ()));
  }
  const CVector2f delta = CVector2f(point.GetX(), point.GetY()) -
                          CVector2f(x30_worldSpherePoint.GetX(), x30_worldSpherePoint.GetY());
  if (delta.Magnitude() > x3c_worldSphereRadius) {
    const CVector2f offset = delta.AsNormalized() * x3c_worldSphereRadius;
    result.SetX(x30_worldSpherePoint.GetX() + offset.GetX());
    result.SetY(x30_worldSpherePoint.GetY() + offset.GetY());
  }
  return result;
}

void CMapWorld::ClearTraversedFlags() const {
  for (int i = 0; i < x20_traversed.size(); ++i) {
    x20_traversed[i] = false;
  }
}

const CFactoryFnReturn FMapWorldFactory(const SObjectTag& tag, CInputStream& in,
                                  const CVParamTransfer& param) {
  return CFactoryFnReturn(rs_new CMapWorld(in));
}

static bool PointInsideCircle(const CVector2f& point, const Circle2& circle, float& distance) {
  const CVector2f delta = point - circle.GetCenter();
  float distanceSquared = delta.MagSquared();
  distance = distanceSquared - circle.GetRadius();
  return distance <= 0.f;
}

static Circle2 ExactCircle1(const CVector2f& point) {
  Circle2 result;
  result.Center() = point;
  result.Radius() = 0.f;
  return result;
}

static Circle2 ExactCircle2(const CVector2f& a, const CVector2f& b) {
  Circle2 result;
  result.Center() = 0.5f * (a + b);
  const CVector2f delta = b - a;
  result.Radius() = 0.25f * delta.MagSquared();
  return result;
}

static Circle2 ExactCircle3(const CVector2f& a, const CVector2f& b, const CVector2f& c) {
  const CVector2f d1 = b - a;
  const CVector2f d2 = c - a;
  const float x1 = d1.GetX();
  const float y1 = d1.GetY();
  const float x2 = d2.GetX();
  const float y2 = d2.GetY();
  const float halfMag1 = 0.5f * d1.MagSquared();
  const float halfMag2 = 0.5f * d2.MagSquared();
  const float determinant = x1 * y2 - y1 * x2;
  Circle2 result;
  if (CMath::AbsF(determinant) > 0.01f) {
    const float inverse = 1.f / determinant;
    CVector2f offset = CVector2f::Zero();
    offset[0] = inverse * (y2 * halfMag1 - y1 * halfMag2);
    offset[1] = inverse * (x1 * halfMag2 - x2 * halfMag1);
    result.Center() = a + offset;
    result.Radius() = offset.MagSquared();
  } else {
    result.Center() = CVector2f::Zero();
    result.Radius() = FLT_MAX;
  }
  return result;
}

static Circle2 UpdateSupport1(int idx, CVector2f** points, Support& support) {
  Circle2 result = ExactCircle2(*points[support.x4_indices[0]], *points[idx]);
  support.x0_count = 2;
  support.x4_indices[1] = idx;
  return result;
}

static Circle2 UpdateSupport2(int idx, CVector2f** points, Support& support) {
  const CVector2f& a = *points[support.x4_indices[0]];
  const CVector2f& b = *points[support.x4_indices[1]];
  const CVector2f& point = *points[idx];
  Circle2 circles[3];
  float distance;
  int best = -1;
  float minRadius = FLT_MAX;
  circles[0] = ExactCircle2(a, point);
  if (PointInsideCircle(b, circles[0], distance)) {
    minRadius = circles[0].GetRadius();
    best = 0;
  }
  circles[1] = ExactCircle2(b, point);
  if (circles[1].GetRadius() < minRadius && PointInsideCircle(a, circles[1], distance)) {
    best = 1;
  }
  Circle2 result;
  if (best != -1) {
    result = circles[best];
    support.x4_indices[1 - best] = idx;
  } else {
    result = ExactCircle3(a, b, point);
    support.x0_count = 3;
    support.x4_indices[2] = idx;
  }
  return result;
}

static Circle2 UpdateSupport3(int idx, CVector2f** points, Support& support) {
  const CVector2f& a = *points[support.x4_indices[0]];
  const CVector2f& b = *points[support.x4_indices[1]];
  const CVector2f& c = *points[support.x4_indices[2]];
  const CVector2f& point = *points[idx];
  Circle2 circles[6];
  float distance;
  int best = -1;
  int fallback = -1;
  float minRadius = FLT_MAX;
  float minDistance = FLT_MAX;

  circles[0] = ExactCircle2(a, point);
  if (PointInsideCircle(b, circles[0], distance)) {
    if (PointInsideCircle(c, circles[0], distance)) {
      minRadius = circles[0].GetRadius();
      best = 0;
    } else {
      minDistance = distance;
      fallback = 0;
    }
  } else {
    minDistance = distance;
    fallback = 0;
  }

  circles[1] = ExactCircle2(b, point);
  if (circles[1].GetRadius() < minRadius) {
    if (PointInsideCircle(a, circles[1], distance)) {
      if (PointInsideCircle(c, circles[1], distance)) {
        minRadius = circles[1].GetRadius();
        best = 1;
      } else if (distance < minDistance) {
        minDistance = distance;
        fallback = 1;
      }
    } else if (distance < minDistance) {
      minDistance = distance;
      fallback = 1;
    }
  }

  circles[2] = ExactCircle2(c, point);
  if (circles[2].GetRadius() < minRadius) {
    if (PointInsideCircle(a, circles[2], distance)) {
      if (PointInsideCircle(b, circles[2], distance)) {
        minRadius = circles[2].GetRadius();
        best = 2;
      } else if (distance < minDistance) {
        minDistance = distance;
        fallback = 2;
      }
    } else if (distance < minDistance) {
      minDistance = distance;
      fallback = 2;
    }
  }

  circles[3] = ExactCircle3(a, b, point);
  if (circles[3].GetRadius() < minRadius) {
    if (PointInsideCircle(c, circles[3], distance)) {
      minRadius = circles[3].GetRadius();
      best = 3;
    } else if (distance < minDistance) {
      minDistance = distance;
      fallback = 3;
    }
  }

  circles[4] = ExactCircle3(a, c, point);
  if (circles[4].GetRadius() < minRadius) {
    if (PointInsideCircle(b, circles[4], distance)) {
      minRadius = circles[4].GetRadius();
      best = 4;
    } else if (distance < minDistance) {
      minDistance = distance;
      fallback = 4;
    }
  }

  circles[5] = ExactCircle3(b, c, point);
  if (circles[5].GetRadius() < minRadius) {
    if (PointInsideCircle(a, circles[5], distance)) {
      best = 5;
    } else if (distance < minDistance) {
      fallback = 5;
    }
  }

  if (best == -1) {
    best = fallback;
  }
  Circle2 result = circles[best];
  switch (best) {
  case 0:
    support.x0_count = 2;
    support.x4_indices[1] = idx;
    break;
  case 1:
    support.x0_count = 2;
    support.x4_indices[0] = idx;
    break;
  case 2:
    support.x0_count = 2;
    support.x4_indices[0] = support.x4_indices[2];
    support.x4_indices[1] = idx;
    break;
  case 3:
    support.x4_indices[2] = idx;
    break;
  case 4:
    support.x4_indices[1] = idx;
    break;
  case 5:
    support.x4_indices[0] = idx;
    break;
  }
  return result;
}

static Circle2 (*gs_aoUpdate[])(int, CVector2f**, Support&) = {nullptr, UpdateSupport1,
                                                               UpdateSupport2, UpdateSupport3};

static Circle2 MinCircle(int count, const CVector2f* points) {
  Circle2 result;
  if (count >= 1) {
    CVector2f** shuffled = rs_new CVector2f* [count];
    int i;
    for (i = 0; i < count; ++i) {
      shuffled[i] = const_cast< CVector2f* >(&points[i]);
    }
    for (i = count - 1; i > 0; --i) {
      int j = rand() % (i + 1);
      if (j != i) {
        rstl::swap(shuffled[i], shuffled[j]);
      }
    }
    result = ExactCircle1(*shuffled[0]);
    Support support;
    support.x0_count = 1;
    support.x4_indices[0] = 0;
    i = 1;
    while (i < count) {
      if (!support.Contains(i, shuffled)) {
        float distance;
        if (!PointInsideCircle(*shuffled[i], result, distance)) {
          Circle2 next = gs_aoUpdate[support.x0_count](i, shuffled, support);
          if (next.GetRadius() > result.GetRadius()) {
            result = next;
            i = 0;
            continue;
          }
        }
      }
      ++i;
    }
    if (shuffled != nullptr) {
      delete[] shuffled;
    }
  }
  result.Radius() = CMath::SqrtF(result.GetRadius());
  return result;
}

#ifdef TARGET_PC
void CMapWorld::PortRefreshDoors() {
  for (size_t i = 0; i < x0_areas.size(); ++i) {
    CMapAreaData& data = x0_areas[i];
    if (data.IsLoaded()) {
      if (CMapArea* area = data.MapArea()) {
        area->PortSetDoorTypes(data.PortAreaRes());
      }
    }
  }
}
#endif
