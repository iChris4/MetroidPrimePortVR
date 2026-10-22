// SPDX-License-Identifier: GPL-3.0-or-later
#include <aurora/vulkan_direct_interop.h>

#include "../internal.hpp"
#include "../stereo.hpp"
#include "../stereo_overlay.hpp"
#include "gpu.hpp"

// PrimedGun's patched Dawn (quest/dawn) adds both Vulkan multiview and the entry points this
// bridge calls (quest/dawn/aurora_vulkan_interop.inc); a stock Dawn gets the stubs below.
#if defined(__ANDROID__) && defined(WEBGPU_DAWN) && defined(WGPU_DAWN_RENDER_PASS_MULTIVIEW_INIT)

#include <vulkan/vulkan_core.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <vector>

extern "C" {
uint32_t AuroraDawnVulkanVersion();
int AuroraDawnVulkanConfigure(const AuroraDawnVulkanHooks* hooks);
int AuroraDawnVulkanGetHandles(void* device, AuroraDawnVulkanHandles* out);
void* AuroraDawnVulkanWrap(void* device, const void* textureDescriptor, uint64_t image);
int AuroraDawnVulkanRelease(void* device, void* const* textures, uint32_t count);
void* AuroraDawnVulkanLock(void* device);
void AuroraDawnVulkanUnlock(void* guard);
int AuroraDawnVulkanDrain(void* device);
}

