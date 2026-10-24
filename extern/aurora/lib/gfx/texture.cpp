#include "resources.hpp"
#include "recording.hpp"

#include "../internal.hpp"
#include "../webgpu/gpu.hpp"
#include "aurora/aurora.h"
#include "aurora/gfx.h"
#include "texture.hpp"
#include "texture_sites.hpp"
#include "texture_convert.hpp"
#include "probe.hpp"
#include "../gx/gx_fmt.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <cstdlib>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <cstdio>
#include <vector>

#include <dawn/native/DawnNative.h>
#include <fmt/format.h>
#include <magic_enum.hpp>
#include <tracy/Tracy.hpp>
#include <webgpu/webgpu_cpp.h>

namespace aurora::gfx {
using webgpu::g_device;
using webgpu::g_queue;

namespace {
constexpr Module Log{"aurora::gfx"};

wgpu::Extent3D physical_size(wgpu::Extent3D size, TextureFormatInfo info) {
  const uint32_t width = ((size.width + info.blockWidth - 1) / info.blockWidth) * info.blockWidth;
  const uint32_t height = ((size.height + info.blockHeight - 1) / info.blockHeight) * info.blockHeight;
  return {.width = width, .height = height, .depthOrArrayLayers = size.depthOrArrayLayers};
}

std::atomic<int64_t> s_liveCount[2];
std::atomic<int64_t> s_liveBytes[2];
// Every live TextureRef, for the breakdown (aurora_get_texture_groups).
std::mutex s_liveMutex;
// Leaked: texture handles may outlive static destruction.
auto& s_liveRefs = *new std::unordered_set<const TextureRef*>;
auto& s_labels = *new std::unordered_set<std::string>;

const char* intern_label(const char* label) {
  if (label == nullptr) {
    return nullptr;
  }
  std::lock_guard lock(s_liveMutex);
  return s_labels.emplace(label).first->c_str();
}

bool setup_swizzle(wgpu::TextureComponentSwizzleDescriptor& swizzle, u32 format) noexcept {
  if (!webgpu::g_textureComponentSwizzleSupported) {
    return false;
  }

  switch (format) {
  case GX_TF_R8_PC:
    swizzle.swizzle.r = wgpu::ComponentSwizzle::R;
    swizzle.swizzle.g = wgpu::ComponentSwizzle::R;
    swizzle.swizzle.b = wgpu::ComponentSwizzle::R;
    swizzle.swizzle.a = wgpu::ComponentSwizzle::R;
    return true;
  case GX_TF_RG8_PC:
    swizzle.swizzle.r = wgpu::ComponentSwizzle::R;
    swizzle.swizzle.g = wgpu::ComponentSwizzle::R;
    swizzle.swizzle.b = wgpu::ComponentSwizzle::R;
    swizzle.swizzle.a = wgpu::ComponentSwizzle::G;
    return true;
  default:
    return false;
  }
}

wgpu::AddressMode wgpu_address_mode(GXTexWrapMode mode) {
  switch (mode) {
    DEFAULT_FATAL("invalid wrap mode {}", underlying(mode));
  case GX_CLAMP:
    return wgpu::AddressMode::ClampToEdge;
  case GX_REPEAT:
    return wgpu::AddressMode::Repeat;
  case GX_MIRROR:
    return wgpu::AddressMode::MirrorRepeat;
  }
}

std::pair<wgpu::FilterMode, wgpu::MipmapFilterMode> wgpu_filter_mode(GXTexFilter filter) {
  switch (filter) {
    DEFAULT_FATAL("invalid filter mode {}", static_cast<int>(filter));
  case GX_NEAR:
    return {wgpu::FilterMode::Nearest, wgpu::MipmapFilterMode::Undefined};
  case GX_LINEAR:
    return {wgpu::FilterMode::Linear, wgpu::MipmapFilterMode::Undefined};
  case GX_NEAR_MIP_NEAR:
    return {wgpu::FilterMode::Nearest, wgpu::MipmapFilterMode::Nearest};
  case GX_LIN_MIP_NEAR:
    return {wgpu::FilterMode::Linear, wgpu::MipmapFilterMode::Nearest};
  case GX_NEAR_MIP_LIN:
    return {wgpu::FilterMode::Nearest, wgpu::MipmapFilterMode::Linear};
  case GX_LIN_MIP_LIN:
    return {wgpu::FilterMode::Linear, wgpu::MipmapFilterMode::Linear};
  }
}

u16 wgpu_aniso(GXAnisotropy aniso) {
  switch (aniso) {
    DEFAULT_FATAL("invalid aniso {}", static_cast<int>(aniso));
  case GX_ANISO_1:
  case GX_MAX_ANISOTROPY:
    return 1;
  case GX_ANISO_2:
    return std::max<u16>(webgpu::g_graphicsConfig.textureAnisotropy / 2, 1);
  case GX_ANISO_4:
    return std::max<u16>(webgpu::g_graphicsConfig.textureAnisotropy, 1);
  }
}
} // namespace

static TextureHandle new_texture_2d(uint32_t width, uint32_t height, uint32_t mips, u32 gxFormat,
                                    wgpu::TextureFormat wgpuFormat, const char* label) noexcept;

TextureHandle new_static_texture_2d(uint32_t width, uint32_t height, uint32_t mips, u32 format, ArrayRef<uint8_t> data,
                                    bool tlut, const char* label) noexcept {
  ZoneScoped;

  ConvertedTexture converted;
  if (format != InvalidTextureFormat) {
    if (tlut) {
      CHECK(height == 1, "new_static_texture_2d[{}]: expected tlut height 1, got {}", label, height);
      CHECK(mips == 1, "new_static_texture_2d[{}]: expected tlut mipCount 1, got {}", label, mips);
      converted = convert_tlut(format, width, data);
    } else {
      converted = convert_texture(format, width, height, mips, data, true);
    }
  }
  const auto wgpuFormat = converted.data.empty() ? to_wgpu(format) : converted.format;
  auto handle = new_texture_2d(width, height, mips, format, wgpuFormat, label);
  auto& ref = *handle;
  if (!converted.data.empty()) {
    data = converted.data;
    ref.hasArbitraryMips = converted.hasArbitraryMips;
  }

  uint32_t offset = 0;
  for (uint32_t mip = 0; mip < mips; ++mip) {
    const wgpu::Extent3D mipSize{
        .width = std::max(ref.size.width >> mip, 1u),
        .height = std::max(ref.size.height >> mip, 1u),
        .depthOrArrayLayers = ref.size.depthOrArrayLayers,
    };
    const auto info = format_info(ref.format);
    const auto physicalSize = physical_size(mipSize, info);
    const uint32_t widthBlocks = physicalSize.width / info.blockWidth;
    const uint32_t heightBlocks = physicalSize.height / info.blockHeight;
    const uint32_t bytesPerRow = widthBlocks * info.blockSize;
    const uint32_t dataSize = bytesPerRow * heightBlocks * mipSize.depthOrArrayLayers;
    CHECK(offset + dataSize <= data.size(), "new_static_texture_2d[{}]: expected at least {} bytes, got {}", label,
          offset + dataSize, data.size());
    const wgpu::TexelCopyTextureInfo dstView{
        .texture = ref.texture,
        .mipLevel = mip,
    };
    if constexpr (UseTextureBuffer) {
      queue_texture_upload_data(data.data() + offset, bytesPerRow, heightBlocks, std::move(dstView), physicalSize);
    } else {
      const wgpu::TexelCopyBufferLayout dataLayout{
          .bytesPerRow = bytesPerRow,
          .rowsPerImage = heightBlocks,
      };
      g_queue.WriteTexture(&dstView, data.data() + offset, dataSize, &dataLayout, &physicalSize);
    }
    offset += dataSize;
  }
  if (!webgpu::g_hasCoreFeatures && format_info(ref.format).compressed) {
    ref.cpuBytes = std::make_shared<const std::vector<uint8_t>>(data.data(), data.data() + offset);
  }
  if (data.size() != UINT32_MAX && offset < data.size()) {
    Log.warn("new_static_texture_2d[{}]: texture used {} bytes, but given {} bytes", label, offset, data.size());
  }
  return handle;
}

void TextureRef::count_live(int sign) noexcept {
  const int kind = attachmentTextureView ? 1 : 0;
  const uint64_t bytes = format == wgpu::TextureFormat::Undefined
                             ? 0
                             : calc_texture_size(format, size.width, size.height, mipCount) *
                                   std::max<uint32_t>(size.depthOrArrayLayers, 1);
  s_liveCount[kind] += sign;
  s_liveBytes[kind] += sign * static_cast<int64_t>(bytes);
  std::lock_guard lock(s_liveMutex);
  if (sign > 0) {
    s_liveRefs.insert(this);
  } else {
    s_liveRefs.erase(this);
  }
}

TextureHandle new_dynamic_texture_2d(uint32_t width, uint32_t height, uint32_t mips, u32 gxFormat,
                                     const char* label) noexcept {
  return new_texture_2d(width, height, mips, gxFormat, to_wgpu(gxFormat), label);
}

static TextureHandle new_texture_2d(uint32_t width, uint32_t height, uint32_t mips, u32 gxFormat,
                                    wgpu::TextureFormat wgpuFormat, const char* label) noexcept {
  ZoneScopedS(3);
  const wgpu::Extent3D size{
      .width = width,
      .height = height,
      .depthOrArrayLayers = 1,
  };
  const wgpu::TextureDescriptor textureDescriptor{
      .label = label,
      .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst | wgpu::TextureUsage::CopySrc,
      .dimension = wgpu::TextureDimension::e2D,
      .size = size,
      .format = wgpuFormat,
      .mipLevelCount = mips,
      .sampleCount = 1,
  };
  auto texture = g_device.CreateTexture(&textureDescriptor);
  const auto viewLabel = fmt::format("{} view", label);
  wgpu::TextureViewDescriptor textureViewDescriptor{
      .label = viewLabel.c_str(),
      .format = wgpuFormat,
      .dimension = wgpu::TextureViewDimension::e2D,
      .mipLevelCount = mips,
  };
  wgpu::TextureComponentSwizzleDescriptor swizzle;
  if (setup_swizzle(swizzle, gxFormat)) {
    textureViewDescriptor.nextInChain = &swizzle;
  }
  auto textureView = texture.CreateView(&textureViewDescriptor);
  return std::make_shared<TextureRef>(std::move(texture), std::move(textureView), wgpu::TextureView{}, size, wgpuFormat,
                                      mips, gxFormat, intern_label(label));
}

TextureHandle new_render_texture(uint32_t width, uint32_t height, u32 gxFormat, const char* label,
                                 uint32_t mips, wgpu::TextureFormat format) noexcept {
  ZoneScoped;

  const auto wgpuFormat =
      format == wgpu::TextureFormat::Undefined ? webgpu::g_graphicsConfig.surfaceConfiguration.format : format;
  const wgpu::Extent3D size{
      .width = width,
      .height = height,
      .depthOrArrayLayers = 1,
  };
  const wgpu::TextureDescriptor textureDescriptor{
      .label = label,
      .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst | wgpu::TextureUsage::RenderAttachment |
               wgpu::TextureUsage::CopySrc,
      .dimension = wgpu::TextureDimension::e2D,
      .size = size,
      .format = wgpuFormat,
      .mipLevelCount = mips,
      .sampleCount = 1,
  };
  auto texture = g_device.CreateTexture(&textureDescriptor);

  // Create texture view for color attachments
  const auto viewLabel = fmt::format("{} view", label);
  wgpu::TextureViewDescriptor textureViewDescriptor{
      .label = viewLabel.c_str(),
      .format = wgpuFormat,
      .dimension = wgpu::TextureViewDimension::e2D,
      .mipLevelCount = 1,
  };
  auto attachmentTextureView = texture.CreateView(&textureViewDescriptor);
  wgpu::TextureView sampleTextureView = attachmentTextureView;
  if (mips > 1) {
    textureViewDescriptor.mipLevelCount = mips;
    sampleTextureView = texture.CreateView(&textureViewDescriptor);
  }
  return std::make_shared<TextureRef>(std::move(texture), std::move(sampleTextureView),
                                      std::move(attachmentTextureView), size, wgpuFormat, mips, gxFormat,
                                      intern_label(label));
}

TextureHandle new_conv_texture(uint32_t width, uint32_t height, u32 gxFormat, const char* label) noexcept {
  ZoneScoped;

  const auto wgpuFormat = to_wgpu(gxFormat);
  const wgpu::Extent3D size{
      .width = width,
      .height = height,
      .depthOrArrayLayers = 1,
  };
  const wgpu::TextureDescriptor textureDescriptor{
      .label = label,
      .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::RenderAttachment,
      .dimension = wgpu::TextureDimension::e2D,
      .size = size,
      .format = wgpuFormat,
      .mipLevelCount = 1,
      .sampleCount = 1,
  };
  auto texture = g_device.CreateTexture(&textureDescriptor);

  // Create texture view for color attachments
  const auto viewLabel = fmt::format("{} view", label);
  wgpu::TextureViewDescriptor textureViewDescriptor{
      .label = viewLabel.c_str(),
      .format = wgpuFormat,
      .dimension = wgpu::TextureViewDimension::e2D,
  };
  auto attachmentTextureView = texture.CreateView(&textureViewDescriptor);
  wgpu::TextureView sampleTextureView = attachmentTextureView;
  return std::make_shared<TextureRef>(std::move(texture), std::move(sampleTextureView),
                                      std::move(attachmentTextureView), size, wgpuFormat, 1, gxFormat,
                                      intern_label(label));
}

const wgpu::TextureView& array_texture_view(TextureRef& ref) noexcept {
  if (!ref.arrayTextureView && ref.texture) {
    wgpu::TextureViewDescriptor descriptor{
        .label = "Multiview array view",
        .format = ref.format,
        .dimension = wgpu::TextureViewDimension::e2DArray,
        .mipLevelCount = std::max(ref.mipCount, 1u),
        .arrayLayerCount = 1,
    };
    wgpu::TextureComponentSwizzleDescriptor swizzle;
    if (setup_swizzle(swizzle, ref.gxFormat)) {
      descriptor.nextInChain = &swizzle;
    }
    ref.arrayTextureView = ref.texture.CreateView(&descriptor);
  }
  return ref.arrayTextureView;
}

void write_texture(TextureRef& ref, ArrayRef<uint8_t> data) noexcept {
  ZoneScoped;

  ConvertedTexture converted;
  if (ref.gxFormat != InvalidTextureFormat) {
    converted = convert_texture(ref.gxFormat, ref.size.width, ref.size.height, ref.mipCount, data);
    ref.hasArbitraryMips = converted.hasArbitraryMips;
    if (!converted.data.empty()) {
      data = converted.data;
    }
  }

  uint32_t offset = 0;
  for (uint32_t mip = 0; mip < ref.mipCount; ++mip) {
    const wgpu::Extent3D mipSize{
        .width = std::max(ref.size.width >> mip, 1u),
        .height = std::max(ref.size.height >> mip, 1u),
        .depthOrArrayLayers = ref.size.depthOrArrayLayers,
    };
    const auto info = format_info(ref.format);
    const auto physicalSize = physical_size(mipSize, info);
    const uint32_t widthBlocks = physicalSize.width / info.blockWidth;
    const uint32_t heightBlocks = physicalSize.height / info.blockHeight;
    const uint32_t bytesPerRow = widthBlocks * info.blockSize;
    const uint32_t dataSize = bytesPerRow * heightBlocks * mipSize.depthOrArrayLayers;
    CHECK(offset + dataSize <= data.size(), "write_texture: expected at least {} bytes, got {}", offset + dataSize,
          data.size());
    const wgpu::TexelCopyTextureInfo dstView{
        .texture = ref.texture,
        .mipLevel = mip,
    };
    if constexpr (UseTextureBuffer) {
      queue_texture_upload_data(data.data() + offset, bytesPerRow, heightBlocks, std::move(dstView), physicalSize);
    } else {
      const wgpu::TexelCopyBufferLayout dataLayout{
          .bytesPerRow = bytesPerRow,
          .rowsPerImage = heightBlocks,
      };
      g_queue.WriteTexture(&dstView, data.data() + offset, dataSize, &dataLayout, &physicalSize);
    }
    offset += dataSize;
  }
  if (!webgpu::g_hasCoreFeatures && format_info(ref.format).compressed) {
    ref.cpuBytes = std::make_shared<const std::vector<uint8_t>>(data.data(), data.data() + offset);
  }
  if (data.size() != UINT32_MAX && offset < data.size()) {
    Log.warn("write_texture: texture used {} bytes, but given {} bytes", offset, data.size());
  }
}

wgpu::SamplerDescriptor TextureBind::get_descriptor() const noexcept {
  auto [minFilter, mipFilter] = wgpu_filter_mode(texObj.min_filter());
  auto [magFilter, _] = wgpu_filter_mode(texObj.mag_filter());
  const bool mipsEnabled = mipFilter != wgpu::MipmapFilterMode::Undefined;
  float minLod = texObj.min_lod();
  float maxLod = texObj.max_lod();
  u16 maxAnisotropy = wgpu_aniso(texObj.max_aniso());
  if (ref && ref->isReplacement) {
    minLod = 0.f;
    maxLod = 1000.f;
    if (!mipsEnabled) {
      mipFilter = wgpu::MipmapFilterMode::Nearest;
    }
  } else if (mipFilter == wgpu::MipmapFilterMode::Undefined) {
    minLod = 0.f;
    maxLod = 0.f;
  }
  if ((ref && ref->hasArbitraryMips) || !mipsEnabled) {
    maxAnisotropy = 1;
  } else if (maxAnisotropy > 1) {
    magFilter = wgpu::FilterMode::Linear;
    minFilter = wgpu::FilterMode::Linear;
    mipFilter = wgpu::MipmapFilterMode::Linear;
  }
  return {
      .label = "Generated Filtering Sampler",
      .addressModeU = wgpu_address_mode(texObj.wrap_s()),
      .addressModeV = wgpu_address_mode(texObj.wrap_t()),
      .addressModeW = wgpu::AddressMode::Repeat,
      .magFilter = magFilter,
      .minFilter = minFilter,
      .mipmapFilter = mipFilter,
      .lodMinClamp = minLod,
      .lodMaxClamp = maxLod,
      .maxAnisotropy = maxAnisotropy,
  };
}
} // namespace aurora::gfx

