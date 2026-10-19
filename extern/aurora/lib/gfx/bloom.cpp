#include "bloom.hpp"

#include "../gx/fifo.hpp"
#include "../logging.hpp"
#include "../stereo_host.hpp"
#include "../webgpu/gpu.hpp"
#include "../webgpu/gpu_prof.hpp"
#include "frame.hpp"
#include "hdr_tone.hpp"
#include "readback_slots.hpp"
#include "recording.hpp"
#include "stereo_shadow.hpp"

#include <aurora/gfx.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "texture_sites.hpp"

// Remastered's bloom, as its CRenderPass_Bloom and shaders do it:
//  - a bright pass over the exposed colour X, at a quarter of the frame's size, on four bilinear
//    taps a source texel out diagonally (each tap filtered as light, then bright-passed, / 4):
//    X * min(max(L - threshold, 0), 8) / max(L, 0.001) times tint 4, L being X's luminance;
//  - four downsamples to 1/64 (the centre four times and four corners a texel out, / 8);
//  - four upsamples back, each added to the next finer level, the coarsest first with
//    tint 0 (four edge taps a texel out and four corners half one, the corners doubled, / 12);
//  - the result b added to the frame as b / (1 + b).
// The EFB holds the tone-mapped colour sRGB encoded, so the bright pass undoes the room's
// tone curve to get X back (Remastered reads its HDR frame; this one is capped where the
// curve has lost its level), and the frame is added to in linear terms.
// The colour grade comes first in the same composite, as Remastered's tonemap shader does it
// right after its tone curve: a 33^3 LUT over the tone-mapped linear colour, sampled at
// c * 32/33 + 0.5/33; two LUTs mixed while a grade fades into another. The bloom is added to
// the graded colour and clamped, as Remastered's composite runs after the tonemap pass.
// Before either, the frame's average for auto exposure: Remastered takes the smallest mip of
// its HDR frame; here a 16x16 grid of tiles, each the mean of 8x8 exposed samples, read back
// a few frames later and divided by the exposure the frame was drawn at.
// On the HDR scene (gfx::set_scene_hdr), which holds the exposed linear light itself, the same chain
// reads it as it is (HdrSource): no decode, no inverse curve, no cap; the composite tones it once into
// the display EFB, which the frame goes on drawing into.
namespace aurora::gfx::bloom {
namespace {
Module Log("aurora::gfx::bloom");
using webgpu::g_device;

constexpr uint32_t Levels = 5;
constexpr uint32_t PassCount = 1 + (Levels - 1) * 2 + 1;
constexpr uint64_t SlotSize = 256;
constexpr auto LevelFormat = wgpu::TextureFormat::RGBA16Float;
constexpr uint32_t AverageSize = 16;
constexpr auto AverageFormat = wgpu::TextureFormat::RGBA32Float;
constexpr uint32_t AverageSamples = AverageSize * 8; // the samples' grid, 8x8 to a tile
constexpr uint32_t AverageRowBytes = AverageSize * 16; // a multiple of 256, as copies need
constexpr uint64_t AverageBytes = uint64_t(AverageRowBytes) * AverageSize;
constexpr size_t ReadbackCount = 3;
// Params::bloom's bits as the task gets them: the bloom, and the composite left to the next pass.
constexpr uint32_t BloomOn = 1;
constexpr uint32_t CompositeInPass = 2;

// Each recorded post keeps its parameters, domain and (HDR) source, and its own uniform ranges, in a
// slot: two posts in one submit must not share either (queue writes all land before the submit). The
// ring outlives every frame in flight at MaxPostsPerFrame; a post beyond that in one frame is refused.
constexpr size_t RecordedSlots = 8;
constexpr size_t MaxPostsPerFrame = 2;
static_assert(MaxPostsPerFrame * detail::FrameSlotCount <= RecordedSlots, "bloom slots would be overwritten while pending");

enum class Domain : uint8_t { Ldr, Hdr };

struct Uniform {
  float texel[4];
  float tint[4]; // w: the threshold
  float tone[3][4];
  float grade[4]; // x: B's weight, y: A is a LUT, z: B is a LUT, w: bloom on
};
static_assert(sizeof(Uniform) <= SlotSize);

constexpr const char* ShaderSource = R"(
struct Params {
  texel: vec4f,
  tint: vec4f,
  tone: array<vec4f, 3>,
  grade: vec4f,
};
@group(0) @binding(0) var samp: sampler;
@group(0) @binding(1) var src: texture_2d<f32>;
@group(0) @binding(2) var<uniform> p: Params;
@group(0) @binding(3) var bloomTex: texture_2d<f32>;
@group(0) @binding(4) var lutA: texture_3d<f32>;
@group(0) @binding(5) var lutB: texture_3d<f32>;

struct VertexOutput {
  @builtin(position) pos: vec4f,
  @location(0) uv: vec2f,
};

@vertex
fn vs_main(@builtin(vertex_index) i: u32) -> VertexOutput {
  var corners = array<vec2f, 3>(vec2f(-1.0, -1.0), vec2f(3.0, -1.0), vec2f(-1.0, 3.0));
  var out: VertexOutput;
  let c = corners[i];
  out.pos = vec4f(c, 0.0, 1.0);
  out.uv = vec2f(c.x * 0.5 + 0.5, 0.5 - c.y * 0.5);
  return out;
}

// The tone curve, forwards (as the PBR shader draws it) for one channel.
fn tone(x: f32) -> f32 {
  if (x < p.tone[1].z) {
    return ((p.tone[0].x * x + p.tone[0].y) * x + p.tone[0].z) * x;
  }
  if (x < p.tone[1].w) {
    return p.tone[1].x * x + p.tone[1].y;
  }
  let st = max(p.tone[2].y * x + p.tone[2].z, 0.0);
  return p.tone[2].x * st / (1.0 + st) + p.tone[2].w;
}

// And back: the exposed level that drew as y (linear).
fn untone(y: f32) -> f32 {
  let mid = p.tone[1].z;
  let lineStart = p.tone[1].x * mid + p.tone[1].y;
  if (y < lineStart) {
    // The toe rises through the origin to the line's start; Newton from a straight guess.
    var x = y / max(lineStart, 1e-4) * mid;
    for (var n = 0; n < 4; n++) {
      let slope = (3.0 * p.tone[0].x * x + 2.0 * p.tone[0].y) * x + p.tone[0].z;
      x = clamp(x - (tone(x) - y) / max(slope, 1e-4), 0.0, mid);
    }
    return x;
  }
  let top = p.tone[2].w;
  if (y < top || p.tone[2].y <= 0.0) {
    return (y - p.tone[1].y) / p.tone[1].x;
  }
  let u = min((y - top) / max(p.tone[2].x, 1e-4), 0.999);
  return u / (1.0 - u) / p.tone[2].y + p.tone[1].w;
}

// The EFB holds colour as Remastered's sRGB swapchain does: the exact piecewise sRGB curve.
fn srgb_enc(c: vec3f) -> vec3f {
  let l = clamp(c, vec3f(0.0), vec3f(1.0));
  return select(1.055 * pow(l, vec3f(1.0 / 2.4)) - 0.055, 12.92 * l, l <= vec3f(0.0031308));
}

fn srgb_dec(c: vec3f) -> vec3f {
  let e = clamp(c, vec3f(0.0), vec3f(1.0));
  return select(pow((e + 0.055) / 1.055, vec3f(2.4)), e / 12.92, e <= vec3f(0.04045));
}

// A channel at white has lost its level: the shoulder's inverse runs off to hundreds there, which turned a
// saturated orange hull into a red flood. Cap it where the curve draws about 0.97 of white (4.0 on these rooms).
const MaxExposed = 4.0;

fn exposed(c: vec3f) -> vec3f {
  let y = srgb_dec(c);
  return min(vec3f(untone(y.r), untone(y.g), untone(y.b)), vec3f(MaxExposed));
}

// One bilinear tap of Remastered's HDR frame at uv: the four texels it blends, each exposed (the
// frame is filtered as light, not as the drawn colour), then the bright pass over the blend.
// Remastered's 000d768 runs the bright pass on each filtered tap: X * min(max(s*L - t, 0), 8) /
// max(s*L, 0.001) * s, L being X's luminance. The EFB texel is already exposed (s * X), so with
// c = s * X that is c * min(max(lum(c) - t, 0), 8) / max(lum(c), 0.001).
fn bright_tap(uv: vec2f, size: vec2i, dim: f32) -> vec3f {
  let pos = uv * vec2f(size) - vec2f(0.5);
  let base = floor(pos);
  let f = pos - base;
  let last = size - vec2i(1);
  let j0 = clamp(vec2i(base), vec2i(0), last);
  let j1 = clamp(vec2i(base) + vec2i(1), vec2i(0), last);
  let d00 = textureLoad(src, vec2i(j0.x, j0.y), 0).rgb;
  let d10 = textureLoad(src, vec2i(j1.x, j0.y), 0).rgb;
  let d01 = textureLoad(src, vec2i(j0.x, j1.y), 0).rgb;
  let d11 = textureLoad(src, vec2i(j1.x, j1.y), 0).rgb;
  // A blend is no brighter than its brightest texel: all below the threshold's drawn value adds
  // nothing (most of the frame in a dark room), and a texel's drawn value is monotonic in its level.
  let hi = max(max(d00, d10), max(d01, d11));
  if (max(max(hi.r, hi.g), hi.b) < dim) {
    return vec3f(0.0);
  }
  let c = mix(mix(exposed(d00), exposed(d10), f.x), mix(exposed(d01), exposed(d11), f.x), f.y);
  let l = dot(c, vec3f(0.2126, 0.7152, 0.0722));
  return c * (min(max(l - p.tint.w, 0.0), 8.0) / max(l, 0.001));
}

// The bright pass and the reduction to 1/4 in one (Remastered's 000d768): four diagonal taps a
// source texel out, each bright-passed, / 4, times the level's tint.
@fragment
fn fs_bright(in: VertexOutput) -> @location(0) vec4f {
  let size = vec2i(textureDimensions(src));
  let t = p.texel.xy;
  let dim = srgb_enc(vec3f(tone(max(p.tint.w, 0.0)))).x * 0.999;
  var sum = bright_tap(in.uv + vec2f(-t.x, -t.y), size, dim);
  sum += bright_tap(in.uv + vec2f(t.x, -t.y), size, dim);
  sum += bright_tap(in.uv + vec2f(-t.x, t.y), size, dim);
  sum += bright_tap(in.uv + vec2f(t.x, t.y), size, dim);
  return vec4f(sum * 0.25 * p.tint.rgb, 1.0);
}

const AverageSize = 16.0;

// The average in two passes: one exposed sample per fragment on an 8x8 grid in each of the 16x16
// tiles, then each tile's 64 summed. A fragment that ran all 64 inverses in turn left the GPU idle.
@fragment
fn fs_average_samples(in: VertexOutput) -> @location(0) vec4f {
  let size = vec2i(textureDimensions(src));
  let cell = vec2i(floor(in.pos.xy));
  let tile = vec2f(cell / 8);
  let sub = vec2f(cell % 8);
  let uv = (tile + (sub + 0.5) / 8.0) / AverageSize;
  let at = min(vec2i(uv * vec2f(size)), size - vec2i(1));
  return vec4f(exposed(textureLoad(src, at, 0).rgb), 1.0);
}

@fragment
fn fs_average(in: VertexOutput) -> @location(0) vec4f {
  let base = vec2i(floor(in.pos.xy)) * 8;
  var sum = vec3f(0.0);
  for (var y = 0; y < 8; y++) {
    for (var x = 0; x < 8; x++) {
      sum += textureLoad(src, base + vec2i(x, y), 0).rgb;
    }
  }
  return vec4f(sum / 64.0, 1.0);
}

@fragment
fn fs_down(in: VertexOutput) -> @location(0) vec4f {
  let t = p.texel.xy;
  var c = textureSampleLevel(src, samp, in.uv, 0.0).rgb * 4.0;
  c += textureSampleLevel(src, samp, in.uv + vec2f(-t.x, -t.y), 0.0).rgb;
  c += textureSampleLevel(src, samp, in.uv + vec2f(t.x, -t.y), 0.0).rgb;
  c += textureSampleLevel(src, samp, in.uv + vec2f(-t.x, t.y), 0.0).rgb;
  c += textureSampleLevel(src, samp, in.uv + vec2f(t.x, t.y), 0.0).rgb;
  return vec4f(c * 0.125, 1.0);
}

@fragment
fn fs_up(in: VertexOutput) -> @location(0) vec4f {
  let t = p.texel.xy;
  let h = t * 0.5;
  var c = textureSampleLevel(src, samp, in.uv + vec2f(-t.x, 0.0), 0.0).rgb;
  c += textureSampleLevel(src, samp, in.uv + vec2f(t.x, 0.0), 0.0).rgb;
  c += textureSampleLevel(src, samp, in.uv + vec2f(0.0, -t.y), 0.0).rgb;
  c += textureSampleLevel(src, samp, in.uv + vec2f(0.0, t.y), 0.0).rgb;
  c += textureSampleLevel(src, samp, in.uv + vec2f(-h.x, -h.y), 0.0).rgb * 2.0;
  c += textureSampleLevel(src, samp, in.uv + vec2f(h.x, -h.y), 0.0).rgb * 2.0;
  c += textureSampleLevel(src, samp, in.uv + vec2f(-h.x, h.y), 0.0).rgb * 2.0;
  c += textureSampleLevel(src, samp, in.uv + vec2f(h.x, h.y), 0.0).rgb * 2.0;
  return vec4f(c / 12.0 * p.tint.rgb, 1.0);
}

@fragment
fn fs_composite(in: VertexOutput) -> @location(0) vec4f {
  let size = vec2i(textureDimensions(src));
  let f = textureLoad(src, min(vec2i(floor(in.pos.xy)), size - vec2i(1)), 0);
  let lin = srgb_dec(f.rgb);
  let at = lin * (32.0 / 33.0) + vec3f(0.5 / 33.0);
  let a = select(lin, textureSampleLevel(lutA, samp, at, 0.0).rgb, p.grade.y > 0.5);
  let g = select(lin, textureSampleLevel(lutB, samp, at, 0.0).rgb, p.grade.z > 0.5);
  var graded = mix(a, g, p.grade.x);
  // Remastered's bloom composite runs after the tonemap pass has graded the frame: the bloom's
  // b / (1 + b) added to the graded colour, clamped.
  if (p.grade.w > 0.5) {
    let b = max(textureSampleLevel(bloomTex, samp, in.uv, 0.0).rgb, vec3f(0.0));
    graded += b / (1.0 + b);
  }
  return vec4f(srgb_enc(graded), f.a);
}
)";

// The HDR scene's entry points, appended after ShaderSource, hdr::ToneWgsl and hdr::PostToneWgsl in a
// module of their own, so the LDR one keeps its bytes. src is the exposed linear scene.
constexpr std::string_view HdrSource = R"(
fn hdr_raw(c: vec3f) -> vec3f {
  return clamp(c, vec3f(0.0), vec3f(hdr_f16_max));
}

const hdr_luma = vec3f(0.2126, 0.7152, 0.0722);

// bright_tap on the scene's own texels, filtered as they are, then the bright pass over the blend.
fn bright_tap_hdr(uv: vec2f, size: vec2i) -> vec3f {
  let pos = uv * vec2f(size) - vec2f(0.5);
  let base = floor(pos);
  let f = pos - base;
  let last = size - vec2i(1);
  let j0 = clamp(vec2i(base), vec2i(0), last);
  let j1 = clamp(vec2i(base) + vec2i(1), vec2i(0), last);
  let d00 = hdr_raw(textureLoad(src, vec2i(j0.x, j0.y), 0).rgb);
  let d10 = hdr_raw(textureLoad(src, vec2i(j1.x, j0.y), 0).rgb);
  let d01 = hdr_raw(textureLoad(src, vec2i(j0.x, j1.y), 0).rgb);
  let d11 = hdr_raw(textureLoad(src, vec2i(j1.x, j1.y), 0).rgb);
  // The blend's luminance is a weighted mean of its texels': none above the threshold adds nothing.
  let hi = max(max(dot(d00, hdr_luma), dot(d10, hdr_luma)), max(dot(d01, hdr_luma), dot(d11, hdr_luma)));
  if (hi <= p.tint.w) {
    return vec3f(0.0);
  }
  let c = mix(mix(d00, d10, f.x), mix(d01, d11, f.x), f.y);
  let l = dot(c, hdr_luma);
  return c * (min(max(l - p.tint.w, 0.0), 8.0) / max(l, 0.001));
}

@fragment
fn fs_bright_hdr(in: VertexOutput) -> @location(0) vec4f {
  let size = vec2i(textureDimensions(src));
  let t = p.texel.xy;
  var sum = bright_tap_hdr(in.uv + vec2f(-t.x, -t.y), size);
  sum += bright_tap_hdr(in.uv + vec2f(t.x, -t.y), size);
  sum += bright_tap_hdr(in.uv + vec2f(-t.x, t.y), size);
  sum += bright_tap_hdr(in.uv + vec2f(t.x, t.y), size);
  return vec4f(sum * 0.25 * p.tint.rgb, 1.0);
}

@fragment
fn fs_average_samples_hdr(in: VertexOutput) -> @location(0) vec4f {
  let size = vec2i(textureDimensions(src));
  let cell = vec2i(floor(in.pos.xy));
  let tile = vec2f(cell / 8);
  let sub = vec2f(cell % 8);
  let uv = (tile + (sub + 0.5) / 8.0) / AverageSize;
  let at = min(vec2i(uv * vec2f(size)), size - vec2i(1));
  return vec4f(hdr_raw(textureLoad(src, at, 0).rgb), 1.0);
}

// The single tone of the HDR scene (hdr_display_linear: the curve, or its identity line), then the
// grade and the bloom as fs_composite has them, encoded for the display EFB.
@fragment
fn fs_composite_hdr(in: VertexOutput) -> @location(0) vec4f {
  let size = vec2i(textureDimensions(src));
  let f = textureLoad(src, min(vec2i(floor(in.pos.xy)), size - vec2i(1)), 0);
  let lin = hdr_display_linear(f.rgb, p.tone[0], p.tone[1], p.tone[2]);
  let at = lin * (32.0 / 33.0) + vec3f(0.5 / 33.0);
  let a = select(lin, textureSampleLevel(lutA, samp, at, 0.0).rgb, p.grade.y > 0.5);
  let g = select(lin, textureSampleLevel(lutB, samp, at, 0.0).rgb, p.grade.z > 0.5);
  var graded = mix(a, g, p.grade.x);
  if (p.grade.w > 0.5) {
    let b = max(textureSampleLevel(bloomTex, samp, in.uv, 0.0).rgb, vec3f(0.0));
    graded += b / (1.0 + b);
  }
  return vec4f(hdr_srgb_enc(graded), f.a);
}
)";

