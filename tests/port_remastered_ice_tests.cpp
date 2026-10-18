// Kind 28 needs the source inputs its fragment shader reads, not just its shader
// id and CCH constants. Exercise its single-layer exception through the converter.

#include "port_pbr_record.h"
#include "port_remastered_convert.h"
#include "port_test.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace PortRemastered;

namespace {

constexpr uint32_t FourCC(char a, char b, char c, char d) {
  return (uint32_t(uint8_t(a)) << 24) | (uint32_t(uint8_t(b)) << 16) | (uint32_t(uint8_t(c)) << 8) | uint8_t(d);
}

ModelTextureRef TextureRef(uint32_t usage, uint8_t id) {
  ModelTextureRef ref;
  ref.usage = usage;
  ref.hasUsage = true;
  ref.id[0] = id;
  ref.id[15] = uint8_t(id + 1);
  ref.wrapX = ref.wrapY = 1;
  return ref;
}

ModelMaterialData Texture(uint32_t usage, uint8_t id) {
  ModelMaterialData d;
  d.usage = usage;
  d.kind = ModelMaterialData::Kind::Texture;
  d.texture = TextureRef(usage, id);
  return d;
}

ModelMaterialData Color(uint32_t usage, float r, float g, float b, float a) {
  ModelMaterialData d;
  d.usage = usage;
  d.kind = ModelMaterialData::Kind::Color;
  d.color[0] = r;
  d.color[1] = g;
  d.color[2] = b;
  d.color[3] = a;
  return d;
}

ModelMaterial IceMaterial() {
  ModelMaterial m;
  m.name = "ice-spreader";
  m.unk1 = 0x10;
  m.types.push_back(FourCC('R', 'L', 'T', 'G'));
  m.shaderId[0] = 0x08;
  m.shaderId[1] = 0x8E;
  m.shaderId[2] = 0x02;
  m.shaderId[3] = 0x5E;
  m.data = {
      Texture(FourCC('D', 'I', 'F', 'T'), 0x10), Texture(FourCC('M', 'E', 'T', 'L'), 0x20),
      Texture(FourCC('N', 'M', 'A', 'P'), 0x30), Texture(FourCC('T', 'C', 'H', '0'), 0x40),
      Texture(FourCC('R', 'E', 'F', 'L'), 0x50), Color(FourCC('C', 'C', 'H', '0'), 0.25f, 0.5f, 0.75f, 0.125f),
      Color(FourCC('C', 'C', 'H', '1'), 0.2f, 0.125f, 3.0f, 0.4f),
      Color(FourCC('C', 'C', 'H', '2'), 0.1f, 0.2f, 0.3f, 0.4f),
      Color(FourCC('C', 'C', 'H', '3'), 0.531f, 0.801f, 1.0f, 1.0f),
      Color(FourCC('C', 'C', 'H', '5'), 0.375f, 0.0f, 0.0f, 0.0f),
  };
  return m;
}

Model BuildModel(const ModelMaterial& material) {
  Model model;
  model.materials.push_back(material);
  ModelVertexBuffer vb;
  vb.vertexCount = 3;
  for (int v = 0; v < 3; ++v) {
    vb.positions.insert(vb.positions.end(), {float(v), float(v * v), 0.f});
    vb.normals.insert(vb.normals.end(), {0.f, 0.f, 1.f});
    if (vb.uvs.empty()) vb.uvs.resize(1);
    vb.uvs[0].insert(vb.uvs[0].end(), {float(v), 0.f});
  }
  model.vertexBuffers.push_back(vb);
  ModelMesh mesh;
  mesh.material = 0;
  mesh.vertexBuffer = 0;
  mesh.indices = {0, 1, 2};
  model.meshes.push_back(mesh);
  return model;
}

std::vector<uint8_t> RetailFixture() {
  ModelMaterial material;
  material.name = "retail-base";
  material.types.push_back(FourCC('R', 'L', 'T', 'G'));
  material.data.push_back(Texture(FourCC('D', 'I', 'F', 'T'), 0x70));
  std::vector<uint8_t> cmdl;
  ConvertIO io;
  io.texture = [](const ModelUuid&, Image& out, std::string&) {
    out.width = out.height = 4;
    out.rgba.assign(64, 200);
    return true;
  };
  io.write = [&cmdl](const std::string& name, const std::vector<uint8_t>& data) {
    if (name.size() > 5 && name.compare(name.size() - 5, 5, ".CMDL") == 0) cmdl = data;
    return true;
  };
  ConvertOptions opt;
  opt.standalone = true;
  opt.retail = 0xABCD1234;
  opt.skip.clear();
  Converter converter(io);
  std::string error;
  if (!converter.Convert(BuildModel(material), opt, error)) {
    std::fprintf(stderr, "FAIL: retail fixture conversion failed: %s\n", error.c_str());
    port_test::CountFailure();
  }
  return cmdl;
}

bool Convert(const ModelMaterial& material, std::vector<uint8_t>& cmdl, std::vector<MaterialDecision>& decisions,
             bool standalone = true) {
  std::vector<uint8_t> retailCmdl;
  ConvertIO io;
  io.texture = [](const ModelUuid&, Image& out, std::string&) {
    out.width = out.height = 4;
    out.rgba.assign(64, 200);
    return true;
  };
  io.cube = [](const ModelUuid&, uint32_t& edge, std::vector<float>& rgba, std::string&) {
    edge = 1;
    rgba.assign(24, 0.5f);
    return true;
  };
  if (!standalone) {
    retailCmdl = RetailFixture();
    io.retail = [&retailCmdl](uint32_t type, uint32_t id, std::vector<uint8_t>& out) {
      if (type != FourCC('C', 'M', 'D', 'L') || id != 0xABCD1234 || retailCmdl.empty()) return false;
      out = retailCmdl;
      return true;
    };
  }
  io.decision = [&decisions](const MaterialDecision& d) { decisions.push_back(d); };
  io.write = [&cmdl](const std::string& name, const std::vector<uint8_t>& data) {
    if (name.size() > 5 && name.compare(name.size() - 5, 5, ".CMDL") == 0) cmdl = data;
    return true;
  };
  ConvertOptions opt;
  opt.standalone = standalone;
  opt.retail = 0xABCD1234;
  opt.skip.clear();
  Converter converter(io);
  std::string error;
  if (converter.Convert(BuildModel(material), opt, error)) return true;
  std::fprintf(stderr, "FAIL: conversion failed: %s\n", error.c_str());
  port_test::CountFailure();
  return false;
}

uint32_t Be32(const std::vector<uint8_t>& d, size_t o) {
  return (uint32_t(d[o]) << 24) | (uint32_t(d[o + 1]) << 16) | (uint32_t(d[o + 2]) << 8) | d[o + 3];
}

float BeFloat(const std::vector<uint8_t>& d, size_t o) {
  const uint32_t bits = Be32(d, o);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

bool FirstBlob(const std::vector<uint8_t>& d, size_t& start, size_t& size) {
  if (d.size() < 48) return false;
  const uint32_t nsec = Be32(d, 36);
  if (nsec < 3 || 44 + 4 * size_t(nsec) > d.size()) return false;
  const size_t base = (44 + 4 * size_t(nsec) + 31) & ~size_t(31);
  if (base + 12 > d.size()) return false;
  const uint32_t ntex = Be32(d, base);
  const size_t nmats = Be32(d, base + 4 + 4 * size_t(ntex));
  if (nmats < 1 || base + 12 + 4 * size_t(ntex) + 4 * nmats > d.size()) return false;
  size = Be32(d, base + 8 + 4 * size_t(ntex));
  start = base + 8 + 4 * size_t(ntex) + 4 * nmats;
  return start + size <= d.size() && size >= 36;
}

std::string BlobTag(const std::vector<uint8_t>& d, size_t start, size_t size) {
  return std::string(reinterpret_cast<const char*>(&d[start + size - 4]), 4);
}

struct BlobInfo {
  std::string tag;
  uint32_t nmaps = 0;
  bool hasRows = false;
  float rows[32] = {};
  float kind = 0.f;
};

BlobInfo ReadBlob(const std::vector<uint8_t>& d, size_t start, size_t size) {
  BlobInfo info;
  info.nmaps = Be32(d, start + 4);
  info.tag = BlobTag(d, start, size);
  if (info.tag == "PBR8" && size >= 132) {
    info.hasRows = true;
    const size_t end = start + size;
    for (int i = 0; i < 32; ++i) info.rows[i] = BeFloat(d, end - 132 + 4 * size_t(i));
    const size_t pbr7Tag = end - 132 - 4;
    if (std::memcmp(&d[pbr7Tag], "PBR7", 4) == 0) {
      info.kind = BeFloat(d, pbr7Tag - 16 - 76 + 13 * 4);
    }
  }
  return info;
}

bool Near(float a, float b) { return std::fabs(a - b) < 1e-6f; }

void CheckFallback(const ModelMaterial& material, const char* label, bool standalone = true) {
  std::vector<uint8_t> cmdl;
  std::vector<MaterialDecision> decisions;
  if (!Convert(material, cmdl, decisions, standalone)) return;
  CHECK_MSG(decisions.size() == 1 && decisions[0].kind == 0, label);
  size_t start = 0, size = 0;
  const bool readable = FirstBlob(cmdl, start, size);
  CHECK_MSG(readable, "fallback material blob is readable");
  if (readable) CHECK_MSG(BlobTag(cmdl, start, size) != "PBR8", "fallback has no kind-28 PBR8 rows");
}

void TestCompleteSingleLayerIce() {
  std::vector<uint8_t> cmdl;
  std::vector<MaterialDecision> decisions;
  if (!Convert(IceMaterial(), cmdl, decisions)) return;
  CHECK_MSG(decisions.size() == 1, "complete ice: one decision");
  if (decisions.empty()) return;
  CHECK_MSG(decisions[0].kind == 28 && decisions[0].path == "pbr", "complete single-layer ice keeps kind 28 on PBR");
  CHECK_MSG(decisions[0].tag == "PBR8" && decisions[0].cube != "-", "ice retains its own reflection cube record");
  CHECK_MSG((decisions[0].mode & 32) != 0, "ice retains inverse-exposed emissive mode");
  size_t start = 0, size = 0;
  const bool readable = FirstBlob(cmdl, start, size);
  CHECK_MSG(readable, "complete ice: material blob is readable");
  if (!readable) return;
  const BlobInfo b = ReadBlob(cmdl, start, size);
  CHECK_MSG(b.nmaps == 4, "complete ice: single-layer PBR has four standard slots");
  CHECK_MSG(b.hasRows && Near(b.kind, 28.f), "complete ice: PBR8 contains kind 28");
  if (!b.hasRows) return;
  CHECK_MSG(Near(b.rows[0], .25f) && Near(b.rows[1], .5f) && Near(b.rows[2], .75f) && Near(b.rows[3], .125f),
            "ice: CCH0 row retained");
  CHECK_MSG(Near(b.rows[4], .2f) && Near(b.rows[5], .125f) && Near(b.rows[6], 3.f) && Near(b.rows[7], .4f),
            "ice: CCH1 rim gain and power retained");
  CHECK_MSG(Near(b.rows[8], .1f) && Near(b.rows[9], .2f) && Near(b.rows[10], .3f) && Near(b.rows[11], .375f),
            "ice: CCH2 cube tint and CCH5 cube gain retained");
  CHECK_MSG(Near(b.rows[12], .531f) && Near(b.rows[13], .801f) && Near(b.rows[14], 1.f),
             "ice: CCH3 inverse-exposure rim colour retained");
}

void TestMissingOptionalCch5DefaultsCubeGain() {
  ModelMaterial noCch5 = IceMaterial();
  bool removedCch5 = false;
  for (auto it = noCch5.data.begin(); it != noCch5.data.end(); ++it) {
    if (it->usage == FourCC('C', 'C', 'H', '5')) {
      noCch5.data.erase(it);
      removedCch5 = true;
      break;
    }
  }
  CHECK_MSG(removedCch5, "ice fixture: optional CCH5 is omitted");

  std::vector<uint8_t> cmdl;
  std::vector<MaterialDecision> decisions;
  if (!Convert(noCch5, cmdl, decisions)) return;
  CHECK_MSG(decisions.size() == 1 && decisions[0].kind == 28 && decisions[0].path == "pbr",
        "ice: missing optional CCH5 retains kind 28 on PBR");
  if (decisions.empty()) return;

  size_t start = 0, size = 0;
  const bool readable = FirstBlob(cmdl, start, size);
  CHECK_MSG(readable, "ice without CCH5: material blob is readable");
  if (!readable) return;
  const BlobInfo b = ReadBlob(cmdl, start, size);
  CHECK_MSG(b.nmaps == 4 && b.hasRows && Near(b.kind, 28.f),
            "ice without CCH5: complete required maps and CCH0-CCH3 keep kind 28 in PBR8");
  if (!b.hasRows) return;
  CHECK_MSG(Near(b.rows[0], .25f) && Near(b.rows[1], .5f) && Near(b.rows[2], .75f) && Near(b.rows[3], .125f) &&
                Near(b.rows[4], .2f) && Near(b.rows[5], .125f) && Near(b.rows[6], 3.f) && Near(b.rows[7], .4f) &&
                Near(b.rows[8], .1f) && Near(b.rows[9], .2f) && Near(b.rows[10], .3f) && Near(b.rows[12], .531f) &&
                Near(b.rows[13], .801f) && Near(b.rows[14], 1.f) && Near(b.rows[15], 1.f),
            "ice without CCH5: required CCH0-CCH3 remain in PBR8");
  CHECK_MSG(Near(b.rows[11], 1.f), "ice without CCH5: shieldRows[11] defaults cube gain to 1");
}

void TestNegativeControls() {
  const uint32_t required[] = {FourCC('D', 'I', 'F', 'T'), FourCC('M', 'E', 'T', 'L'), FourCC('N', 'M', 'A', 'P'),
                               FourCC('T', 'C', 'H', '0')};
  for (uint32_t usage : required) {
    ModelMaterial missing = IceMaterial();
    for (auto it = missing.data.begin(); it != missing.data.end(); ++it) {
      if (it->usage == usage) {
        missing.data.erase(it);
        break;
      }
    }
    CheckFallback(missing, "missing required ice map falls back to kind 0", usage != FourCC('D', 'I', 'F', 'T'));
  }
  for (uint32_t i = 0; i < 4; ++i) {
    ModelMaterial missingCch = IceMaterial();
    const uint32_t usage = FourCC('C', 'C', 'H', char('0' + i));
    for (auto it = missingCch.data.begin(); it != missingCch.data.end(); ++it) {
      if (it->usage == usage) {
        missingCch.data.erase(it);
        break;
      }
    }
    CheckFallback(missingCch, "missing required ice CCH0-CCH3 row falls back to kind 0");
  }
  {
    ModelMaterial wrongShader = IceMaterial();
    wrongShader.shaderId[3] = 0x5F;
    CheckFallback(wrongShader, "wrong shader falls back to kind 0");
  }
}

void TestLayeredIceUnchanged() {
  ModelMaterial layered = IceMaterial();
  // Preserve the historical layered kind-28 path even though it lacks the standard
  // normal/MR maps required by the new single-layer exception.
  for (auto it = layered.data.begin(); it != layered.data.end();) {
    if (it->usage == FourCC('M', 'E', 'T', 'L') || it->usage == FourCC('N', 'M', 'A', 'P')) {
      it = layered.data.erase(it);
    } else {
      ++it;
    }
  }
  ModelMaterialData bcrl;
  bcrl.usage = FourCC('B', 'C', 'R', 'L');
  bcrl.kind = ModelMaterialData::Kind::LayeredTexture;
  bcrl.layeredTextures[0] = TextureRef(FourCC('B', 'C', 'R', 'L'), 0x60);
  bcrl.layeredTextures[1] = TextureRef(FourCC('B', 'C', 'R', 'L'), 0x61);
  layered.data.push_back(bcrl);
  std::vector<uint8_t> cmdl;
  std::vector<MaterialDecision> decisions;
  if (!Convert(layered, cmdl, decisions)) return;
  CHECK_MSG(decisions.size() == 1 && decisions[0].kind == 28, "layered ice keeps its existing kind-28 behavior");
  size_t start = 0, size = 0;
  if (FirstBlob(cmdl, start, size)) CHECK_MSG(ReadBlob(cmdl, start, size).nmaps == 7, "layered ice still binds its second layer");
  else CHECK_MSG(false, "layered ice material blob is readable");
}

} // namespace

int main() {
  TestCompleteSingleLayerIce();
  TestMissingOptionalCch5DefaultsCubeGain();
  TestNegativeControls();
  TestLayeredIceUnchanged();
  return PORT_TEST_RESULT();
}
