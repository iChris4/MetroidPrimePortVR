#include <aurora/envfx.hpp>

#include "../gx/fifo.hpp"
#include "../gx/gx.hpp"
#include "../logging.hpp"
#include "../webgpu/gpu.hpp"
#include "recording.hpp"
#include "resource_cache.hpp"
#include "texture_sites.hpp"
#include "volfog.hpp"

#include <aurora/gfx.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

// Remastered's EnvironmentFX (see aurora/envfx.hpp), from its decompiled shaders
// (MaterialArchive.arc blobs 0011154..0013690; kb topic/rain-snow-splash-render.md):
//   VS  pos = v0.xyz / 256 + corner.x * params[1] + corner.y * params[2]; pos.xz += grid[cell].xy;
//       u = grid[cell].z * pos.y + grid[cell].w; fog perm: the froxel at the vertex (in-scatter x the
//       injector's block weight, transmittance).
//   PS  rain, underwater: rgb = flake.rgb * params[0].y, a = gradient(u).x * flake.a * params[0].x
//       snow:             rgb = gradient(u).x * params[0].y * flake.rgb, a = gradient(u).w * params[0].x
//       fog perm: rgb = rgb * T + in-scatter.
// The quad is Remastered's particle unit quad (CGraphicsUtilCache::BuildParticleInstancingBuffers
// slot 0x288): corners in [0,1], used as uv too. Samplers are not traced (inferred: the gradient
// clamped and linear, the flakes repeating, trilinear).
//
// Exposure: Remastered's scene holds unexposed light and its tonemap multiplies by the exposure;
// the gain params[0].y is intensity x GetInverseExposure, so the flakes reach the curve at
// flake x intensity whatever the exposure. The port's HDR scene holds exposed light (hdr_tone.hpp,
// the curve takes it as is), so the same result is the colour written at exposure 1: the caller's
// gain is the intensity alone and nothing here multiplies by the room's exposure. The froxel
// in-scatter is unexposed light (as water.cpp reads it) and is multiplied by the fog's exposure.
namespace aurora::gfx::envfx {
namespace {
Module Log("aurora::gfx::envfx");

constexpr uint32_t GridCells = 64;

struct Uniform {
  float proj[16];
  float mv[12];
  float params[3][4];
  float fog[4];  // near, range, exposure, on
  float misc[4]; // in-scatter weight, mode
  float grid[GridCells][4];
};
static_assert(sizeof(Uniform) == 1216);

enum Binding : uint32_t { BUniform, BGradient, BGradSamp, BFlake, BFlakeSamp, BFroxels, BFogSamp, BCount };

struct Payload {
  uint32_t mode;
  uint32_t compare; // wgpu::CompareFunction
  uint32_t count;
  Range instances;
  Range uniform;
};
static_assert(sizeof(Payload) <= InlineDrawPayloadSize);

// A texture's mip chain on the CPU, each mip layer-major RGBA8.
struct CpuTexture {
  uint32_t width = 0, height = 0, layers = 0;
  bool srgb = false;
  std::vector<std::vector<uint8_t>> mips;
};

struct State {
  std::mutex mutex; // cpu, generation
  CpuTexture cpu[4]; // gradient, then the flakes by Mode
  uint32_t generation = 0;
  DrawTypeId drawType = InvalidDrawType;
  // Render thread only
  uint32_t gpuGeneration = 0;
  wgpu::Texture texture[4];
  wgpu::TextureView view[4];
  wgpu::BindGroupLayout bindLayout;
  wgpu::PipelineLayout pipelineLayout;
  wgpu::ShaderModule module;
  std::map<std::array<uint64_t, 4>, wgpu::RenderPipeline> pipelines;
  std::map<std::array<const void*, 3>, wgpu::BindGroup> groups;
};
State g_state;

const char* ShaderSource = R"(
struct U {
  proj: mat4x4f,
  mv: mat3x4f,
  params: array<vec4f, 3>,
  fog: vec4f,
  misc: vec4f,
  grid: array<vec4f, 64>,
}
@group(0) @binding(0) var<uniform> u: U;
@group(0) @binding(1) var gradient: texture_2d<f32>;
@group(0) @binding(2) var gradSamp: sampler;
@group(0) @binding(3) var flake: texture_2d_array<f32>;
@group(0) @binding(4) var flakeSamp: sampler;
@group(0) @binding(5) var froxels: texture_3d<f32>;
@group(0) @binding(6) var fogSamp: sampler;

struct VOut {
  @builtin(position) pos: vec4f,
  @location(0) uv: vec4f,  // Remastered's o1: flake layer, gradient u, corner
  @location(1) fog: vec4f, // o2: the in-scatter (x the block weight) and transmittance
}

@vertex
fn vs_main(@builtin(vertex_index) vi: u32, @location(0) inst: vec4<i32>) -> VOut {
  var corners = array<vec2f, 6>(vec2f(0.0, 0.0), vec2f(1.0, 0.0), vec2f(1.0, 1.0),
                                vec2f(0.0, 0.0), vec2f(1.0, 1.0), vec2f(0.0, 1.0));
  let c = corners[vi];
  let w = u32(inst.w);
  let cell = u.grid[w & 63u];
  var p = vec3f(inst.xyz) * 0.00390625 + c.x * u.params[1].xyz + c.y * u.params[2].xyz;
  p = vec3f(p.xy + cell.xy, p.z);
  var out: VOut;
  out.uv = vec4f(f32((w >> 6u) & 63u), cell.z * p.z + cell.w, c);
  let vp = vec4f(p, 1.0) * u.mv;
  let clip = vec4f(vp, 1.0) * u.proj;
  out.pos = clip;
  out.fog = vec4f(0.0, 0.0, 0.0, 1.0);
  if (u.fog.w > 0.5) {
    let ndc = clip.xy / clip.w;
    let span = u.fog.y - u.fog.x;
    let slice = sqrt(clamp(select((-vp.z - u.fog.x) / span, select(0.0, 1.0, -vp.z > u.fog.x), span == 0.0), 0.0, 1.0));
    let f = textureSampleLevel(froxels, fogSamp, vec3f(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5, slice), 0.0);
    out.fog = vec4f(f.rgb * (u.fog.z * u.misc.x), f.a);
  }
  return out;
}

@fragment
fn fs_main(in: VOut) -> @location(0) vec4f {
  let ramp = textureSample(gradient, gradSamp, vec2f(in.uv.y, 0.0));
  let f = textureSample(flake, flakeSamp, in.uv.zw, i32(in.uv.x));
  var rgb: vec3f;
  var a: f32;
  if (u.misc.y == 1.0) {
    rgb = ramp.x * u.params[0].y * f.rgb;
    a = ramp.w * u.params[0].x;
  } else {
    rgb = f.rgb * u.params[0].y;
    a = ramp.x * f.a * u.params[0].x;
  }
  return vec4f(rgb * in.fog.w + in.fog.xyz, a);
}
)";

float srgb_to_linear(uint8_t v) {
  const float c = float(v) / 255.f;
  return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

uint8_t linear_to_srgb(float c) {
  c = std::clamp(c, 0.f, 1.f);
  const float e = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.f / 2.4f) - 0.055f;
  return uint8_t(std::lround(e * 255.f));
}

// The top mip copied, then each next one a 2x2 box of the last (an odd edge repeats its last texel),
// sRGB colour averaged in linear light, down to 1x1.
bool build_mips(const Image& img, bool srgb, bool mips, CpuTexture& out) {
  if (img.rgba == nullptr || img.width == 0 || img.height == 0 || img.layers == 0) {
    return false;
  }
  out = CpuTexture{img.width, img.height, img.layers, srgb, {}};
  out.mips.emplace_back(img.rgba, img.rgba + size_t(img.width) * img.height * img.layers * 4);
  float lut[256];
  for (int i = 0; i < 256; ++i) {
    lut[i] = srgb ? srgb_to_linear(uint8_t(i)) : float(i) / 255.f;
  }
  uint32_t w = img.width, h = img.height;
  while (mips && (w > 1 || h > 1)) {
    const uint32_t nw = std::max(w / 2, 1u), nh = std::max(h / 2, 1u);
    const std::vector<uint8_t>& src = out.mips.back();
    std::vector<uint8_t> dst(size_t(nw) * nh * img.layers * 4);
    for (uint32_t l = 0; l < img.layers; ++l) {
      const uint8_t* s = src.data() + size_t(l) * w * h * 4;
      uint8_t* d = dst.data() + size_t(l) * nw * nh * 4;
      for (uint32_t y = 0; y < nh; ++y) {
        for (uint32_t x = 0; x < nw; ++x) {
          const uint32_t x0 = std::min(2 * x, w - 1), x1 = std::min(2 * x + 1, w - 1);
          const uint32_t y0 = std::min(2 * y, h - 1), y1 = std::min(2 * y + 1, h - 1);
          const uint8_t* t[4] = {s + (size_t(y0) * w + x0) * 4, s + (size_t(y0) * w + x1) * 4,
                                 s + (size_t(y1) * w + x0) * 4, s + (size_t(y1) * w + x1) * 4};
          for (int ch = 0; ch < 4; ++ch) {
            if (ch < 3) {
              const float v = 0.25f * (lut[t[0][ch]] + lut[t[1][ch]] + lut[t[2][ch]] + lut[t[3][ch]]);
              d[(size_t(y) * nw + x) * 4 + ch] = srgb ? linear_to_srgb(v) : uint8_t(std::lround(v * 255.f));
            } else {
              d[(size_t(y) * nw + x) * 4 + ch] = uint8_t((t[0][ch] + t[1][ch] + t[2][ch] + t[3][ch] + 2) / 4);
            }
          }
        }
      }
    }
    out.mips.push_back(std::move(dst));
    w = nw;
    h = nh;
  }
  return true;
}

void ensure_static(const wgpu::Device& device) {
  if (g_state.pipelineLayout) {
    return;
  }
  using SS = wgpu::ShaderStage;
  const auto tex = [](uint32_t binding, SS vis, wgpu::TextureViewDimension dim) {
    return wgpu::BindGroupLayoutEntry{
        .binding = binding,
        .visibility = vis,
        .texture = {.sampleType = wgpu::TextureSampleType::Float, .viewDimension = dim}};
  };
  const auto samp = [](uint32_t binding, SS vis) {
    return wgpu::BindGroupLayoutEntry{
        .binding = binding, .visibility = vis, .sampler = {.type = wgpu::SamplerBindingType::Filtering}};
  };
  const wgpu::BindGroupLayoutEntry entries[BCount] = {
      {.binding = BUniform,
       .visibility = SS::Vertex | SS::Fragment,
       .buffer = {.type = wgpu::BufferBindingType::Uniform,
                  .hasDynamicOffset = true,
                  .minBindingSize = sizeof(Uniform)}},
      tex(BGradient, SS::Fragment, wgpu::TextureViewDimension::e2D),
      samp(BGradSamp, SS::Fragment),
      tex(BFlake, SS::Fragment, wgpu::TextureViewDimension::e2DArray),
      samp(BFlakeSamp, SS::Fragment),
      tex(BFroxels, SS::Vertex, wgpu::TextureViewDimension::e3D),
      samp(BFogSamp, SS::Vertex),
  };
  const wgpu::BindGroupLayoutDescriptor layoutDescriptor{
      .label = "EnvFX Bind Group Layout",
      .entryCount = BCount,
      .entries = entries,
  };
  g_state.bindLayout = device.CreateBindGroupLayout(&layoutDescriptor);
  const wgpu::PipelineLayoutDescriptor pipelineLayoutDescriptor{
      .label = "EnvFX Pipeline Layout",
      .bindGroupLayoutCount = 1,
      .bindGroupLayouts = &g_state.bindLayout,
  };
  g_state.pipelineLayout = device.CreatePipelineLayout(&pipelineLayoutDescriptor);
  wgpu::ShaderSourceWGSL wgsl{};
  wgsl.code = ShaderSource;
  const wgpu::ShaderModuleDescriptor moduleDescriptor{.nextInChain = &wgsl, .label = "EnvFX Module"};
  g_state.module = device.CreateShaderModule(&moduleDescriptor);
}

// Remastered's EBlendMode per effect: rain 6 (colour and alpha SrcA,One), snow 1 (One,One; alpha
// Zero,One), underwater 2 (SrcA,InvSrcA). The scene's alpha is left alone (port choice: the HDR
// scene's alpha is not light).
wgpu::BlendState mode_blend(Mode mode) {
  using F = wgpu::BlendFactor;
  wgpu::BlendComponent color{.operation = wgpu::BlendOperation::Add};
  switch (mode) {
  case Mode::Rain:
    color.srcFactor = F::SrcAlpha;
    color.dstFactor = F::One;
    break;
  case Mode::Snow:
    color.srcFactor = F::One;
    color.dstFactor = F::One;
    break;
  case Mode::Underwater:
    color.srcFactor = F::SrcAlpha;
    color.dstFactor = F::OneMinusSrcAlpha;
    break;
  }
  return wgpu::BlendState{.color = color, .alpha = {.operation = wgpu::BlendOperation::Add,
                                                    .srcFactor = F::Zero,
                                                    .dstFactor = F::One}};
}

wgpu::RenderPipeline make_pipeline(const DrawContext& ctx, Mode mode, uint32_t compare) {
  ensure_static(ctx.device);
  const RenderTargetLayout& layout = ctx.layout;
  static constexpr wgpu::VertexAttribute attribute{
      .format = wgpu::VertexFormat::Sint16x4, .offset = 0, .shaderLocation = 0};
  const wgpu::VertexBufferLayout vertexLayout{
      .stepMode = wgpu::VertexStepMode::Instance,
      .arrayStride = sizeof(Instance),
      .attributeCount = 1,
      .attributes = &attribute,
  };
  const wgpu::BlendState blend = mode_blend(mode);
  std::array<wgpu::ColorTargetState, MaxColorAttachments> targets{};
  for (uint32_t i = 0; i < layout.colorAttachmentCount; ++i) {
    const bool scene = i == SceneColorAttachmentIndex;
    targets[i] = {
        .format = layout.colorAttachments[i].format,
        .blend = scene ? &blend : nullptr,
        .writeMask = scene ? wgpu::ColorWriteMask::Red | wgpu::ColorWriteMask::Green | wgpu::ColorWriteMask::Blue
                           : wgpu::ColorWriteMask::None,
    };
  }
  const wgpu::FragmentState fragment{
      .module = g_state.module,
      .entryPoint = "fs_main",
      .targetCount = layout.colorAttachmentCount,
      .targets = targets.data(),
  };
  const wgpu::DepthStencilState depth{
      .format = layout.depthStencilFormat,
      .depthWriteEnabled = false,
      .depthCompare = static_cast<wgpu::CompareFunction>(compare),
  };
  const wgpu::RenderPipelineDescriptor descriptor{
      .label = "EnvFX Pipeline",
      .layout = g_state.pipelineLayout,
      .vertex = {.module = g_state.module, .entryPoint = "vs_main", .bufferCount = 1, .buffers = &vertexLayout},
      .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList, .cullMode = wgpu::CullMode::None},
      .depthStencil = layout.depthStencilFormat != wgpu::TextureFormat::Undefined ? &depth : nullptr,
      .multisample = {.count = layout.sampleCount, .mask = UINT32_MAX},
      .fragment = &fragment,
  };
  return ctx.device.CreateRenderPipeline(&descriptor);
}

