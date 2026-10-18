#include "Kyoto/Animation/CSkinRules.hpp"
#include "port_env.h"
#include "Kyoto/Animation/CCharLayoutInfo.hpp"
#include "Kyoto/Animation/CPoseAsTransforms.hpp"
#include "Kyoto/Graphics/CModel.hpp"
#include "dolphin/os/OSCache.h"
#include "rstl/math.hpp"

#ifdef TARGET_PC
#include "Kyoto/Basics/CBasics.hpp"
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <stdlib.h>
#include <string.h>
#include <thread>
#include <vector>
#endif

static int StreamFloatToShort(CInputStream& in) {
  const int result = in.Get< int >();
  if (result == -1) {
    return in.ReadLong();
  }
  uchar junk[780];
  for (int i = 0, iVar2 = 0; i < (result * 3); i += iVar2) {
    iVar2 = rstl::min_val(((result * 3) - i), 192);
    in.Get(junk, iVar2 * 4);
  }
  return result;
}

CSkinRules::CSkinRules(CInputStream& in)
: x0_virtualBones(in)
, x10_vertexCount(StreamFloatToShort(in))
, x14_normalCount(StreamFloatToShort(in)) {

  CModel::AddToTotal(x0_virtualBones.size() * sizeof(CVirtualBone) + sizeof(CSkinRules));
}

CSkinRules::~CSkinRules() {
  CModel::RemoveFromTotal(x0_virtualBones.size() * sizeof(CVirtualBone) + sizeof(CSkinRules));
}

void CSkinRules::BuildAccumulatedTransforms(const CPoseAsTransforms& pose,
                                            const CCharLayoutInfo& layoutInfo) const {
  float pointStorage[100][3];
  CVector3f* points = reinterpret_cast< CVector3f* >(pointStorage);
  CSegId id = pose.GetTransforms().GetFirstElementPresent();
  while (id != CSegId::Null()) {
    const CVector3f& origin = layoutInfo.GetReferenceStanceOffset(id);
    const CVector3f& rotatedOrigin = pose.GetTransformMinusOffset(id) * origin;
    points[id.val()] = pose.GetOffset(id) - rotatedOrigin;
    id = pose.GetTransforms().GetIdAfter(id);
  }

  for (int i = 0; i < x0_virtualBones.size(); ++i) {
    x0_virtualBones[i].BuildAccumulatedTransform(pose, points);
  }
}

void CSkinRules::BuildPoints(volatile void* pipe) const {
  // On the console the write-gather pipe advances itself as vertices are
  // written, so the same `pipe` value is passed to every bone. The PC
  // implementation writes plain floats and cannot advance the caller's pointer,
  // so advance it here.
#ifndef __MWERKS__
  volatile uchar* out = static_cast< volatile uchar* >(pipe);
#endif
  for (int i = 0; i < x0_virtualBones.size(); ++i) {
    int vertexCount = x0_virtualBones[i].GetNumIndices();
    ushort* buffer = nullptr;
    for (int done = 0; done < vertexCount;) {
      const int count = ProcessingPoints(vertexCount - done, &buffer);
#ifdef __MWERKS__
      x0_virtualBones[i].BuildPoints(buffer, pipe, count);
#else
      x0_virtualBones[i].BuildPoints(buffer, out, count);
      out += count * sizeof(CVector3f);
#endif
      done += count;
    }
  }
}

void CSkinRules::BuildNormals(volatile void* pipe) const {
#ifndef __MWERKS__
  volatile uchar* out = static_cast< volatile uchar* >(pipe);
#endif
  for (int i = 0; i < x0_virtualBones.size(); ++i) {
    int vertexCount = x0_virtualBones[i].GetNumIndices();
    ushort* buffer = nullptr;
    for (int done = 0; done < vertexCount;) {
      const int count = ProcessingNormals(vertexCount - done, &buffer);
#ifdef __MWERKS__
      x0_virtualBones[i].BuildNormals(buffer, pipe, count);
#else
      x0_virtualBones[i].BuildNormals(buffer, out, count);
      out += count * sizeof(CVector3f);
#endif
      done += count;
    }
  }
}

void CSkinRules::BuildNormalsFrom(const CVector3f* averageNormals, CVector3f* out) const {
  int offset = 0;
  for (int i = 0; i < x0_virtualBones.size(); ++i) {
    const CVirtualBone& bone = x0_virtualBones[i];
    int count = bone.GetNumIndices();
    bone.BuildNormals(averageNormals + offset, out + offset, count);
    offset += count;
  }
}

#ifdef TARGET_PC
static inline float LoadBigFloat(const uchar* p) {
  float value;
  memcpy(&value, p, sizeof(value));
  return CBasics::SwapBytes(value);
}

