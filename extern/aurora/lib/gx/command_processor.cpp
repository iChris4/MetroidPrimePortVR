#include "command_processor.hpp"
#include "../gfx/perf_counters.hpp"
#include "../gfx/frame.hpp"
#include "../gfx/hash.hpp"
#include "../gfx/resource_cache.hpp"

#include <absl/container/flat_hash_map.h>

#include <atomic>

#include <cstring>

#include "../gfx/bloom.hpp"
#include "../gfx/volfog.hpp"
#include "../gfx/thermal.hpp"
#include "../gfx/xray.hpp"
#include "../gfx/depth_peek.hpp"
#include "geometry_cache.hpp"
#include "native_vertex.hpp"
#include "../gfx/probe.hpp"
#include "../gfx/shadow.hpp"
#include "../gfx/actor_shadow.hpp"
#include "../gfx/hash.hpp"
#include "../gfx/pipeline_cache.hpp"
#include "../gfx/recording.hpp"
#include "../gfx/stereo_shadow.hpp"
#include "../internal.hpp"
#include "dolphin/gd/GDGeometry.h"
#include "dolphin/gx/GXAurora.h"
#include "gx.hpp"
#include "pipeline.hpp"
#include "regs.hpp"
#include "resident.hpp"
#include "shader_info.hpp"
#include "texture.hpp"

#include <absl/container/flat_hash_map.h>
#include <tracy/Tracy.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>
#include <memory>
#include <optional>
#include <vector>

namespace aurora::gx::fifo {
namespace {
constexpr Module Log{"aurora::gx::fifo"};

// The triangle-list indices a primitive takes: its vertices as GX_TRIANGLES
// for a strip, fan or quads, and those indices written out.
u32 list_index_count(GXPrimitive prim, u16 vtxCount) noexcept {
  switch (prim) {
  case GX_QUADS:
    return (vtxCount / 4u) * 6u;
  case GX_TRIANGLES:
    return vtxCount / 3u * 3u;
  case GX_TRIANGLEFAN:
  case GX_TRIANGLESTRIP:
    return vtxCount < 3 ? 0 : (vtxCount - 3u) * 3u + 3u;
  case GX_LINES:
  case GX_LINESTRIP:
  case GX_POINTS:
    return 6;
  default:
    UNLIKELY FATAL("unsupported primitive type {}", static_cast<u32>(prim));
  }
  return 0;
}

void write_list_indices(u16* out, GXPrimitive prim, u16 vtxStart, u16 vtxCount) noexcept {
  switch (prim) {
  case GX_QUADS:
    for (u32 v = 0; v + 4 <= vtxCount; v += 4) {
      const u16 idx0 = static_cast<u16>(vtxStart + v);
      *out++ = idx0;
      *out++ = static_cast<u16>(idx0 + 1);
      *out++ = static_cast<u16>(idx0 + 2);
      *out++ = static_cast<u16>(idx0 + 2);
      *out++ = static_cast<u16>(idx0 + 3);
      *out++ = idx0;
    }
    break;
  case GX_TRIANGLES:
    for (u32 v = 0; v < vtxCount; ++v) {
      *out++ = static_cast<u16>(vtxStart + v);
    }
    break;
  case GX_TRIANGLEFAN:
    for (u32 v = 0; v < vtxCount; ++v) {
      const u16 idx = static_cast<u16>(vtxStart + v);
      if (v < 3) {
        *out++ = idx;
        continue;
      }
      *out++ = vtxStart;
      *out++ = static_cast<u16>(idx - 1);
      *out++ = idx;
    }
    break;
  case GX_TRIANGLESTRIP:
    for (u32 v = 0; v < vtxCount; ++v) {
      const u16 idx = static_cast<u16>(vtxStart + v);
      if (v < 3) {
        *out++ = idx;
        continue;
      }
      if ((v & 1) == 0) {
        *out++ = static_cast<u16>(idx - 2);
        *out++ = static_cast<u16>(idx - 1);
      } else {
        *out++ = static_cast<u16>(idx - 1);
        *out++ = static_cast<u16>(idx - 2);
      }
      *out++ = idx;
    }
    break;
  case GX_LINES:
  case GX_LINESTRIP:
  case GX_POINTS:
    out[0] = 0;
    out[1] = 1;
    out[2] = 3;
    out[3] = 3;
    out[4] = 2;
    out[5] = 0;
    break;
  default:
    break;
  }
}

// Sized once and written directly (upstream 2f044fe6 did the same): this runs for nearly every draw.
u32 prepare_idx_buffer(ByteBuffer& buf, GXPrimitive prim, u16 vtxStart, u16 vtxCount) noexcept {
  const u32 count = list_index_count(prim, vtxCount);
  write_list_indices(reinterpret_cast<u16*>(buf.append_uninitialized(count * sizeof(u16))), prim, vtxStart, vtxCount);
  return count;
}

// GX FIFO opcodes - use CP_ prefix to avoid clashing with GXCommandList.h macros
constexpr u8 CP_CMD_NOP = GX_NOP;
constexpr u8 CP_CMD_LOAD_CP_REG = GX_LOAD_CP_REG;
constexpr u8 CP_CMD_LOAD_XF_REG = GX_LOAD_XF_REG;
constexpr u8 CP_CMD_LOAD_INDX_A = GX_LOAD_INDX_A;
constexpr u8 CP_CMD_LOAD_INDX_B = GX_LOAD_INDX_B;
constexpr u8 CP_CMD_LOAD_INDX_C = GX_LOAD_INDX_C;
constexpr u8 CP_CMD_LOAD_INDX_D = GX_LOAD_INDX_D;
constexpr u8 CP_CMD_CALL_DL = GX_CMD_CALL_DL;
constexpr u8 CP_CMD_INVAL_VTX = GX_CMD_INVL_VC;
constexpr u8 CP_CMD_LOAD_BP_REG = GX_LOAD_BP_REG & GX_OPCODE_MASK;

// Primitive type mask
constexpr u8 CP_OPCODE_MASK = GX_OPCODE_MASK;
constexpr u8 CP_VAT_MASK = GX_VAT_MASK;

struct FogRangeLutKey {
  std::array<u16, 10> rangeK;
  f32 rangeCenter;
  f32 renderWidth;
  u32 targetWidth;

