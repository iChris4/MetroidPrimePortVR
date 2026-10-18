// LineTestEx frontOnly (Remastered TriangleNoBackface 0xbf9360, mode 1): the signed determinant
// test skips triangles the ray travels with (back faces) and the traversal must keep searching for
// a farther front face. Default (frontOnly omitted) stays two-sided. Drives the real octree leaf
// traversal over a hand-built tree.
#include "Collision/CMaterialFilter.hpp"
#include "Kyoto/Basics/CBasics.hpp"
#include "Kyoto/Math/CLine.hpp"
#include "WorldFormat/CAreaOctTree.hpp"
#include "port_test.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

namespace {
struct V {
  float x, y, z;
  operator CVector3f() const { return CVector3f(x, y, z); }
};
struct Fixture {
  std::vector<CVector3f> verts;
  std::vector<CCollisionEdge> edges;
  std::vector<ushort> polyEdges;
  std::vector<uchar> polyMats;
  std::vector<uint> mats{0};
  std::vector<uchar> bytes; // serialized tree (big-endian, as in the area resource)
  CAreaOctTree::Node::ETreeType rootType = CAreaOctTree::Node::kTT_Leaf;
  uchar vm = 0;
  std::unique_ptr<CAreaOctTree> tree; // borrows the vectors above; owns nothing else

  // Triangle (a, b, c); e1 x e2 = (b-a) x (c-a).
  void Add(V a, V b, V c) {
    const ushort base = ushort(verts.size());
    verts.push_back(a);
    verts.push_back(b);
    verts.push_back(c);
    edges.emplace_back(base, ushort(base + 1));
    edges.emplace_back(ushort(base + 1), ushort(base + 2));
    edges.emplace_back(ushort(base + 2), base);
    const ushort e = ushort((edges.size() - 3));
    polyEdges.push_back(e);
    polyEdges.push_back(ushort(e + 1));
    polyEdges.push_back(ushort(e + 2));
    polyMats.push_back(0);
  }
  static void PutU16(std::vector<uchar>& b, ushort v) {
    b.push_back(uchar(v >> 8));
    b.push_back(uchar(v));
  }
  static void PutU32(std::vector<uchar>& b, uint v) {
    PutU16(b, ushort(v >> 16));
    PutU16(b, ushort(v));
  }
  static void PutF(std::vector<uchar>& b, float f) {
    uint v;
    std::memcpy(&v, &f, 4);
    PutU32(b, v);
  }
  // leaf record: CAABox, count, triangle indices
  static std::vector<uchar> LeafBytes(const std::vector<ushort>& tris) {
    std::vector<uchar> b;
    for (float f : {-10.f, -10.f, -10.f, 10.f, 10.f, 10.f})
      PutF(b, f);
    PutU16(b, ushort(tris.size()));
    for (ushort t : tris)
      PutU16(b, t);
    return b;
  }
  // branch record: flags (2 bits per child, 2 = leaf), 8 offsets from +36, then children.
  void BuildBranch(ushort flags, const std::vector<std::pair<int, std::vector<ushort>>>& kids) {
    std::vector<uchar> body;
    uint offsets[8] = {};
    for (const auto& k : kids) {
      offsets[k.first] = uint(body.size());
      const std::vector<uchar> l = LeafBytes(k.second);
      body.insert(body.end(), l.begin(), l.end());
    }
    bytes.clear();
    PutU16(bytes, flags);
    PutU16(bytes, 0);
    for (uint o : offsets)
      PutU32(bytes, o);
    bytes.insert(bytes.end(), body.begin(), body.end());
    Finish(CAreaOctTree::Node::kTT_Branch);
  }
  void Build() {
    std::vector<ushort> all;
    for (size_t i = 0; i < polyMats.size(); ++i)
      all.push_back(ushort(i));
    bytes = LeafBytes(all);
    Finish(CAreaOctTree::Node::kTT_Leaf);
  }
  void Finish(CAreaOctTree::Node::ETreeType type) {
    rootType = type;
    tree.reset(new CAreaOctTree(
        CAABox(CVector3f(-10.f, -10.f, -10.f), CVector3f(10.f, 10.f, 10.f)), type, nullptr,
        bytes.data(), 1, mats.data(), &vm, &vm, polyMats.data(), uint(edges.size()), edges.data(),
        uint(polyMats.size()), polyEdges.data(), uint(verts.size()), verts.data()));
  }
  // returns hit?; t and normal z out
  bool Cast(V o, V d, bool useFlag, bool front, float len, float& t,
            float& nz) const {
    CAreaOctTree::Node node(bytes.data(), tree->GetBoundingBox(), *tree, rootType);
    CAreaOctTree::SRayResult r;
    CLine line(CVector3f(o), CUnitVector3f(d.x, d.y, d.z));
    CMaterialFilter f;
    if (useFlag)
      node.LineTestEx(line, f, r, len, front);
    else
      node.LineTestEx(line, f, r, len);
    if (!r.x10_surface.valid())
      return false;
    t = r.x3c_t;
    nz = r.x0_plane.GetNormal().GetZ();
    return true;
  }
};
} // namespace