static inline void Normalize3(float* v) {
  const float len2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
  if (len2 > 0.f) {
    const float inv = 1.f / std::sqrt(len2);
    v[0] *= inv;
    v[1] *= inv;
    v[2] *= inv;
  }
}

// The sign of dot(cross(n, t), b): the frame's handedness, which the shader reads off B.
static inline float Handedness(const float* n, const float* t, const float* b) {
  const float cx = n[1] * t[2] - n[2] * t[1];
  const float cy = n[2] * t[0] - n[0] * t[2];
  const float cz = n[0] * t[1] - n[1] * t[0];
  return cx * b[0] + cy * b[1] + cz * b[2] >= 0.f ? 1.f : -1.f;
}

// Remastered's skinned vertex shader (e400bbf9, faa085bd) normalises the skinned N and T,
// and its handedness is the authored one: a mirrored bone does not flip it. The skinned
// entry (N, B, T[, B1, T1]) is made so: N and T unit, and B = w * cross(N, T) with the
// source's w, which is all the shader reads of B.
static void FinishTangentFrame(const uchar* src, float* out, int vecs) {
  float in[15];
  for (int k = 0; k < vecs * 3; ++k) {
    in[k] = LoadBigFloat(src + k * 4);
  }
  float* const n = out;
  Normalize3(n);
  for (int f = 1; f + 1 < vecs; f += 2) {
    float* const b = out + f * 3;
    float* const t = out + (f + 1) * 3;
    const float w = Handedness(in, in + (f + 1) * 3, in + f * 3);
    Normalize3(t);
    b[0] = w * (n[1] * t[2] - n[2] * t[1]);
    b[1] = w * (n[2] * t[0] - n[0] * t[2]);
    b[2] = w * (n[0] * t[1] - n[1] * t[0]);
  }
}

// Skins the bones [first, end), whose vertices start at `offset`. Each virtual
// bone owns the next run of vertices, in the same order for the point and
// normal arrays. The expressions match CTransform4f/CMatrix3f's operator*, so
// the output is bit-identical to the retail path.
// `vecs` is the vectors per normal entry (1, or 3 for N, B, T, or 5 with a second frame); all rotate alike.
static void BuildBoneRange(const CVirtualBone* bones, int first, int end, int offset,
                           const uchar* srcPoints, const uchar* srcNormals, float* points,
                           float* normals, int vecs) {
  const uchar* src = srcPoints + offset * 12;
  float* out = points + offset * 3;
  for (int b = first; b < end; ++b) {
    const CTransform4f& xf = bones[b].GetTransform();
    const float m00 = xf.Get00(), m01 = xf.Get01(), m02 = xf.Get02(), m03 = xf.Get03();
    const float m10 = xf.Get10(), m11 = xf.Get11(), m12 = xf.Get12(), m13 = xf.Get13();
    const float m20 = xf.Get20(), m21 = xf.Get21(), m22 = xf.Get22(), m23 = xf.Get23();
    const int count = bones[b].GetNumIndices();
    for (int i = 0; i < count; ++i, src += 12, out += 3) {
      const float x = LoadBigFloat(src), y = LoadBigFloat(src + 4), z = LoadBigFloat(src + 8);
      out[0] = m00 * x + m01 * y + m02 * z + m03;
      out[1] = m10 * x + m11 * y + m12 * z + m13;
      out[2] = m20 * x + m21 * y + m22 * z + m23;
    }
  }

  src = srcNormals + offset * 12 * vecs;
  out = normals + offset * 3 * vecs;
  for (int b = first; b < end; ++b) {
    const CMatrix3f& rot = bones[b].GetRotation();
    const float m00 = rot.Get00(), m01 = rot.Get01(), m02 = rot.Get02();
    const float m10 = rot.Get10(), m11 = rot.Get11(), m12 = rot.Get12();
    const float m20 = rot.Get20(), m21 = rot.Get21(), m22 = rot.Get22();
    const int count = bones[b].GetNumIndices();
    for (int i = 0; i < count; ++i) {
      const uchar* const vsrc = src;
      float* const vout = out;
      for (int v = 0; v < vecs; ++v, src += 12, out += 3) {
        const float x = LoadBigFloat(src), y = LoadBigFloat(src + 4), z = LoadBigFloat(src + 8);
        out[0] = x * m00 + y * m01 + z * m02;
        out[1] = x * m10 + y * m11 + z * m12;
        out[2] = x * m20 + y * m21 + z * m22;
      }
      if (vecs >= 3) {
        FinishTangentFrame(vsrc, vout, vecs);
      }
    }
  }
}

namespace {
// Helper threads for large (mod) models. Run() hands out job indices to the
// helpers and the calling thread and returns when every job has finished.
class SkinPool {
public:
  static SkinPool& Get() {
    // Never destroyed: the helpers sleep until process exit.
    static SkinPool* pool = new SkinPool();
    return *pool;
  }