struct Level {
  wgpu::Texture texture;
  wgpu::TextureView view;
  uint32_t width = 0;
  uint32_t height = 0;
};

struct BindGroupKey {
  WGPUTextureView source;
  WGPUTextureView bloomSource;
  WGPUTextureView lutA;
  WGPUTextureView lutB;
  uint32_t slot;

  bool operator==(const BindGroupKey&) const = default;
};
struct BindGroupKeyHash {
  size_t operator()(const BindGroupKey& key) const noexcept {
    size_t hash = std::hash<const void*>{}(key.source);
    for (const void* view : {static_cast<const void*>(key.bloomSource), static_cast<const void*>(key.lutA),
                             static_cast<const void*>(key.lutB)}) {
      hash = hash * 31 + std::hash<const void*>{}(view);
    }
    return hash * 31 + key.slot;
  }
};

// A post as recorded (game thread), and what its encode left the in-pass composite (render worker).
struct Recorded {
  Params params{};
  Domain domain = Domain::Ldr;
  // The HDR scene the post reads, as it was when recorded.
  webgpu::TextureWithSampler color;
  webgpu::TextureWithSampler resolved;
};
struct Encoded {
  bool ready = false;
  Domain domain = Domain::Ldr;
  wgpu::TextureView source;
  wgpu::TextureView lutA;
  wgpu::TextureView lutB;
};
std::mutex g_recordMutex;
std::array<Recorded, RecordedSlots> g_recorded;
uint32_t g_nextSlot = 0;
uint64_t g_postFrame = UINT64_MAX;
size_t g_postCount = 0;
std::array<Encoded, RecordedSlots> g_encoded;

