#pragma once

// Images for the Remastered importer: resizing, and the three files a mod
// texture is written as: a GameCube TXTR (RGBA8 or CMPR, with mips) and the
// port's native texture, a DX10 .dds in BC7 or BC5 (port_mods.h), or in ASTC
// 4x4 where the GPU has no BC (most phones; Aurora refuses a BC .dds there).
// TextureFormat() picks: BC unless SetGpuTextureSupport() says the device has
// ASTC and not BC; MP_REMASTERED_TEXTURE_FORMAT=bc|astc overrides. An import
// with no GPU (--import-remastered) never calls it, so it writes BC.
//
// The encoders are the importer's own and aim at "fast and good", not at the
// best a format can do: BC7 uses mode 6 only (one RGBA line a block, 4-bit
// indices), which is right for the smooth colour and data maps these are.
// ASTC is ARM's astc-encoder (extern/astcenc) at its FAST preset, LDR, linear
// UNORM like the BC7. A normal map is stored as RGBA with R=x, G=y, B=0, A=255,
// since the PBR shader reads `.rg` and ASTC has no two-channel format.

#include <cstdint>
#include <vector>

namespace PortRemastered {

struct Image {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> rgba;  // width * height * 4
  bool srgb = false;          // as decoded: the source format is an sRGB one (the bytes are encoded either way)
  bool hdr = false;           // the source is BC6H: the bytes are its linear values clamped to 1 (ConvertIO::hdrDds has them all)
};

enum class DdsFormat {
  BC7,  // all four channels
  BC5,  // red and green only: a normal map
  ASTC4x4,        // all four channels
  ASTC4x4Normal,  // red and green kept, blue 0, alpha 255: a normal map
};

// What a map's texels mean, which decides the space they are filtered in:
//   Data    the bytes as they are (metal/roughness, AO, masks);
//   Colour  RGB is sRGB, filtered in linear light (alpha stays linear);
//   Normal  RG (and B, when the map has it) is a tangent-space direction in
//           [0,255] -> [-1,1], filtered as a vector and renormalised.
enum class MapKind {
  Data,
  Colour,
  Normal,
};

// What the importer writes its .dds files as: BC or ASTC (see above).
enum class TextureFormat { BC, ASTC };
// Tells the importer what the GPU can sample; call once the device exists.
void SetGpuTextureSupport(bool bc, bool astc);
TextureFormat WantedTextureFormat();
const char* TextureFormatName();  // "BC7" or "ASTC 4x4"
// The DdsFormat for a colour/data map, or a normal map, under WantedTextureFormat().
DdsFormat ColourDdsFormat();
DdsFormat NormalDdsFormat();

// Lanczos-3, each channel on its own (alpha is often a mask here, not
// opacity, so nothing is premultiplied). The arithmetic follows Pillow's.
Image Resize(const Image& image, int width, int height, MapKind kind = MapKind::Data);

// An sRGB byte as the linear value it encodes, rounded to a byte.
uint8_t SrgbToLinearByte(uint8_t value);

// An sRGB byte as linear light (0..1), exact.
float SrgbByteToLinear(uint8_t value);
// Linear light (0..1) as the nearest sRGB byte.
uint8_t LinearToSrgbByteExact(float linear);

// An sRGB byte times `scale` in linear light, back to an sRGB byte.
uint8_t ScaleSrgbByte(uint8_t value, double scale);

// A mipmapped RGBA8 TXTR (format 9); mips stop once a side reaches minSize.
// Sides must be multiples of 4.
std::vector<uint8_t> EncodeTxtrRgba8(const Image& image, int minSize = 8, MapKind kind = MapKind::Data);
// A mipmapped CMPR TXTR (format 10). Without `alpha` every texel is opaque;
// with it, texels under alpha 128 are transparent, and the smaller levels keep
// the top level's share of opaque texels. Sides must be multiples of 8.
std::vector<uint8_t> EncodeTxtrCmpr(const Image& image, bool alpha, MapKind kind = MapKind::Data);
// A .dds with the whole mip chain down to 1x1. `punch` (a cut-out alpha) keeps
// the share of opaque texels in the smaller levels, as EncodeTxtrCmpr does.
// `srgb` writes the sRGB DXGI code (BC7, ASTC), so the GPU decodes RGB itself;
// the texels are the same sRGB-encoded bytes.
std::vector<uint8_t> EncodeDds(const Image& image, DdsFormat format, bool punch = false, MapKind kind = MapKind::Data,
                                 bool srgb = false);

// High dynamic range maps (a sky's BC6H), linear and never sRGB:
// a DX10 .dds of BC6H blocks passed through as they are (DXGI 95 BC6H_UF16, 96 BC6H_SF16),
// mips[m] holding ceil(w_m / 4) * ceil(h_m / 4) blocks; empty when a level's size is wrong.
std::vector<uint8_t> EncodeDdsBc6h(uint32_t width, uint32_t height, bool isSigned,
                                   const std::vector<std::vector<uint8_t>>& mips);
// A DX10 .dds of RGBA half floats (DXGI 10, R16G16B16A16_FLOAT), for a GPU without BC;
// mips[m] holding w_m * h_m * 4 halves. Empty when a level's size is wrong.
std::vector<uint8_t> EncodeDdsRgba16f(uint32_t width, uint32_t height, const std::vector<std::vector<uint16_t>>& mips);
// The next level of an RGBA half float image: a 2x2 box in float.
std::vector<uint16_t> HalveRgba16f(const std::vector<uint16_t>& level, uint32_t width, uint32_t height);

// One 4x4 block, 16 RGBA texels in, 16 bytes out; exposed for the tests.
void EncodeBc7Block(const uint8_t* rgba, uint8_t* out);
void EncodeBc5Block(const uint8_t* rgba, uint8_t* out);

}  // namespace PortRemastered