namespace aurora::gfx {
namespace {
std::mutex s_dawnMutex;
AuroraDawnMemory s_dawnMemory;
bool s_dawnSampled = false;
std::atomic<int64_t> s_dawnSampleNs = 0;
std::atomic<texture_sites::Site*> s_sites = nullptr;
std::atomic_bool s_dawnReduceRequested = false;
// Allocator slack (allocated - used) above which the recycled memory is handed back, and the slack a
// release left behind: what ReduceMemoryUsage cannot free (partly used heaps) is not retried until the
// slack grows past it by the threshold again.
constexpr uint64_t kDawnReduceSlack = 64ull << 20;
uint64_t s_dawnSlackFloor = 0;
bool s_dawnReducePending = false;

void reduce_dawn_memory(WGPUDevice device, const dawn::native::AllocatorMemoryInfo& before) {
  const bool requested = s_dawnReduceRequested.exchange(false, std::memory_order_relaxed);
  const uint64_t slack = before.totalAllocatedMemory - std::min(before.totalUsedMemory, before.totalAllocatedMemory);
  s_dawnSlackFloor = std::min(s_dawnSlackFloor, slack);
  if (!requested && !s_dawnReducePending && slack < s_dawnSlackFloor + kDawnReduceSlack) {
    return;
  }
  // Dawn's allocators keep freed GPU heaps for reuse and never return them on their own (issue #162).
  s_dawnReducePending = dawn::native::ReduceMemoryUsage(device);
  const auto after = dawn::native::GetAllocatorMemoryInfo(device);
  s_dawnSlackFloor = after.totalAllocatedMemory - std::min(after.totalUsedMemory, after.totalAllocatedMemory);
  if (!requested && after.totalAllocatedMemory == before.totalAllocatedMemory) {
    return; // a pending retry that freed nothing yet
  }
  Log.info("dawn: released recycled GPU memory ({}): allocated {} -> {} MiB, used {} MiB{}",
           requested ? "requested" : "slack", before.totalAllocatedMemory >> 20, after.totalAllocatedMemory >> 20,
           after.totalUsedMemory >> 20, s_dawnReducePending ? ", more after in-flight work" : "");
}
} // namespace

// Dawn's memory queries are not synchronized with its other calls (the device has no implicit synchronization), so
// they run here, on the thread that submits, and the game thread reads the copy.
void sample_dawn_memory() noexcept {
  const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();
  const int64_t last = s_dawnSampleNs.load(std::memory_order_relaxed);
  if ((last != 0 && now - last < 1'000'000'000) || !webgpu::g_device) {
    return;
  }
  s_dawnSampleNs.store(now, std::memory_order_relaxed);
  const WGPUDevice device = webgpu::g_device.Get();
  const auto usage = dawn::native::ComputeEstimatedMemoryUsageInfo(device);
  auto allocator = dawn::native::GetAllocatorMemoryInfo(device);
  reduce_dawn_memory(device, allocator);
  allocator = dawn::native::GetAllocatorMemoryInfo(device);
  std::lock_guard lock(s_dawnMutex);
  s_dawnMemory = {usage.totalUsage, usage.texturesUsage, usage.buffersUsage, allocator.totalUsedMemory,
                  allocator.totalAllocatedMemory};
  s_dawnSampled = true;
}

void texture_sites::note(Site& site, const wgpu::TextureDescriptor* descriptor) noexcept {
  if (!site.registered.exchange(true, std::memory_order_acq_rel)) {
    site.next = s_sites.load(std::memory_order_relaxed);
    while (!s_sites.compare_exchange_weak(site.next, &site, std::memory_order_release, std::memory_order_relaxed)) {
    }
  }
  // calc_texture_size is fatal for formats the port does not know; count the rest by their name.
  uint64_t bytes = 0;
  const std::string_view name = magic_enum::enum_name(descriptor->format);
  if (name.starts_with("BC") || name.starts_with("ASTC") || name.starts_with("ETC2") || name.starts_with("EAC")) {
    bytes = calc_texture_size(descriptor->format, descriptor->size.width, descriptor->size.height,
                              descriptor->mipLevelCount);
  } else {
    uint32_t texelBytes = 4;
    size_t channels = 0;
    while (channels < name.size() && std::string_view("RGBA").find(name[channels]) != std::string_view::npos) {
      ++channels;
    }
    if (name.starts_with("BGRA")) {
      channels = 4;
    }
    if (name.starts_with("Depth16")) {
      texelBytes = 2;
    } else if (name.starts_with("Stencil8")) {
      texelBytes = 1;
    } else if (name.starts_with("Depth32FloatStencil8")) {
      texelBytes = 8;
    } else if (channels != 0 && channels < name.size() && name[channels] >= '0' && name[channels] <= '9' &&
               !name.starts_with("RGB10") && !name.starts_with("RGB9")) {
      const uint32_t bits = static_cast<uint32_t>(std::atoi(name.data() + channels));
      texelBytes = std::max<uint32_t>(static_cast<uint32_t>(channels) * bits / 8, 1);
    }
    bytes = uint64_t{descriptor->size.width} * descriptor->size.height * texelBytes;
    if (descriptor->mipLevelCount > 1) {
      bytes += bytes / 3;
    }
  }
  bytes *= std::max<uint32_t>(descriptor->size.depthOrArrayLayers, 1) * std::max<uint32_t>(descriptor->sampleCount, 1);
  site.count.fetch_add(1, std::memory_order_relaxed);
  site.bytes.fetch_add(bytes, std::memory_order_relaxed);
}
} // namespace aurora::gfx

void aurora_get_texture_stats(AuroraTextureStats* out) {
  using namespace aurora::gfx;
  for (int kind = 0; kind < 2; ++kind) {
    out->count[kind] = static_cast<uint32_t>(std::max<int64_t>(s_liveCount[kind].load(), 0));
    out->bytes[kind] = static_cast<uint64_t>(std::max<int64_t>(s_liveBytes[kind].load(), 0));
  }
  probe::live_stats(out->count[2], out->bytes[2]);
}

void aurora_reduce_gpu_memory(void) { aurora::gfx::s_dawnReduceRequested.store(true, std::memory_order_relaxed); }

bool aurora_get_dawn_memory(AuroraDawnMemory* out) {
  std::lock_guard lock(aurora::gfx::s_dawnMutex);
  *out = aurora::gfx::s_dawnMemory;
  return aurora::gfx::s_dawnSampled;
}

uint32_t aurora_get_texture_sites(AuroraTextureSite* out, uint32_t cap) {
  using namespace aurora::gfx;
  uint32_t n = 0;
  for (const texture_sites::Site* site = s_sites.load(std::memory_order_acquire); site != nullptr; site = site->next) {
    if (n < cap) {
      const char* name = site->name;
      if (const char* lib = std::strstr(name, "lib/")) {
        name = lib + 4;
      }
      out[n] = {name, site->count.load(std::memory_order_relaxed), site->bytes.load(std::memory_order_relaxed)};
    }
    ++n;
  }
  return n;
}

uint32_t aurora_get_texture_groups(AuroraTextureGroup* out, uint32_t cap) {
  using namespace aurora::gfx;
  std::vector<AuroraTextureGroup> groups;
  {
    struct Key {
      const char* label;
      uint32_t width, height, mips, format, depth;
      bool operator==(const Key& o) const noexcept {
        return label == o.label && width == o.width && height == o.height && mips == o.mips && format == o.format &&
               depth == o.depth;
      }
    };
    struct KeyHash {
      size_t operator()(const Key& k) const noexcept {
        size_t h = std::hash<const void*>()(k.label);
        for (const uint32_t v : {k.width, k.height, k.mips, k.format, k.depth}) {
          h = h * 1000003u ^ v;
        }
        return h;
      }
    };
    std::unordered_map<Key, size_t, KeyHash> index;
    std::lock_guard lock(s_liveMutex);
    for (const TextureRef* ref : s_liveRefs) {
      const Key key{ref->label, ref->size.width, ref->size.height, ref->mipCount,
                    static_cast<uint32_t>(ref->format), ref->size.depthOrArrayLayers};
      const auto [it, added] = index.try_emplace(key, groups.size());
      if (added) {
        AuroraTextureGroup g{};
        g.label = ref->label != nullptr ? ref->label : "(none)";
        const auto name = magic_enum::enum_name(ref->format);
        std::snprintf(g.format, sizeof(g.format), "%.*s", static_cast<int>(name.size()), name.data());
        g.width = ref->size.width;
        g.height = ref->size.height;
        g.mips = ref->mipCount;
        groups.push_back(g);
      }
      AuroraTextureGroup& g = groups[it->second];
      ++g.count;
      g.bytes += ref->format == wgpu::TextureFormat::Undefined
                     ? 0
                     : calc_texture_size(ref->format, ref->size.width, ref->size.height, ref->mipCount) *
                           std::max<uint32_t>(ref->size.depthOrArrayLayers, 1);
    }
  }
  std::sort(groups.begin(), groups.end(), [](const AuroraTextureGroup& a, const AuroraTextureGroup& b) {
    return a.count != b.count ? a.count > b.count : a.bytes > b.bytes;
  });
  const uint32_t n = std::min<uint32_t>(cap, static_cast<uint32_t>(groups.size()));
  std::copy_n(groups.begin(), n, out);
  return static_cast<uint32_t>(groups.size());
}
