// The generated ball looks' image work on synthetic data: the CMPR decode,
// the HSV fit, the colour statistics and the masked recolour, the CMDL
// material set copy, and each look's keys.

#include "port_ball_looks.h"
#include "port_test.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <tuple>

namespace {

using PortRemastered::Image;
namespace PSB = PortBallLooks;

void PutBE16(std::vector<uint8_t>& out, uint32_t value) {
  out.push_back(uint8_t(value >> 8));
  out.push_back(uint8_t(value));
}

void PutBE32(std::vector<uint8_t>& out, uint32_t value) {
  PutBE16(out, value >> 16);
  PutBE16(out, value & 0xFFFF);
}

uint32_t GetBE32(const std::vector<uint8_t>& in, size_t at) {
  return uint32_t(in[at]) << 24 | uint32_t(in[at + 1]) << 16 | uint32_t(in[at + 2]) << 8 | in[at + 3];
}

template < typename... Args >
std::string Format(const char* format, Args... args) {
  char text[128];
  std::snprintf(text, sizeof(text), format, args...);
  return text;
}

const uint8_t* Texel(const Image& image, int x, int y) { return &image.rgba[(size_t(y) * image.width + x) * 4]; }

bool TexelIs(const Image& image, int x, int y, int r, int g, int b, int a) {
  const uint8_t* t = Texel(image, x, y);
  return t[0] == r && t[1] == g && t[2] == b && t[3] == a;
}

void TestDecode() {
  // One 8x8 tile: four 4x4 blocks.
  std::vector<uint8_t> txtr;
  PutBE32(txtr, 10);
  PutBE16(txtr, 8);
  PutBE16(txtr, 8);
  PutBE32(txtr, 1);
  const auto block = [&](uint32_t c0, uint32_t c1, uint8_t row) {
    PutBE16(txtr, c0);
    PutBE16(txtr, c1);
    for (int y = 0; y < 4; ++y) {
      txtr.push_back(row);
    }
  };
  block(0xF800, 0x001F, 0x00);  // top left: all colour 0, red
  block(0x001F, 0xF800, 0xFF);  // top right: three-colour mode's colour 3, transparent
  block(0xF800, 0x001F, 0xAA);  // bottom left: colour 2 of four, 5/8 red 3/8 blue
  block(0xF800, 0x001F, 0x1B);  // bottom right: a row of colours 0, 1, 2, 3
  Image image;
  REQUIRE(PSB::DecodeCmprTxtr(txtr, image));
  CHECK_EQ(image.width, 8);
  CHECK_EQ(image.height, 8);
  CHECK(TexelIs(image, 0, 0, 255, 0, 0, 255));
  CHECK(TexelIs(image, 3, 3, 255, 0, 0, 255));
  CHECK(TexelIs(image, 5, 1, 127, 0, 127, 0));
  CHECK(TexelIs(image, 2, 6, 159, 0, 95, 255));
  CHECK(TexelIs(image, 4, 4, 255, 0, 0, 255));
  CHECK(TexelIs(image, 5, 4, 0, 0, 255, 255));
  CHECK(TexelIs(image, 6, 4, 159, 0, 95, 255));
  CHECK(TexelIs(image, 7, 4, 95, 0, 159, 255));

  std::vector<uint8_t> other = txtr;
  other[3] = 0;  // I4, not CMPR
  CHECK(!PSB::DecodeCmprTxtr(other, image));
  other = txtr;
  other.resize(40);  // truncated
  CHECK(!PSB::DecodeCmprTxtr(other, image));
}

// The port's own CMPR encoder picks palette entries by distance, so what it
// writes must decode back near the source (19 at worst here; a swapped palette gives 64).
void TestDecodeRoundTrip() {
  Image source;
  source.width = 8;
  source.height = 8;
  for (int y = 0; y < 8; ++y) {
    for (int x = 0; x < 8; ++x) {
      const int ramp = (x % 4) * 60 + (y % 4) * 5;
      source.rgba.insert(source.rgba.end(), {uint8_t(40 + ramp), uint8_t(200 - ramp / 2), 60, 255});
    }
  }
  Image image;
  REQUIRE(PSB::DecodeCmprTxtr(PortRemastered::EncodeTxtrCmpr(source, false), image));
  int worst = 0;
  for (size_t at = 0; at < source.rgba.size(); ++at) {
    worst = std::max(worst, std::abs(int(source.rgba[at]) - int(image.rgba[at])));
  }
  CHECK_MSG(worst <= 24, Format("worst channel error %d", worst).c_str());
}

Image Blank(int width, int height, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  Image image;
  image.width = width;
  image.height = height;
  for (int i = 0; i < width * height; ++i) {
    image.rgba.insert(image.rgba.end(), {r, g, b, a});
  }
  return image;
}

// Saturated, mid-bright colours, so a modest shift neither clamps nor loses its hue.
Image Coloured(int width, int height, unsigned seed) {
  std::mt19937 random(seed);
  Image image = Blank(width, height, 0, 0, 0, 255);
  for (size_t at = 0; at < image.rgba.size(); at += 4) {
    const int high = 140 + int(random() % 80);
    const int low = 20 + int(random() % 40);
    const int middle = low + int(random() % (high - low));
    const int order = int(random() % 6);
    const int channels[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
    image.rgba[at + channels[order][0]] = uint8_t(high);
    image.rgba[at + channels[order][1]] = uint8_t(middle);
    image.rgba[at + channels[order][2]] = uint8_t(low);
  }
  return image;
}

void TestFit() {
  const Image from = Coloured(32, 32, 7);
  Image to = from;
  const Image black = Blank(32, 32, 0, 0, 0, 255);
  PSB::Shift shift;
  shift.hue = 40.0;
  shift.saturation = -0.1;
  shift.value = 0.05;
  CHECK_EQ(PSB::Recolour(to, black, shift), size_t(32 * 32));
  const PSB::Shift fit = PSB::FitShift(from, to);
  CHECK_EQ(fit.samples, size_t(32 * 32));
  CHECK_MSG(std::fabs(fit.hue - 40.0) < 1.5, Format("hue %f", fit.hue).c_str());
  CHECK_MSG(std::fabs(fit.saturation + 0.1) < 0.02, Format("saturation %f", fit.saturation).c_str());
  CHECK_MSG(std::fabs(fit.value - 0.05) < 0.01, Format("value %f", fit.value).c_str());

  // Hue across 0: a shift of -30 comes back as -30, not +330.
  Image back = from;
  shift = {};
  shift.hue = -30.0;
  PSB::Recolour(back, black, shift);
  CHECK_MSG(std::fabs(PSB::FitShift(from, back).hue + 30.0) < 1.5, Format("hue %f", PSB::FitShift(from, back).hue).c_str());

  // Grey, transparent or matching texels don't count; nothing left gives no fit.
  const Image grey = Blank(32, 32, 128, 128, 128, 255);
  CHECK_EQ(PSB::FitShift(grey, Blank(32, 32, 200, 200, 200, 255)).samples, size_t(0));
  CHECK_EQ(PSB::FitShift(from, from).samples, size_t(0));
  Image clear = to;
  for (size_t at = 3; at < clear.rgba.size(); at += 4) {
    clear.rgba[at] = 0;
  }
  CHECK_EQ(PSB::FitShift(from, clear).samples, size_t(0));
  CHECK_EQ(PSB::FitShift(from, Blank(16, 16, 0, 0, 0, 255)).samples, size_t(0));
}

void TestRecolourMask() {
  // Only the texels that differ from the other look move.
  const Image original = Coloured(16, 16, 11);
  Image other = original;
  for (int y = 0; y < 16; ++y) {
    for (int x = 8; x < 16; ++x) {
      uint8_t* t = &other.rgba[(size_t(y) * 16 + x) * 4];
      t[0] = uint8_t(255 - t[0]);
      t[1] = uint8_t(255 - t[1]);
      t[2] = uint8_t(255 - t[2]);
    }
  }
  // Transparent texels stay too.
  Image image = original;
  image.rgba[(15 * 16 + 15) * 4 + 3] = 0;
  PSB::Shift shift;
  shift.hue = 90.0;
  const size_t changed = PSB::Recolour(image, other, shift);
  size_t moved = 0;
  bool leftKept = true;
  for (int y = 0; y < 16; ++y) {
    for (int x = 0; x < 16; ++x) {
      const uint8_t* a = Texel(image, x, y);
      const uint8_t* b = Texel(original, x, y);
      const bool same = a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
      if (x < 8) {
        leftKept = leftKept && same;
      } else if (!same) {
        ++moved;
      }
    }
  }
  CHECK(leftKept);
  CHECK_EQ(changed, size_t(8 * 16 - 1));
  CHECK_EQ(moved, changed);
  CHECK_EQ(PSB::Recolour(image, Blank(8, 8, 0, 0, 0, 255), shift), size_t(0));
}

// A CMDL with `sizes` sections (the first `sets` are material sets); each
// section's bytes count up from its index * 16, and a set starts with
// its texture count and ids.
std::vector<uint8_t> Cmdl(const std::vector<uint32_t>& sizes, uint32_t sets,
                          const std::vector<std::vector<uint32_t>>& textures) {
  std::vector<uint8_t> out;
  PutBE32(out, 0xDEADBABE);
  PutBE32(out, 2);
  PutBE32(out, 0);
  for (int i = 0; i < 6; ++i) {
    PutBE32(out, 0);
  }
  PutBE32(out, uint32_t(sizes.size()));
  PutBE32(out, sets);
  for (uint32_t size : sizes) {
    PutBE32(out, size);
  }
  out.resize((out.size() + 31) & ~size_t(31), 0);
  for (size_t i = 0; i < sizes.size(); ++i) {
    std::vector<uint8_t> section;
    if (i < textures.size()) {
      PutBE32(section, uint32_t(textures[i].size()));
      for (uint32_t id : textures[i]) {
        PutBE32(section, id);
      }
    }
    for (uint8_t n = uint8_t(i * 16); section.size() < sizes[i]; ++n) {
      section.push_back(n);
    }
    out.insert(out.end(), section.begin(), section.end());
  }
  return out;
}

void TestCopyMaterialSet() {
  const std::vector<uint8_t> model = Cmdl({64, 32, 96, 32}, 2, {{0x111, 0x222, 0x333}, {0x444}});
  std::vector<uint8_t> copy = model;
  REQUIRE(PSB::CopyMaterialSet(copy, 1, 0, {{0x444, 0x999}, {0x111, 0x888}}));
  // The header with set 0's new size, set 1 with the swapped id, then sets 1 on unchanged.
  std::vector<uint8_t> expected(model.begin(), model.begin() + 64);
  expected[47] = 32;
  expected.insert(expected.end(), model.begin() + 128, model.begin() + 160);
  expected[71] = 0x99;
  expected[70] = 0x09;
  expected.insert(expected.end(), model.begin() + 128, model.end());
  CHECK_EQ(copy.size(), model.size() - 32);
  CHECK(copy.size() == expected.size());
  for (size_t at = 0; at < expected.size() && at < copy.size(); ++at) {
    if (copy[at] != expected[at]) {
      CHECK_MSG(false, Format("byte %zu: %02X, want %02X", at, copy[at], expected[at]).c_str());
      break;
    }
  }
  CHECK_EQ(GetBE32(copy, 44), uint32_t(32));
  CHECK_EQ(GetBE32(copy, 64 + 4), uint32_t(0x999));
  CHECK_EQ(GetBE32(copy, 64 + 32 + 4), uint32_t(0x444));

  // Trailing bytes past the sections are kept.
  std::vector<uint8_t> tail = model;
  tail.insert(tail.end(), {1, 2, 3});
  REQUIRE(PSB::CopyMaterialSet(tail, 1, 0));
  CHECK_EQ(tail.size(), model.size() - 32 + 3);
  CHECK_EQ(tail.back(), uint8_t(3));

  // Not a CMDL, a set it lacks, or truncated: false and untouched.
  for (auto [bytes, from, to] : {std::tuple{model, 1u, 2u}, std::tuple{model, 5u, 0u},
                                 std::tuple{std::vector<uint8_t>(model.begin(), model.end() - 1), 1u, 0u},
                                 std::tuple{std::vector<uint8_t>(model.begin(), model.begin() + 40), 1u, 0u}}) {
    const std::vector<uint8_t> before = bytes;
    CHECK(!PSB::CopyMaterialSet(bytes, from, to));
    CHECK(bytes == before);
  }
  std::vector<uint8_t> wrong = model;
  wrong[0] = 0;
  CHECK(!PSB::CopyMaterialSet(wrong, 1, 0));
  // A texture count running past its set.
  std::vector<uint8_t> overrun = model;
  overrun[64 + 64 + 3] = 100;
  const std::vector<uint8_t> before = overrun;
  CHECK(!PSB::CopyMaterialSet(overrun, 1, 0));
  CHECK(overrun == before);
}

// Hue taken round the circle, the grey and dark texels where they belong,
// and the mask.
void TestColourStats() {
  Image image;
  image.width = 9;
  image.height = 1;
  image.rgba = {
      255, 0,   43,  255,  // hue 349.9
      255, 0,   43,  255,  //
      255, 0,   0,   255,  // 0
      255, 43,  0,   255,  // 10.1
      255, 43,  0,   255,  //
      128, 128, 128, 255,  // grey: saturation and value, no hue
      10,  0,   0,   255,  // dark: left out
      0,   255, 0,   0,    // transparent: left out
      255, 0,   0,   255,  // 0
  };
  PSB::Stats stats = PSB::ColourStats(image);
  CHECK_EQ(stats.samples, size_t(7));
  CHECK_EQ(stats.hueSamples, size_t(6));
  // The mean sits near 0, so the median is red, not the linear median of
  // 0..360 (10).
  CHECK_MSG(std::min(stats.hue, 360.0 - stats.hue) < 1.0, Format("hue %.2f", stats.hue).c_str());
  CHECK(std::fabs(stats.saturation - 1.0) < 1e-9);
  CHECK(std::fabs(stats.value - 1.0) < 1e-9);

  // Masked: only the texels that differ from `other`.
  Image other = image;
  for (int x : {3, 4}) {
    uint8_t* t = &other.rgba[size_t(x) * 4];
    t[0] = 0, t[1] = 0, t[2] = 255;
  }
  stats = PSB::ColourStats(image, &other);
  CHECK_EQ(stats.samples, size_t(2));
  CHECK_EQ(stats.hueSamples, size_t(2));
  CHECK(std::fabs(stats.hue - 10.12) < 0.1);

  Image smaller = image;
  smaller.width = 8;
  smaller.rgba.resize(32);
  CHECK_EQ(PSB::ColourStats(image, &smaller).samples, size_t(0));
  Image dark;
  dark.width = 8;
  dark.height = 1;
  dark.rgba.assign(32, 0);
  stats = PSB::ColourStats(dark);
  CHECK_EQ(stats.samples, size_t(0));
  CHECK_EQ(stats.hueSamples, size_t(0));
}

void TestStatsShift() {
  PSB::Stats from;
  from.hue = 350.0, from.saturation = 0.9, from.value = 0.9, from.samples = 10, from.hueSamples = 10;
  PSB::Stats to;
  to.hue = 10.0, to.saturation = 0.5, to.value = 0.3, to.samples = 5, to.hueSamples = 5;
  PSB::Shift shift = PSB::StatsShift(from, to);
  CHECK(std::fabs(shift.hue - 20.0) < 1e-9);  // the short way round
  CHECK(std::fabs(shift.saturation + 0.4) < 1e-9);
  CHECK(std::fabs(shift.value + 0.6) < 1e-9);
  CHECK_EQ(shift.samples, size_t(5));
  CHECK(std::fabs(PSB::StatsShift(to, from).hue + 20.0) < 1e-9);

  // No coloured texels on one side: the hue is kept.
  to.hueSamples = 0;
  shift = PSB::StatsShift(from, to);
  CHECK_EQ(shift.hue, 0.0);
  CHECK(std::fabs(shift.value + 0.6) < 1e-9);
  // No texels at all: nothing to apply.
  to.samples = 0;
  CHECK_EQ(PSB::StatsShift(from, to).samples, size_t(0));
}

void TestKeys() {
  CHECK_EQ(PSB::ModelKeys(PSB::Look::PowerSpider).size(), size_t(10));
  CHECK_EQ(PSB::ModelKeys(PSB::Look::Gravity).size(), size_t(7));
  CHECK_EQ(PSB::ModelKeys(PSB::Look::Phazon).size(), size_t(7));
  for (const PSB::Look look : PSB::kLooks) {
    const auto keys = PSB::ModelKeys(look);
    for (size_t i = 0; i < keys.size(); ++i) {
      for (size_t j = i + 1; j < keys.size(); ++j) {
        CHECK(keys[i] != keys[j]);
      }
    }
  }
  // The Gravity and Phazon balls share the retail ball they are made from,
  // never a generated id.
  const auto gravity = PSB::ModelKeys(PSB::Look::Gravity);
  const auto phazon = PSB::ModelKeys(PSB::Look::Phazon);
  const auto spider = PSB::ModelKeys(PSB::Look::PowerSpider);
  size_t shared = 0;
  for (const auto& key : gravity) {
    shared += std::count(phazon.begin(), phazon.end(), key);
    CHECK(std::count(spider.begin(), spider.end(), key) == 0);
  }
  CHECK_EQ(shared, size_t(3));
  for (const auto& key : phazon) {
    CHECK(std::count(spider.begin(), spider.end(), key) == 0);
  }
  CHECK_EQ(PSB::Suit(PSB::Look::Gravity), 1);
  CHECK_EQ(PSB::Suit(PSB::Look::Phazon), 3);
}

void TestState() {
  PSB::SetState(true, 1u << unsigned(PSB::Look::Phazon));
  CHECK(PSB::Requested());
  CHECK(PSB::Active(PSB::Look::Phazon));
  CHECK(!PSB::Active(PSB::Look::Gravity));
  CHECK(!PSB::Active(PSB::Look::PowerSpider));
  PSB::SetState(false, 0);
  CHECK(!PSB::Requested());
  CHECK(!PSB::Active(PSB::Look::Phazon));
}

}  // namespace

int main() {
  TestDecode();
  TestDecodeRoundTrip();
  TestFit();
  TestRecolourMask();
  TestCopyMaterialSet();
  TestColourStats();
  TestStatsShift();
  TestKeys();
  TestState();
  if (port_test::Failures() == 0) {
    std::printf("port_ball_looks: all passed\n");
  }
  return PORT_TEST_RESULT();
}
