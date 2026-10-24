#include "probe.hpp"

#include "tex_copy_conv.hpp"
#include "../gx/fifo.hpp"
#include "../logging.hpp"
#include "../webgpu/gpu.hpp"

#include <aurora/gfx.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aurora::gfx::probe {
using webgpu::g_device;

namespace {
Module Log("aurora::gfx::probe");
std::atomic<int64_t> g_liveCount;
std::atomic<int64_t> g_liveBytes;
// Counts its textures in the live totals for as long as the owner holds it.
struct Live {
  uint32_t count = 0;
  uint64_t bytes = 0;
  Live() = default;
  Live(uint32_t count, uint64_t bytes) : count(count), bytes(bytes) { add(1); }
  Live(Live&& o) noexcept : count(std::exchange(o.count, 0)), bytes(std::exchange(o.bytes, 0)) {}
  Live& operator=(Live&& o) noexcept {
    if (this != &o) {
      add(-1);
      count = std::exchange(o.count, 0);
      bytes = std::exchange(o.bytes, 0);
    }
    return *this;
  }
  ~Live() { add(-1); }
  void add(int sign) const {
    g_liveCount += sign * int64_t(count);
    g_liveBytes += sign * int64_t(bytes);
  }
};
// A cube's whole mip chain, all faces.
uint64_t cube_bytes(uint32_t size, uint32_t mipCount, uint32_t texelSize) {
  uint64_t total = 0;
  for (uint32_t m = 0; m < mipCount; ++m) {
    const uint64_t edge = std::max(size >> m, 1u);
    total += edge * edge * texelSize * 6;
  }
  return total;
}
constexpr uint32_t FaceCount = 6;
wgpu::Texture g_texture;
wgpu::Texture g_mipScratch; // Compatibility: a plain 2D texture the mip chain is downsampled in
std::array<TextureHandle, MipCount> g_scratchMips;
wgpu::TextureView g_cubeView;
wgpu::Sampler g_sampler;
std::array<std::array<TextureHandle, MipCount>, FaceCount> g_faces;
struct RoomCube {
  wgpu::Texture texture;
  wgpu::TextureView view;
  uint32_t size = 0;
  uint32_t mipCount = 0;
  // A blend target's faces, one 2D view per face and mip (face-major).
  std::vector<wgpu::TextureView> faces;
  Live live;
};
// Written on the FIFO thread (create/destroy) and the game thread (blend targets, with the FIFO
// drained); the lock covers lookups from both.
std::mutex g_cubeMutex;
std::unordered_map<uint32_t, RoomCube> g_roomCubes;

// The cube blend: a weighted sum of up to four room cubes rendered into a blend target, face by
// face and mip by mip, as Remastered blends the reflection probes around the camera.
constexpr uint64_t BlendSlotSize = 256;
struct BlendUniform {
  float weights[4];
  uint32_t face;
  float lod;
  uint32_t count;
  uint32_t pad;
};
struct BlendJob {
  std::vector<wgpu::TextureView> targets; // face-major, mipCount per face
  std::array<wgpu::TextureView, MaxBlend> sources;
  std::array<float, MaxBlend> weights{};
  uint32_t count = 0;
  uint32_t mipCount = 0;
};
std::mutex g_blendMutex;
std::unordered_map<uint64_t, BlendJob> g_blendJobs;
uint64_t g_nextBlendJob = 1;
EncoderTaskId g_blendTask = InvalidEncoderTask;
wgpu::ShaderModule g_blendModule;
wgpu::BindGroupLayout g_blendLayout;
wgpu::RenderPipeline g_blendPipeline;

constexpr char BlendShader[] = R"(
struct Blend {
  weights: vec4f,
  face: u32,
  lod: f32,
  count: u32,
  pad: u32,
};
@group(0) @binding(0) var cubeSampler: sampler;
@group(0) @binding(1) var cube0: texture_cube<f32>;
@group(0) @binding(2) var cube1: texture_cube<f32>;
@group(0) @binding(3) var cube2: texture_cube<f32>;
@group(0) @binding(4) var cube3: texture_cube<f32>;
@group(0) @binding(5) var<uniform> blend: Blend;

struct VertexOut {
  @builtin(position) pos: vec4f,
  @location(0) uv: vec2f,
};

@vertex
fn vs_main(@builtin(vertex_index) index: u32) -> VertexOut {
  let p = vec2f(f32((index << 1u) & 2u), f32(index & 2u));
  var out: VertexOut;
  out.pos = vec4f(p * 2.0 - 1.0, 0.0, 1.0);
  out.uv = vec2f(p.x, 1.0 - p.y);
  return out;
}

@fragment
fn fs_main(in: VertexOut) -> @location(0) vec4f {
  let s = in.uv.x * 2.0 - 1.0;
  let t = in.uv.y * 2.0 - 1.0;
  var dir: vec3f;
  switch blend.face {
    case 0u: { dir = vec3f(1.0, -t, -s); }
    case 1u: { dir = vec3f(-1.0, -t, s); }
    case 2u: { dir = vec3f(s, 1.0, t); }
    case 3u: { dir = vec3f(s, -1.0, -t); }
    case 4u: { dir = vec3f(s, -t, 1.0); }
    default: { dir = vec3f(-s, -t, -1.0); }
  }
  var sum = textureSampleLevel(cube0, cubeSampler, dir, blend.lod) * blend.weights.x;
  if (blend.count > 1u) {
    sum += textureSampleLevel(cube1, cubeSampler, dir, blend.lod) * blend.weights.y;
  }
  if (blend.count > 2u) {
    sum += textureSampleLevel(cube2, cubeSampler, dir, blend.lod) * blend.weights.z;
  }
  if (blend.count > 3u) {
    sum += textureSampleLevel(cube3, cubeSampler, dir, blend.lod) * blend.weights.w;
  }
  return sum;
}
)";
struct Volume {
  std::array<wgpu::Texture, VolumeTextures> textures;
  std::array<wgpu::TextureView, VolumeTextures> views;
  Live live;
};
std::unordered_map<uint32_t, Volume> g_volumes;
struct Lightmap {
  wgpu::Texture texture;
  wgpu::TextureView view;
  Live live;
};
std::unordered_map<uint32_t, Lightmap> g_lightmaps;
wgpu::Texture g_emptyLightmap;
wgpu::TextureView g_emptyLightmapView;
wgpu::Texture g_brdfLut;
wgpu::TextureView g_brdfLutView;
wgpu::Texture g_emptyBrdfLut;
wgpu::TextureView g_emptyBrdfLutView;
wgpu::Texture g_emptyVolume;
wgpu::TextureView g_emptyVolumeView;
Live g_probeLive, g_emptyLightmapLive, g_brdfLive, g_emptyBrdfLive, g_emptyVolumeLive;