struct State {
  EncoderTaskId task = InvalidEncoderTask;
  wgpu::ShaderModule module;
  // The HDR entry points' module (HdrSource), made on the first HDR post.
  wgpu::ShaderModule hdrModule;
  wgpu::RenderPipeline brightHdr;
  wgpu::RenderPipeline averageSamplesHdr;
  wgpu::BindGroupLayout layout;
  wgpu::PipelineLayout pipelineLayout;
  wgpu::Sampler sampler;
  wgpu::Buffer uniforms;
  wgpu::RenderPipeline bright;
  wgpu::RenderPipeline down;
  wgpu::RenderPipeline up;
  wgpu::RenderPipeline composite;
  wgpu::TextureFormat compositeFormat = wgpu::TextureFormat::Undefined;
  uint32_t compositeSamples = 0;
  // The composite as the first draw of the EFB pass after the task (see record), with the
  // pipeline for that pass's layout, and what the task left it to draw with.
  DrawTypeId compositeDraw = InvalidDrawType;
  std::array<wgpu::RenderPipeline, 2> passComposite;
  std::array<uint64_t, 2> passCompositeKey{};
  // The frame as it was (LDR only), and the levels; remade when the frame's size or format changes.
  wgpu::Texture frame;
  wgpu::TextureView frameView;
  wgpu::TextureFormat frameFormat = wgpu::TextureFormat::Undefined;
  uint32_t frameWidth = 0;
  uint32_t frameHeight = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  std::array<Level, Levels> levels;
  // Grade LUTs by id, and a 1^3 stand-in bound where a pass draws none.
  std::unordered_map<uint32_t, wgpu::TextureView> luts;
  wgpu::TextureView noLut;
  // The frame's average, rendered here and copied into a readback buffer, from its samples (which
  // are unfilterable, so they bind on a layout of their own).
  wgpu::RenderPipeline averageSamples;
  wgpu::Texture averageSamplesTexture;
  wgpu::TextureView averageSamplesView;
  wgpu::BindGroupLayout averageLayout;
  wgpu::BindGroup averageGroup;
  wgpu::RenderPipeline average;
  wgpu::Texture averageTexture;
  wgpu::TextureView averageView;
  // The passes' bind groups, which only change with the targets, the frame or a LUT. A group
  // holds its views, so no view in a key can be freed and its handle reused while cached.
  std::unordered_map<BindGroupKey, wgpu::BindGroup, BindGroupKeyHash> groups;
};
State g_state;
// ensure_task runs on the producer while the FIFO worker may preflight scene entry.
std::mutex g_taskRegistrationMutex;