namespace aurora::vulkan_direct {
namespace {

Module Log("aurora::vulkan_direct");

int64_t to_vk_format(wgpu::TextureFormat format) noexcept {
  switch (format) {
  case wgpu::TextureFormat::RGBA8Unorm:
    return VK_FORMAT_R8G8B8A8_UNORM;
  case wgpu::TextureFormat::RGBA8UnormSrgb:
    return VK_FORMAT_R8G8B8A8_SRGB;
  case wgpu::TextureFormat::BGRA8Unorm:
    return VK_FORMAT_B8G8R8A8_UNORM;
  case wgpu::TextureFormat::BGRA8UnormSrgb:
    return VK_FORMAT_B8G8R8A8_SRGB;
  case wgpu::TextureFormat::RGBA16Float:
    return VK_FORMAT_R16G16B16A16_SFLOAT;
  default:
    return VK_FORMAT_UNDEFINED;
  }
}

// CopyTextureToTexture needs the two formats to match modulo sRGB encoding (the swapchain is
// declared sRGB so the compositor decodes Aurora's gamma-encoded bytes).
int copy_family(int64_t format) noexcept {
  switch (format) {
  case VK_FORMAT_R8G8B8A8_UNORM:
  case VK_FORMAT_R8G8B8A8_SRGB:
    return 1;
  case VK_FORMAT_B8G8R8A8_UNORM:
  case VK_FORMAT_B8G8R8A8_SRGB:
    return 2;
  case VK_FORMAT_R16G16B16A16_SFLOAT:
    return 3;
  default:
    return 0;
  }
}

bool same_copy_family(int64_t left, int64_t right) noexcept {
  const int family = copy_family(left);
  return family != 0 && family == copy_family(right);
}

wgpu::TextureFormat to_wgpu_format(int64_t format) noexcept {
  switch (format) {
  case VK_FORMAT_R8G8B8A8_UNORM:
    return wgpu::TextureFormat::RGBA8Unorm;
  case VK_FORMAT_R8G8B8A8_SRGB:
    return wgpu::TextureFormat::RGBA8UnormSrgb;
  case VK_FORMAT_B8G8R8A8_UNORM:
    return wgpu::TextureFormat::BGRA8Unorm;
  case VK_FORMAT_B8G8R8A8_SRGB:
    return wgpu::TextureFormat::BGRA8UnormSrgb;
  case VK_FORMAT_R16G16B16A16_SFLOAT:
    return wgpu::TextureFormat::RGBA16Float;
  default:
    return wgpu::TextureFormat::Undefined;
  }
}

// Draws an eye into a swapchain image: a triangle covering the target, each pixel loaded from the
// eye. On the Quest's Adreno a transfer copy into the runtime's images took about 2.6 ms a frame
// for both eyes, the draw about 0.4 ms.
class Blitter final {
public:
  bool Encode(wgpu::CommandEncoder& encoder, const stereo::EyeImage& source, const wgpu::TextureView& target,
              wgpu::TextureFormat format, uint32_t width, uint32_t height) noexcept {
    if (source.view == nullptr || !*source.view || !Initialize()) {
      return false;
    }
    const wgpu::RenderPipeline& pipeline = PipelineFor(format);
    if (!pipeline) {
      return false;
    }
    const wgpu::BindGroupEntry entry{.binding = 0, .textureView = *source.view};
    const wgpu::BindGroupDescriptor bindGroupDescriptor{
        .label = "OpenXR eye blit",
        .layout = m_layout,
        .entryCount = 1,
        .entries = &entry,
    };
    const wgpu::BindGroup bindGroup = webgpu::g_device.CreateBindGroup(&bindGroupDescriptor);
    const wgpu::RenderPassColorAttachment attachment{
        .view = target,
        .loadOp = wgpu::LoadOp::Clear,
        .storeOp = wgpu::StoreOp::Store,
        .clearValue = {0.0, 0.0, 0.0, 0.0},
    };
    const wgpu::RenderPassDescriptor passDescriptor{
        .label = "OpenXR eye blit",
        .colorAttachmentCount = 1,
        .colorAttachments = &attachment,
    };
    const auto pass = encoder.BeginRenderPass(&passDescriptor);
    pass.SetViewport(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f);
    pass.SetScissorRect(0, 0, width, height);
    pass.SetPipeline(pipeline);
    pass.SetBindGroup(0, bindGroup);
    pass.Draw(3);
    pass.End();
    return true;
  }

private:
  bool Initialize() noexcept {
    if (m_layout) {
      return true;
    }
    static constexpr char kShader[] = R"(
@group(0) @binding(0) var eye: texture_2d<f32>;

@vertex
fn vs_main(@builtin(vertex_index) index: u32) -> @builtin(position) vec4f {
    let uv = vec2f(f32((index << 1u) & 2u), f32(index & 2u));
    return vec4f(uv * vec2f(2.0, -2.0) + vec2f(-1.0, 1.0), 0.0, 1.0);
}

@fragment
fn fs_raw(@builtin(position) position: vec4f) -> @location(0) vec4f {
    return textureLoad(eye, vec2i(position.xy), 0);
}

// The eyes hold gamma-encoded bytes and an sRGB attachment encodes what it is given: decode first.
@fragment
fn fs_srgb(@builtin(position) position: vec4f) -> @location(0) vec4f {
    let color = textureLoad(eye, vec2i(position.xy), 0);
    let low = color.rgb / 12.92;
    let high = pow((color.rgb + 0.055) / 1.055, vec3f(2.4));
    return vec4f(select(high, low, color.rgb <= vec3f(0.04045)), color.a);
}
)";
    wgpu::ShaderSourceWGSL wgsl{};
    wgsl.code = kShader;
    const wgpu::ShaderModuleDescriptor moduleDescriptor{.nextInChain = &wgsl, .label = "OpenXR eye blit"};
    m_module = webgpu::g_device.CreateShaderModule(&moduleDescriptor);
    const wgpu::BindGroupLayoutEntry entry{
        .binding = 0,
        .visibility = wgpu::ShaderStage::Fragment,
        .texture =
            {
                .sampleType = wgpu::TextureSampleType::UnfilterableFloat,
                .viewDimension = wgpu::TextureViewDimension::e2D,
            },
    };
    const wgpu::BindGroupLayoutDescriptor layoutDescriptor{
        .label = "OpenXR eye blit",
        .entryCount = 1,
        .entries = &entry,
    };
    m_layout = webgpu::g_device.CreateBindGroupLayout(&layoutDescriptor);
    const wgpu::PipelineLayoutDescriptor pipelineLayoutDescriptor{
        .label = "OpenXR eye blit",
        .bindGroupLayoutCount = 1,
        .bindGroupLayouts = &m_layout,
    };
    m_pipelineLayout = webgpu::g_device.CreatePipelineLayout(&pipelineLayoutDescriptor);
    return m_module && m_layout && m_pipelineLayout;
  }

