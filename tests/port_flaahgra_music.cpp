#include "port_flaahgra_music.h"
#include "port_test.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace {

void Put32(std::vector<uint8_t>& out, size_t at, uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    out[at + i] = uint8_t(value >> (24 - i * 8));
  }
}

// A header for `samples` at 32 kHz, looping, as the discs' files have it, and the file's length.
std::vector<uint8_t> Header(uint32_t samples, uint32_t nibbles, uint32_t rate = 32000, uint8_t loop = 1,
                            uint8_t format = 0) {
  std::vector<uint8_t> head(PortFlaahgraMusic::kDspHeaderSize, 0);
  Put32(head, 0, samples);
  Put32(head, 4, nibbles);
  Put32(head, 8, rate);
  head[0xD] = loop;
  head[0xF] = format;
  Put32(head, 0x10, 2);
  Put32(head, 0x14, nibbles - 24);
  return head;
}

uint64_t FileSize(uint32_t nibbles) { return PortFlaahgraMusic::kDspHeaderSize + (uint64_t(nibbles) + 1) / 2; }

const char* Check(const std::vector<uint8_t>& head, uint64_t size, uint32_t& samples) {
  return PortFlaahgraMusic::CheckChannel(head.data(), head.size(), size, samples);
}

}  // namespace

int main() {
  uint32_t samples = 0;
  // The PAL disc's: 3671012 samples in 4195444 nibbles.
  REQUIRE(Check(Header(3671012, 4195444), FileSize(4195444), samples) == nullptr);
  REQUIRE(samples == 3671012);
  // The NTSC discs' shortened copy: 1159268 samples.
  REQUIRE(Check(Header(1159268, 1325000), FileSize(1325000), samples) != nullptr);
  REQUIRE(samples == 1159268);
  REQUIRE(Check(Header(1159269, 1325000), FileSize(1325000), samples) == nullptr);
  // Wrong rate, no loop, not ADPCM, cut short, longer than the header says.
  REQUIRE(Check(Header(3671012, 4195444, 48000), FileSize(4195444), samples) != nullptr);
  REQUIRE(Check(Header(3671012, 4195444, 32000, 0), FileSize(4195444), samples) != nullptr);
  REQUIRE(Check(Header(3671012, 4195444, 32000, 1, 1), FileSize(4195444), samples) != nullptr);
  REQUIRE(Check(Header(3671012, 4195444), FileSize(4195444) - 1, samples) != nullptr);
  REQUIRE(Check(Header(3671012, 4195444), FileSize(4195444) + 1, samples) != nullptr);
  // More samples than nibbles can hold.
  REQUIRE(Check(Header(4195445, 4195444), FileSize(4195444), samples) != nullptr);
  // Garbage and too little data.
  std::vector<uint8_t> junk(PortFlaahgraMusic::kDspHeaderSize, 0xA5);
  REQUIRE(Check(junk, junk.size(), samples) != nullptr);
  std::vector<uint8_t> zeros(PortFlaahgraMusic::kDspHeaderSize, 0);
  REQUIRE(Check(zeros, zeros.size(), samples) != nullptr);
  REQUIRE(PortFlaahgraMusic::CheckChannel(junk.data(), 8, 8, samples) != nullptr);
  REQUIRE(PortFlaahgraMusic::CheckChannel(nullptr, 0, 0, samples) != nullptr);
  // The channels must match.
  REQUIRE(PortFlaahgraMusic::CheckPair(3671012, 3671012) == nullptr);
  REQUIRE(PortFlaahgraMusic::CheckPair(3671012, 3671000) != nullptr);
  return 0;
}
