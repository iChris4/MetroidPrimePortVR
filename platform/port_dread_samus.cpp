// The dread-samus import's core: Dread suit bodies -> re-skin -> suit materials -> converter.
// See port_dread_samus.h.

#include "port_dread_samus.h"

#include "port_dread_bcmdl.h"
#include "port_dread_bctex.h"
#include "port_dread_bsmat.h"
#include "port_dread_pkg.h"
#include "port_remastered_convert.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <set>

namespace PortDreadSamus {

namespace {

constexpr uint32_t FourCC(char a, char b, char c, char d) {
  return (uint32_t(uint8_t(a)) << 24) | (uint32_t(uint8_t(b)) << 16) | (uint32_t(uint8_t(c)) << 8) | uint8_t(d);
}

uint32_t Be32(const std::vector<uint8_t>& d, size_t o) {
  return (uint32_t(d[o]) << 24) | (uint32_t(d[o + 1]) << 16) | (uint32_t(d[o + 2]) << 8) | d[o + 3];
}

float BeF(const std::vector<uint8_t>& d, size_t o) {
  const uint32_t u = Be32(d, o);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

std::string Hex8(uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof(b), "%08X", v);
  return b;
}

bool Fail(std::string* error, std::string message) {
  if (error != nullptr) {
    *error = std::move(message);
  }
  return false;
}

}  // namespace

bool RetailSkinFromCmdl(const std::vector<uint8_t>& m, const std::vector<uint8_t>& k, PortDread::RetailSkinRef& out,
                        std::string* error) {
  out = {};
  if (m.size() < 0x2c || Be32(m, 0) != 0xDEADBABE || k.size() < 4) {
    return Fail(error, "retail body: not a CMDL and CSKR");
  }
  const uint32_t nsec = Be32(m, 0x24), nmat = Be32(m, 0x28);
  if (nmat >= nsec || 0x2c + 4ull * nsec > m.size()) {
    return Fail(error, "retail body: bad section table");
  }
  size_t o = (0x2c + 4 * size_t(nsec) + 31) & ~size_t(31);
  for (uint32_t i = 0; i < nmat; ++i) {
    o += Be32(m, 0x2c + 4 * size_t(i));
  }
  const size_t psize = Be32(m, 0x2c + 4 * size_t(nmat));
  if (o + psize > m.size()) {
    return Fail(error, "retail body: positions past the end");
  }
  size_t ko = 4;
  const uint32_t groups = Be32(k, 0);
  for (uint32_t g = 0; g < groups; ++g) {
    if (ko + 4 > k.size()) {
      return Fail(error, "retail skin: cut short");
    }
    const uint32_t nw = Be32(k, ko);
    ko += 4;
    if (nw > 64 || ko + 8ull * nw + 4 > k.size()) {
      return Fail(error, "retail skin: cut short");
    }
    std::vector<std::pair<uint32_t, float>> ws;
    for (uint32_t i = 0; i < nw; ++i, ko += 8) {
      ws.emplace_back(Be32(k, ko), BeF(k, ko + 4));
    }
    const uint32_t count = Be32(k, ko);
    ko += 4;
    if (out.weights.size() + count > psize / 12 + 1) {
      return Fail(error, "retail skin: more vertices than the model");
    }
    out.weights.insert(out.weights.end(), count, ws);
  }
  const size_t n = std::min(out.weights.size(), psize / 12);
  out.weights.resize(n);
  out.pos.reserve(n * 3);
  for (size_t i = 0; i < n * 3; ++i) {
    out.pos.push_back(BeF(m, o + 4 * i));
  }
  return n > 0 ? true : Fail(error, "retail body: no skinned vertices");
}

bool BuildModel(const PortDread::ReskinResult& reskin, const std::vector<const PortDread::SuitMaterial*>& meshMaterials,
                PortRemastered::Model& out, std::vector<std::vector<std::vector<std::pair<uint32_t, float>>>>& boneWeights,
                std::string* error) {
  using namespace PortRemastered;
  out = {};
  boneWeights.clear();
  out.form = FourCC('C', 'M', 'D', 'L');
  out.skinned = true;
  float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
  std::map<const PortDread::SuitMaterial*, uint32_t> matIndex;
  for (size_t mi = 0; mi < reskin.meshes.size(); ++mi) {
    const PortDread::ReskinMesh& rm = reskin.meshes[mi];
    const PortDread::SuitMaterial* sm = mi < meshMaterials.size() ? meshMaterials[mi] : nullptr;
    if (sm == nullptr || rm.vertex_count == 0) {
      continue;
    }
    if (rm.pos.size() != size_t(rm.vertex_count) * 3 || rm.normal.size() != rm.pos.size() ||
        rm.weights.size() != rm.vertex_count) {
      return Fail(error, "mesh " + rm.name + ": inconsistent arrays");
    }
    auto [it, added] = matIndex.emplace(sm, uint32_t(out.materials.size()));
    if (added) {
      out.materials.push_back(sm->material);
    }
    const uint32_t buffer = uint32_t(out.vertexBuffers.size());
    ModelVertexBuffer& vb = out.vertexBuffers.emplace_back();
    vb.vertexCount = rm.vertex_count;
    vb.positions = rm.pos;
    vb.normals = rm.normal;
    if (rm.tangent.size() == size_t(rm.vertex_count) * 4) {
      vb.tangents = rm.tangent;
    }
    size_t sets = 0;
    for (size_t s = 0; s < 3; ++s) {
      if (rm.uv[s].size() == size_t(rm.vertex_count) * 2) {
        sets = s + 1;
      }
    }
    vb.uvs.resize(std::max<size_t>(sets, 1));
    for (size_t s = 0; s < vb.uvs.size(); ++s) {
      vb.uvs[s] = rm.uv[s].size() == size_t(rm.vertex_count) * 2 ? rm.uv[s]
                                                                   : std::vector<float>(size_t(rm.vertex_count) * 2, 0.f);
    }
    for (uint32_t v = 0; v < rm.vertex_count; ++v) {
      for (int a = 0; a < 3; ++a) {
        lo[a] = std::min(lo[a], rm.pos[size_t(v) * 3 + a]);
        hi[a] = std::max(hi[a], rm.pos[size_t(v) * 3 + a]);
      }
    }
    boneWeights.resize(buffer + 1);
    boneWeights[buffer] = rm.weights;
    for (const PortDread::ReskinSubmesh& sub : rm.submeshes) {
      if (sub.indices.size() < 3) {
        continue;
      }
      for (uint32_t i : sub.indices) {
        if (i >= rm.vertex_count) {
          return Fail(error, "mesh " + rm.name + ": index past its vertices");
        }
      }
      ModelMesh& mesh = out.meshes.emplace_back();
      mesh.material = it->second;
      mesh.vertexBuffer = buffer;
      mesh.indexBuffer = buffer;
      mesh.indexCount = uint32_t(sub.indices.size() / 3 * 3);
      mesh.indexWidth = 4;
      mesh.vertexCount = rm.vertex_count;
      mesh.bits2 = 0;
      mesh.indices.assign(sub.indices.begin(), sub.indices.begin() + mesh.indexCount);
    }
  }
  if (out.meshes.empty()) {
    return Fail(error, "no mesh with a suit material");
  }
  for (int a = 0; a < 3; ++a) {
    out.boundsMin[a] = lo[a];
    out.boundsMax[a] = hi[a];
  }
  return true;
}

bool FitBall(const PortDread::Bcmdl& source, const std::vector<uint8_t>& retailCmdl,
             uint32_t joint, PortDread::ReskinResult& out, std::string* error) {
  out = {};
  if (retailCmdl.size() < 44 || Be32(retailCmdl, 0) != 0xDEADBABE)
    return Fail(error, "ball fit: bad retail CMDL");
  const PortDread::BcmdlMesh* shell = nullptr;
  for (const auto& mesh : source.meshes) {
    if (!mesh.enabled) continue;
    if (shell) return Fail(error, "ball fit: more than one enabled shell");
    shell = &mesh;
  }
  if (!shell || shell->vertices.count == 0 || shell->submeshes.empty() ||
      shell->vertices.normal.size() != size_t(shell->vertices.count) * 3)
    return Fail(error, "ball fit: missing shell or normals");
  std::vector<PortDread::Mat4> world;
  if (!PortDread::ComputeWorldBind(source, world, error)) return false;
  PortDread::ReskinMesh mesh;
  mesh.name = shell->name;
  mesh.bsmat_path = shell->bsmat_path;
  mesh.vertex_count = shell->vertices.count;
  mesh.normal = shell->vertices.normal;
  mesh.tangent = shell->vertices.tangent;
  for (int i = 0; i < 3; ++i) mesh.uv[i] = shell->vertices.uv[i];
  for (const auto& sub : shell->submeshes) {
    std::vector<float> positions;
    if (!PortDread::BindPoseModelPositions(source, world, *shell, sub, positions, error)) return false;
    if (!mesh.pos.empty() && positions != mesh.pos)
      return Fail(error, "ball fit: submeshes disagree on bind positions");
    mesh.pos = std::move(positions);
    if (uint64_t(sub.first_index) + sub.index_count > shell->indices.size() || sub.index_count % 3)
      return Fail(error, "ball fit: bad triangle range");
    auto& triangles = mesh.submeshes.emplace_back().indices;
    triangles.assign(shell->indices.begin() + sub.first_index,
                     shell->indices.begin() + sub.first_index + sub.index_count);
  }
  // Rigid bone-local shells need their bind rotation applied to normals/tangents too.
  for (const auto& sub : shell->submeshes) {
    if (sub.skin_type != shell->submeshes.front().skin_type ||
        sub.joint_map != shell->submeshes.front().joint_map)
      return Fail(error, "ball fit: inconsistent normal spaces");
  }
  const auto& first = shell->submeshes.front();
  if (first.skin_type == 1) {
    const auto& vertices = shell->vertices;
    if (vertices.weights.size() != size_t(mesh.vertex_count) * 4 || vertices.joints.size() != vertices.weights.size())
      return Fail(error, "ball fit: missing rigid source weights");
    for (uint32_t v = 0; v < mesh.vertex_count; ++v) {
      int bind = -1;
      for (int k = 0; k < 4; ++k) {
        const size_t i = size_t(v) * 4 + k;
        if (vertices.weights[i] == 0) continue;
        const int j = vertices.joints[i];
        if (bind != -1 || std::fabs(vertices.weights[i] - 1.f) > 1e-5f || j < 0 || size_t(j) >= first.joint_map.size())
          return Fail(error, "ball fit: shell is not rigid");
        bind = first.joint_map[j];
      }
      if (bind < 0 || size_t(bind) >= world.size()) return Fail(error, "ball fit: bad source joint");
      const auto& matrix = world[bind];
      for (int a = 0; a < 3; ++a) for (int b = 0; b < 3; ++b) {
        float dot = 0;
        for (int r = 0; r < 3; ++r) dot += matrix.m[r][a] * matrix.m[r][b];
        if (std::fabs(dot - (a == b ? 1.f : 0.f)) > 1e-3f)
          return Fail(error, "ball fit: non-rigid bind transform");
      }
      auto normal = [&](float* n) {
        const float old[3] = {n[0],n[1],n[2]};
        for (int a = 0; a < 3; ++a) {
          n[a] = 0;
          for (int b = 0; b < 3; ++b) n[a] += matrix.m[a][b] * old[b];
        }
      };
      normal(&mesh.normal[size_t(v)*3]);
      if (mesh.tangent.size() == size_t(mesh.vertex_count)*4) normal(&mesh.tangent[size_t(v)*4]);
    }
  } else if (first.skin_type != 2) {
    return Fail(error, "ball fit: unsupported shell normal space");
  }
  float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
  for (size_t v = 0; v < mesh.pos.size(); v += 3) {
    for (int a = 0; a < 3; ++a) {
      if (!std::isfinite(mesh.pos[v + a])) return Fail(error, "ball fit: non-finite position");
      lo[a] = std::min(lo[a], mesh.pos[v + a]);
      hi[a] = std::max(hi[a], mesh.pos[v + a]);
    }
  }
  float sourceSize = 0, targetSize = 0, center[3];
  for (int a = 0; a < 3; ++a) {
    const float l = BeF(retailCmdl, 12 + 4 * a), h = BeF(retailCmdl, 24 + 4 * a);
    if (!std::isfinite(l) || !std::isfinite(h) || h <= l)
      return Fail(error, "ball fit: bad retail bounds");
    center[a] = (l + h) * 0.5f;
    sourceSize = std::max(sourceSize, hi[a] - lo[a]);
    targetSize = std::max(targetSize, h - l);
  }
  if (!(sourceSize > 0)) return Fail(error, "ball fit: degenerate shell");
  out.scale = targetSize / sourceSize;
  // Dread Y-up to Prime Z-up, same proper rotation as the converter's default.
  auto rotate = [](float* p) { const float x = p[0], y = p[1]; p[0] = -x; p[1] = p[2]; p[2] = y; };
  for (size_t v = 0; v < mesh.pos.size(); v += 3) {
    for (int a = 0; a < 3; ++a) mesh.pos[v + a] = (mesh.pos[v + a] - (lo[a] + hi[a]) * 0.5f) * out.scale;
    rotate(&mesh.pos[v]);
    for (int a = 0; a < 3; ++a) mesh.pos[v + a] += center[a];
    rotate(&mesh.normal[v]);
    if (mesh.tangent.size() == size_t(mesh.vertex_count) * 4) rotate(&mesh.tangent[v / 3 * 4]);
  }
  mesh.weights.assign(mesh.vertex_count, {{joint, 1.f}});
  out.meshes.push_back(std::move(mesh));
  return true;
}

bool Import(const Io& io, Result& result, std::string& error) {
  result = {};
  auto log = [&](const std::string& line) {
    if (io.log) {
      io.log(line);
    }
  };
  auto cancelled = [&] { return io.cancelled && io.cancelled(); };

  std::vector<uint8_t> pkgData;
  if (!io.dread("packs/players/samus.pkg", pkgData, error)) {
    error = "cannot read packs/players/samus.pkg: " + error;
    return false;
  }
  PortDread::Pkg pkg;
  if (!pkg.Parse(pkgData.data(), pkgData.size(), error, pkgData.size())) {
    error = "samus.pkg: " + error;
    return false;
  }
  auto pkgFile = [&](const std::string& path, std::vector<uint8_t>& out) {
    const PortDread::PkgEntry* e = pkg.Find(path);
    if (e == nullptr || e->end > pkgData.size() || e->start > e->end) {
      return false;
    }
    out.assign(pkgData.begin() + e->start, pkgData.begin() + e->end);
    return true;
  };

  // Suit materials, one per BSMAT path; null where the BSMAT does not build (not mp_samuspbr,
  // a texture missing): its meshes are left out.
  std::map<std::string, std::unique_ptr<PortDread::SuitMaterial>> materials;
  auto material = [&](const std::string& bsmatPath) -> const PortDread::SuitMaterial* {
    auto found = materials.find(bsmatPath);
    if (found != materials.end()) {
      return found->second.get();
    }
    std::unique_ptr<PortDread::SuitMaterial> built;
    std::string why;
    std::vector<uint8_t> data;
    PortDread::Bsmat bsmat;
    std::string bc, at, nm;
    if (!pkgFile(bsmatPath, data)) {
      why = "not in samus.pkg";
    } else if (!PortDread::ParseBsmat(data.data(), data.size(), bsmat, &why) ||
               !PortDread::SuitTexturePaths(bsmat, "textures", bc, at, nm, &why)) {
    } else {
      PortDread::Bctex tex[3];
      const std::string* paths[3] = {&bc, &at, &nm};
      bool ok = true;
      for (int i = 0; i < 3 && ok; ++i) {
        std::vector<uint8_t> bytes;
        std::string readError;
        if (!io.dread(*paths[i], bytes, readError)) {
          why = "cannot read " + *paths[i] + ": " + readError;
          ok = false;
        } else if (!tex[i].Parse(bytes.data(), bytes.size(), readError)) {
          why = *paths[i] + ": " + readError;
          ok = false;
        }
      }
      if (ok) {
        built = std::make_unique<PortDread::SuitMaterial>();
        if (!PortDread::BuildSuitMaterial(bsmat, tex[0], tex[1], tex[2], *built, &why)) {
          built.reset();
        }
      }
    }
    if (!built) {
      log("dread: material " + bsmatPath + " left out: " + why);
    }
    return (materials[bsmatPath] = std::move(built)).get();
  };

  PortRemastered::ConvertIO cio;
  cio.retail = io.retail;
  std::set<uint32_t> reserved;
  for (const auto& t : kTargets) reserved.insert(DreadCinf(t.cmdl));
  std::map<std::string, std::vector<uint8_t>> pending;
  cio.retailId = [&](uint32_t id) { return reserved.count(id) || (io.retailId && io.retailId(id)); };
  cio.texture = [&](const PortRemastered::ModelUuid& id, PortRemastered::Image& out, std::string& why) {
    for (const auto& [path, m] : materials) {
      if (m) {
        for (const PortDread::SuitImage& si : m->images) {
          if (si.id == id) {
            out = si.image;
            return true;
          }
        }
      }
    }
    why = "no such suit texture";
    return false;
  };
  cio.write = [&](const std::string& name, const std::vector<uint8_t>& data) {
    pending[name] = data;
    // A CMDL does not name itself, so the alias is the same bytes.
    if (name == Hex8(kPhazonCutsceneBody) + ".CMDL") {
      const std::string alias = Hex8(kPhazonCutsceneAlias) + ".CMDL";
      pending[alias] = data;
    }
    return true;
  };
  cio.log = [&](const std::string& line) {
    if (io.detail) {
      io.detail("dread: " + line);
    } else {
      log("dread: " + line);
    }
  };
  PortRemastered::Converter converter(cio);

  std::map<std::string, PortDread::Bcmdl> models;
  std::map<uint32_t, PortDread::PrimeSkeleton> skeletons;
  std::map<uint32_t, std::vector<uint8_t>> cinfData;
  const size_t bodyTotal = std::size(kTargets);
  const size_t total = bodyTotal + std::size(kBallTargets);
  for (size_t ti = 0; ti < bodyTotal; ++ti) {
    const Target& t = kTargets[ti];
    if (io.progress) {
      io.progress(ti, total);
    }
    if (cancelled()) {
      error = "cancelled";
      return false;
    }
    const std::string label = std::string(t.suit) + " " + t.skeleton + " -> " + Hex8(t.cmdl);
    auto failTarget = [&](const std::string& why) {
      log("dread: " + label + " failed: " + why);
      ++result.failed;
    };
    // The Dread model, parsed once per suit.
    auto model = models.find(t.suit);
    if (model == models.end()) {
      const std::string path = std::string("actors/characters/samus/models/") + t.suit + ".bcmdl";
      std::vector<uint8_t> data;
      PortDread::Bcmdl parsed;
      std::string why;
      if (!pkgFile(path, data)) {
        failTarget(path + " is not in samus.pkg");
        continue;
      }
      if (!PortDread::ParseBcmdl(data.data(), data.size(), parsed, &why)) {
        failTarget(path + ": " + why);
        continue;
      }
      model = models.emplace(t.suit, std::move(parsed)).first;
    }
    auto skeleton = skeletons.find(t.cinf);
    if (skeleton == skeletons.end()) {
      std::vector<uint8_t> data;
      PortDread::PrimeSkeleton parsed;
      std::string why;
      if (!io.retail(FourCC('C', 'I', 'N', 'F'), t.cinf, data)) {
        failTarget("CINF " + Hex8(t.cinf) + " is not on the disc");
        continue;
      }
      if (!PortDread::ParseCinf(data.data(), data.size(), parsed, &why)) {
        failTarget("CINF " + Hex8(t.cinf) + ": " + why);
        continue;
      }
      skeleton = skeletons.emplace(t.cinf, std::move(parsed)).first;
      cinfData[t.cinf] = std::move(data);
    }
    std::vector<uint8_t> retailCmdl, retailCskr;
    PortDread::RetailSkinRef retailSkin;
    std::string why;
    if (!io.retail(FourCC('C', 'M', 'D', 'L'), t.cmdl, retailCmdl) ||
        !io.retail(FourCC('C', 'S', 'K', 'R'), t.cskr[0], retailCskr)) {
      failTarget("the retail body is not on the disc");
      continue;
    }
    const bool haveVotes = RetailSkinFromCmdl(retailCmdl, retailCskr, retailSkin, &why);
    if (!haveVotes) {
      log("dread: " + label + ": no retail votes (" + why + "), table weights only");
    }
    PortDread::ReskinOptions ro;
    ro.retail = haveVotes ? &retailSkin : nullptr;
    ro.repose = false;
    PortDread::ReskinResult reskin;
    if (!PortDread::ReskinToPrime(model->second, skeleton->second, ro, reskin, &why)) {
      failTarget("re-skin: " + why);
      continue;
    }
    // The mesh keeps Dread's proportions; the skeleton it is bound to ships as its own CINF.
    const uint32_t dreadCinf = DreadCinf(t.cmdl);
    std::vector<uint8_t> patchedCinf;
    if (dreadCinf == 0 || (io.retailId && io.retailId(dreadCinf))) {
      failTarget("CINF id " + Hex8(dreadCinf) + " is taken");
      continue;
    }
    const std::vector<uint8_t>& baseCinf = cinfData[t.cinf];
    if (!PortDread::PatchCinfHeads(baseCinf.data(), baseCinf.size(), reskin.skeleton, patchedCinf, &why)) {
      failTarget("CINF " + Hex8(dreadCinf) + ": " + why);
      continue;
    }
    for (const std::string& line : reskin.log) {
      log("dread: " + label + ": " + line);
    }
    std::vector<const PortDread::SuitMaterial*> meshMats;
    for (const PortDread::ReskinMesh& rm : reskin.meshes) {
      meshMats.push_back(rm.bsmat_path.empty() ? nullptr : material(rm.bsmat_path));
      if (meshMats.back() == nullptr) {
        log("dread: " + label + ": mesh " + rm.name + " left out (no suit material)");
      }
    }
    PortRemastered::Model built;
    PortRemastered::ConvertOptions opt;
    if (!BuildModel(reskin, meshMats, built, opt.boneWeights, &why)) {
      failTarget(why);
      continue;
    }
    opt.retail = t.cmdl;
    opt.source = std::string("dread ") + t.suit;
    for (uint32_t s : t.cskr) {
      if (s != 0) {
        opt.skins.push_back(s);
      }
    }
    for (int r = 0; r < 3; ++r) {
      for (int c = 0; c < 3; ++c) {
        opt.orient[r][c] = r == c ? 1.0 : 0.0;
      }
      opt.offset[r] = 0.0;
    }
    opt.skip.clear();
    opt.texturePrefix = "dread:";
    if (!converter.Convert(built, opt, why)) {
      failTarget("convert: " + why);
      continue;
    }
    if (!cio.write(Hex8(dreadCinf) + ".CINF", patchedCinf)) {
      failTarget("could not write CINF " + Hex8(dreadCinf));
      continue;
    }
    ++result.converted;
    log("dread: " + label + ": " + std::to_string(built.meshes.size()) + " meshes, " +
        std::to_string(built.materials.size()) + " materials, CINF " + Hex8(dreadCinf) + " (" +
        std::to_string(reskin.skeleton_mapped) + " joints from Dread)");

    // Where the Dread cannon's tip sits against the gun locators, carried at Prime's offsets (step 12).
    if (!result.measured && std::strcmp(t.skeleton, "cine50") == 0) {
      const int gun = reskin.skeleton.Find("GUN_LCTR");
      const int charge = reskin.skeleton.Find("Charge_LCTR");
      float maxX = -1e30f;
      for (const PortDread::ReskinMesh& rm : reskin.meshes) {
        if (rm.name.find("Cannon") != std::string::npos) {
          for (uint32_t v = 0; v < rm.vertex_count; ++v) {
            maxX = std::max(maxX, rm.pos[size_t(v) * 3]);
          }
        }
      }
      double sum[3] = {0, 0, 0};
      size_t count = 0;
      for (const PortDread::ReskinMesh& rm : reskin.meshes) {
        if (rm.name.find("Cannon") != std::string::npos) {
          for (uint32_t v = 0; v < rm.vertex_count; ++v) {
            if (rm.pos[size_t(v) * 3] >= maxX - 0.02f) {
              for (int a = 0; a < 3; ++a) {
                sum[a] += rm.pos[size_t(v) * 3 + a];
              }
              ++count;
            }
          }
        }
      }
      if (gun >= 0 && charge >= 0 && count > 0) {
        result.measured = true;
        for (int a = 0; a < 3; ++a) {
          result.cannonTip[a] = float(sum[a] / double(count));
          result.tipFromGun[a] = result.cannonTip[a] - reskin.skeleton.joints[size_t(gun)].head[a];
          result.tipFromCharge[a] = result.cannonTip[a] - reskin.skeleton.joints[size_t(charge)].head[a];
        }
        char line[256];
        std::snprintf(line, sizeof(line),
                      "dread: cannon tip (%.3f %.3f %.3f): GUN_LCTR %+.3f %+.3f %+.3f, Charge_LCTR %+.3f %+.3f %+.3f",
                      result.cannonTip[0], result.cannonTip[1], result.cannonTip[2], result.tipFromGun[0],
                      result.tipFromGun[1], result.tipFromGun[2], result.tipFromCharge[0], result.tipFromCharge[1],
                      result.tipFromCharge[2]);
        log(line);
      }
    }
  }
  if (result.failed != 0 || result.converted != bodyTotal) {
    error = "incomplete Dread bodies (the notes say why); no output published";
    return false;
  }
  // Keep Prime's ANCS and animations intact, changing only character 0's model and skin.
  // All resources are buffered until every target succeeds; the installer additionally
  // publishes the staging directory atomically, so failed writes cannot replace a mod.
  for (const auto& t : kBallTargets) {
    if (io.progress) io.progress(result.converted, total);
    if (cancelled()) return Fail(&error, "cancelled");
    std::vector<uint8_t> ancs;
    if (!io.retail(FourCC('A','N','C','S'), t.ancs, ancs) || ancs.size() < 14 || Be32(ancs, 4) == 0)
      return Fail(&error, "ball: missing ANCS " + Hex8(t.ancs));
    size_t at = 14;
    while (at < ancs.size() && ancs[at]) ++at;
    ++at;
    if (at + 12 > ancs.size() || Be32(ancs, at) != t.cmdl)
      return Fail(&error, "ball: unexpected ANCS character model");
    const uint32_t skin = Be32(ancs, at + 4), cinf = Be32(ancs, at + 8);
    const uint32_t modelId = PortModelVariant::Id(t.cmdl, t.suit);
    const uint32_t skinId = PortModelVariant::Id(skin, t.suit);
    const uint32_t ancsId = PortModelVariant::Id(t.ancs, t.suit);
    const uint32_t lowId = PortModelVariant::Id(t.low, t.suit);
    for (uint32_t id : {modelId, skinId, ancsId, lowId}) {
      if (id == 0 || (io.retailId && io.retailId(id)) || !reserved.insert(id).second)
        return Fail(&error, "ball: output id collision " + Hex8(id));
      for (const auto& [name, data] : pending) {
        if (name.compare(0, 8, Hex8(id)) == 0)
          return Fail(&error, "ball: generated resource id collision " + Hex8(id));
      }
    }
    const std::string path = std::string("actors/characters/morphball/models/") + t.source + ".bcmdl";
    std::vector<uint8_t> bytes, retailCmdl, retailSkin, cinfBytes;
    PortDread::Bcmdl source;
    PortDread::PrimeSkeleton skeleton;
    PortDread::RetailSkinRef ref;
    if (!pkgFile(path, bytes) || !PortDread::ParseBcmdl(bytes.data(), bytes.size(), source, &error) ||
        !io.retail(FourCC('C','M','D','L'), t.cmdl, retailCmdl) ||
        !io.retail(FourCC('C','S','K','R'), skin, retailSkin) ||
        !io.retail(FourCC('C','I','N','F'), cinf, cinfBytes) ||
        !PortDread::ParseCinf(cinfBytes.data(), cinfBytes.size(), skeleton, &error) ||
        !RetailSkinFromCmdl(retailCmdl, retailSkin, ref, &error))
      return Fail(&error, "ball " + std::string(t.source) + ": missing or invalid model/skin/skeleton: " + error);
    std::map<uint32_t, double> votes;
    for (const auto& weights : ref.weights) for (const auto& [joint, weight] : weights) votes[joint] += weight;
    const auto rigid = std::max_element(votes.begin(), votes.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
    if (rigid == votes.end() || skeleton.IndexOfId(rigid->first) < 0)
      return Fail(&error, "ball: no retail shell joint");
    PortDread::ReskinResult fit;
    if (!FitBall(source, retailCmdl, rigid->first, fit, &error)) return false;
    std::vector<const PortDread::SuitMaterial*> mats;
    for (const auto& mesh : fit.meshes) {
      mats.push_back(material(mesh.bsmat_path));
      if (!mats.back()) return Fail(&error, "ball: unsupported or missing shell material " + mesh.bsmat_path);
    }
    PortRemastered::Model built;
    PortRemastered::ConvertOptions opt;
    if (!BuildModel(fit, mats, built, opt.boneWeights, &error)) return false;
    opt.retail = t.cmdl;
    opt.outputModel = modelId;
    opt.skins = {skin};
    opt.outputSkins = {skinId};
    opt.texturePrefix = "dread:";
    opt.source = std::string("dread ") + t.source;
    opt.skip.clear();
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) opt.orient[r][c] = r == c ? 1.0 : 0.0;
    if (!converter.Convert(built, opt, error)) return false;
    auto put = [&](size_t offset, uint32_t value) {
      for (int i = 0; i < 4; ++i) ancs[offset + i] = uint8_t(value >> (24 - i * 8));
    };
    put(at, modelId);
    put(at + 4, skinId);
    if (!cio.write(Hex8(ancsId) + ".ANCS", ancs)) return Fail(&error, "ball: ANCS write failed");
    opt.retail = t.low;
    opt.outputModel = lowId;
    opt.skins.clear();
    opt.outputSkins.clear();
    opt.boneWeights.clear();
    built.skinned = false;
    if (!converter.Convert(built, opt, error)) return false;
    ++result.converted;
    log("dread: " + std::string(t.source) + (t.spider ? " Spider" : " normal") +
        ": measured fit " + std::to_string(fit.scale) + ", rigid joint " + std::to_string(rigid->first));
  }
  if (cancelled()) return Fail(&error, "cancelled");
  for (const auto& [name, bytes] : pending) {
    if (cancelled()) return Fail(&error, "cancelled; discard staging");
    if (!io.write(name, bytes)) return Fail(&error, "cannot write " + name + "; discard staging");
    result.files.push_back(name);
  }
  if (cancelled()) return Fail(&error, "cancelled; discard staging");
  if (io.progress) io.progress(total, total);
  return true;
}

}  // namespace PortDreadSamus