  bool operator==(const FogRangeLutKey&) const = default;
};

struct FogRangeLutEntry {
  FogRangeLutKey key;
  std::vector<f32> factors;
};

constexpr size_t MaxFogRangeLuts = 32;
std::vector<FogRangeLutEntry> sFogRangeLuts;

struct PipelineMemo {
  ShaderInfo shaderInfo{};
  gfx::PipelineRef shadowPipelineRef{};
  gfx::PipelineRef cutPipelineRef{};
  bool shadowCaster = false;
  bool cutCaster = false;
};
// Keyed by pipeline ref (the config's hash); entries stay valid until the config's pipeline is
// dropped, which gfx::touch_pipeline reports.
absl::flat_hash_map<gfx::PipelineRef, PipelineMemo> sPipelineMemo;

// aurora_gpu_selftest_run sets blocking_pipelines() for its passes, so a draw of those passes is not
// skipped for a pipeline that is still compiling (gx.cpp).

struct DrawCache {
  PipelineConfig config{};
  ShaderInfo shaderInfo{};
  gfx::PipelineRef pipelineRef{};
  // The sun's shadow-map pipeline, for a draw that casts (GXPortSetShadowCaster)
  gfx::PipelineRef shadowPipelineRef{};
  bool shadowCaster = false;
  // A cutout caster's own texture group: the receiver layout holds the shadow map being drawn into.
  gfx::PipelineRef cutPipelineRef{};
  bool cutCaster = false;
  // GX_AURORA_PORT_DEPTH_PREPASS pass 1 and a draw that can't go in it (depth_prepass_eligible)
  bool prepassSkip = false;
  GXBindGroups cutBindGroups{};
  GXBindGroups bindGroups{};
  uint64_t bindGeneration = 0;
  GXVtxFmt fmt = GX_MAX_VTXFMT;
  u8 lineMode = 0;
  bool hasPipeline = false;
  gfx::Range uniformRange{};
  ByteBuffer uniformBytes;
  u8 uniformRoute = 0;
  gfx::stereo_replay::HeadLockedPlane uniformPlane{};
  gfx::StereoPlacement3D uniformPlacement{};
  gfx::StereoScreenTexMtx uniformScreenTexMtx{};
  std::array<uint32_t, 2> stereoUniformOffsets{UINT32_MAX, UINT32_MAX};
  std::array<gfx::BindGroupRef, 2> stereoBindGroups{};
  uint64_t stereoEpoch = 0;
  // Multiview stereo replay (gfx/stereo_multiview.hpp): the pipelines drawing both
  // views, one per MultiviewMode, made for the current config when a multiview
  // frame first needs one (bit `mode` of the mask); the mode the staged uniform
  // needs (gfx::stereo_multiview_mode); the stereo bind groups are then
  // [multiview bind group, 0].
  std::array<gfx::PipelineRef, 4> multiviewPipelineRefs{};
  u8 multiviewPipelineMask = 0;
  u8 multiviewMode = MultiviewNone;
  bool stereoBindGroupsMultiview = false;
  gfx::Range fogRange{};
  FogRangeLutKey fogRangeKey{};
  bool hasFogRange = false;
  GXVtxFmt lastDrawFmt = GX_MAX_VTXFMT;
};
DrawCache sDrawCache;

// Indexed vertex attributes resolved on the CPU (GXState::deindexVertices): the
// plan for the current vertex format and arrays, mirroring populate_pipeline_config's
// resolved layout, and the FIFO records rewritten with the array elements inline.
std::atomic<bool> sDeindexRequested{false};
struct DeindexAttr {
  const u8* data = nullptr; // the array; null when none is set
  u32 size = 0;
  u32 stride = 0;
  u32 base = 0;      // GX_AURORA_LOAD_ARRAY_BASE_INDEX
  u16 srcOffset = 0; // of the index, or the direct data, in the FIFO record
  u16 dstOffset = 0; // in the resolved record
  u8 bytes = 0;      // per index: the element, or one slice of it (NBT3)
  u8 indexSize = 0;  // 0: direct data, copied as it is
  u8 indices = 1;    // 3 for GX_NRM_NBT3, one per slice
};
struct DeindexPlan {
  GXVtxFmt fmt = GX_MAX_VTXFMT;
  u32 srcStride = 0;
  u32 dstStride = 0;
  bool active = false; // any indexed attribute
  u32 count = 0;
  std::array<DeindexAttr, GX_VA_TEX7 + 1> attrs{};
};
DeindexPlan sDeindexPlan;
u32 sDeindexOutOfRange = 0;

// Rebuilt after any state change (the dirty bits clear with the first draw, so
// the strips that follow reuse it).
const DeindexPlan& deindex_plan(GXVtxFmt fmt) noexcept {
  auto& plan = sDeindexPlan;
  if (plan.fmt == fmt && g_gxState.dirty == 0) {
    return plan;
  }
  plan.fmt = fmt;
  plan.count = 0;
  plan.active = false;
  const auto& vtxFmt = g_gxState.vtxFmts[fmt];
  // gx.hpp deindexed_attr_align: the layout populate_pipeline_config describes
  const bool aligned = deindexed_layout();
  u32 src = 0;
  u32 dst = 0;
  for (int i = GX_VA_PNMTXIDX; i <= GX_VA_TEX7; ++i) {
    const auto attr = static_cast<GXAttr>(i);
    const auto type = g_gxState.vtxDesc[i];
    if (type == GX_NONE) {
      continue;
    }
    if (aligned) {
      dst = AURORA_ALIGN(dst, deindexed_attr_align(attr));
    }
    const auto& attrFmt = vtxFmt.attrs[i];
    const u32 compSize = comp_type_size(attr, attrFmt.type);
    const u32 cnt = comp_cnt_count(attr, attrFmt.cnt);
    const bool nbt3 = attr == GX_VA_NRM && attrFmt.cnt == GX_NRM_NBT3;
    auto& a = plan.attrs[plan.count++];
    a = {};
    a.srcOffset = static_cast<u16>(src);
    a.dstOffset = static_cast<u16>(dst);
    if (type == GX_DIRECT) {
      a.bytes = static_cast<u8>(compSize * cnt);
      src += a.bytes;
      dst += a.bytes;
      continue;
    }
    const auto& array = g_gxState.arrays[i];
    a.data = static_cast<const u8*>(array.data);
    a.size = array.size;
    a.stride = array.stride;
    a.base = array.baseIndex;
    a.indexSize = type == GX_INDEX8 ? 1 : 2;
    a.indices = nbt3 ? 3 : 1;
    a.bytes = static_cast<u8>(compSize * cnt / a.indices);
    plan.active = true;
    src += a.indexSize * a.indices;
    dst += compSize * cnt;
  }
  plan.srcStride = src;
  plan.dstStride = aligned ? AURORA_ALIGN(dst, 4u) : dst;
  return plan;
}

// One attribute of `count` records: `Bytes` per element (0: a.bytes at run time),
// `IndexSize` bytes per big-endian index (0: direct data, copied).
template <u32 Bytes, u32 IndexSize>
void deindex_attr(const DeindexAttr& a, const u8* src, u32 srcStride, u32 count, u8* dst, u32 dstStride) noexcept {
  const u32 bytes = Bytes != 0 ? Bytes : a.bytes;
  src += a.srcOffset;
  dst += a.dstOffset;
  for (u32 v = 0; v < count; ++v, src += srcStride, dst += dstStride) {
    if constexpr (IndexSize == 0) {
      std::memcpy(dst, src, bytes);
    } else {
      const u32 index = IndexSize == 1 ? src[0] : (static_cast<u32>(src[0]) << 8) | src[1];
      const size_t offset = static_cast<size_t>(a.base + index) * a.stride;
      if (offset + bytes <= a.size) [[likely]] {
        std::memcpy(dst, a.data + offset, bytes);
      } else {
        // The GPU path would read whatever follows the array
        std::memset(dst, 0, bytes);
        ++sDeindexOutOfRange;
      }
    }
  }
}

template <u32 IndexSize>
void deindex_attr_sized(const DeindexAttr& a, const u8* src, u32 srcStride, u32 count, u8* dst,
                        u32 dstStride) noexcept {
  switch (a.bytes) {
  case 1:
    return deindex_attr<1, IndexSize>(a, src, srcStride, count, dst, dstStride);
  case 2:
    return deindex_attr<2, IndexSize>(a, src, srcStride, count, dst, dstStride);
  case 4:
    return deindex_attr<4, IndexSize>(a, src, srcStride, count, dst, dstStride);
  case 6:
    return deindex_attr<6, IndexSize>(a, src, srcStride, count, dst, dstStride);
  case 8:
    return deindex_attr<8, IndexSize>(a, src, srcStride, count, dst, dstStride);
  case 12:
    return deindex_attr<12, IndexSize>(a, src, srcStride, count, dst, dstStride);
  default:
    return deindex_attr<0, IndexSize>(a, src, srcStride, count, dst, dstStride);
  }
}

// `count` FIFO records at `src` resolved into records at `dst`, attribute by
// attribute. An index past its array reads as zeros.
void deindex_records(const DeindexPlan& plan, const u8* src, u32 count, u8* dst) noexcept {
  for (u32 k = 0; k < plan.count; ++k) {
    const auto& a = plan.attrs[k];
    if (a.indices != 1) {
      // GX_NRM_NBT3: three indices, one per 3-vector slice
      for (u32 s = 0; s < a.indices; ++s) {
        DeindexAttr slice = a;
        slice.srcOffset = static_cast<u16>(a.srcOffset + s * a.indexSize);
        slice.dstOffset = static_cast<u16>(a.dstOffset + s * a.bytes);
        slice.base = a.base;
        // The slice's element starts s slices into the array element
        slice.data = a.data != nullptr ? a.data + s * a.bytes : nullptr;
        slice.size = a.size > s * a.bytes ? a.size - s * a.bytes : 0;
        slice.indices = 1;
        if (a.indexSize == 1) {
          deindex_attr_sized<1>(slice, src, plan.srcStride, count, dst, plan.dstStride);
        } else {
          deindex_attr_sized<2>(slice, src, plan.srcStride, count, dst, plan.dstStride);
        }
      }
      continue;
    }
    switch (a.indexSize) {
    case 0:
      deindex_attr_sized<0>(a, src, plan.srcStride, count, dst, plan.dstStride);
      break;
    case 1:
      deindex_attr_sized<1>(a, src, plan.srcStride, count, dst, plan.dstStride);
      break;
    default:
      deindex_attr_sized<2>(a, src, plan.srcStride, count, dst, plan.dstStride);
      break;
    }
  }
}

// The frame's vertex and index staging are mapped regions of a fixed size (gfx/frame.cpp):
// a push past their end aborts. What no longer fits is skipped for this frame and counted
// (staging_report at the frame's end), and the geometry cache leaves the frame room for its
// plain draws before uploading new surfaces (handle_cached_display_list).
struct StagingShortfall {
  u32 skippedDraws = 0;
  u32 deferredUploads = 0;
  u64 deferredBytes = 0;
  size_t vertexUsed = 0;
  size_t vertexCapacity = 0;
  size_t indexUsed = 0;
  size_t indexCapacity = 0;
  u32 reports = 0;
};
StagingShortfall sStaging;

bool staging_room(const ByteBuffer* buffer, size_t bytes, size_t reserve = 0) noexcept {
  // No buffer (no frame recording: the push itself refuses) or a growable one (tests):
  // no fixed end to keep to.
  return buffer == nullptr || buffer->owned() ||
         AURORA_ALIGN(buffer->size(), 4) + bytes + reserve <= buffer->capacity();
}

void note_skipped_draw() noexcept {
  ++sStaging.skippedDraws;
  if (const ByteBuffer* verts = gfx::staging_verts()) {
    sStaging.vertexUsed = verts->size();
    sStaging.vertexCapacity = verts->capacity();
  }
  if (const ByteBuffer* indices = gfx::staging_indices()) {
    sStaging.indexUsed = indices->size();
    sStaging.indexCapacity = indices->capacity();
  }
}

// At the frame's end: what did not fit, logged now and then.
void staging_report() noexcept {
  if (sStaging.skippedDraws != 0 && (sStaging.reports < 20 || sStaging.reports % 300 == 0)) {
    Log.warn("Frame staging full: {} draws skipped (vertices {} of {} KB, indices {} of {} KB)",
             sStaging.skippedDraws, sStaging.vertexUsed >> 10, sStaging.vertexCapacity >> 10,
             sStaging.indexUsed >> 10, sStaging.indexCapacity >> 10);
  }
  if (sStaging.deferredUploads != 0 && (sStaging.reports < 20 || sStaging.reports % 300 == 0)) {
    Log.info("Geometry cache: {} new surfaces ({} KB) left for later frames, the frame's staging was full",
             sStaging.deferredUploads, sStaging.deferredBytes >> 10);
  }
  if (sStaging.skippedDraws != 0 || sStaging.deferredUploads != 0) {
    ++sStaging.reports;
  }
  const u32 reports = sStaging.reports;
  sStaging = {};
  sStaging.reports = reports;
}

// The records of a draw staged: resolved when the plan asks, else as they are. Empty when
// the frame's vertex staging cannot hold them (the draw is skipped).
std::optional<gfx::Range> push_vertex_records(const DeindexPlan* plan, std::span<const u8> data, u32 vtxCount,
                                              size_t alignment) noexcept {
  const bool resolve = plan != nullptr && plan->active;
  if (!resolve) {
    const auto range = gfx::push_verts(data.data(), data.size(), alignment);
    if (gfx::overflowed(range)) { note_skipped_draw(); return std::nullopt; }
    return range;
  }
  static ByteBuffer resolved;
  resolved.clear();
  const size_t bytes = static_cast<size_t>(vtxCount) * plan->dstStride;
  {
    const gfx::perf::Bucket bucket{gfx::perf::g_fifoDeindexTicks, gfx::perf::enabled()};
    deindex_records(*plan, data.data(), vtxCount, resolved.append_uninitialized(bytes));
    gfx::perf::g_fifoDeindexedVerts.fetch_add(vtxCount, std::memory_order_relaxed);
  }
  const auto range = gfx::push_verts(resolved.data(), resolved.size(), alignment);
  if (gfx::overflowed(range)) { note_skipped_draw(); return std::nullopt; }
  return range;
}

// The texture bind groups by what build_texture_bind_group reads. Prime binds a
// few hundred texture sets a frame, each for a run of draws, so the descriptor
// hashing and sampler lookups behind build_bind_groups only run for a set the
// cache has not seen; direct-mapped by the key's hash, the whole key compared.
struct BindGroupKey {
  struct Slot {
    const void* view = nullptr; // the texture's sample view
    u32 mode0 = 0;              // wrap, filters, LOD bias, anisotropy
    u32 mode1 = 0;              // LOD clamps
    u32 flags = 0;              // replacement, arbitrary mips
    u32 reserved = 0;
  };
  std::array<Slot, MaxTextures> slots{};
  u32 sampled = 0;
  u32 pbr = 0;
  // The PBR probes by id, under the generation of their creations and
  // destructions (an id's views are fixed in between).
  u32 pbrCube = 0;
  u32 pbrVolume = 0;
  u32 probeGeneration = 0;
  u32 reserved = 0;
};
static_assert(std::has_unique_object_representations_v<BindGroupKey>);
u32 sProbeGeneration = 0;

struct BindGroupCacheEntry {
  BindGroupKey key{};
  GXBindGroups mono{};
  gfx::BindGroupRef multiview{};
  uint64_t multiviewEpoch = 0;
  uint32_t touchedFrame = UINT32_MAX;
  bool monoValid = false;
  bool multiviewValid = false;
};
// By the key's hash; the entries nothing used for a while are dropped now and then.
absl::flat_hash_map<uint64_t, BindGroupCacheEntry> sBindGroupCache;
constexpr uint32_t BindGroupCacheRetainFrames = 64;
uint32_t sBindGroupCachePrunedFrame = 0;

BindGroupKey bind_group_key(const ShaderInfo& info) noexcept {
  BindGroupKey key;
  std::memset(&key, 0, sizeof(key));
  for (u32 i = 0; i < MaxTextures; ++i) {
    const auto& tex = g_gxState.textures[i];
    if (!tex || !(info.sampledTextures[i] || info.sampledIndTextures[i])) {
      continue;
    }
    key.sampled |= 1u << i;
    auto& slot = key.slots[i];
    slot.view = tex.ref->sampleTextureView.Get();
    slot.mode0 = tex.texObj.mode0;
    slot.mode1 = tex.texObj.mode1;
    slot.flags = (tex.ref->isReplacement ? 1u : 0u) | (tex.ref->hasArbitraryMips ? 2u : 0u);
  }
  // Resolution has already applied texture invalidations. Key the resulting
  // views, not that global generation: EFB copies can invalidate bindings
  // every frame without changing any of the world's material descriptors.
  key.pbr = g_gxState.pbr ? 1u : 0u;
  key.pbrCube = g_gxState.pbrCube;
  key.pbrVolume = g_gxState.pbrVolume;
  key.probeGeneration = sProbeGeneration;
  return key;
}

// The entry for the current textures, or null when the draw samples none. Once
// a frame it keeps the entry's bind groups from expiring in the gfx cache; one
// that has expired is built again.
BindGroupCacheEntry* current_bind_group_entry(const ShaderInfo& info) noexcept {
  if (!info.sampledTextures.any() && !info.sampledIndTextures.any()) {
    return nullptr;
  }
  const BindGroupKey key = bind_group_key(info);
  const uint64_t hash = XXH3_64bits(&key, sizeof(key));
  const uint32_t frame = gfx::current_frame();
  if (frame != UINT32_MAX && frame - sBindGroupCachePrunedFrame > BindGroupCacheRetainFrames) {
    sBindGroupCachePrunedFrame = frame;
    absl::erase_if(sBindGroupCache, [frame](const auto& item) {
      return frame - item.second.touchedFrame > BindGroupCacheRetainFrames;
    });
  }
  auto [it, inserted] = sBindGroupCache.try_emplace(hash);
  auto& entry = it->second;
  if (inserted) {
    entry.key = key;
  } else if (std::memcmp(&entry.key, &key, sizeof(key)) != 0) {
    // A hash collision: the newcomer takes the entry
    entry = BindGroupCacheEntry{};
    entry.key = key;
  }
  if (entry.touchedFrame != frame) {
    entry.touchedFrame = frame;
    if (entry.monoValid && entry.mono.textureBindGroup != 0 && !gfx::touch_bind_group(entry.mono.textureBindGroup)) {
      entry.monoValid = false;
    }
    if (entry.multiviewValid && entry.multiview != 0 && !gfx::touch_bind_group(entry.multiview)) {
      entry.multiviewValid = false;
    }
  }
  return &entry;
}

GXBindGroups cached_bind_groups(const ShaderInfo& info) noexcept {
  auto* entry = current_bind_group_entry(info);
  if (entry == nullptr) {
    return {};
  }
  if (!entry->monoValid || entry->mono.textureBindGroup == 0) {
    gfx::perf::count(gfx::perf::g_fifoBindGroupMisses, gfx::perf::enabled());
    entry->mono = build_bind_groups(info);
    entry->monoValid = true;
  }
  return entry->mono;
}

gfx::BindGroupRef cached_multiview_bind_group(const ShaderInfo& info, uint64_t stereoEpoch) noexcept {
  auto* entry = current_bind_group_entry(info);
  if (entry == nullptr) {
    return {};
  }
  if (!entry->multiviewValid || entry->multiview == 0 || entry->multiviewEpoch != stereoEpoch) {
    entry->multiview = build_multiview_bind_group(info);
    entry->multiviewEpoch = stereoEpoch;
    entry->multiviewValid = true;
  }
  return entry->multiview;
}

FogRangeLutKey fog_range_lut_key() noexcept {
  const auto& state = g_gxState.fog;
  const f32 logicalWidth = std::max(g_gxState.logicalViewport.width, 1.f);
  const f32 renderWidth = std::max(g_gxState.renderViewport.width, 1.f);
  return {
      .rangeK = state.rangeK,
      .rangeCenter = ((static_cast<f32>(state.rangeCenter) - g_gxState.logicalViewport.left) / logicalWidth) * 2.f -
                     1.f + (g_gxState.renderViewport.left / renderWidth) * 2.f,
      .renderWidth = renderWidth,
      .targetWidth = gfx::get_render_target_size().x,
  };
}

std::vector<f32> build_fog_range_lut(const FogRangeLutKey& key) {
  std::array<f32, 10> rangeK;
  for (u32 i = 0; i < rangeK.size(); ++i) {
    const u32 source = (i & ~1u) | (1u - (i & 1u));
    rangeK[i] = static_cast<f32>(key.rangeK[source]) / 64.f;
  }

  std::vector<f32> lut(key.targetWidth);
  for (u32 x = 0; x < key.targetWidth; ++x) {
    const f32 screenX = ((static_cast<f32>(x) + 0.5f) / key.renderWidth) * 2.f - 1.f;
    const f32 offset = screenX - key.rangeCenter;
    const f32 rangeIndex = std::clamp(9.f - std::abs(offset) * 9.f, 0.f, 9.f);
    const u32 lower = static_cast<u32>(rangeIndex);
    const u32 upper = std::min(lower + 1, 9u);
    const f32 fraction = rangeIndex - static_cast<f32>(lower);
    const f32 k = std::max(rangeK[lower] * (1.f - fraction) + rangeK[upper] * fraction, 0.000001f);
    lut[x] = std::sqrt(offset * offset + k * k) / k;
  }
  return lut;
}

const std::vector<f32>& resolve_fog_range_lut(const FogRangeLutKey& key) {
  for (const auto& entry : sFogRangeLuts) {
    if (entry.key == key) {
      return entry.factors;
    }
  }
  if (sFogRangeLuts.size() == MaxFogRangeLuts) {
    sFogRangeLuts.erase(sFogRangeLuts.begin());
  }
  sFogRangeLuts.emplace_back(FogRangeLutEntry{key, build_fog_range_lut(key)});
  return sFogRangeLuts.back().factors;
}

gfx::Range push_fog_range_lut(const FogRangeLutKey& key) {
  const auto& lut = resolve_fog_range_lut(key);
  return gfx::push_storage(reinterpret_cast<const u8*>(lut.data()), lut.size() * sizeof(f32));
}

u8 line_mode_for_prim(GXPrimitive prim) noexcept {
  switch (prim) {
  case GX_LINES:
    return 1;
  case GX_LINESTRIP:
    return 2;
  case GX_POINTS:
    return 3;
  default:
    return 0;
  }
}
} // namespace

static void handle_draw(u8 cmd, ByteReader& reader) noexcept;
static void handle_aurora(ByteReader& reader) noexcept;
static void handle_cached_display_list(ByteReader& reader) noexcept;

ProcessResult process(const u8* data, u32 size) noexcept {
  ZoneScoped;
  ByteReader reader{{data, size}};
  const bool perfOn = gfx::perf::enabled();

  while (!reader.empty()) {
    const u8 cmd = reader.read<u8>();
    u8 opcode = cmd & CP_OPCODE_MASK;

    switch (opcode) {
    case CP_CMD_NOP:
      continue;

    case CP_CMD_LOAD_BP_REG: {
      const u32 value = reader.read<u32>();
      {
        const gfx::perf::Bucket bucket{gfx::perf::g_fifoBpTicks, perfOn, &gfx::perf::g_fifoBpLoads};
        handle_bp(value);
      }
      if (reg_get(value, 8, 24) == GX_BP_REG_DRAWDONE) {
        return {static_cast<u32>(reader.offset()), true};
      }
      break;
    }

    case CP_CMD_LOAD_CP_REG: {
      const u8 addr = reader.read<u8>();
      const gfx::perf::Bucket bucket{gfx::perf::g_fifoCpTicks, perfOn};
      handle_cp(addr, reader.read<u32>());
      break;
    }

    case CP_CMD_LOAD_XF_REG: {
      const u32 header = reader.read<u32>();
      const u32 count = ((header >> 16) & 0xFFFF) + 1;
      const u16 addr = header & 0xFFFF;
      const gfx::perf::Bucket bucket{gfx::perf::g_fifoXfTicks, perfOn, &gfx::perf::g_fifoXfLoads};
      handle_xf(addr, reader.take(count * sizeof(u32)));
      break;
    }

    case CP_CMD_LOAD_INDX_A:
    case CP_CMD_LOAD_INDX_B:
    case CP_CMD_LOAD_INDX_C:
    case CP_CMD_LOAD_INDX_D: {
      ZoneScopedN("LOAD_INDX");
      const gfx::perf::Bucket bucket{gfx::perf::g_fifoXfTicks, perfOn, &gfx::perf::g_fifoXfLoads};
      const u32 arrayType = GX_POS_MTX_ARRAY + (opcode - CP_CMD_LOAD_INDX_A) / 0x08;
      const u16 srcArrayIdx = reader.read<u16>();
      const u16 addrLen = reader.read<u16>();

      const u16 len = (addrLen >> 12) + 1;
      const u16 dstAddr = addrLen & 0x0FFF;
      auto const& array = g_gxState.arrays[arrayType];
      const u32 srcOffset = static_cast<u32>(srcArrayIdx) * array.stride;
      const u32 srcSize = static_cast<u32>(len) * sizeof(u32);
      AURORA_ASSERT(array.data != nullptr, "indexed XF load from unmapped array {}", arrayType);
      AURORA_ASSERT(srcOffset <= array.size && srcSize <= array.size - srcOffset,
                    "indexed XF load outside array {}: offset={}, size={}, array size={}", arrayType, srcOffset,
                    srcSize, array.size);
      auto const* srcData = static_cast<const u8*>(array.data) + srcOffset;
      if (!copy_xf_data(dstAddr, srcData, len, array.le ? std::endian::little : std::endian::big)) {
#ifndef NDEBUG
        Log.debug("Unimplemented indexed XF load (opcode 0x{:02X}, dstAddr=%04x)", opcode, dstAddr);
#endif
      }
      break;
    }

    case CP_CMD_CALL_DL: {
      // Call display list: 8 bytes (address + size)
      Log.warn("Ignoring nested GX_CMD_CALL_DL");
      reader.skip(8);
      break;
    }

    case CP_CMD_INVAL_VTX: {
      for (auto& array : g_gxState.arrays) {
        array.cachedRange = {};
      }
      g_gxState.dirty |= DirtyImmediates;
      break;
    }

    case GX_AURORA: {
      // A cached display list's draws count under their own buckets, not this one.
      if (reader.remaining() >= 2 && read_bits<u16>(reader.data() + reader.offset()) == GX_AURORA_CALL_CACHED_DL) {
        reader.skip(2);
        handle_cached_display_list(reader);
        break;
      }
      const gfx::perf::Bucket bucket{gfx::perf::g_fifoAuroraTicks, perfOn};
      handle_aurora(reader);
      break;
    }

    // Draw commands: 0x80-0xBF
    case GX_DRAW_QUADS:
    case GX_DRAW_TRIANGLES:
    case GX_DRAW_TRIANGLE_STRIP:
    case GX_DRAW_TRIANGLE_FAN:
    case GX_DRAW_LINES:
    case GX_DRAW_LINE_STRIP:
    case GX_DRAW_POINTS: {
      handle_draw(cmd, reader);
      break;
    }

    default:
      // Check if it's a draw command (0x80-0xBF range)
      if (cmd >= 0x80) {
        handle_draw(cmd, reader);
      } else {
        // Hex dump surrounding bytes for debugging
        {
          const size_t pos = reader.offset();
          size_t dumpStart = (pos > 17) ? pos - 17 : 0;
          size_t dumpEnd = (pos + 16 < size) ? pos + 16 : size;
          std::string hex;
          for (size_t i = dumpStart; i < dumpEnd; i++) {
            if (i == pos - 1)
              hex += fmt::format("[{:02x}]", data[i]);
            else
              hex += fmt::format(" {:02x}", data[i]);
          }
          Log.error("  hex dump (pos {}-{}):{}", dumpStart, dumpEnd - 1, hex);
        }
        FATAL("command_processor: unknown opcode 0x{:02X} at pos {}", cmd, reader.offset() - 1);
      }
      break;
    }
  }
  return {size, false};
}

// A static world surface's display list the geometry cache cannot take (no
// de-indexing, not a pure surface, no room): processed where it lives, like FIFO
// bytes, and counted (perf_counters.hpp g_fifoCachedDl*).
static void process_display_list_in_place(const u8* data, u32 size) noexcept {
  const bool perfOn = gfx::perf::enabled();
  const ByteBuffer* const verts = perfOn ? gfx::staging_verts() : nullptr;
  const ByteBuffer* const indices = perfOn ? gfx::staging_indices() : nullptr;
  const size_t vertBytes0 = verts != nullptr ? verts->size() : 0;
  const size_t indexBytes0 = indices != nullptr ? indices->size() : 0;
  const u32 deindexed0 = perfOn ? gfx::perf::g_fifoDeindexedVerts.load(std::memory_order_relaxed) : 0;
  process(data, size);
  if (perfOn) {
    gfx::perf::g_fifoCachedDlCalls.fetch_add(1, std::memory_order_relaxed);
    gfx::perf::g_fifoCachedDlBytes.fetch_add(size, std::memory_order_relaxed);
    if (verts != nullptr) {
      gfx::perf::g_fifoCachedDlVertBytes.fetch_add(verts->size() - vertBytes0, std::memory_order_relaxed);
    }
    if (indices != nullptr) {
      gfx::perf::g_fifoCachedDlIndexBytes.fetch_add(indices->size() - indexBytes0, std::memory_order_relaxed);
    }
    gfx::perf::g_fifoCachedDlVerts.fetch_add(
        gfx::perf::g_fifoDeindexedVerts.load(std::memory_order_relaxed) - deindexed0, std::memory_order_relaxed);
  }
}

[[noreturn]] static void handle_draw_overrun(size_t totalVtxBytes, const ByteReader& reader) noexcept {
  // Hex dump around the draw command for debugging
  const size_t pos = reader.offset();
  const size_t size = reader.size();
  const u8* data = reader.data();
  size_t cmdPos = pos - 2 - 1; // opcode byte position (before vtxCount and pos++)
  size_t dumpStart = (cmdPos > 16) ? cmdPos - 16 : 0;
  size_t dumpEnd = (cmdPos + 32 < size) ? cmdPos + 32 : size;
  std::string hex;
  for (size_t i = dumpStart; i < dumpEnd; i++) {
    if (i == cmdPos)
      hex += fmt::format("[{:02x}]", data[i]);
    else
      hex += fmt::format(" {:02x}", data[i]);
  }
  Log.error("  hex dump around draw cmd (pos {}-{}):{}", dumpStart, dumpEnd - 1, hex);
  FATAL("draw vertex data overrun: need {} bytes at pos {}, have {}", totalVtxBytes, pos, reader.remaining());
}

static u32 calc_vtx_size(GXVtxFmt fmt) noexcept {
  u32 vtxSize = 0;
  const auto& vtxFmt = g_gxState.vtxFmts[fmt];
  for (int i = GX_VA_PNMTXIDX; i <= GX_VA_TEX7; ++i) {
    const auto& attrFmt = vtxFmt.attrs[i];
    switch (g_gxState.vtxDesc[i]) {
    case GX_NONE:
      break;
    case GX_DIRECT: {
      const auto attr = static_cast<GXAttr>(i);
      vtxSize += comp_type_size(attr, attrFmt.type) * comp_cnt_count(attr, attrFmt.cnt);
      break;
    }
    case GX_INDEX8:
      vtxSize += i == GX_VA_NRM && attrFmt.cnt == GX_NRM_NBT3 ? 3 : 1;
      break;
    case GX_INDEX16:
      vtxSize += i == GX_VA_NRM && attrFmt.cnt == GX_NRM_NBT3 ? 6 : 2;
      break;
    }
  }
  g_gxState.lastVtxFmt = fmt;
  g_gxState.lastVtxSize = vtxSize;
  return vtxSize;
}

// The multiview shader variant the uniform just staged needs
// (gfx::stage_stereo_uniforms), or MultiviewNone, which leaves the draw out of
// the eye passes, when the staged data does not sit where that variant reads it:
// the clip matrices or the second eye copy at the shader's uniform size
// (shader.cpp to_multiview_source).
static u8 staged_multiview_mode(const DrawCache& cache) noexcept {
  const u8 mode = gfx::stereo_multiview_mode();
  const auto& offsets = cache.stereoUniformOffsets;
  u32 stride = cache.shaderInfo.uniformSize;
  if (mode == MultiviewClip) {
    stride = gfx::align_uniform(cache.uniformRange.size);
  } else if (mode == MultiviewFull && offsets[0] != offsets[1]) {
    stride = offsets[1] - offsets[0];
  }
  if (stride != cache.shaderInfo.uniformSize) {
    static bool sReported = false;
    if (!sReported) {
      sReported = true;
      Log.error("Multiview uniform data lies {} bytes on, the shader expects {}", stride,
                cache.shaderInfo.uniformSize);
    }
    return MultiviewNone;
  }
  return mode;
}

static void select_native_vertices(bool native) noexcept {
  if (g_gxState.nativeVertices != native) {
    g_gxState.nativeVertices = native;
    g_gxState.dirty |= DirtyPipeline;
  }
}

// A texgen reading a UV set the vertices don't carry draws with zero UVs (shader.cpp
// vtx_attr). Names the draw from GX_AURORA_SET_DRAW_TAG, once per model, material and
// texcoord, so a bad mod model can be found.
static void warn_missing_uv_sets(const ShaderConfig& config, const ShaderInfo& info) noexcept {
  static std::vector<std::array<u32, 4>> sReported;
  for (u32 i = 0; i < info.sampledTexCoords.size(); ++i) {
    if (!info.sampledTexCoords.test(i)) {
      continue;
    }
    const auto& tcg = config.tcgs[i];
    if ((tcg.type >= GX_TG_BUMP0 && tcg.type <= GX_TG_BUMP7) || tcg.src < GX_TG_TEX0 || tcg.src > GX_TG_TEX7) {
      continue;
    }
    const u32 set = tcg.src - GX_TG_TEX0;
    if (config.attrs[GX_VA_TEX0 + set].attrType != GX_NONE) {
      continue;
    }
    const auto& tag = g_gxState.drawTag;
    const std::array<u32, 4> key{tag[0], tag[1], tag[2], i};
    if (sReported.size() >= 64 || std::find(sReported.begin(), sReported.end(), key) != sReported.end()) {
      continue;
    }
    sReported.push_back(key);
    if (tag[0] == 0 && tag[1] == UINT32_MAX) {
      Log.warn("untagged draw: texcoord {} reads UV set {}, which its vertices lack{}", i, set,
               config.pbr ? " (PBR)" : "");
    } else {
      Log.warn("model {:08X} (index {}) material {}: texcoord {} reads UV set {}, which its vertices lack{}", tag[0],
               static_cast<s32>(tag[1]), tag[2], i, set, config.pbr ? " (PBR)" : "");
    }
  }
}

// A draw whose data the frame's buffers have no room left for (e.g. an oversized replacement
// model) is dropped instead of aborting the game: true when range overflowed. Warns once per
// model. Whatever was pushed before the drop is orphaned, so the next draw must not merge.
static bool drop_overflowed_draw(gfx::Range range, const char* buffer) noexcept {
  if (!gfx::overflowed(range))
    LIKELY { return false; }
  sDrawCache.lastDrawFmt = GX_MAX_VTXFMT;
  static std::vector<u32> sReported;
  const auto& tag = g_gxState.drawTag;
  if (sReported.size() < 64 && std::find(sReported.begin(), sReported.end(), tag[0]) == sReported.end()) {
    sReported.push_back(tag[0]);
    if (tag[0] == 0 && tag[1] == UINT32_MAX) {
      Log.warn("untagged draw dropped: the frame's {} buffer is full", buffer);
    } else {
      Log.warn("model {:08X} (index {}) material {}: draw dropped, the frame's {} buffer is full", tag[0],
               static_cast<s32>(tag[1]), tag[2], buffer);
    }
  }
  return true;
}

// --- Vertex array overrun check (MP_GX_CHECK_ARRAYS) -------------------------------------------------
//
// 807013b1e: a skinned PBR draw read its bind pose through GX_VA_TEX7 while TEX7's array held 8-byte UVs.
// The shader reads 12-byte positions there, so it read past the array, which on Vulkan with Dawn's
// robustness off crashed. The check below walks a draw's vertex indices and gives the verdict the
// shader's own fetches would: the index comes out of the vertex (shader.cpp attr_address: raw_fetch_u8_1
// / raw_fetch_u16_1(..., false)), the element's byte size from the fetch that reads it (fetch_attr,
// fetch_color_attr). Off unless MP_GX_CHECK_ARRAYS is set to anything but "0"; a build without NDEBUG
// has it on.

// The bytes one shader fetch reads for an attribute, given the vertex format's component type and count.
static u32 attr_element_bytes(GXAttr attr, GXCompType type, u32 cnt) noexcept {
  if (attr == GX_VA_CLR0 || attr == GX_VA_CLR1) {
    // fetch_color_attr unpacks one packed colour, whatever the format's count says.
    return comp_type_size(attr, type);
  }
  if (type == GX_RGBA8) {
    return 4; // unpack4x8unorm(load_u32_raw(&abuf, offs))
  }
  // A normal's NBT slices read from the same element, up to 3 comps at (cnt - 3) comps in, so the
  // full cnt covers them.
  return u32(comp_type_size(attr, type)) * cnt;
}

struct IndexedAttrFetch {
  GXAttr attr;
  u8 descType;   // GX_INDEX8 or GX_INDEX16
  u32 vtxOffset; // where the vertex holds the index (populate_pipeline_config's walk)
  u32 elementBytes;
};

// Whether the shader uses a texture coordinate attribute: a texgen reads it, or it is the PBR lightmap's
// UV (populate_pipeline_config). The shader loads every attribute in the vertex description, but the
// load of one nothing uses is dead code. Retail does leave such indices past their array: a packed
// lightmap draw (VTXFMT2) whose material has no lightmap keeps TEX0 in its description, with the
// unpacked UVs bound and packed-UV indices, and its one texgen reading another attribute.
static bool tex_attr_used(GXAttr attr) noexcept {
  const auto& state = g_gxState;
  if (state.pbr != 0 && state.pbrLightmapAttr == attr) {
    return true;
  }
  const auto src = static_cast<GXTexGenSrc>(GX_TG_TEX0 + (attr - GX_VA_TEX0));
  for (u32 t = 0; t < state.numTexGens; ++t) {
    if (state.tcgs[t].src == src) {
      return true;
    }
  }
  return false;
}

// The attributes the vertex shader fetches from an array, with the offset of their index in the vertex.
static u32 collect_indexed_attr_fetches(GXVtxFmt fmt, IndexedAttrFetch* out) noexcept {
  const auto& vtxFmt = g_gxState.vtxFmts[fmt];
  u32 count = 0;
  u32 vtxOffset = 0;
  for (int i = GX_VA_PNMTXIDX; i <= GX_VA_TEX7; ++i) {
    const auto attr = static_cast<GXAttr>(i);
    const auto descType = g_gxState.vtxDesc[i];
    const auto& attrFmt = vtxFmt.attrs[i];
    const u32 cnt = comp_cnt_count(attr, attrFmt.cnt);
    const bool nbt3 = attr == GX_VA_NRM && attrFmt.cnt == GX_NRM_NBT3;
    if (descType == GX_INDEX8 || descType == GX_INDEX16) {
      // Only VA_POS..VA_TEX7 have a base in DrawImmediateData::arrayStart.
      if (i >= GX_VA_POS && (i < GX_VA_TEX0 || tex_attr_used(attr))) {
        out[count++] = IndexedAttrFetch{attr, u8(descType), vtxOffset, attr_element_bytes(attr, attrFmt.type, cnt)};
      }
      vtxOffset += descType == GX_INDEX16 ? (nbt3 ? 6u : 2u) : (nbt3 ? 3u : 1u);
    } else if (descType == GX_DIRECT) {
      vtxOffset += u32(comp_type_size(attr, attrFmt.type)) * cnt;
    }
  }
  return count;
}

constexpr std::string_view attr_name(GXAttr attr) noexcept {
  switch (attr) {
  case GX_VA_POS:
    return "POS"sv;
  case GX_VA_NRM:
    return "NRM"sv;
  case GX_VA_CLR0:
    return "CLR0"sv;
  case GX_VA_CLR1:
    return "CLR1"sv;
  case GX_VA_TEX0:
    return "TEX0"sv;
  case GX_VA_TEX1:
    return "TEX1"sv;
  case GX_VA_TEX2:
    return "TEX2"sv;
  case GX_VA_TEX3:
    return "TEX3"sv;
  case GX_VA_TEX4:
    return "TEX4"sv;
  case GX_VA_TEX5:
    return "TEX5"sv;
  case GX_VA_TEX6:
    return "TEX6"sv;
  case GX_VA_TEX7:
    return "TEX7"sv;
  default:
    return "?"sv;
  }
}

// True when the array is missing, or the draw's last index's element reads past its end. shaderStride is
// the step the shader takes between elements, which is the array's own stride except for the bind pose.
static bool array_overrun(GXAttr attr, const AttrArray& array, u32 shaderStride, u32 maxIndex,
                          u32 elementBytes) noexcept {
  const u64 base = u64(array.baseIndex) * array.stride;
  const u64 needed = base + u64(maxIndex) * shaderStride + elementBytes;
  if (array.data != nullptr && needed <= array.size) {
    return false;
  }
  // Once per (attribute, array), as drop_overflowed_draw reports a full buffer once.
  static std::vector<std::pair<GXAttr, const void*>> sReported;
  const std::pair<GXAttr, const void*> key{attr, array.data};
  if (sReported.size() < 64 && std::find(sReported.begin(), sReported.end(), key) == sReported.end()) {
    sReported.push_back(key);
    Log.error("gx: vertex array overrun: attr {} ({}), max index {}, base index {}, stride {}, element {} bytes: "
              "needs {} bytes, the array has {}",
              static_cast<u32>(attr), attr_name(attr), maxIndex, array.baseIndex, shaderStride, elementBytes, needed,
              array.size);
  }
  testing::vertexArrayOverruns.fetch_add(1, std::memory_order_relaxed);
  return true;
}

// Whether the draw's bind pose is read from TEX7's array (shader.cpp, pbrBindPos): bind_pos_active, less
// the actor shadow decal pass, which drops the PBR mode and with it the bind pose (populate_pipeline_config).
static bool bind_pos_read(GXPrimitive prim) noexcept {
  if (!bind_pos_active()) {
    return false;
  }
  const auto& state = g_gxState;
  const bool depthOnly = state.depthPrepass == 1 || state.depthPrepass == 3;
  const bool drawId = state.drawIdMode && !depthOnly;
  const bool shadow =
      state.shadowCaster && state.shadowActive && prim != GX_LINES && prim != GX_LINESTRIP && prim != GX_POINTS;
  return !(state.actorDecal && shadow && !drawId && !depthOnly);
}

// True when this draw must be dropped: one of the arrays its shader reads does not hold the elements its
// indices reach. vtxData is the draw's vertices as they are in the FIFO (or the display list) and
// vtxStride their size in bytes.
static bool check_indexed_arrays(const u8* vtxData, GXVtxFmt fmt, GXPrimitive prim, u16 vtxCount,
                                 u32 vtxStride) noexcept {
  if (!testing::vertexArrayCheck.load(std::memory_order_relaxed)) {
    LIKELY { return false; }
  }
  if (vtxData == nullptr || vtxCount == 0) {
    return false;
  }
  auto& state = g_gxState;
  IndexedAttrFetch fetches[MaxIndexAttr];
  const u32 fetchCount = collect_indexed_attr_fetches(fmt, fetches);
  // The largest index each array is read with, over the draw's vertices.
  u32 maxIndex[MaxIndexAttr] = {};
  for (u32 v = 0; v < vtxCount; ++v) {
    const u8* const vtx = vtxData + size_t(v) * vtxStride;
    for (u32 a = 0; a < fetchCount; ++a) {
      // raw_fetch_u16_1(..., false): the index is big-endian, as GX data is.
      const u32 idx = fetches[a].descType == GX_INDEX8
                          ? u32{vtx[fetches[a].vtxOffset]}
                          : (u32{vtx[fetches[a].vtxOffset]} << 8) | u32{vtx[fetches[a].vtxOffset + 1]};
      maxIndex[a] = std::max(maxIndex[a], idx);
    }
  }
  bool drop = false;
  for (u32 a = 0; a < fetchCount; ++a) {
    const auto& array = state.arrays[fetches[a].attr];
    drop = array_overrun(fetches[a].attr, array, array.stride, maxIndex[a], fetches[a].elementBytes) || drop;
  }
  if (bind_pos_read(prim)) {
    // The bind pose is GX_VA_POS's index into TEX7's array, read as a 12-byte position's y (shader.cpp:
    // `imm_array_start + index * 12u + 4u`, 4 bytes), whatever TEX7's array holds: 807013b1e was an
    // 8-byte-UV array read that way.
    u32 maxPosIndex = 0;
    for (u32 a = 0; a < fetchCount; ++a) {
      if (fetches[a].attr == GX_VA_POS) {
        maxPosIndex = maxIndex[a];
      }
    }
    drop = array_overrun(GX_VA_TEX7, state.arrays[GX_VA_TEX7], 12, maxPosIndex, 8) || drop;
  }
  if (drop) {
    // Whatever was pushed before the drop is orphaned: the next draw must not merge into it.
    sDrawCache.lastDrawFmt = GX_MAX_VTXFMT;
  }
  return drop;
}

static void push_gx_draw(GXPrimitive prim, GXVtxFmt fmt, u32 vtxCount, gfx::Range vertRange, gfx::Range idxRange,
                         u32 numIndices, bool cachedGeometry = false) noexcept {
  auto& state = g_gxState;
  auto& cache = sDrawCache;
  if (drop_overflowed_draw(vertRange, "vertex") || drop_overflowed_draw(idxRange, "index")) {
    return;
  }
  const bool perfOn = gfx::perf::enabled();
  const auto t0 = gfx::perf::stamp(perfOn);

  DrawImmediateData immediates{
      .vtxStart = vertRange.offset, .currentPnMtx = state.currentPnMtx, .serial = state.drawSerial};
  // The arrays, unless the vertices carry their elements already (deindexVertices), and
  // TEX7's when it holds a skinned draw's bind pose, read with POS's index (bind_pos_active;
  // de-indexed vertices have no POS index left to read it with).
  const bool bindPos = bind_pos_active();
  for (int i = GX_VA_POS; i <= GX_VA_TEX7 && !state.deindexVertices; ++i) {
    if (state.vtxDesc[i] != GX_INDEX8 && state.vtxDesc[i] != GX_INDEX16 && !(bindPos && i == GX_VA_TEX7)) {
      continue;
    }
    auto& array = state.arrays[i];
    if (array.cachedRange.size == 0 && !resident::array_range(array.data, array.size, array.cachedRange)) {
      array.cachedRange = gfx::push_storage(static_cast<const uint8_t*>(array.data), array.size);
      if (drop_overflowed_draw(array.cachedRange, "storage")) {
        array.cachedRange = {};
        return;
      }
    }
    immediates.arrayStart[i - GX_VA_POS] = array.cachedRange.offset + array.baseIndex * array.stride;
  }

  const u8 lineMode = line_mode_for_prim(prim);
  const bool pipelineValid = cache.hasPipeline && (state.dirty & DirtyPipeline) == 0 && cache.fmt == fmt &&
                             cache.lineMode == lineMode && cache.config.msaaSamples == gfx::get_sample_count() &&
                             // The pass's target, not only the FIFO's switch: a new frame (reset to LDR),
                             // an offscreen pass or a colour pass must not keep an HDR pipeline, or back.
                             cache.config.shaderConfig.sceneHdr == gfx::scene_hdr_pass_active() &&
                             // A lit config made before aurora_set_per_pixel_lighting changed.
                             cache.config.shaderConfig.perPixelLight ==
                                 wants_per_pixel_light(cache.config.shaderConfig);
  if (!pipelineValid) {
    const bool hadPipeline = cache.hasPipeline;
    const auto prevSampledTextures = cache.shaderInfo.sampledTextures;
    const auto prevSampledIndTextures = cache.shaderInfo.sampledIndTextures;
    const bool prevUsesVolFog = cache.shaderInfo.usesVolFog;
    const bool prevShadowReceive = cache.shaderInfo.shadowReceive;
    const bool prevActorDecal = cache.shaderInfo.actorDecal;
    const bool prevUsesShell = cache.shaderInfo.usesShell;
    const bool prevUsesLightmap = cache.shaderInfo.usesLightmap;
    populate_pipeline_config(cache.config, prim, fmt);
    // PrimedGun: the eye passes' pipelines (multiviewPipelineRefs) were the previous config's.
    cache.multiviewPipelineMask = 0;
    // GX_AURORA_PORT_DEPTH_PREPASS: pass 2 shades an eligible draw where the depth is equal without
    // writing it, yet it casts as the draw it stands for (LEQUAL, writing). Its config can equal a
    // plain EQUAL draw's, which doesn't cast, so it bypasses the memo.
    const bool prepassEligible = state.depthPrepass != 0 && depth_prepass_eligible(lineMode);
    const bool prepassShade = state.depthPrepass == 2 && prepassEligible;
    cache.prepassSkip = (state.depthPrepass == 1 || state.depthPrepass == 3) && !prepassEligible;
    // A frame switches pipelines hundreds of times between a few hundred configs. The config's
    // hash is the pipeline's ref, so a config seen before reuses its shader info and shadow
    // pipeline without the second lookup (shader info depends on the config alone). With blocking
    // pipelines the lookup must still happen, since it is what waits for this draw's pipeline.
    const bool blocking = blocking_pipelines();
    const gfx::PipelineRef ref = xxh3_hash(cache.config, static_cast<HashType>(gfx::ShaderType::GX));
    auto memoIt = blocking || prepassShade ? sPipelineMemo.end() : sPipelineMemo.find(ref);
    if (memoIt != sPipelineMemo.end() && gfx::touch_pipeline(ref) &&
        (!memoIt->second.shadowCaster || gfx::touch_pipeline(memoIt->second.shadowPipelineRef)) &&
        (!memoIt->second.cutCaster || gfx::touch_pipeline(memoIt->second.cutPipelineRef))) {
      const auto& memo = memoIt->second;
      cache.shaderInfo = memo.shaderInfo;
      cache.pipelineRef = ref;
      cache.shadowCaster = memo.shadowCaster;
      cache.shadowPipelineRef = memo.shadowPipelineRef;
      cache.cutPipelineRef = memo.cutPipelineRef;
      cache.cutCaster = memo.cutCaster;
    } else {
      cache.shaderInfo = build_shader_info(cache.config.shaderConfig);
      warn_missing_uv_sets(cache.config.shaderConfig, cache.shaderInfo);
      cache.pipelineRef = gfx::pipeline_ref(cache.config, blocking);
      // Only an opaque surface that writes depth casts: the shadow map's vertex-only pass can't
      // alpha-test or blend. The game draws most opaque surfaces as a ONE/ZERO blend.
      const auto& sc = cache.config.shaderConfig;
      const bool opaque = cache.config.blendMode == GX_BM_NONE ||
                          (cache.config.blendMode == GX_BM_BLEND && cache.config.blendFacSrc == GX_BL_ONE &&
                           cache.config.blendFacDst == GX_BL_ZERO);
      cache.shadowCaster = cache.shaderInfo.usesShadow && cache.config.depthCompare &&
                           (cache.config.depthUpdate || prepassShade) && opaque && !sc.alphaCompare &&
                           sc.depthOnly == 0;
      // The caster pipelines take the draw's own depth state, not pass 2's.
      const auto casterConfig = [&](u8 shadowPass) {
        PipelineConfig config = cache.config;
        config.shadowPass = shadowPass;
        config.msaaSamples = 1;
        if (prepassShade) {
          config.depthFunc = state.depthFunc;
          config.depthUpdate = true;
        }
        return config;
      };
      if (cache.shadowCaster) {
        cache.shadowPipelineRef = gfx::pipeline_ref(casterConfig(1), blocking);
      }
      // A Remastered cutout (the PBR cut bit of a draw's mode) casts through its own cut test.
      cache.cutCaster = cache.shadowCaster && sc.pbr != 0 && sc.shadow && shader_has_cut_caster(sc);
      if (cache.cutCaster) {
        cache.cutPipelineRef = gfx::pipeline_ref(casterConfig(2), blocking);
      }
      if (!prepassShade) {
        sPipelineMemo.insert_or_assign(cache.pipelineRef, PipelineMemo{
                                                              .shaderInfo = cache.shaderInfo,
                                                              .shadowPipelineRef = cache.shadowPipelineRef,
                                                              .cutPipelineRef = cache.cutPipelineRef,
                                                              .shadowCaster = cache.shadowCaster,
                                                              .cutCaster = cache.cutCaster,
                                                          });
      }
    }
    cache.fmt = fmt;
    cache.lineMode = lineMode;
    cache.hasPipeline = true;
    // BP cmode0 (blend state) is DirtyPipeline. Always refresh uniforms even when only that
    // register changed: non-PBR HDR's hdr_tev_blend follows the actual SRCALPHA/ONE or SRCALPHA/INVSRCALPHA blend.
    state.dirty = (state.dirty & ~DirtyPipeline) | DirtyUniform;
    gfx::perf::count(gfx::perf::g_fifoPipelineBuilds, perfOn);
    if (!hadPipeline || prevSampledTextures != cache.shaderInfo.sampledTextures ||
        prevSampledIndTextures != cache.shaderInfo.sampledIndTextures ||
        prevUsesVolFog != cache.shaderInfo.usesVolFog || prevUsesLightmap != cache.shaderInfo.usesLightmap ||
        prevShadowReceive != cache.shaderInfo.shadowReceive || prevActorDecal != cache.shaderInfo.actorDecal ||
        prevUsesShell != cache.shaderInfo.usesShell || cache.cutCaster) {
      cache.bindGeneration = 0;
    }
  }

  const auto tA = gfx::perf::stamp(perfOn);
  note_draw_shader(state.drawSerial, cache.config.shaderConfig);

  const bool bindGroupsValid =
      (state.dirty & DirtyTextures) == 0 && cache.bindGeneration == texture::current_bind_generation();
  if (!bindGroupsValid) {
    gfx::perf::count(gfx::perf::g_fifoBindGroupBuilds, perfOn);
    const auto prevBindGroup = cache.bindGroups.textureBindGroup;
    {
      const gfx::perf::Bucket bucket{gfx::perf::g_fifoResolveTicks, perfOn};
      resolve_sampled_textures(cache.shaderInfo);
    }
    cache.bindGroups = cached_bind_groups(cache.shaderInfo);
    if (cache.cutCaster) {
      ShaderInfo plain = cache.shaderInfo;
      plain.shadowReceive = false;
      cache.cutBindGroups = build_bind_groups(plain);
    }
    cache.bindGeneration = texture::current_bind_generation();
    state.dirty &= ~DirtyTextures;
    // For texture_size_bias uniform
    if (cache.bindGroups.textureBindGroup != prevBindGroup) {
      state.dirty |= DirtyUniform;
    }
  }
  // Stereo replay: eye bind groups for draws that sample a per-eye EFB copy, or
  // under multiview every draw's bind group of 2D array views.
  const bool multiview = gfx::recording_multiview();
  if (gfx::stereo_shadow::active()) {
    const uint64_t stereoEpoch = gfx::stereo_shadow::epoch();
    if (!bindGroupsValid || cache.stereoEpoch != stereoEpoch || cache.stereoBindGroupsMultiview != multiview) {
      cache.stereoBindGroups =
          multiview ? std::array<gfx::BindGroupRef, 2>{cached_multiview_bind_group(cache.shaderInfo, stereoEpoch), {}}
                    : build_stereo_bind_groups(cache.shaderInfo);
      cache.stereoEpoch = stereoEpoch;
      cache.stereoBindGroupsMultiview = multiview;
    }
  } else {
    cache.stereoBindGroups = {};
  }

  const auto t1 = gfx::perf::stamp(perfOn);
  const bool uniformValid = (state.dirty & DirtyUniform) == 0 && cache.uniformRange.size != 0;
  if (!uniformValid) {
    gfx::perf::count(gfx::perf::g_fifoUniformBuilds, perfOn);
    cache.uniformRange = build_uniform(cache.shaderInfo, cache.stereoUniformOffsets, &cache.uniformBytes);
    if (drop_overflowed_draw(cache.uniformRange, "uniform")) {
      cache.uniformRange = {};
      return;
    }
    cache.uniformRoute = gfx::stereo_draw_route();
    cache.uniformPlane = gfx::stereo_head_locked_plane();
    cache.uniformPlacement = gfx::stereo_placement_3d();
    cache.uniformScreenTexMtx = gfx::stereo_screen_tex_mtx();
    cache.multiviewMode = staged_multiview_mode(cache);
    state.dirty &= ~DirtyUniform;
  }
  const auto t2 = gfx::perf::stamp(perfOn);
  // The eye passes' pipeline: under multiview the staged mode's; per eye, EyeClipImmediate
  // for a draw staged with its eye clips (a MultiviewFull one keeps its per-eye uniform
  // copies and the plain pipeline).
  gfx::PipelineRef multiviewPipeline{};
  const u8 eyeMode = multiview                                 ? cache.multiviewMode
                     : cache.multiviewMode == MultiviewClip ? static_cast<u8>(EyeClipImmediate)
                                                            : static_cast<u8>(MultiviewNone);
  if (eyeMode != MultiviewNone) {
    const u8 modeBit = 1u << eyeMode;
    if ((cache.multiviewPipelineMask & modeBit) == 0) {
      PipelineConfig eyeConfig = cache.config;
      eyeConfig.shaderConfig.multiview = eyeMode;
      // A multiview eye pass must not run with any of its draws skipped for a pipeline still
      // building: on the Quest 3 (Adreno 740) such a pass faults the GPU ("CP opcode error",
      // VK_ERROR_DEVICE_LOST). The cache builds them at startup; one it did not have is waited
      // for here, a hitch the first time instead. Per-eye passes (EyeClipImmediate) skip safely.
      const bool waitForBuild = eyeMode != EyeClipImmediate || blocking_pipelines();
      cache.multiviewPipelineRefs[eyeMode] = gfx::pipeline_ref(eyeConfig, waitForBuild);
      cache.multiviewPipelineMask |= modeBit;
    }
    multiviewPipeline = cache.multiviewPipelineRefs[eyeMode];
  }
  if (cache.config.shaderConfig.fogRangeEnabled) {
    const auto key = fog_range_lut_key();
    if (!cache.hasFogRange || cache.fogRangeKey != key) {
      cache.fogRange = push_fog_range_lut(key);
      if (drop_overflowed_draw(cache.fogRange, "storage")) {
        cache.hasFogRange = false;
        return;
      }
      cache.fogRangeKey = key;
      cache.hasFogRange = true;
    }
  }
  immediates.fogRangeBase = cache.fogRange.offset / sizeof(u32);
  // MultiviewFull: an eye pair (element view_index of it) or one uniform for both eyes.
  set_eye_mask(immediates, cache.stereoUniformOffsets[0] != cache.stereoUniformOffsets[1] ? 1 : 0);

  state.dirty &= ~DirtyImmediates;

  uint32_t instanceCount = 1;
  if (prim == GX_LINES) {
    instanceCount = vtxCount / 2;
  } else if (prim == GX_LINESTRIP) {
    instanceCount = vtxCount - 1;
  } else if (prim == GX_POINTS) {
    instanceCount = vtxCount;
  }
  cache.lastDrawFmt = fmt;
  const DrawData draw{
      .pipeline = cache.pipelineRef,
      .vertRange = vertRange,
      .idxRange = idxRange,
      .uniformRange = cache.uniformRange,
      .immediateData = immediates,
      .vtxCount = vtxCount,
      .indexCount = numIndices,
      .instanceCount = instanceCount,
      .bindGroups = cache.bindGroups,
      .dstAlpha = state.dstAlpha,
      .stereoUniformOffset = cache.stereoUniformOffsets,
      .cachedGeometry = cachedGeometry,
      .nativeVertices = cache.config.shaderConfig.nativeVertices != 0,
      .textureLayout = texture_layout(cache.config.shaderConfig),
      .stereoTextureBindGroup = cache.stereoBindGroups,
      .multiviewPipeline = multiviewPipeline,
  };
  if (cache.prepassSkip) {
    return;
  }
  if (!state.shadowCasterOnly) {
    gfx::push_draw_command(draw);
  }
  if (cache.shadowCaster) {
    DrawData caster = draw;
    caster.pipeline = cache.shadowPipelineRef;
    caster.bindGroups = {};
    // The PBR mode's cut bit (512): the colour pass discards the pixel, so it casts no shadow either.
    if (cache.cutCaster && pbr_cut_mode(state.pbrBacklight)) {
      caster.pipeline = cache.cutPipelineRef;
      caster.bindGroups = cache.cutBindGroups;
    }
    if (state.actorLayer >= 0) {
      // The node's own cull, as Remastered's RenderShadowMaps leaves it to the material. The map's
      // frame is mirrored (z = -world z), which flips the winding: back and front swap. A cutout's
      // caster (shadowPass 2) alpha-tests the same way and takes the same cull.
      PipelineConfig actorConfig = cache.config;
      actorConfig.shadowPass = caster.pipeline == cache.shadowPipelineRef ? 1 : 2;
      if (actorConfig.cullMode == GX_CULL_BACK) {
        actorConfig.cullMode = GX_CULL_FRONT;
      } else if (actorConfig.cullMode == GX_CULL_FRONT) {
        actorConfig.cullMode = GX_CULL_BACK;
      }
      actorConfig.msaaSamples = 1;
      caster.pipeline = gfx::pipeline_ref(actorConfig);
      gfx::actor_shadow::add_caster(static_cast<uint32_t>(state.actorLayer), caster);
    } else {
      gfx::shadow::add_caster(caster);
    }
  }
  if (perfOn) {
    const auto t3 = gfx::perf::tick();
    gfx::perf::count(gfx::perf::g_fifoNativeDraws, state.nativeVertices);
    gfx::perf::add(gfx::perf::g_fifoPipelineTicks, t0, tA);
    gfx::perf::add(gfx::perf::g_fifoBindsTicks, tA, t1);
    gfx::perf::add(gfx::perf::g_fifoUniformTicks, t1, t2);
    gfx::perf::add(gfx::perf::g_fifoPushTicks, t2, t3);
    gfx::perf::g_fifoDrawsPushed.fetch_add(1, std::memory_order_relaxed);
  }
}

static void handle_draw_unmerged(GXPrimitive prim, GXVtxFmt fmt, u16 vtxCount, gfx::Range vertRange) noexcept {
  ZoneScoped;
  u32 numIndices = 0;
  gfx::Range idxRange;

  if (prim != GX_TRIANGLES) {
    ZoneScopedN("build idx buffer");
    const gfx::perf::Bucket bucket{gfx::perf::g_fifoVertsTicks, gfx::perf::enabled()};
    static ByteBuffer idxBuf;
    numIndices = prepare_idx_buffer(idxBuf, prim, 0, vtxCount);
    idxRange = gfx::push_indices(idxBuf.data(), idxBuf.size(), 4);
    idxBuf.clear();
    if (gfx::overflowed(idxRange)) { note_skipped_draw(); return; }
  }

  push_gx_draw(prim, fmt, vtxCount, vertRange, idxRange, numIndices);
}

// Index staging for a merged draw: `count` more list indices, a new range
// 4-byte aligned, its staged offset in `offset`.
static u16* reserve_staged_indices(ByteBuffer& indices, u32 count, bool newRange, u32& offset) noexcept {
  if (newRange) {
    const size_t aligned = AURORA_ALIGN(indices.size(), 4);
    if (aligned > indices.size()) {
      indices.append_zeroes(aligned - indices.size());
    }
  }
  offset = static_cast<u32>(indices.size());
  return reinterpret_cast<u16*>(indices.append_uninitialized(static_cast<size_t>(count) * sizeof(u16)));
}

// Counts why a draw could not join the last one (perf_counters.hpp MergeBreak).
static void count_merge_breaks(bool formatOrModeDiffers, bool noLastDraw) noexcept {
  if (!gfx::perf::enabled()) {
    return;
  }
  const u32 dirty = g_gxState.dirty;
  gfx::perf::count_merge_break(gfx::perf::BreakPipeline, (dirty & DirtyPipeline) != 0);
  gfx::perf::count_merge_break(gfx::perf::BreakTextures, (dirty & DirtyTextures) != 0);
  gfx::perf::count_merge_break(gfx::perf::BreakUniform, (dirty & DirtyUniform) != 0);
  gfx::perf::count_merge_break(gfx::perf::BreakImmediates, (dirty & DirtyImmediates) != 0);
  gfx::perf::count_merge_break(gfx::perf::BreakFormat, formatOrModeDiffers);
  gfx::perf::count_merge_break(gfx::perf::BreakNoDraw, dirty == 0 && !formatOrModeDiffers && noLastDraw);
}

// GX register writes can change unused state or temporarily change it and put
// it back. Only the final shader, sampled textures and uniform bytes need to
// agree to concatenate vertices. Keep submission order and all pass boundaries.
static void reconcile_merge_state(GXPrimitive prim, GXVtxFmt fmt) noexcept {
  auto& state = g_gxState;
  auto& cache = sDrawCache;
  if (!cache.hasPipeline || cache.uniformRange.size == 0 || prim == GX_LINESTRIP ||
      gfx::get_last_draw_command<DrawData>() == nullptr) {
    return;
  }
  const auto plane = gfx::stereo_head_locked_plane();
  const auto screen = gfx::stereo_screen_tex_mtx();
  if (cache.uniformRoute != gfx::stereo_draw_route() ||
      cache.uniformPlane.tanHalfWidth != plane.tanHalfWidth ||
      cache.uniformPlane.tanHalfHeight != plane.tanHalfHeight || cache.uniformPlane.distance != plane.distance ||
      cache.uniformScreenTexMtx.texSlot != screen.texSlot || cache.uniformScreenTexMtx.pnSlot != screen.pnSlot ||
      !(cache.uniformPlacement == gfx::stereo_placement_3d())) {
    state.dirty |= DirtyUniform;
    return;
  }
  if (gfx::stereo_shadow::active() && cache.stereoEpoch != gfx::stereo_shadow::epoch()) {
    state.dirty |= DirtyTextures;
    return;
  }
  if ((state.dirty & DirtyPipeline) != 0 || cache.fmt != fmt || cache.lineMode != line_mode_for_prim(prim)) {
    PipelineConfig candidate{};
    populate_pipeline_config(candidate, prim, fmt);
    if (std::memcmp(&candidate, &cache.config, sizeof(candidate)) != 0) {
      return;
    }
    state.dirty &= ~DirtyPipeline;
    cache.fmt = fmt;
    cache.lastDrawFmt = fmt;
  }
  if ((state.dirty & DirtyTextures) != 0 || cache.bindGeneration != texture::current_bind_generation()) {
    state.dirty |= DirtyTextures;
    resolve_sampled_textures(cache.shaderInfo);
    const auto groups = cached_bind_groups(cache.shaderInfo);
    if (groups.textureBindGroup != cache.bindGroups.textureBindGroup) {
      return;
    }
    cache.bindGeneration = texture::current_bind_generation();
    state.dirty &= ~DirtyTextures;
    // Even an unchanged texture binding may have new logical dimensions or
    // LOD bias. These live in the uniform, not in the bind-group descriptor.
    state.dirty |= DirtyUniform;
  }
  if ((state.dirty & DirtyUniform) != 0 && uniform_matches(cache.shaderInfo, cache.uniformBytes)) {
    state.dirty &= ~DirtyUniform;
  }
}

// `cmd` is the FIFO draw command, whose following commands of the same kind
// merge along; 0 for a draw delivered otherwise (GX_AURORA_DRAW_SIZED).
static void draw_prim(u8 cmd, GXPrimitive prim, GXVtxFmt fmt, u16 vtxCount, ByteReader& reader) noexcept {
  ZoneScoped;
  select_native_vertices(false);
  u32 vtxSize;
  if (g_gxState.lastVtxFmt == fmt)
    LIKELY { vtxSize = g_gxState.lastVtxSize; }
  else
    UNLIKELY { vtxSize = calc_vtx_size(fmt); }

  u32 totalVtxBytes = vtxCount * vtxSize;
  if (totalVtxBytes > reader.remaining())
    UNLIKELY { handle_draw_overrun(totalVtxBytes, reader); }

  // A draw whose indices read past its arrays reads garbage, or crashes where nothing bounds the
  // read (807013b1e): dropped instead. Checked before a merge, whose vertices it would otherwise add.
  if (check_indexed_arrays(reader.data() + reader.offset(), fmt, prim, vtxCount, vtxSize)) UNLIKELY {
    reader.skip(totalVtxBytes);
    return;
  }

  // One that makes no whole triangle draws nothing on GX (a 2-vertex strip, as Trilogy's scan
  // lens draws for a one-degree run); kept, it would be a draw without indices that a following
  // merge extends as if its vertices were triangles.
  const bool trianglePrim = prim != GX_LINES && prim != GX_LINESTRIP && prim != GX_POINTS;
  if (trianglePrim && vtxCount < (prim == GX_QUADS ? 4 : 3)) UNLIKELY {
    reader.skip(totalVtxBytes);
    return;
  }

  // The GPU expands each strip segment to an independent quad already. Give it
  // independent endpoint pairs so adjacent strips can share one instanced draw
  // without creating a segment between strips. Expand the FIFO records before
  // de-indexing, preserving all per-vertex attributes and their byte order.
  if (prim == GX_LINESTRIP) {
    const auto source = reader.take(totalVtxBytes);
    static ByteBuffer pairs;
    for (u32 first = 0; first + 1 < vtxCount;) {
      const u32 segments = std::min<u32>(vtxCount - first - 1, UINT16_MAX / 2);
      pairs.clear();
      auto* dst = pairs.append_uninitialized(segments * 2 * vtxSize);
      for (u32 segment = 0; segment < segments; ++segment) {
        std::memcpy(dst + segment * 2 * vtxSize, source.data() + (first + segment) * vtxSize, 2 * vtxSize);
      }
      ByteReader pairReader{pairs.data(), pairs.size()};
      draw_prim(0, GX_LINES, fmt, static_cast<u16>(segments * 2), pairReader);
      first += segments;
    }
    return;
  }

  // Resolve the plan before clearing array dirtiness: after de-indexing, new
  // array addresses/base indices change the copied records, not the GPU state.
  const DeindexPlan* const plan = g_gxState.deindexVertices ? &deindex_plan(fmt) : nullptr;
  if (g_gxState.dirty != 0 || fmt != sDrawCache.lastDrawFmt) {
    reconcile_merge_state(prim, fmt);
  }
  const u32 drawDirty = g_gxState.dirty & ~(g_gxState.deindexVertices ? DirtyImmediates : 0u);
  const u8 lineMode = line_mode_for_prim(prim);
  const bool formatDiffers = fmt != sDrawCache.lastDrawFmt || sDrawCache.lineMode != lineMode;
  // Independent lines and points are instances of the same quad. Line strips
  // cannot concatenate: doing so would connect the end of one to the next.
  const bool cleanState = drawDirty == 0 && !g_gxState.shadowCasterOnly && !sDrawCache.prepassSkip && g_gxState.actorLayer < 0 &&
                          !formatDiffers && prim != GX_LINESTRIP;
  auto* lastDraw = cleanState ? gfx::get_last_draw_command<DrawData>() : nullptr;
  ByteBuffer* const verts = lastDraw != nullptr ? gfx::staging_verts() : nullptr;
  ByteBuffer* const indices = lastDraw != nullptr ? gfx::staging_indices() : nullptr;
  const bool canMerge = lastDraw != nullptr && !lastDraw->cachedGeometry &&
                        (lineMode != 0 || lastDraw->instanceCount == 1) &&
                        lastDraw->immediateData.currentPnMtx == g_gxState.currentPnMtx && verts != nullptr &&
                        indices != nullptr &&
                        gfx::frame_buffer_sizes().base(gfx::StagingKind::Vertex) == 0 &&
                        gfx::frame_buffer_sizes().base(gfx::StagingKind::Index) == 0 &&
                        lastDraw->vertRange.offset + lastDraw->vertRange.size == verts->size() &&
                        (lastDraw->idxRange.size == 0 ||
                         lastDraw->idxRange.offset + lastDraw->idxRange.size == indices->size()) &&
                        lastDraw->vtxCount + vtxCount <= UINT16_MAX &&
                        (prim != GX_LINES || (lastDraw->vtxCount % 2 == 0 && vtxCount % 2 == 0)) &&
                        (prim != GX_TRIANGLES || ((lastDraw->indexCount != 0 || lastDraw->vtxCount % 3 == 0) &&
                                                  vtxCount % 3 == 0));
  const bool perfOn = gfx::perf::enabled();
  const auto t0 = gfx::perf::stamp(perfOn);
  const bool deindex = plan != nullptr && plan->active;

  if (!canMerge) {
    count_merge_breaks(formatDiffers || prim == GX_LINESTRIP, lastDraw == nullptr);
    const auto vertexData = reader.take(totalVtxBytes);
    const auto vertRange = push_vertex_records(plan, vertexData, vtxCount, 4);
    if (perfOn) {
      gfx::perf::add(gfx::perf::g_fifoVertsTicks, t0, gfx::perf::tick());
    }
    if (!vertRange) {
      return;
    }
    handle_draw_unmerged(prim, fmt, vtxCount, *vertRange);
    return;
  }

  // Merge into the previous draw. Prime's world surfaces are thousands of short
  // strips a frame, so this appends to the staging buffers directly, and takes
  // the strips that follow under the same command in the same loop.
  g_gxState.dirty &= ~DirtyImmediates;
  u32 merged = 0;
  while (true) {
    const auto vertexData = reader.take(totalVtxBytes);
    CHECK(lastDraw->vertRange.offset + lastDraw->vertRange.size == verts->size(),
          "Non-consecutive vertex ranges ({} < {})", lastDraw->vertRange.offset + lastDraw->vertRange.size,
          verts->size());
    const u32 stagedBytes = deindex ? vtxCount * plan->dstStride : totalVtxBytes;
    // This strip's records and, when indexed, its list indices (the first time, the previous
    // draw's own as well): a strip the frame's staging cannot hold is skipped.
    const size_t indexBytes =
        lineMode != 0 ? 0
                      : (static_cast<size_t>(list_index_count(prim, vtxCount)) + lastDraw->vtxCount) * sizeof(u16) + 4;
    if (!staging_room(verts, stagedBytes) || !staging_room(indices, indexBytes)) {
      const auto range = push_vertex_records(plan, vertexData, vtxCount, 4);
      if (range) {
        handle_draw_unmerged(prim, fmt, vtxCount, *range);
      }
      break;
    } else {
      if (deindex) {
        const gfx::perf::Bucket bucket{gfx::perf::g_fifoDeindexTicks, perfOn};
        deindex_records(*plan, vertexData.data(), vtxCount, verts->append_uninitialized(stagedBytes));
        if (perfOn) {
          gfx::perf::g_fifoDeindexedVerts.fetch_add(vtxCount, std::memory_order_relaxed);
        }
      } else if (totalVtxBytes > 0) {
        std::memcpy(verts->append_uninitialized(totalVtxBytes), vertexData.data(), totalVtxBytes);
      }
      lastDraw->vertRange.size += stagedBytes;
      if (lineMode != 0) {
        // Keep the existing quad indices; instances address the appended points
        // or independent pairs relative to the original draw's vertex start.
        lastDraw->instanceCount += prim == GX_LINES ? vtxCount / 2 : vtxCount;
      } else if (lastDraw->indexCount == 0 && prim != GX_TRIANGLES) {
        // The previous draw's own triangles take their list indices first
        const u32 count = list_index_count(GX_TRIANGLES, static_cast<u16>(lastDraw->vtxCount));
        u32 offset = 0;
        write_list_indices(reserve_staged_indices(*indices, count, true, offset), GX_TRIANGLES, 0,
                           static_cast<u16>(lastDraw->vtxCount));
        lastDraw->idxRange = gfx::Range{offset, static_cast<u32>(count * sizeof(u16))};
        lastDraw->indexCount = count;
      }
      if (lineMode == 0 && lastDraw->indexCount != 0) {
        const u32 count = list_index_count(prim, vtxCount);
        const bool newRange = lastDraw->idxRange.size == 0;
        u32 offset = 0;
        u16* const out = reserve_staged_indices(*indices, count, newRange, offset);
        write_list_indices(out, prim, static_cast<u16>(lastDraw->vtxCount), vtxCount);
        if (newRange) {
          lastDraw->idxRange = gfx::Range{offset, static_cast<u32>(count * sizeof(u16))};
        } else {
          CHECK(lastDraw->idxRange.offset + lastDraw->idxRange.size == offset, "Non-consecutive index ranges ({} < {})",
                lastDraw->idxRange.offset + lastDraw->idxRange.size, offset);
          lastDraw->idxRange.size += static_cast<u32>(count * sizeof(u16));
        }
        lastDraw->indexCount += count;
      }
      lastDraw->vtxCount += vtxCount;
      ++merged;
    }

    // The next command, when it is another strip of this kind
    if (cmd == 0 || reader.remaining() < 3 || reader.data()[reader.offset()] != cmd) {
      break;
    }
    const u16 nextCount = read_bits<u16>(reader.data() + reader.offset() + 1);
    const u32 nextBytes = nextCount * vtxSize;
    if (nextCount == 0 || lastDraw->vtxCount + nextCount > UINT16_MAX || nextBytes > reader.remaining() - 3 ||
        (prim == GX_LINES && nextCount % 2 != 0) || (prim == GX_TRIANGLES && nextCount % 3 != 0)) {
      break;
    }
    reader.skip(3);
    vtxCount = nextCount;
    totalVtxBytes = nextBytes;
  }
  gfx::detail::increment_merged_draw_count(merged);
  if (perfOn) {
    gfx::perf::add(gfx::perf::g_fifoVertsTicks, t0, gfx::perf::tick());
    gfx::perf::g_fifoDrawsMerged.fetch_add(merged, std::memory_order_relaxed);
  }
}

// The layout a cached surface's records are resolved with (geometry_cache::Key::layout):
// the format, the arrays and where each attribute lands.
static u64 deindex_plan_hash(const DeindexPlan& plan) noexcept {
  u64 hash = 1469598103934665603ull;
  const auto mix = [&hash](u64 value) {
    hash ^= value;
    hash *= 1099511628211ull;
  };
  mix(plan.fmt);
  mix(plan.srcStride);
  mix(plan.dstStride);
  mix(plan.count);
  for (u32 i = 0; i < plan.count; ++i) {
    const auto& a = plan.attrs[i];
    mix(reinterpret_cast<uintptr_t>(a.data));
    mix(a.size);
    mix(a.stride);
    mix(a.base);
    mix(a.srcOffset);
    mix(a.dstOffset);
    mix(a.bytes);
    mix(a.indexSize);
    mix(a.indices);
  }
  return hash;
}

// A primitive's triangle-list indices, 32-bit, from vertex `base` on.
static void append_list_indices(std::vector<u32>& out, GXPrimitive prim, u32 base, u16 vtxCount) noexcept {
  switch (prim) {
  case GX_QUADS:
    for (u32 v = 0; v + 4 <= vtxCount; v += 4) {
      out.insert(out.end(), {base + v, base + v + 1, base + v + 2, base + v + 2, base + v + 3, base + v});
    }
    break;
  case GX_TRIANGLES:
    for (u32 v = 0; v + 3 <= vtxCount; v += 3) {
      out.insert(out.end(), {base + v, base + v + 1, base + v + 2});
    }
    break;
  case GX_TRIANGLEFAN:
    for (u32 v = 2; v < vtxCount; ++v) {
      out.insert(out.end(), {base, base + v - 1, base + v});
    }
    break;
  case GX_TRIANGLESTRIP:
    for (u32 v = 2; v < vtxCount; ++v) {
      if ((v & 1) == 0) {
        out.insert(out.end(), {base + v - 2, base + v - 1, base + v});
      } else {
        out.insert(out.end(), {base + v - 1, base + v - 2, base + v});
      }
    }
    break;
  default:
    break;
  }
}

// A display list resolved for the cache: its draws' records de-indexed one after the
// other (deindex_records), their triangle-list indices from the first record. False
// when it holds anything but triangle draws of `fmt`.
static bool resolve_display_list(const u8* data, u32 size, GXVtxFmt fmt, const DeindexPlan& plan,
                                 ByteBuffer& records, std::vector<u32>& indices) noexcept {
  u32 vertexCount = 0;
  size_t pos = 0;
  while (pos < size) {
    const u8 cmd = data[pos];
    if (cmd == CP_CMD_NOP) {
      ++pos;
      continue;
    }
    if ((cmd & 0x80) == 0 || (cmd & CP_VAT_MASK) != fmt || size - pos < 3) {
      return false;
    }
    const auto prim = static_cast<GXPrimitive>(cmd & CP_OPCODE_MASK);
    if (prim != GX_QUADS && prim != GX_TRIANGLES && prim != GX_TRIANGLESTRIP && prim != GX_TRIANGLEFAN) {
      return false;
    }
    const u16 count = read_bits<u16>(data + pos + 1);
    const size_t bytes = static_cast<size_t>(count) * plan.srcStride;
    pos += 3;
    if (bytes > size - pos) {
      return false;
    }
    if (count != 0) {
      deindex_records(plan, data + pos, count, records.append_uninitialized(static_cast<size_t>(count) * plan.dstStride));
      append_list_indices(indices, prim, vertexCount, count);
      vertexCount += count;
    }
    pos += bytes;
  }
  return vertexCount != 0 && !indices.empty();
}

// A cached surface's draw: its indices into the frame's index staging, joined to the
// last draw when that one reads the cache under the same state (draw_prim's rules,
// without the vertex staging: the records are in the geometry buffer, vtxStart 0).
static void draw_cached_geometry(GXVtxFmt fmt, const geometry_cache::Entry& entry) noexcept {
  constexpr GXPrimitive prim = GX_TRIANGLES;
  if (g_gxState.dirty != 0 || fmt != sDrawCache.lastDrawFmt) {
    reconcile_merge_state(prim, fmt);
  }
  // The immediates do not matter: vtxStart is 0 and the arrays are in the records.
  const u32 drawDirty = g_gxState.dirty & ~DirtyImmediates;
  const bool formatDiffers = fmt != sDrawCache.lastDrawFmt || sDrawCache.lineMode != 0;
  auto* lastDraw = drawDirty == 0 && !formatDiffers ? gfx::get_last_draw_command<DrawData>() : nullptr;
  ByteBuffer* const indices = gfx::staging_indices();
  const bool canMerge = lastDraw != nullptr && lastDraw->cachedGeometry && lastDraw->instanceCount == 1 &&
                        lastDraw->immediateData.currentPnMtx == g_gxState.currentPnMtx && indices != nullptr &&
                        gfx::frame_buffer_sizes().base(gfx::StagingKind::Index) == 0 &&
                        lastDraw->idxRange.offset + lastDraw->idxRange.size == indices->size() &&
                        staging_room(indices, entry.indices.size() * sizeof(u32));
  const bool perfOn = gfx::perf::enabled();
  const auto t0 = gfx::perf::stamp(perfOn);
  const size_t bytes = entry.indices.size() * sizeof(u32);
  if (!canMerge) {
    count_merge_breaks(formatDiffers, lastDraw == nullptr);
    const gfx::Range idxRange = gfx::push_indices(reinterpret_cast<const u8*>(entry.indices.data()), bytes, 4);
    if (perfOn) {
      gfx::perf::add(gfx::perf::g_fifoVertsTicks, t0, gfx::perf::tick());
    }
    if (gfx::overflowed(idxRange)) { note_skipped_draw(); return; }
    push_gx_draw(prim, fmt, entry.vertexCount, gfx::Range{0, entry.bytes}, idxRange,
                 static_cast<u32>(entry.indices.size()), true);
    return;
  }
  g_gxState.dirty &= ~DirtyImmediates;
  CHECK(lastDraw->idxRange.offset + lastDraw->idxRange.size == indices->size(),
        "Non-consecutive index ranges ({} < {})", lastDraw->idxRange.offset + lastDraw->idxRange.size,
        indices->size());
  std::memcpy(indices->append_uninitialized(bytes), entry.indices.data(), bytes);
  lastDraw->idxRange.size += static_cast<u32>(bytes);
  lastDraw->indexCount += static_cast<u32>(entry.indices.size());
  lastDraw->vtxCount += entry.vertexCount;
  gfx::detail::increment_merged_draw_count(1);
  if (perfOn) {
    gfx::perf::add(gfx::perf::g_fifoVertsTicks, t0, gfx::perf::tick());
    gfx::perf::g_fifoDrawsMerged.fetch_add(1, std::memory_order_relaxed);
  }
}

// Whether a cached surface's records take native vertex input (gx/native_vertex.hpp)
// depends only on what populate_pipeline_config lays its attributes out from: the
// vertex descriptor, the format's attribute formats, the arrays' byte order and the
// de-indexing and map-batch modes. Thousands of cached display lists a frame share a
// handful of these, so the decision is made once per combination.
struct NativeVertexInputs {
  std::array<u8, GX_VA_TEX7 + 1> type{};
  std::array<u8, GX_VA_TEX7 + 1> cnt{};
  std::array<u8, GX_VA_TEX7 + 1> comp{};
  std::array<u8, GX_VA_TEX7 + 1> frac{};
  std::array<u8, GX_VA_TEX7 + 1> le{};
  u8 fmt = 0;
  u8 deindex = 0;
  u8 mapBatch = 0;
  u8 pad = 0;

