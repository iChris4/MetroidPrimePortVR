#pragma once

#include "pipeline_cache.hpp"
#include "resources.hpp"
#include "types.hpp"
#include "tex_copy_conv.hpp"
#include "tex_palette_conv.hpp"
#include "stereo_frame.hpp"
#include "texture.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace aurora::gfx::detail {

// A block of resolved vertex records for the geometry cache (gx/geometry_cache.hpp):
// `size` bytes at `src` of this frame's vertex staging, copied to `dst` of the geometry
// buffer (geometry_buffer.hpp) before the frame's next op.
struct GeometryUpload {
  uint32_t src = 0;
  uint32_t dst = 0;
  uint32_t size = 0;
};

struct StagingHighWater {
  uint32_t verts = 0;
  uint32_t uniforms = 0;
  uint32_t indices = 0;
  uint32_t storage = 0;
  uint32_t textureUpload = 0;
  size_t textureUploadCount = 0;
  size_t geometryUploadCount = 0;
  size_t bufferUploadCount = 0;
};

// A copy into a shared buffer's resident region (queue_resident_upload).
struct BufferUpload {
  wgpu::Buffer src;
  // Which buffer is looked up when it is encoded, as a frame can grow it (grow_frame_buffer).
  ResidentBuffer dst = ResidentBuffer::Vertex;
  uint64_t dstOffset = 0;
  uint64_t size = 0;
};

struct CustomDrawCommand {
  DrawTypeId type = 0;
  uint32_t payloadSize = 0;
  alignas(std::max_align_t) std::array<std::byte, InlineDrawPayloadSize> payload{};
};

struct RenderPass;
using DrawEncoder = void (*)(void* payload, const wgpu::RenderPassEncoder& pass, const RenderPass& passInfo);

// What a draw's payload is, for the encoders that treat some draws specially
// (the stereo eye passes re-issue GX and clear draws and leave the rest mono).
enum class DrawKind : uint8_t {
  Other,
  GX,
  Clear,
};

struct DrawCommand {
  DrawEncoder encoder = nullptr;
  DrawKind kind = DrawKind::Other;
  alignas(std::max_align_t) std::array<std::byte, InlineDrawPayloadSize> payload{};
};

enum class CommandType {
  SetViewport,
  SetScissor,
  Draw,
  CustomDraw,
  DebugMarker,
};

struct Command {
  CommandType type;
#ifdef AURORA_GFX_DEBUG_GROUPS
  std::vector<std::string> debugGroupStack;
#endif
  union Data {
    Viewport setViewport;
    ClipRect setScissor;
    DrawCommand draw;
    CustomDrawCommand customDraw;
    size_t debugMarkerIndex;
  } data;
};

using CommandList = std::vector<Command>;

struct RenderPass {
  struct ColorAttachment {
    ColorAttachmentSemantic semantic = ColorAttachmentSemantic::Auxiliary;
    wgpu::TextureFormat format = wgpu::TextureFormat::Undefined;
    wgpu::Extent3D size;
    wgpu::TextureView view;
    wgpu::TextureView resolveView;
    Vec4<float> clearValue{0.f, 0.f, 0.f, 0.f};
    wgpu::LoadOp loadOp = wgpu::LoadOp::Undefined;
    wgpu::StoreOp storeOp = wgpu::StoreOp::Store;
    bool clear = true;
  };

  std::string label;
  std::array<ColorAttachment, MaxColorAttachments> colorAttachments;
  uint32_t colorAttachmentCount = 0;
  wgpu::TextureView depthStencilView;
  wgpu::TextureFormat depthStencilFormat = wgpu::TextureFormat::Undefined;
  wgpu::Texture copySourceTexture;
  wgpu::TextureView copySourceView;
  wgpu::TextureView copySourceDepthView;
  uint32_t msaaSamples = 1;

  TextureHandle resolveTarget;
  GXTexFmt resolveFormat = GX_TF_RGBA8;
  ClipRect resolveRect;
  Range resolveUniformRange;
  Range resolveMipsUniformRange;
  bool resolveMips = false; // resolveTarget has a mip chain to fill after the copy
  // What the colour copy makes of the source (an HDR scene pass's copies are not Display) and,
  // for HdrDisplay, the tone curve it was recorded with.
  tex_copy_conv::CopyDomain resolveDomain = tex_copy_conv::CopyDomain::Display;
  Range resolveToneRange;
  int probeFace = -1; // resolveTarget is this face of the PBR probe
  Range probeUniformRange;
  wgpu::Texture snapshotColorDst;
  wgpu::TextureView snapshotDepthDst;
  float clearDepthValue = 1.f;
  wgpu::LoadOp depthLoadOp = wgpu::LoadOp::Undefined;
  wgpu::StoreOp depthStoreOp = wgpu::StoreOp::Store;
  wgpu::LoadOp stencilLoadOp = wgpu::LoadOp::Undefined;
  wgpu::StoreOp stencilStoreOp = wgpu::StoreOp::Undefined;
  uint32_t stencilClearValue = 0;
  CommandList commands;
  bool clearDepth = true;
  bool hasDepth = true;
  bool hasStencil = false;
  bool hasDraws = false;
  bool discardable = false;
  bool captureDepthSnapshot = false;
  bool sealed = false;
  // The frame's EFB (scene) target: the passes the stereo replay re-encodes per eye.
  bool efb = false;
  StereoPassReplay stereo;
  std::vector<tex_palette_conv::ConvRequest> paletteConvs;