// Readbacks of the average. Each slot goes Available -> CopySubmitted (encoded) -> MapPending
// (after the submit) -> Available (mapped and read); readback_slots.hpp holds the transitions and
// their lock, since a map callback can arrive on any thread.
using ReadbackSlots = detail::ReadbackSlots<wgpu::Buffer, ReadbackCount>;
wgpu::Buffer make_readback() {
  const wgpu::BufferDescriptor descriptor{
      .label = "Frame Average Readback",
      .usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst,
      .size = AverageBytes,
  };
  return g_device.CreateBuffer(&descriptor);
}
ReadbackSlots g_readbacks(make_readback);
// The frame's average, in g_radianceMutex. It is written from a map callback, under the slots'
// lock, so the two are always taken in that order.
std::mutex g_radianceMutex;
float g_radiance[3] = {};
uint32_t g_radianceSerial = 0;

void complete_readback(const ReadbackSlots::Key& key, wgpu::MapAsyncStatus status, wgpu::StringView message) {
  if (status != wgpu::MapAsyncStatus::Success && status != wgpu::MapAsyncStatus::CallbackCancelled &&
      status != wgpu::MapAsyncStatus::Aborted) {
    Log.warn("frame average readback failed: {}", message);
  }
  g_readbacks.complete(key, [status](const wgpu::Buffer& buffer, float exposure) {
    if (status != wgpu::MapAsyncStatus::Success) {
      return;
    }
    const auto* texels = static_cast<const float*>(buffer.GetConstMappedRange(0, AverageBytes));
    if (texels != nullptr && exposure > 0.f) {
      double sum[3] = {};
      for (uint32_t y = 0; y < AverageSize; ++y) {
        const float* row = texels + size_t(y) * AverageRowBytes / sizeof(float);
        for (uint32_t x = 0; x < AverageSize; ++x) {
          for (int c = 0; c < 3; ++c) {
            sum[c] += row[x * 4 + c];
          }
        }
      }
      const std::lock_guard lock(g_radianceMutex);
      for (int c = 0; c < 3; ++c) {
        g_radiance[c] = float(sum[c] / (AverageSize * AverageSize) / exposure);
      }
      ++g_radianceSerial;
    }
    buffer.Unmap();
  });
}

// LUTs set by the game thread, uploaded by the next encode.
std::mutex g_pendingMutex;
std::unordered_map<uint32_t, std::vector<uint8_t>> g_pendingLuts;

wgpu::TextureView make_lut(uint32_t size, const uint8_t* rgba, const wgpu::Queue& queue) {
  const wgpu::TextureDescriptor descriptor{
      .label = "Colour Grade LUT",
      .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst,
      .dimension = wgpu::TextureDimension::e3D,
      .size = {size, size, size},
      .format = wgpu::TextureFormat::RGBA8Unorm,
  };
  AURORA_NOTE_TEXTURE(&descriptor);
  const auto texture = g_device.CreateTexture(&descriptor);
  const wgpu::TexelCopyTextureInfo dst{.texture = texture};
  const wgpu::TexelCopyBufferLayout layout{.bytesPerRow = size * 4, .rowsPerImage = size};
  const wgpu::Extent3D extent{size, size, size};
  queue.WriteTexture(&dst, rgba, size_t(size) * size * size * 4, &layout, &extent);
  return texture.CreateView();
}

void upload_pending(const wgpu::Queue& queue) {
  std::unordered_map<uint32_t, std::vector<uint8_t>> pending;
  {
    std::lock_guard lock(g_pendingMutex);
    pending.swap(g_pendingLuts);
  }
  for (const auto& [id, data] : pending) {
    g_state.luts[id] = make_lut(GradeLutSize, data.data(), queue);
  }
  if (!pending.empty()) {
    g_state.groups.clear();
  }
  if (!g_state.noLut) {
    const uint8_t white[4]{255, 255, 255, 255};
    g_state.noLut = make_lut(1, white, queue);
  }
}

wgpu::TextureView find_lut(uint32_t id) {
  if (id != 0) {
    if (const auto it = g_state.luts.find(id); it != g_state.luts.end()) {
      return it->second;
    }
  }
  return {};
}

wgpu::RenderPipeline make_pipeline(const char* label, const char* entry, wgpu::TextureFormat format,
                                   uint32_t samples, const wgpu::BlendState* blend,
                                   const wgpu::PipelineLayout& layout = {}, const wgpu::ShaderModule& module = {}) {
  const wgpu::ColorTargetState target{
      .format = format,
      .blend = blend,
      .writeMask = wgpu::ColorWriteMask::All,
  };
  const wgpu::FragmentState fragment{
      .module = module ? module : g_state.module,
      .entryPoint = entry,
      .targetCount = 1,
      .targets = &target,
  };
  const wgpu::RenderPipelineDescriptor descriptor{
      .label = label,
      .layout = layout ? layout : g_state.pipelineLayout,
      .vertex = {.module = g_state.module, .entryPoint = "vs_main"},
      .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList},
      .multisample = {.count = samples, .mask = UINT32_MAX},
      .fragment = &fragment,
  };
  return g_device.CreateRenderPipeline(&descriptor);
}

