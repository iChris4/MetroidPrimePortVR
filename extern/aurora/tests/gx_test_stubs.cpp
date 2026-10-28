// Stub implementations for renderer symbols that the GX/FIFO/command_processor code
// references but that live in the full renderer (gfx, gx.cpp, model/shader.cpp, etc.).
// These allow the test binary to link without pulling in WebGPU runtime.

#include "gx/gx.hpp"
#include "gfx/bloom.hpp"
#include "gfx/volfog.hpp"
#include "gfx/thermal.hpp"
#include "gfx/xray.hpp"
#include "gfx/shadow.hpp"
#include "gfx/clear.hpp"
#include "gfx/resources.hpp"
#include "gfx/depth_peek.hpp"
#include "gfx/recording.hpp"
#include "gfx/perf_counters.hpp"
#include "gfx/stereo_shadow.hpp"
#include "gfx/tex_copy_conv.hpp"
#include "gfx/tex_palette_conv.hpp"
#include "gfx/texture.hpp"
#include "gfx/texture_replacement.hpp"
#include "gx/pipeline.hpp"
#include "gx/shader_info.hpp"
#include "internal.hpp"
#include "webgpu/gpu.hpp"

#include <atomic>
#include <cstdio>
#include <fmt/format.h>

// --- aurora::g_config ---
namespace aurora {
AuroraConfig g_config{};
} // namespace aurora

// --- aurora::log_internal ---
namespace aurora {
void log_internal(AuroraLogLevel level, const char* module, const char* message, unsigned int len) noexcept {
  fprintf(stderr, "[%d] %s: %.*s\n", static_cast<int>(level), module, len, message);
}
} // namespace aurora

// --- fmt::formatter<AuroraLogLevel> ---
auto fmt::formatter<AuroraLogLevel>::format(AuroraLogLevel level, format_context& ctx) const
    -> format_context::iterator {
  return fmt::format_to(ctx.out(), "{}", static_cast<int>(level));
}

// --- GPU resources (default-constructed, not used in tests) ---
namespace aurora::gfx::detail {
Resources& resources() noexcept {
  static Resources resources;
  return resources;
}

void increment_merged_draw_count(uint32_t) noexcept {}
} // namespace aurora::gfx::detail

namespace aurora::webgpu {
GraphicsConfig g_graphicsConfig{};
bool g_dualSourceBlendingSupported = false;
// Tests clear it to see GXPortSceneHdr fail closed.
bool g_testSceneHdrSupported = true;
bool scene_hdr_supported() noexcept { return g_testSceneHdrSupported; }
} // namespace aurora::webgpu

#include "webgpu/gpu_prof.hpp"
namespace aurora::webgpu::gpu_prof {
void set_enabled(bool) {}
bool supported() { return false; }
Result results() { return {}; }
} // namespace aurora::webgpu::gpu_prof

// --- GXState ---
namespace aurora::gx {
GXState g_gxState{};
static std::atomic<u16> sTestDrawSyncToken{0};
void set_draw_sync_token(u16 token) noexcept { sTestDrawSyncToken.store(token, std::memory_order_release); }
u16 draw_sync_token() noexcept { return sTestDrawSyncToken.load(std::memory_order_acquire); }
void set_viewport_policy(AuroraViewportPolicy policy) noexcept {}
} // namespace aurora::gx

namespace aurora::vi {
Vec2<uint32_t> configured_fb_size() noexcept { return {640, 480}; }
void configure(const GXRenderModeObj*) noexcept {}
} // namespace aurora::vi