void ensure() {
  if (g_texture) {
    return;
  }
  const auto format = webgpu::g_graphicsConfig.surfaceConfiguration.format;
  wgpu::TextureBindingViewDimension cubeBinding;
  cubeBinding.textureBindingViewDimension = wgpu::TextureViewDimension::Cube;
  const wgpu::TextureDescriptor textureDescriptor{
      .nextInChain = &cubeBinding,
      .label = "PBR Probe",
      .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::RenderAttachment |
               (webgpu::g_hasCoreFeatures ? wgpu::TextureUsage::None
                                          : wgpu::TextureUsage::CopySrc | wgpu::TextureUsage::CopyDst),
      .dimension = wgpu::TextureDimension::e2D,
      .size = {Size, Size, FaceCount},
      .format = format,
      .mipLevelCount = MipCount,
      .sampleCount = 1,
  };
  g_texture = g_device.CreateTexture(&textureDescriptor);
  g_probeLive = Live(1, calc_texture_size(format, Size, Size, MipCount) * FaceCount);
  const wgpu::TextureViewDescriptor cubeDescriptor{
      .label = "PBR Probe cube view",
      .format = format,
      .dimension = wgpu::TextureViewDimension::Cube,
      .baseMipLevel = 0,
      .mipLevelCount = MipCount,
      .baseArrayLayer = 0,
      .arrayLayerCount = FaceCount,
  };
  g_cubeView = g_texture.CreateView(&cubeDescriptor);
  for (uint32_t f = 0; f < FaceCount; ++f) {
    for (uint32_t m = 0; m < MipCount; ++m) {
      const wgpu::TextureViewDescriptor viewDescriptor{
          .label = "PBR Probe face view",
          .format = format,
          .dimension = wgpu::TextureViewDimension::e2D,
          .baseMipLevel = m,
          .mipLevelCount = 1,
          .baseArrayLayer = f,
          .arrayLayerCount = 1,
      };
      auto view = g_texture.CreateView(&viewDescriptor);
      const wgpu::Extent3D size{Size >> m, Size >> m, 1};
      g_faces[f][m] = std::make_shared<TextureRef>(g_texture, view, view, size, format, 1, GX_TF_RGBA8);
    }
  }
  constexpr wgpu::SamplerDescriptor samplerDescriptor{
      .label = "PBR Probe sampler",
      .addressModeU = wgpu::AddressMode::ClampToEdge,
      .addressModeV = wgpu::AddressMode::ClampToEdge,
      .addressModeW = wgpu::AddressMode::ClampToEdge,
      .magFilter = wgpu::FilterMode::Linear,
      .minFilter = wgpu::FilterMode::Linear,
      .mipmapFilter = wgpu::MipmapFilterMode::Linear,
  };
  g_sampler = g_device.CreateSampler(&samplerDescriptor);
}

