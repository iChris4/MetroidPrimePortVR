// Dread -> Prime re-skin (platform/port_dread_reskin.cpp): CINF parse, frame/scale fit, the
// repose of a bent chain onto a straight one, and the weight mapping.
//
// The fixtures are synthetic stick figures built here. The real-data checks run only when
// PORT_DREAD_SAMUS_PKG_DIR names the extracted Samus pkg (build/dread/samus_pkg) and
// PORT_PRIME_SAMUS_DIR a directory of Prime resources dumped from the user's disc as
// <ID>.CINF / .CMDL / .CSKR (build/s-dread4-20261024/dump_prime.py); otherwise they print
// that they skipped. PORT_DREAD_RESKIN_DUMP=<dir> also writes each result there (.obj and a
// .bin of positions, normals, triangles and weights) for the offline renders.
#include "port_dread_bcmdl.h"
#include "port_dread_reskin.h"
#include "port_test.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

using namespace PortDread;

namespace {

bool Near(double a, double b, double eps = 1e-4) { return std::fabs(a - b) <= eps; }
bool Near3(const float* a, double x, double y, double z, double eps = 1e-4) {
  return Near(a[0], x, eps) && Near(a[1], y, eps) && Near(a[2], z, eps);
}

// Big-endian CINF builder.
struct CinfJoint {
  uint32_t id, parent;
  const char* name;
  float x, y, z;
};

std::vector<uint8_t> BuildCinf(const std::vector<CinfJoint>& js) {
  std::vector<uint8_t> b;
  auto u32 = [&](uint32_t v) {
    for (int i = 3; i >= 0; --i) {
      b.push_back(uint8_t(v >> (8 * i)));
    }
  };
  auto f32 = [&](float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    u32(u);
  };
  u32(uint32_t(js.size()));
  for (const CinfJoint& j : js) {
    u32(j.id);
    u32(j.parent);
    f32(j.x);
    f32(j.y);
    f32(j.z);
    u32(1);
    u32(j.parent);  // link list (parent and children); the reader skips it
  }
  u32(uint32_t(js.size()));
  for (const CinfJoint& j : js) {
    u32(j.id);
  }
  u32(uint32_t(js.size()));
  for (const CinfJoint& j : js) {
    b.insert(b.end(), j.name, j.name + std::strlen(j.name) + 1);
    u32(j.id);
  }
  return b;
}

// A Prime-style T-pose: Z up, +Y forward, left at -X, metres. Root id 1's parent (0) is not
// in the file, as in the game's.
const std::vector<CinfJoint> kPrimeFigure = {
    {1, 0, "root", 0, 0, 0},
    {2, 1, "Pelvis", 0, 0, 1.0f},
    {3, 2, "Spine_1", 0, 0, 1.0f},
    {4, 3, "Spine_2", 0, 0, 1.2f},
    {5, 4, "Head_1", 0, 0, 1.6f},
    {6, 4, "L_collar", -0.1f, 0, 1.4f},
    {7, 6, "L_shoulder", -0.2f, 0, 1.4f},
    {8, 7, "L_elbow", -0.45f, 0, 1.4f},
    {9, 8, "L_wrist", -0.70f, 0, 1.4f},
    {10, 4, "R_shoulder", 0.2f, 0, 1.4f},
    {11, 10, "R_elbow", 0.45f, 0, 1.4f},
    {12, 2, "L_hip", -0.1f, 0, 0.95f},
    {13, 12, "L_knee", -0.1f, 0, 0.5f},
    {14, 13, "L_ankle", -0.1f, 0, 0.1f},
    {15, 2, "R_hip", 0.1f, 0, 0.95f},
    {16, 15, "R_knee", 0.1f, 0, 0.5f},
    {17, 16, "R_ankle", 0.1f, 0, 0.1f},
};

PrimeSkeleton PrimeFigure() {
  const std::vector<uint8_t> bytes = BuildCinf(kPrimeFigure);
  PrimeSkeleton s;
  std::string err;
  REQUIRE_MSG(ParseCinf(bytes.data(), bytes.size(), s, &err), err);
  return s;
}

struct DJ {
  const char* name;
  const char* parent;
  float x, y, z;  // WORLD position (Dread: Y up, +X left, +Z forward, cm); stored as local
};

// The same figure the Dread way, pelvis at the origin. `bent` turns the left forearm forward
// (+Z) and makes it 30 cm instead of 25.
std::vector<DJ> DreadFigure(bool bent) {
  std::vector<DJ> js = {
      {"DC_Root", nullptr, 0, 0, 0},
      {"DC_Floor", "DC_Root", 0, -100, 0},
      {"C01_Master_SKN", "DC_Root", 0, 0, 0},
      {"C01_Spine0_SKN", "C01_Master_SKN", 0, 0, 0},
      {"C01_Spine1_SKN", "C01_Spine0_SKN", 0, 20, 0},
      {"C01_Head_SKN", "C01_Spine1_SKN", 0, 60, 0},
      {"L01_Shoulder_Aux2_SKN", "C01_Spine1_SKN", 15, 45, 0},
      {"L01_UpperArm_SKN", "C01_Spine1_SKN", 20, 40, 0},
      {"L01_Forearm_SKN", "L01_UpperArm_SKN", 45, 40, 0},
      {"L01_Hand_SKN", "L01_Forearm_SKN", 70, 40, 0},
      {"L01_FingerX_SKN", "L01_Hand_SKN", 75, 40, 0},
      {"R01_UpperArm_SKN", "C01_Spine1_SKN", -20, 40, 0},
      {"R01_Forearm_SKN", "R01_UpperArm_SKN", -45, 40, 0},
      {"L01_Thigh_SKN", "C01_Master_SKN", 10, -5, 0},
      {"L01_Calf_SKN", "L01_Thigh_SKN", 10, -50, 0},
      {"L01_Foot_SKN", "L01_Calf_SKN", 10, -90, 0},
      {"R01_Thigh_SKN", "C01_Master_SKN", -10, -5, 0},
      {"R01_Calf_SKN", "R01_Thigh_SKN", -10, -50, 0},
      {"R01_Foot_SKN", "R01_Calf_SKN", -10, -90, 0},
  };
  if (bent) {
    for (DJ& j : js) {
      if (std::strcmp(j.name, "L01_Hand_SKN") == 0) {
        j.x = 45, j.z = 30;
      } else if (std::strcmp(j.name, "L01_FingerX_SKN") == 0) {
        j.x = 45, j.z = 35;
      }
    }
  }
  return js;
}

struct Vert {
  float p[3], n[3];
  std::vector<std::pair<const char*, float>> w;  // joint name, weight (<= 4)
};

// A Bcmdl holding the figure and one enabled skin-type-2 mesh of the given vertices (bind
// model space), one triangle per three vertices.
Bcmdl BuildModel(const std::vector<DJ>& js, const std::vector<Vert>& verts) {
  Bcmdl m;
  for (const DJ& d : js) {
    BcmdlJoint j;
    j.name = d.name;
    j.parent = d.parent ? m.FindJoint(d.parent) : -1;
    REQUIRE(d.parent == nullptr || j.parent >= 0);
    float px = 0, py = 0, pz = 0;
    for (const DJ& e : js) {
      if (d.parent && std::strcmp(e.name, d.parent) == 0) {
        px = e.x, py = e.y, pz = e.z;
      }
    }
    j.pos[0] = d.x - px, j.pos[1] = d.y - py, j.pos[2] = d.z - pz;
    m.joints.push_back(j);
  }
  BcmdlMesh mesh;
  mesh.name = "Mesh_Test";
  mesh.enabled = true;
  BcmdlSubmesh sm;
  sm.skin_type = 2;
  for (size_t j = 0; j < m.joints.size(); ++j) {
    sm.joint_map.push_back(int32_t(j));
  }
  BcmdlVertices& vs = mesh.vertices;
  vs.count = uint32_t(verts.size());
  for (const Vert& v : verts) {
    vs.pos.insert(vs.pos.end(), v.p, v.p + 3);
    vs.normal.insert(vs.normal.end(), v.n, v.n + 3);
    for (int k = 0; k < 4; ++k) {
      if (size_t(k) < v.w.size()) {
        vs.joints.push_back(m.FindJoint(v.w[size_t(k)].first));
        REQUIRE(vs.joints.back() >= 0);
        vs.weights.push_back(v.w[size_t(k)].second);
      } else {
        vs.joints.push_back(0);
        vs.weights.push_back(0.f);
      }
    }
  }
  for (size_t i = 0; i + 2 < verts.size(); i += 3) {
    for (int k = 0; k < 3; ++k) {
      mesh.indices.push_back(uint16_t(i + size_t(k)));
    }
  }
  sm.first_index = 0;
  sm.index_count = uint32_t(mesh.indices.size());
  mesh.submeshes.push_back(sm);
  m.meshes.push_back(mesh);
  // A disabled mesh is skipped by default.
  BcmdlMesh off = m.meshes[0];
  off.name = "Mesh_FX_Off";
  off.enabled = false;
  m.meshes.push_back(off);
  return m;
}

double WeightOf(const std::vector<std::pair<uint32_t, float>>& ws, uint32_t id) {
  for (const auto& w : ws) {
    if (w.first == id) {
      return w.second;
    }
  }
  return 0.0;
}

void TestCinf() {
  const PrimeSkeleton s = PrimeFigure();
  CHECK_EQ(s.joints.size(), kPrimeFigure.size());
  const int e = s.Find("L_elbow");
  REQUIRE(e >= 0);
  CHECK_EQ(s.joints[size_t(e)].id, 8u);
  CHECK_EQ(s.joints[size_t(e)].parent, 7u);
  CHECK(Near3(s.joints[size_t(e)].head, -0.45, 0, 1.4, 1e-6));
  CHECK_EQ(s.joints[size_t(s.Find("root"))].parent, kPrimeNoParent);  // parent 0 not in file
  CHECK_EQ(s.IndexOfId(14), s.Find("L_ankle"));
  CHECK_EQ(s.Find("Nope"), -1);
  // Truncations fail cleanly.
  const std::vector<uint8_t> bytes = BuildCinf(kPrimeFigure);
  for (size_t cut : {size_t(0), size_t(3), size_t(20), bytes.size() / 2, bytes.size() - 1}) {
    PrimeSkeleton t;
    std::string err;
    CHECK_MSG(!ParseCinf(bytes.data(), cut, t, &err), "truncated CINF parsed at " + std::to_string(cut));
  }
}

// The straight Dread figure is Prime's in other units and axes: the fit finds them exactly
// and every vertex lands on the same spot.
void TestFitIdentity() {
  const PrimeSkeleton s = PrimeFigure();
  std::vector<Vert> verts = {
      {{70, 40, 0}, {0, 1, 0}, {{"L01_Hand_SKN", 1.f}}},
      {{0, 30, 10}, {0, 0, 1}, {{"C01_Spine1_SKN", 1.f}}},
      {{10, -90, 5}, {0, 0, 1}, {{"L01_Foot_SKN", 1.f}}},
  };
  const Bcmdl m = BuildModel(DreadFigure(false), verts);
  ReskinResult r;
  std::string err;
  REQUIRE_MSG(ReskinToPrime(m, s, {}, r, &err), err);
  CHECK(Near(r.scale, 0.01, 1e-6));
  const float want[3][3] = {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}};
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      CHECK(Near(r.frame[i][j], want[i][j], 1e-6));
    }
  }
  CHECK(Near3(r.offset, 0, 0, 1.0, 1e-5));
  REQUIRE(r.meshes.size() == size_t(1));  // the disabled mesh is left out
  const ReskinMesh& mesh = r.meshes[0];
  CHECK(Near3(&mesh.pos[0], -0.70, 0, 1.4, 1e-5));
  CHECK(Near3(&mesh.normal[0], 0, 0, 1, 1e-5));
  CHECK(Near3(&mesh.pos[3], 0, 0.10, 1.3, 1e-5));
  CHECK(Near3(&mesh.normal[3], 0, 1, 0, 1e-5));
  CHECK(Near3(&mesh.pos[6], -0.1, 0.05, 0.1, 1e-5));
  REQUIRE(mesh.submeshes.size() == size_t(1));
  CHECK_EQ(mesh.submeshes[0].indices.size(), size_t(3));
}