int main() {
  Fixture fx;
  // near z=0, normal +Z; far z=2, normal -Z
  fx.Add({0, 0, 0}, {4, 0, 0}, {0, 4, 0});
  fx.Add({0, 0, 2}, {0, 4, 2}, {4, 0, 2});
  fx.Build();
  float t = 0, nz = 0;

  // below, travelling +Z: T0 is a back face, T1 a front face
  CHECK_MSG(fx.Cast({1, 1, -1}, {0, 0, 1}, false, false, 10, t, nz) && std::fabs(t - 1.f) < 1e-4f &&
                nz > 0.f,
            "default two-sided: nearest back face");
  CHECK_MSG(fx.Cast({1, 1, -1}, {0, 0, 1}, true, false, 10, t, nz) && std::fabs(t - 1.f) < 1e-4f,
            "explicit false == default");
  CHECK_MSG(fx.Cast({1, 1, -1}, {0, 0, 1}, true, true, 10, t, nz) && std::fabs(t - 3.f) < 1e-4f &&
                nz < 0.f,
            "frontOnly skips near back face, continues to far front face");
  // above, travelling -Z: T1 back face first, T0 front face behind it
  CHECK_MSG(fx.Cast({1, 1, 5}, {0, 0, -1}, false, false, 10, t, nz) && std::fabs(t - 3.f) < 1e-4f,
            "default from above: back face T1");
  CHECK_MSG(fx.Cast({1, 1, 5}, {0, 0, -1}, true, true, 10, t, nz) && std::fabs(t - 5.f) < 1e-4f &&
                nz > 0.f,
            "frontOnly from above: front face T0");
  // only a back face within range: front-only misses
  CHECK_MSG(!fx.Cast({1, 1, -1}, {0, 0, 1}, true, true, 2, t, nz), "frontOnly misses when only back");
  CHECK_MSG(fx.Cast({1, 1, -1}, {0, 0, 1}, true, false, 2, t, nz), "two-sided hits same range");
  // coplanar
  CHECK_MSG(!fx.Cast({-1, 1, 0}, {1, 0, 0}, true, true, 10, t, nz), "coplanar frontOnly miss");
  CHECK_MSG(!fx.Cast({-1, 1, 0}, {1, 0, 0}, true, false, 10, t, nz), "coplanar two-sided miss");
  // t == 0 on the near front face
  CHECK_MSG(fx.Cast({1, 1, 0}, {0, 0, -1}, true, true, 10, t, nz) && t == 0.f && nz > 0.f,
            "t=0 front face hit frontOnly");
  CHECK_MSG(fx.Cast({1, 1, 0}, {0, 0, -1}, false, false, 10, t, nz) && t == 0.f,
            "t=0 default hit");
  // t == 0 on a back face
  CHECK_MSG(!fx.Cast({1, 1, 0}, {0, 0, 1}, true, true, 1, t, nz), "t=0 back face frontOnly miss");

  // Recursive paths: near child holds only a back face, far child a front face.
  // z=0 plane normal +Z (back face for a +Z ray); z=2 plane normal -Z (front face).
  for (int mode = 0; mode < 3; ++mode) {
    Fixture br;
    br.Add({-8, -8, -4}, {-2, -8, -4}, {-8, -2, -4}); // tri 0: normal +Z, back face
    br.Add({-8, -8, 4}, {-8, -2, 4}, {-2, -8, 4});    // tri 1: normal -Z, front face
    if (mode == 2) {
      // 2-child path, near back face in the RIGHT child, far front face in the LEFT child
      br.BuildBranch(0x000a, {{0, {1}}, {1, {0}}});
    } else if (mode == 0) {
      // 2-child path (flags 0xa): both children leaves; each tested independently
      br.BuildBranch(0x000a, {{0, {0}}, {1, {1}}});
    } else {
      // 8-way path: child 0 (low z) and child 4 (high z) leaves, ray crosses the z=0 centre
      br.BuildBranch(0x0202, {{0, {0}}, {4, {1}}});
    }
    const char* name = mode == 0 ? "2-child" : mode == 1 ? "8-way" : "2-child swapped";
    char msg[96];
    float rt = 0, rn = 0;
    std::snprintf(msg, sizeof msg, "%s default returns near back face", name);
    CHECK_MSG(br.Cast({-5, -5, -8}, {0, 0, 1}, false, false, 20, rt, rn) && std::fabs(rt - 4.f) < 1e-3f &&
                  rn > 0.f,
              msg);
    std::snprintf(msg, sizeof msg, "%s frontOnly skips near, returns far front", name);
    CHECK_MSG(br.Cast({-5, -5, -8}, {0, 0, 1}, true, true, 20, rt, rn) && std::fabs(rt - 12.f) < 1e-3f &&
                  rn < 0.f,
              msg);
    std::snprintf(msg, sizeof msg, "%s frontOnly misses when range ends before far", name);
    CHECK_MSG(!br.Cast({-5, -5, -8}, {0, 0, 1}, true, true, 6, rt, rn), msg);
  }

  if (port_test::Failures() == 0) {
    std::printf("ok\n");
  }
  return PORT_TEST_RESULT();
}