void ensure_blend_pipeline() {
  if (g_blendPipeline) {
    return;
  }
  wgpu::ShaderSourceWGSL source{};
  source.code = BlendShader;
  const wgpu::ShaderModuleDescriptor moduleDescriptor{.nextInChain = &source, .label = "PBR Cube Blend Module"};
  g_blendModule = g_device.CreateShaderModule(&moduleDescriptor);
  std::array<wgpu::BindGroupLayoutEntry, 2 + MaxBlend> entries{};
  entries[0] = wgpu::BindGroupLayoutEntry{
      .binding = 0,
      .visibility = wgpu::ShaderStage::Fragment,
      .sampler = {.type = wgpu::SamplerBindingType::Filtering},
  };
  for (uint32_t i = 0; i < MaxBlend; ++i) {
    entries[1 + i] = wgpu::BindGroupLayoutEntry{
        .binding = 1 + i,
        .visibility = wgpu::ShaderStage::Fragment,
        .texture = {.sampleType = wgpu::TextureSampleType::Float, .viewDimension = wgpu::TextureViewDimension::Cube},
    };
  }
  entries[1 + MaxBlend] = wgpu::BindGroupLayoutEntry{
      .binding = 1 + MaxBlend,
      .visibility = wgpu::ShaderStage::Fragment,
      .buffer = {.type = wgpu::BufferBindingType::Uniform, .minBindingSize = sizeof(BlendUniform)},
  };
  const wgpu::BindGroupLayoutDescriptor layoutDescriptor{
      .label = "PBR Cube Blend Bind Group Layout",
      .entryCount = entries.size(),
      .entries = entries.data(),
  };
  g_blendLayout = g_device.CreateBindGroupLayout(&layoutDescriptor);
  const wgpu::PipelineLayoutDescriptor pipelineLayoutDescriptor{
      .label = "PBR Cube Blend Pipeline Layout",
      .bindGroupLayoutCount = 1,
      .bindGroupLayouts = &g_blendLayout,
  };
  const auto pipelineLayout = g_device.CreatePipelineLayout(&pipelineLayoutDescriptor);
  const wgpu::ColorTargetState target{
      .format = wgpu::TextureFormat::RGBA16Float,
      .writeMask = wgpu::ColorWriteMask::All,
  };
  const wgpu::FragmentState fragment{
      .module = g_blendModule,
      .entryPoint = "fs_main",
      .targetCount = 1,
      .targets = &target,
  };
  const wgpu::RenderPipelineDescriptor descriptor{
      .label = "PBR Cube Blend",
      .layout = pipelineLayout,
      .vertex = {.module = g_blendModule, .entryPoint = "vs_main"},
      .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList},
      .multisample = {.count = 1, .mask = UINT32_MAX},
      .fragment = &fragment,
  };
  g_blendPipeline = g_device.CreateRenderPipeline(&descriptor);
}

void encode_blend(const EncoderTaskContext& ctx, const wgpu::CommandEncoder& cmd, const void* payload,
                  size_t payloadSize, void*) {
  uint64_t key = 0;
  if (payloadSize != sizeof(key)) {
    return;
  }
  std::memcpy(&key, payload, sizeof(key));
  BlendJob job;
  {
    std::lock_guard lock(g_blendMutex);
    const auto found = g_blendJobs.find(key);
    if (found == g_blendJobs.end()) {
      return;
    }
    job = std::move(found->second);
    g_blendJobs.erase(found);
  }
  if (job.count == 0 || job.mipCount == 0 || job.targets.size() != size_t(FaceCount) * job.mipCount) {
    return;
  }
  ensure();
  ensure_blend_pipeline();
  const uint32_t draws = FaceCount * job.mipCount;
  std::vector<uint8_t> slots(size_t(draws) * BlendSlotSize);
  for (uint32_t f = 0; f < FaceCount; ++f) {
    for (uint32_t m = 0; m < job.mipCount; ++m) {
      BlendUniform u{};
      std::memcpy(u.weights, job.weights.data(), sizeof(u.weights));
      u.face = f;
      u.lod = float(m);
      u.count = job.count;
      std::memcpy(slots.data() + size_t(f * job.mipCount + m) * BlendSlotSize, &u, sizeof(u));
    }
  }
  // A buffer per job: two blends in one submit must not share uniforms.
  const wgpu::BufferDescriptor bufferDescriptor{
      .label = "PBR Cube Blend Uniforms",
      .usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
      .size = slots.size(),
  };
  const auto uniforms = g_device.CreateBuffer(&bufferDescriptor);
  ctx.queue.WriteBuffer(uniforms, 0, slots.data(), slots.size());
  for (uint32_t i = job.count; i < MaxBlend; ++i) {
    job.sources[i] = job.sources[0];
  }
  for (uint32_t d = 0; d < draws; ++d) {
    std::array<wgpu::BindGroupEntry, 2 + MaxBlend> entries{};
    entries[0] = wgpu::BindGroupEntry{.binding = 0, .sampler = g_sampler};
    for (uint32_t i = 0; i < MaxBlend; ++i) {
      entries[1 + i] = wgpu::BindGroupEntry{.binding = 1 + i, .textureView = job.sources[i]};
    }
    entries[1 + MaxBlend] = wgpu::BindGroupEntry{
        .binding = 1 + MaxBlend, .buffer = uniforms, .offset = d * BlendSlotSize, .size = sizeof(BlendUniform)};
    const wgpu::BindGroupDescriptor groupDescriptor{
        .layout = g_blendLayout,
        .entryCount = entries.size(),
        .entries = entries.data(),
    };
    const auto group = g_device.CreateBindGroup(&groupDescriptor);
    const wgpu::RenderPassColorAttachment attachment{
        .view = job.targets[d],
        .loadOp = wgpu::LoadOp::Clear,
        .storeOp = wgpu::StoreOp::Store,
        .clearValue = {0.0, 0.0, 0.0, 0.0},
    };
    const wgpu::RenderPassDescriptor passDescriptor{
        .label = "PBR Cube Blend Pass",
        .colorAttachmentCount = 1,
        .colorAttachments = &attachment,
    };
    const auto pass = cmd.BeginRenderPass(&passDescriptor);
    pass.SetPipeline(g_blendPipeline);
    pass.SetBindGroup(0, group);
    pass.Draw(3);
    pass.End();
  }
}
} // namespace