// A bent, longer left forearm is turned and shrunk onto Prime's straight one: points along
// it land on the matching points of Prime's bone, the hand rides the wrist, normals turn too.
void TestReposeBentChain() {
  const PrimeSkeleton s = PrimeFigure();
  std::vector<Vert> verts = {
      {{45, 40, 15}, {1, 0, 0}, {{"L01_Forearm_SKN", 1.f}}},  // forearm middle, normal left
      {{45, 40, 30}, {0, 1, 0}, {{"L01_Hand_SKN", 1.f}}},     // on the wrist
      {{45, 40, 35}, {0, 1, 0}, {{"L01_FingerX_SKN", 1.f}}},  // 5 cm past it (unmapped joint)
      {{45, 40, 0}, {0, 1, 0}, {{"L01_UpperArm_SKN", 0.5f}, {"L01_Forearm_SKN", 0.5f}}},  // elbow
      {{20, 40, 0}, {0, 1, 0}, {{"L01_UpperArm_SKN", 1.f}}},  // shoulder (unchanged)
      {{-45, 40, 0}, {0, 1, 0}, {{"R01_Forearm_SKN", 1.f}}},  // right elbow (unchanged)
  };
  const Bcmdl m = BuildModel(DreadFigure(true), verts);
  ReskinResult r;
  std::string err;
  REQUIRE_MSG(ReskinToPrime(m, s, {}, r, &err), err);
  REQUIRE(r.meshes.size() == size_t(1));
  const ReskinMesh& mesh = r.meshes[0];
  CHECK(Near3(&mesh.pos[0], -0.575, 0, 1.4, 1e-5));
  CHECK(Near3(&mesh.normal[0], 0, -1, 0, 1e-5));  // Dread left -> Prime -X, turned 90 deg
  CHECK(Near3(&mesh.pos[3], -0.70, 0, 1.4, 1e-5));
  CHECK(Near3(&mesh.pos[6], -0.75, 0, 1.4, 1e-5));  // the hand is turned, not stretched
  CHECK(Near3(&mesh.pos[9], -0.45, 0, 1.4, 1e-5));
  CHECK(Near3(&mesh.pos[12], -0.2, 0, 1.4, 1e-5));
  CHECK(Near3(&mesh.pos[15], 0.45, 0, 1.4, 1e-5));
  for (float f : mesh.normal) {
    CHECK(std::isfinite(f));
  }
}