// --- get_texture ---
namespace aurora::gx {
namespace testing {
bool sampledTexture = false;
uint64_t bindGeneration = 1;
uint32_t bindBuilds = 0;
uint32_t multiviewBuilds = 0;
} // namespace testing
const gfx::TextureBind& get_texture(GXTexMapID id) noexcept { return g_gxState.textures[id]; }
bool shader_has_cut_caster(const ShaderConfig&) noexcept { return false; }
namespace texture {
void invalidate_bindings() noexcept {}
uint64_t current_bind_generation() noexcept { return testing::bindGeneration; }
} // namespace texture
void evict_texture_object(u32 texObjId) noexcept {
  for (auto& obj : g_gxState.loadedTextures) {
    if (obj.texObjId == texObjId) {
      obj.set_no_cache(true);
    }
  }
}
void evict_tlut_object(u32 tlutObjId) noexcept {
  for (auto& obj : g_gxState.loadedTluts) {
    if (obj.tlutObjId == tlutObjId) {
      obj.set_no_cache(true);
    }
  }
}
void evict_copy_texture(const void* dest) noexcept {
  g_gxState.copyTextures.erase(dest);
  for (auto it = g_gxState.copyTextureCache.begin(); it != g_gxState.copyTextureCache.end();) {
    if (it->first.dest == dest) {
      g_gxState.copyTextureCache.erase(it++);
    } else {
      ++it;
    }
  }
}
void trim_copy_sizes(const void* dest, size_t keep) noexcept {}
void shutdown() noexcept {}
Vec2<uint32_t> logical_fb_size() noexcept { return {640, 480}; }
gfx::Viewport map_logical_viewport(const gfx::Viewport& logicalViewport) noexcept { return logicalViewport; }
gfx::ClipRect map_logical_scissor(const gfx::ClipRect& logicalScissor) noexcept { return logicalScissor; }
void set_logical_viewport(const gfx::Viewport& viewport) noexcept {
  g_gxState.logicalViewport = viewport;
  set_render_viewport(map_logical_viewport(viewport));
}
void set_render_viewport(const gfx::Viewport& viewport) noexcept { g_gxState.renderViewport = viewport; }
void set_logical_scissor(const gfx::ClipRect& scissor) noexcept {
  g_gxState.logicalScissor = scissor;
  set_render_scissor(map_logical_scissor(scissor));
}
void set_render_scissor(const gfx::ClipRect& scissor) noexcept { g_gxState.renderScissor = scissor; }
} // namespace aurora::gx