void shutdown() {
  for (auto& face : g_faces) {
    for (auto& mip : face) {
      mip.reset();
    }
  }
  {
    std::lock_guard lock(g_blendMutex);
    g_blendJobs.clear();
  }
  if (g_blendTask != InvalidEncoderTask) {
    unregister_encoder_task_type(g_blendTask);
    g_blendTask = InvalidEncoderTask;
  }
  g_blendPipeline = {};
  g_blendLayout = {};
  g_blendModule = {};
  std::lock_guard lock(g_cubeMutex);
  g_roomCubes.clear();
  g_volumes.clear();
  g_lightmaps.clear();
  g_emptyLightmapView = {};
  g_emptyLightmap = {};
  g_emptyVolumeView = {};
  g_emptyVolume = {};
  g_brdfLut = {};
  g_brdfLutView = {};
  g_emptyBrdfLut = {};
  g_emptyBrdfLutView = {};
  g_emptyLightmapLive = {};
  g_emptyVolumeLive = {};
  g_brdfLive = {};
  g_emptyBrdfLive = {};
  g_sampler = {};
  g_cubeView = {};
  for (auto& mip : g_scratchMips) {
    mip.reset();
  }
  g_mipScratch = {};
  g_texture = {};
  g_probeLive = {};
}

const wgpu::TextureView& cube_view() {
  ensure();
  return g_cubeView;
}

const wgpu::Sampler& sampler() {
  ensure();
  return g_sampler;
}

TextureHandle face(uint32_t face) {
  ensure();
  return g_faces[face % FaceCount][0];
}

// Compatibility devices reject sampling through a 2D view of the Cube-bound probe texture. The same chain, with the
// same blits, runs in a plain 2D scratch texture instead; the base level is copied in and each mip copied back
// (copies are exact), so the result matches the 2D-view path bit for bit.
static void encode_mips_compat(const wgpu::CommandEncoder& cmd, uint32_t face, Range uvRange) {
  const auto format = webgpu::g_graphicsConfig.surfaceConfiguration.format;
  if (!g_mipScratch) {
    const wgpu::TextureDescriptor descriptor{
        .label = "PBR Probe mip scratch",
        .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::RenderAttachment |
                 wgpu::TextureUsage::CopySrc | wgpu::TextureUsage::CopyDst,
        .dimension = wgpu::TextureDimension::e2D,
        .size = {Size, Size, 1},
        .format = format,
        .mipLevelCount = MipCount,
        .sampleCount = 1,
    };
    g_mipScratch = g_device.CreateTexture(&descriptor);
    for (uint32_t m = 0; m < MipCount; ++m) {
      const wgpu::TextureViewDescriptor viewDescriptor{
          .label = "PBR Probe mip scratch view",
          .format = format,
          .dimension = wgpu::TextureViewDimension::e2D,
          .baseMipLevel = m,
          .mipLevelCount = 1,
          .baseArrayLayer = 0,
          .arrayLayerCount = 1,
      };
      auto view = g_mipScratch.CreateView(&viewDescriptor);
      const wgpu::Extent3D size{Size >> m, Size >> m, 1};
      g_scratchMips[m] = std::make_shared<TextureRef>(g_mipScratch, view, view, size, format, 1, GX_TF_RGBA8);
    }
  }
  const auto copyLevel = [&](const wgpu::Texture& from, uint32_t fromMip, uint32_t fromLayer, const wgpu::Texture& to,
                             uint32_t toMip, uint32_t toLayer) {
    const wgpu::TexelCopyTextureInfo src{.texture = from, .mipLevel = fromMip, .origin = {0, 0, fromLayer}};
    const wgpu::TexelCopyTextureInfo dst{.texture = to, .mipLevel = toMip, .origin = {0, 0, toLayer}};
    const wgpu::Extent3D extent{Size >> fromMip, Size >> fromMip, 1};
    cmd.CopyTextureToTexture(&src, &dst, &extent);
  };
  copyLevel(g_texture, 0, face, g_mipScratch, 0, 0);
  for (uint32_t m = 1; m < MipCount; ++m) {
    tex_copy_conv::blit(cmd, tex_copy_conv::ConvRequest{
                                 .fmt = GX_TF_RGBA8,
                                 .srcView = g_scratchMips[m - 1]->sampleTextureView,
                                 .uniformRange = uvRange,
                                 .dst = g_scratchMips[m],
                                 .sampleFilter = tex_copy_conv::SampleFilter::Linear,
                             });
    copyLevel(g_mipScratch, m, 0, g_texture, m, face);
  }
}