  const wgpu::RenderPipeline& PipelineFor(wgpu::TextureFormat format) noexcept {
    for (const auto& [cached, pipeline] : m_pipelines) {
      if (cached == format) {
        return pipeline;
      }
    }
    const bool srgb =
        format == wgpu::TextureFormat::RGBA8UnormSrgb || format == wgpu::TextureFormat::BGRA8UnormSrgb;
    const wgpu::ColorTargetState target{.format = format};
    const wgpu::FragmentState fragment{
        .module = m_module,
        .entryPoint = srgb ? "fs_srgb" : "fs_raw",
        .targetCount = 1,
        .targets = &target,
    };
    const wgpu::RenderPipelineDescriptor descriptor{
        .label = "OpenXR eye blit",
        .layout = m_pipelineLayout,
        .vertex = {.module = m_module, .entryPoint = "vs_main"},
        .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList},
        .multisample = {.count = 1},
        .fragment = &fragment,
    };
    m_pipelines.emplace_back(format, webgpu::g_device.CreateRenderPipeline(&descriptor));
    return m_pipelines.back().second;
  }

  wgpu::ShaderModule m_module;
  wgpu::BindGroupLayout m_layout;
  wgpu::PipelineLayout m_pipelineLayout;
  std::vector<std::pair<wgpu::TextureFormat, wgpu::RenderPipeline>> m_pipelines;
};

bool abi_matches() noexcept {
  static const bool matches = AuroraDawnVulkanVersion() == AURORA_DAWN_VULKAN_ABI;
  return matches;
}

// A swapchain image wrapped as a Dawn texture of its own format, kept while its swapchain lives.
struct Wrap {
  uint64_t image = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  wgpu::TextureFormat format = wgpu::TextureFormat::Undefined;
  wgpu::Texture texture;
  wgpu::TextureView view;
};

// The eyes, then the settings panel's layer image after them.
constexpr uint32_t kMaxImages = AURORA_STEREO_EYE_COUNT + 1;

class Bridge final {
public:
  Bridge(AuroraVulkanDirectSubmittedCallback callback, void* userdata) noexcept
  : m_callback(callback), m_userdata(userdata) {}

  bool SetTargets(uint64_t token, const AuroraVulkanDirectTarget* targets, uint32_t count,
                  const AuroraVulkanDirectTarget* panel) noexcept {
    if (token == 0 || targets == nullptr || count == 0 || count > AURORA_STEREO_EYE_COUNT) {
      return false;
    }
    const auto valid = [](const AuroraVulkanDirectTarget& target) {
      return target.image != 0 && target.width != 0 && target.height != 0;
    };
    for (uint32_t i = 0; i < count; ++i) {
      if (!valid(targets[i])) {
        return false;
      }
    }
    if (panel != nullptr && !valid(*panel)) {
      return false;
    }
    std::lock_guard lock(m_mutex);
    if (m_token != 0) {
      return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
      m_targets[i] = targets[i];
    }
    m_panel = panel != nullptr;
    if (m_panel) {
      m_targets[count] = *panel;
    }
    m_count = count;
    m_images = count + (m_panel ? 1u : 0u);
    m_encoded = false;
    m_token = token;
    m_targetsSet.notify_all();
    return true;
  }

  // Only while the worker is not encoding this token (it holds the lock then) and has not.
  bool Cancel(uint64_t token) noexcept {
    std::unique_lock lock(m_mutex, std::try_to_lock);
    if (!lock.owns_lock() || m_encoded || m_token == 0 || m_token != token) {
      return false;
    }
    m_token = 0;
    m_cancelled = token;
    return true;
  }