// --- Shader/pipeline stubs ---
namespace aurora::gfx {
bool scene_hdr_pass_active() noexcept;
} // namespace aurora::gfx
namespace aurora::gx {
namespace testing {
ShaderConfig nativeVertexSource{};
}
uint32_t g_testPopulateCount = 0;
void populate_pipeline_config(PipelineConfig& config, GXPrimitive primitive, GXVtxFmt fmt) noexcept {
  ++g_testPopulateCount;
  config = {};
  config.shaderConfig.sceneHdr = gfx::scene_hdr_pass_active();
  config.shaderConfig.attrs = testing::nativeVertexSource.attrs;
  config.shaderConfig.vtxStride = testing::nativeVertexSource.vtxStride;
  config.shaderConfig.nativeVertices = g_gxState.nativeVertices;
  config.cullMode = g_gxState.cullMode;
  config.shaderConfig.currentPnMtx = g_gxState.currentPnMtx;
  config.shaderConfig.mapBatch = g_gxState.mapBatch && primitive == GX_TRIANGLES;
  config.shaderConfig.mapCull = config.shaderConfig.mapBatch ? g_gxState.cullMode : GX_CULL_NONE;
  config.shaderConfig.lineMode = primitive == GX_LINES ? 1 : primitive == GX_LINESTRIP ? 2 : primitive == GX_POINTS ? 3 : 0;
}
GXBindGroups build_bind_groups(const ShaderInfo& info) noexcept {
  return {.textureBindGroup = ++testing::bindBuilds};
}
gfx::BindGroupRef build_multiview_bind_group(const ShaderInfo&) noexcept { return ++testing::multiviewBuilds; }
bool depth_prepass_eligible(u8) noexcept { return false; }
ShaderInfo build_shader_info(const ShaderConfig& config) noexcept {
  ShaderInfo info{};
  info.sampledTextures[0] = testing::sampledTexture;
  info.usesHdrTone = config.sceneHdr && !config.pbr;
  return info;
}
bool per_pixel_lighting() noexcept { return false; }
bool blocking_pipelines() noexcept { return false; }
uint32_t g_testUniformBuildCount = 0;
bool g_testUniformCompatSrcAlphaAdditive = false;
gfx::Range build_uniform(const ShaderInfo& info) noexcept {
  ++g_testUniformBuildCount;
  g_testUniformCompatSrcAlphaAdditive = info.usesHdrTone && g_gxState.blendMode == GX_BM_BLEND &&
                                        g_gxState.blendFacSrc == GX_BL_SRCALPHA &&
                                        (g_gxState.blendFacDst == GX_BL_ONE || g_gxState.blendFacDst == GX_BL_INVSRCALPHA);
  return {.offset = g_testUniformBuildCount, .size = 1};
}

static void write_test_uniform_snapshot(const ShaderInfo& info, ByteBuffer& snapshot) {
  snapshot.clear();
  snapshot.append(g_gxState.lineWidth);
  if (info.usesHdrTone) {
    const bool compat = g_gxState.blendMode == GX_BM_BLEND &&
                        g_gxState.blendFacSrc == GX_BL_SRCALPHA && g_gxState.blendFacDst == GX_BL_ONE;
    snapshot.append(compat);
  }
}
gfx::Range build_uniform(const ShaderInfo& info, std::array<uint32_t, 2>& stereoUniformOffsets,
                         ByteBuffer* snapshot) noexcept {
  stereoUniformOffsets = {UINT32_MAX, UINT32_MAX};
  if (snapshot != nullptr) write_test_uniform_snapshot(info, *snapshot);
  return build_uniform(info);
}
bool uniform_matches(const ShaderInfo& info, const ByteBuffer& snapshot) noexcept {
  ByteBuffer current;
  write_test_uniform_snapshot(info, current);
  return snapshot.size() == current.size() &&
         std::memcmp(snapshot.data(), current.data(), current.size()) == 0;
}
void resolve_sampled_textures(const ShaderInfo& info) noexcept {}
std::array<gfx::BindGroupRef, 2> build_stereo_bind_groups(const ShaderInfo& info) noexcept { return {}; }
u32 dump_shaders(const char* dir) noexcept { return 0; }
void set_shader_override_dir(const char* dir) noexcept {}
void set_draw_shader_log(bool on) noexcept {}
void note_draw_shader(u32 serial, const ShaderConfig& config) noexcept {}
u64 draw_shader_hash(u32 serial) noexcept { return 0; }
bool shader_overridden(u64 hash) noexcept { return false; }
} // namespace aurora::gx
namespace aurora::gfx {
void drop_pipelines() {}
} // namespace aurora::gfx
extern "C" {
void aurora_set_pipeline_eviction(uint32_t) {}
void aurora_trim_pipelines(uint32_t) {}
uint32_t aurora_get_pipeline_count(void) { return 0; }
uint64_t aurora_pipeline_set_context(uint64_t) { return AURORA_PIPELINE_CONTEXT_FRONTEND; }
void aurora_pipeline_prefetch(uint64_t) {}
void aurora_get_pipeline_stats(AuroraPipelineStats* out) { *out = {}; }
}

// --- Stereo replay stubs (the FIFO never runs immersive here) ---
namespace aurora::gfx {
namespace testing {
uint32_t frame = 0;
bool bindGroupsAlive = true;
bool multiview = false;
uint64_t stereoEpoch = 1;
} // namespace testing
uint32_t current_frame() noexcept { return testing::frame; }
bool touch_bind_group(BindGroupRef) { return testing::bindGroupsAlive; }
bool recording_multiview() noexcept { return testing::multiview; }
uint8_t stereo_multiview_mode() noexcept { return 0; }
uint32_t align_uniform(uint32_t value) { return (value + 255u) & ~255u; }
PipelineRef clear_multiview_pipeline_ref(const clear::PipelineConfig&) noexcept { return 0; }
void set_stereo_draw_route(uint8_t route) noexcept {}
uint8_t stereo_draw_route() noexcept { return 0; }
void set_stereo_head_locked_plane(float tanHalfWidth, float tanHalfHeight, float distance) noexcept {}
stereo_replay::HeadLockedPlane stereo_head_locked_plane() noexcept { return {}; }
void set_stereo_placement_3d(const StereoPlacement3D&) noexcept {}
StereoPlacement3D stereo_placement_3d() noexcept { return {}; }
void set_stereo_screen_tex_mtx(uint8_t texSlot, uint8_t pnSlot) noexcept {}
StereoScreenTexMtx stereo_screen_tex_mtx() noexcept { return {}; }
namespace stereo_shadow {
bool active() noexcept { return testing::multiview; }
uint64_t epoch() noexcept { return testing::stereoEpoch; }
} // namespace stereo_shadow
} // namespace aurora::gfx

