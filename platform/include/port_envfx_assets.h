#pragma once

#include <cstdint>
#include <vector>

// Remastered's EnvironmentFX textures (rain, snow and underwater flakes; build/mpr/kb
// topic/rain-snow-splash-render.md), as the Remastered import writes them to a mod's
// envfx.bin and the native EnvFX draw (aurora/envfx.hpp) reads them.
namespace PortEnvFx {
// The top mip of a texture, decoded to RGBA8; layer-major (layer 0's rows, then layer 1's...).
// The flakes are sRGB-encoded (sample them as sRGB); the gradient is linear (R8 in the file,
// its value in .r, .a = 255).
struct Image {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t layers = 0;
  std::vector<uint8_t> rgba; // width * height * layers * 4 bytes
};

enum Mode : int { kRain = 0, kSnow = 1, kUnderwater = 2 }; // NMaterialEnvFX fx_mode

struct Assets {
  Image gradient;  // d98eeff1, 16x1x1
  Image flake[3];  // by Mode: d372ef39 32x256x64, 5afbea59 64x64x64, 8576eb33 64x64x1
};

// The first loaded mod's envfx.bin, read once on first call and kept; null when no mod
// supplies one or it does not parse (the caller then draws retail's rain/snow). Main thread.
const Assets* Get();
} // namespace PortEnvFx