  RenderTargetLayout target_layout() const noexcept;
  bool has_consumer() const { return resolveTarget || snapshotColorDst || snapshotDepthDst; }
  bool has_content() const {
    if (hasDraws || clearDepth) {
      return true;
    }
    for (uint32_t i = 0; i < colorAttachmentCount; ++i) {
      if (colorAttachments[i].clear) {
        return true;
      }
    }
    return false;
  }
};

inline void finalize_render_target_layout(RenderTargetLayout& layout) noexcept {
  Hasher hasher{};
  hasher.update(layout.colorAttachmentCount);
  for (uint32_t i = 0; i < layout.colorAttachmentCount; ++i) {
    hasher.update(layout.colorAttachments[i].semantic);
    hasher.update(layout.colorAttachments[i].format);
  }
  hasher.update(static_cast<uint32_t>(layout.depthStencilFormat));
  hasher.update(layout.sampleCount);
  layout.key = hasher.digest();
}

inline RenderTargetLayout RenderPass::target_layout() const noexcept {
  RenderTargetLayout layout{
      .colorAttachmentCount = colorAttachmentCount,
      .depthStencilFormat = hasDepth || hasStencil ? depthStencilFormat : wgpu::TextureFormat::Undefined,
      .sampleCount = msaaSamples,
  };
  for (uint32_t i = 0; i < colorAttachmentCount; ++i) {
    layout.colorAttachments[i] = {
        .semantic = colorAttachments[i].semantic,
        .format = colorAttachments[i].format,
        .width = colorAttachments[i].size.width,
        .height = colorAttachments[i].size.height,
    };
  }
  finalize_render_target_layout(layout);
  return layout;
}

struct TextureCopy {
  wgpu::TexelCopyTextureInfo src;
  wgpu::TexelCopyTextureInfo dst;
  wgpu::Extent3D size;
};

struct EncoderTask {
  EncoderTaskId type = InvalidEncoderTask;
  std::array<uint8_t, InlineDrawPayloadSize> payload{};
  uint32_t payloadSize = 0;
};

enum class FrameOpType : uint8_t {
  RenderPass,
  TextureCopy,
  EncoderTask,
};

struct FrameOp {
  FrameOpType type = FrameOpType::RenderPass;
  uint32_t index = 0;
  RenderPass* renderPass = nullptr;
  TextureCopy* textureCopy = nullptr;
  EncoderTask* encoderTask = nullptr;
  StagingHighWater highWater;
  std::vector<const TextureUpload*> textureUploads;
  std::vector<const GeometryUpload*> geometryUploads;
  std::vector<const BufferUpload*> bufferUploads;
};

using RenderPassList = std::deque<RenderPass>;

// Where a frame's vertex, uniform, index or storage data goes once its staging buffer is full:
// memory allocated at the first overflow (address-space only until written), laid out after
// the staging part so offsets stay contiguous, and copied to the device buffer through a
// transient buffer. The ByteBuffer then reads as full (size() == capacity()).
struct SpillBuffer {
  std::unique_ptr<uint8_t[]> data;
  size_t capacity = 0;
  size_t length = 0;
};

// Bytes a frame has written to a kind of buffer, staging and spill together.
inline size_t logical_size(const ByteBuffer& staging, const SpillBuffer& spill) noexcept {
  return staging.size() + spill.length;
}

struct FramePacket {
  RenderPassList renderPasses;
  std::deque<TextureCopy> textureCopies;
  std::deque<EncoderTask> encoderTasks;
  std::deque<FrameOp> ops;
  std::deque<TextureUpload> textureUploads;
  std::deque<GeometryUpload> geometryUploads;
  std::deque<BufferUpload> bufferUploads;
#ifdef AURORA_GFX_DEBUG_GROUPS
  std::vector<std::string> debugMarkers;
#endif
  ByteBuffer verts;
  ByteBuffer uniforms;
  ByteBuffer indices;
  ByteBuffer storage;
  ByteBuffer textureUpload;
  SpillBuffer spillVerts;
  SpillBuffer spillUniforms;
  SpillBuffer spillIndices;
  SpillBuffer spillStorage;
  wgpu::CommandEncoder encoder;
  std::vector<AfterSubmitCallback> afterSubmitCallbacks;
  uint64_t frameId = 0;
  uint32_t frameIndex = 0;
  size_t stagingBuffer = 0;
  StagingHighWater copied;
  AuroraStats stats{};
  StereoFrameState stereo;
};

} // namespace aurora::gfx::detail