// --- Buffer push stubs ---
namespace aurora::gfx {
const FrameBufferSizes& frame_buffer_sizes() noexcept { static const FrameBufferSizes sizes; return sizes; }
namespace testing {
std::vector<uint8_t> pushedVerts; // the last push_verts' bytes
bool mergeDraws = false;
ByteBuffer stagedVerts;
ByteBuffer stagedIndices;
} // namespace testing
ByteBuffer* staging_verts() noexcept { return testing::mergeDraws ? &testing::stagedVerts : nullptr; }
ByteBuffer* staging_indices() noexcept { return testing::mergeDraws ? &testing::stagedIndices : nullptr; }
static Range stage_test_bytes(ByteBuffer& buffer, const uint8_t* data, size_t length, size_t alignment) {
  const auto offset = (buffer.size() + alignment - 1) / alignment * alignment;
  if (!buffer.owned() && offset + length > buffer.capacity()) {
    return OverflowRange;
  }
  (void)buffer.append_uninitialized(offset - buffer.size());
  auto* dst = buffer.append_uninitialized(length);
  if (length != 0) {
    std::memcpy(dst, data, length);
  }
  return {static_cast<uint32_t>(offset), static_cast<uint32_t>(length)};
}
Range push_verts(const uint8_t* data, size_t length, size_t alignment) {
  testing::pushedVerts.assign(data, data + length);
  if (testing::mergeDraws) {
    return stage_test_bytes(testing::stagedVerts, data, length, alignment);
  }
  return {};
}
Range push_indices(const uint8_t* data, size_t length, size_t alignment) {
  return testing::mergeDraws ? stage_test_bytes(testing::stagedIndices, data, length, alignment) : Range{};
}
Range push_uniform(const uint8_t* data, size_t length) { return {}; }
// --- The geometry buffer (gfx/geometry_buffer.hpp): the uploads kept for inspection ---
namespace testing {
struct GeometryUploadRecord {
  uint32_t offset;
  std::vector<uint8_t> bytes;
};
std::vector<GeometryUploadRecord> geometryUploads;
uint64_t geometryCapacity = 1u << 20;
} // namespace testing
uint64_t geometry_buffer_capacity() noexcept { return testing::geometryCapacity; }
void queue_geometry_upload(uint32_t offset, const uint8_t* data, uint32_t size) {
  testing::geometryUploads.push_back({offset, std::vector<uint8_t>(data, data + size)});
}
// Tests set it to see draws dropped for want of storage room.
bool g_testStorageFull = false;
Range push_storage(const uint8_t* data, size_t length) { return g_testStorageFull ? OverflowRange : Range{}; }

// The resident regions: tests set them, and see the uploads.
Range g_testResidentRegions[3]{};
uint32_t g_testResidentUploads = 0;
Range resident_region(ResidentBuffer kind) noexcept { return g_testResidentRegions[static_cast<int>(kind)]; }
bool queue_resident_upload(ResidentBuffer kind, uint32_t offset, const uint8_t* data, size_t size) {
  ++g_testResidentUploads;
  return true;
}

Vec2<uint32_t> get_render_target_size() noexcept { return {640, 480}; }
void set_viewport(const Viewport& viewport) noexcept {}
void set_scissor(uint32_t x, uint32_t y, uint32_t w, uint32_t h) noexcept {}
uint32_t get_sample_count() noexcept { return 1; }
// The scene HDR mode the FIFO set, and how many switches it asked for.
bool g_testSceneHdr = false;
uint32_t g_testSceneHdrSwitches = 0;
// Tests set it to see a refused switch.
bool g_testSceneHdrRefuse = false;
bool set_scene_hdr(bool on, bool pause) {
  if (g_testSceneHdrRefuse) {
    return false;
  }
  ++g_testSceneHdrSwitches;
  g_testSceneHdr = on;
  return true;
}
bool scene_hdr_active() noexcept { return g_testSceneHdr; }
// Tests set it to stand for an offscreen pass while the HDR scene is on.
bool g_testSceneHdrOffscreen = false;
bool scene_hdr_pass_active() noexcept { return g_testSceneHdr && !g_testSceneHdrOffscreen; }
RenderTargetLayout get_render_target_layout() noexcept {
  return {
      .colorAttachmentCount = 1,
      .colorAttachments = {{{ColorAttachmentSemantic::SceneColor, wgpu::TextureFormat::RGBA8Unorm}}},
      .depthStencilFormat = wgpu::TextureFormat::Depth24Plus,
      .sampleCount = 1,
  };
}
} // namespace aurora::gfx

