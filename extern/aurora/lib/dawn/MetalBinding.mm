#include "BackendBinding.hpp"

#import <Foundation/Foundation.h>
#include <SDL3/SDL_metal.h>
#include <SDL3/SDL_properties.h>
#include <TargetConditionals.h>

#if TARGET_OS_VISION
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <SDL3/SDL_video.h>
#endif

namespace aurora::webgpu::utils {
#if !TARGET_OS_VISION
namespace {
constexpr const char* MetalViewProperty = "aurora.metal_view";

void SDLCALL destroy_metal_view(void*, void* value) {
  if (value != nullptr) {
    SDL_Metal_DestroyView(static_cast<SDL_MetalView>(value));
  }
}
} // namespace
#endif

std::shared_ptr<wgpu::ChainedStruct> SetupWindowAndGetSurfaceDescriptorCocoa(SDL_Window* window) {
#if TARGET_OS_VISION
  // Apple Vision Pro: SDL runs on its offscreen video driver (lib/window.cpp), so there is no UIKit
  // view to hang a Metal layer on, and nothing ever shows the desktop image anyway: CompositorServices
  // owns the display and the eyes reach it through aurora/metal_interop.h. Dawn still wants a
  // CAMetalLayer for its surface, so it gets one that belongs to no view. Its drawables are acquired
  // and presented like any other layer's and simply never reach a screen. One layer per process: a
  // surface rebuild reconfigures it rather than leaking a fresh one each time. (Wiicompiled VR.)
  static CAMetalLayer* layer = nil;
  if (layer == nil) {
    layer = [CAMetalLayer layer];
    layer.device = MTLCreateSystemDefaultDevice();
    layer.framebufferOnly = NO;
    layer.opaque = YES;
  }
  int width = 0;
  int height = 0;
  if (window != nullptr && SDL_GetWindowSizeInPixels(window, &width, &height) && width > 0 && height > 0) {
    layer.drawableSize = CGSizeMake(width, height);
  }
  std::shared_ptr<wgpu::SurfaceSourceMetalLayer> desc = std::make_shared<wgpu::SurfaceSourceMetalLayer>();
  desc->layer = (__bridge void*)layer;
  return std::move(desc);
#else
  const auto properties = SDL_GetWindowProperties(window);
  auto view = static_cast<SDL_MetalView>(SDL_GetPointerProperty(properties, MetalViewProperty, nullptr));
  if (view == nullptr) {
    view = SDL_Metal_CreateView(window);
    if (view == nullptr ||
        !SDL_SetPointerPropertyWithCleanup(properties, MetalViewProperty, view, destroy_metal_view, nullptr)) {
      if (view != nullptr) {
        SDL_Metal_DestroyView(view);
      }
      return nullptr;
    }
  }
  std::shared_ptr<wgpu::SurfaceSourceMetalLayer> desc = std::make_shared<wgpu::SurfaceSourceMetalLayer>();
  desc->layer = SDL_Metal_GetLayer(view);
  if (desc->layer == nullptr) {
    return nullptr;
  }
  return std::move(desc);
#endif
}
} // namespace aurora::webgpu::utils
