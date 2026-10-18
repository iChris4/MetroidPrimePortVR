#include "Kyoto/Animation/CSkinnedModel.hpp"
#include "port_env.h"

#include "Kyoto/Alloc/CCircularBuffer.hpp"
#include "Kyoto/Alloc/CMemory.hpp"
#include "Kyoto/Animation/CSkinRules.hpp"
#include "Kyoto/Animation/CVertexMorphEffect.hpp"
#include "Kyoto/Graphics/CGraphics.hpp"
#include "Kyoto/Graphics/CModel.hpp"
#include "Kyoto/Graphics/CModelFlags.hpp"

#include <dolphin/PPCArch.h>
#include <dolphin/gx.h>
#include <dolphin/os.h>
#include <dolphin/os/OSCache.h>

#include <rstl/list.hpp>
#include <rstl/optional_object.hpp>

#ifdef TARGET_PC
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// Port: MP_SKIN_LEGACY=1 runs the retail locked-cache path, MP_SKIN_VERIFY=1
// runs both and reports any difference, MP_SKIN_STATS=1 logs skinning time.
namespace PortSkin {
typedef std::chrono::steady_clock Clock;

static bool Flag(const char* name) { return port::EnvFlag(name); }
bool Legacy() {
  static const bool legacy = Flag("MP_SKIN_LEGACY");
  return legacy;
}
bool Verify() {
  static const bool verify = Flag("MP_SKIN_VERIFY");
  return verify;
}
static bool Stats() {
  static const bool stats = Flag("MP_SKIN_STATS");
  return stats;
}
Clock::time_point Now() { return Stats() ? Clock::now() : Clock::time_point(); }

void Record(int points, Clock::time_point start) {
  if (!Stats()) {
    return;
  }
  static Clock::time_point windowStart = Clock::now();
  static double seconds = 0.0;
  static long long calls = 0;
  static long long totalPoints = 0;
  static int maxPoints = 0;
  const Clock::time_point now = Clock::now();
  seconds += std::chrono::duration< double >(now - start).count();
  ++calls;
  totalPoints += points;
  if (points > maxPoints) {
    maxPoints = points;
  }
  const double window = std::chrono::duration< double >(now - windowStart).count();
  if (window >= 5.0) {
    std::fprintf(stderr,
                 "[skin] %s: %.2f ms/s skinning, %lld models, %.0f points/s, largest %d, %.3f us "
                 "per 1k points\n",
                 Legacy() ? "legacy" : "fast", seconds * 1000.0 / window, calls,
                 totalPoints / window, maxPoints,
                 totalPoints ? seconds * 1e6 / (totalPoints / 1000.0) : 0.0);
    windowStart = now;
    seconds = 0.0;
    calls = totalPoints = 0;
    maxPoints = 0;
  }
}
} // namespace PortSkin
#endif

CSkinnedModel::TPointGenFunc CSkinnedModel::sPointGen;
void* CSkinnedModel::sPointGenData;

struct SSkinnedAllocation {
  SSkinnedAllocation(void* ptr, int w1, ushort w2) : x0_ptr(ptr), x4_unk1(w1), x8_unk2(w2) {}

  void* x0_ptr;
  int x4_unk1;
  ushort x8_unk2;
};

namespace Skinning {
#if VERSION >= VERSION_GM8P_00 && VERSION != VERSION_GM8E_02
static void* sStaticSkinningData = nullptr;
static int sStaticSkinningDataSize = 0x80000;
#endif
static ushort skCurrentToken = 0;
static int sNumSkinnedObjects = 0;
static bool sSkinningInitialized = false;
#if VERSION < VERSION_GM8P_00 || VERSION == VERSION_GM8E_02
#ifdef TARGET_PC
// Port: every skinned draw takes its vertices from this ring until the GPU
// command stream has consumed them. Retail's 512 KB caps a model at ~21k
// vertices (EnsureAllocation spins forever past that), too small for mods.
static char sStaticSkinningData[32 * 1024 * 1024] ATTRIBUTE_ALIGN(32);
#else
static char sStaticSkinningData[0x80000] ATTRIBUTE_ALIGN(32);
#endif
#endif
static rstl::optional_object< CCircularBuffer > sSkinningBuffer;
static rstl::list< SSkinnedAllocation > sAllocations;
static bool sbDumpedSpinLockMessage = false;

void AddSkinnedRef();
void DelSkinnedRef();
#if VERSION >= VERSION_GM8P_00 && VERSION != VERSION_GM8E_02
void SetSkinningBuffer(void* buffer, int size);
#endif
} // namespace Skinning