// --- Pipeline/draw command stubs ---
namespace aurora::gfx {
namespace clear {
PipelineConfig make_pipeline_config(const RenderTargetLayout& layout, bool clearColor, bool clearAlpha,
                                    bool clearDepth) noexcept {
  return {
      .targetLayoutKey = layout.key,
      .depthStencilFormat = layout.depthStencilFormat,
      .colorAttachmentCount = layout.colorAttachmentCount,
      .msaaSamples = layout.sampleCount,
      .clearColor = clearColor,
      .clearAlpha = clearAlpha,
      .clearDepth = clearDepth,
  };
}
} // namespace clear

template <>
PipelineRef pipeline_ref<clear::PipelineConfig>(const clear::PipelineConfig& config) {
  return 0;
}
template <>
void push_draw_command<clear::DrawData>(clear::DrawData data) {
  // No-op
}
template <>
PipelineRef pipeline_ref<gx::PipelineConfig>(const gx::PipelineConfig& config) {
  return 0;
}
PipelineRef pipeline_ref(const gx::PipelineConfig& config, bool blocking) { return 0; }
// Never live, so the shader-info memo is rebuilt on every draw.
bool touch_pipeline(PipelineRef ref) { return false; }
gx::DrawData g_testLastDraw{};
uint32_t g_testDrawCount = 0;
std::atomic<uint32_t> g_testProcessedDrawCount{0};

template <>
void push_draw_command<gx::DrawData>(gx::DrawData data) {
  g_testLastDraw = data;
  ++g_testDrawCount;
  g_testProcessedDrawCount.fetch_add(1, std::memory_order_release);
}
template <>
gx::DrawData* get_last_draw_command() {
  return testing::mergeDraws && g_testDrawCount != 0 ? &g_testLastDraw : nullptr;
}
} // namespace aurora::gfx

// --- TextureBind::get_descriptor ---
namespace aurora::gfx {
void TextureRef::count_live(int) noexcept {}
wgpu::SamplerDescriptor TextureBind::get_descriptor() const noexcept { return wgpu::SamplerDescriptor{}; }
} // namespace aurora::gfx