void encode_mips(const wgpu::CommandEncoder& cmd, uint32_t face, Range uvRange) {
  ensure();
  face %= FaceCount;
  if (!webgpu::g_hasCoreFeatures) {
    encode_mips_compat(cmd, face, uvRange);
    return;
  }
  const auto& mips = g_faces[face];
  for (uint32_t m = 1; m < MipCount; ++m) {
    // A 2:1 bilinear blit is a box filter.
    tex_copy_conv::blit(cmd, tex_copy_conv::ConvRequest{
                                 .fmt = GX_TF_RGBA8,
                                 .srcView = mips[m - 1]->sampleTextureView,
                                 .uniformRange = uvRange,
                                 .dst = mips[m],
                                 .sampleFilter = tex_copy_conv::SampleFilter::Linear,
                             });
  }
}

void live_stats(uint32_t& count, uint64_t& bytes) {
  count = static_cast<uint32_t>(std::max<int64_t>(g_liveCount.load(), 0));
  bytes = static_cast<uint64_t>(std::max<int64_t>(g_liveBytes.load(), 0));
}

void create_cube(uint32_t id, uint32_t size, uint32_t mipCount, const uint8_t* texels, size_t length) {
  constexpr auto format = wgpu::TextureFormat::RGBA16Float;
  constexpr uint32_t texelSize = 8;
  size_t needed = 0;
  for (uint32_t m = 0; m < mipCount; ++m) {
    const size_t edge = std::max(size >> m, 1u);
    needed += edge * edge * texelSize * FaceCount;
  }
  if (id == 0 || size == 0 || mipCount == 0 || length < needed) {
    return;
  }
  wgpu::TextureBindingViewDimension cubeBinding;
  cubeBinding.textureBindingViewDimension = wgpu::TextureViewDimension::Cube;
  const wgpu::TextureDescriptor textureDescriptor{
      .nextInChain = &cubeBinding,
      .label = "PBR room cube",
      .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst,
      .dimension = wgpu::TextureDimension::e2D,
      .size = {size, size, FaceCount},
      .format = format,
      .mipLevelCount = mipCount,
      .sampleCount = 1,
  };
  RoomCube cube;
  cube.size = size;
  cube.mipCount = mipCount;
  cube.texture = g_device.CreateTexture(&textureDescriptor);
  cube.live = Live(1, cube_bytes(size, mipCount, texelSize));
  const uint8_t* in = texels;
  for (uint32_t f = 0; f < FaceCount; ++f) {
    for (uint32_t m = 0; m < mipCount; ++m) {
      const uint32_t edge = std::max(size >> m, 1u);
      const wgpu::TexelCopyTextureInfo dst{
          .texture = cube.texture,
          .mipLevel = m,
          .origin = {0, 0, f},
      };
      const wgpu::Extent3D extent{edge, edge, 1};
      const size_t bytes = size_t(edge) * edge * texelSize;
      // Through the frame's uploads: the render worker may be submitting the last frame, and the queue is not
      // safe to write from here meanwhile.
      queue_texture_upload_data(in, edge * texelSize, edge, dst, extent);
      in += bytes;
    }
  }
  const wgpu::TextureViewDescriptor cubeDescriptor{
      .label = "PBR room cube view",
      .format = format,
      .dimension = wgpu::TextureViewDimension::Cube,
      .baseMipLevel = 0,
      .mipLevelCount = mipCount,
      .baseArrayLayer = 0,
      .arrayLayerCount = FaceCount,
  };
  cube.view = cube.texture.CreateView(&cubeDescriptor);
  std::lock_guard lock(g_cubeMutex);
  g_roomCubes[id] = std::move(cube);
}