  bool operator==(const NativeVertexInputs& rhs) const { return std::memcmp(this, &rhs, sizeof(*this)) == 0; }
};
static_assert(std::has_unique_object_representations_v<NativeVertexInputs>);
struct NativeVertexChoice {
  bool native = false;
  u32 vtxStride = 0;
  u64 layoutSalt = 0;    // into the geometry cache key's layout when native
  ShaderConfig source{}; // its attributes and stride, for native_vertex::convert
};
struct NativeVertexMemo {
  NativeVertexInputs last{};
  const NativeVertexChoice* lastChoice = nullptr;
  absl::flat_hash_map<u64, std::pair<NativeVertexInputs, NativeVertexChoice>> choices;
};
NativeVertexMemo sNativeVertexMemo;

static NativeVertexInputs native_vertex_inputs(GXVtxFmt fmt) noexcept {
  NativeVertexInputs in;
  const auto& vtxFmt = g_gxState.vtxFmts[fmt];
  for (int i = GX_VA_PNMTXIDX; i <= GX_VA_TEX7; ++i) {
    const auto type = g_gxState.vtxDesc[i];
    in.type[i] = static_cast<u8>(type);
    if (type == GX_NONE) {
      continue;
    }
    const auto& attrFmt = vtxFmt.attrs[i];
    in.cnt[i] = static_cast<u8>(attrFmt.cnt);
    in.comp[i] = static_cast<u8>(attrFmt.type);
    in.frac[i] = attrFmt.frac;
    in.le[i] = type != GX_DIRECT && g_gxState.arrays[i].le ? 1 : 0;
  }
  in.fmt = static_cast<u8>(fmt);
  in.deindex = g_gxState.deindexVertices ? 1 : 0;
  in.mapBatch = g_gxState.mapBatch ? 1 : 0;
  return in;
}

static const NativeVertexChoice& native_vertex_choice(GXVtxFmt fmt) noexcept {
  auto& memo = sNativeVertexMemo;
  const NativeVertexInputs inputs = native_vertex_inputs(fmt);
  if (memo.lastChoice != nullptr && inputs == memo.last) {
    return *memo.lastChoice;
  }
  const u64 hash = xxh3_hash(inputs);
  auto it = memo.choices.find(hash);
  if (it == memo.choices.end() || !(it->second.first == inputs)) {
    PipelineConfig source{};
    populate_pipeline_config(source, GX_TRIANGLES, fmt);
    NativeVertexChoice choice;
    choice.native = native_vertex::layout(source.shaderConfig).valid;
    choice.vtxStride = source.shaderConfig.vtxStride;
    if (choice.native) {
      choice.layoutSalt = xxh3_hash(source.shaderConfig.attrs) ^ 0x4e41544956455631ull;
      choice.source.attrs = source.shaderConfig.attrs;
      choice.source.vtxStride = source.shaderConfig.vtxStride;
    }
    // A hash collision replaces the other combination; it is decided again when it returns.
    it = memo.choices.insert_or_assign(hash, std::pair{inputs, choice}).first;
  }
  // Only this function inserts, and it re-points the last choice after doing so: the
  // pointer never outlives a rehash of the map's values.
  memo.last = inputs;
  memo.lastChoice = &it->second.second;
  return *memo.lastChoice;
}

void clear_native_vertex_choices() noexcept {
  sNativeVertexMemo.lastChoice = nullptr;
  sNativeVertexMemo.choices.clear();
}

// A static world surface's display list (GX_AURORA_CALL_CACHED_DL), read where it
// lives: drawn from the geometry cache, resolved into it first when new, or
// processed in place when the cache cannot take it.
static void handle_cached_display_list(ByteReader& reader) noexcept {
  const u32 set = reader.read<u32>();
  const auto address = static_cast<uintptr_t>(reader.read<u64>());
  const u32 size = reader.read<u32>();
  if (address == 0 || size == 0) {
    return;
  }
  const auto* const data = reinterpret_cast<const u8*>(address);
  // The records' layout follows the first draw's format
  size_t first = 0;
  while (first < size && data[first] == CP_CMD_NOP) {
    ++first;
  }
  const bool deindex = g_gxState.deindexVertices && first < size && (data[first] & 0x80) != 0;
  const auto fmt = static_cast<GXVtxFmt>(deindex ? data[first] & CP_VAT_MASK : 0);
  const DeindexPlan* const plan = deindex ? &deindex_plan(fmt) : nullptr;
  if (plan == nullptr || !plan->active || gfx::staging_indices() == nullptr) {
    process_display_list_in_place(data, size);
    return;
  }
  bool native = false;
  u64 layoutHash = deindex_plan_hash(*plan);
  if (g_gxState.nativeVertexInputRequested) {
    const NativeVertexChoice& choice = native_vertex_choice(fmt);
    native = choice.native && choice.vtxStride == plan->dstStride;
    if (native) {
      // Numeric encoding and source byte order affect the converted bytes too.
      layoutHash ^= choice.layoutSalt;
    }
  }
  const geometry_cache::Key key{.set = set, .address = static_cast<u64>(address), .layout = layoutHash};
  const geometry_cache::Entry* entry = geometry_cache::find(key);
  if (entry == nullptr) {
    static ByteBuffer records;
    static std::vector<u32> indices;
    records.clear();
    indices.clear();
    if (!resolve_display_list(data, size, fmt, *plan, records, indices)) {
      process_display_list_in_place(data, size);
      return;
    }
    // New surfaces upload through the frame's vertex staging (gfx::queue_geometry_upload):
    // a burst of them (an area coming into view) leaves a quarter of it to the frame's
    // plain draws and is cached over the next frames, drawn in place meanwhile.
    const ByteBuffer* const verts = gfx::staging_verts();
    if (verts != nullptr && !staging_room(verts, records.size(), verts->owned() ? 0 : verts->capacity() / 4)) {
      ++sStaging.deferredUploads;
      sStaging.deferredBytes += records.size();
      process_display_list_in_place(data, size);
      return;
    }
    if (native && !native_vertex::convert(native_vertex_choice(fmt).source, {records.data(), records.size()})) {
      process_display_list_in_place(data, size);
      return;
    }
    entry = geometry_cache::insert(key, plan->dstStride, {records.data(), records.size()}, indices);
    if (entry == nullptr) {
      process_display_list_in_place(data, size);
      return;
    }
    gfx::perf::g_fifoGeometryMisses.fetch_add(1, std::memory_order_relaxed);
  } else {
    gfx::perf::g_fifoGeometryHits.fetch_add(1, std::memory_order_relaxed);
  }
  select_native_vertices(native);
  draw_cached_geometry(fmt, *entry);
}

// How many indices prepare_idx_buffer makes of a draw, or 0 for one it cannot make into
// whole triangles.
static u32 triangle_index_count(GXPrimitive prim, u16 vtxCount) noexcept {
  switch (prim) {
  case GX_TRIANGLES:
    return vtxCount >= 3 && vtxCount % 3 == 0 ? vtxCount : 0;
  case GX_QUADS:
    return vtxCount >= 4 && vtxCount % 4 == 0 ? vtxCount / 4 * 6 : 0;
  case GX_TRIANGLESTRIP:
  case GX_TRIANGLEFAN:
    return vtxCount >= 3 ? (u32(vtxCount) - 2) * 3 : 0;
  default:
    return 0;
  }
}

static u32 current_vtx_size(GXVtxFmt fmt) noexcept {
  return g_gxState.lastVtxFmt == fmt ? g_gxState.lastVtxSize : calc_vtx_size(fmt);
}

// Makes a retained display list into triangle lists in the resident buffers, as the
// processor would draw it now (merged draws, with their index buffers). False when it holds
// anything but triangle draws, or there is no room for it.
static bool build_resident_dl(resident::Entry& entry) noexcept {
  ZoneScoped;
  const std::vector<u8>& bytes = *entry.bytes;
  std::array<u32, GX_MAX_VTXFMT> sizes{};
  std::vector<resident::Chunk> chunks;
  std::vector<u8> verts;
  std::vector<u8> indices;
  ByteBuffer idxBuf;
  size_t pos = 0;
  while (pos < bytes.size()) {
    const u8 cmd = bytes[pos++];
    if (cmd == CP_CMD_NOP) {
      continue;
    }
    if (cmd < 0x80 || bytes.size() - pos < 2) {
      return false;
    }
    const auto fmt = static_cast<GXVtxFmt>(cmd & CP_VAT_MASK);
    const auto prim = static_cast<GXPrimitive>(cmd & CP_OPCODE_MASK);
    const u16 vtxCount = static_cast<u16>((bytes[pos] << 8) | bytes[pos + 1]);
    pos += 2;
    const u32 numIndices = triangle_index_count(prim, vtxCount);
    if (numIndices == 0 || numIndices > 0xFFFF) {
      return false;
    }
    if (sizes[fmt] == 0) {
      sizes[fmt] = current_vtx_size(fmt);
    }
    const size_t vtxBytes = size_t(vtxCount) * sizes[fmt];
    if (sizes[fmt] == 0 || vtxBytes > bytes.size() - pos) {
      return false;
    }
    if (chunks.empty() || chunks.back().fmt != fmt || u32(chunks.back().vtxCount) + vtxCount > 0xFFFF) {
      // A new draw starts where push_verts and push_indices would start one: 4-aligned.
      verts.resize(AURORA_ALIGN(verts.size(), 4));
      indices.resize(AURORA_ALIGN(indices.size(), 4));
      chunks.push_back(resident::Chunk{
          .fmt = fmt,
          .vtxCount = 0,
          .vertOffset = static_cast<u32>(verts.size()),
          .vertSize = 0,
          .idxOffset = static_cast<u32>(indices.size()),
          .idxCount = 0,
          .srcOffset = static_cast<u32>(pos),
          .srcContiguous = true,
      });
    }
    resident::Chunk& chunk = chunks.back();
    if (static_cast<u32>(pos) != chunk.srcOffset + chunk.vertSize) {
      // Skipped bytes between the draws: the chunk has no contiguous run to read in the list's bytes.
      chunk.srcContiguous = false;
    }
    verts.insert(verts.end(), bytes.begin() + pos, bytes.begin() + pos + vtxBytes);
    pos += vtxBytes;
    idxBuf.clear();
    const u32 made = prepare_idx_buffer(idxBuf, prim, chunk.vtxCount, vtxCount);
    indices.insert(indices.end(), idxBuf.data(), idxBuf.data() + idxBuf.size());
    chunk.vtxCount = static_cast<u16>(chunk.vtxCount + vtxCount);
    chunk.vertSize += static_cast<u32>(vtxBytes);
    chunk.idxCount += made;
  }
  if (chunks.empty() || !resident::store_dl(entry, verts, indices)) {
    return false;
  }
  entry.vtxSizes = sizes;
  entry.chunks = std::move(chunks);
  return true;
}

static void call_resident_dl(const void* key, u32 size) noexcept {
  ZoneScoped;
  resident::Entry* const entry = resident::find(key);
  if (entry == nullptr || size > entry->bytes->size()) {
    Log.error("GX_AURORA_RESIDENT_CALL_DL: {} is not retained with {} bytes", key, size);
    return;
  }
  // Port: with indexed vertices resolved on the CPU (GXState::deindexVertices) the pipeline reads
  // resolved records, which the retained raw vertices are not: processed as if sent.
  if (g_gxState.deindexVertices) {
    process(entry->bytes->data(), size);
    return;
  }
  if (!entry->chunks.empty()) {
    // Parsed with other vertex sizes: what the vertices are has changed since.
    for (int fmt = 0; fmt < GX_MAX_VTXFMT; ++fmt) {
      const u32 parsed = entry->vtxSizes[fmt];
      if (parsed != 0 && parsed != current_vtx_size(static_cast<GXVtxFmt>(fmt))) {
        resident::free_dl(*entry);
        break;
      }
    }
  }
  if (entry->chunks.empty() && !entry->dlTried) {
    entry->dlTried = true;
    build_resident_dl(*entry);
  }
  if (entry->chunks.empty()) {
    // As GXCallDisplayList would have sent it.
    process(entry->bytes->data(), size);
    return;
  }
  for (const resident::Chunk& chunk : entry->chunks) {
    const gfx::Range vertRange{entry->vert.offset + chunk.vertOffset, chunk.vertSize};
    const gfx::Range idxRange{entry->idx.offset + chunk.idxOffset, chunk.idxCount * u32(sizeof(u16))};
    // The same vertices as they are in the list's bytes, which is what the array check reads.
    const u8* const vtxData = chunk.srcContiguous ? entry->bytes->data() + chunk.srcOffset : nullptr;
    if (check_indexed_arrays(vtxData, chunk.fmt, GX_TRIANGLES, chunk.vtxCount, current_vtx_size(chunk.fmt))) UNLIKELY {
      continue;
    }
    push_gx_draw(GX_TRIANGLES, chunk.fmt, chunk.vtxCount, vertRange, idxRange, chunk.idxCount);
  }
  // The next draw must not merge into these: its vertices are not after them.
  sDrawCache.lastDrawFmt = GX_MAX_VTXFMT;
}

static void handle_draw(u8 cmd, ByteReader& reader) noexcept {
  const auto fmt = static_cast<GXVtxFmt>(cmd & CP_VAT_MASK);
  const auto prim = static_cast<GXPrimitive>(cmd & CP_OPCODE_MASK);
  draw_prim(cmd, prim, fmt, reader.read<u16>(), reader);
}

// Port: PrimedGun and upstream both append subcommand IDs to GXAurora.h. A duplicate still
// compiles (handle_aurora's if-chain takes the first match) but hands the other command's
// payload to the wrong handler, which then misreads the FIFO: fail the build instead.
static constexpr bool aurora_subcommand_ids_unique() noexcept {
  constexpr u16 ids[] = {
      GX_AURORA_LOAD_VIEWPORT_RENDER,     GX_AURORA_LOAD_SCISSOR_RENDER,      GX_AURORA_LOAD_PROJECTION_FULL,
      GX_AURORA_DEBUG_GROUP_PUSH,         GX_AURORA_DEBUG_GROUP_POP,          GX_AURORA_DEBUG_MARKER_INSERT,
      GX_AURORA_SET_DRAW_SYNC,            GX_AURORA_LOAD_TEXOBJ,              GX_AURORA_LOAD_TLUT,
      GX_AURORA_DESTROY_TEXOBJ,           GX_AURORA_DESTROY_TLUT,             GX_AURORA_DESTROY_COPY_TEX,
      GX_AURORA_LOAD_COPY_SRC,            GX_AURORA_LOAD_COPY_DST,            GX_AURORA_LOAD_COPY_DEST,
      GX_AURORA_REQUEST_DEPTH_SNAPSHOT,   GX_AURORA_BEGIN_OFFSCREEN,          GX_AURORA_END_OFFSCREEN,
      GX_AURORA_DRAW_SIZED,               GX_AURORA_DRAW_INDEXED,             GX_AURORA_LOAD_ARRAY_BASE_INDEX,
      GX_AURORA_SET_PBR,                  GX_AURORA_COPY_PROBE_FACE,          GX_AURORA_SET_PBR_PROBE,
      GX_AURORA_SET_PBR_MATERIAL,         GX_AURORA_CREATE_PBR_CUBE,          GX_AURORA_DESTROY_PBR_CUBE,
      GX_AURORA_SET_PBR_CUBE,             GX_AURORA_SET_PBR_AMBIENT,          GX_AURORA_CREATE_PBR_VOLUME,
      GX_AURORA_DESTROY_PBR_VOLUME,       GX_AURORA_SET_PBR_VOLUME,           GX_AURORA_CREATE_PBR_LIGHTMAP,
      GX_AURORA_DESTROY_PBR_LIGHTMAP,     GX_AURORA_SET_PBR_LIGHTMAP,         GX_AURORA_SET_PBR_LIGHTMAP_ATTR,
      GX_AURORA_SET_PBR_TONE,             GX_AURORA_SET_PBR_BRDF_LUT,         GX_AURORA_SET_SDF,
      GX_AURORA_SET_HUD_SAMPLE,           GX_AURORA_SET_DRAW_TAG,             GX_AURORA_SET_PBR_LIGHT_SKIP,
      GX_AURORA_SET_PBR_LIGHT_SCALE,      GX_AURORA_PORT_POST_PROCESS,        GX_AURORA_SET_PBR_LIGHT_HDR,
      GX_AURORA_SET_PBR_BAKED_LIGHT_MODULATION, GX_AURORA_SET_PBR_BACKLIGHT,  GX_AURORA_SET_PBR_SHIELD,
      GX_AURORA_PORT_VOLUMETRIC_FOG,      GX_AURORA_PORT_VOLUMETRIC_FOG_END,  GX_AURORA_PORT_DEPTH_PREPASS,
      GX_AURORA_PORT_DRAW_SERIAL,         GX_AURORA_PORT_DRAW_ID_MODE,        GX_AURORA_RESIDENT_RETAIN,
      GX_AURORA_RESIDENT_RELEASE,         GX_AURORA_RESIDENT_CALL_DL,         GX_AURORA_PORT_SHADOW_CASTER,
      GX_AURORA_PORT_SHADOW_FRAME,        GX_AURORA_PORT_SHADOW_RENDER,       GX_AURORA_PORT_ROOM_LIGHTS,
      GX_AURORA_PORT_PARTICLE_FOG,        GX_AURORA_PORT_XRAY,                GX_AURORA_PORT_THERMAL,
      GX2_SET_POLYGON_OFFSET,
      // PrimedGun's
      GX_AURORA_STEREO_DRAW_ROUTE,        GX_AURORA_STEREO_HEAD_LOCKED_PLANE, GX_AURORA_STEREO_SCREEN_TEX_MTX,
      GX_AURORA_MAP_BATCH,                GX_AURORA_CALL_CACHED_DL,           GX_AURORA_FREE_GEOMETRY_SET,
      GX_AURORA_STEREO_PLACEMENT_3D,
  };
  constexpr size_t count = sizeof(ids) / sizeof(ids[0]);
  for (size_t i = 0; i < count; ++i) {
    // GX_AURORA_LOAD_ARRAYBASE takes the attribute in its low four bits.
    if (ids[i] >= GX_AURORA_LOAD_ARRAYBASE && ids[i] <= (GX_AURORA_LOAD_ARRAYBASE | 0x0f)) {
      return false;
    }
    for (size_t j = i + 1; j < count; ++j) {
      if (ids[i] == ids[j]) {
        return false;
      }
    }
  }
  return true;
}
static_assert(aurora_subcommand_ids_unique(), "GXAurora.h: two Aurora subcommands share an ID");

// PrimedGun: our own subcommands stay in their block (GXAurora.h PRIMEDGUN_AURORA_SUBCMD_FIRST..LAST),
// which upstream's numbering never reaches, so its next IDs cannot land on ours again.
static constexpr bool primedgun_aurora_subcommands_in_block() noexcept {
  constexpr u16 ids[] = {
      GX_AURORA_STEREO_DRAW_ROUTE, GX_AURORA_STEREO_HEAD_LOCKED_PLANE, GX_AURORA_STEREO_SCREEN_TEX_MTX,
      GX_AURORA_MAP_BATCH,         GX_AURORA_CALL_CACHED_DL,           GX_AURORA_FREE_GEOMETRY_SET,
      GX_AURORA_STEREO_PLACEMENT_3D,
  };
  for (const u16 id : ids) {
    if (id < PRIMEDGUN_AURORA_SUBCMD_FIRST || id > PRIMEDGUN_AURORA_SUBCMD_LAST) {
      return false;
    }
  }
  return true;
}
static_assert(primedgun_aurora_subcommands_in_block(),
              "GXAurora.h: PrimedGun's subcommands belong in PRIMEDGUN_AURORA_SUBCMD_FIRST..LAST (0xF000-0xF0FF)");

void handle_aurora(ByteReader& reader) noexcept {
  ZoneScoped;
  const u16 subCmd = reader.read<u16>();

  if (subCmd == GX_AURORA_LOAD_VIEWPORT_RENDER) {
    const f32 left = reader.read<f32>();
    const f32 top = reader.read<f32>();
    const f32 width = reader.read<f32>();
    const f32 height = reader.read<f32>();
    const f32 nearZ = reader.read<f32>();
    const f32 farZ = reader.read<f32>();
    set_render_viewport({
        .left = left,
        .top = top,
        .width = width,
        .height = height,
        .znear = nearZ,
        .zfar = farZ,
    });
  } else if (subCmd == GX_AURORA_LOAD_SCISSOR_RENDER) {
    const s32 left = reader.read<s32>();
    const s32 top = reader.read<s32>();
    const s32 width = reader.read<s32>();
    const s32 height = reader.read<s32>();
    set_render_scissor({left, top, width, height});
  } else if (subCmd == GX_AURORA_LOAD_PROJECTION_FULL) {
    auto& proj = g_gxState.proj;
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 4; ++c) {
        proj[r][c] = reader.read<f32>();
      }
    }
    // Invalidate projection XF regs
    for (u32 reg = 0x20; reg <= 0x26; ++reg) {
      g_gxState.xfRegValid.reset(reg);
    }
    g_gxState.dirty |= DirtyUniform;
  } else if (subCmd == GX_AURORA_MAP_BATCH) {
    const bool enabled = reader.read<u8>() != 0;
    if (g_gxState.mapBatch != enabled) {
      g_gxState.mapBatch = enabled;
      g_gxState.dirty |= DirtyPipeline;
    }
  } else if (subCmd == GX_AURORA_STEREO_DRAW_ROUTE) {
    const u8 route = reader.read<u8>();
    if (route != gfx::stereo_draw_route()) {
      // The eye uniform copies depend on the route, and a draw under a new
      // route must not merge into the previous one.
      g_gxState.dirty |= DirtyUniform;
      gfx::set_stereo_draw_route(route);
    }
  } else if (subCmd == GX_AURORA_STEREO_HEAD_LOCKED_PLANE) {
    const f32 tanHalfWidth = reader.read<f32>();
    const f32 tanHalfHeight = reader.read<f32>();
    const f32 distance = reader.read<f32>();
    const auto current = gfx::stereo_head_locked_plane();
    if (current.tanHalfWidth != tanHalfWidth || current.tanHalfHeight != tanHalfHeight ||
        current.distance != distance) {
      // The eye copies of a 2D draw on the plane depend on it.
      g_gxState.dirty |= DirtyUniform;
      gfx::set_stereo_head_locked_plane(tanHalfWidth, tanHalfHeight, distance);
    }
  } else if (subCmd == GX_AURORA_STEREO_PLACEMENT_3D) {
    gfx::StereoPlacement3D placement;
    placement.scale = reader.read<f32>();
    placement.x = reader.read<f32>();
    placement.y = reader.read<f32>();
    placement.z = reader.read<f32>();
    placement.headX = reader.read<f32>();
    placement.headY = reader.read<f32>();
    placement.headZ = reader.read<f32>();
    placement.follow = reader.read<f32>();
    if (!(placement == gfx::stereo_placement_3d())) {
      // The eye copies of an anchored or floating 3D draw depend on it.
      g_gxState.dirty |= DirtyUniform;
      gfx::set_stereo_placement_3d(placement);
    }
  } else if (subCmd == GX_AURORA_STEREO_SCREEN_TEX_MTX) {
    const u8 texSlot = reader.read<u8>();
    const u8 pnSlot = reader.read<u8>();
    const auto current = gfx::stereo_screen_tex_mtx();
    if (current.texSlot != texSlot || current.pnSlot != pnSlot) {
      // The eye uniform copies depend on it.
      g_gxState.dirty |= DirtyUniform;
      gfx::set_stereo_screen_tex_mtx(texSlot, pnSlot);
    }
  } else if (subCmd >= GX_AURORA_LOAD_ARRAYBASE && subCmd <= (GX_AURORA_LOAD_ARRAYBASE | 0x0f)) {
    const u32 attrIdx = subCmd - GX_AURORA_LOAD_ARRAYBASE + GX_VA_POS;
    const u64 arrayAddr = reader.read<u64>();
    const u32 arraySize = reader.read<u32>();
    const bool le = reader.read<u8>() == 1;

    auto& array = g_gxState.arrays[attrIdx];
    const auto newData = reinterpret_cast<void*>(arrayAddr);
    if (array.data != newData || array.size != arraySize || array.le != le) {
      // Only an indexed attribute's array, or TEX7's under pbr (bind_pos_active), is in the
      // pipeline config (a VCD or pbr change dirties it anyway).
      const auto type = g_gxState.vtxDesc[attrIdx];
      const bool inConfig =
          type == GX_INDEX8 || type == GX_INDEX16 || (attrIdx == GX_VA_TEX7 && g_gxState.pbr != 0);
      if (inConfig &&
          (array.le != le || (attrIdx == GX_VA_TEX7 && (array.data == nullptr) != (newData == nullptr)))) {
        // Endianness is baked into the shader
        g_gxState.dirty |= DirtyPipeline;
      }
      array.data = newData;
      array.size = arraySize;
      array.le = le;
      array.cachedRange = {};
      g_gxState.dirty |= DirtyImmediates;
    }
  } else if (subCmd == GX_AURORA_LOAD_TEXOBJ) {
    const auto texMapId = reader.read<u8>();
    CHECK(texMapId < MaxTextures, "invalid texture map id {}", texMapId);
    auto& slot = g_gxState.loadedTextures[texMapId];
    const auto newData = reinterpret_cast<const void*>(reader.read<u64>());
    const auto newUserData = reinterpret_cast<const void*>(reader.read<u64>());
    const u32 newWidth = reader.read<u32>();
    const u32 newHeight = reader.read<u32>();
    const auto newFormat = static_cast<GXTexFmt>(reader.read<u32>());
    const auto newTlut = static_cast<GXTlut>(reader.read<u32>());
    u8 newFlags = slot.flags & ~0x80u; // Reset no-cache flag
    if (reader.read<u8>() != 0) {
      newFlags |= 1u;
    } else {
      newFlags &= ~1u;
    }
    const u32 newTexObjId = reader.read<u32>();
    const u32 newTexDataVersion = reader.read<u32>();
    if (slot.data != newData || slot.userData != newUserData || slot.mWidth != newWidth || slot.mHeight != newHeight ||
        slot.mFormat != static_cast<u32>(newFormat) || slot.tlut != newTlut || slot.flags != newFlags ||
        slot.texObjId != newTexObjId || slot.texDataVersion != newTexDataVersion) {
      slot.data = newData;
      slot.userData = newUserData;
      slot.mWidth = newWidth;
      slot.mHeight = newHeight;
      slot.mFormat = newFormat;
      slot.tlut = newTlut;
      slot.flags = newFlags;
      slot.texObjId = newTexObjId;
      slot.texDataVersion = newTexDataVersion;
      g_gxState.dirty |= DirtyTextures;
    }
  } else if (subCmd == GX_AURORA_LOAD_TLUT) {
    const auto idx = reader.read<u8>();
    CHECK(idx < MaxTluts, "invalid tlut slot {}", idx);
    auto& slot = g_gxState.loadedTluts[idx];
    const auto newData = reinterpret_cast<const void*>(reader.read<u64>());
    const auto newFormat = static_cast<GXTlutFmt>(reader.read<u32>());
    const u16 newNumEntries = reader.read<u16>();
    const u32 newTlutObjId = reader.read<u32>();
    const u32 newTlutDataVersion = reader.read<u32>();
    const u8 newFlags = slot.flags & ~0x80u; // Reset no-cache flag
    if (slot.data != newData || slot.format != newFormat || slot.numEntries != newNumEntries ||
        slot.tlutObjId != newTlutObjId || slot.tlutDataVersion != newTlutDataVersion || slot.flags != newFlags) {
      if (slot.tlutObjId != newTlutObjId || slot.tlutDataVersion != newTlutDataVersion) {
        texture::invalidate_bindings();
      }
      slot.data = newData;
      slot.format = newFormat;
      slot.numEntries = newNumEntries;
      slot.tlutObjId = newTlutObjId;
      slot.tlutDataVersion = newTlutDataVersion;
      slot.flags = newFlags;
      g_gxState.dirty |= DirtyTextures;
    }
  } else if (subCmd == GX2_SET_POLYGON_OFFSET) {
    const f32 frontOffset = reader.read<f32>();
    const f32 frontScale = reader.read<f32>();
    const f32 backOffset = reader.read<f32>();
    const f32 backScale = reader.read<f32>();
    const f32 clamp = reader.read<f32>();
    if (g_gxState.frontOffset != frontOffset || g_gxState.frontScale != frontScale ||
        g_gxState.backOffset != backOffset || g_gxState.backScale != backScale || g_gxState.clamp != clamp) {
      g_gxState.frontOffset = frontOffset;
      g_gxState.frontScale = frontScale;
      g_gxState.backOffset = backOffset;
      g_gxState.backScale = backScale;
      g_gxState.clamp = clamp;
      g_gxState.dirty |= DirtyPipeline;
    }
  } else if (subCmd == GX_AURORA_LOAD_COPY_SRC) {
    const s32 left = reader.read<s32>();
    const s32 top = reader.read<s32>();
    const s32 width = reader.read<s32>();
    const s32 height = reader.read<s32>();
    g_gxState.texCopySrc = {left, top, width, height};
  } else if (subCmd == GX_AURORA_LOAD_COPY_DST) {
    g_gxState.texCopyDstWidth = reader.read<u32>();
    g_gxState.texCopyDstHeight = reader.read<u32>();
    g_gxState.texCopyFmt = static_cast<GXTexFmt>(reader.read<u32>());
    g_gxState.texCopyMips = reader.read<u8>() != 0;
    g_gxState.texCopyDstWide = true;
  } else if (subCmd == GX_AURORA_LOAD_COPY_DEST) {
    g_gxState.texCopyDest = reinterpret_cast<const void*>(reader.read<u64>());
  } else if (subCmd == GX_AURORA_REQUEST_DEPTH_SNAPSHOT) {
    gfx::depth_peek::request_snapshot();
  } else if (subCmd == GX_AURORA_BEGIN_OFFSCREEN) {
    const u32 width = reader.read<u32>();
    const u32 height = reader.read<u32>();
    gfx::begin_offscreen(width, height);
  } else if (subCmd == GX_AURORA_END_OFFSCREEN) {
    gfx::end_offscreen();
  } else if (subCmd == GX_AURORA_DESTROY_TEXOBJ) {
    evict_texture_object(reader.read<u32>());
  } else if (subCmd == GX_AURORA_DESTROY_TLUT) {
    evict_tlut_object(reader.read<u32>());
  } else if (subCmd == GX_AURORA_DESTROY_COPY_TEX) {
    evict_copy_texture(reinterpret_cast<const void*>(reader.read<u64>()));
  } else if (subCmd == GX_AURORA_DRAW_SIZED) {
    const u8 cmd = reader.read<u8>();
    const u32 byteLen = reader.read<u32>();
    const GXVtxFmt fmt = static_cast<GXVtxFmt>(cmd & CP_VAT_MASK);
    const GXPrimitive prim = static_cast<GXPrimitive>(cmd & CP_OPCODE_MASK);
    if (byteLen != 0) {
      u32 vtxSize;
      if (g_gxState.lastVtxFmt == fmt) {
        vtxSize = g_gxState.lastVtxSize;
      } else {
        vtxSize = calc_vtx_size(fmt);
      }
      AURORA_ASSERT(vtxSize != 0 && byteLen % vtxSize == 0,
                    "GX_AURORA_DRAW_SIZED: {} bytes is not a whole number of size-{} vertices", byteLen, vtxSize);
      u32 vtxCount = byteLen / vtxSize;
      AURORA_ASSERT(vtxCount <= 0xFFFF, "GX_AURORA_DRAW_SIZED: too many vertices ({})", vtxCount);
      draw_prim(0, prim, fmt, static_cast<u16>(vtxCount), reader);
    }
  } else if (subCmd == GX_AURORA_RESIDENT_RETAIN) {
    const auto* key = reinterpret_cast<const void*>(reader.read<u64>());
    std::unique_ptr<std::vector<u8>> bytes{reinterpret_cast<std::vector<u8>*>(reader.read<u64>())};
    resident::retain(key, std::move(bytes));
  } else if (subCmd == GX_AURORA_RESIDENT_RELEASE) {
    const auto* key = reinterpret_cast<const void*>(reader.read<u64>());
    resident::release(key);
    // An array still set to it reads its resident copy no more.
    for (auto& array : g_gxState.arrays) {
      if (array.data == key) {
        array.cachedRange = {};
      }
    }
  } else if (subCmd == GX_AURORA_RESIDENT_CALL_DL) {
    const auto* key = reinterpret_cast<const void*>(reader.read<u64>());
    const u32 size = reader.read<u32>();
    call_resident_dl(key, size);
  } else if (subCmd == GX_AURORA_DRAW_INDEXED) {
    ZoneScopedN("DRAW_INDEXED");
    const u8 cmd = reader.read<u8>();
    const u16 vtxCount = reader.read<u16>();
    const u32 indexCount = reader.read<u32>();
    const GXVtxFmt fmt = static_cast<GXVtxFmt>(cmd & CP_VAT_MASK);
    const GXPrimitive prim = static_cast<GXPrimitive>(cmd & CP_OPCODE_MASK);
    AURORA_ASSERT(prim == GX_TRIANGLES, "GX_AURORA_DRAW_INDEXED: primitive must be GX_TRIANGLES, got {}",
                  static_cast<u32>(prim));
    const size_t idxBytes = static_cast<size_t>(indexCount) * sizeof(u16);
    // Index data is always host-endian; push it to the GPU buffer as-is
    const auto indexData = reader.take(idxBytes);
    const gfx::Range idxRange = gfx::push_indices(indexData.data(), indexData.size(), 4);
    u32 vtxSize;
    if (g_gxState.lastVtxFmt == fmt) {
      vtxSize = g_gxState.lastVtxSize;
    } else {
      vtxSize = calc_vtx_size(fmt);
    }
    const u32 totalVtxBytes = vtxCount * vtxSize;
    const auto vertexData = reader.take(totalVtxBytes);
    if (check_indexed_arrays(vertexData.data(), fmt, prim, vtxCount, vtxSize)) UNLIKELY {
      sDrawCache.lastDrawFmt = GX_MAX_VTXFMT;
    } else {
      const DeindexPlan* const plan = g_gxState.deindexVertices ? &deindex_plan(fmt) : nullptr;
      const auto vertRange = push_vertex_records(plan, vertexData, vtxCount, 4);
      if (indexCount != 0 && vertRange) {
        push_gx_draw(prim, fmt, vtxCount, *vertRange, idxRange, indexCount);
      }
    }
  } else if (subCmd == GX_AURORA_DEBUG_GROUP_PUSH) {
    auto label = reader.read_string();
    gfx::push_debug_group(std::move(label));
  } else if (subCmd == GX_AURORA_DEBUG_GROUP_POP) {
    pop_debug_group();
  } else if (subCmd == GX_AURORA_DEBUG_MARKER_INSERT) {
    auto label = reader.read_string();
    gfx::insert_debug_marker(std::move(label));
  } else if (subCmd == GX_AURORA_SET_DRAW_SYNC) {
    aurora::gx::set_draw_sync_token(reader.read<u16>());
  } else if (subCmd == GX_AURORA_LOAD_ARRAY_BASE_INDEX) {
    const u32 attrIdx = (reader.read<u8>() & 0x0f) + GX_VA_POS;
    const u32 base = reader.read<u32>();
    if (attrIdx < g_gxState.arrays.size() && g_gxState.arrays[attrIdx].baseIndex != base) {
      g_gxState.arrays[attrIdx].baseIndex = base;
      g_gxState.dirty |= DirtyImmediates;
    }
  } else if (subCmd == GX_AURORA_SET_PBR) {
    const u8 pbr = reader.read<u8>();
    if (g_gxState.pbr != pbr) {
      g_gxState.pbr = pbr;
      g_gxState.dirty |= DirtyPipeline;
    }
  } else if (subCmd == GX_AURORA_SET_SDF) {
    const u8 sdf = reader.read<u8>();
    if (g_gxState.sdf != sdf) {
      g_gxState.sdf = sdf;
      g_gxState.dirty |= DirtyPipeline;
    }
  } else if (subCmd == GX_AURORA_SET_HUD_SAMPLE) {
    const u8 mode = reader.read<u8>();
    const f32 x = reader.read<f32>();
    const f32 y = reader.read<f32>();
    if (g_gxState.hudSample != mode) {
      g_gxState.hudSample = mode;
      g_gxState.dirty |= DirtyPipeline;
    }
    const Vec4<float> dyin{x, y, 0.f, 0.f};
    if (g_gxState.hudDyin != dyin) {
      g_gxState.hudDyin = dyin;
      g_gxState.dirty |= DirtyUniform;
    }
  } else if (subCmd == GX_AURORA_SET_CLIP_PLANE) {
    const bool on = reader.read<u8>() != 0;
    Vec4<float> eq;
    eq.x() = reader.read<f32>();
    eq.y() = reader.read<f32>();
    eq.z() = reader.read<f32>();
    eq.w() = reader.read<f32>();
    if (g_gxState.clipPlane != on) {
      g_gxState.clipPlane = on;
      g_gxState.dirty |= DirtyPipeline;
    }
    if (g_gxState.clipPlaneEq != eq) {
      g_gxState.clipPlaneEq = eq;
      g_gxState.dirty |= DirtyUniform;
    }
  } else if (subCmd == GX_AURORA_PORT_SHELL) {
    const u8 mode = reader.read<u8>();
    auto& st = g_gxState;
    if (mode == 1 || mode == 2) {
      for (size_t i = 0; i < 6; ++i) {
        auto& v = st.shellUniform[i];
        v.x() = reader.read<f32>();
        v.y() = reader.read<f32>();
        v.z() = reader.read<f32>();
        v.w() = reader.read<f32>();
      }
      // The fragment's position over the target it draws into is the scene copy's uv.
      const auto target = gfx::get_render_target_size();
      auto& size = st.shellUniform[6];
      size.x() = 1.f / static_cast<float>(std::max(target.x, 1u));
      size.y() = 1.f / static_cast<float>(std::max(target.y, 1u));
      size.z() = 0.f;
      size.w() = 0.f;
      st.dirty |= DirtyUniform;
    }
    if (mode == 1) {
      // The slots' binds are resolved only at a draw, so they still hold the last draw's maps.
      st.shellTexObjs = {st.loadedTextures[GX_TEXMAP0], st.loadedTextures[GX_TEXMAP1]};
      st.shellTextures = {};
      st.dirty |= DirtyTextures;
    } else if (mode == 0) {
      st.shellTexObjs = {};
      st.shellTextures = {};
    }
    const u8 shell = mode <= 2 ? mode : 0;
    if (st.shell != shell) {
      st.shell = shell;
      // Slot 2 holds the scene copy while the colour pass is on; re-emit the bind groups either way.
      sDrawCache.hasPipeline = false;
      sDrawCache.bindGeneration = 0;
      sDrawCache.uniformRange = {};
      sDrawCache.lastDrawFmt = GX_MAX_VTXFMT;
      st.dirty |= DirtyPipeline | DirtyUniform | DirtyTextures;
    }
  } else if (subCmd == GX_AURORA_PORT_DEPTH_PREPASS) {
    const u8 pass = reader.read<u8>();
    if (g_gxState.depthPrepass != pass) {
      g_gxState.depthPrepass = pass;
      g_gxState.dirty |= DirtyPipeline;
    }
  } else if (subCmd == GX_AURORA_PORT_DRAW_SERIAL) {
    const u32 serial = reader.read<u32>();
    if (g_gxState.drawSerial != serial) {
      g_gxState.drawSerial = serial;
      // Draws with different serials must not merge.
      g_gxState.dirty |= DirtyImmediates;
    }
  } else if (subCmd == GX_AURORA_PORT_DRAW_ID_MODE) {
    bool on = reader.read<u8>() != 0;
    // Draw-id output is a flat LDR colour: leave the HDR scene first, or stay out of draw-id mode.
    if (on && gfx::scene_hdr_active()) {
      if (gfx::set_scene_hdr(false)) {
        g_gxState.dirty |= DirtyPipeline;
      } else {
        Log.warn("GX_AURORA_PORT_DRAW_ID_MODE: refused, the HDR scene cannot be left here");
        on = false;
      }
    }
    if (g_gxState.drawIdMode != on) {
      g_gxState.drawIdMode = on;
      g_gxState.dirty |= DirtyPipeline;
    }
  } else if (subCmd == GX_AURORA_SET_DRAW_TAG) {
    for (u32& value : g_gxState.drawTag) {
      value = reader.read<u32>();
    }
  } else if (subCmd == GX_AURORA_COPY_PROBE_FACE) {
    copy_probe_face(reader.read<u8>());
  } else if (subCmd == GX_AURORA_SET_PBR_PROBE) {
    Mat3x4<float> mtx;
    for (Vec4<float>* col : {&mtx.m0, &mtx.m1, &mtx.m2}) {
      const f32 x = reader.read<f32>();
      const f32 y = reader.read<f32>();
      const f32 z = reader.read<f32>();
      const f32 w = reader.read<f32>();
      *col = Vec4<float>{x, y, z, w};
    }
    if (g_gxState.pbrProbe != mtx) {
      g_gxState.pbrProbe = mtx;
      g_gxState.dirty |= DirtyUniform;
    }
  } else if (subCmd == GX_AURORA_CREATE_PBR_CUBE) {
    ++sProbeGeneration;
    const u32 id = reader.read<u32>();
    const u32 size = reader.read<u32>();
    const u32 mipCount = reader.read<u32>();
    const std::unique_ptr<std::vector<u8>> texels{reinterpret_cast<std::vector<u8>*>(reader.read<u64>())};
    gfx::probe::create_cube(id, size, mipCount, texels->data(), texels->size());
    g_gxState.dirty |= DirtyTextures;
  } else if (subCmd == GX_AURORA_DESTROY_PBR_CUBE) {
    ++sProbeGeneration;
    gfx::probe::destroy_cube(reader.read<u32>());
    g_gxState.dirty |= DirtyTextures;
  } else if (subCmd == GX_AURORA_SET_PBR_CUBE) {
    u32 id = reader.read<u32>();
    Vec4<float> value;
    for (int i = 0; i < 4; ++i) {
      value[i] = reader.read<f32>();
    }
    if (id == 0 || !gfx::probe::has_cube(id)) {
      // No such cube: the probe, which is not HDR.
      id = 0;
      value = {};
    }
    if (g_gxState.pbrCube != id) {
      g_gxState.pbrCube = id;
      g_gxState.dirty |= DirtyTextures;
    }
    if (g_gxState.pbrCubeParams != value) {
      g_gxState.pbrCubeParams = value;
      g_gxState.dirty |= DirtyUniform;
    }
  } else if (subCmd == GX_AURORA_SET_PBR_AMBIENT) {
    for (Vec4<float>& v : g_gxState.pbrAmbient) {
      const f32 x = reader.read<f32>();
      const f32 y = reader.read<f32>();
      const f32 z = reader.read<f32>();
      const f32 w = reader.read<f32>();
      const Vec4<float> value{x, y, z, w};
      if (v != value) {
        v = value;
        g_gxState.dirty |= DirtyUniform;
      }
    }
  } else if (subCmd == GX_AURORA_CREATE_PBR_VOLUME) {
    ++sProbeGeneration;
    const u32 id = reader.read<u32>();
    const u32 sizeX = reader.read<u32>();
    const u32 sizeY = reader.read<u32>();
    const u32 sizeZ = reader.read<u32>();
    const std::unique_ptr<std::vector<u8>> texels{reinterpret_cast<std::vector<u8>*>(reader.read<u64>())};
    gfx::probe::create_volume(id, sizeX, sizeY, sizeZ, texels->data(), texels->size());
    g_gxState.dirty |= DirtyTextures;
  } else if (subCmd == GX_AURORA_DESTROY_PBR_VOLUME) {
    ++sProbeGeneration;
    gfx::probe::destroy_volume(reader.read<u32>());
    g_gxState.dirty |= DirtyTextures;
  } else if (subCmd == GX_AURORA_SET_PBR_VOLUME) {
    u32 id = reader.read<u32>();
    std::array<Vec4<float>, 6> rows;
    for (Vec4<float>& v : rows) {
      const f32 x = reader.read<f32>();
      const f32 y = reader.read<f32>();
      const f32 z = reader.read<f32>();
      const f32 w = reader.read<f32>();
      v = {x, y, z, w};
    }
    if (id == 0 || !gfx::probe::has_volume(id)) {
      id = 0;
      rows = {};
    }
    if (g_gxState.pbrVolume != id) {
      g_gxState.pbrVolume = id;
      g_gxState.dirty |= DirtyTextures;
    }
    if (g_gxState.pbrVolumeRows != rows) {
      g_gxState.pbrVolumeRows = rows;
      g_gxState.dirty |= DirtyUniform;
    }
  } else if (subCmd == GX_AURORA_CREATE_PBR_LIGHTMAP) {
    const u32 id = reader.read<u32>();
    const u32 width = reader.read<u32>();
    const u32 height = reader.read<u32>();
    const u32 layers = reader.read<u32>();
    const u32 format = reader.read<u32>();
    const std::unique_ptr<std::vector<u8>> texels{reinterpret_cast<std::vector<u8>*>(reader.read<u64>())};
    gfx::probe::create_lightmap(id, width, height, layers, format, texels->data(), texels->size());
    g_gxState.dirty |= DirtyTextures;
  } else if (subCmd == GX_AURORA_DESTROY_PBR_LIGHTMAP) {
    gfx::probe::destroy_lightmap(reader.read<u32>());
    g_gxState.dirty |= DirtyTextures;
  } else if (subCmd == GX_AURORA_SET_PBR_LIGHTMAP) {
    u32 id = reader.read<u32>();
    Vec4<float> rect;
    {
      const f32 x = reader.read<f32>();
      const f32 y = reader.read<f32>();
      const f32 z = reader.read<f32>();
      const f32 w = reader.read<f32>();
      rect = {x, y, z, w};
    }
    std::array<Vec4<float>, 3> axes;
    for (Vec4<float>& v : axes) {
      const f32 x = reader.read<f32>();
      const f32 y = reader.read<f32>();
      const f32 z = reader.read<f32>();
      v = {x, y, z, 0.f};
    }
    if (id == 0 || !gfx::probe::has_lightmap(id)) {
      id = 0;
      rect = {};
      axes = {};
    }
    if (g_gxState.pbrLightmap != id) {
      g_gxState.pbrLightmap = id;
      g_gxState.dirty |= DirtyTextures;
    }
    if (g_gxState.pbrLightmapRect != rect || g_gxState.pbrLightmapAxes != axes) {
      g_gxState.pbrLightmapRect = rect;
      g_gxState.pbrLightmapAxes = axes;
      g_gxState.dirty |= DirtyUniform;
    }
  } else if (subCmd == GX_AURORA_SET_PBR_LIGHTMAP_ATTR) {
    const u8 attr = reader.read<u8>();
    if (g_gxState.pbrLightmapAttr != attr) {
      g_gxState.pbrLightmapAttr = attr;
      g_gxState.dirty |= DirtyPipeline;
    }
  } else if (subCmd == GX_AURORA_SET_PBR_BRDF_LUT) {
    const std::unique_ptr<std::vector<u8>> texels{reinterpret_cast<std::vector<u8>*>(reader.read<u64>())};
    const bool on = gfx::probe::set_brdf_lut(texels->data(), texels->size());
    if (g_gxState.pbrBrdfLut != on) {
      g_gxState.pbrBrdfLut = on;
      g_gxState.dirty |= DirtyUniform;
    }
    g_gxState.dirty |= DirtyTextures;
  } else if (subCmd == GX_AURORA_SET_PBR_TONE) {
    std::array<Vec4<float>, 3> rows;
    for (Vec4<float>& v : rows) {
      const f32 x = reader.read<f32>();
      const f32 y = reader.read<f32>();
      const f32 z = reader.read<f32>();
      const f32 w = reader.read<f32>();
      v = {x, y, z, w};
    }
    if (g_gxState.pbrTone != rows) {
      g_gxState.pbrTone = rows;
      g_gxState.dirty |= DirtyUniform;
    }
  } else if (subCmd == GX_AURORA_PORT_POST_PROCESS) {
    u32 words[49];
    for (u32& word : words) {
      word = reader.read<u32>();
    }
    gfx::bloom::Params params;
    static_assert(sizeof(params) == sizeof(words));
    std::memcpy(&params, words, sizeof(params));
    const bool hdrBefore = gfx::scene_hdr_active();
    gfx::bloom::record(params);
    if (gfx::scene_hdr_active() != hdrBefore) {
      // An HDR post left the scene: the draws after it go to the display EFB's format.
      g_gxState.dirty |= DirtyPipeline;
    }
  } else if (subCmd == GX_AURORA_PORT_VOLUMETRIC_FOG) {
    gfx::volfog::Params params;
    u32 words[sizeof(params) / sizeof(u32)];
    for (u32& word : words) {
      word = reader.read<u32>();
    }
    std::memcpy(&params, words, sizeof(params));
    if (gfx::volfog::record(params)) {
      // The draws after it fog themselves through the froxels it fills.
      // w: where the world's depth range starts; nearer is the viewmodel (vf_depth).
      const Vec4<float> fogParams{params.depth[0], params.fog[0], params.colorA[3], params.depth[2]};
      std::array<Vec4<float>, 3> tone;
      for (size_t i = 0; i < tone.size(); ++i) {
        tone[i] = {params.tone[i][0], params.tone[i][1], params.tone[i][2], params.tone[i][3]};
      }
      // The viewmodel's depth reading (vf_depth) needs 1 - near / far; tone[0].w is unused.
      tone[0] = {params.tone[0][0], params.tone[0][1], params.tone[0][2], 1.f - params.depth[0] / params.depth[1]};
      if (!g_gxState.volFog) {
        g_gxState.volFog = true;
        g_gxState.dirty |= DirtyPipeline;
      }
      if (g_gxState.volFogParams != fogParams || g_gxState.volFogTone != tone) {
        g_gxState.volFogParams = fogParams;
        g_gxState.volFogTone = tone;
        g_gxState.dirty |= DirtyUniform;
      }
      // A new froxel texture when the frame's size changed.
      g_gxState.dirty |= DirtyTextures;
    }
  } else if (subCmd == GX_AURORA_PORT_VOLUMETRIC_FOG_END) {
    if (g_gxState.volFog) {
      g_gxState.volFog = false;
      g_gxState.dirty |= DirtyPipeline;
    }
  } else if (subCmd == GX_AURORA_PORT_XRAY) {
    gfx::xray::Params params;
    u32 words[sizeof(params) / sizeof(u32)];
    for (u32& word : words) {
      word = reader.read<u32>();
    }
    std::memcpy(&params, words, sizeof(params));
    // PrimedGun: an immersive frame's eye passes replay no encoder tasks (as bloom::record), so the
    // post would reach only the mono EFB and split the eye passes in two. The game draws retail's
    // visor in such a frame (CStateManager::DrawWorld); this covers a frame whose presentation
    // became immersive after the game checked. The payload is read above either way, so the FIFO
    // stays in step.
    if (!gfx::stereo_shadow::active()) {
      gfx::xray::record(params);
    }
  } else if (subCmd == GX_AURORA_PORT_THERMAL) {
    gfx::thermal::Params params;
    u32 words[sizeof(params) / sizeof(u32)];
    for (u32& word : words) {
      word = reader.read<u32>();
    }
    std::memcpy(&params, words, sizeof(params));
    // PrimedGun: skipped in an immersive frame, as the X-ray post above.
    if (!gfx::stereo_shadow::active()) {
      gfx::thermal::record(params);
    }
  } else if (subCmd == GX_AURORA_PORT_SCENE_HDR) {
    const u8 requested = reader.read<u8>();
    // Eye replay has no HDR composite pass; keep immersive frames on the LDR path.
    const u8 value = gfx::stereo_shadow::active() ? 0 : requested;
    if (value > 2) {
      Log.warn("GX_AURORA_PORT_SCENE_HDR: invalid value {}", value);
    } else if (value != 0 && g_gxState.drawIdMode) {
      Log.warn("GX_AURORA_PORT_SCENE_HDR: refused in draw-id mode");
    } else if (value == 2 || gfx::scene_hdr_active() != (value != 0)) {
      if (gfx::set_scene_hdr(value != 0, value == 2)) {
        g_gxState.dirty |= DirtyPipeline;
      } else {
        Log.warn("GX_AURORA_PORT_SCENE_HDR: switch to {} refused", value);
      }
    }
  } else if (subCmd == GX_AURORA_PORT_COPY_SCENE) {
    const u8 value = reader.read<u8>();
    if (value > 1) {
      Log.warn("GX_AURORA_PORT_COPY_SCENE: invalid value {}", value);
    }
    g_gxState.copySceneLinear = value == 1;
  } else if (subCmd == GX_AURORA_PORT_PARTICLE_FOG) {
    const u8 on = reader.read<u8>();
    if (on > GXState::kParticleUnfogged) {
      Log.warn("GX_AURORA_PORT_PARTICLE_FOG: invalid value {}", on);
    }
    if (g_gxState.particleFog != on) {
      g_gxState.particleFog = on;
      g_gxState.dirty |= DirtyPipeline;
    }
  } else if (subCmd == GX_AURORA_PORT_SHADOW_CASTER) {
    const u8 mode = reader.read<u8>();
    const bool on = mode != 0;
    g_gxState.shadowCasterOnly = mode == 2;
    if (g_gxState.shadowCaster != on) {
      g_gxState.shadowCaster = on;
      g_gxState.dirty |= DirtyPipeline;
    }
  } else if (subCmd == GX_AURORA_PORT_SHADOW_FRAME) {
    f32 worldToView[3][4];
    for (auto& row : worldToView) {
      for (f32& v : row) {
        v = reader.read<f32>();
      }
    }
    gfx::shadow::cascade::Frustum frustum;
    for (f32* v : {&frustum.left, &frustum.right, &frustum.top, &frustum.bottom, &frustum.znear, &frustum.zfar}) {
      *v = reader.read<f32>();
    }
    f32 sunDir[3];
    for (f32& v : sunDir) {
      v = reader.read<f32>();
    }
    const f32 distance = reader.read<f32>();
    f32 color[3];
    for (f32& v : color) {
      v = reader.read<f32>();
    }
    gfx::shadow::Uniform uniform{};
    const bool active = gfx::shadow::set_frame(worldToView, frustum, sunDir, distance, color, uniform);
    if (g_gxState.shadowActive != active) {
      g_gxState.shadowActive = active;
      g_gxState.dirty |= DirtyPipeline;
    }
    if (active) {
      static_assert(sizeof(uniform) == sizeof(g_gxState.shadowUniform));
      std::array<Vec4<float>, 36> values;
      std::memcpy(values.data(), &uniform, sizeof(uniform));
      if (g_gxState.shadowUniform != values) {
        g_gxState.shadowUniform = values;
        g_gxState.dirty |= DirtyUniform;
      }
    }
  } else if (subCmd == GX_AURORA_PORT_ROOM_LIGHTS) {
    const u32 count = reader.read<u32>();
    std::vector<f32> records(static_cast<size_t>(count) * 16);
    for (f32& v : records) {
      v = reader.read<f32>();
    }
    Vec4<float> value{};
    if (count != 0) {
      const auto range = gfx::push_storage(reinterpret_cast<const uint8_t*>(records.data()), records.size() * sizeof(f32));
      if (!gfx::overflowed(range)) {
        const u32 base = range.offset / sizeof(u32);
        std::memcpy(&value.x(), &base, sizeof(base));
        value.y() = static_cast<f32>(count);
      }
    }
    // x holds a u32 offset as float bits: compare bits, since denormals may compare equal under FTZ/DAZ.
    if (std::memcmp(&g_gxState.pbrRoomLights, &value, sizeof(value)) != 0) {
      g_gxState.pbrRoomLights = value;
      g_gxState.dirty |= DirtyUniform;
    }
  } else if (subCmd == GX_AURORA_PORT_SHADOW_RENDER) {
    gfx::shadow::record();
  } else if (subCmd == GX_AURORA_PORT_ACTOR_SHADOW) {
    const u8 mode = reader.read<u8>();
    auto& st = g_gxState;
    if (mode == 1 || mode == 2) {
      const u8 layer = reader.read<u8>();
      std::array<Vec4<float>, 36> values{};
      for (auto& v : values) {
        v = {};
      }
      f32 raw[64];
      for (f32& f : raw) {
        f = reader.read<f32>();
      }
      std::memcpy(values.data(), raw, sizeof(raw));
      if (st.actorLayer < 0 && !st.actorDecal) {
        st.actorSavedCaster = st.shadowCaster;
        st.actorSavedCasterOnly = st.shadowCasterOnly;
        st.actorSavedActive = st.shadowActive;
        st.actorSavedUniform = st.shadowUniform;
      }
      st.shadowUniform = values;
      st.shadowActive = true;
      st.shadowCaster = true;
      st.shadowCasterOnly = mode == 1;
      st.actorLayer = mode == 1 ? static_cast<int>(layer) : -1;
      st.actorDecal = mode == 2;
      st.dirty |= DirtyPipeline | DirtyUniform;
    } else if (mode == 0) {
      if (st.actorLayer >= 0 || st.actorDecal) {
        st.shadowCaster = st.actorSavedCaster;
        st.shadowCasterOnly = st.actorSavedCasterOnly;
        st.shadowActive = st.actorSavedActive;
        st.shadowUniform = st.actorSavedUniform;
        st.actorLayer = -1;
        st.actorDecal = false;
        st.dirty |= DirtyPipeline | DirtyUniform;
      }
      // The caster/decal layers bound their own pipelines, bind groups and uniforms (and the depth
      // passes forgot the texture groups): re-emit everything for the draws that follow.
      sDrawCache.hasPipeline = false;
      sDrawCache.bindGeneration = 0;
      sDrawCache.uniformRange = {};
      sDrawCache.lastDrawFmt = GX_MAX_VTXFMT;
      st.dirty |= DirtyPipeline | DirtyUniform | DirtyTextures;
    } else if (mode == 3) {
      gfx::actor_shadow::record();
      sDrawCache.hasPipeline = false;
      sDrawCache.bindGeneration = 0;
      sDrawCache.uniformRange = {};
      sDrawCache.lastDrawFmt = GX_MAX_VTXFMT;
      st.dirty |= DirtyPipeline | DirtyUniform | DirtyTextures;
    }
  } else if (subCmd == GX_AURORA_SET_PBR_LIGHT_SKIP) {
    const Vec4<float> value{static_cast<f32>(reader.read<u32>() & 0xFF), 0.f, 0.f, 0.f};
    if (g_gxState.pbrLightSkip != value) {
      g_gxState.pbrLightSkip = value;
      g_gxState.dirty |= DirtyUniform;
    }
  } else if (subCmd == GX_AURORA_SET_PBR_BAKED_LIGHT_MODULATION) {
    Vec4<float> value{1.f, 1.f, 1.f, 0.f};
    value.x() = reader.read<f32>();
    value.y() = reader.read<f32>();
    value.z() = reader.read<f32>();
    if (g_gxState.pbrBakedLightModulation != value) {
      g_gxState.pbrBakedLightModulation = value;
      g_gxState.dirty |= DirtyUniform;
    }
  } else if (subCmd == GX_AURORA_SET_PBR_BACKLIGHT) {
    f32 v[11];
    for (f32& f : v) {
      f = reader.read<f32>();
    }
    const std::array<Vec4<float>, 3> value{
        Vec4<float>{v[0], v[1], v[2], v[3]},
        Vec4<float>{v[4], v[5], v[6], v[7]},
        Vec4<float>{v[8], v[9], v[10], 0.f},
    };
    if (g_gxState.pbrBacklightLights != value) {
      if ((g_gxState.pbrBacklightLights[2].z() > 0.f) != (value[2].z() > 0.f)) {
        g_gxState.dirty |= DirtyPipeline;
      }
      g_gxState.pbrBacklightLights = value;
      g_gxState.dirty |= DirtyUniform;
    }
  } else if (subCmd == GX_AURORA_SET_PBR_SHIELD) {
    std::array<Vec4<float>, 8> value;
    for (auto& row : value) {
      const f32 x = reader.read<f32>();
      const f32 y = reader.read<f32>();
      const f32 z = reader.read<f32>();
      const f32 w = reader.read<f32>();
      row = Vec4<float>{x, y, z, w};
    }
    if (g_gxState.pbrShield != value) {
      g_gxState.pbrShield = value;
      g_gxState.dirty |= DirtyUniform;
    }
  } else if (subCmd == GX_AURORA_SET_PBR_LIGHT_HDR) {
    const u32 bit = reader.read<u32>() & 0xFF;
    f32 v[8];
    for (f32& f : v) {
      f = reader.read<f32>();
    }
    const u32 falloff = std::min(reader.read<u32>(), 3u);
    if (bit != 0) {
      const u32 idx = static_cast<u32>(std::countr_zero(bit));
      const bool on = v[7] > 0.f;
      const std::array<Vec4<float>, 3> rows{
          on ? Vec4<float>{v[0], v[1], v[2], static_cast<f32>(falloff + 1)} : Vec4<float>{},
          on ? Vec4<float>{v[3], v[4], v[5], v[6]} : Vec4<float>{},
          on ? Vec4<float>{v[7], 0.f, 0.f, 0.f} : Vec4<float>{},
      };
      for (u32 row = 0; row < 3; ++row) {
        if (g_gxState.pbrLightHdr[idx * 3 + row] != rows[row]) {
          g_gxState.pbrLightHdr[idx * 3 + row] = rows[row];
          g_gxState.dirty |= DirtyUniform;
        }
      }
    }
  } else if (subCmd == GX_AURORA_SET_PBR_LIGHT_SCALE) {
    const f32 diffuse = reader.read<f32>();
    const f32 f0 = reader.read<f32>();
    const f32 alpha = std::clamp(reader.read<f32>(), 0.f, 1.f);
    const bool alphaReplaces = reader.read<u32>() != 0;
    // w is the fade as the shader reads it: 0 none, 1 + alpha in place of the material's
    // alpha, -(1 + alpha) times it.
    const f32 fade = alphaReplaces ? 1.f + alpha : alpha < 1.f ? -(1.f + alpha) : 0.f;
    const Vec4<float> value{diffuse, f0, 0.f, fade};
    if (g_gxState.pbrLightScale != value) {
      g_gxState.pbrLightScale = value;
      g_gxState.dirty |= DirtyUniform;
    }
  } else if (subCmd == GX_AURORA_SET_PBR_MODEL_MODULATION) {
    f32 v[8];
    for (f32& f : v) {
      f = reader.read<f32>();
    }
    const Vec4<float> diffuse{v[0], v[1], v[2], v[3]};
    const Vec4<float> incan{v[4], v[5], v[6], v[7]};
    const Vec4<float> coverage{reader.read<f32>(), 0.f, 0.f, 0.f};
    if (g_gxState.pbrModelDiffuse != diffuse || g_gxState.pbrModelIncan != incan ||
        g_gxState.pbrModelCoverage != coverage) {
      g_gxState.pbrModelDiffuse = diffuse;
      g_gxState.pbrModelIncan = incan;
      g_gxState.pbrModelCoverage = coverage;
      g_gxState.dirty |= DirtyUniform;
    }
  } else if (subCmd == GX_AURORA_SET_PBR_THERMAL) {
    const f32 x = reader.read<f32>();
    const f32 y = reader.read<f32>();
    const f32 blended = reader.read<f32>();
    const bool on = reader.read<u32>() != 0;
    const Vec4<float> value = on ? Vec4<float>{x, y, blended, 1.f} : Vec4<float>{};
    if (g_gxState.pbrThermal != value) {
      g_gxState.pbrThermal = value;
      g_gxState.dirty |= DirtyUniform;
    }
  } else if (subCmd == GX_AURORA_SET_PBR_MATERIAL) {
    for (Vec4<float>* v :
         {&g_gxState.pbrEmissive, &g_gxState.pbrBacklight, &g_gxState.pbrLayer, &g_gxState.pbrLayerHeight,
          &g_gxState.pbrParam, &g_gxState.pbrUp}) {
      const f32 x = reader.read<f32>();
      const f32 y = reader.read<f32>();
      const f32 z = reader.read<f32>();
      const f32 w = reader.read<f32>();
      const Vec4<float> value{x, y, z, w};
      if (*v != value) {
        // The cut bit is in the shader key (ShaderConfig::pbrCut).
        if (v == &g_gxState.pbrBacklight && pbr_cut_mode(*v) != pbr_cut_mode(value)) {
          g_gxState.dirty |= DirtyPipeline;
        }
        *v = value;
        g_gxState.dirty |= DirtyUniform;
      }
    }
  }

  else if (subCmd == GX_AURORA_CALL_CACHED_DL) {
    handle_cached_display_list(reader);
  } else if (subCmd == GX_AURORA_FREE_GEOMETRY_SET) {
    geometry_cache::free_set(reader.read<u32>());
  } else {
    Log.error("Unknown Aurora subcommand: {:04X}", subCmd);
  }
}

