#include "pipeline.hpp"

#include "../gfx/encoding.hpp"
#include "../gfx/overdraw.hpp"
#include "../gfx/resources.hpp"
#include "../gfx/pipeline_cache.hpp"
#include "../gfx/resource_cache.hpp"
#include "../webgpu/gpu.hpp"

#include "gx_fmt.hpp"
#include "native_vertex.hpp"
#include "shader_info.hpp"
#include "../logging.hpp"
#include "../webgpu/gpu.hpp"

#include <tracy/Tracy.hpp>

static aurora::Module Log("aurora::gx::pipeline");

namespace aurora::gx {

wgpu::RenderPipeline create_pipeline(const PipelineConfig& config) {
  if (!pipeline_config_compatible(config, webgpu::g_lightmapBinding)) {
    Log.error("Refusing GX pipeline with PBR lightmap attribute {}: lightmap binding is unsupported",
              config.shaderConfig.pbrLightmapAttr);
    return {};
  }

  ZoneScoped;
  if ((config.shaderConfig.multiview == MultiviewClip || config.shaderConfig.multiview == MultiviewFull) &&
      !webgpu::g_multiviewSupported) {
    // A multiview config from the pipeline cache on a device without the feature
    // (gfx/stereo_multiview.hpp): its shader would not compile. Never bound.
    return {};
  }
  auto dstAlphaMode = dst_alpha_mode(config);
  const auto shader = build_shader(config.shaderConfig, dstAlphaMode);
  const auto label =
      fmt::format("GX Pipeline {:x} shader {:x}", xxh3_hash(config, static_cast<HashType>(gfx::ShaderType::GX)),
                  xxh3_hash(config.shaderConfig));
  if (config.shaderConfig.nativeVertices) {
    const auto native = native_vertex::layout(config.shaderConfig);
    CHECK(native.valid, "Unsupported native vertex layout reached pipeline creation");
    // Dawn versions used by desktop and Quest declare these fields in different orders.
    wgpu::VertexBufferLayout buffer{};
    buffer.arrayStride = config.shaderConfig.vtxStride;
    buffer.stepMode = wgpu::VertexStepMode::Vertex;
    buffer.attributeCount = native.count;
    buffer.attributes = native.attributes.data();
    const std::array buffers{buffer};
    return build_pipeline(config, buffers, shader, label.c_str(), dstAlphaMode);
  }
  return build_pipeline(config, {}, shader, label.c_str(), dstAlphaMode);
}

void render(const DrawData& data, const wgpu::RenderPassEncoder& pass) {
  if (!gfx::bind_pipeline(data.pipeline, pass)) {
    return;
  }

  const auto& resources = gfx::detail::resources();
  pass.SetImmediates(0, &data.immediateData, sizeof(data.immediateData));
  gfx::bind_gx_uniform(pass, resources.uniformBindGroup, data.uniformRange.offset);
  // Group 2 in the pipeline's layout: a draw without a group of its own inherits the bound one only
  // in that layout (bind_gx_textures puts the layout's empty group back otherwise).
  gfx::bind_gx_textures(pass, data.bindGroups.textureBindGroup, data.textureLayout);
  gfx::bind_gx_geometry(pass, data.cachedGeometry);
  if (data.nativeVertices) {
    gfx::bind_gx_native_vertices(pass);
  }
  gfx::bind_gx_indices(pass, resources.indexBuffer, data.idxRange.offset, data.idxRange.size,
                       data.cachedGeometry ? wgpu::IndexFormat::Uint32 : wgpu::IndexFormat::Uint16);
  if (data.dstAlpha != UINT32_MAX) {
    const wgpu::Color color{0.f, 0.f, 0.f, data.dstAlpha / 255.f};
    pass.SetBlendConstant(&color);
  }
  if (data.indexCount == 0) {
    pass.Draw(data.vtxCount, data.instanceCount);
  } else {
    pass.DrawIndexed(data.indexCount, data.instanceCount);
  }
}

void render_eye(const DrawData& data, const wgpu::RenderPassEncoder& pass, uint32_t eye) {
  const uint32_t uniformOffset = data.stereoUniformOffset[eye];
  if (uniformOffset == UINT32_MAX) {
    return;
  }
  const auto& resources = gfx::detail::resources();
  // EyeClipImmediate (a frame without multiview): the mono uniform with the eye clips
  // after it, the eye named by the immediates.
  const bool eyeClip = data.multiviewPipeline != gfx::PipelineRef{};
  if (!gfx::bind_pipeline(eyeClip ? data.multiviewPipeline : data.pipeline, pass)) {
    return;
  }
  if (eyeClip) {
    DrawImmediateData immediates = data.immediateData;
    set_eye_mask(immediates, eye);
    pass.SetImmediates(0, &immediates, sizeof(immediates));
    gfx::bind_gx_uniform(pass, resources.multiviewUniformBindGroup, uniformOffset);
  } else {
    pass.SetImmediates(0, &data.immediateData, sizeof(data.immediateData));
    gfx::bind_gx_uniform(pass, resources.uniformBindGroup, uniformOffset);
  }
  const gfx::BindGroupRef textureBindGroup =
      data.stereoTextureBindGroup[eye] ? data.stereoTextureBindGroup[eye] : data.bindGroups.textureBindGroup;
  gfx::bind_gx_textures(pass, textureBindGroup, data.textureLayout);
  gfx::bind_gx_geometry(pass, data.cachedGeometry);
  if (data.nativeVertices) {
    gfx::bind_gx_native_vertices(pass);
  }
  gfx::bind_gx_indices(pass, resources.indexBuffer, data.idxRange.offset, data.idxRange.size,
                       data.cachedGeometry ? wgpu::IndexFormat::Uint32 : wgpu::IndexFormat::Uint16);
  if (data.dstAlpha != UINT32_MAX) {
    const wgpu::Color color{0.f, 0.f, 0.f, data.dstAlpha / 255.f};
    pass.SetBlendConstant(&color);
  }
  if (data.indexCount == 0) {
    pass.Draw(data.vtxCount, data.instanceCount);
  } else {
    pass.DrawIndexed(data.indexCount, data.instanceCount);
  }
}

void render_multiview(const DrawData& data, const wgpu::RenderPassEncoder& pass) {
  const uint32_t uniformOffset = data.stereoUniformOffset[0];
  if (uniformOffset == UINT32_MAX || data.multiviewPipeline == gfx::PipelineRef{}) {
    return;
  }
  if (!gfx::bind_pipeline(data.multiviewPipeline, pass)) {
    return;
  }

  const auto& resources = gfx::detail::resources();
  pass.SetImmediates(0, &data.immediateData, sizeof(data.immediateData));
  // The mono uniform and the eye clip matrices after it (MultiviewClip), or the
  // pair of eye copies from this offset (MultiviewFull; the mono uniform as
  // element 0 when the eye mask is 0).
  gfx::bind_gx_uniform(pass, resources.multiviewUniformBindGroup, uniformOffset);
  gfx::bind_gx_textures(pass, data.stereoTextureBindGroup[0], data.textureLayout, true);
  gfx::bind_gx_geometry(pass, data.cachedGeometry);
  if (data.nativeVertices) {
    gfx::bind_gx_native_vertices(pass);
  }
  gfx::bind_gx_indices(pass, resources.indexBuffer, data.idxRange.offset, data.idxRange.size,
                       data.cachedGeometry ? wgpu::IndexFormat::Uint32 : wgpu::IndexFormat::Uint16);
  if (data.dstAlpha != UINT32_MAX) {
    const wgpu::Color color{0.f, 0.f, 0.f, data.dstAlpha / 255.f};
    pass.SetBlendConstant(&color);
  }
  const uint32_t query = gfx::overdraw::draw_begin(data.pipeline, pass);
  if (data.indexCount == 0) {
    pass.Draw(data.vtxCount, data.instanceCount);
  } else {
    pass.DrawIndexed(data.indexCount, data.instanceCount);
  }
  gfx::overdraw::draw_end(query, pass);
}

} // namespace aurora::gx