void destroy_cube(uint32_t id) {
  std::lock_guard lock(g_cubeMutex);
  g_roomCubes.erase(id);
}

bool has_cube(uint32_t id) {
  std::lock_guard lock(g_cubeMutex);
  return g_roomCubes.find(id) != g_roomCubes.end();
}

const wgpu::TextureView& cube_view(uint32_t id) {
  {
    // Map nodes are stable, and only the FIFO thread (or the game thread with it drained)
    // erases, so the reference outlives the lock.
    std::lock_guard lock(g_cubeMutex);
    const auto found = g_roomCubes.find(id);
    if (found != g_roomCubes.end()) {
      return found->second.view;
    }
  }
  return cube_view();
}

bool blend_cubes(uint32_t dst, const uint32_t* src, const float* weights, uint32_t count) {
  if (dst == 0 || src == nullptr || weights == nullptr || count == 0 || count > MaxBlend) {
    return false;
  }
  if (g_blendTask == InvalidEncoderTask) {
    g_blendTask = register_encoder_task_type(EncoderTaskDescriptor{.label = "PBR Cube Blend", .callback = encode_blend});
    if (g_blendTask == InvalidEncoderTask) {
      Log.warn("could not register the cube blend task");
      return false;
    }
  }
  // The sources' creates and the target's last reads must be in before the target changes.
  gx::fifo::drain();
  BlendJob job;
  job.count = count;
  uint32_t size = 0;
  uint32_t mipCount = 0;
  {
    std::lock_guard lock(g_cubeMutex);
    for (uint32_t i = 0; i < count; ++i) {
      const auto found = g_roomCubes.find(src[i]);
      if (found == g_roomCubes.end() || found->second.size == 0 || src[i] == dst) {
        return false;
      }
      job.sources[i] = found->second.view;
      job.weights[i] = weights[i];
      // The first (heaviest) source sets the size; mips stop where any source's do.
      if (i == 0) {
        size = found->second.size;
        mipCount = found->second.mipCount;
      } else {
        mipCount = std::min(mipCount, found->second.mipCount);
      }
    }
    mipCount = std::max(mipCount, 1u);
    auto& target = g_roomCubes[dst];
    if (!target.texture || target.size != size || target.mipCount != mipCount || target.faces.empty()) {
      wgpu::TextureBindingViewDimension cubeBinding;
      cubeBinding.textureBindingViewDimension = wgpu::TextureViewDimension::Cube;
      const wgpu::TextureDescriptor textureDescriptor{
          .nextInChain = &cubeBinding,
          .label = "PBR blended cube",
          .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::RenderAttachment,
          .dimension = wgpu::TextureDimension::e2D,
          .size = {size, size, FaceCount},
          .format = wgpu::TextureFormat::RGBA16Float,
          .mipLevelCount = mipCount,
          .sampleCount = 1,
      };
      RoomCube cube;
      cube.size = size;
      cube.mipCount = mipCount;
      cube.texture = g_device.CreateTexture(&textureDescriptor);
      cube.live = Live(1, cube_bytes(size, mipCount, 8));
      const wgpu::TextureViewDescriptor cubeDescriptor{
          .label = "PBR blended cube view",
          .format = wgpu::TextureFormat::RGBA16Float,
          .dimension = wgpu::TextureViewDimension::Cube,
          .baseMipLevel = 0,
          .mipLevelCount = mipCount,
          .baseArrayLayer = 0,
          .arrayLayerCount = FaceCount,
      };
      cube.view = cube.texture.CreateView(&cubeDescriptor);
      for (uint32_t f = 0; f < FaceCount; ++f) {
        for (uint32_t m = 0; m < mipCount; ++m) {
          const wgpu::TextureViewDescriptor faceDescriptor{
              .label = "PBR blended cube face",
              .format = wgpu::TextureFormat::RGBA16Float,
              .dimension = wgpu::TextureViewDimension::e2D,
              .baseMipLevel = m,
              .mipLevelCount = 1,
              .baseArrayLayer = f,
              .arrayLayerCount = 1,
          };
          cube.faces.push_back(cube.texture.CreateView(&faceDescriptor));
        }
      }
      target = std::move(cube);
    }
    job.targets = target.faces;
    job.mipCount = mipCount;
  }
  uint64_t key = 0;
  {
    std::lock_guard lock(g_blendMutex);
    key = g_nextBlendJob++;
    g_blendJobs.emplace(key, std::move(job));
  }
  if (!push_encoder_task(g_blendTask, &key, sizeof(key))) {
    std::lock_guard lock(g_blendMutex);
    g_blendJobs.erase(key);
    return false;
  }
  return true;
}

