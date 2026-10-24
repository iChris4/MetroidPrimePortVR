#include "port_envfx_file.h"
#include "port_test.h"

#include <cstdio>
#include <vector>

namespace {
PortEnvFx::Image Make(uint32_t w, uint32_t h, uint32_t layers, uint8_t seed) {
  PortEnvFx::Image image;
  image.width = w;
  image.height = h;
  image.layers = layers;
  image.rgba.resize(size_t(w) * h * layers * 4);
  for (size_t i = 0; i < image.rgba.size(); ++i) {
    image.rgba[i] = uint8_t(i * 7 + seed);
  }
  return image;
}

PortEnvFx::Assets Make() {
  PortEnvFx::Assets assets;
  assets.gradient = Make(16, 1, 1, 1);
  assets.flake[PortEnvFx::kRain] = Make(32, 256, 64, 2);
  assets.flake[PortEnvFx::kSnow] = Make(64, 64, 64, 3);
  assets.flake[PortEnvFx::kUnderwater] = Make(64, 64, 1, 4);
  return assets;
}

void TestRoundTrip() {
  const PortEnvFx::Assets assets = Make();
  const std::vector<uint8_t> file = PortEnvFx::Serialize(assets);
  PortEnvFx::Assets back;
  std::string error;
  CHECK_MSG(PortEnvFx::Parse(file.data(), file.size(), back, error), error);
  CHECK(back.gradient.rgba == assets.gradient.rgba);
  for (int i = 0; i < 3; ++i) {
    CHECK_EQ(back.flake[i].width, assets.flake[i].width);
    CHECK_EQ(back.flake[i].height, assets.flake[i].height);
    CHECK_EQ(back.flake[i].layers, assets.flake[i].layers);
    CHECK(back.flake[i].rgba == assets.flake[i].rgba);
  }
}

void TestRefusals() {
  const std::vector<uint8_t> file = PortEnvFx::Serialize(Make());
  PortEnvFx::Assets out;
  std::string error;
  CHECK_MSG(!PortEnvFx::Parse(file.data(), file.size() - 1, out, error), "a truncated file is refused");
  std::vector<uint8_t> trailing = file;
  trailing.push_back(0);
  CHECK_MSG(!PortEnvFx::Parse(trailing.data(), trailing.size(), out, error), "trailing bytes are refused");
  std::vector<uint8_t> magic = file;
  magic[0] ^= 1;
  CHECK_MSG(!PortEnvFx::Parse(magic.data(), magic.size(), out, error), "a wrong magic is refused");
  std::vector<uint8_t> version = file;
  version[4] = 2;
  CHECK_MSG(!PortEnvFx::Parse(version.data(), version.size(), out, error), "a wrong version is refused");
  PortEnvFx::Assets odd = Make();
  odd.flake[PortEnvFx::kSnow] = Make(32, 32, 64, 3);
  const std::vector<uint8_t> oddFile = PortEnvFx::Serialize(odd);
  CHECK_MSG(!PortEnvFx::Parse(oddFile.data(), oddFile.size(), out, error), "unexpected dimensions are refused");
  CHECK_MSG(!PortEnvFx::Parse(nullptr, 0, out, error), "no data is refused");
}
}  // namespace

int main() {
  TestRoundTrip();
  TestRefusals();
  if (port_test::Failures() == 0) {
    std::printf("port_envfx_file: all tests passed\n");
  }
  return PORT_TEST_RESULT();
}