  bool Forget(const uint64_t* images, uint32_t count) noexcept {
    {
      std::lock_guard lock(m_mutex);
      if (m_token != 0) {
        return false;
      }
    }
    // Every copy into the images retires before their textures go.
    if (!webgpu::g_device || AuroraDawnVulkanDrain(webgpu::g_device.Get()) == 0) {
      return false;
    }
    std::lock_guard lock(m_mutex);
    std::erase_if(m_wraps, [&](const Wrap& wrap) { return std::find(images, images + count, wrap.image) != images + count; });
    m_active = {};
    m_activeViews = {};
    return true;
  }

  void Clear() noexcept {
    std::lock_guard lock(m_mutex);
    m_token = 0;
    m_cancelled = 0;
    m_encoded = false;
    m_active = {};
    m_activeViews = {};
    m_wraps.clear();
  }

  bool Encode(wgpu::CommandEncoder& encoder, const stereo::SinkFrame& frame) noexcept {
    std::unique_lock lock(m_mutex);
    if (m_token == 0 && frame.frameToken != m_cancelled) {
      // Pipelined pacing sets a packet's targets once the previous packet's eyes were
      // submitted, which can be moments before its own encode reaches this point. Any
      // targets end the wait: another packet's mean this one's were withdrawn.
      m_targetsSet.wait_for(lock, std::chrono::milliseconds(20), [&] { return m_token != 0; });
    }
    if (m_token == 0 || m_encoded || frame.frameToken != m_token) {
      return false;
    }
    std::array<stereo::EyeImage, kMaxImages> sources{};
    for (uint32_t i = 0; i < m_count; ++i) {
      sources[i] = frame.eyes[i];
    }
    if (m_panel && !stereo_overlay::layer_source(encoder, m_targets[m_count].width, m_targets[m_count].height,
                                                 sources[m_count])) {
      return Decline("the settings panel's layer image is unavailable");
    }
    // Every target is wrapped before any copy is recorded.
    for (uint32_t i = 0; i < m_images; ++i) {
      const auto& source = sources[i];
      const auto& target = m_targets[i];
      if (source.texture == nullptr || !*source.texture) {
        return Decline("an eye image is missing");
      }
      if (!same_copy_family(to_vk_format(source.format), target.vkFormat)) {
        return Decline("an eye's format cannot be copied into its swapchain image");
      }
      const wgpu::TextureFormat format = to_wgpu_format(target.vkFormat);
      auto it = std::find_if(m_wraps.begin(), m_wraps.end(),
                             [&](const Wrap& wrap) { return wrap.image == target.image; });
      if (it != m_wraps.end() && (it->width != target.width || it->height != target.height || it->format != format)) {
        // The runtime reuses VkImage handles across swapchain recreation: wrap the image again.
        m_wraps.erase(it);
        it = m_wraps.end();
      }
      if (it == m_wraps.end()) {
        wgpu::TextureDescriptor descriptor{};
        descriptor.label = "OpenXR swapchain image";
        descriptor.usage = wgpu::TextureUsage::CopyDst | wgpu::TextureUsage::RenderAttachment;
        descriptor.size = {target.width, target.height, 1};
        descriptor.format = format;
        void* wrapped = AuroraDawnVulkanWrap(webgpu::g_device.Get(), &descriptor, target.image);
        if (wrapped == nullptr) {
          return Decline("Dawn could not wrap a swapchain image");
        }
        Wrap wrap{target.image, target.width, target.height, format,
                  wgpu::Texture::Acquire(static_cast<WGPUTexture>(wrapped)), {}};
        wrap.view = wrap.texture.CreateView();
        m_wraps.push_back(std::move(wrap));
        it = m_wraps.end() - 1;
      }
      m_active[i] = it->texture;
      m_activeViews[i] = it->view;
    }
    for (uint32_t i = 0; i < m_images; ++i) {
      const auto& source = sources[i];
      // An eye rendered smaller than its image (the immersive window's) fills the top-left part
      // the layer shows; one rendered larger while the resolution changes gives what fits.
      const uint32_t width = std::min(source.size.width, m_targets[i].width);
      const uint32_t height = std::min(source.size.height, m_targets[i].height);
      if (m_blitter.Encode(encoder, source, m_activeViews[i], to_wgpu_format(m_targets[i].vkFormat), width,
                           height)) {
        continue;
      }
      // The blit could not be set up: copy instead.
      const wgpu::TexelCopyTextureInfo from{
          .texture = *source.texture,
          .mipLevel = 0,
          // Multiview eye targets hold both eyes as layers of one texture.
          .origin = {0, 0, source.layer},
          .aspect = wgpu::TextureAspect::All,
      };
      const wgpu::TexelCopyTextureInfo to{
          .texture = m_active[i],
          .mipLevel = 0,
          .origin = {},
          .aspect = wgpu::TextureAspect::All,
      };
      const wgpu::Extent3D extent{width, height, 1};
      encoder.CopyTextureToTexture(&from, &to, &extent);
    }
    m_encoded = true;
    return true;
  }