void create_volume(uint32_t id, uint32_t sizeX, uint32_t sizeY, uint32_t sizeZ, const uint8_t* texels,
                   size_t length) {
  const size_t points = size_t(sizeX) * sizeY * sizeZ;
  if (id == 0 || points == 0 || length < points * 28) {
    return;
  }
  Volume volume;
  // volume.live accumulates below; added to the totals once the volume is whole.
  const uint8_t* in = texels;
  for (uint32_t i = 0; i < VolumeTextures; ++i) {
    const bool half = i < 2;
    const uint32_t texelSize = half ? 8 : 4;
    // One zero texel around the grid: with clamp-to-edge it makes the sampler read what
    // Remastered's CLAMP_TO_BORDER (black border) does, a fade to 0 over the half texel
    // outside and 0 beyond. The shader maps uvw into the padded extent.
    const uint32_t padX = sizeX + 2, padY = sizeY + 2, padZ = sizeZ + 2;
    const wgpu::TextureDescriptor textureDescriptor{
        .label = "PBR ambient volume",
        .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst,
        .dimension = wgpu::TextureDimension::e3D,
        .size = {padX, padY, padZ},
        .format = half ? wgpu::TextureFormat::RGBA16Float : wgpu::TextureFormat::RGBA8Unorm,
        .mipLevelCount = 1,
        .sampleCount = 1,
    };
    volume.textures[i] = g_device.CreateTexture(&textureDescriptor);
    volume.live.count += 1;
    volume.live.bytes += uint64_t(padX) * padY * padZ * texelSize;
    // A slice at a time, through the frame's uploads (see create_cube).
    std::vector<uint8_t> slice(size_t(padX) * padY * texelSize, 0);
    for (uint32_t z = 0; z < padZ; ++z) {
      std::fill(slice.begin(), slice.end(), uint8_t(0));
      if (z >= 1 && z <= sizeZ) {
        for (uint32_t y = 0; y < sizeY; ++y) {
          std::memcpy(slice.data() + (size_t(y + 1) * padX + 1) * texelSize, in + size_t(y) * sizeX * texelSize,
                      size_t(sizeX) * texelSize);
        }
        in += size_t(sizeX) * sizeY * texelSize;
      }
      const wgpu::TexelCopyTextureInfo dst{
          .texture = volume.textures[i],
          .mipLevel = 0,
          .origin = {0, 0, z},
      };
      queue_texture_upload_data(slice.data(), padX * texelSize, padY, dst, wgpu::Extent3D{padX, padY, 1});
    }
    volume.views[i] = volume.textures[i].CreateView();
  }
  volume.live.add(1);
  g_volumes[id] = std::move(volume);
}

void destroy_volume(uint32_t id) { g_volumes.erase(id); }

bool has_volume(uint32_t id) { return g_volumes.find(id) != g_volumes.end(); }

bool lightmap_available() { return webgpu::g_lightmapBinding; }

bool lightmap_bc_supported() { return webgpu::g_bcTexturesSupported; }

void create_lightmap(uint32_t id, uint32_t width, uint32_t height, uint32_t layers, uint32_t format,
                     const uint8_t* texels, size_t length) {
  // GXPBRLightmapFormat: 0 BC6H signed float, 1 BC6H unsigned float, 2 RGBA16Float.
  const bool bc = format == 0 || format == 1;
  if (id == 0 || width == 0 || height == 0 || layers == 0 || format > 2 || !webgpu::g_lightmapBinding ||
      (bc && (!webgpu::g_bcTexturesSupported || width % 4 != 0 || height % 4 != 0))) {
    return;
  }
  const uint32_t rows = bc ? height / 4 : height;
  const uint32_t bytesPerRow = bc ? width / 4 * 16 : width * 8;
  const size_t layerBytes = size_t(bytesPerRow) * rows;
  if (length < layerBytes * layers) {
    return;
  }
  const wgpu::TextureDescriptor textureDescriptor{
      .label = "PBR baked lightmap",
      .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst,
      .dimension = wgpu::TextureDimension::e2D,
      .size = {width, height, layers},
      .format = !bc                ? wgpu::TextureFormat::RGBA16Float
                : format == 0      ? wgpu::TextureFormat::BC6HRGBFloat
                                   : wgpu::TextureFormat::BC6HRGBUfloat,
      .mipLevelCount = 1,
      .sampleCount = 1,
  };
  Lightmap lightmap;
  lightmap.texture = g_device.CreateTexture(&textureDescriptor);
  lightmap.live = Live(1, layerBytes * layers);
  // A layer at a time, through the frame's uploads (see create_volume).
  for (uint32_t layer = 0; layer < layers; ++layer) {
    const wgpu::TexelCopyTextureInfo dst{
        .texture = lightmap.texture,
        .mipLevel = 0,
        .origin = {0, 0, layer},
    };
    queue_texture_upload_data(texels + layerBytes * layer, bytesPerRow, rows, dst, wgpu::Extent3D{width, height, 1});
  }
  const wgpu::TextureViewDescriptor viewDescriptor{
      .label = "PBR baked lightmap view",
      .format = textureDescriptor.format,
      .dimension = wgpu::TextureViewDimension::e2DArray,
      .baseMipLevel = 0,
      .mipLevelCount = 1,
      .baseArrayLayer = 0,
      .arrayLayerCount = layers,
  };
  lightmap.view = lightmap.texture.CreateView(&viewDescriptor);
  g_lightmaps[id] = std::move(lightmap);
}