// --- Texture creation/write/replacement stubs ---
namespace aurora::gfx {
TextureHandle new_static_texture_2d(uint32_t width, uint32_t height, uint32_t mips, u32 gxFormat,
                                    ArrayRef<uint8_t> data, bool tlut, const char* label) noexcept {
  return {};
}
TextureHandle new_dynamic_texture_2d(uint32_t width, uint32_t height, uint32_t mips, u32 gxFormat,
                                     const char* label) noexcept {
  return {};
}
namespace testing {
// The last new_render_texture's mip count and format.
uint32_t lastRenderTextureMips = 0;
wgpu::TextureFormat lastRenderTextureFormat = wgpu::TextureFormat::Undefined;
} // namespace testing
TextureHandle new_render_texture(uint32_t width, uint32_t height, u32 gxFormat, const char* label, uint32_t mips,
                                 wgpu::TextureFormat format) noexcept {
  testing::lastRenderTextureMips = mips;
  testing::lastRenderTextureFormat = format;
  return {};
}
TextureHandle new_conv_texture(uint32_t width, uint32_t height, u32 gxFormat, const char* label) noexcept { return {}; }
void write_texture(TextureRef& ref, ArrayRef<uint8_t> data) noexcept {}
void queue_texture_upload(TextureUpload upload) {}
void queue_texture_upload_data(const uint8_t* data, size_t length, uint32_t bytesPerRow, uint32_t rowsPerImage,
                               wgpu::TexelCopyTextureInfo tex, wgpu::Extent3D size) {}
void queue_palette_conv(tex_palette_conv::ConvRequest req) {}
namespace testing {
std::atomic<uint32_t> beginOffscreenCount{0};
std::atomic<uint32_t> endOffscreenCount{0};
std::atomic<uint32_t> resolvePassCount{0};
std::atomic<uint32_t> offscreenWidth{0};
std::atomic<uint32_t> offscreenHeight{0};
// The last resolve_pass_into's ResolveHdr (hdr::Tone{} / false when it passed none).
bool lastResolveHasHdr = false;
ResolveHdr lastResolveHdr{};
bool lastResolveMips = false;
} // namespace testing

void resolve_pass_into(TextureHandle texture, ClipRect rect, bool clearColor, bool clearAlpha, bool clearDepth,
                       Vec4<float> clearColorValue, float clearDepthValue, GXTexFmt resolveFormat, int probeFace,
                       bool resolveMips, const ResolveHdr* hdr, bool opaqueAlpha) {
  testing::resolvePassCount.fetch_add(1, std::memory_order_release);
  testing::lastResolveHasHdr = hdr != nullptr;
  testing::lastResolveHdr = hdr != nullptr ? *hdr : ResolveHdr{};
  testing::lastResolveMips = resolveMips;
}
namespace probe {
TextureHandle face(uint32_t face) { return {}; }
void create_cube(uint32_t id, uint32_t size, uint32_t mipCount, const uint8_t* texels, size_t length) {}
void destroy_cube(uint32_t id) {}
bool has_cube(uint32_t id) { return false; }
bool blend_cubes(uint32_t dst, const uint32_t* src, const float* weights, uint32_t count) { return false; }
void create_volume(uint32_t id, uint32_t sizeX, uint32_t sizeY, uint32_t sizeZ, const uint8_t* texels, size_t length) {}
void destroy_volume(uint32_t id) {}
bool set_brdf_lut(const uint8_t* texels, size_t length) { return length == 256; }
bool has_volume(uint32_t id) { return false; }
bool lightmap_available() { return false; }
bool lightmap_bc_supported() { return false; }
void create_lightmap(uint32_t id, uint32_t width, uint32_t height, uint32_t layers, uint32_t format,
                     const uint8_t* texels, size_t length) {}
void destroy_lightmap(uint32_t id) {}
bool has_lightmap(uint32_t id) { return false; }
} // namespace probe
void begin_offscreen(uint32_t width, uint32_t height) {
  testing::offscreenWidth.store(width, std::memory_order_relaxed);
  testing::offscreenHeight.store(height, std::memory_order_relaxed);
  testing::beginOffscreenCount.fetch_add(1, std::memory_order_release);
}
void end_offscreen() { testing::endOffscreenCount.fetch_add(1, std::memory_order_release); }
bool is_offscreen() noexcept { return false; }
} // namespace aurora::gfx

namespace aurora::gfx::bloom {
namespace testing {
// Whether ensure_task succeeds, and what the POST_PROCESS handler recorded.
bool task = false;
uint32_t records = 0;
Params lastRecord{};
bool leaveScene = false; // record leaves the HDR scene, as the real one does
} // namespace testing
bool ensure_task() { return testing::task; }
bool record(const Params& params) {
  ++testing::records;
  testing::lastRecord = params;
  if (testing::leaveScene) {
    g_testSceneHdr = false;
  }
  return testing::task;
}
void set_grade_lut(uint32_t id, const uint8_t* rgba) {}
bool measure_scene(float exposure) { return false; }
void after_submit() noexcept {}
bool frame_radiance(float out[3], uint32_t& serial) { return false; }
void shutdown() {}
} // namespace aurora::gfx::bloom

namespace aurora::gfx::volfog {
bool ensure_task() { return false; }
bool record(const Params& params) { return false; }
void shutdown() {}
} // namespace aurora::gfx::volfog

namespace aurora::gfx::thermal {
bool set_lut(const uint8_t*, uint32_t) { return false; }
bool has_lut() { return false; }
bool ensure_task() { return false; }
bool record(const Params& params) { return false; }
void shutdown() {}
} // namespace aurora::gfx::thermal