// Render thread: (re)makes the GPU textures from the last upload. False if there is none.
bool ensure_textures(const DrawContext& ctx) {
  const std::lock_guard lock{g_state.mutex};
  if (g_state.generation == 0) {
    return false;
  }
  if (g_state.gpuGeneration == g_state.generation) {
    return true;
  }
  g_state.groups.clear();
  for (uint32_t t = 0; t < 4; ++t) {
    const CpuTexture& c = g_state.cpu[t];
    const wgpu::TextureDescriptor descriptor{
        .label = t == 0 ? "EnvFX Gradient" : "EnvFX Flake",
        .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst,
        .size = {c.width, c.height, c.layers},
        .format = c.srgb ? wgpu::TextureFormat::RGBA8UnormSrgb : wgpu::TextureFormat::RGBA8Unorm,
        .mipLevelCount = uint32_t(c.mips.size()),
    };
    AURORA_NOTE_TEXTURE(&descriptor);
    g_state.texture[t] = ctx.device.CreateTexture(&descriptor);
    uint32_t w = c.width, h = c.height;
    for (uint32_t m = 0; m < c.mips.size(); ++m) {
      const wgpu::TexelCopyTextureInfo dst{.texture = g_state.texture[t], .mipLevel = m};
      const wgpu::TexelCopyBufferLayout bufferLayout{.bytesPerRow = w * 4, .rowsPerImage = h};
      const wgpu::Extent3D size{w, h, c.layers};
      ctx.queue.WriteTexture(&dst, c.mips[m].data(), c.mips[m].size(), &bufferLayout, &size);
      w = std::max(w / 2, 1u);
      h = std::max(h / 2, 1u);
    }
    const wgpu::TextureViewDescriptor viewDescriptor{
        .dimension = t == 0 ? wgpu::TextureViewDimension::e2D : wgpu::TextureViewDimension::e2DArray,
    };
    g_state.view[t] = g_state.texture[t].CreateView(&viewDescriptor);
  }
  g_state.gpuGeneration = g_state.generation;
  return true;
}