  void Submitted(const stereo::SinkFrame& frame) noexcept {
    std::lock_guard lock(m_mutex);
    if (m_token == 0 || !m_encoded || m_token != frame.frameToken) {
      return;
    }
    std::array<void*, kMaxImages> textures{};
    for (uint32_t i = 0; i < m_images; ++i) {
      textures[i] = m_active[i].Get();
    }
    // The colour-attachment layout OpenXR expects at xrReleaseSwapchainImage, on Dawn's queue,
    // flushed before the XR thread may release.
    const bool success = AuroraDawnVulkanRelease(webgpu::g_device.Get(), textures.data(), m_images) != 0;
    const uint64_t token = m_token;
    m_token = 0;
    m_encoded = false;
    m_active = {};
    m_activeViews = {};
    m_callback(token, success, m_userdata);
  }

private:
  // Leaves the token pending: no GPU work references the images, and the XR thread cancels the
  // frame when no completion arrives.
  bool Decline(const char* reason) noexcept {
    if ((m_declines++ % 120) == 0) {
      Log.warn("Direct stereo frame {} not copied: {} ({} so far)", m_token, reason, m_declines);
    }
    return false;
  }

  std::mutex m_mutex;
  // SetTargets wakes an Encode waiting for its packet's targets.
  std::condition_variable m_targetsSet;
  std::vector<Wrap> m_wraps;
  std::array<AuroraVulkanDirectTarget, kMaxImages> m_targets{};
  std::array<wgpu::Texture, kMaxImages> m_active{};
  std::array<wgpu::TextureView, kMaxImages> m_activeViews{};
  Blitter m_blitter;
  AuroraVulkanDirectSubmittedCallback m_callback = nullptr;
  void* m_userdata = nullptr;
  uint64_t m_token = 0;
  // The last token withdrawn before its encode: a frame already carrying it waits for nothing.
  uint64_t m_cancelled = 0;
  uint32_t m_count = 0;
  uint32_t m_images = 0;
  uint32_t m_declines = 0;
  bool m_panel = false;
  bool m_encoded = false;
};

std::unique_ptr<Bridge> g_bridge;

} // namespace
} // namespace aurora::vulkan_direct

bool aurora_vulkan_direct_available() { return aurora::vulkan_direct::abi_matches(); }

bool aurora_vulkan_direct_configure(const AuroraDawnVulkanHooks* hooks) {
  return aurora::vulkan_direct::abi_matches() && AuroraDawnVulkanConfigure(hooks) != 0;
}

bool aurora_vulkan_direct_get_handles(AuroraDawnVulkanHandles* handles, int64_t* colorVkFormat) {
  using namespace aurora;
  if (handles == nullptr || colorVkFormat == nullptr) {
    return false;
  }
  *handles = {};
  *colorVkFormat = vulkan_direct::to_vk_format(webgpu::g_graphicsConfig.surfaceConfiguration.format);
  if (!vulkan_direct::abi_matches() || !webgpu::g_device || webgpu::g_backendType != wgpu::BackendType::Vulkan ||
      *colorVkFormat == VK_FORMAT_UNDEFINED) {
    return false;
  }
  return AuroraDawnVulkanGetHandles(webgpu::g_device.Get(), handles) != 0;
}