#if VERSION >= VERSION_GM8P_00 && VERSION != VERSION_GM8E_02
void Skinning::SetSkinningBuffer(void* buffer, int size) {
  sStaticSkinningDataSize = size;
  sAllocations.clear();
  sStaticSkinningData = buffer;
  if (buffer != nullptr && sNumSkinnedObjects != 0) {
    sSkinningBuffer = CCircularBuffer(buffer, sStaticSkinningDataSize);
  }
}
#endif

void Skinning::AddSkinnedRef() {
  if (!sSkinningInitialized) {
    GXSetDrawSync(0xFFFF);
    while (GXReadDrawSync() != 0xFFFF) {
    }
    skCurrentToken = 1;
    sSkinningInitialized = true;
  }

#if VERSION >= VERSION_GM8P_00 && VERSION != VERSION_GM8E_02
  if (sNumSkinnedObjects++ == 0) {
    sSkinningBuffer = CCircularBuffer(sStaticSkinningData, sStaticSkinningDataSize);
  }
#else
  if (sNumSkinnedObjects == 0) {
    sSkinningBuffer = CCircularBuffer(sStaticSkinningData, sizeof(sStaticSkinningData));
  }
  ++sNumSkinnedObjects;
#endif
}

void Skinning::DelSkinnedRef() {
  --sNumSkinnedObjects;
  if (sNumSkinnedObjects == 0) {
    sSkinningBuffer.clear();
    sAllocations.clear();
  }
}

#if VERSION >= VERSION_GM8P_00 && VERSION != VERSION_GM8E_02
void CSkinnedModel::SetSkinningBuffer(void* buffer, int size) {
  Skinning::SetSkinningBuffer(buffer, size);
}
#endif

CSkinnedModel::CSkinnedModel(const TLockedToken< CModel >& model,
                             const TLockedToken< CSkinRules >& skinRules,
                             const TLockedToken< CCharLayoutInfo >& layoutInfo,
                             EDataOwnership ownership)
: x4_model(model)
, x10_skinRules(skinRules)
, x1c_layoutInfo(layoutInfo)
, x28_vertWorkspace()
, x30_normalWorkspace()
, x38_owned(ownership == kDO_Owned)
, x39_disableWorkspaces(false) {
  Construct();
}

CSkinnedModel::CSkinnedModel(const CSkinnedModel& other)
: x4_model(other.x4_model)
, x10_skinRules(other.x10_skinRules)
, x1c_layoutInfo(other.x1c_layoutInfo)
, x28_vertWorkspace()
, x30_normalWorkspace()
, x38_owned(other.x38_owned)
, x39_disableWorkspaces(false) {
  Construct();
}

CSkinnedModel::~CSkinnedModel() { Skinning::DelSkinnedRef(); }

#ifdef TARGET_PC
// Bytes per skinned normal: nine floats (N, B, T) when the model has NBT normals, fifteen with a second frame.
static uint NormalStride(const CModel& model) {
  return model.GetCubeModel()->NormalVecs() * 12;
}
#else
static uint NormalStride(const CModel&) { return 12; }
#endif

void CSkinnedModel::Construct() {
  Skinning::AddSkinnedRef();
  if (!x38_owned) {
    // `Calculate` writes points and then normals into one contiguous buffer, so
    // the normal workspace must alias the tail of the vertex workspace.
    const uint vertexCount = x10_skinRules->GetNumPoints();
    const uint normalCount = x10_skinRules->GetNumNormals();
    const uint vertSize = (vertexCount * 12 + 31) & ~31u;
    const uint normSize = (normalCount * NormalStride(**x4_model) + 31) & ~31u;
    float* ptr = rs_new float[(vertSize + normSize) / sizeof(float)];
    x28_vertWorkspace = rstl::auto_ptr< float[] >(ptr);
    x30_normalWorkspace = rstl::auto_ptr< float[] >(
        reinterpret_cast< float* >(reinterpret_cast< uchar* >(ptr) + vertSize));
    x30_normalWorkspace.release();
  }
  if (x10_skinRules->GetNumVirtualBones() == 1) {
    x39_disableWorkspaces = true;
  }
}