void TestWeights() {
  const PrimeSkeleton s = PrimeFigure();
  std::vector<Vert> verts = {
      // Through the table and an unmapped joint's ancestor: elbow 0.5, wrist 0.3 + 0.2.
      {{45, 40, 0}, {0, 1, 0}, {{"L01_Forearm_SKN", 0.5f}, {"L01_Hand_SKN", 0.3f}, {"L01_FingerX_SKN", 0.2f}}},
      // Four Prime joints, quantised to 1/64.
      {{0, 30, 0},
       {0, 1, 0},
       {{"C01_Spine1_SKN", 0.4f}, {"C01_Head_SKN", 0.3f}, {"L01_UpperArm_SKN", 0.2f}, {"C01_Master_SKN", 0.1f}}},
      // Unnormalised Dread weights still sum to 1; joints sharing a Prime bone merge.
      {{0, 0, 0}, {0, 1, 0}, {{"C01_Master_SKN", 1.f}, {"C01_Spine0_SKN", 0.5f}, {"DC_Root", 0.5f}}},
      // A vote joint (shoulder helper).
      {{15, 45, 0}, {0, 1, 0}, {{"L01_Shoulder_Aux2_SKN", 1.f}}},
      {{15, 45, 1}, {0, 1, 0}, {{"L01_Shoulder_Aux2_SKN", 0.5f}, {"C01_Spine1_SKN", 0.5f}}},
      {{15, 45, 2}, {0, 1, 0}, {{"L01_Shoulder_Aux2_SKN", 1.f}}},
  };
  const Bcmdl m = BuildModel(DreadFigure(false), verts);
  auto id = [&](const char* n) { return s.joints[size_t(s.Find(n))].id; };
  for (int pass = 0; pass < 2; ++pass) {
    RetailSkinRef retail;
    retail.pos = {-0.15f, 0.f, 1.45f, 0.f, 0.f, 0.f};
    retail.weights = {{{id("L_collar"), 0.75f}, {id("Spine_2"), 0.25f}}, {{id("Pelvis"), 1.f}}};
    ReskinOptions opt;
    opt.retail = pass == 1 ? &retail : nullptr;
    ReskinResult r;
    std::string err;
    REQUIRE_MSG(ReskinToPrime(m, s, opt, r, &err), err);
    const ReskinMesh& mesh = r.meshes[0];
    for (const auto& ws : mesh.weights) {
      double sum = 0.0;
      CHECK(!ws.empty() && ws.size() <= kReskinMaxWeights);
      for (size_t i = 0; i < ws.size(); ++i) {
        sum += ws[i].second;
        CHECK(i == 0 || ws[i - 1].first < ws[i].first);  // sorted, unique
      }
      CHECK(Near(sum, 1.0, 1e-6));
    }
    CHECK(Near(WeightOf(mesh.weights[0], id("L_elbow")), 0.5, 1e-6));
    CHECK(Near(WeightOf(mesh.weights[0], id("L_wrist")), 0.5, 1e-6));
    CHECK_EQ(mesh.weights[1].size(), size_t(4));
    CHECK(Near(WeightOf(mesh.weights[1], id("Spine_2")), 26 / 64.0, 1e-6));
    CHECK(Near(WeightOf(mesh.weights[1], id("Head_1")), 19 / 64.0, 1e-6));
    CHECK(Near(WeightOf(mesh.weights[1], id("L_shoulder")), 13 / 64.0, 1e-6));
    CHECK(Near(WeightOf(mesh.weights[1], id("Pelvis")), 6 / 64.0, 1e-6));
    // Master -> Pelvis, Spine0 -> Spine_1, DC_Root (no row, no parent) -> Pelvis.
    CHECK(Near(WeightOf(mesh.weights[2], id("Pelvis")), 0.75, 1e-6));
    CHECK(Near(WeightOf(mesh.weights[2], id("Spine_1")), 0.25, 1e-6));
    CHECK(mesh.source[0] == WeightSource::Table);
    if (pass == 0) {
      // No retail weights: the vote row's own candidate.
      CHECK(Near(WeightOf(mesh.weights[3], id("L_collar")), 1.0, 1e-6));
      CHECK(mesh.source[3] == WeightSource::Table);
      CHECK_EQ(r.stats.verts_vote, size_t(0));
    } else {
      CHECK(Near(WeightOf(mesh.weights[3], id("L_collar")), 0.75, 1e-6));
      CHECK(Near(WeightOf(mesh.weights[3], id("Spine_2")), 0.25, 1e-6));
      CHECK(mesh.source[3] == WeightSource::Vote);
      CHECK(mesh.source[4] == WeightSource::Mixed);
      CHECK(Near(WeightOf(mesh.weights[4], id("Spine_2")), 0.5 + 0.125, 1.0 / 64));
      CHECK_EQ(r.stats.verts_vote, size_t(2));
      CHECK_EQ(r.stats.verts_mixed, size_t(1));
    }
    // Weight targets per joint: the vote joint is -1 only when votes ran.
    CHECK_EQ(r.weight_joint[size_t(m.FindJoint("L01_Shoulder_Aux2_SKN"))], pass == 1 ? -1 : s.Find("L_collar"));
    CHECK_EQ(r.weight_joint[size_t(m.FindJoint("L01_FingerX_SKN"))], s.Find("L_wrist"));
    CHECK_EQ(r.stats.joints, m.joints.size());
  }
}

