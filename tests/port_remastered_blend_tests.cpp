// How a Remastered mesh is blended: its class (ModelMesh::bits2), not a material flag.
// A synthetic model with three single-mesh materials is run through PortRemastered::Converter
// and the GX blend factors of the CMDL's materials are read back: an opaque one writes
// (One, Zero), a transparent one of class 1 (SrcAlpha, InvSrcAlpha) and one of class 3 (SrcAlpha, One).

#include "port_remastered_convert.h"
#include "port_test.h"

#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace PortRemastered;

namespace {

constexpr uint32_t FourCC(char a, char b, char c, char d) {
  return (uint32_t(uint8_t(a)) << 24) | (uint32_t(uint8_t(b)) << 16) | (uint32_t(uint8_t(c)) << 8) | uint8_t(d);
}

ModelMaterial Material(const char* name, uint32_t flags) {
  ModelMaterial material;
  material.name = name;
  material.unk1 = flags;
  material.types.push_back(FourCC('R', 'L', 'T', 'G'));
  ModelMaterialData d;
  d.usage = FourCC('D', 'I', 'F', 'T');
  d.kind = ModelMaterialData::Kind::Texture;
  d.texture.usage = d.usage;
  d.texture.hasUsage = true;
  d.texture.id[3] = 0x11;
  d.texture.wrapX = 1;
  d.texture.wrapY = 1;
  material.data.push_back(d);
  return material;
}

struct Surface {
  uint32_t flags;
  uint8_t cls;
};

Model BuildModel(const std::vector<Surface>& surfaces) {
  Model model;
  ModelVertexBuffer vb;
  vb.vertexCount = 3;
  for (int v = 0; v < 3; ++v) {
    vb.positions.insert(vb.positions.end(), {float(v), float(v * v), 0.f});
    vb.normals.insert(vb.normals.end(), {0.f, 0.f, 1.f});
  }
  vb.uvs.resize(1);
  for (int v = 0; v < 3; ++v) {
    vb.uvs[0].insert(vb.uvs[0].end(), {float(v), 0.f});
  }
  model.vertexBuffers.push_back(vb);
  for (size_t i = 0; i < surfaces.size(); ++i) {
    model.materials.push_back(Material(("blendtest" + std::to_string(i)).c_str(), surfaces[i].flags));
    ModelMesh mesh;
    mesh.material = uint32_t(i);
    mesh.vertexBuffer = 0;
    mesh.bits2 = surfaces[i].cls;
    mesh.indices = {0, 1, 2};
    model.meshes.push_back(mesh);
  }
  return model;
}

bool Convert(const Model& model, std::vector<uint8_t>& cmdl) {
  ConvertIO io;
  io.texture = [](const ModelUuid&, Image& out, std::string&) {
    out.width = out.height = 4;
    out.rgba.assign(64, 200);
    return true;
  };
  io.write = [&cmdl](const std::string& name, const std::vector<uint8_t>& data) {
    if (name.size() > 5 && name.compare(name.size() - 5, 5, ".CMDL") == 0) {
      cmdl = data;
    }
    return true;
  };
  ConvertOptions opt;
  opt.standalone = true;
  opt.retail = 0xABCD1234;
  opt.skip.clear();
  Converter converter(io);
  std::string error;
  if (!converter.Convert(model, opt, error)) {
    std::fprintf(stderr, "FAIL: the conversion failed: %s\n", error.c_str());
    port_test::CountFailure();
    return false;
  }
  return true;
}

uint32_t Be32(const std::vector<uint8_t>& d, size_t o) {
  return (uint32_t(d[o]) << 24) | (uint32_t(d[o + 1]) << 16) | (uint32_t(d[o + 2]) << 8) | d[o + 3];
}

uint32_t Be16(const std::vector<uint8_t>& d, size_t o) { return (uint32_t(d[o]) << 8) | d[o + 1]; }

// The (source, destination) factors of every material in the CMDL, in order. A material
// is its flags, the texture count and indices, the vertex descriptor and the group id
// (no konst colours here), then the destination and source factors as 16 bit words.
bool Factors(const std::vector<uint8_t>& d, std::vector<std::pair<uint32_t, uint32_t>>& out) {
  if (d.size() < 48) {
    return false;
  }
  const uint32_t nsec = Be32(d, 36);
  if (nsec < 3 || 44 + 4 * size_t(nsec) > d.size()) {
    return false;
  }
  const size_t base = (44 + 4 * size_t(nsec) + 31) & ~size_t(31);
  const uint32_t ntex = Be32(d, base);
  const size_t countAt = base + 4 + 4 * size_t(ntex);
  const uint32_t n = Be32(d, countAt);
  const size_t ends = countAt + 4;
  const size_t first = ends + 4 * size_t(n);
  if (first > d.size()) {
    return false;
  }
  uint32_t begin = 0;
  for (uint32_t i = 0; i < n; ++i) {
    const size_t m = first + begin;
    const uint32_t flags = Be32(d, m);
    if (flags & 0x8) {
      return false;  // konst colours: not a layout this test reads
    }
    const uint32_t nt = Be32(d, m + 4);
    const size_t at = m + 8 + 4 * size_t(nt) + 8;
    if (at + 4 > d.size()) {
      return false;
    }
    out.emplace_back(Be16(d, at + 2), Be16(d, at));
    begin = Be32(d, ends + 4 * size_t(i));
  }
  return true;
}

void TestClasses() {
  std::vector<uint8_t> cmdl;
  if (!Convert(BuildModel({{0, 0}, {0x1, 1}, {0x1, 3}}), cmdl)) {
    return;
  }
  std::vector<std::pair<uint32_t, uint32_t>> f;
  if (!Factors(cmdl, f)) {
    CHECK_MSG(false, "the converted CMDL's materials are readable");
    return;
  }
  CHECK_MSG(f.size() == 3, "three surfaces, three materials");
  const std::set<std::pair<uint32_t, uint32_t>> got(f.begin(), f.end());
  CHECK_MSG(got.count({1, 0}), "an opaque surface blends (One, Zero)");
  CHECK_MSG(got.count({4, 5}), "a class 1 transparent surface blends (SrcAlpha, InvSrcAlpha)");
  CHECK_MSG(got.count({4, 1}), "a class 3 transparent surface blends (SrcAlpha, One)");
  CHECK_MSG(got.size() == 3, "each class has factors of its own");
}

// Lit glass of Remastered's premultiplied families (here 11B30369) adds its own colour:
// (One, InvSrcAlpha), however its mesh class says to blend.
void TestPremultipliedGlass() {
  Model model = BuildModel({{0x1, 1}});
  model.materials[0].shaderId[0] = 0x11;
  model.materials[0].shaderId[1] = 0xB3;
  model.materials[0].shaderId[2] = 0x03;
  model.materials[0].shaderId[3] = 0x69;
  std::vector<uint8_t> cmdl;
  if (!Convert(model, cmdl)) {
    return;
  }
  std::vector<std::pair<uint32_t, uint32_t>> f;
  CHECK_MSG(Factors(cmdl, f) && f.size() == 1, "the glass's material is readable");
  CHECK_MSG((f.size() == 1 && f[0] == std::pair<uint32_t, uint32_t>(1, 5)), "premultiplied glass blends (One, InvSrcAlpha)");
}

// Glass_DX11 (03407341, the Waste Disposal tank) with its distortion map and colours is
// drawn over the screen copy, premultiplied, whatever its mesh class says.
void TestHoloGlass() {
  Model model = BuildModel({{0x100053, 1}});
  ModelMaterial& m = model.materials[0];
  m.shaderId[0] = 0x03;
  m.shaderId[1] = 0x40;
  m.shaderId[2] = 0x73;
  m.shaderId[3] = 0x41;
  ModelMaterialData tch2 = m.data[0];
  tch2.usage = tch2.texture.usage = FourCC('T', 'C', 'H', '2');
  tch2.texture.id[3] = 0x22;
  m.data.push_back(tch2);
  for (int i = 0; i < 4; ++i) {
    ModelMaterialData c;
    c.usage = FourCC('C', 'C', 'H', char('0' + i));
    c.kind = ModelMaterialData::Kind::Color;
    c.color[0] = c.color[1] = c.color[2] = c.color[3] = 0.5f;
    m.data.push_back(c);
  }
  std::vector<uint8_t> cmdl;
  if (!Convert(model, cmdl)) {
    return;
  }
  std::vector<std::pair<uint32_t, uint32_t>> f;
  CHECK_MSG(Factors(cmdl, f) && f.size() == 1, "the tank glass's material is readable");
  CHECK_MSG((f.size() == 1 && f[0] == std::pair<uint32_t, uint32_t>(1, 5)), "the tank glass blends (One, InvSrcAlpha)");
}

}  // namespace

int main() {
  TestClasses();
  TestPremultipliedGlass();
  TestHoloGlass();
  if (port_test::Failures() == 0) {
    std::printf("port_remastered_blend_tests: ok\n");
  }
  return PORT_TEST_RESULT();
}