  int Threads() const { return static_cast< int >(mWorkers.size()) + 1; }

  void Run(int jobs, const std::function< void(int) >& fn) {
    std::lock_guard< std::mutex > runLock(mRunMutex);
    unsigned generation;
    {
      std::lock_guard< std::mutex > lock(mMutex);
      generation = ++mGeneration;
      mFn = &fn;
      mJobs = jobs;
      mPending = jobs;
      mNext.store(static_cast< unsigned long long >(generation) << 32);
    }
    mWake.notify_all();
    Work(generation, fn, jobs);
    std::unique_lock< std::mutex > lock(mMutex);
    mDone.wait(lock, [this] { return mPending == 0; });
  }

private:
  SkinPool() {
    unsigned hw = std::thread::hardware_concurrency();
    const int helpers = hw > 1 ? static_cast< int >(rstl::min_val(hw - 1, 3u)) : 0;
    for (int i = 0; i < helpers; ++i) {
      mWorkers.emplace_back([this] { Loop(); });
      mWorkers.back().detach();
    }
  }

  void Loop() {
    unsigned seen = 0;
    for (;;) {
      unsigned generation;
      const std::function< void(int) >* fn;
      int jobs;
      {
        std::unique_lock< std::mutex > lock(mMutex);
        mWake.wait(lock, [&] { return mGeneration != seen; });
        seen = generation = mGeneration;
        fn = mFn;
        jobs = mJobs;
      }
      Work(generation, *fn, jobs);
    }
  }

  // The counter holds the generation in its high half, so a helper that wakes
  // late never takes a job from a later Run with this Run's function.
  void Work(unsigned generation, const std::function< void(int) >& fn, int jobs) {
    int finished = 0;
    unsigned long long value = mNext.load();
    for (;;) {
      if (static_cast< unsigned >(value >> 32) != generation ||
          static_cast< int >(value & 0xFFFFFFFFu) >= jobs) {
        break;
      }
      if (!mNext.compare_exchange_weak(value, value + 1)) {
        continue;
      }
      fn(static_cast< int >(value & 0xFFFFFFFFu));
      ++finished;
      value = mNext.load();
    }
    if (finished != 0) {
      std::lock_guard< std::mutex > lock(mMutex);
      mPending -= finished;
      if (mPending == 0) {
        mDone.notify_all();
      }
    }
  }

  std::vector< std::thread > mWorkers;
  std::mutex mRunMutex;
  std::mutex mMutex;
  std::condition_variable mWake;
  std::condition_variable mDone;
  const std::function< void(int) >* mFn = nullptr;
  int mJobs = 0;
  int mPending = 0;
  unsigned mGeneration = 0;
  std::atomic< unsigned long long > mNext{0};
};

int ThreadThreshold() {
  // MP_SKIN_THREAD_MIN=<points>: models at least this big are split across threads.
  static const int threshold = port::EnvInt("MP_SKIN_THREAD_MIN", 20000);
  return threshold;
}
} // namespace

void CSkinRules::PortBuildPointsAndNormals(const CModel& model, float* points,
                                           float* normals) const {
  const uchar* srcPoints = static_cast< const uchar* >(model.GetCubeModel()->GetPositions());
  const uchar* srcNormals = static_cast< const uchar* >(model.GetCubeModel()->GetNormals());
  const int nbt = static_cast< int >(model.GetCubeModel()->NormalVecs());
  const CVirtualBone* bones = x0_virtualBones.data();
  const int boneCount = x0_virtualBones.size();

  const int threads = x10_vertexCount >= ThreadThreshold() ? SkinPool::Get().Threads() : 1;
  if (threads <= 1 || boneCount < 2) {
    BuildBoneRange(bones, 0, boneCount, 0, srcPoints, srcNormals, points, normals, nbt);
    return;
  }

  // Split the bones into runs of roughly equal vertex counts.
  const int kMaxChunks = 16;
  int chunkFirst[kMaxChunks + 1];
  int chunkOffset[kMaxChunks + 1];
  const int chunks = rstl::min_val(threads * 2, kMaxChunks);
  int count = 0;
  int offset = 0;
  chunkFirst[0] = 0;
  chunkOffset[0] = 0;
  for (int b = 0; b < boneCount; ++b) {
    offset += bones[b].GetNumIndices();
    if (count + 1 < chunks && offset >= static_cast< long long >(x10_vertexCount) * (count + 1) / chunks) {
      ++count;
      chunkFirst[count] = b + 1;
      chunkOffset[count] = offset;
    }
  }
  ++count;
  chunkFirst[count] = boneCount;
  chunkOffset[count] = offset;

  const std::function< void(int) > job = [&](int c) {
    BuildBoneRange(bones, chunkFirst[c], chunkFirst[c + 1], chunkOffset[c], srcPoints, srcNormals,
                   points, normals, nbt);
  };
  SkinPool::Get().Run(count, job);
}
#endif