void CSkinnedModel::Draw(const CModelFlags& flags) const {
  CGraphics::sRenderState.SetSkinnedArraySizes(
      x10_skinRules->GetNumPoints() * 12,
      x10_skinRules->GetNumNormals() * NormalStride(**x4_model), NormalStride(**x4_model));
  if (x39_disableWorkspaces) {
    CTransform4f saved(CGraphics::GetModelMatrix());
    CGraphics::SetModelMatrix(saved * x10_skinRules->GetVirtualBones()[0].GetTransform());
    x4_model->Draw(flags);
    CGraphics::SetModelMatrix(saved);
  } else if (x28_vertWorkspace.null()) {
    x4_model->Draw(flags);
  } else {
    x4_model->Draw(x28_vertWorkspace.get(), x30_normalWorkspace.get(), flags);
    PostDrawFunc();
  }
}

void CSkinnedModel::Draw(const TDrawFunc func, void* data) {
  CGraphics::sRenderState.SetSkinnedArraySizes(
      x10_skinRules->GetNumPoints() * 12,
      x10_skinRules->GetNumNormals() * NormalStride(**x4_model), NormalStride(**x4_model));
  if (x39_disableWorkspaces) {
    CTransform4f saved(CGraphics::GetModelMatrix());
    CGraphics::SetModelMatrix(saved * x10_skinRules->GetVirtualBones()[0].GetTransform());
    Draw(func, x4_model->GetPositions(), x4_model->GetNormals(), data);
    CGraphics::SetModelMatrix(saved);
  } else if (x28_vertWorkspace.null()) {
    Draw(func, x4_model->GetPositions(), x4_model->GetNormals(), data);
  } else {
    func(x28_vertWorkspace.get(), x30_normalWorkspace.get(), data);
    uint vertSize = (x10_skinRules->GetNumPoints() * 12 + 31) & ~31u;
    DCFlushRangeNoSync(x28_vertWorkspace.get(), vertSize);
    uint normSize = (x10_skinRules->GetNumNormals() * NormalStride(**x4_model) + 31) & ~31u;
    DCFlushRangeNoSync(x30_normalWorkspace.get(), normSize);
    PPCSync();
    PostDrawFunc();
  }
}

void CSkinnedModel::Draw(const float* positions, const float* normals,
                         const CModelFlags& flags) const {
  CGraphics::sRenderState.SetSkinnedArraySizes(
      x10_skinRules->GetNumPoints() * 12,
      x10_skinRules->GetNumNormals() * NormalStride(**x4_model), NormalStride(**x4_model));
  x4_model->Draw(positions, normals, flags);
  PostDrawFunc();
}

