// SPDX-License-Identifier: GPL-3.0-or-later
#include "port_cannon_repair.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>

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
        "only paired mesh/skin intercepted");
  if (argc < 2)
    return 0;
  const std::filesystem::path folder = argv[1];
  const auto m = Read(folder / "19AE626C.CMDL"), s = Read(folder / "7281CDBA.CSKR"),
             b = Read(folder / "46ED31E0.CINF");
  Check(Build(m, s, b, result), "rebuild canonical local retail assets");
  Check(result.triangles > 2000 && result.triangles < 2400, "full half replacement triangle count");
  Check(result.seamVertices > 10, "shared centreline vertices");
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
}