bool aurora_vulkan_direct_enable(AuroraVulkanDirectSubmittedCallback submitted, void* userdata) {
  using namespace aurora::vulkan_direct;
  if (g_bridge || submitted == nullptr || !abi_matches() || !aurora::webgpu::g_device) {
    return false;
  }
  g_bridge = std::make_unique<Bridge>(submitted, userdata);
  aurora::stereo::set_sink(
      [](wgpu::CommandEncoder& encoder, const aurora::stereo::SinkFrame& frame, void* self) noexcept {
        return static_cast<Bridge*>(self)->Encode(encoder, frame);
      },
      [](const aurora::stereo::SinkFrame& frame, void* self) noexcept { static_cast<Bridge*>(self)->Submitted(frame); },
      g_bridge.get());
  return true;
}

bool aurora_vulkan_direct_set_targets(uint64_t frameToken, const AuroraVulkanDirectTarget* targets,
                                      uint32_t targetCount, const AuroraVulkanDirectTarget* panel) {
  using namespace aurora::vulkan_direct;
  return g_bridge && g_bridge->SetTargets(frameToken, targets, targetCount, panel);
}

bool aurora_vulkan_direct_cancel(uint64_t frameToken) {
  using namespace aurora::vulkan_direct;
  return g_bridge && g_bridge->Cancel(frameToken);
}

bool aurora_vulkan_direct_forget_targets(const uint64_t* images, uint32_t count) {
  using namespace aurora::vulkan_direct;
  if (!g_bridge) {
    return true;
  }
  return images != nullptr && g_bridge->Forget(images, count);
}

bool aurora_vulkan_direct_disable() {
  using namespace aurora::vulkan_direct;
  if (!g_bridge) {
    return true;
  }
  aurora::stereo::set_sink(nullptr, nullptr, nullptr);
  if (aurora::webgpu::g_device && AuroraDawnVulkanDrain(aurora::webgpu::g_device.Get()) == 0) {
    (void)g_bridge.release();
    return false;
  }
  g_bridge->Clear();
  g_bridge.reset();
  return true;
}

void* aurora_vulkan_direct_lock_queue() {
  using namespace aurora;
  if (!vulkan_direct::abi_matches() || !webgpu::g_device) {
    return nullptr;
  }
  return AuroraDawnVulkanLock(webgpu::g_device.Get());
}

void aurora_vulkan_direct_unlock_queue(void* guard) {
  if (guard != nullptr) {
    AuroraDawnVulkanUnlock(guard);
  }
}

#else

bool aurora_vulkan_direct_available() { return false; }
bool aurora_vulkan_direct_configure(const AuroraDawnVulkanHooks*) { return false; }
bool aurora_vulkan_direct_get_handles(AuroraDawnVulkanHandles* handles, int64_t* colorVkFormat) {
  if (handles != nullptr) {
    *handles = {};
  }
  if (colorVkFormat != nullptr) {
    *colorVkFormat = 0;
  }
  return false;
}
bool aurora_vulkan_direct_enable(AuroraVulkanDirectSubmittedCallback, void*) { return false; }
bool aurora_vulkan_direct_set_targets(uint64_t, const AuroraVulkanDirectTarget*, uint32_t,
                                      const AuroraVulkanDirectTarget*) {
  return false;
}
bool aurora_vulkan_direct_cancel(uint64_t) { return false; }
bool aurora_vulkan_direct_forget_targets(const uint64_t*, uint32_t) { return true; }
bool aurora_vulkan_direct_disable() { return true; }
void* aurora_vulkan_direct_lock_queue() { return nullptr; }
void aurora_vulkan_direct_unlock_queue(void*) {}

#endif
