// SPDX-License-Identifier: GPL-3.0-or-later
#include "port_cannon_repair.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <stdexcept>

namespace PortCannonRepair {
namespace {
using Bytes = std::vector< uint8_t >;
using Weights = std::map< uint32_t, float >;
struct Vec {
  float x = 0, y = 0, z = 0;
  Vec operator+(Vec b) const { return {x + b.x, y + b.y, z + b.z}; }
  Vec operator-(Vec b) const { return {x - b.x, y - b.y, z - b.z}; }
  Vec operator*(float f) const { return {x * f, y * f, z * f}; }
};
float Dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec Cross(Vec a, Vec b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
Vec Normalize(Vec a) {
  const float len = std::sqrt(Dot(a, a));
  return len > 1.e-12f ? a * (1.f / len) : Vec{0, 0, 1};
}
uint32_t U32(const Bytes& d, size_t p) {
  if (p > d.size() || d.size() - p < 4)
    throw std::runtime_error("cannon resource bounds");
  return uint32_t(d[p]) << 24 | uint32_t(d[p + 1]) << 16 | uint32_t(d[p + 2]) << 8 | d[p + 3];
}
uint16_t U16(const Bytes& d, size_t p) {
  if (p > d.size() || d.size() - p < 2)
    throw std::runtime_error("cannon index bounds");
  return uint16_t(d[p]) << 8 | d[p + 1];
}
float F32(const Bytes& d, size_t p) {
  const auto w = U32(d, p);
  float f;
  std::memcpy(&f, &w, 4);
  return f;
}
void Put32(Bytes& d, size_t p, uint32_t v) {
  if (p > d.size() || d.size() - p < 4)
    throw std::runtime_error("cannon output bounds");
  for (unsigned i = 0; i < 4; ++i)
    d[p + i] = uint8_t(v >> (24 - i * 8));
}
void Add32(Bytes& d, uint32_t v) {
  const size_t p = d.size();
  d.resize(p + 4);
  Put32(d, p, v);
}
void Add16(Bytes& d, size_t v) {
  if (v >= 65536)
    throw std::runtime_error("cannon vertex limit");
  d.push_back(uint8_t(v >> 8));
  d.push_back(uint8_t(v));
}
void AddFloat(Bytes& d, float f) {
  uint32_t w;
  std::memcpy(&w, &f, 4);
  Add32(d, w);
}
void AddVec(Bytes& d, Vec v) {
  AddFloat(d, v.x);
  AddFloat(d, v.y);
  AddFloat(d, v.z);
}
void Pad(Bytes& d) { d.resize((d.size() + 31) & ~size_t(31), 0); }
uint64_t Hash(const Bytes& d) {
  uint64_t h = UINT64_C(14695981039346656037);
  for (auto b : d)
    h = (h ^ b) * UINT64_C(1099511628211);
  return h;
}
uint32_t MirrorBone(uint32_t b) {
  // Real paired animation bones; the CINF fingerprint verifies this exact skeleton.
  if (b == 19)
    return 20;
  if (b == 20)
    return 19;
  if (b == 21)
    return 22;
  if (b == 22)
    return 21;
  if (b >= 27 && b <= 36)
    return b + 13; // g2..g11 <-> L_g2..L_g11
  if (b >= 40 && b <= 49)
    return b - 13;
  if (b == 38)
    return 50;
  if (b == 50)
    return 38; // gBase
  if (b == 39)
    return 51;
  if (b == 51)
    return 39; // gPanel
  return b;    // centre/rings/g1/g12
}
Weights BlendWeights(const Weights& a, const Weights& b, float t) {
  Weights out;
  for (const auto& w : a)
    out[w.first] += w.second * (1 - t);
  for (const auto& w : b)
    out[w.first] += w.second * t;
  float sum = 0;
  for (auto it = out.begin(); it != out.end();) {
    if (it->second < 1.e-6f)
      it = out.erase(it);
    else {
      sum += it->second;
      ++it;
    }
  }
  if (out.empty() || out.size() > 4)
    throw std::runtime_error("cannon skin influences");
  for (auto& w : out)
    w.second /= sum;
  return out;
}
Weights ReflectWeights(const Weights& in) {
  Weights out;
  for (const auto& w : in)
    out[MirrorBone(w.first)] += w.second;
  return out;
}
struct Vertex {
  Vec p, n;
  std::array< std::array< float, 2 >, 3 > uv{};
  Weights weights;
};
void Seam(Vertex& v) {
  // Shared weights as well as a shared point keep the join closed during asymmetric animations.
  v.p.x = 0;
  v.n.x = 0;
  v.n = Normalize(v.n);
  v.weights = BlendWeights(v.weights, ReflectWeights(v.weights), .5f);
}
Vertex Interpolate(const Vertex& a, const Vertex& b, float t) {
  Vertex v;
  v.p = a.p * (1 - t) + b.p * t;
  v.n = Normalize(a.n * (1 - t) + b.n * t);
  for (unsigned i = 0; i < 3; ++i)
    for (unsigned j = 0; j < 2; ++j)
      v.uv[i][j] = a.uv[i][j] * (1 - t) + b.uv[i][j] * t;
  v.weights = BlendWeights(a.weights, b.weights, t);
  Seam(v);
  return v;
}
struct Triangle {
  std::array< Vertex, 3 > v;
  size_t surface;
  uint8_t format;
};
using Key = std::vector< int64_t >;
Key WeightKey(const Weights& w) {
  Key k;
  for (const auto& a : w) {
    k.push_back(a.first);
    k.push_back(std::llround(a.second * 1000000));
  }
  return k;
}
Key VertexKey(const Vertex& v) {
  Key k = WeightKey(v.weights);
  k.push_back(-1);
  for (float f : {v.p.x, v.p.y, v.p.z, v.n.x, v.n.y, v.n.z})
    k.push_back(std::llround(f * 1000000));
  return k;
}

bool Rebuild(const Bytes& model, const Bytes& skin, Result& result) {
  const size_t count = U32(model, 36), matSets = U32(model, 40);
  size_t offset = (44 + count * 4 + 31) & ~size_t(31);
  std::vector< Bytes > parts;
  for (size_t i = 0; i < count; ++i) {
    const size_t len = U32(model, 44 + i * 4);
    if (offset + len > model.size())
      return false;
    parts.emplace_back(model.begin() + offset, model.begin() + offset + len);
    offset += len;
  }
  if (matSets != 5 || parts.size() != 19 || U32(model, 8) != 5)
    return false;
  const size_t si = 10, ns = U32(parts[si], 0);
  if (ns != 8)
    return false;
  std::vector< Weights > skinWeights;
  size_t sp = 4;
  for (size_t i = 0; i < U32(skin, 0); ++i) {
    const size_t n = U32(skin, sp);
    sp += 4;
    Weights w;
    for (size_t j = 0; j < n; ++j) {
      const auto b = U32(skin, sp);
      w[b] = F32(skin, sp + 4);
      sp += 8;
    }
    const size_t len = U32(skin, sp);
    sp += 4;
    skinWeights.insert(skinWeights.end(), len, w);
  }
  if (skinWeights.size() != 1920 || parts[5].size() != 1920 * 12 || parts[6].size() != 1920 * 12)
    return false;
  const auto& mats = parts[0];
  size_t mp = 4 + U32(mats, 0) * 4;
  const size_t nm = U32(mats, mp);
  mp += 4;
  const size_t ends = mp;
  mp += nm * 4;
  std::vector< uint32_t > descs;
  for (size_t i = 0; i < nm; ++i) {
    const size_t at = mp + (i ? U32(mats, ends + (i - 1) * 4) : 0);
    descs.push_back(U32(mats, at + 8 + U32(mats, at + 4) * 4));
  }
  // Preserve all five beam material sets and verify they share the same corner layout.
  for (size_t set = 1; set < matSets; ++set) {
    const auto& ms = parts[set];
    size_t at = 4 + U32(ms, 0) * 4;
    if (U32(ms, at) != nm)
      return false;
    at += 4;
    const size_t ep = at;
    at += nm * 4;
    for (size_t i = 0; i < nm; ++i) {
      const size_t m = at + (i ? U32(ms, ep + (i - 1) * 4) : 0);
      if (U32(ms, m + 8 + U32(ms, m + 4) * 4) != descs[i])
        return false;
    }
  }
  std::vector< Triangle > output;
  std::vector< size_t > uvCounts(ns);
  for (size_t surf = 0; surf < ns; ++surf) {
    const auto& data = parts[si + 1 + surf];
    const size_t mi = U32(data, 12);
    if (mi >= descs.size())
      return false;
    const auto desc = descs[mi];
    if (desc != 0xf0f && desc != 0x3f0f)
      return false;
    const size_t nuv = desc == 0xf0f ? 2 : 3, stride = 4 + nuv * 2;
    uvCounts[surf] = nuv;
    size_t at = (0x4b + U32(data, 28)) & ~size_t(31);
    const size_t end = at + (U32(data, 16) & 0x7fffffff);
    if (end > data.size())
      return false;
    auto load = [&](size_t corner) {
      Vertex v;
      const size_t pi = U16(data, corner), ni = U16(data, corner + 2);
      if (pi >= skinWeights.size() || ni >= skinWeights.size() ||
          skinWeights[pi] != skinWeights[ni])
        throw std::runtime_error("cannon position/normal bone mismatch");
      const size_t p = pi * 12, n = ni * 12;
      v.p = {F32(parts[5], p), F32(parts[5], p + 4), F32(parts[5], p + 8)};
      v.n = {F32(parts[6], n), F32(parts[6], n + 4), F32(parts[6], n + 8)};
      v.weights = skinWeights[pi];
      for (size_t j = 0; j < nuv; ++j) {
        const size_t uv = U16(data, corner + 4 + j * 2) * 8;
        v.uv[j] = {F32(parts[8], uv), F32(parts[8], uv + 4)};
      }
      if (std::abs(v.p.x) < 1.e-6f)
        Seam(v);
      return v;
    };
    auto emit = [&](size_t a, size_t b, size_t c, uint8_t format, size_t begin) {
      std::vector< Vertex > poly = {load(begin + a * stride), load(begin + b * stride),
                                    load(begin + c * stride)},
                            clipped;
      // Clip crossing triangles instead of keeping whole triangles from the bad half.
      for (size_t i = 0; i < poly.size(); ++i) {
        const auto& v = poly[i];
        const auto& prev = poly[(i + poly.size() - 1) % poly.size()];
        const bool inside = v.p.x <= 0, was = prev.p.x <= 0;
        if (inside != was)
          clipped.push_back(Interpolate(prev, v, -prev.p.x / (v.p.x - prev.p.x)));
        if (inside)
          clipped.push_back(v);
      }
      for (size_t i = 1; i + 1 < clipped.size(); ++i) {
        Triangle t{{clipped[0], clipped[i], clipped[i + 1]}, surf, format};
        const Vec cross = Cross(t.v[1].p - t.v[0].p, t.v[2].p - t.v[0].p);
        if (Dot(cross, cross) < 1.e-18f)
          continue;
        // A face wholly on the cut plane is an internal divider, not exterior skin.
        if (t.v[0].p.x == 0 && t.v[1].p.x == 0 && t.v[2].p.x == 0)
          continue;
        output.push_back(t);
        std::swap(t.v[1], t.v[2]);
        for (auto& v : t.v) {
          v.p.x = -v.p.x;
          v.n.x = -v.n.x;
          v.weights = ReflectWeights(v.weights);
        }
        output.push_back(t);
      }
    };
    while (at < end && data[at]) {
      const uint8_t op = data[at] & 0xf8, format = data[at] & 7;
      const size_t n = U16(data, at + 1);
      at += 3;
      if (n > (end - at) / stride)
        return false;
      if (op == 0x90 && n % 3 == 0) {
        for (size_t i = 0; i < n; i += 3)
          emit(i, i + 1, i + 2, format, at);
      } else if (op == 0x98) {
        for (size_t i = 0; i + 2 < n; ++i)
          emit(i + (i & 1), i + 1 - (i & 1), i + 2, format, at);
      } else if (op == 0xa0) {
        for (size_t i = 1; i + 1 < n; ++i)
          emit(0, i, i + 1, format, at);
      } else if (op == 0x80 && n % 4 == 0) {
        for (size_t i = 0; i < n; i += 4) {
          emit(i, i + 1, i + 2, format, at);
          emit(i, i + 2, i + 3, format, at);
        }
      } else
        return false;
      at += n * stride;
    }
  }
  // UVs are corner attributes: they do not split the welded position/normal/weight vertices.
  // Sort by influence set because CSKR stores consecutive virtual-bone runs.
  std::map< Key, Vertex > vertices;
  for (const auto& t : output)
    for (const auto& v : t.v)
      vertices.emplace(VertexKey(v), v);
  std::map< Key, size_t > indices;
  std::vector< Vertex > ordered;
  for (const auto& item : vertices) {
    indices[item.first] = ordered.size();
    ordered.push_back(item.second);
  }
  parts[5].clear();
  parts[6].clear();
  parts[8].clear();
  Bytes newSkin;
  std::vector< std::pair< Weights, size_t > > groups;
  for (const auto& v : ordered) {
    AddVec(parts[5], v.p);
    AddVec(parts[6], Normalize(v.n));
    if (groups.empty() || WeightKey(groups.back().first) != WeightKey(v.weights))
      groups.push_back({v.weights, 0});
    ++groups.back().second;
    if (v.p.x == 0)
      ++result.seamVertices;
  }
  Add32(newSkin, uint32_t(groups.size()));
  for (const auto& g : groups) {
    Add32(newSkin, uint32_t(g.first.size()));
    for (const auto& w : g.first) {
      Add32(newSkin, w.first);
      AddFloat(newSkin, w.second);
    }
    Add32(newSkin, uint32_t(g.second));
  }
  Add32(newSkin, 0xffffffff);
  Add32(newSkin, uint32_t(ordered.size()));
  Add32(newSkin, 0xffffffff);
  Add32(newSkin, uint32_t(ordered.size()));
  Pad(parts[5]);
  Pad(parts[6]);
  std::vector< Bytes > lists(ns);
  std::map< std::array< uint32_t, 2 >, size_t > uvIndices;
  auto uvIndex = [&](const std::array< float, 2 >& uv) {
    std::array< uint32_t, 2 > key;
    std::memcpy(key.data(), uv.data(), 8);
    const auto found = uvIndices.find(key);
    if (found != uvIndices.end())
      return found->second;
    const size_t index = parts[8].size() / 8;
    uvIndices[key] = index;
    AddFloat(parts[8], uv[0]);
    AddFloat(parts[8], uv[1]);
    return index;
  };
  for (const auto& t : output) {
    auto& list = lists[t.surface];
    list.push_back(0x90 | t.format);
    list.push_back(0);
    list.push_back(3);
    for (const auto& v : t.v) {
      const size_t index = indices.at(VertexKey(v));
      Add16(list, index);
      Add16(list, index);
      for (size_t i = 0; i < uvCounts[t.surface]; ++i)
        Add16(list, uvIndex(v.uv[i]));
    }
  }
  Pad(parts[8]);
  size_t cumulative = 0;
  for (size_t i = 0; i < ns; ++i) {
    auto& s = parts[si + 1 + i];
    const size_t start = (0x4b + U32(s, 28)) & ~size_t(31);
    Pad(lists[i]);
    s.resize(start);
    s.insert(s.end(), lists[i].begin(), lists[i].end());
    Put32(s, 16, uint32_t(lists[i].size()));
    if (U32(s, 28) >= 24)
      for (size_t j = 0; j < 6; ++j)
        Put32(s, 44 + j * 4, U32(model, 12 + j * 4));
    cumulative += s.size();
    Put32(parts[si], 4 + i * 4, uint32_t(cumulative));
  }
  Bytes rebuilt(model.begin(), model.begin() + ((44 + count * 4 + 31) & ~size_t(31)));
  for (size_t i = 0; i < parts.size(); ++i) {
    Put32(rebuilt, 44 + i * 4, uint32_t(parts[i].size()));
    rebuilt.insert(rebuilt.end(), parts[i].begin(), parts[i].end());
  }
  result.model = {0x434d444c, std::move(rebuilt)};
  result.skin = {0x43534b52, std::move(newSkin)};
  result.triangles = output.size();
  return result.triangles > 2000 && result.seamVertices > 0;
}
std::mutex mutex;
bool attempted = false;
Result cached;
} // namespace
bool Build(const Bytes& model, const Bytes& skin, const Bytes& skeleton, Result& result) {
  result = Result{};
  if (Hash(model) != UINT64_C(0xb05e91167c85f48d) || Hash(skin) != UINT64_C(0xd974f8278da52c52) ||
      Hash(skeleton) != UINT64_C(0x47802be885e3be9f))
    return false;
  try {
    Result rebuilt;
    if (!Rebuild(model, skin, rebuilt))
      return false;
    result = std::move(rebuilt);
    return true;
  } catch (const std::exception&) {
    return false;
  }
}
bool IsResource(uint32_t id) { return id == kModel || id == kSkin; }
const PortCustomRes::Resource* Find(uint32_t id, const PortCustomRes::DiscReader& read) {
  if (!IsResource(id))
    return nullptr;
  std::lock_guard< std::mutex > lock(mutex);
  if (!attempted) {
    attempted = true;
    Bytes m, s, b;
    if (read(kModel, m) && read(kSkin, s) && read(kSkeleton, b))
      Build(m, s, b, cached);
  }
  if (cached.model.data.empty())
    return nullptr;
  return id == kModel ? &cached.model : &cached.skin;
}
void Reset() {
  std::lock_guard< std::mutex > lock(mutex);
  cached = Result{};
  attempted = false;
}
} // namespace PortCannonRepair
