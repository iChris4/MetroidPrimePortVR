#pragma once

#include <cstdint>

// Port extension: Remastered's rain, snow and underwater flakes (CEnvFxContextSceneNodeMPT,
// material function EnvironmentFX; build/mpr/kb topic/rain-snow-splash-render.md), drawn natively
// as instanced quads into the HDR scene. The shaders follow Remastered's decompiled EnvironmentFX
// vertex and pixel shaders (one per mode, with the froxel fog perm); the port's world is z up where
// Remastered's is y up, so its pos.xz is our pos.xy and the gradient coordinate runs along z.
namespace aurora::gfx::envfx {
enum class Mode : uint8_t { Rain, Snow, Underwater }; // NMaterialEnvFX fx_mode

// One particle, Remastered's 8-byte instance: the position in the grid in 1/256 units, and
// w = grid cell (0..63) | flake layer (0..63) << 6.
struct Instance {
  int16_t pos[3];
  uint16_t w;
};
static_assert(sizeof(Instance) == 8);

// A texture's top mip, RGBA8, layer-major (layer 0's rows, then layer 1's...).
struct Image {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t layers = 0;
  const uint8_t* rgba = nullptr;
};

// Copies the textures (the gradient linear, the flakes sRGB-encoded, indexed by Mode) and builds
// their mips (box filter, the flakes' in linear light); the GPU textures are made on first draw.
// Once is enough: a later call replaces them. False (nothing kept) if an image is malformed.
bool upload(const Image& gradient, const Image (&flake)[3]);
bool uploaded();

struct DrawDesc {
  Mode mode = Mode::Rain;
  // Remastered's `params`: [0].x alpha scale, [0].y rgb gain, [1] and [2] the quad's two edges
  // (model space; the quad spans pos .. pos + [1] + [2], its uv the corner in [0,1]).
  float params[3][4] = {};
  // `grid_cell_data`: per cell, the offset (x, y) added to its particles and the gradient
  // coordinate's scale and bias along z (u = z * pos.z + w).
  float grid[64][4] = {};
  // Fog the flakes by this frame's froxels (when the GX state is in the volumetric fog's range):
  // rgb * transmittance, plus the in-scatter for Underwater only (Remastered's analytic injectors:
  // additive w = 0 for rain and snow, 1 for underwater).
  bool fog = true;
};

// Records the quads into the current pass with the projection and the current position matrix
// (view * model) the GX state holds at this call, depth test on and write off, no culling, and the
// mode's blend (rain SrcA,One; snow One,One; underwater SrcA,InvSrcA). The colour is written as is
// into the HDR scene's exposed light: Remastered's gain carries the inverse of the exposure its
// tonemap applies, so the two cancel and the flakes are display-referred (see envfx.cpp).
// Draws only into the HDR scene and only once the textures are uploaded: false (nothing recorded)
// otherwise, so the caller can draw its fallback.
bool draw(const DrawDesc& desc, const Instance* instances, uint32_t count);
void shutdown();
} // namespace aurora::gfx::envfx
