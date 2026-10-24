#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <webgpu/webgpu_cpp.h>

// Port extension: Remastered's volumetric fog (CRenderPass_VolumetricFog), run on the finished
// EFB between two passes (see GXPortVolumetricFog).
namespace aurora::gfx::volfog {
constexpr uint32_t LutSize = 64;
constexpr uint32_t MaxRegions = 8;

// The task's uniform, word for word. View space is GX's: x right, y up, z towards the camera.
struct Params {
  float viewToWorld[3][4];   // rows: world = row . (view, 1)
  float worldToVolume[3][4]; // rows of world -> the ambient volume's texture coordinates
  float frustum[4];          // left, right, bottom, top at a view depth of 1
  float depth[4];            // near, far, the depth range the world draws in (GX z, min, max)
  float fog[4];              // range (the fog's far), scatter, absorb, density
  float shape[4];            // height slope, height bias (over world z), noise frequency, noise strength
  float noise[4];            // xyz: the noise's offset (wind), w: the light's largest channel
  float colorB[4];           // the light's multiplier; w: the volume's level (0: no volume light)
  float colorA[4];           // the light added; w: the exposure the EFB was drawn at
  float tone[3][4];          // the tone curve the EFB was drawn through (as GXSetPBRTone)
  float lut[LutSize];        // density over distance: entry i at (i / 63)^2 * range
  // The fog regions (SFogDensityRegionParams), MaxRegions of 7 rows: world -> 0..1 over the
  // box (3), edge scale + mult, edge bias + cap, colour, density (x).
  float regions[MaxRegions * 7][4];
  uint32_t regionCount;
  uint32_t pad[3];
  uint32_t volume;           // the ambient volume (probe::create_volume id), 0 for none
  uint32_t flags;
  uint32_t grid[2];          // set by the task: the froxel grid's width and height
};
static_assert(sizeof(Params) == (132 + MaxRegions * 28 + 4) * 4);

// Registers the fog's encoder task (game thread); false if it could not be.
bool ensure_task();
// Records the fog from the FIFO processor (GX_AURORA_PORT_VOLUMETRIC_FOG), once ensure_task has
// returned true. False if nothing was recorded.
bool record(const Params& params);
// The froxels the last recorded fog fills (a placeholder before the first), and their sampler.
// The draws after the fog read them to fog themselves (GXState::volFog).
const wgpu::TextureView& froxel_view();
const wgpu::Sampler& sampler();
void shutdown();

// The apply pass's WGSL for a sample count (exposed for the tests). It fogs the HDR scene
// (set_scene_hdr); outside its pass record() records nothing.
std::string apply_source(uint32_t samples);
} // namespace aurora::gfx::volfog