void ensure_pipelines() {
  if (g_state.module) {
    return;
  }
  wgpu::ShaderSourceWGSL source{};
  source.code = ShaderSource;
  const wgpu::ShaderModuleDescriptor moduleDescriptor{.nextInChain = &source, .label = "Bloom Module"};
  g_state.module = g_device.CreateShaderModule(&moduleDescriptor);
  const std::array entries{
      wgpu::BindGroupLayoutEntry{
          .binding = 0,
          .visibility = wgpu::ShaderStage::Fragment,
          .sampler = {.type = wgpu::SamplerBindingType::Filtering},
      },
      wgpu::BindGroupLayoutEntry{
          .binding = 1,
          .visibility = wgpu::ShaderStage::Fragment,
          .texture = {.sampleType = wgpu::TextureSampleType::Float, .viewDimension = wgpu::TextureViewDimension::e2D},
      },
      wgpu::BindGroupLayoutEntry{
          .binding = 2,
          .visibility = wgpu::ShaderStage::Fragment,
          .buffer = {.type = wgpu::BufferBindingType::Uniform, .minBindingSize = sizeof(Uniform)},
      },
      wgpu::BindGroupLayoutEntry{
          .binding = 3,
          .visibility = wgpu::ShaderStage::Fragment,
          .texture = {.sampleType = wgpu::TextureSampleType::Float, .viewDimension = wgpu::TextureViewDimension::e2D},
      },
      wgpu::BindGroupLayoutEntry{
          .binding = 4,
          .visibility = wgpu::ShaderStage::Fragment,
          .texture = {.sampleType = wgpu::TextureSampleType::Float, .viewDimension = wgpu::TextureViewDimension::e3D},
      },
      wgpu::BindGroupLayoutEntry{
          .binding = 5,
          .visibility = wgpu::ShaderStage::Fragment,
          .texture = {.sampleType = wgpu::TextureSampleType::Float, .viewDimension = wgpu::TextureViewDimension::e3D},
      },
  };
  const wgpu::BindGroupLayoutDescriptor layoutDescriptor{
      .label = "Bloom Bind Group Layout",
      .entryCount = entries.size(),
      .entries = entries.data(),
  };
  g_state.layout = g_device.CreateBindGroupLayout(&layoutDescriptor);
  const wgpu::PipelineLayoutDescriptor pipelineLayoutDescriptor{
      .label = "Bloom Pipeline Layout",
      .bindGroupLayoutCount = 1,
      .bindGroupLayouts = &g_state.layout,
  };
  g_state.pipelineLayout = g_device.CreatePipelineLayout(&pipelineLayoutDescriptor);
  const wgpu::SamplerDescriptor samplerDescriptor{
      .label = "Bloom Sampler",
      .addressModeU = wgpu::AddressMode::ClampToEdge,
      .addressModeV = wgpu::AddressMode::ClampToEdge,
      .magFilter = wgpu::FilterMode::Linear,
      .minFilter = wgpu::FilterMode::Linear,
  };
  g_state.sampler = g_device.CreateSampler(&samplerDescriptor);
  const wgpu::BufferDescriptor bufferDescriptor{
      .label = "Bloom Uniforms",
      .usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
      .size = SlotSize * PassCount * RecordedSlots,
  };
  g_state.uniforms = g_device.CreateBuffer(&bufferDescriptor);
  g_state.bright = make_pipeline("Bloom Bright Pass", "fs_bright", LevelFormat, 1, nullptr);
  g_state.down = make_pipeline("Bloom Downsample", "fs_down", LevelFormat, 1, nullptr);
  const wgpu::BlendState add{
      .color = {.operation = wgpu::BlendOperation::Add,
                .srcFactor = wgpu::BlendFactor::One,
                .dstFactor = wgpu::BlendFactor::One},
      .alpha = {.operation = wgpu::BlendOperation::Add,
                .srcFactor = wgpu::BlendFactor::Zero,
                .dstFactor = wgpu::BlendFactor::One},
  };
  g_state.up = make_pipeline("Bloom Upsample", "fs_up", LevelFormat, 1, &add);
  g_state.averageSamples = make_pipeline("Frame Average Samples", "fs_average_samples", AverageFormat, 1, nullptr);
  const wgpu::TextureDescriptor samplesDescriptor{
      .label = "Frame Average Samples",
      .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding,
      .size = {AverageSamples, AverageSamples, 1},
      .format = AverageFormat,
  };
  AURORA_NOTE_TEXTURE(&samplesDescriptor);
  g_state.averageSamplesTexture = g_device.CreateTexture(&samplesDescriptor);
  g_state.averageSamplesView = g_state.averageSamplesTexture.CreateView();
  const wgpu::BindGroupLayoutEntry samplesEntry{
      .binding = 1,
      .visibility = wgpu::ShaderStage::Fragment,
      .texture = {.sampleType = wgpu::TextureSampleType::UnfilterableFloat,
                  .viewDimension = wgpu::TextureViewDimension::e2D},
  };
  const wgpu::BindGroupLayoutDescriptor averageLayoutDescriptor{
      .label = "Frame Average Bind Group Layout",
      .entryCount = 1,
      .entries = &samplesEntry,
  };
  g_state.averageLayout = g_device.CreateBindGroupLayout(&averageLayoutDescriptor);
  const wgpu::PipelineLayoutDescriptor averagePipelineLayoutDescriptor{
      .label = "Frame Average Pipeline Layout",
      .bindGroupLayoutCount = 1,
      .bindGroupLayouts = &g_state.averageLayout,
  };
  const wgpu::PipelineLayout averagePipelineLayout = g_device.CreatePipelineLayout(&averagePipelineLayoutDescriptor);
  g_state.average = make_pipeline("Frame Average", "fs_average", AverageFormat, 1, nullptr, averagePipelineLayout);
  const wgpu::BindGroupEntry samplesBinding{.binding = 1, .textureView = g_state.averageSamplesView};
  const wgpu::BindGroupDescriptor averageGroupDescriptor{
      .label = "Frame Average Bind Group",
      .layout = g_state.averageLayout,
      .entryCount = 1,
      .entries = &samplesBinding,
  };
  g_state.averageGroup = g_device.CreateBindGroup(&averageGroupDescriptor);
  const wgpu::TextureDescriptor averageDescriptor{
      .label = "Frame Average",
      .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc,
      .size = {AverageSize, AverageSize, 1},
      .format = AverageFormat,
  };
  AURORA_NOTE_TEXTURE(&averageDescriptor);
  g_state.averageTexture = g_device.CreateTexture(&averageDescriptor);
  g_state.averageView = g_state.averageTexture.CreateView();
}

// The HDR entry points, in a module of their own so the LDR one keeps its source.
void ensure_hdr_pipelines() {
  if (g_state.hdrModule) {
    return;
  }
  std::string code{ShaderSource};
  code += hdr::ToneWgsl;
  code += hdr::PostToneWgsl;
  code += HdrSource;
  wgpu::ShaderSourceWGSL source{};
  source.code = code.c_str();
  const wgpu::ShaderModuleDescriptor moduleDescriptor{.nextInChain = &source, .label = "Bloom HDR Module"};
  g_state.hdrModule = g_device.CreateShaderModule(&moduleDescriptor);
  g_state.brightHdr =
      make_pipeline("Bloom Bright Pass (HDR)", "fs_bright_hdr", LevelFormat, 1, nullptr, {}, g_state.hdrModule);
  g_state.averageSamplesHdr = make_pipeline("Frame Average Samples (HDR)", "fs_average_samples_hdr", AverageFormat, 1,
                                            nullptr, {}, g_state.hdrModule);
}

void ensure_frame(uint32_t width, uint32_t height, wgpu::TextureFormat format) {
  if (g_state.frame && g_state.frameWidth == width && g_state.frameHeight == height && g_state.frameFormat == format) {
    return;
  }
  g_state.frameWidth = width;
  g_state.frameHeight = height;
  g_state.frameFormat = format;
  g_state.groups.clear();
  const wgpu::TextureDescriptor frameDescriptor{
      .label = "Bloom Frame Copy",
      .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst,
      .size = {width, height, 1},
      .format = format,
  };
  AURORA_NOTE_TEXTURE(&frameDescriptor);
  g_state.frame = g_device.CreateTexture(&frameDescriptor);
  g_state.frameView = g_state.frame.CreateView();
}

void ensure_levels(uint32_t width, uint32_t height) {
  if (g_state.levels[0].texture && g_state.width == width && g_state.height == height) {
    return;
  }
  g_state.width = width;
  g_state.height = height;
  g_state.groups.clear();
  uint32_t w = (width + 3) / 4;
  uint32_t h = (height + 3) / 4;
  for (auto& level : g_state.levels) {
    level.width = std::max(w, 1u);
    level.height = std::max(h, 1u);
    const wgpu::TextureDescriptor descriptor{
        .label = "Bloom Level",
        .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding,
        .size = {level.width, level.height, 1},
        .format = LevelFormat,
    };
    AURORA_NOTE_TEXTURE(&descriptor);
    level.texture = g_device.CreateTexture(&descriptor);
    level.view = level.texture.CreateView();
    w = (w + 1) / 2;
    h = (h + 1) / 2;
  }
}

wgpu::BindGroup bind_group(uint32_t slot, const wgpu::TextureView& source, const wgpu::TextureView& bloomSource,
                           const wgpu::TextureView& lutA, const wgpu::TextureView& lutB) {
  const wgpu::TextureView& viewA = lutA ? lutA : g_state.noLut;
  const wgpu::TextureView& viewB = lutB ? lutB : g_state.noLut;
  const BindGroupKey key{source.Get(), bloomSource.Get(), viewA.Get(), viewB.Get(), slot};
  // A frame that keeps changing (a source remade every frame) cannot grow this for ever.
  if (g_state.groups.size() > 256 && !g_state.groups.contains(key)) {
    g_state.groups.clear();
  }
  wgpu::BindGroup& group = g_state.groups[key];
  if (!group) {
    const std::array entries{
        wgpu::BindGroupEntry{.binding = 0, .sampler = g_state.sampler},
        wgpu::BindGroupEntry{.binding = 1, .textureView = source},
        wgpu::BindGroupEntry{
            .binding = 2, .buffer = g_state.uniforms, .offset = slot * SlotSize, .size = sizeof(Uniform)},
        wgpu::BindGroupEntry{.binding = 3, .textureView = bloomSource},
        wgpu::BindGroupEntry{.binding = 4, .textureView = viewA},
        wgpu::BindGroupEntry{.binding = 5, .textureView = viewB},
    };
    const wgpu::BindGroupDescriptor groupDescriptor{
        .layout = g_state.layout,
        .entryCount = entries.size(),
        .entries = entries.data(),
    };
    group = g_device.CreateBindGroup(&groupDescriptor);
  }
  return group;
}

