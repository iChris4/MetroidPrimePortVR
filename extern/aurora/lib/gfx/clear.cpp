#include "clear.hpp"

#include "encoding.hpp"
#include "stereo_multiview.hpp"
#include "../webgpu/gpu.hpp"
#include "tracy/Tracy.hpp"

namespace aurora::gfx::clear {
using webgpu::g_device;

namespace {
wgpu::ColorWriteMask clear_write_mask(bool clearColor, bool clearAlpha) {
  auto writeMask = wgpu::ColorWriteMask::None;
  if (clearColor) {
    writeMask |= wgpu::ColorWriteMask::Red | wgpu::ColorWriteMask::Green | wgpu::ColorWriteMask::Blue;
  }
  if (clearAlpha) {
    writeMask |= wgpu::ColorWriteMask::Alpha;
  }
  return writeMask;
}

std::string shader_source(bool writesSceneColor) {
  std::string source{R"""(
struct VertexOutput {
    @builtin(position) pos: vec4<f32>,
};

var<private> pos: array<vec2<f32>, 3> = array<vec2<f32>, 3>(
    vec2(-1.0, 1.0),
    vec2(-1.0, -3.0),
    vec2(3.0, 1.0),
);

@vertex
fn vs_main(@builtin(vertex_index) vtxIdx: u32) -> VertexOutput {
    var out: VertexOutput;
    out.pos = vec4<f32>(pos[vtxIdx], 0.0, 1.0);
    return out;
}
)"""};

  if (writesSceneColor) {
    source += fmt::format(R"""(
@fragment
fn fs_main() -> @location({}) vec4<f32> {{
    return vec4<f32>(1.0);
}}
)""",
                          SceneColorAttachmentIndex);
  } else {
    source += R"""(
@fragment
fn fs_main() {
}
)""";
  }
  return source;
}
} // namespace

PipelineConfig make_pipeline_config(const RenderTargetLayout& layout, bool clearColor, bool clearAlpha,
                                    bool clearDepth) noexcept {
  PipelineConfig config{
      .targetLayoutKey = layout.key,
      .depthStencilFormat = layout.depthStencilFormat,
      .colorAttachmentCount = layout.colorAttachmentCount,
      .msaaSamples = layout.sampleCount,
      .clearColor = clearColor,
      .clearAlpha = clearAlpha,
      .clearDepth = clearDepth,
  };
  for (uint32_t i = 0; i < layout.colorAttachmentCount; ++i) {
    config.colorFormats[i] = layout.colorAttachments[i].format;
  }
  return config;
}

wgpu::RenderPipeline create_pipeline(const PipelineConfig& config) {
  ZoneScoped;
  if (config.multiview && !webgpu::g_multiviewSupported) {
    // A cached multiview config on a device without the feature: never bound.
    return {};
  }
  const bool writesSceneColor = config.clearColor || config.clearAlpha;
  const auto source = shader_source(writesSceneColor);
  wgpu::ShaderSourceWGSL sourceDescriptor{};
  sourceDescriptor.code = source.c_str();
  const wgpu::ShaderModuleDescriptor moduleDescriptor{
      .nextInChain = &sourceDescriptor,
      .label = "EFB Clear Module",
  };
  auto module = g_device.CreateShaderModule(&moduleDescriptor);
  constexpr wgpu::PipelineLayoutDescriptor layoutDescriptor{
      .bindGroupLayoutCount = 0,
      .bindGroupLayouts = nullptr,
  };
  auto pipelineLayout = g_device.CreatePipelineLayout(&layoutDescriptor);
  constexpr wgpu::BlendState blendState{
      .color =
          wgpu::BlendComponent{
              .operation = wgpu::BlendOperation::Add,
              .srcFactor = wgpu::BlendFactor::Constant,
              .dstFactor = wgpu::BlendFactor::Zero,
          },
      .alpha =
          wgpu::BlendComponent{
              .operation = wgpu::BlendOperation::Add,
              .srcFactor = wgpu::BlendFactor::Constant,
              .dstFactor = wgpu::BlendFactor::Zero,
          },
  };
  std::array<wgpu::ColorTargetState, MaxColorAttachments> colorTargets{};
  for (uint32_t i = 0; i < config.colorAttachmentCount; ++i) {
    colorTargets[i] = {
        .format = config.colorFormats[i],
        .writeMask = wgpu::ColorWriteMask::None,
    };
  }
  colorTargets[SceneColorAttachmentIndex].blend = &blendState;
  colorTargets[SceneColorAttachmentIndex].writeMask = clear_write_mask(config.clearColor, config.clearAlpha);
  const wgpu::FragmentState fragmentState{
      .module = module,
      .entryPoint = "fs_main",
      .targetCount = config.colorAttachmentCount,
      .targets = colorTargets.data(),
  };
  const wgpu::DepthStencilState depthStencil{
      .format = config.depthStencilFormat,
      .depthWriteEnabled = config.clearDepth,
      .depthCompare = wgpu::CompareFunction::Always,
  };
  const auto label = fmt::format("EFB Clear Pipeline (color {}, alpha {}, depth {}{})", config.clearColor,
                                 config.clearAlpha, config.clearDepth, config.multiview ? ", multiview" : "");
  wgpu::RenderPipelineDescriptor pipelineDescriptor{
      .label = label.c_str(),
      .layout = pipelineLayout,
      .vertex =
          wgpu::VertexState{
              .module = module,
              .entryPoint = "vs_main",
          },
      .primitive =
          wgpu::PrimitiveState{
              .topology = wgpu::PrimitiveTopology::TriangleList,
          },
      .depthStencil = config.depthStencilFormat != wgpu::TextureFormat::Undefined ? &depthStencil : nullptr,
      .multisample =
          wgpu::MultisampleState{
              .count = config.msaaSamples,
          },
      .fragment = &fragmentState,
  };
#if defined(WGPU_DAWN_RENDER_PIPELINE_MULTIVIEW_INIT)
  wgpu::DawnRenderPipelineMultiview multiviewState{};
  multiviewState.viewMask = stereo_multiview::kViewMask;
  if (config.multiview) {
    pipelineDescriptor.nextInChain = &multiviewState;
  }
#endif
  return g_device.CreateRenderPipeline(&pipelineDescriptor);
}

void render(const DrawData& data, const wgpu::RenderPassEncoder& pass, const wgpu::Extent3D& targetSize) {
  if (!bind_pipeline(data.pipeline, pass)) {
    return;
  }

  pass.SetBlendConstant(&data.color);
  pass.SetViewport(0.f, 0.f, static_cast<float>(targetSize.width), static_cast<float>(targetSize.height), data.depth,
                   data.depth);
  const auto rect = target_rect(data, targetSize);
  pass.SetScissorRect(rect.x, rect.y, rect.width, rect.height);
  pass.Draw(3);
}

void render_multiview(const DrawData& data, const wgpu::RenderPassEncoder& pass, const wgpu::Extent3D& targetSize) {
  if (data.multiviewPipeline == PipelineRef{}) {
    return;
  }
  DrawData multiview = data;
  multiview.pipeline = data.multiviewPipeline;
  render(multiview, pass, targetSize);
}
} // namespace aurora::gfx::clear
