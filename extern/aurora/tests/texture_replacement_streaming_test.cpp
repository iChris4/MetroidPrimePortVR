#include "gfx/dds_io.hpp"
#include "gfx/texture_replacement.hpp"

#include <aurora/texture.hpp>
#include <gtest/gtest.h>

#include <atomic>
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <string>
#include <vector>

namespace aurora::gfx::texture_replacement {
namespace {
using namespace std::chrono_literals;

constexpr const char* VirtualPathA = "tex1_4x4_0000000000000001_6.dds";
constexpr const char* VirtualPathB = "tex1_4x4_0000000000000002_6.dds";
constexpr const char* VirtualPngPath = "tex1_4x4_0000000000000001_6.png";

struct BlockingSource {
  std::mutex mutex;
  std::condition_variable cv;
  bool entered = false;
  bool release = false;
  std::atomic<uint32_t> reads = 0;

  static bool read(void* userData, const char*, std::vector<uint8_t>&) {
    auto& source = *static_cast<BlockingSource*>(userData);
    ++source.reads;
    std::unique_lock lock{source.mutex};
    source.entered = true;
    source.cv.notify_all();
    source.cv.wait(lock, [&] { return source.release; });
    return false;
  }
};

struct RecordingSource {
  std::mutex mutex;
  std::condition_variable cv;
  std::vector<std::string> paths;

  static bool read(void* userData, const char* path, std::vector<uint8_t>&) {
    auto& source = *static_cast<RecordingSource*>(userData);
    {
      std::lock_guard lock{source.mutex};
      source.paths.emplace_back(path);
    }
    source.cv.notify_all();
    return false;
  }
};

class ReplacementStreamingTest : public ::testing::Test {
protected:
  void SetUp() override {
    shutdown();
    testing::set_worker_count(1);
  }