void clear_draw_cache() noexcept {
  staging_report();
  // Uniform/vertex ranges belong to this frame, but texture bind groups do
  // not. Keep their descriptor cache warm; current_bind_group_entry() checks
  // the resource cache's lifetime before reusing an entry in a later frame.
  sDrawCache.bindGeneration = 0;
  sDrawCache.uniformRange = {};
  sDrawCache.stereoUniformOffsets = {UINT32_MAX, UINT32_MAX};
  sDrawCache.multiviewMode = MultiviewNone;
  sDrawCache.stereoBindGroups = {};
  sDrawCache.stereoEpoch = 0;
  sDrawCache.fogRange = {};
  sDrawCache.hasFogRange = false;
  g_gxState.copySceneLinear = false;
  // GX_AURORA_PORT_ROOM_LIGHTS points into this frame's storage buffer.
  if (g_gxState.pbrRoomLights.y() != 0.f) {
    g_gxState.pbrRoomLights = {};
    g_gxState.dirty |= DirtyUniform;
  }
  // Vertex de-indexing changes between frames only, with the pipelines
  const bool deindex = sDeindexRequested.load(std::memory_order_relaxed);
  if (deindex != g_gxState.deindexVertices) {
    g_gxState.deindexVertices = deindex;
    g_gxState.dirty |= DirtyPipeline | DirtyImmediates;
    sDeindexPlan.fmt = GX_MAX_VTXFMT;
  }
}