// Prime's ankles lower than Dread's (fitted): the boot is squashed about the ankle so its sole
// stays on Prime's floor, not pushed through it.
void TestFloorSquash() {
  std::vector<CinfJoint> js = kPrimeFigure;
  for (CinfJoint& j : js) {
    if (std::strstr(j.name, "_ankle") != nullptr) {
      j.z = 0.085f;
    }
  }
  const std::vector<uint8_t> bytes = BuildCinf(js);
  PrimeSkeleton s;
  std::string err;
  REQUIRE(ParseCinf(bytes.data(), bytes.size(), s, &err));
  const Bcmdl m = BuildModel(DreadFigure(false), {{{10, -100, 4}, {0, 1, 0}, {{"L01_Foot_SKN", 1.f}}},
                                                   {{10, -90, 4}, {0, 1, 0}, {{"L01_Foot_SKN", 1.f}}},
                                                   {{10, -95, 4}, {0, 1, 0}, {{"L01_Foot_SKN", 1.f}}}});
  ReskinResult r;
  REQUIRE_MSG(ReskinToPrime(m, s, {}, r, &err), err);
  const ReskinMesh& mesh = r.meshes[0];
  CHECK(Near(mesh.pos[2], 0.0, 1e-5));    // sole on the floor
  CHECK(Near(mesh.pos[5], 0.085, 1e-5));  // ankle height on the ankle
  CHECK(Near(mesh.pos[0], -0.1, 1e-5));
  bool logged = false;
  for (const std::string& l : r.log) {
    logged = logged || l.find("floor: L01_Foot_SKN") != std::string::npos;
  }
  CHECK(logged);
}

