// envfx.bin: Remastered's EnvironmentFX textures (rain, snow, underwater flakes and the
// 16x1 fade gradient), decoded by the import so the native EnvFX draw needs no TXTR reader.
//
// Layout, little-endian throughout:
//   u32 magic    'MPEF' (bytes 4D 50 45 46)
//   u32 version  1
//   u32 count    4 (gradient, rain flake, snow flake, underwater flake, in that order)
//   count x { u32 width, u32 height, u32 layers, width*height*layers*4 bytes of RGBA8 }
// Layers are stored layer-major (layer 0's rows, then layer 1's...), rows in the file's
// stored order, no flip. The flakes keep their sRGB encoding (sample them as sRGB); the
// gradient is the R8 texture expanded to (value, 0, 0, 255).
// Expected sizes: gradient 16x1x1, rain 32x256x64, snow 64x64x64, underwater 64x64x1.
#include "port_envfx_file.h"

#include "port_bytes.h"

namespace PortEnvFx {
namespace {
constexpr uint32_t kMagic = 0x4645504D;  // 'MPEF'
constexpr uint32_t kVersion = 1;
constexpr uint32_t kCount = 4;
constexpr uint32_t kExpected[kCount][3] = {{16, 1, 1}, {32, 256, 64}, {64, 64, 64}, {64, 64, 1}};

const Image& ImageAt(const Assets& assets, uint32_t i) { return i == 0 ? assets.gradient : assets.flake[i - 1]; }
Image& ImageAt(Assets& assets, uint32_t i) { return i == 0 ? assets.gradient : assets.flake[i - 1]; }
}  // namespace

std::vector<uint8_t> Serialize(const Assets& assets) {
  std::vector<uint8_t> out;
  port::AppendLE32(out, kMagic);
  port::AppendLE32(out, kVersion);
  port::AppendLE32(out, kCount);
  for (uint32_t i = 0; i < kCount; ++i) {
    const Image& image = ImageAt(assets, i);
    port::AppendLE32(out, image.width);
    port::AppendLE32(out, image.height);
    port::AppendLE32(out, image.layers);
    out.insert(out.end(), image.rgba.begin(), image.rgba.end());
  }
  return out;
}

bool Parse(const uint8_t* data, size_t size, Assets& out, std::string& error) {
  if (data == nullptr || size < 12 || port::ReadLE32(data) != kMagic) {
    error = "not an envfx.bin";
    return false;
  }
  if (port::ReadLE32(data + 4) != kVersion) {
    error = "envfx.bin version " + std::to_string(port::ReadLE32(data + 4)) + " (expected " +
            std::to_string(kVersion) + ")";
    return false;
  }
  if (port::ReadLE32(data + 8) != kCount) {
    error = "envfx.bin image count " + std::to_string(port::ReadLE32(data + 8));
    return false;
  }
  size_t at = 12;
  for (uint32_t i = 0; i < kCount; ++i) {
    if (size - at < 12) {
      error = "envfx.bin truncated";
      return false;
    }
    Image& image = ImageAt(out, i);
    image.width = port::ReadLE32(data + at);
    image.height = port::ReadLE32(data + at + 4);
    image.layers = port::ReadLE32(data + at + 8);
    at += 12;
    if (image.width != kExpected[i][0] || image.height != kExpected[i][1] || image.layers != kExpected[i][2]) {
      error = "envfx.bin image " + std::to_string(i) + " is " + std::to_string(image.width) + "x" +
              std::to_string(image.height) + "x" + std::to_string(image.layers);
      return false;
    }
    const size_t bytes = size_t(image.width) * image.height * image.layers * 4;
    if (size - at < bytes) {
      error = "envfx.bin truncated";
      return false;
    }
    image.rgba.assign(data + at, data + at + bytes);
    at += bytes;
  }
  if (at != size) {
    error = "envfx.bin has trailing bytes";
    return false;
  }
  return true;
}
}  // namespace PortEnvFx