const CFactoryFnReturn FSkinRulesFactory(const SObjectTag& tag, CInputStream& in,
                                         const CVParamTransfer&) {
  return rs_new CSkinRules(in);
}

static CSkinRules* sLockedRules = nullptr;
static const CModel* sCurrentTransaction = nullptr;
static int sCurrentPointCount = 0;
static bool sTransferringFirstPage = true;
static int sNextPointStart = 0;
static int sNextNormalStart = 0;
static int sCurrentPoint = 0;
static int sCurrentNormal = 0;
static CVector3f* sCurrentBase = nullptr;
static int sCurrentFirst = 0;
static int sTransactionCount = 0;

void CSkinRules::InitLockedCacheState(const CModel& model) {
  sLockedRules = this;
  sCurrentTransaction = &model;
  sTransferringFirstPage = true;
  sNextPointStart = 0;
  sNextNormalStart = 0;
  sCurrentPoint = 0;
  sCurrentNormal = 0;
  sCurrentBase = nullptr;
  sCurrentFirst = 0;
  sTransactionCount = 0;
  StartNextTransaction();
}

void CSkinRules::StartNextTransaction() {
  uchar* destination = reinterpret_cast< uchar* >(LCGetBase());
  if (!sTransferringFirstPage) {
    destination += 0x1000;
  }

  int count;
  const CVector3f* source;
  if (sNextPointStart != sLockedRules->GetNumPoints()) {
    count = rstl::min_val(336, sLockedRules->GetNumPoints() - sNextPointStart);
    source = static_cast< const CVector3f* >(sCurrentTransaction->GetCubeModel()->GetPositions()) +
             sNextPointStart;
  } else {
    const int normalCount = sLockedRules->GetNumNormals();
    if (normalCount == sNextNormalStart) {
      return;
    }
    count = rstl::min_val(336, normalCount - sNextNormalStart);
    source = static_cast< const CVector3f* >(sCurrentTransaction->GetCubeModel()->GetNormals()) +
             sNextNormalStart;
  }

  LCLoadData(destination, const_cast< CVector3f* >(source), (count * sizeof(CVector3f) + 31) & ~31);
  sCurrentPointCount = count;
  ++sTransactionCount;
  sTransferringFirstPage = !sTransferringFirstPage;
}

static void WaitForQueue() {
  if (!LCQueueLength()) {
    return;
  }
  LCQueueWait(0);
}

int CSkinRules::ProcessingPoints(int count, ushort** buf) {
  if (sCurrentPoint + count > sNextPointStart) {
    if (sCurrentPoint == sNextPointStart) {
      WaitForQueue();
      sCurrentFirst = sNextPointStart;
      sCurrentBase = reinterpret_cast< CVector3f* >(LCGetBase());
      if (sTransferringFirstPage) {
        sCurrentBase = reinterpret_cast< CVector3f* >(static_cast< uchar* >(LCGetBase()) + 0x1000);
      }

      sNextPointStart += sCurrentPointCount;
      StartNextTransaction();
    }

    int c = rstl::min_val(sNextPointStart - sCurrentPoint, count);
    *buf = reinterpret_cast< ushort* >(sCurrentBase + (sCurrentPoint - sCurrentFirst));
    sCurrentPoint += c;
    return c;
  } else {
    *buf = reinterpret_cast< ushort* >(sCurrentBase + (sCurrentPoint - sCurrentFirst));
    sCurrentPoint += count;
    return count;
  }
}

int CSkinRules::ProcessingNormals(int count, ushort** buf) {
  if (sCurrentNormal + count > sNextNormalStart) {
    if (sCurrentNormal == sNextNormalStart) {
      WaitForQueue();
      sCurrentFirst = sNextNormalStart;
      sCurrentBase = reinterpret_cast< CVector3f* >(LCGetBase());
      if (sTransferringFirstPage) {
        sCurrentBase = reinterpret_cast< CVector3f* >(static_cast< uchar* >(LCGetBase()) + 0x1000);
      }

      sNextNormalStart += sCurrentPointCount;
      StartNextTransaction();
    }

    int c = rstl::min_val(sNextNormalStart - sCurrentNormal, count);
    *buf = reinterpret_cast< ushort* >(sCurrentBase + (sCurrentNormal - sCurrentFirst));
    sCurrentNormal += c;
    return c;
  } else {
    *buf = reinterpret_cast< ushort* >(sCurrentBase + (sCurrentNormal - sCurrentFirst));
    sCurrentNormal += count;
    return count;
  }
}
