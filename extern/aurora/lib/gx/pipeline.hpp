#pragma once

#include "../gfx/types.hpp"
#include "gx.hpp"

#include <array>
#include <cstdint>

namespace aurora::gx {
struct DrawData {
  gfx::PipelineRef pipeline;
  gfx::Range vertRange;
  gfx::Range idxRange;
  gfx::Range uniformRange;
  DrawImmediateData immediateData;
  uint32_t vtxCount;
  uint32_t indexCount;
  uint32_t instanceCount;
  GXBindGroups bindGroups;
  uint32_t dstAlpha;
  // Stereo replay (gfx/stereo_frame.hpp): the uniform offsets this draw binds
  // in the left and right eye passes, staged along with the mono uniform.
  // UINT32_MAX leaves the draw out of that eye.
  std::array<uint32_t, 2> stereoUniformOffset{UINT32_MAX, UINT32_MAX};
  // The draw reads the geometry cache's buffer (gfx/geometry_buffer.hpp) with 32-bit
  // indices absolute into it (gx/geometry_cache.hpp), vtxStart 0.
  bool cachedGeometry = false;
  bool nativeVertices = false;
  TextureLayout textureLayout = TextureLayout::Lean; // group 2's layout, the pipeline's (texture_layout)
  // ... and the texture bind groups, when the draw samples an EFB copy taken
  // per eye; zero binds the mono group. Under multiview (gfx/stereo_multiview.hpp)
  // [0] is the multiview pipeline's bind group, of 2D array views.
  std::array<gfx::BindGroupRef, 2> stereoTextureBindGroup{};
  // Multiview: the pipeline that draws both views of an eye pass, with the
  // uniform pair at stereoUniformOffset[0]. In a frame replayed per eye, the
  // EyeClipImmediate pipeline for a draw staged with its eye clips, else zero.
  gfx::PipelineRef multiviewPipeline{};
};

constexpr uint32_t GXPipelineConfigVersion = 16;
struct PipelineConfig {
  uint32_t version = GXPipelineConfigVersion;
  uint32_t msaaSamples = 1;
  ShaderConfig shaderConfig;
  GXCompare depthFunc;
  GXCullMode cullMode;
  GXBlendMode blendMode;
  GXBlendFactor blendFacSrc, blendFacDst;
  GXLogicOp blendOp;
  uint32_t dstAlpha;
  uint32_t polygonOffsetBits;
  uint32_t polygonOffsetScaleBits;
  uint32_t polygonOffsetClampBits;
  // 1: the sun's shadow-map pipeline of a ShaderConfig::shadow draw (vs_shadow, depth only).
  // 2: the same with the draw's cut test (vs_shadow_cut, fs_shadow; the colour pass's group 2 and layout).
  uint32_t shadowPass = 0;
  bool depthCompare, depthUpdate, alphaUpdate, colorUpdate;
};
static_assert(std::has_unique_object_representations_v<PipelineConfig>);

constexpr bool pipeline_config_compatible(const PipelineConfig& config, bool lightmapBinding) noexcept {
  const auto& shader = config.shaderConfig;
  return shader.pbr == 0 || shader.pbrLightmapAttr == GX_VA_NULL || lightmapBinding;
}

wgpu::RenderPipeline create_pipeline([[maybe_unused]] const PipelineConfig& config);
void render(const DrawData& data, const wgpu::RenderPassEncoder& pass);
// The same draw in a stereo eye pass, bound to its eye uniform.
void render_eye(const DrawData& data, const wgpu::RenderPassEncoder& pass, uint32_t eye);
// The same draw in a multiview eye pass (gfx/stereo_multiview.hpp): both eyes at once.
void render_multiview(const DrawData& data, const wgpu::RenderPassEncoder& pass);

void queue_surface(const u8* dlStart, uint32_t dlSize, bool bigEndian) noexcept;
} // namespace aurora::gx
