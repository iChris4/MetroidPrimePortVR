#pragma once

#include <aurora/gfx.hpp>

#include "types.hpp"

#include <array>

#include <webgpu/webgpu_cpp.h>

namespace aurora::gfx::clear {
struct DrawData {
  PipelineRef pipeline;
  wgpu::Color color;
  float depth = 0.f;
  // The same clear in a multiview eye pass (stereo_multiview.hpp); zero outside a
  // multiview frame (recording.hpp clear_multiview_pipeline_ref).
  PipelineRef multiviewPipeline{};
  // The part of the target cleared; empty: all of it.
  ClipRect rect{};
  uint32_t rectReferenceWidth = 0;
  uint32_t rectReferenceHeight = 0;
};

inline ClipRect target_rect(const DrawData& data, const wgpu::Extent3D& target) noexcept {
  if (data.rect.width <= 0 || data.rect.height <= 0) {
    return {0, 0, static_cast<int32_t>(target.width), static_cast<int32_t>(target.height)};
  }
  if (data.rectReferenceWidth == 0 || data.rectReferenceHeight == 0) return data.rect;
  const auto left = uint64_t(data.rect.x) * target.width / data.rectReferenceWidth;
  const auto top = uint64_t(data.rect.y) * target.height / data.rectReferenceHeight;
  const auto right = (uint64_t(data.rect.x + data.rect.width) * target.width + data.rectReferenceWidth - 1) /
                     data.rectReferenceWidth;
  const auto bottom = (uint64_t(data.rect.y + data.rect.height) * target.height + data.rectReferenceHeight - 1) /
                      data.rectReferenceHeight;
  return {static_cast<int32_t>(left), static_cast<int32_t>(top), static_cast<int32_t>(right - left),
          static_cast<int32_t>(bottom - top)};
}

constexpr uint32_t ClearPipelineConfigVersion = 4;
struct PipelineConfig {
  uint64_t targetLayoutKey = 0;
  std::array<wgpu::TextureFormat, MaxColorAttachments> colorFormats{};
  wgpu::TextureFormat depthStencilFormat = wgpu::TextureFormat::Undefined;
  uint32_t version = ClearPipelineConfigVersion;
  uint32_t colorAttachmentCount = 0;
  uint32_t msaaSamples = 1;
  bool clearColor = true;
  bool clearAlpha = true;
  bool clearDepth = true;
  // Both views of a multiview eye pass (stereo_multiview.hpp); from the padding, so
  // other configs keep their bytes.
  bool multiview = false;
  std::array<uint8_t, 4> _pad{};
};
static_assert(std::has_unique_object_representations_v<PipelineConfig>);

PipelineConfig make_pipeline_config(const RenderTargetLayout& layout, bool clearColor, bool clearAlpha,
                                    bool clearDepth) noexcept;
wgpu::RenderPipeline create_pipeline(const PipelineConfig& config);
void render(const DrawData& data, const wgpu::RenderPassEncoder& pass, const wgpu::Extent3D& targetSize);
// The clear in a multiview eye pass, both eyes at once.
void render_multiview(const DrawData& data, const wgpu::RenderPassEncoder& pass, const wgpu::Extent3D& targetSize);
} // namespace aurora::gfx::clear