void reset_draw_cache() noexcept {
  // Initialization/shutdown can replace layouts and default texture views.
  sBindGroupCache.clear();
  // The memo's pipelines go with the pipeline cache's shutdown, and its uniform sizes follow the
  // device's uniform alignment.
  sPipelineMemo.clear();
  sDrawCache = {};
  geometry_cache::clear();
  clear_native_vertex_choices();
}

} // namespace aurora::gx::fifo

extern "C" void aurora_set_gx_deindex_vertices(bool enabled) {
  aurora::gx::fifo::sDeindexRequested.store(enabled, std::memory_order_relaxed);
}
namespace aurora::gx::testing {
namespace {
// Read once, at start-up: MP_GX_CHECK_ARRAYS=0 keeps it off, any other value turns it on, and with no
// environment a build without NDEBUG has it on.
bool check_on_by_default() noexcept {
  if (const char* value = std::getenv("MP_GX_CHECK_ARRAYS"); value != nullptr) {
    return std::strcmp(value, "0") != 0;
  }
#ifdef NDEBUG
  return false;
#else
  return true;
#endif
}
} // namespace

std::atomic<bool> vertexArrayCheck{check_on_by_default()};
std::atomic<uint32_t> vertexArrayOverruns{0};

void set_vertex_array_check(bool enabled) noexcept {
  vertexArrayCheck.store(enabled, std::memory_order_relaxed);
}
} // namespace aurora::gx::testing