void CSkinnedModel::Calculate(const CPoseAsTransforms& pose,
                              const rstl::optional_object< CVertexMorphEffect >& morphEffect,
                              const float* averagedNormals, float* workVerts) {
  size_t alignedNormSize = 0;
  size_t alignedVertSize = 0;
  size_t totalSize = 0;
  size_t vertSize = x10_skinRules->GetNumPoints() * sizeof(CVector3f);
  size_t normSize = x10_skinRules->GetNumNormals() * NormalStride(**x4_model);
  float* verts;

  if (workVerts != nullptr) {
    verts = workVerts;
  } else {
    if (x39_disableWorkspaces) {
      x10_skinRules->BuildAccumulatedTransforms(pose, **x1c_layoutInfo);
      return;
    }
    AllocateStorage();
    verts = x28_vertWorkspace.get();
  }

  alignedNormSize = ((normSize + 31) & ~31u);
  alignedVertSize = ((vertSize + 31) & ~31u);
  totalSize = alignedVertSize + alignedNormSize;

  DCFlushRange(verts, totalSize);
  BOOL interruptState = OSDisableInterrupts();
  volatile void* pipe = GXRedirectWriteGatherPipe(verts);

#ifdef TARGET_PC
  const PortSkin::Clock::time_point skinStart = PortSkin::Now();
  // The locked-cache path knows only 12-byte normals; NBT models always skin the fast way.
  const bool legacy = PortSkin::Legacy() && NormalStride(**x4_model) == 12;
  if (legacy) {
    x10_skinRules->InitLockedCacheState(**x4_model);
  }
#else
  x10_skinRules->InitLockedCacheState(**x4_model);
#endif
  x10_skinRules->BuildAccumulatedTransforms(pose, **x1c_layoutInfo);
#ifdef __MWERKS__
  x10_skinRules->BuildPoints(pipe);

  int numWords = x10_skinRules->GetNumPoints() * 3;
  int padWords = ((numWords + 7) & ~7) - numWords;
  for (int i = 0; i < padWords; i++) {
    *reinterpret_cast< volatile u32* >(pipe) = 0;
  }

  x10_skinRules->BuildNormals(pipe);
#else
  if (legacy) {
    PortBuildLegacy(pipe);
  } else {
    float* normals = reinterpret_cast< float* >(reinterpret_cast< uchar* >(verts) + alignedVertSize);
    x10_skinRules->PortBuildPointsAndNormals(**x4_model, verts, normals);
    memset(reinterpret_cast< uchar* >(verts) + vertSize, 0, alignedVertSize - vertSize);
    if (PortSkin::Verify() && !legacy && NormalStride(**x4_model) == 12) {
      PortVerify(verts, alignedVertSize + normSize);
    }
  }
  PortSkin::Record(x10_skinRules->GetNumPoints(), skinStart);
#endif
  GXRestoreWriteGatherPipe();
  OSRestoreInterrupts(interruptState);

  if (morphEffect.valid()) {
    (*morphEffect)
        .MorphVertices(reinterpret_cast< CVector3f* >(verts),
                       reinterpret_cast< const CVector3f* >(averagedNormals), x10_skinRules, pose,
                       x10_skinRules->GetNumPoints());
    DCFlushRange(verts, alignedVertSize);
  }

  if (sPointGen != nullptr) {
    const CVector3f* positions = reinterpret_cast< const CVector3f* >(verts);
    sPointGen(sPointGenData, positions, positions + x10_skinRules->GetNumPoints(),
              x10_skinRules->GetNumPoints());
    DCInvalidateRange(verts, totalSize);
  }
}

#ifdef TARGET_PC
void CSkinnedModel::PortBuildLegacy(volatile void* pipe) const {
  // The console's write-gather pipe advances as data is written; the PC buffers
  // are plain memory, so the write cursor has to be advanced explicitly.
  volatile uchar* writePtr = static_cast< volatile uchar* >(pipe);
  x10_skinRules->BuildPoints(writePtr);

  const int numWords = x10_skinRules->GetNumPoints() * 3;
  writePtr += numWords * sizeof(u32);
  const int padWords = ((numWords + 7) & ~7) - numWords;
  for (int i = 0; i < padWords; i++) {
    *reinterpret_cast< volatile u32* >(writePtr) = 0;
    writePtr += sizeof(u32);
  }

  x10_skinRules->BuildNormals(writePtr);
}

void CSkinnedModel::PortVerify(const float* verts, size_t size) const {
  static std::vector< float > scratch;
  static int reports = 0;
  scratch.assign(size / sizeof(float), 0.f);
  x10_skinRules->InitLockedCacheState(**x4_model);
  PortBuildLegacy(scratch.data());
  if (memcmp(scratch.data(), verts, size) == 0 || reports >= 20) {
    return;
  }
  size_t diffs = 0;
  float maxDiff = 0.f;
  for (size_t i = 0; i < scratch.size(); ++i) {
    if (memcmp(&scratch[i], &verts[i], sizeof(float)) != 0) {
      ++diffs;
      maxDiff = std::fmax(maxDiff, std::fabs(scratch[i] - verts[i]));
    }
  }
  ++reports;
  std::fprintf(stderr, "[skin] verify: %zu of %zu floats differ (max %g), %d points\n", diffs,
               scratch.size(), maxDiff, x10_skinRules->GetNumPoints());
}
#endif

void CSkinnedModel::CalculateDefault() {
  x28_vertWorkspace = rstl::auto_ptr< float[] >();
  x30_normalWorkspace = rstl::auto_ptr< float[] >();
}

void CSkinnedModel::TickAllocations() {
  int syncVal = GXReadDrawSync();
  if (syncVal > static_cast< int >(Skinning::skCurrentToken)) {
    syncVal -= 0x10000;
  }
  while (Skinning::sAllocations.size() != 0) {
    SSkinnedAllocation& front = Skinning::sAllocations.front();
    int tokenVal = static_cast< int >(front.x8_unk2);
    if (tokenVal > static_cast< int >(Skinning::skCurrentToken)) {
      tokenVal -= 0x10000;
    }
    if (syncVal < tokenVal)
      break;
    Skinning::sSkinningBuffer->Free(front.x0_ptr, front.x4_unk1);
    Skinning::sAllocations.pop_front();
  }
}