void destroy_lightmap(uint32_t id) { g_lightmaps.erase(id); }

bool has_lightmap(uint32_t id) { return g_lightmaps.find(id) != g_lightmaps.end(); }

const wgpu::TextureView& lightmap_view(uint32_t id) {
  const auto found = g_lightmaps.find(id);
  if (found != g_lightmaps.end()) {
    return found->second.view;
  }
  if (!g_emptyLightmap) {
    constexpr wgpu::TextureDescriptor descriptor{
        .label = "Empty PBR baked lightmap",
        .usage = wgpu::TextureUsage::TextureBinding,
        .dimension = wgpu::TextureDimension::e2D,
        .size = {1, 1, 4},
        .format = wgpu::TextureFormat::RGBA16Float,
    };
    g_emptyLightmap = g_device.CreateTexture(&descriptor);
    g_emptyLightmapLive = Live(1, 4 * 8);
    const wgpu::TextureViewDescriptor viewDescriptor{
        .label = "Empty PBR baked lightmap view",
        .format = wgpu::TextureFormat::RGBA16Float,
        .dimension = wgpu::TextureViewDimension::e2DArray,
        .mipLevelCount = 1,
        .arrayLayerCount = 4,
    };
    g_emptyLightmapView = g_emptyLightmap.CreateView(&viewDescriptor);
  }
  return g_emptyLightmapView;
}

bool set_brdf_lut(const uint8_t* texels, size_t length) {
  if (length != BrdfLutBytes) {
    g_brdfLut = {};
    g_brdfLutView = {};
    g_brdfLive = {};
    return false;
  }
  const wgpu::TextureDescriptor descriptor{
      .label = "PBR BRDF table",
      .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst,
      .size = {16, 8, 1},
      .format = wgpu::TextureFormat::RG8Unorm,
  };
  g_brdfLut = g_device.CreateTexture(&descriptor);
  g_brdfLive = Live(1, 16 * 8 * 2);
  const wgpu::TexelCopyTextureInfo dst{.texture = g_brdfLut};
  queue_texture_upload_data(texels, 16 * 2, 8, dst, wgpu::Extent3D{16, 8, 1});
  g_brdfLutView = g_brdfLut.CreateView();
  return true;
}

const wgpu::TextureView& brdf_lut_view() {
  if (g_brdfLutView) {
    return g_brdfLutView;
  }
  if (!g_emptyBrdfLut) {
    constexpr wgpu::TextureDescriptor descriptor{
        .label = "Empty PBR BRDF table",
        .usage = wgpu::TextureUsage::TextureBinding,
        .size = {1, 1, 1},
        .format = wgpu::TextureFormat::RG8Unorm,
    };
    g_emptyBrdfLut = g_device.CreateTexture(&descriptor);
    g_emptyBrdfLive = Live(1, 2);
    g_emptyBrdfLutView = g_emptyBrdfLut.CreateView();
  }
  return g_emptyBrdfLutView;
}

const wgpu::TextureView& volume_view(uint32_t id, uint32_t index) {
  const auto found = g_volumes.find(id);
  if (found != g_volumes.end()) {
    return found->second.views[index % VolumeTextures];
  }
  if (!g_emptyVolume) {
    constexpr wgpu::TextureDescriptor descriptor{
        .label = "Empty PBR ambient volume",
        .usage = wgpu::TextureUsage::TextureBinding,
        .dimension = wgpu::TextureDimension::e3D,
        .size = {1, 1, 1},
        .format = wgpu::TextureFormat::RGBA8Unorm,
    };
    g_emptyVolume = g_device.CreateTexture(&descriptor);
    g_emptyVolumeLive = Live(1, 4);
    g_emptyVolumeView = g_emptyVolume.CreateView();
  }
  return g_emptyVolumeView;
}
} // namespace aurora::gfx::probe