// Repose off: the mesh only fitted, the skeleton moved onto the fitted Dread joints (kBind), the
// joints Dread lacks carried at Prime's offsets from their parents, and the CINF patched to it.
void TestDreadSkeleton() {
  const PrimeSkeleton s = PrimeFigure();
  std::vector<Vert> verts = {
      {{45, 40, 15}, {1, 0, 0}, {{"L01_Forearm_SKN", 1.f}}},
      {{10, -90, 5}, {0, 0, 1}, {{"L01_Foot_SKN", 1.f}}},
      {{0, 30, 10}, {0, 0, 1}, {{"C01_Spine1_SKN", 1.f}}},
  };
  const std::vector<DJ> dj = DreadFigure(true);
  const Bcmdl m = BuildModel(dj, verts);
  ReskinOptions opt;
  opt.repose = false;
  ReskinResult r;
  std::string err;
  REQUIRE_MSG(ReskinToPrime(m, s, opt, r, &err), err);
  auto fit = [&](float x, float y, float z, float out[3]) {
    const float d[3] = {x, y, z};
    for (int i = 0; i < 3; ++i) {
      out[i] = r.offset[i];
      for (int j = 0; j < 3; ++j) {
        out[i] += r.frame[i][j] * d[j] * r.scale;
      }
    }
  };
  auto dread = [&](const char* name, float out[3]) {
    for (const DJ& d : dj) {
      if (std::strcmp(d.name, name) == 0) {
        fit(d.x, d.y, d.z, out);
        return;
      }
    }
    REQUIRE_MSG(false, name);
  };
  const ReskinMesh& mesh = r.meshes[0];
  for (size_t v = 0; v < verts.size(); ++v) {
    float want[3];
    fit(verts[v].p[0], verts[v].p[1], verts[v].p[2], want);
    CHECK(Near3(&mesh.pos[v * 3], want[0], want[1], want[2], 1e-5));
  }
  CHECK(Near3(&mesh.normal[0], -1, 0, 0, 1e-5));  // not turned
  const PrimeSkeleton& k = r.skeleton;
  REQUIRE(k.joints.size() == s.joints.size());
  auto head = [&](const PrimeSkeleton& sk, const char* n) { return sk.joints[size_t(sk.Find(n))].head; };
  float want[3];
  for (const auto& [prime, d] : std::vector<std::pair<const char*, const char*>>{{"L_wrist", "L01_Hand_SKN"},
                                                                                 {"L_elbow", "L01_Forearm_SKN"},
                                                                                 {"Pelvis", "C01_Master_SKN"},
                                                                                 {"Spine_2", "C01_Spine1_SKN"},
                                                                                 {"R_knee", "R01_Calf_SKN"}}) {
    dread(d, want);
    CHECK_MSG(Near3(head(k, prime), want[0], want[1], want[2], 1e-5), prime);
  }
  // L_collar (no Dread joint in the figure) rides Spine_2 at Prime's offset; root keeps its head.
  for (int a = 0; a < 3; ++a) {
    CHECK(Near(head(k, "L_collar")[a] - head(k, "Spine_2")[a], head(s, "L_collar")[a] - head(s, "Spine_2")[a], 1e-5));
  }
  CHECK(Near3(head(k, "root"), 0, 0, 0, 1e-6));
  CHECK(r.skeleton_mapped >= 5);
  for (size_t j = 0; j < s.joints.size(); ++j) {
    CHECK_EQ(k.joints[j].id, s.joints[j].id);
    CHECK_EQ(k.joints[j].parent, s.joints[j].parent);
  }

  // The patched CINF: same bytes but the heads.
  const std::vector<uint8_t> bytes = BuildCinf(kPrimeFigure);
  std::vector<uint8_t> patched;
  REQUIRE_MSG(PatchCinfHeads(bytes.data(), bytes.size(), k, patched, &err), err);
  CHECK_EQ(patched.size(), bytes.size());
  PrimeSkeleton back;
  REQUIRE_MSG(ParseCinf(patched.data(), patched.size(), back, &err), err);
  REQUIRE(back.joints.size() == k.joints.size());
  for (size_t j = 0; j < k.joints.size(); ++j) {
    CHECK_EQ(back.joints[j].name, k.joints[j].name);
    CHECK_EQ(back.joints[j].id, k.joints[j].id);
    CHECK(Near3(back.joints[j].head, k.joints[j].head[0], k.joints[j].head[1], k.joints[j].head[2], 0));
  }
  // A skeleton missing one of the CINF's joints, or a broken CINF, is refused.
  PrimeSkeleton fewer = k;
  fewer.joints.pop_back();
  CHECK(!PatchCinfHeads(bytes.data(), bytes.size(), fewer, patched, &err));
  CHECK(!PatchCinfHeads(bytes.data(), bytes.size() / 2, k, patched, &err));
}