void* CSkinnedModel::EnsureAllocation(int size) {
  size = (size + 31) & ~31;
  void* ptr = Skinning::sSkinningBuffer->Alloc(size);
  if (ptr == nullptr && !Skinning::sbDumpedSpinLockMessage) {
    Skinning::sSkinningBuffer->GetAllocatedAmount();
    Skinning::sbDumpedSpinLockMessage = true;
  }
  s32 startTick = OSGetTick();
  while (ptr == nullptr) {
    TickAllocations();
    ptr = Skinning::sSkinningBuffer->Alloc(size);
    if (ptr == nullptr) {
      s32 currentTick = OSGetTick();
      if (OSTicksToMilliseconds(static_cast< uint >(currentTick - startTick)) > 60) {
        GXReadDrawSync();
        for (AUTO(it, Skinning::sAllocations.begin()); it != Skinning::sAllocations.end(); ++it) {
        }
        startTick = currentTick;
        GXSetDrawSync(Skinning::skCurrentToken);
        ++Skinning::skCurrentToken;
      }
    }
  }
  Skinning::sAllocations.push_back(SSkinnedAllocation(ptr, size, Skinning::skCurrentToken));
  return ptr;
}

void CSkinnedModel::AllocateStorage() {
  if (x38_owned && (x28_vertWorkspace.null() || x30_normalWorkspace.null())) {
    int vertexCount = x10_skinRules->GetNumPoints();
    int normalCount = x10_skinRules->GetNumNormals();
    TickAllocations();
    int normSize = (normalCount * NormalStride(**x4_model) + 31) & ~31;
    int vertSize = (vertexCount * 12 + 31) & ~31;
    int totalSize = vertSize + normSize + 32;
    void* ptr = EnsureAllocation(totalSize);
    if (ptr == Skinning::sStaticSkinningData) {
      GXInvalidateVtxCache();
    }
    x28_vertWorkspace = rstl::auto_ptr< float[] >(static_cast< float* >(ptr));
    x30_normalWorkspace =
        rstl::auto_ptr< float[] >(reinterpret_cast< float* >(static_cast< char* >(ptr) + vertSize));
    x28_vertWorkspace.release();
    x30_normalWorkspace.release();
  }
}

void CSkinnedModel::PostDrawFunc() const {
  if (x38_owned && !x28_vertWorkspace.null()) {
    x28_vertWorkspace = rstl::auto_ptr< float[] >();
    x30_normalWorkspace = rstl::auto_ptr< float[] >();
    GXSetDrawSync(Skinning::skCurrentToken);
    ++Skinning::skCurrentToken;
  }
}

void CSkinnedModel::AddDummySkinnedModelRef() { Skinning::AddSkinnedRef(); }

void CSkinnedModel::RemoveDummySkinnedModelRef() { Skinning::DelSkinnedRef(); }

void CSkinnedModel::SetPointGeneratorFunc(void* data, void (*func)(void*, const CVector3f*,
                                                                   const CVector3f*, int)) {
  sPointGen = func;
  sPointGenData = data;
}

void CSkinnedModel::ClearPointGeneratorFunc() { sPointGen = nullptr; }

float* CSkinnedModel::AllocateNewWorkspace(float** vertOut) {
  const CSkinRules* skinRules = *x10_skinRules;
  int normalCount = skinRules->GetNumNormals();
  int vertexCount = skinRules->GetNumPoints();
  int alignedNormSize = (normalCount * NormalStride(**x4_model) + 31) & ~31;
  int alignedVertSize = (vertexCount * 12 + 31) & ~31;
  int vertSize = vertexCount * 12;
  float* ptr = static_cast< float* >(
      CMemory::Alloc(((vertSize + 31) & ~31) + alignedNormSize, IAllocator::kHI_RoundUpLen));
  if (vertOut != nullptr) {
    // `Calculate` writes points first and then normals, so the normal workspace
    // must follow the aligned point data.
    *vertOut = reinterpret_cast< float* >(reinterpret_cast< char* >(ptr) + alignedVertSize);
  }
  return ptr;
}
