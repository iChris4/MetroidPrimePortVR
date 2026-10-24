#pragma once

#include <aurora/aurora.h>
#include <aurora/math.hpp>

#include "wgpu.hpp"

#include <array>
#include <cstdint>

struct SDL_Window;

namespace aurora::webgpu {
struct GraphicsConfig {
  wgpu::SurfaceConfiguration surfaceConfiguration;
  wgpu::TextureFormat depthFormat;
  uint32_t msaaSamples;
  uint16_t textureAnisotropy;
};
struct TextureWithSampler {
  wgpu::Texture texture;
  wgpu::TextureView view;
  wgpu::Extent3D size;
  wgpu::TextureFormat format;
  wgpu::Sampler sampler;
};
struct Viewport {
  float left;
  float top;
  float width;
  float height;
  float znear;
  float zfar;

  bool operator==(const Viewport& rhs) const {
    return left == rhs.left && top == rhs.top && width == rhs.width && height == rhs.height && znear == rhs.znear &&
           zfar == rhs.zfar;
  }
  bool operator!=(const Viewport& rhs) const { return !(*this == rhs); }
};

extern wgpu::Device g_device;
extern wgpu::Queue g_queue;
extern wgpu::Surface g_surface;
extern wgpu::BackendType g_backendType;
// AuroraConfig::vulkanLibraryDir's library failed to start, so the system's was used.
extern bool g_vulkanLibraryFailed;
extern GraphicsConfig g_graphicsConfig;
extern TextureWithSampler g_frameBuffer;
// Frames left to time after an EFB resize (end_frame logs them): the new targets' first use
// can cost more than the resize itself.
extern int g_resizeFramesToTime;
extern TextureWithSampler g_frameBufferResolved;
extern TextureWithSampler g_depthBuffer;
// Linear HDR scene colour (MSAA + resolved), the world EFB's size, sharing g_depthBuffer. Empty until
// ensure_scene_hdr_target() allocates it; g_frameBuffer stays the 8-bit display EFB that present and
// screenshots read.
inline constexpr wgpu::TextureFormat SceneHdrFormat = wgpu::TextureFormat::RGBA16Float;
extern TextureWithSampler g_sceneHdr;
extern TextureWithSampler g_sceneHdrResolved;
extern wgpu::RenderPipeline g_CopyPipeline;
extern wgpu::RenderPipeline g_CopyPremultipliedAlphaPipeline;
extern wgpu::BindGroup g_CopyBindGroup;
extern wgpu::Instance g_instance;
extern wgpu::AdapterInfo g_adapterInfo;
extern bool g_hasCoreFeatures;
extern bool g_bcTexturesSupported;
// The device was given 17 sampled textures a stage (the default is 16), so group 2 has the baked lightmap's slot.
extern bool g_lightmapBinding;
extern bool g_astcTexturesSupported;
extern bool g_textureComponentSwizzleSupported;
// The device has PrimedGun's Vulkan multiview (quest/dawn patches, gfx/stereo_multiview.hpp).
extern bool g_multiviewSupported;
extern bool g_dualSourceBlendingSupported;

bool initialize(AuroraBackend backend, bool allowCpu);
void shutdown();
void release_surface() noexcept;
bool refresh_surface(bool recreate = true);
// True when the live surface is bound to a native window that no longer matches
// the SDL window, so it has to be rebuilt instead of reconfigured.
bool surface_window_changed();
void resize_swapchain(uint32_t width, uint32_t height, uint32_t nativeWidth, uint32_t nativeHeight, bool force = false);
// Changes MSAA (1 or 4) and the anisotropy that GX_ANISO_4 maps to. Call between frames with a live surface.
void set_quality(uint32_t msaaSamples, uint16_t anisotropy);
// format Undefined means the surface format.
TextureWithSampler create_render_texture(uint32_t width, uint32_t height, bool multisampled,
                                         wgpu::TextureFormat format = wgpu::TextureFormat::Undefined);
// True with a live device. RGBA16Float as a blendable, multisampled, resolvable attachment is core WebGPU,
// so Compatibility devices (no CoreFeaturesAndLimits) try the HDR scene too; ensure_scene_hdr_target()
// falls back to LDR when the allocation raises an error.
bool scene_hdr_supported() noexcept;
// Allocates g_sceneHdr/g_sceneHdrResolved for the current g_frameBuffer size and sample count if
// needed, under Validation/OutOfMemory/Internal error scopes. True only with both textures and views
// valid and error-free; otherwise nothing stays allocated, and a failed size/sample count is warned
// about once and not retried until it changes or the target is released. Kept across on/off until
// resize, MSAA change or shutdown.
bool ensure_scene_hdr_target();
// g_sceneHdr's sample count, 0 while it is not allocated.
uint32_t scene_hdr_sample_count() noexcept;
void release_scene_hdr_target() noexcept;
// A depth attachment in the configured depth format and MSAA sample count.
TextureWithSampler create_depth_texture(uint32_t width, uint32_t height);
// The EFB's targets as the calling thread sees them. The main thread records each frame against
// the newest (g_frameBuffer, g_frameBufferResolved, g_depthBuffer); the render worker encodes
// frames handed to it earlier, so on the worker these are the ones the frame it is encoding was
// recorded against. A resize that only changes the EFB scale hands the new set to the worker as a
// queued item between the last frame at the old size and the first at the new one, instead of
// waiting for the worker to drain (resize_swapchain).
struct EfbTargets {
  TextureWithSampler color;
  TextureWithSampler resolved;
  TextureWithSampler depth;
};
EfbTargets efb_targets() noexcept;
// What is presented (the resolved EFB with MSAA, else the EFB), as the calling thread sees it.
const TextureWithSampler& present_source() noexcept;
wgpu::BindGroup create_copy_bind_group(const TextureWithSampler& source);
void set_resampler(AuroraSampler sampler) noexcept;
void set_present_sharpness(float sharpness) noexcept;
AuroraSampler get_resampler() noexcept;
Viewport calculate_present_viewport(uint32_t surface_width, uint32_t surface_height, uint32_t content_width,
                                    uint32_t content_height) noexcept;
const TextureWithSampler& resample_present_source(const wgpu::CommandEncoder& encoder, const Viewport& viewport);
// The same for any texture (a stereo eye target shown in the window).
const TextureWithSampler& resample_source(const wgpu::CommandEncoder& encoder, const Viewport& viewport,
                                          const TextureWithSampler& source);
void draw_clear(const wgpu::RenderPassEncoder& pass, bool clearColor, bool clearAlpha, bool clearDepth,
                const Vec4<float>& clearColorValue, float clearDepthValue);

size_t load_from_cache(void const* key, size_t keySize, void* value, size_t valueSize, void* userdata);
void store_to_cache(void const* key, size_t keySize, void const* value, size_t valueSize, void* userdata);
void cache_prune();
void cache_shutdown();

} // namespace aurora::webgpu