// A model or skeleton without the fit's joints fails with a message.
void TestMissingJoints() {
  std::vector<CinfJoint> js = kPrimeFigure;
  js.erase(js.begin() + 4);  // Head_1
  const std::vector<uint8_t> bytes = BuildCinf(js);
  PrimeSkeleton s;
  std::string err;
  REQUIRE(ParseCinf(bytes.data(), bytes.size(), s, &err));
  const Bcmdl m = BuildModel(DreadFigure(false), {{{0, 0, 0}, {0, 1, 0}, {{"C01_Master_SKN", 1.f}}}});
  ReskinResult r;
  CHECK(!ReskinToPrime(m, s, {}, r, &err));
  CHECK(err.find("Head_1") != std::string::npos);
}

// ---- real data ----

bool ReadFile(const std::string& path, std::vector<uint8_t>& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    return false;
  }
  out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return true;
}

uint32_t BE32(const std::vector<uint8_t>& d, size_t o) {
  return (uint32_t(d[o]) << 24) | (uint32_t(d[o + 1]) << 16) | (uint32_t(d[o + 2]) << 8) | d[o + 3];
}
float BEF(const std::vector<uint8_t>& d, size_t o) {
  const uint32_t u = BE32(d, o);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

// Retail CMDL positions (the section after the material sets) and CSKR groups, per vertex;
// also the CMDL's bounding box.
bool LoadRetail(const std::string& dir, uint32_t cmdl, uint32_t cskr, RetailSkinRef& out, float aabb[6]) {
  char name[32];
  std::vector<uint8_t> m, k;
  std::snprintf(name, sizeof(name), "/%08X.CMDL", cmdl);
  if (!ReadFile(dir + name, m) || m.size() < 0x2c) {
    return false;
  }
  std::snprintf(name, sizeof(name), "/%08X.CSKR", cskr);
  if (!ReadFile(dir + name, k) || k.size() < 4) {
    return false;
  }
  for (int i = 0; i < 6; ++i) {
    aabb[i] = BEF(m, 12 + 4 * size_t(i));
  }
  const uint32_t nsec = BE32(m, 0x24), nmat = BE32(m, 0x28);
  if (nmat >= nsec || 0x2c + 4ull * nsec > m.size()) {
    return false;
  }
  size_t o = (0x2c + 4 * size_t(nsec) + 31) & ~size_t(31);
  for (uint32_t i = 0; i < nmat; ++i) {
    o += BE32(m, 0x2c + 4 * size_t(i));
  }
  const size_t psize = BE32(m, 0x2c + 4 * size_t(nmat));
  if (o + psize > m.size()) {
    return false;
  }
  size_t ko = 4;
  const uint32_t groups = BE32(k, 0);
  for (uint32_t g = 0; g < groups; ++g) {
    const uint32_t nw = BE32(k, ko);
    ko += 4;
    std::vector<std::pair<uint32_t, float>> ws;
    for (uint32_t i = 0; i < nw; ++i, ko += 8) {
      ws.emplace_back(BE32(k, ko), BEF(k, ko + 4));
    }
    const uint32_t count = BE32(k, ko);
    ko += 4;
    for (uint32_t i = 0; i < count; ++i) {
      out.weights.push_back(ws);
    }
  }
  const size_t n = std::min(out.weights.size(), psize / 12);
  out.weights.resize(n);
  for (size_t i = 0; i < n * 3; ++i) {
    out.pos.push_back(BEF(m, o + 4 * i));
  }
  return n > 0;
}

void Dump(const std::string& dir, const std::string& stem, const ReskinResult& r) {
  std::ofstream obj(dir + "/" + stem + ".obj");
  std::ofstream bin(dir + "/" + stem + ".bin", std::ios::binary);
  auto u32 = [&](uint32_t v) { bin.write(reinterpret_cast<const char*>(&v), 4); };
  auto f32 = [&](float v) { bin.write(reinterpret_cast<const char*>(&v), 4); };
  u32(uint32_t(r.meshes.size()));
  size_t base = 1;
  for (const ReskinMesh& m : r.meshes) {
    obj << "o " << m.name << "\n";
    for (uint32_t v = 0; v < m.vertex_count; ++v) {
      obj << "v " << m.pos[3 * v] << " " << m.pos[3 * v + 1] << " " << m.pos[3 * v + 2] << "\n";
      obj << "vn " << m.normal[3 * v] << " " << m.normal[3 * v + 1] << " " << m.normal[3 * v + 2] << "\n";
    }
    size_t tris = 0;
    for (const ReskinSubmesh& s : m.submeshes) {
      for (size_t i = 0; i + 2 < s.indices.size(); i += 3) {
        obj << "f";
        for (int k = 0; k < 3; ++k) {
          const size_t idx = base + s.indices[i + size_t(k)];
          obj << " " << idx << "//" << idx;
        }
        obj << "\n";
      }
      tris += s.indices.size() / 3;
    }
    base += m.vertex_count;
    // name, vertices: pos3 nrm3 src (u32) nw (u32) {id, w} x4; triangles
    u32(uint32_t(m.name.size()));
    bin.write(m.name.data(), std::streamsize(m.name.size()));
    u32(m.vertex_count);
    for (uint32_t v = 0; v < m.vertex_count; ++v) {
      for (int a = 0; a < 3; ++a) {
        f32(m.pos[3 * v + a]);
      }
      for (int a = 0; a < 3; ++a) {
        f32(m.normal[3 * v + a]);
      }
      u32(uint32_t(m.source[v]));
      u32(uint32_t(m.weights[v].size()));
      for (size_t k = 0; k < 4; ++k) {
        u32(k < m.weights[v].size() ? m.weights[v][k].first : 0);
        f32(k < m.weights[v].size() ? m.weights[v][k].second : 0.f);
      }
    }
    u32(uint32_t(tris));
    for (const ReskinSubmesh& s : m.submeshes) {
      for (size_t i = 0; i + 2 < s.indices.size(); i += 3) {
        for (int k = 0; k < 3; ++k) {
          u32(s.indices[i + size_t(k)]);
        }
      }
    }
  }
}

void TestRealData() {
  const char* dpkg = std::getenv("PORT_DREAD_SAMUS_PKG_DIR");
  const char* pdir = std::getenv("PORT_PRIME_SAMUS_DIR");
  if (dpkg == nullptr || *dpkg == '\0' || pdir == nullptr || *pdir == '\0') {
    std::printf("SKIP: real-data re-skin checks (PORT_DREAD_SAMUS_PKG_DIR / PORT_PRIME_SAMUS_DIR not set)\n");
    return;
  }
  const char* dump = std::getenv("PORT_DREAD_RESKIN_DUMP");
  struct Skel {
    const char* tag;
    uint32_t cinf;
    uint32_t body[3][2];  // power, varia, gravity: CMDL, CSKR
  };
  const Skel skels[] = {
      {"cine50", 0x19C53902, {{0xF39476F2, 0x98BBD924}, {0x776C1A96, 0x1C43B540}, {0x57F5C930, 0x3CDA66E6}}},
      {"game20", 0xA496DA02, {{0x4A1A8FD3, 0x21352005}, {0xE449874A, 0x8F66289C}, {0x57489AD3, 0x3C673505}}},
  };
  const char* suits[] = {"samuspower", "samusvaria", "samusgravity"};
  for (const Skel& sk : skels) {
    char name[32];
    std::snprintf(name, sizeof(name), "/%08X.CINF", sk.cinf);
    std::vector<uint8_t> cinf;
    if (!CHECK_MSG(ReadFile(std::string(pdir) + name, cinf), std::string("cannot read ") + pdir + name)) {
      continue;
    }
    PrimeSkeleton skel;
    std::string err;
    if (!CHECK_MSG(ParseCinf(cinf.data(), cinf.size(), skel, &err), err)) {
      continue;
    }
    std::printf("%s: CINF %08X, %zu joints\n", sk.tag, sk.cinf, skel.joints.size());
    for (int si = 0; si < 3; ++si) {
      std::vector<uint8_t> data;
      const std::string path = std::string(dpkg) + "/actors/characters/samus/models/" + suits[si] + ".bcmdl";
      Bcmdl model;
      if (!CHECK_MSG(ReadFile(path, data) && ParseBcmdl(data.data(), data.size(), model, &err), path + " " + err)) {
        continue;
      }
      RetailSkinRef retail;
      float aabb[6];
      if (!CHECK_MSG(LoadRetail(pdir, sk.body[si][0], sk.body[si][1], retail, aabb), "retail body")) {
        continue;
      }
      ReskinOptions opt;
      opt.retail = &retail;
      ReskinResult r;
      if (!CHECK_MSG(ReskinToPrime(model, skel, opt, r, &err), err)) {
        continue;
      }
      const ReskinStats& st = r.stats;
      std::printf("  %s: joints %zu: direct %zu, vote %zu, inherited %zu; repose: anchors %zu, follow %zu, "
                  "carried %zu; verts table %zu vote %zu mixed %zu\n",
                  suits[si], st.joints, st.weight_direct, st.weight_vote, st.weight_inherited, st.anchors, st.follows,
                  st.carried, st.verts_table, st.verts_vote, st.verts_mixed);
      for (const std::string& line : r.log) {
        std::printf("    %s\n", line.c_str());
      }
      // Sanity: finite, normalised, the body's size near the retail body's, the hand on the wrist.
      float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
      bool finite = true, summed = true;
      double hand[3] = {0, 0, 0};
      size_t handN = 0;
      const uint32_t wrist = skel.joints[size_t(skel.Find("L_wrist"))].id;
      for (const ReskinMesh& m : r.meshes) {
        for (uint32_t v = 0; v < m.vertex_count; ++v) {
          double sum = 0.0;
          for (const auto& w : m.weights[v]) {
            sum += w.second;
          }
          summed = summed && Near(sum, 1.0, 1e-5) && m.weights[v].size() <= kReskinMaxWeights;
          for (int a = 0; a < 3; ++a) {
            const float p = m.pos[3 * v + a];
            finite = finite && std::isfinite(p) && std::isfinite(m.normal[3 * v + a]);
            lo[a] = std::min(lo[a], p);
            hi[a] = std::max(hi[a], p);
          }
          if (WeightOf(m.weights[v], wrist) >= 0.99) {
            for (int a = 0; a < 3; ++a) {
              hand[a] += m.pos[3 * v + a];
            }
            ++handN;
          }
        }
      }
      CHECK_MSG(finite, std::string(suits[si]) + ": non-finite vertex");
      CHECK_MSG(summed, std::string(suits[si]) + ": weights do not sum to 1");
      const double height = hi[2] - lo[2], retailHeight = aabb[5] - aabb[2];
      std::printf("    bbox (%.3f %.3f %.3f)-(%.3f %.3f %.3f), height %.3f vs retail %.3f\n", lo[0], lo[1], lo[2],
                  hi[0], hi[1], hi[2], height, retailHeight);
      CHECK_MSG(std::fabs(height / retailHeight - 1.0) < 0.10, std::string(suits[si]) + ": height off by > 10%");
      CHECK_MSG(std::fabs(lo[0] / aabb[0] - 1.0) < 0.10, std::string(suits[si]) + ": left reach off by > 10%");
      CHECK_MSG(std::fabs(lo[2] - aabb[2]) < 0.10, std::string(suits[si]) + ": feet off the floor");
      if (CHECK_MSG(handN > 0, std::string(suits[si]) + ": nothing rides the left wrist")) {
        const float* wp = skel.joints[size_t(skel.Find("L_wrist"))].head;
        double d = 0.0;
        for (int a = 0; a < 3; ++a) {
          const double e = hand[a] / double(handN) - wp[a];
          d += e * e;
        }
        std::printf("    left hand centre %.3f m from L_wrist (%zu verts)\n", std::sqrt(d), handN);
        CHECK_MSG(std::sqrt(d) < 0.12, std::string(suits[si]) + ": hand far from L_wrist");
      }
      if (dump != nullptr && *dump != '\0') {
        Dump(dump, std::string(suits[si]) + "_" + sk.tag, r);
      }
    }
  }
}

} // namespace

int main() {
  TestCinf();
  TestFitIdentity();
  TestReposeBentChain();
  TestWeights();
  TestMissingJoints();
  TestFloorSquash();
  TestDreadSkeleton();
  TestRealData();
  return PORT_TEST_RESULT();
}