void draw_payload(const DrawContext& ctx, const wgpu::RenderPassEncoder& pass, const void* data, size_t size,
                  void*) {
  if (size != sizeof(Payload)) {
    return;
  }
  Payload p;
  std::memcpy(&p, data, sizeof(p));
  // Fail closed: the colour is the HDR scene's light, never an LDR target's.
  if (ctx.layout.colorAttachmentCount <= SceneColorAttachmentIndex ||
      ctx.layout.colorAttachments[SceneColorAttachmentIndex].format != webgpu::SceneHdrFormat || p.mode > 2) {
    return;
  }
  ensure_static(ctx.device);
  if (!ensure_textures(ctx)) {
    return;
  }
  const wgpu::TextureView& gradient = g_state.view[0];
  const wgpu::TextureView& flake = g_state.view[1 + p.mode];
  const wgpu::TextureView& froxels = volfog::froxel_view();
  if (!gradient || !flake || !froxels) {
    return;
  }
  const std::array<uint64_t, 4> pipelineKey{p.mode, p.compare, ctx.layout.key, ctx.layout.sampleCount};
  auto pipeline = g_state.pipelines.find(pipelineKey);
  if (pipeline == g_state.pipelines.end()) {
    wgpu::RenderPipeline created = make_pipeline(ctx, Mode(p.mode), p.compare);
    if (!created) {
      return;
    }
    pipeline = g_state.pipelines.emplace(pipelineKey, std::move(created)).first;
  }
  const std::array<const void*, 3> groupKey{flake.Get(), froxels.Get(), ctx.uniformBuffer.Get()};
  auto group = g_state.groups.find(groupKey);
  if (group == g_state.groups.end()) {
    if (g_state.groups.size() > 64) {
      g_state.groups.clear();
    }
    const wgpu::Sampler gradSamp = sampler_ref(wgpu::SamplerDescriptor{
        .addressModeU = wgpu::AddressMode::ClampToEdge,
        .addressModeV = wgpu::AddressMode::ClampToEdge,
        .magFilter = wgpu::FilterMode::Linear,
        .minFilter = wgpu::FilterMode::Linear,
        .mipmapFilter = wgpu::MipmapFilterMode::Nearest,
        .maxAnisotropy = 1,
    });
    const wgpu::Sampler flakeSamp = sampler_ref(wgpu::SamplerDescriptor{
        .addressModeU = wgpu::AddressMode::Repeat,
        .addressModeV = wgpu::AddressMode::Repeat,
        .magFilter = wgpu::FilterMode::Linear,
        .minFilter = wgpu::FilterMode::Linear,
        .mipmapFilter = wgpu::MipmapFilterMode::Linear,
        .maxAnisotropy = 1,
    });
    const wgpu::BindGroupEntry entries[BCount] = {
        {.binding = BUniform, .buffer = ctx.uniformBuffer, .size = sizeof(Uniform)},
        {.binding = BGradient, .textureView = gradient},
        {.binding = BGradSamp, .sampler = gradSamp},
        {.binding = BFlake, .textureView = flake},
        {.binding = BFlakeSamp, .sampler = flakeSamp},
        {.binding = BFroxels, .textureView = froxels},
        {.binding = BFogSamp, .sampler = volfog::sampler()},
    };
    const wgpu::BindGroupDescriptor descriptor{
        .label = "EnvFX Bind Group",
        .layout = g_state.bindLayout,
        .entryCount = BCount,
        .entries = entries,
    };
    group = g_state.groups.emplace(groupKey, ctx.device.CreateBindGroup(&descriptor)).first;
  }
  pass.SetPipeline(pipeline->second);
  pass.SetBindGroup(0, group->second, 1, &p.uniform.offset);
  pass.SetVertexBuffer(0, ctx.vertexBuffer, p.instances.offset, p.instances.size);
  pass.Draw(6, p.count);
}