void draw(const wgpu::CommandEncoder& cmd, const char* name, const wgpu::RenderPipeline& pipeline,
          const wgpu::BindGroup& group, const wgpu::TextureView& target, bool load,
          const wgpu::TextureView& resolve = {}) {
  const wgpu::RenderPassColorAttachment attachment{
      .view = target,
      .resolveTarget = resolve,
      .loadOp = load ? wgpu::LoadOp::Load : wgpu::LoadOp::Clear,
      .storeOp = wgpu::StoreOp::Store,
      .clearValue = {0.0, 0.0, 0.0, 0.0},
  };
  const wgpu::RenderPassDescriptor passDescriptor{
      .label = "Bloom Pass",
      .colorAttachmentCount = 1,
      .colorAttachments = &attachment,
      .timestampWrites = webgpu::gpu_prof::pass_writes(name),
  };
  const auto pass = cmd.BeginRenderPass(&passDescriptor);
  pass.SetPipeline(pipeline);
  pass.SetBindGroup(0, group);
  pass.Draw(3);
  pass.End();
}

void draw(const wgpu::CommandEncoder& cmd, const char* name, const wgpu::RenderPipeline& pipeline, uint32_t slot,
          const wgpu::TextureView& source, const wgpu::TextureView& bloomSource, const wgpu::TextureView& target,
          bool load, const wgpu::TextureView& resolve = {}, const wgpu::TextureView& lutA = {},
          const wgpu::TextureView& lutB = {}) {
  draw(cmd, name, pipeline, bind_group(slot, source, bloomSource, lutA, lutB), target, load, resolve);
}

// The composite's pipeline for the EFB pass it is drawn in: the frame as its first target, the
// pass's other targets untouched, its depth neither tested nor written.
wgpu::RenderPipeline make_pass_composite(const RenderTargetLayout& layout, const wgpu::ShaderModule& module,
                                         const char* entry) {
  std::array<wgpu::ColorTargetState, MaxColorAttachments> targets{};
  for (uint32_t i = 0; i < layout.colorAttachmentCount; ++i) {
    targets[i] = {
        .format = layout.colorAttachments[i].format,
        .writeMask = i == SceneColorAttachmentIndex ? wgpu::ColorWriteMask::All : wgpu::ColorWriteMask::None,
    };
  }
  const wgpu::FragmentState fragment{
      .module = module,
      .entryPoint = entry,
      .targetCount = layout.colorAttachmentCount,
      .targets = targets.data(),
  };
  const wgpu::DepthStencilState depth{
      .format = layout.depthStencilFormat,
      .depthWriteEnabled = false,
      .depthCompare = wgpu::CompareFunction::Always,
  };
  const wgpu::RenderPipelineDescriptor descriptor{
      .label = "Bloom Composite (EFB pass)",
      .layout = g_state.pipelineLayout,
      .vertex = {.module = module, .entryPoint = "vs_main"},
      .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList},
      .depthStencil = layout.depthStencilFormat != wgpu::TextureFormat::Undefined ? &depth : nullptr,
      .multisample = {.count = layout.sampleCount, .mask = UINT32_MAX},
      .fragment = &fragment,
  };
  return g_device.CreateRenderPipeline(&descriptor);
}

// The composite drawn first in the pass after the task, which cleared the frame rather than load
// it: a full-resolution store and load fewer than a pass of its own (see record). The payload is the
// post's slot. An HDR post's pass is the display EFB's (record_encoder_task_overwriting's
// leaveSceneHdr): a scene-format target here would be the wrong one, and is left undrawn.
void draw_pass_composite(const DrawContext& ctx, const wgpu::RenderPassEncoder& pass, const void* payload,
                         size_t payloadSize, void*) {
  uint32_t rec = 0;
  if (payloadSize != sizeof(rec)) {
    return;
  }
  std::memcpy(&rec, payload, sizeof(rec));
  if (rec >= RecordedSlots) {
    return;
  }
  Encoded& encoded = g_encoded[rec];
  if (!encoded.ready) {
    return;
  }
  encoded.ready = false;
  const auto& frame = ctx.layout.colorAttachments[SceneColorAttachmentIndex];
  const bool hdr = encoded.domain == Domain::Hdr;
  if (hdr && frame.format == webgpu::SceneHdrFormat) {
    Log.warn("HDR post composite reached an HDR scene target; not drawn");
    encoded = {};
    return;
  }
  const size_t d = hdr ? 1 : 0;
  if (!g_state.passComposite[d] || g_state.passCompositeKey[d] != ctx.layout.key) {
    g_state.passComposite[d] = hdr ? make_pass_composite(ctx.layout, g_state.hdrModule, "fs_composite_hdr")
                                   : make_pass_composite(ctx.layout, g_state.module, "fs_composite");
    g_state.passCompositeKey[d] = ctx.layout.key;
  }
  pass.SetPipeline(g_state.passComposite[d]);
  pass.SetBindGroup(0, bind_group(rec * PassCount + PassCount - 1, encoded.source, g_state.levels[0].view,
                                  encoded.lutA, encoded.lutB));
  pass.SetViewport(0.f, 0.f, float(frame.width), float(frame.height), 0.f, 1.f);
  pass.SetScissorRect(0, 0, frame.width, frame.height);
  pass.Draw(3);
  encoded = {};
}

// The frame copy's average into a free readback slot; none free skips this frame.
void encode_average(const wgpu::CommandEncoder& cmd, float exposure, const wgpu::TextureView& frame, uint32_t slot,
                    const wgpu::RenderPipeline& samples) {
  const wgpu::Buffer buffer = g_readbacks.claim(exposure);
  if (!buffer) {
    return;
  }
  draw(cmd, "Bloom average samples", samples, slot, frame, frame, g_state.averageSamplesView, false);
  draw(cmd, "Bloom average", g_state.average, g_state.averageGroup, g_state.averageView, false);
  const wgpu::TexelCopyTextureInfo source{.texture = g_state.averageTexture};
  const wgpu::TexelCopyBufferInfo target{
      .layout = {.bytesPerRow = AverageRowBytes, .rowsPerImage = AverageSize},
      .buffer = buffer,
  };
  const wgpu::Extent3D size{AverageSize, AverageSize, 1};
  cmd.CopyTextureToBuffer(&source, &target, &size);
}

// The HDR scene a post recorded, checked whole: the scene format, a single-sample resolve of the same
// size when multisampled. Anything else fails closed (no post) rather than read 16F texels as 8-bit.
bool hdr_source_valid(const Recorded& r, uint32_t samples) {
  const auto& c = r.color;
  if (!c.texture || !c.view || c.format != webgpu::SceneHdrFormat || c.texture.GetFormat() != webgpu::SceneHdrFormat ||
      c.texture.GetSampleCount() != samples || c.size.width == 0 || c.size.height == 0) {
    return false;
  }
  if (samples > 1) {
    const auto& v = r.resolved;
    return v.texture && v.view && v.format == webgpu::SceneHdrFormat &&
           v.texture.GetFormat() == webgpu::SceneHdrFormat && v.texture.GetSampleCount() == 1 &&
           v.size.width == c.size.width && v.size.height == c.size.height;
  }
  return true;
}

