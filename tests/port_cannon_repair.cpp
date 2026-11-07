// SPDX-License-Identifier: GPL-3.0-or-later
#include "port_cannon_repair.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>

void Check(bool value, const char* what) {
  if (!value) {
    std::fprintf(stderr, "cannon mirror: %s\n", what);
    std::abort();
  }
}
std::vector< uint8_t > Read(const std::filesystem::path& path) {
  std::ifstream f(path, std::ios::binary);
  return {(std::istreambuf_iterator< char >(f)), {}};
}
int main(int argc, char** argv) {
  using namespace PortCannonRepair;
  Result result;
  Check(!Build({}, {}, {}, result), "reject missing/replacement resources");
  Check(result.model.data.empty() && result.skin.data.empty(), "failure returns no partial pair");
  Check(IsResource(kModel) && IsResource(kSkin) && !IsResource(kSkeleton),
        "cannon pair intercepted without replacing skeleton");
  Check(IsResource(0x07111aa7) && IsResource(0x201e4b6f), "rigid suit arm resources intercepted");
  Check(!BuildArm(0x07111aa7, {}, result), "reject missing/replacement arm mesh");
  if (argc < 2)
    return 0;
  const std::filesystem::path folder = argv[1];
  const auto m = Read(folder / "19AE626C.CMDL"), s = Read(folder / "7281CDBA.CSKR"),
             b = Read(folder / "46ED31E0.CINF");
  Check(Build(m, s, b, result), "rebuild canonical local retail assets");
  Check(result.triangles > 2000 && result.triangles < 2400, "full half replacement triangle count");
  Check(result.seamVertices > 10, "shared centreline vertices");
  // The original has one bone per vertex. Rebuilding must not average opposite
  // hinge bones into a synthetic blend that welds opening panels together.
  auto word = [&](size_t at) {
    const auto& d = result.skin.data;
    return uint32_t(d.at(at)) << 24 | uint32_t(d.at(at + 1)) << 16 | uint32_t(d.at(at + 2)) << 8 |
           d.at(at + 3);
  };
  size_t skinAt = 4;
  auto modelWord = [&](size_t at) {
    const auto& d = result.model.data;
    return uint32_t(d.at(at)) << 24 | uint32_t(d.at(at + 1)) << 16 | uint32_t(d.at(at + 2)) << 8 |
           d.at(at + 3);
  };
  size_t positionsAt = (44 + modelWord(36) * 4 + 31) & ~size_t(31);
  for (size_t i = 0; i < modelWord(40); ++i)
    positionsAt += modelWord(44 + i * 4);
  size_t firstVertex = 0;
  using WingPositions = std::map< uint32_t, std::set< std::array< uint32_t, 3 > > >;
  WingPositions rebuiltWings;
  bool leftNozzleCrosses = false, rightNozzleCrosses = false;
  for (size_t i = 0; i < word(0); ++i) {
    Check(word(skinAt) == 1, "independent hinge bone influences, no left/right averaging");
    Check(word(skinAt + 8) == 0x3f800000, "original full bone weight preserved");
    const auto bone = word(skinAt + 4), count = word(skinAt + 12);
    for (size_t j = 0; j < count; ++j) {
      const auto bits = modelWord(positionsAt + (firstVertex + j) * 12);
      float x;
      std::memcpy(&x, &bits, 4);
      if (bone >= 19 && bone <= 22)
        rebuiltWings[bone].insert({bits, modelWord(positionsAt + (firstVertex + j) * 12 + 4),
                                   modelWord(positionsAt + (firstVertex + j) * 12 + 8)});
      leftNozzleCrosses |= bone == 20 && x > .0015f;
      rightNozzleCrosses |= bone == 19 && x < -.0015f;
    }
    firstVertex += count;
    skinAt += 16;
  }
  Check(leftNozzleCrosses && rightNozzleCrosses,
        "opening nozzle components retain authored corners crossing the body mirror plane");
  auto sourceWord = [](const std::vector< uint8_t >& bytes, size_t at) {
    return uint32_t(bytes.at(at)) << 24 | uint32_t(bytes.at(at + 1)) << 16 |
           uint32_t(bytes.at(at + 2)) << 8 | bytes.at(at + 3);
  };
  size_t sourcePositions = (44 + sourceWord(m, 36) * 4 + 31) & ~size_t(31);
  for (size_t i = 0; i < sourceWord(m, 40); ++i)
    sourcePositions += sourceWord(m, 44 + i * 4);
  size_t sourceSkin = 4, sourceVertex = 0;
  WingPositions originalWings;
  for (size_t i = 0; i < sourceWord(s, 0); ++i) {
    Check(sourceWord(s, sourceSkin) == 1, "original wing fixture has one bone per vertex");
    const auto bone = sourceWord(s, sourceSkin + 4), count = sourceWord(s, sourceSkin + 12);
    if (bone >= 19 && bone <= 22)
      for (size_t j = 0; j < count; ++j) {
        const size_t at = sourcePositions + (sourceVertex + j) * 12;
        originalWings[bone].insert(
            {sourceWord(m, at), sourceWord(m, at + 4), sourceWord(m, at + 8)});
      }
    sourceVertex += count;
    sourceSkin += 16;
  }
  Check(originalWings == rebuiltWings,
        "all four original wing shapes preserved, including asymmetric corners");
  std::printf("Mirrored cannon: %zu triangles, %zu seam vertices\n", result.triangles,
              result.seamVertices);
  std::ofstream(folder / "mirrored.CMDL", std::ios::binary)
      .write(reinterpret_cast< const char* >(result.model.data.data()), result.model.data.size());
  std::ofstream(folder / "mirrored.CSKR", std::ios::binary)
      .write(reinterpret_cast< const char* >(result.skin.data.data()), result.skin.data.size());
  auto changed = s;
  changed[8] ^= 1;
  Check(!Build(m, changed, b, result), "reject custom skin, not just custom geometry");
  Check(result.model.data.empty(), "no mismatched mesh after skin rejection");
  Reset();
  int reads = 0;
  auto reader = [&](uint32_t id, std::vector< uint8_t >& out) {
    ++reads;
    out = id == kModel ? m : id == kSkin ? s : b;
    return true;
  };
  const auto* mesh = Find(kModel, reader);
  const auto* skin = Find(kSkin, reader);
  Check(mesh && skin && reads == 3, "mesh-first loads a coherent cached pair");
  Reset();
  reads = 0;
  skin = Find(kSkin, reader);
  mesh = Find(kModel, reader);
  Check(mesh && skin && reads == 3, "skin-first loads the same coherent pair");
  Reset();
  for (const auto id : {0x07111aa7u, 0x81856809u, 0x4ad9bbacu, 0x57dc8b14u, 0x9c8058b1u,
                        0xd148f9bau, 0x1a142a1fu, 0x201e4b6fu}) {
    char name[32];
    std::snprintf(name, sizeof(name), "%08X.CMDL", id);
    const auto input = Read(folder / name);
    Check(BuildArm(id, input, result), "rebuild canonical suit arm");
    Check(result.skin.data.empty(), "rigid arms need no replacement bones or skin");
    Check(result.triangles > 200 && result.triangles < 500 && result.seamVertices > 0,
          "full arm half replacement");
    std::printf("Arm %08X: %zu triangles, %zu seam vertices\n", id, result.triangles,
                result.seamVertices);
    std::snprintf(name, sizeof(name), "%08X-mirrored.CMDL", id);
    std::ofstream(folder / name, std::ios::binary)
        .write(reinterpret_cast< const char* >(result.model.data.data()), result.model.data.size());
    auto custom = input;
    custom[12] ^= 1;
    Check(!BuildArm(id, custom, result), "custom/replacement arm stays unchanged");
    int armReads = 0;
    const auto* arm = Find(id, [&](uint32_t requested, std::vector< uint8_t >& out) {
      Check(requested == id, "arm does not request cannon or skeleton assets");
      ++armReads;
      out = input;
      return true;
    });
    Check(arm && armReads == 1, "independent arm cache");
    Check(Find(id,
               [&](uint32_t, std::vector< uint8_t >&) {
                 std::abort();
                 return false;
               }) == arm,
          "stable cached arm");
  }
  Reset();
}