namespace aurora::gfx::xray {
bool ensure_task() { return false; }
bool record(const Params& params) { return false; }
void shutdown() {}
} // namespace aurora::gfx::xray

namespace aurora::gfx::shadow {
bool ensure_task() { return false; }
bool set_frame(const float worldToView[3][4], const cascade::Frustum& frustum, const float sunDir[3], float distance,
               const float color[3], Uniform& out) {
  return false;
}
bool fit_frame(const float worldToView[3][4], const cascade::Frustum& frustum, const float sunDir[3], float distance,
               cascade::Fit out[cascade::Count]) {
  return false;
}
bool box_casts(const float worldToView[3][4], const cascade::Frustum& frustum, const float sunDir[3], float distance,
               const float min[3], const float max[3]) {
  return false;
}
uint32_t last_caster_count() { return 0; }
void add_caster(const gx::DrawData& draw) {}
bool record() { return false; }
const wgpu::TextureView& map_view() {
  static const wgpu::TextureView view;
  return view;
}
const wgpu::Sampler& sampler() {
  static const wgpu::Sampler sampler;
  return sampler;
}
void shutdown() {}
} // namespace aurora::gfx::shadow

namespace aurora::gfx::actor_shadow {
bool ensure_task() { return false; }
void add_caster(uint32_t layer, const gx::DrawData& draw) {}
bool record() { return false; }
const wgpu::TextureView& map_view() {
  static const wgpu::TextureView view;
  return view;
}
const wgpu::Sampler& sampler() {
  static const wgpu::Sampler sampler;
  return sampler;
}
void shutdown() {}
} // namespace aurora::gfx::actor_shadow

namespace aurora::gfx::depth_peek {
namespace {
bool s_snapshotRequested = false;
uint32_t s_width = 0;
uint32_t s_height = 0;
std::vector<uint32_t> s_data;
} // namespace

void initialize() {}
void shutdown() {}
void request_snapshot() noexcept { s_snapshotRequested = true; }
void poll() noexcept {}
void encode_frame_snapshot(const wgpu::CommandEncoder& cmd, const wgpu::TextureView& depthView,
                           wgpu::Extent3D sourceSize, uint32_t msaaSamples) noexcept {}
void after_submit() noexcept {}

bool read_latest(uint16_t x, uint16_t y, uint32_t& z) noexcept {
  if (x >= s_width || y >= s_height || s_data.empty()) {
    return false;
  }
  z = s_data[static_cast<size_t>(y) * s_width + x] & 0x00ffffffu;
  return true;
}

namespace testing {
void reset() noexcept {
  s_snapshotRequested = false;
  s_width = 0;
  s_height = 0;
  s_data.clear();
}

bool snapshot_requested() noexcept { return s_snapshotRequested; }

void set_latest(uint32_t width, uint32_t height, const std::vector<uint32_t>& data) {
  s_width = width;
  s_height = height;
  s_data = data;
}
} // namespace testing
} // namespace aurora::gfx::depth_peek

namespace aurora::gfx::testing {
// When set, RGB565 needs a conversion, as it does on a real device (tex_copy_conv's pipelines).
bool copyConvertsRgb565 = false;
} // namespace aurora::gfx::testing
namespace aurora::gfx::tex_copy_conv {
bool needs_conversion(GXTexFmt fmt) { return testing::copyConvertsRgb565 && fmt == GX_TF_RGB565; }
} // namespace aurora::gfx::tex_copy_conv

namespace aurora::gfx::tex_palette_conv {
void queue(ConvRequest req) {}
} // namespace aurora::gfx::tex_palette_conv

namespace aurora::gfx::texture_replacement {
u32 compute_texture_upload_size(const GXTexObj_& obj) noexcept { return 0; }
bool has_replacement(const GXTexObj_&) noexcept { return false; }
bool has_replacement(const GXTexObj_&, const GXTlutObj_&) noexcept { return false; }
} // namespace aurora::gfx::texture_replacement

// --- Window stub ---
#include "../lib/window.hpp"
namespace aurora::window {
AuroraWindowSize get_window_size() { return {640, 480, 640, 480, 640, 480, 1.0f}; }
void set_frame_buffer_aspect_fit(bool) {}
} // namespace aurora::window