void encode(const EncoderTaskContext& ctx, const wgpu::CommandEncoder& cmd, const void* payload, size_t payloadSize,
            void*) {
  uint32_t rec = 0;
  if (payloadSize != sizeof(rec)) {
    return;
  }
  std::memcpy(&rec, payload, sizeof(rec));
  if (rec >= RecordedSlots) {
    return;
  }
  g_encoded[rec] = {};
  Recorded recorded;
  {
    const std::lock_guard lock(g_recordMutex);
    recorded = g_recorded[rec];
  }
  Params& params = recorded.params;
  const bool hdr = recorded.domain == Domain::Hdr;
  // The targets this frame was recorded against (a resize may have made newer ones since).
  const auto efb = webgpu::efb_targets();
  const auto& source = webgpu::present_source();
  const auto& target = efb.color;
  const uint32_t samples = webgpu::g_graphicsConfig.msaaSamples > 1 ? webgpu::g_graphicsConfig.msaaSamples : 1;
  const auto format = webgpu::g_graphicsConfig.surfaceConfiguration.format;
  if (hdr && !hdr_source_valid(recorded, samples)) {
    // The pass after it was retargeted to the display EFB and clears it: its composite stays undrawn.
    Log.warn("HDR post: the recorded scene is not a valid {}-sample RGBA16F target; skipped", samples);
    return;
  }
  // The HDR scene is read where it is (resolved when multisampled): the composite writes the display EFB.
  const auto& hdrSource = samples > 1 ? recorded.resolved : recorded.color;
  const uint32_t width = hdr ? hdrSource.size.width : source.size.width;
  const uint32_t height = hdr ? hdrSource.size.height : source.size.height;
  if (width == 0 || height == 0 || (!hdr && !source.texture) || !target.view) {
    return;
  }
  ensure_pipelines();
  if (hdr) {
    ensure_hdr_pipelines();
  } else {
    ensure_frame(width, height, format);
  }
  ensure_levels(width, height);
  upload_pending(ctx.queue);
  const auto lutA = find_lut(params.gradeA);
  const auto lutB = find_lut(params.gradeB);
  const bool bloom = (params.bloom & BloomOn) != 0;
  const bool inPass = (params.bloom & CompositeInPass) != 0;
  // tone[0][3] carries the exposure to measure at; the curve has no use for it.
  const float exposure = params.tone[0][3];
  params.tone[0][3] = 0.f;
  // The HDR scene holds the exposed light itself, curve or none.
  const bool measure = exposure > 0.f && (hdr || params.tone[1][0] > 0.f);
  // The pass after an in-pass composite clears the frame, so it must be drawn even before a
  // LUT has been uploaded.
  const bool post = bloom || lutA || lutB || inPass;
  if (!post && !measure) {
    return;
  }
  if (!hdr &&
      (!g_state.composite || g_state.compositeFormat != format || g_state.compositeSamples != samples)) {
    g_state.composite = make_pipeline("Bloom Composite", "fs_composite", format, samples, nullptr);
    g_state.compositeFormat = format;
    g_state.compositeSamples = samples;
  }

  // Every pass's uniforms: bright, the downsamples, the upsamples, the composite; in the post's own
  // ranges, as another post in this submit writes its own before either runs.
  const uint32_t base = rec * PassCount;
  std::array<std::array<uint8_t, SlotSize>, PassCount> slots{};
  const auto put = [&](uint32_t slot, float texelX, float texelY, const float* tint) {
    Uniform u{};
    u.texel[0] = texelX;
    u.texel[1] = texelY;
    if (tint != nullptr) {
      std::memcpy(u.tint, tint, sizeof(float) * 3);
    }
    u.tint[3] = params.threshold;
    std::memcpy(u.tone, params.tone, sizeof(u.tone));
    u.grade[0] = std::clamp(params.gradeWeight, 0.f, 1.f);
    u.grade[1] = lutA ? 1.f : 0.f;
    u.grade[2] = lutB ? 1.f : 0.f;
    u.grade[3] = bloom ? 1.f : 0.f;
    std::memcpy(slots[slot].data(), &u, sizeof(u));
  };
  uint32_t slot = 0;
  put(slot++, 1.f / float(width), 1.f / float(height), params.tints[4]);
  for (uint32_t i = 1; i < Levels; ++i) {
    const auto& from = g_state.levels[i - 1];
    put(slot++, 1.f / float(from.width), 1.f / float(from.height), nullptr);
  }
  for (uint32_t i = Levels - 1; i-- > 0;) {
    const auto& from = g_state.levels[i + 1];
    put(slot++, 1.f / float(from.width), 1.f / float(from.height), params.tints[Levels - 2 - i]);
  }
  put(slot++, 0.f, 0.f, nullptr);
  ctx.queue.WriteBuffer(g_state.uniforms, uint64_t(base) * SlotSize, slots.data(), sizeof(slots));

  const auto& averageSamples = hdr ? g_state.averageSamplesHdr : g_state.averageSamples;
  if (!post) {
    // Measuring only: nothing draws into the frame here, so it is read where it is. The copy
    // exists for the composite, which writes the frame it reads.
    encode_average(cmd, exposure, hdr ? hdrSource.view : source.view, base, averageSamples);
    return;
  }
  // The frame the chain reads: the HDR scene as it is (the composite writes another target), else a
  // copy of the EFB the composite overwrites.
  wgpu::TextureView frameView = g_state.frameView;
  if (hdr) {
    frameView = hdrSource.view;
  } else {
    const wgpu::TexelCopyTextureInfo copySource{.texture = source.texture};
    const wgpu::TexelCopyTextureInfo copyTarget{.texture = g_state.frame};
    const wgpu::Extent3D copySize{width, height, 1};
    if ((params.bloom & CostNoFrameCopy) == 0) {
      cmd.CopyTextureToTexture(&copySource, &copyTarget, &copySize);
    }
  }

  if (measure) {
    encode_average(cmd, exposure, frameView, base, averageSamples);
  }
  slot = base;
  auto& levels = g_state.levels;
  if (bloom) {
    draw(cmd, "Bloom bright", hdr ? g_state.brightHdr : g_state.bright, slot++, frameView, frameView, levels[0].view,
         false);
    for (uint32_t i = 1; i < Levels; ++i) {
      draw(cmd, "Bloom down", g_state.down, slot++, levels[i - 1].view, levels[i - 1].view, levels[i].view, false);
    }
    for (uint32_t i = Levels - 1; i-- > 0;) {
      draw(cmd, "Bloom up", g_state.up, slot++, levels[i + 1].view, levels[i + 1].view, levels[i].view, true);
    }
  } else {
    slot = base + PassCount - 1;
  }
  if (inPass) {
    g_encoded[rec] = {.ready = true, .domain = recorded.domain, .source = frameView, .lutA = lutA, .lutB = lutB};
    return;
  }
  // The composite writes every pixel without blending, so the frame need not be loaded first
  // (a full-resolution read on a tile-based GPU). An HDR post always composites in the pass (record).
  if (hdr) {
    return;
  }
  draw(cmd, "Bloom composite", g_state.composite, slot++, frameView, levels[0].view, target.view, false,
       samples > 1 ? efb.resolved.view : wgpu::TextureView{}, lutA, lutB);
}

// A slot for a post in this frame; false (warned) past MaxPostsPerFrame. Under g_recordMutex.
bool claim_slot(uint32_t& slot) {
  const uint64_t frame = current_frame();
  if (g_postFrame != frame) {
    g_postFrame = frame;
    g_postCount = 0;
  }
  if (g_postCount >= MaxPostsPerFrame) {
    Log.warn("more than {} posts in one frame; this one is skipped", MaxPostsPerFrame);
    return false;
  }
  ++g_postCount;
  slot = g_nextSlot++ % RecordedSlots;
  return true;
}
} // namespace

bool ensure_task() {
  const std::lock_guard registrationLock(g_taskRegistrationMutex);
  if (g_state.task == InvalidEncoderTask) {
    g_state.task = register_encoder_task_type(EncoderTaskDescriptor{.label = "Bloom", .callback = encode});
    if (g_state.task == InvalidEncoderTask) {
      Log.warn("could not register the bloom task");
      return false;
    }
  }
  if (g_state.compositeDraw == InvalidDrawType) {
    g_state.compositeDraw = register_draw_type(DrawTypeDescriptor{.label = "Bloom Composite", .draw = draw_pass_composite});
  }
  return true;
}

