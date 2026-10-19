#pragma once

#include "texture.hpp"
#include "types.hpp"

// Port extension: the environment probe the PBR path reflects (GX_AURORA_COPY_PROBE_FACE).
// One cube map, written a face at a time from an EFB copy and blurred down its mip chain,
// so the shader can pick a mip by roughness.
namespace aurora::gfx::probe {
constexpr uint32_t Size = 128;
constexpr uint32_t MipCount = 6; // 128 .. 4

void shutdown();
// Live textures this file owns (cubes incl. blend targets, volumes, lightmaps, the probe and the fixed
// BRDF/empty ones) and their bytes, for aurora_get_texture_stats.
void live_stats(uint32_t& count, uint64_t& bytes);
const wgpu::TextureView& cube_view();
const wgpu::Sampler& sampler();
// Mip 0 of one face, as a resolve target.
TextureHandle face(uint32_t face);
// Rebuilds a face's mips from its mip 0. `uvRange` is an identity UV transform uniform.
void encode_mips(const wgpu::CommandEncoder& cmd, uint32_t face, Range uvRange);

// Room cubes (GX_AURORA_CREATE_PBR_CUBE): prefiltered HDR cube maps the game supplies, which
// a PBR draw can reflect instead of the probe above. `texels` is RGBA16Float, every mip of
// face 0 from the largest down, then face 1 and so on.
void create_cube(uint32_t id, uint32_t size, uint32_t mipCount, const uint8_t* texels, size_t length);
void destroy_cube(uint32_t id);
// The view of a room cube, or the probe's when there is no such cube.
const wgpu::TextureView& cube_view(uint32_t id);
bool has_cube(uint32_t id);
// GX_AURORA_BLEND_PBR_CUBE, on the game thread: renders sum(weights[i] * src[i]) into room cube
// `dst` (created or resized to the first source's size), face by face and mip by mip. Returns
// false, leaving `dst` as it was or blank, when a source is missing or no EFB pass is open.
constexpr uint32_t MaxBlend = 4;
bool blend_cubes(uint32_t dst, const uint32_t* src, const float* weights, uint32_t count);

// The environment BRDF table (GX_AURORA_SET_PBR_BRDF_LUT): 16x8 RG8, 256 bytes. Returns
// whether one is now set; any other length clears it.
constexpr uint32_t BrdfLutBytes = 256;
bool set_brdf_lut(const uint8_t* texels, size_t length);
// The table, or a 1x1 dummy while there is none. The sampler is sampler().
const wgpu::TextureView& brdf_lut_view();

// Ambient volumes (GX_AURORA_CREATE_PBR_VOLUME): a room's baked ambient light as five 3D
// textures (mean, lobe, and the direction and sharpness of red, green and blue), which a
// PBR draw samples per pixel. `texels` is laid out as GXCreatePBRVolume says.
constexpr uint32_t VolumeTextures = 5;
void create_volume(uint32_t id, uint32_t sizeX, uint32_t sizeY, uint32_t sizeZ, const uint8_t* texels, size_t length);
void destroy_volume(uint32_t id);
bool has_volume(uint32_t id);
// One texture of a volume, or of an empty one when there is no such volume.
const wgpu::TextureView& volume_view(uint32_t id, uint32_t index);

// Baked lightmaps (GX_AURORA_CREATE_PBR_LIGHTMAP): a 2D array of layers, laid out as
// GXCreatePBRLightmap says (`format` is its GXPBRLightmapFormat). Not created when the
// layout has no slot for it (lightmap_available) or the data is short.
bool lightmap_available();
bool lightmap_bc_supported();
void create_lightmap(uint32_t id, uint32_t width, uint32_t height, uint32_t layers, uint32_t format,
                     const uint8_t* texels, size_t length);
void destroy_lightmap(uint32_t id);
bool has_lightmap(uint32_t id);
// The lightmap's array view, or a 1x1x4 dummy when there is no such lightmap.
const wgpu::TextureView& lightmap_view(uint32_t id);
} // namespace aurora::gfx::probe