  void TearDown() override { shutdown(); }
};

TEST_F(ReplacementStreamingTest, UnregisterWaitsForInFlightVirtualRead) {
  BlockingSource source;
  const auto registration =
      texture::register_virtual_replacement(VirtualPathA, {.read = &BlockingSource::read, .userData = &source});
  ASSERT_NE(registration.id, 0u);
  {
    std::unique_lock lock{source.mutex};
    ASSERT_TRUE(source.cv.wait_for(lock, 2s, [&] { return source.entered; }));
  }

  auto unregister = std::async(std::launch::async, [&] { texture::unregister_replacement(registration); });
  EXPECT_EQ(unregister.wait_for(50ms), std::future_status::timeout);
  {
    std::lock_guard lock{source.mutex};
    source.release = true;
  }
  source.cv.notify_all();

  EXPECT_EQ(unregister.wait_for(2s), std::future_status::ready);
  EXPECT_EQ(source.reads.load(), 1u);
}

TEST_F(ReplacementStreamingTest, CancelledQueuedVirtualJobNeverReads) {
  RecordingSource source;
  testing::set_workers_paused(true);
  const auto registration =
      texture::register_virtual_replacement(VirtualPathA, {.read = &RecordingSource::read, .userData = &source});
  ASSERT_NE(registration.id, 0u);

  texture::unregister_replacement(registration);
  testing::set_workers_paused(false);
  shutdown();

  EXPECT_TRUE(source.paths.empty());
}

TEST_F(ReplacementStreamingTest, HighPriorityFullLoadRunsBeforeThumbnailQueue) {
  RecordingSource source;
  testing::set_workers_paused(true);
  const auto low =
      texture::register_virtual_replacement(VirtualPathA, {.read = &RecordingSource::read, .userData = &source});
  const auto high =
      texture::register_virtual_replacement(VirtualPathB, {.read = &RecordingSource::read, .userData = &source});
  ASSERT_NE(low.id, 0u);
  ASSERT_NE(high.id, 0u);

  GXTexObj_ obj{};
  const auto key = std::get<texture::TextureSourceKey>(high.key);
  ASSERT_TRUE(find_source_replacement(obj, key).has_value());
  testing::set_workers_paused(false);
  {
    std::unique_lock lock{source.mutex};
    ASSERT_TRUE(source.cv.wait_for(lock, 2s, [&] { return !source.paths.empty(); }));
    EXPECT_EQ(source.paths.front(), VirtualPathB);
  }
  const std::array registrations{low, high};
  texture::unregister_replacements(registrations);
}

TEST_F(ReplacementStreamingTest, StaleFailureDoesNotPoisonNewRegistration) {
  RecordingSource source;
  testing::set_workers_paused(true);
  const auto oldRegistration =
      texture::register_virtual_replacement(VirtualPngPath, {.read = &RecordingSource::read, .userData = &source});
  ASSERT_NE(oldRegistration.id, 0u);
  GXTexObj_ obj{};
  const auto key = std::get<texture::TextureSourceKey>(oldRegistration.key);
  ASSERT_TRUE(find_source_replacement(obj, key).has_value());
  testing::set_workers_paused(false);
  ASSERT_TRUE(testing::wait_for_completions(oldRegistration.id, 1, 2000));
  texture::unregister_replacement(oldRegistration);

  testing::set_workers_paused(true);
  const auto newRegistration =
      texture::register_virtual_replacement(VirtualPngPath, {.read = &RecordingSource::read, .userData = &source});
  process_streaming();

  ASSERT_TRUE(find_source_replacement(obj, key).has_value());
  testing::set_workers_paused(false);
  EXPECT_TRUE(testing::wait_for_completions(newRegistration.id, 1, 2000));
  texture::unregister_replacement(newRegistration);
}

template <typename T>
void write_u32(std::vector<uint8_t>& bytes, size_t offset, T value) {
  const uint32_t word = static_cast<uint32_t>(value);
  std::memcpy(bytes.data() + offset, &word, sizeof(word));
}

TEST(DdsMipTailTest, SlicesTheSameTailAsFullDecode) {
  std::vector<uint8_t> base(8 * 8 * 4);
  auto encoded = dds::encode_rgba8_dds(8, 8, base);
  std::vector<uint8_t> bytes(encoded.data(), encoded.data() + encoded.size());
  bytes.resize(128 + (8 * 8 + 4 * 4 + 2 * 2 + 1) * 4);
  for (size_t i = 128; i < bytes.size(); ++i) {
    bytes[i] = static_cast<uint8_t>(i);
  }
  write_u32(bytes, 8, 0x1u | 0x2u | 0x4u | 0x8u | 0x1000u | 0x20000u);
  write_u32(bytes, 28, 4u);
  write_u32(bytes, 108, 0x1000u | 0x8u | 0x400000u);

  const auto full = dds::parse_dds_bytes({bytes.data(), bytes.size()});
  const auto tail = dds::parse_dds_mip_tail({bytes.data(), bytes.size()}, 2);
  const auto path =
      std::filesystem::temp_directory_path() /
      ("aurora-dds-tail-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".dds");
  {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }
  const auto fileTail = dds::load_dds_mip_tail(path, 2);
  std::error_code removeError;
  std::filesystem::remove(path, removeError);
  ASSERT_TRUE(full.has_value());
  ASSERT_TRUE(tail.has_value());
  ASSERT_TRUE(fileTail.has_value());
  EXPECT_FALSE(tail->includesBase);
  EXPECT_EQ(tail->texture.width, 2u);
  EXPECT_EQ(tail->texture.height, 2u);
  EXPECT_EQ(tail->texture.mips, 2u);
  ASSERT_EQ(tail->texture.data.size(), 20u);
  EXPECT_TRUE(std::equal(tail->texture.data.data(), tail->texture.data.data() + tail->texture.data.size(),
                         full->data.data() + full->data.size() - 20));
  ASSERT_EQ(fileTail->texture.data.size(), tail->texture.data.size());
  EXPECT_TRUE(std::equal(fileTail->texture.data.data(), fileTail->texture.data.data() + fileTail->texture.data.size(),
                         tail->texture.data.data()));
}

TEST(DdsMipTailTest, RejectsMiplessLargeTextureButKeepsSmallWholeTexture) {
  std::vector<uint8_t> largePixels(128 * 128 * 4);
  const auto large = dds::encode_rgba8_dds(128, 128, largePixels);
  EXPECT_FALSE(dds::parse_dds_mip_tail(large, 64).has_value());

  std::vector<uint8_t> smallPixels(32 * 16 * 4);
  const auto small = dds::encode_rgba8_dds(32, 16, smallPixels);
  const auto tail = dds::parse_dds_mip_tail(small, 64);
  ASSERT_TRUE(tail.has_value());
  EXPECT_TRUE(tail->includesBase);
  ASSERT_EQ(tail->texture.data.size(), smallPixels.size());
  EXPECT_TRUE(
      std::equal(tail->texture.data.data(), tail->texture.data.data() + tail->texture.data.size(), smallPixels.data()));
}
// ---------------------------------------------------------------------------
// Mip tail base alignment: a compressed tail becomes a standalone texture, so its base level must cover whole
// texel blocks. maxDimension is a soft limit: the tail walks back to the nearest aligned earlier level when the
// first level that fits is not aligned.
// ---------------------------------------------------------------------------

constexpr size_t DdsHeaderBytes = 4 + 124;
constexpr size_t DdsDx10HeaderBytes = DdsHeaderBytes + 20;

// Payload byte i is (i ^ (i >> 8)) & 0xFF. The pattern does repeat, but only every 65536 bytes, while it varies
// with the absolute file offset; across the (much smaller) levels tested here, bytes at the tested level boundaries
// differ, so a tail sliced at the wrong offset cannot compare equal by accident.
void fill_payload(std::vector<uint8_t>& bytes, size_t offset) {
  for (size_t i = offset; i < bytes.size(); ++i) {
    bytes[i] = static_cast<uint8_t>((i ^ (i >> 8)) & 0xFF);
  }
}

std::vector<uint8_t> make_dds(uint32_t width, uint32_t height, uint32_t mips, wgpu::TextureFormat format) {
  const bool compressed = format_info(format).compressed;
  const size_t headerBytes = compressed ? DdsDx10HeaderBytes : DdsHeaderBytes;
  const size_t payloadBytes = static_cast<size_t>(calc_texture_size(format, width, height, mips));
  std::vector<uint8_t> bytes(headerBytes + payloadBytes, 0);
  std::memcpy(bytes.data(), "DDS ", 4);
  write_u32(bytes, 4, 124u);                                                             // dwSize
  write_u32(bytes, 8, 0x1u | 0x2u | 0x4u | 0x8u | 0x1000u | (mips > 1 ? 0x20000u : 0u)); // dwFlags
  write_u32(bytes, 12, height);
  write_u32(bytes, 16, width);
  write_u32(bytes, 20, static_cast<uint32_t>(payloadBytes)); // dwPitchOrLinearSize (unvalidated)
  write_u32(bytes, 28, mips);
  write_u32(bytes, 76, 32u); // DDS_PIXELFORMAT dwSize
  write_u32(bytes, 108, 0x1000u | (mips > 1 ? 0x8u | 0x400000u : 0u));
  if (compressed) {
    write_u32(bytes, 80, 0x4u);        // DDPF_FOURCC
    write_u32(bytes, 84, 0x30315844u); // "DX10"
    write_u32(bytes, 128, 98u);        // DXGI_FORMAT_BC7_UNORM
    write_u32(bytes, 132, 3u);         // D3D10_RESOURCE_DIMENSION_TEXTURE2D
    write_u32(bytes, 140, 1u);         // arraySize
  } else {
    write_u32(bytes, 80, 0x40u | 0x1u); // DDPF_RGB | DDPF_ALPHAPIXELS
    write_u32(bytes, 88, 32u);
    write_u32(bytes, 92, 0x000000FFu);
    write_u32(bytes, 96, 0x0000FF00u);
    write_u32(bytes, 100, 0x00FF0000u);
    write_u32(bytes, 104, 0xFF000000u);
  }
  fill_payload(bytes, headerBytes);
  return bytes;
}

std::filesystem::path write_temp_dds(const std::vector<uint8_t>& bytes) {
  const auto path = std::filesystem::temp_directory_path() /
                    ("aurora-dds-align-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                     "-" + std::to_string(bytes.size()) + ".dds");
  std::ofstream file(path, std::ios::binary);
  file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  return path;
}

// The in-memory and file tails must agree for the same bytes and limit.
void expect_memory_file_parity(const std::vector<uint8_t>& bytes, uint32_t maxDimension) {
  const auto memory = dds::parse_dds_mip_tail({bytes.data(), bytes.size()}, maxDimension);
  const auto path = write_temp_dds(bytes);
  const auto file = dds::load_dds_mip_tail(path, maxDimension);
  std::error_code removeError;
  std::filesystem::remove(path, removeError);
  ASSERT_EQ(memory.has_value(), file.has_value());
  if (!memory.has_value()) {
    return;
  }
  EXPECT_EQ(file->includesBase, memory->includesBase);
  EXPECT_EQ(file->texture.format, memory->texture.format);
  EXPECT_EQ(file->texture.width, memory->texture.width);
  EXPECT_EQ(file->texture.height, memory->texture.height);
  EXPECT_EQ(file->texture.mips, memory->texture.mips);
  ASSERT_EQ(file->texture.data.size(), memory->texture.data.size());
  EXPECT_TRUE(std::equal(file->texture.data.data(), file->texture.data.data() + file->texture.data.size(),
                         memory->texture.data.data()));
}

TEST(DdsMipTailTest, CompressedTailWalksBackToBlockAlignedBase) {
  constexpr uint32_t Mips = 9;            // 256x8 down to 1x1
  constexpr size_t BaseLevelBytes = 2048; // 64 x 2 blocks of 16 bytes
  constexpr size_t TailBytes = 1040;      // 128x4 down: 512+256+128+64+32+16+16+16
  const auto bytes = make_dds(256, 8, Mips, wgpu::TextureFormat::BC7RGBAUnorm);
  ASSERT_EQ(bytes.size(), DdsDx10HeaderBytes + BaseLevelBytes + TailBytes);
  ASSERT_EQ(DdsDx10HeaderBytes + BaseLevelBytes, 2196u); // exact payload offset of the selected base

  const auto full = dds::parse_dds_bytes({bytes.data(), bytes.size()});
  ASSERT_TRUE(full.has_value());
  // 64 is gx::texture::ReplacementThumbnailDim: 64x2 is the first level within the limit, but 2 is not a whole
  // 4-texel block, so the base is 128x4 instead.
  const auto tail = dds::parse_dds_mip_tail({bytes.data(), bytes.size()}, 64);
  ASSERT_TRUE(tail.has_value());
  EXPECT_FALSE(tail->includesBase);
  EXPECT_EQ(tail->texture.format, wgpu::TextureFormat::BC7RGBAUnorm);
  EXPECT_EQ(tail->texture.width, 128u);
  EXPECT_EQ(tail->texture.height, 4u);
  EXPECT_EQ(tail->texture.mips, Mips - 1);
  EXPECT_EQ(tail->texture.data.size(), TailBytes);
  EXPECT_TRUE(std::equal(tail->texture.data.data(), tail->texture.data.data() + tail->texture.data.size(),
                         full->data.data() + BaseLevelBytes));
  expect_memory_file_parity(bytes, 64);
}

TEST(DdsMipTailTest, CompressedTailWalksBackForTallTexture) {
  constexpr uint32_t Mips = 9;            // 8x256 down to 1x1
  constexpr size_t BaseLevelBytes = 2048; // 2 x 64 blocks of 16 bytes
  constexpr size_t TailBytes = 1040;      // 4x128 down: 512+256+128+64+32+16+16+16
  const auto bytes = make_dds(8, 256, Mips, wgpu::TextureFormat::BC7RGBAUnorm);
  ASSERT_EQ(bytes.size(), DdsDx10HeaderBytes + BaseLevelBytes + TailBytes);

  const auto full = dds::parse_dds_bytes({bytes.data(), bytes.size()});
  ASSERT_TRUE(full.has_value());
  // 2x64 is the first level within the limit, but 2 is not a whole 4-texel block, so the base is 4x128 instead.
  const auto tail = dds::parse_dds_mip_tail({bytes.data(), bytes.size()}, 64);
  ASSERT_TRUE(tail.has_value());
  EXPECT_FALSE(tail->includesBase);
  EXPECT_EQ(tail->texture.width, 4u);
  EXPECT_EQ(tail->texture.height, 128u);
  EXPECT_EQ(tail->texture.mips, Mips - 1);
  EXPECT_EQ(tail->texture.data.size(), TailBytes);
  EXPECT_TRUE(std::equal(tail->texture.data.data(), tail->texture.data.data() + tail->texture.data.size(),
                         full->data.data() + BaseLevelBytes));
  expect_memory_file_parity(bytes, 64);
}

TEST(DdsMipTailTest, CompressedTailSelectsBlockAlignedLevelExceedingLimit) {
  constexpr size_t PrefixBytes = 5440; // 4096 + 1024 + 256 + 64 for 64x64, 32x32, 16x16, 8x8
  constexpr size_t TailBytes = 48;     // 4x4 + 2x2 + 1x1
  const auto bytes = make_dds(64, 64, 7, wgpu::TextureFormat::BC7RGBAUnorm);
  ASSERT_EQ(bytes.size(), DdsDx10HeaderBytes + PrefixBytes + TailBytes);

  const auto full = dds::parse_dds_bytes({bytes.data(), bytes.size()});
  ASSERT_TRUE(full.has_value());
  // 2x2 is within the limit but is not a whole block, so the 4x4 level above it is the base, exceeding the limit.
  const auto tail = dds::parse_dds_mip_tail({bytes.data(), bytes.size()}, 2);
  ASSERT_TRUE(tail.has_value());
  EXPECT_FALSE(tail->includesBase);
  EXPECT_EQ(tail->texture.width, 4u);
  EXPECT_EQ(tail->texture.height, 4u);
  EXPECT_EQ(tail->texture.mips, 3u);
  EXPECT_EQ(tail->texture.data.size(), TailBytes);
  EXPECT_TRUE(std::equal(tail->texture.data.data(), tail->texture.data.data() + tail->texture.data.size(),
                         full->data.data() + PrefixBytes));
  expect_memory_file_parity(bytes, 2);
}

TEST(DdsMipTailTest, UncompressedTailKeepsFirstLevelWithinLimit) {
  constexpr size_t PrefixBytes = 1200; // (20x12 + 10x6) * 4
  constexpr size_t TailBytes = 72;     // (5x3 + 2x1 + 1x1) * 4
  const auto bytes = make_dds(20, 12, 5, wgpu::TextureFormat::RGBA8Unorm);
  ASSERT_EQ(bytes.size(), DdsHeaderBytes + PrefixBytes + TailBytes);

  const auto full = dds::parse_dds_bytes({bytes.data(), bytes.size()});
  ASSERT_TRUE(full.has_value());
  // 5x3 is not a multiple of any block size, but uncompressed formats have no block alignment requirement.
  const auto tail = dds::parse_dds_mip_tail({bytes.data(), bytes.size()}, 8);
  ASSERT_TRUE(tail.has_value());
  EXPECT_FALSE(tail->includesBase);
  EXPECT_EQ(tail->texture.format, wgpu::TextureFormat::RGBA8Unorm);
  EXPECT_EQ(tail->texture.width, 5u);
  EXPECT_EQ(tail->texture.height, 3u);
  EXPECT_EQ(tail->texture.mips, 3u);
  EXPECT_EQ(tail->texture.data.size(), TailBytes);
  EXPECT_TRUE(std::equal(tail->texture.data.data(), tail->texture.data.data() + tail->texture.data.size(),
                         full->data.data() + PrefixBytes));
  expect_memory_file_parity(bytes, 8);
}

TEST(DdsMipTailTest, CompressedTailRejectsUnalignedBases) {
  // A malformed 7x7 base: no level of the chain is a whole 4x4 block, so there is no valid base to walk back to.
  const auto unalignedBase = make_dds(7, 7, 3, wgpu::TextureFormat::BC7RGBAUnorm);
  EXPECT_FALSE(dds::parse_dds_mip_tail({unalignedBase.data(), unalignedBase.size()}, 64).has_value());
  expect_memory_file_parity(unalignedBase, 64);

  // 5x5 down to 2x2: the first level within the limit is 2x2 and the level above it is unaligned too.
  const auto smallUnalignedBase = make_dds(5, 5, 3, wgpu::TextureFormat::BC7RGBAUnorm);
  EXPECT_FALSE(dds::parse_dds_mip_tail({smallUnalignedBase.data(), smallUnalignedBase.size()}, 4).has_value());
  expect_memory_file_parity(smallUnalignedBase, 4);

  // A single level larger than the limit has no level to walk back to (the previous no-fitting-mip behaviour).
  const auto singleLevel = make_dds(8, 8, 1, wgpu::TextureFormat::BC7RGBAUnorm);
  EXPECT_FALSE(dds::parse_dds_mip_tail({singleLevel.data(), singleLevel.size()}, 4).has_value());
  expect_memory_file_parity(singleLevel, 4);
}

TEST(DdsMipTailTest, TruncatedCompressedTailIsRejected) {
  auto bytes = make_dds(256, 8, 9, wgpu::TextureFormat::BC7RGBAUnorm);
  ASSERT_EQ(bytes.size(), DdsDx10HeaderBytes + 2048 + 1040);
  bytes.resize(bytes.size() - 1); // The aligned tail no longer fits in the file.
  EXPECT_FALSE(dds::parse_dds_mip_tail({bytes.data(), bytes.size()}, 64).has_value());
  expect_memory_file_parity(bytes, 64);
}
} // namespace
} // namespace aurora::gfx::texture_replacement