bool ensure_registered() {
  if (g_state.drawType == InvalidDrawType) {
    g_state.drawType = register_draw_type(DrawTypeDescriptor{.label = "EnvFX", .draw = draw_payload});
  }
  return g_state.drawType != InvalidDrawType;
}
} // namespace

bool upload(const Image& gradient, const Image (&flake)[3]) {
  CpuTexture cpu[4];
  if (!build_mips(gradient, false, false, cpu[0]) || gradient.layers != 1) {
    Log.warn("EnvFX: malformed gradient");
    return false;
  }
  for (uint32_t i = 0; i < 3; ++i) {
    if (!build_mips(flake[i], true, true, cpu[1 + i])) {
      Log.warn("EnvFX: malformed flake texture {}", i);
      return false;
    }
  }
  const std::lock_guard lock{g_state.mutex};
  for (uint32_t i = 0; i < 4; ++i) {
    g_state.cpu[i] = std::move(cpu[i]);
  }
  ++g_state.generation;
  return true;
}

bool uploaded() {
  const std::lock_guard lock{g_state.mutex};
  return g_state.generation != 0;
}

bool draw(const DrawDesc& desc, const Instance* instances, uint32_t count) {
  if (!uploaded() || !ensure_registered()) {
    return false;
  }
  // The GX state the draw sees is the one after everything recorded so far (vfx.cpp draw_prims).
  gx::fifo::drain();
  if (!scene_hdr_pass_active()) {
    return false;
  }
  if (instances == nullptr || count == 0) {
    return true;
  }
  const auto& gx = gx::g_gxState;
  Uniform u{};
  Mat4x4<float> proj = gx.proj;
  if (gx::UseReversedZ) {
    proj.m2 = proj.m2 * Vec4<float>{-1.f, -1.f, -1.f, -1.f};
  } else {
    proj.m2 = proj.m2 + proj.m3;
  }
  static_assert(sizeof(proj) == sizeof(u.proj));
  std::memcpy(u.proj, &proj, sizeof(u.proj));
  static_assert(sizeof(gx.pnMtx[0].pos) == sizeof(u.mv));
  std::memcpy(u.mv, &gx.pnMtx[gx.currentPnMtx].pos, sizeof(u.mv));
  std::memcpy(u.params, desc.params, sizeof(u.params));
  std::memcpy(u.grid, desc.grid, sizeof(u.grid));
  if (desc.fog && gx.volFog) {
    // The froxels the fog pass filled this frame (GX_AURORA_PORT_VOLUMETRIC_FOG).
    u.fog[0] = gx.volFogParams.x();
    u.fog[1] = gx.volFogParams.y();
    u.fog[2] = gx.volFogParams.z();
    u.fog[3] = 1.f;
  }
  u.misc[0] = desc.mode == Mode::Underwater ? 1.f : 0.f;
  u.misc[1] = float(desc.mode);

  Payload p{};
  p.mode = uint32_t(desc.mode);
  p.compare = uint32_t(gx::UseReversedZ ? wgpu::CompareFunction::GreaterEqual : wgpu::CompareFunction::LessEqual);
  p.count = count;
  p.instances = push_verts(reinterpret_cast<const uint8_t*>(instances), size_t(count) * sizeof(Instance), 4);
  p.uniform = push_uniform(reinterpret_cast<const uint8_t*>(&u), sizeof(u));
  if (overflowed(p.instances) || overflowed(p.uniform) || p.instances.size == 0 || p.uniform.size == 0) {
    return true;
  }
  push_custom_draw(g_state.drawType, &p, sizeof(p));
  return true;
}

void shutdown() {
  g_state.pipelines.clear();
  g_state.groups.clear();
  for (uint32_t i = 0; i < 4; ++i) {
    g_state.view[i] = {};
    g_state.texture[i] = {};
  }
  g_state.gpuGeneration = 0;
  g_state.module = {};
  g_state.bindLayout = {};
  g_state.pipelineLayout = {};
  const auto draw = g_state.drawType;
  g_state.drawType = InvalidDrawType;
  if (draw != InvalidDrawType) {
    unregister_draw_type(draw);
  }
}
} // namespace aurora::gfx::envfx
