#pragma once

#include "texture_convert.hpp"

#include <filesystem>
#include <optional>

namespace aurora::gfx::dds {
struct MipTail {
  ConvertedTexture texture;
  bool includesBase = false;
};

std::optional<ConvertedTexture> parse_dds_bytes(ArrayRef<uint8_t> bytes) noexcept;
std::optional<ConvertedTexture> load_dds_file(const std::filesystem::path& path) noexcept;

// Slices the mip chain from the first level that fits within maxDimension, for use as a standalone texture.
// maxDimension is a soft limit: because a compressed base level must be block-aligned, an earlier (larger) mip
// may be selected instead. Returns nullopt when no level fits, or when no level at or before the first that
// fits has a block-aligned base (callers fall back to a full load).
std::optional<MipTail> parse_dds_mip_tail(ArrayRef<uint8_t> bytes, uint32_t maxDimension) noexcept;
std::optional<MipTail> load_dds_mip_tail(const std::filesystem::path& path, uint32_t maxDimension) noexcept;
ByteBuffer encode_rgba8_dds(uint32_t width, uint32_t height, ArrayRef<uint8_t> pixels);
bool write_rgba8_dds(const std::filesystem::path& path, uint32_t width, uint32_t height, ArrayRef<u8> pixels) noexcept;

} // namespace aurora::gfx::dds