bool scene_post_available() {
  // The FIFO preflight and all post records share one writer. Normal teardown stops it before these
  // types are unregistered; invalidation after this check is unsupported failure injection.
  {
    const std::lock_guard registrationLock(g_taskRegistrationMutex);
    if (g_state.task == InvalidEncoderTask || g_state.compositeDraw == InvalidDrawType ||
        !detail::find_runtime_encoder_task_type(g_state.task) ||
        !detail::find_runtime_draw_type(g_state.compositeDraw)) {
      return false;
    }
  }
  const std::lock_guard lock(g_recordMutex);
  const uint64_t frame = current_frame();
  const size_t claimed = g_postFrame == frame ? g_postCount : 0;
  return claimed < MaxPostsPerFrame;
}

bool push(const Params& params) {
  if (!ensure_task()) {
    return false;
  }
  // Pending FIFO commands first, so the post lands after what they draw.
  gx::fifo::drain();
  if (scene_hdr_active()) {
    Log.warn("bloom::push under the HDR scene; use GXPortPostProcessScene");
    return false;
  }
  Params task = params;
  task.bloom = task.bloom != 0 ? BloomOn : 0;
  uint32_t slot = 0;
  {
    const std::lock_guard lock(g_recordMutex);
    if (!claim_slot(slot)) {
      return false;
    }
    g_recorded[slot] = Recorded{.params = task};
  }
  return record_encoder_task(g_state.task, &slot, sizeof(slot));
}

bool record(const Params& params) {
  if (g_state.task == InvalidEncoderTask) {
    return false;
  }
  const bool hdr = scene_hdr_pass_active();
  if (!hdr && scene_hdr_active()) {
    // The HDR scene is on but not this pass's target (an offscreen pass): there is no scene to read.
    Log.warn("post recorded outside the HDR scene's pass; skipped");
    return false;
  }
  const bool scene = (params.bloom & ForceScene) != 0;
  Params task = params;
  task.bloom &= BloomOn | CostNoFrameCopy | CostNoDepthReload | CostKeepDepth;
  // PrimedGun: an immersive frame's eye passes replay neither encoder tasks nor custom draws
  // (encoding.cpp render_stereo_eye_pass_commands), so a bloom or grade would reach only the mono
  // EFB, which the headset does not show, and the clear of the in-pass composite's pass would be
  // mirrored into the eyes and wipe the world from them. Such a frame keeps only the exposure
  // measurement, taken from the mono EFB (still drawn up to here), through the plain task below,
  // whose next pass loads the colour in the mono and the eyes alike. With nothing to measure it
  // records nothing, so the eye passes are not broken in two for no work. Nor does it measure when
  // the frame's mono image is skipped (a standalone headset, or a desktop mirror showing the eyes):
  // only the final pass skips its mono render, so the break would bring back a whole mono render of
  // the world for one average, and the exposure stays where it was instead.
  if (stereo_shadow::active()) {
    task.bloom &= ~BloomOn;
    task.gradeA = 0;
    task.gradeB = 0;
    // The same test encode measures on.
    const bool measure = task.tone[0][3] > 0.f && task.tone[1][0] > 0.f;
    if (!measure || stereo_host::headset_owns_display() || stereo_host::mirror_skips_mono()) {
      return true;
    }
  }
  const bool effects = (task.bloom & BloomOn) || task.gradeA != 0 || task.gradeB != 0 || task.tone[0][3] > 0.f;
  if (!hdr && scene && !effects) {
    // GXPortPostProcessScene on the display EFB with nothing to do: it is already display colour.
    return true;
  }
  if (hdr && g_state.compositeDraw == InvalidDrawType) {
    Log.warn("HDR post without the composite draw type; skipped");
    return false;
  }
  // A frame that is composited draws it as the first thing in the pass that resumes the EFB: on a
  // tile-based GPU a pass of its own stores the frame only for that pass to load it again. That pass
  // clears depth instead of loading it when nothing after the bloom (the HUD) tests against the world's.
  // The HDR scene is converted to display colour whatever else the post does: its composite always
  // draws, as the first draw of the display EFB's pass the FIFO goes on in.
  const bool inPass = hdr || (g_state.compositeDraw != InvalidDrawType &&
                              ((task.bloom & BloomOn) || task.gradeA != 0 || task.gradeB != 0));
  if (inPass) {
    task.bloom |= CompositeInPass;
  }
  uint32_t slot = 0;
  {
    const std::lock_guard lock(g_recordMutex);
    if (!claim_slot(slot)) {
      return false;
    }
    g_recorded[slot] = Recorded{
        .params = task,
        .domain = hdr ? Domain::Hdr : Domain::Ldr,
        .color = hdr ? webgpu::g_sceneHdr : webgpu::TextureWithSampler{},
        .resolved = hdr ? webgpu::g_sceneHdrResolved : webgpu::TextureWithSampler{},
    };
  }
  const auto depth = (task.bloom & CostNoDepthReload) ? DepthAfter::Clear
                     : (task.bloom & CostKeepDepth)   ? DepthAfter::Load
                                                      : DepthAfter::IfUnread;
  if (inPass) {
    return record_encoder_task_overwriting(g_state.task, &slot, sizeof(slot), g_state.compositeDraw, depth, &slot,
                                           sizeof(slot), hdr);
  }
  return record_encoder_task(g_state.task, &slot, sizeof(slot));
}

void set_grade_lut(uint32_t id, const uint8_t* rgba) {
  if (id == 0 || rgba == nullptr) {
    return;
  }
  std::lock_guard lock(g_pendingMutex);
  g_pendingLuts[id].assign(rgba, rgba + size_t(GradeLutSize) * GradeLutSize * GradeLutSize * 4);
}

void after_submit() noexcept {
  const auto submitted = g_readbacks.submit();
  for (size_t i = 0; i < submitted.count; ++i) {
    const auto& mapping = submitted.mappings[i];
    // The callback carries the key alone: a pending map must not hold the buffer it maps, or that
    // buffer would outlive its slot. Mapped with no lock held, as the callback takes it again.
    mapping.buffer.MapAsync(wgpu::MapMode::Read, 0, AverageBytes, wgpu::CallbackMode::AllowSpontaneous,
                            [key = mapping.key](wgpu::MapAsyncStatus status, wgpu::StringView message) {
                              complete_readback(key, status, message);
                            });
  }
}

bool frame_radiance(float out[3], uint32_t& serial) {
  std::lock_guard lock(g_radianceMutex);
  serial = g_radianceSerial;
  if (g_radianceSerial == 0) {
    return false;
  }
  std::memcpy(out, g_radiance, sizeof(g_radiance));
  return true;
}

void shutdown() {
  // The readback buffers come out of the slots under the slots' lock, and are released only once
  // it is gone: releasing one with a map still in flight runs that map's callback on this thread,
  // and the callback takes the lock again.
  auto retired = g_readbacks.retire([] {
    const std::lock_guard lock(g_radianceMutex);
    g_radianceSerial = 0;
  });
  retired.clear();
  const auto task = g_state.task;
  const auto compositeDraw = g_state.compositeDraw;
  g_state = {};
  g_encoded = {};
  {
    const std::lock_guard lock(g_recordMutex);
    g_recorded = {};
    g_nextSlot = 0;
    g_postFrame = UINT64_MAX;
    g_postCount = 0;
  }
  {
    std::lock_guard lock(g_pendingMutex);
    g_pendingLuts.clear();
  }
  if (task != InvalidEncoderTask) {
    unregister_encoder_task_type(task);
  }
  if (compositeDraw != InvalidDrawType) {
    unregister_draw_type(compositeDraw);
  }
}
} // namespace aurora::gfx::bloom
