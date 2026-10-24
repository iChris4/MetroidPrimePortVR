#include "actor_shadow.hpp"

#include "../gx/pipeline.hpp"
#include "../internal.hpp"
#include "../logging.hpp"
#include "../webgpu/gpu.hpp"
#include "../webgpu/gpu_prof.hpp"
#include "encoding.hpp"
#include "pipeline_cache.hpp"
#include "resource_cache.hpp"
#include "recording.hpp"
#include "resources.hpp"

#include <aurora/gfx.hpp>

#include <array>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "texture_sites.hpp"

// The actors' shadow maps. The casters (an actor's opaque draws, re-drawn depth only with a vs_shadow
// pipeline whose caster matrix is the actor's own view -> map clip) are recorded per layer and drawn
// when the frame's actor phase ends, ahead of the decal draws that read them in the same frame.
namespace aurora::gfx::actor_shadow {
namespace {
Module Log("aurora::gfx::actor_shadow");
using webgpu::g_device;

struct State {
  EncoderTaskId task = InvalidEncoderTask;
  wgpu::Texture map;
  wgpu::TextureView mapView;
  std::array<wgpu::TextureView, MaxLayers> layerViews;
  wgpu::Sampler sampler;
};

State g_state;
std::array<std::vector<gx::DrawData>, MaxLayers> g_casters;
std::array<std::array<std::vector<gx::DrawData>, MaxLayers>, 32> g_recorded;
uint32_t g_nextSlot = 0;

void ensure_map() {
  if (g_state.map) {
    return;
  }
  const wgpu::TextureDescriptor descriptor{
      .label = "Actor Shadow Map",
      .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding,
      .dimension = wgpu::TextureDimension::e2D,
      .size = {MapSize, MapSize, MaxLayers},
      .format = MapFormat,
  };
  AURORA_NOTE_TEXTURE(&descriptor);
  g_state.map = g_device.CreateTexture(&descriptor);
  const wgpu::TextureViewDescriptor arrayView{
      .label = "Actor Shadow Map",
      .dimension = wgpu::TextureViewDimension::e2DArray,
      .arrayLayerCount = MaxLayers,
  };
  g_state.mapView = g_state.map.CreateView(&arrayView);
  for (uint32_t l = 0; l < MaxLayers; ++l) {
    const wgpu::TextureViewDescriptor layerView{
        .label = "Actor Shadow Layer",
        .dimension = wgpu::TextureViewDimension::e2D,
        .baseArrayLayer = l,
        .arrayLayerCount = 1,
    };
    g_state.layerViews[l] = g_state.map.CreateView(&layerView);
  }
}

void encode(const EncoderTaskContext&, const wgpu::CommandEncoder& cmd, const void* payload, size_t payloadSize,
            void*) {
  uint32_t slot = 0;
  if (payloadSize != sizeof(slot) || !g_state.mapView) {
    return;
  }
  std::memcpy(&slot, payload, sizeof(slot));
  const auto& layers = g_recorded[slot % g_recorded.size()];
  const auto& resources = detail::resources();
  for (uint32_t l = 0; l < MaxLayers; ++l) {
    const auto& casters = layers[l];
    if (casters.empty()) {
      continue;
    }
    // A depth map cleared to 1 (nothing between the actor and its receivers) for every layer used.
    const wgpu::RenderPassDepthStencilAttachment depth{
        .view = g_state.layerViews[l],
        .depthLoadOp = wgpu::LoadOp::Clear,
        .depthStoreOp = wgpu::StoreOp::Store,
        .depthClearValue = 1.f,
    };
    const wgpu::RenderPassDescriptor passDescriptor{
        .label = "Actor Shadow Map",
        .depthStencilAttachment = &depth,
        .timestampWrites = webgpu::gpu_prof::pass_writes("Actor shadow map"),
    };
    const auto pass = cmd.BeginRenderPass(&passDescriptor);
    pass.SetBindGroup(0, resources.staticBindGroup);
    for (const auto& draw : casters) {
      wgpu::RenderPipeline pipeline;
      if (!get_pipeline(draw.pipeline, pipeline)) {
        continue;
      }
      auto immediates = draw.immediateData;
      immediates.serial = 0; // the draw's own uniform carries the actor's matrix in caster[0]
      pass.SetPipeline(pipeline);
      if (draw.bindGroups.textureBindGroup) {
        pass.SetBindGroup(2, find_bind_group(draw.bindGroups.textureBindGroup));
      }
      pass.SetImmediates(0, &immediates, sizeof(immediates));
      pass.SetBindGroup(1, resources.uniformBindGroup, 1, &draw.uniformRange.offset);
      pass.SetIndexBuffer(resources.indexBuffer, wgpu::IndexFormat::Uint16, draw.idxRange.offset, draw.idxRange.size);
      if (draw.indexCount == 0) {
        pass.Draw(draw.vtxCount, draw.instanceCount);
      } else {
        pass.DrawIndexed(draw.indexCount, draw.instanceCount);
      }
    }
    pass.End();
  }
  forget_texture_group();
}
} // namespace

bool ensure_task() {
  if (g_state.task == InvalidEncoderTask) {
    g_state.task = register_encoder_task_type(EncoderTaskDescriptor{.label = "Actor Shadow Map", .callback = encode});
    if (g_state.task == InvalidEncoderTask) {
      Log.warn("could not register the actor shadow map task");
      return false;
    }
  }
  return true;
}

void add_caster(uint32_t layer, const gx::DrawData& draw) {
  if (layer < MaxLayers) {
    g_casters[layer].push_back(draw);
  }
}

bool record() {
  bool any = false;
  for (const auto& layer : g_casters) {
    any = any || !layer.empty();
  }
  if (!any) {
    return false;
  }
  if (g_state.task == InvalidEncoderTask) {
    for (auto& layer : g_casters) {
      layer.clear();
    }
    return false;
  }
  ensure_map();
  static const bool shadowDebug = env_flag("MP_ACTOR_SHADOW_DEBUG");
  if (shadowDebug) {
    for (uint32_t l = 0; l < MaxLayers; ++l) {
      if (!g_casters[l].empty()) {
        Log.info("actor shadow record: layer {} casters {}", l, g_casters[l].size());
      }
    }
  }
  const uint32_t slot = g_nextSlot % g_recorded.size();
  for (uint32_t l = 0; l < MaxLayers; ++l) {
    g_recorded[slot][l].swap(g_casters[l]);
    g_casters[l].clear();
  }
  if (!record_encoder_task(g_state.task, &slot, sizeof(slot))) {
    for (auto& layer : g_recorded[slot]) {
      layer.clear();
    }
    return false;
  }
  ++g_nextSlot;
  return true;
}

const wgpu::TextureView& map_view() {
  ensure_map();
  return g_state.mapView;
}

// Remastered's NShadowMap::skSamplerParams (KB topic/simple-shadow.md line 41; REPORT.md "Shadow-map sampler"):
// bilinear, compare ON (LEQUAL), wrap U/V NVN mode 5, border white. Linear min/mag with a compare sampler is the
// hardware 2x2 compare filter. Mode 5 is CLAMP_TO_BORDER (the same table row the BLPD/LTPB KB entries read), which
// WebGPU has no address mode for: the decal shader returns vis = 1 (the white border) for any uv outside [0,1], so
// the clamp only ever serves the border texel row and ClampToEdge is the equivalent.
const wgpu::Sampler& sampler() {
  if (!g_state.sampler) {
    const wgpu::SamplerDescriptor descriptor{
        .label = "Actor Shadow Map Sampler",
        .addressModeU = wgpu::AddressMode::ClampToEdge,
        .addressModeV = wgpu::AddressMode::ClampToEdge,
        .addressModeW = wgpu::AddressMode::ClampToEdge,
        .magFilter = wgpu::FilterMode::Linear,
        .minFilter = wgpu::FilterMode::Linear,
        .compare = wgpu::CompareFunction::LessEqual,
    };
    g_state.sampler = g_device.CreateSampler(&descriptor);
  }
  return g_state.sampler;
}

void shutdown() {
  const auto task = g_state.task;
  g_state = {};
  g_casters = {};
  g_recorded = {};
  if (task != InvalidEncoderTask) {
    unregister_encoder_task_type(task);
  }
}
} // namespace aurora::gfx::actor_shadow