// --- WebGPU C API stubs (prevent linker errors from wgpu:: destructors) ---
extern "C" {
void wgpuDeviceRelease(WGPUDevice) {}
void wgpuQueueRelease(WGPUQueue) {}
void wgpuSurfaceRelease(WGPUSurface) {}
void wgpuBufferRelease(WGPUBuffer) {}
void wgpuTextureRelease(WGPUTexture) {}
void wgpuTextureViewRelease(WGPUTextureView) {}
void wgpuSamplerRelease(WGPUSampler) {}
void wgpuShaderModuleRelease(WGPUShaderModule) {}
void wgpuRenderPipelineRelease(WGPURenderPipeline) {}
void wgpuBindGroupRelease(WGPUBindGroup) {}
void wgpuBindGroupLayoutRelease(WGPUBindGroupLayout) {}
void wgpuPipelineLayoutRelease(WGPUPipelineLayout) {}
void wgpuInstanceRelease(WGPUInstance) {}
void wgpuDeviceAddRef(WGPUDevice) {}
void wgpuQueueAddRef(WGPUQueue) {}
void wgpuSurfaceAddRef(WGPUSurface) {}
void wgpuBufferAddRef(WGPUBuffer) {}
void wgpuTextureAddRef(WGPUTexture) {}
void wgpuTextureViewAddRef(WGPUTextureView) {}
void wgpuInstanceAddRef(WGPUInstance) {}
}

void aurora::gfx::push_debug_group(std::string) {}
void push_debug_group(const char*) {}
void pop_debug_group() {}
void aurora::gfx::insert_debug_marker(std::string) {}

// Disabled diagnostics for the asset-free FIFO tests.
namespace aurora::gfx::perf {
bool enabled() noexcept { return false; }
std::atomic<uint64_t> g_fifoNs{0};
std::atomic<uint64_t> g_fifoBytes{0};
std::atomic<uint64_t> g_encodeNs{0};
std::atomic<uint64_t> g_submitNs{0};
std::atomic<uint64_t> g_fifoXfTicks{0};
std::atomic<uint64_t> g_fifoBpTicks{0};
std::atomic<uint64_t> g_fifoCpTicks{0};
std::atomic<uint64_t> g_fifoAuroraTicks{0};
std::atomic<uint64_t> g_fifoVertsTicks{0};
std::atomic<uint64_t> g_fifoPipelineTicks{0};
std::atomic<uint64_t> g_fifoBindsTicks{0};
std::atomic<uint64_t> g_fifoUniformTicks{0};
std::atomic<uint64_t> g_fifoPushTicks{0};
std::atomic<uint32_t> g_fifoXfLoads{0};
std::atomic<uint32_t> g_fifoBpLoads{0};
std::atomic<uint32_t> g_fifoDrawsMerged{0};
std::atomic<uint32_t> g_fifoDrawsPushed{0};
std::atomic<uint32_t> g_fifoPipelineBuilds{0};
std::atomic<uint32_t> g_fifoBindGroupBuilds{0};
std::atomic<uint32_t> g_fifoBindGroupMisses{0};
std::atomic<uint64_t> g_fifoResolveTicks{0};
std::atomic<uint32_t> g_fifoUniformBuilds{0};
std::atomic<uint32_t> g_mergeBreaks[BreakCount]{};
std::atomic<uint64_t> g_fifoDeindexTicks{0};
std::atomic<uint32_t> g_fifoDeindexedVerts{0};
std::atomic<uint32_t> g_fifoCachedDlCalls{0};
std::atomic<uint64_t> g_fifoCachedDlBytes{0};
std::atomic<uint64_t> g_fifoCachedDlVertBytes{0};
std::atomic<uint64_t> g_fifoCachedDlIndexBytes{0};
std::atomic<uint32_t> g_fifoCachedDlVerts{0};
std::atomic<uint32_t> g_fifoGeometryHits{0};
std::atomic<uint32_t> g_fifoGeometryMisses{0};
std::atomic<uint32_t> g_fifoNativeDraws{0};
std::atomic<uint32_t> g_geometryEntries{0};
std::atomic<uint64_t> g_geometryResidentBytes{0};
std::atomic<uint64_t> g_drainWaitNs{0};
std::atomic<uint32_t> g_drainCalls{0};
}
