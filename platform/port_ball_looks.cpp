// The generated ball looks' image work: CMPR decoding, the HSV fits and
// recolour, and the CMDL material set copy (port_ball_looks.h). The disc
// reading and the cache are in port_ball_looks_disc.cpp.

#include "port_ball_looks.h"

#include "port_bytes.h"
#include "port_model_variant.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>

namespace PortBallLooks {
namespace {

constexpr uint32_t kTXTR = 0x54585452;
constexpr uint32_t kCMDL = 0x434D444C;
constexpr uint32_t kANCS = 0x414E4353;

// A coloured texel: a hue means something.
constexpr double kMinSaturation = 0.2;
constexpr double kMinValue = 0.15;

constexpr double kPi = 3.14159265358979323846;  // MSVC has no M_PI

// Written by PortMods::Initialize, read by the game and the settings UI.
std::atomic<bool> sRequested{false};
std::atomic<unsigned> sActive{0};

uint8_t Expand5(uint32_t n) { return uint8_t((n << 3) | (n >> 2)); }
uint8_t Expand6(uint32_t n) { return uint8_t((n << 2) | (n >> 4)); }

struct Hsv {
  double h = 0.0;  // degrees, 0..360
  double s = 0.0;
  double v = 0.0;
};

Hsv ToHsv(const uint8_t* rgb) {
  const double r = rgb[0] / 255.0;
  const double g = rgb[1] / 255.0;
  const double b = rgb[2] / 255.0;
  const double max = std::max({r, g, b});
  const double min = std::min({r, g, b});
  const double range = max - min;
  Hsv out;
  out.v = max;
  out.s = max > 0.0 ? range / max : 0.0;
  if (range > 0.0) {
    if (max == r) {
      out.h = 60.0 * std::fmod((g - b) / range + 6.0, 6.0);
    } else if (max == g) {
      out.h = 60.0 * ((b - r) / range + 2.0);
    } else {
      out.h = 60.0 * ((r - g) / range + 4.0);
    }
  }
  return out;
}

void FromHsv(const Hsv& hsv, uint8_t* rgb) {
  const double h = std::fmod(std::fmod(hsv.h, 360.0) + 360.0, 360.0) / 60.0;
  const double c = hsv.v * hsv.s;
  const double x = c * (1.0 - std::fabs(std::fmod(h, 2.0) - 1.0));
  const double m = hsv.v - c;
  double r = 0.0, g = 0.0, b = 0.0;
  switch (int(h)) {
  case 0: r = c, g = x; break;
  case 1: r = x, g = c; break;
  case 2: g = c, b = x; break;
  case 3: g = x, b = c; break;
  case 4: r = x, b = c; break;
  default: r = c, b = x; break;
  }
  const auto byte = [](double value) { return uint8_t(std::clamp(std::lround(value * 255.0), 0l, 255l)); };
  rgb[0] = byte(r + m);
  rgb[1] = byte(g + m);
  rgb[2] = byte(b + m);
}

bool Opaque(const uint8_t* texel) { return texel[3] >= 128; }

bool Differ(const uint8_t* a, const uint8_t* b, int threshold) {
  for (int c = 0; c < 3; ++c) {
    if (std::abs(int(a[c]) - int(b[c])) > threshold) {
      return true;
    }
  }
  return false;
}

double Median(std::vector<double>& values) {
  const auto middle = values.begin() + values.size() / 2;
  std::nth_element(values.begin(), middle, values.end());
  return *middle;
}

}  // namespace

const char* Name(Look look) {
  switch (look) {
  case Look::PowerSpider: return "power spider ball";
  case Look::Gravity: return "gravity ball";
  case Look::Phazon: return "phazon ball";
  }
  return "";
}

int Suit(Look look) {
  switch (look) {
  case Look::PowerSpider: return 0;
  case Look::Gravity: return 1;
  case Look::Phazon: return 3;
  }
  return 0;
}

bool DecodeCmprTxtr(const std::vector<uint8_t>& txtr, PortRemastered::Image& out) {
  // Header: u32 format, u16 width, u16 height, u32 mip count; then the levels.
  if (txtr.size() < 12 || port::GetBE32(txtr, 0) != 10) {
    return false;
  }
  const int width = int(port::ReadBE16(txtr.data() + 4));
  const int height = int(port::ReadBE16(txtr.data() + 6));
  if (width == 0 || height == 0 || width % 8 != 0 || height % 8 != 0 ||
      txtr.size() < 12 + size_t(width) * size_t(height) / 2) {
    return false;
  }
  out = {};
  out.width = width;
  out.height = height;
  out.rgba.assign(size_t(width) * size_t(height) * 4, 0);
  // 8x8 tiles, each four 4x4 blocks: two RGB565 colours, then 2-bit indices
  // a row per byte, the leftmost texel in the top bits.
  const uint8_t* src = txtr.data() + 12;
  for (int ty = 0; ty < height; ty += 8) {
    for (int tx = 0; tx < width; tx += 8) {
      for (int block = 0; block < 4; ++block, src += 8) {
        const uint32_t c0 = port::ReadBE16(src);
        const uint32_t c1 = port::ReadBE16(src + 2);
        std::array<std::array<uint8_t, 4>, 4> colours{};
        colours[0] = {Expand5(c0 >> 11), Expand6((c0 >> 5) & 63), Expand5(c0 & 31), 255};
        colours[1] = {Expand5(c1 >> 11), Expand6((c1 >> 5) & 63), Expand5(c1 & 31), 255};
        for (int c = 0; c < 3; ++c) {
          const uint32_t a = colours[0][c];
          const uint32_t b = colours[1][c];
          if (c0 > c1) {
            // Index 2 is the one nearer colour 0.
            colours[2][c] = uint8_t((5 * a + 3 * b) >> 3);
            colours[3][c] = uint8_t((3 * a + 5 * b) >> 3);
          } else {
            colours[2][c] = uint8_t((a + b) >> 1);
            colours[3][c] = colours[2][c];
          }
        }
        colours[2][3] = 255;
        colours[3][3] = c0 > c1 ? 255 : 0;
        const int bx = tx + (block & 1) * 4;
        const int by = ty + (block >> 1) * 4;
        for (int y = 0; y < 4; ++y) {
          for (int x = 0; x < 4; ++x) {
            const auto& colour = colours[(src[4 + y] >> (6 - 2 * x)) & 3];
            std::copy(colour.begin(), colour.end(), out.rgba.begin() + (size_t(by + y) * width + (bx + x)) * 4);
          }
        }
      }
    }
  }
  return true;
}

Shift FitShift(const PortRemastered::Image& from, const PortRemastered::Image& to, int threshold) {
  Shift shift;
  if (from.width != to.width || from.height != to.height || from.rgba.size() != to.rgba.size()) {
    return shift;
  }
  std::vector<double> hues, saturations, values;
  for (size_t at = 0; at + 4 <= from.rgba.size(); at += 4) {
    const uint8_t* a = &from.rgba[at];
    const uint8_t* b = &to.rgba[at];
    if (!Opaque(a) || !Opaque(b) || !Differ(a, b, threshold)) {
      continue;
    }
    const Hsv ha = ToHsv(a);
    const Hsv hb = ToHsv(b);
    if (ha.s < kMinSaturation || hb.s < kMinSaturation || ha.v < kMinValue || hb.v < kMinValue) {
      continue;
    }
    hues.push_back(std::remainder(hb.h - ha.h, 360.0));  // the short way round, -180..180
    saturations.push_back(hb.s - ha.s);
    values.push_back(hb.v - ha.v);
  }
  if (hues.empty()) {
    return shift;
  }
  shift.hue = Median(hues);
  shift.saturation = Median(saturations);
  shift.value = Median(values);
  shift.samples = hues.size();
  return shift;
}

Stats ColourStats(const PortRemastered::Image& image, const PortRemastered::Image* other, int threshold) {
  Stats stats;
  if (other != nullptr &&
      (image.width != other->width || image.height != other->height || image.rgba.size() != other->rgba.size())) {
    return stats;
  }
  std::vector<double> hues, saturations, values;
  double sine = 0.0, cosine = 0.0;
  for (size_t at = 0; at + 4 <= image.rgba.size(); at += 4) {
    const uint8_t* texel = &image.rgba[at];
    if (!Opaque(texel) ||
        (other != nullptr && (!Opaque(&other->rgba[at]) || !Differ(texel, &other->rgba[at], threshold)))) {
      continue;
    }
    const Hsv hsv = ToHsv(texel);
    if (hsv.v < kMinValue) {
      continue;
    }
    saturations.push_back(hsv.s);
    values.push_back(hsv.v);
    if (hsv.s >= kMinSaturation) {
      hues.push_back(hsv.h);
      sine += std::sin(hsv.h * kPi / 180.0);
      cosine += std::cos(hsv.h * kPi / 180.0);
    }
  }
  if (values.empty()) {
    return stats;
  }
  stats.saturation = Median(saturations);
  stats.value = Median(values);
  stats.samples = values.size();
  if (!hues.empty()) {
    // The median of the offsets from the mean direction, so a region round
    // red (350 and 10) is not taken for cyan.
    const double mean = std::atan2(sine, cosine) * 180.0 / kPi;
    for (double& hue : hues) {
      hue = std::remainder(hue - mean, 360.0);
    }
    stats.hue = std::fmod(mean + Median(hues) + 720.0, 360.0);
    stats.hueSamples = hues.size();
  }
  return stats;
}

Shift StatsShift(const Stats& from, const Stats& to) {
  Shift shift;
  if (from.samples == 0 || to.samples == 0) {
    return shift;
  }
  // Without coloured texels on both sides a hue means nothing: keep it.
  if (from.hueSamples != 0 && to.hueSamples != 0) {
    shift.hue = std::remainder(to.hue - from.hue, 360.0);
  }
  shift.saturation = to.saturation - from.saturation;
  shift.value = to.value - from.value;
  shift.samples = std::min(from.samples, to.samples);
  return shift;
}

size_t Recolour(PortRemastered::Image& image, const PortRemastered::Image& other, const Shift& shift,
                int threshold) {
  if (image.width != other.width || image.height != other.height || image.rgba.size() != other.rgba.size()) {
    return 0;
  }
  size_t changed = 0;
  for (size_t at = 0; at + 4 <= image.rgba.size(); at += 4) {
    uint8_t* texel = &image.rgba[at];
    if (!Opaque(texel) || !Opaque(&other.rgba[at]) || !Differ(texel, &other.rgba[at], threshold)) {
      continue;
    }
    Hsv hsv = ToHsv(texel);
    hsv.h += shift.hue;
    hsv.s = std::clamp(hsv.s + shift.saturation, 0.0, 1.0);
    hsv.v = std::clamp(hsv.v + shift.value, 0.0, 1.0);
    FromHsv(hsv, texel);
    ++changed;
  }
  return changed;
}

bool CopyMaterialSet(std::vector<uint8_t>& cmdl, uint32_t from, uint32_t to, const std::map<uint32_t, uint32_t>& swap) {
  // Header: magic, version, flags, AABB (6 floats), section count, material
  // set count, section sizes; the data starts 32-byte aligned with the
  // material sets, each starting with its texture count and ids.
  if (cmdl.size() < 44 || port::GetBE32(cmdl, 0) != 0xDEADBABE) {
    return false;
  }
  const uint64_t sections = port::GetBE32(cmdl, 36);
  const uint64_t sets = port::GetBE32(cmdl, 40);
  if (from >= sets || to >= sets || sets > sections || 44 + 4 * sections > cmdl.size()) {
    return false;
  }
  const uint64_t dataStart = (44 + 4 * sections + 31) & ~uint64_t(31);
  std::vector<uint64_t> starts(sections), sizes(sections);
  uint64_t at = dataStart;
  for (uint64_t i = 0; i < sections; ++i) {
    starts[i] = at;
    sizes[i] = port::GetBE32(cmdl, 44 + 4 * i);
    at += sizes[i];
  }
  if (at > cmdl.size() || sizes[from] < 4) {
    return false;
  }
  std::vector<uint8_t> out(cmdl.begin(), cmdl.begin() + dataStart);
  port::SetBE32(out, 44 + 4 * to, uint32_t(sizes[from]));
  uint64_t setStart = 0;
  for (uint64_t i = 0; i < sections; ++i) {
    const uint64_t source = i == to ? from : i;
    if (i == to) {
      setStart = out.size();
    }
    out.insert(out.end(), cmdl.begin() + starts[source], cmdl.begin() + starts[source] + sizes[source]);
  }
  out.insert(out.end(), cmdl.begin() + at, cmdl.end());
  const uint64_t count = port::GetBE32(out, setStart);
  if (4 + 4 * count > sizes[from]) {
    return false;
  }
  for (uint64_t i = 0; i < count; ++i) {
    const auto found = swap.find(port::GetBE32(out, setStart + 4 + 4 * i));
    if (found != swap.end()) {
      port::SetBE32(out, setStart + 4 + 4 * i, found->second);
    }
  }
  cmdl = std::move(out);
  return true;
}

std::vector<std::pair<uint32_t, uint32_t>> ModelKeys(Look look) {
  const uint32_t suit = uint32_t(Suit(look));
  std::vector<std::pair<uint32_t, uint32_t>> keys;
  if (look == Look::PowerSpider) {
    for (const auto& [type, id] : {std::pair{kANCS, kSpiderAncs}, std::pair{kCMDL, kSpiderCmdl},
                                   std::pair{kCMDL, kLowPolyCmdl}, std::pair{kCMDL, kGlassCmdl}}) {
      keys.emplace_back(type, id);
      keys.emplace_back(type, PortModelVariant::Id(id, suit));
    }
    keys.emplace_back(kTXTR, PortModelVariant::Id(kSpiderVaria, suit));
    keys.emplace_back(kTXTR, PortModelVariant::Id(kLowPolyVaria, suit));
  } else {
    for (const auto& [type, id] :
         {std::pair{kANCS, kBallAncs}, std::pair{kCMDL, kBallCmdl}, std::pair{kCMDL, kBallLowPolyCmdl}}) {
      keys.emplace_back(type, id);
      keys.emplace_back(type, PortModelVariant::Id(id, suit));
    }
    keys.emplace_back(kTXTR, PortModelVariant::Id(kBallVaria, suit));
  }
  return keys;
}

void SetState(bool requested, unsigned activeLooks) {
  sRequested.store(requested);
  sActive.store(activeLooks);
}

bool Requested() { return sRequested.load(); }
bool Active(Look look) { return (sActive.load() >> unsigned(look)) & 1u; }

}  // namespace PortBallLooks
