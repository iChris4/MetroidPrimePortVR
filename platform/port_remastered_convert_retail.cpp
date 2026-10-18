// The Remastered model converter's retail side: the retail CMDL's materials and surfaces,
// and its CSKR. See port_remastered_convert_internal.h.

#include "port_remastered_convert_internal.h"

namespace PortRemastered {
namespace ConvertDetail {

// ---- retail side ----


RetailMaterial ParseMaterial(Span m) {
  RetailMaterial out;
  size_t o = 0;
  out.flags = R32(m, o);
  o += 4;
  const uint32_t nt = R32(m, o);
  o += 4;
  for (uint32_t i = 0; i < nt; ++i, o += 4) {
    out.tex.push_back(R32(m, o));
  }
  out.vtx = R32(m, o);
  o += 4;
  out.group = R32(m, o);
  o += 4;
  if (out.flags & 0x8) {
    const uint32_t nk = R32(m, o);
    if (nk != 0) {
      out.konstAlpha = double(R8(m, o + 4 + 3)) / 255.0;
    }
    o += 4 + size_t(nk) * 4;
  }
  out.blendDst = R16(m, o);
  out.blendSrc = R16(m, o + 2);
  o += 4;
  if (out.flags & 0x400) {
    o += 4;
  }
  const uint32_t nch = R32(m, o);
  o += 4;
  for (uint32_t i = 0; i < nch; ++i, o += 4) {
    out.chans.push_back(R32(m, o));
  }
  const uint32_t ntev = R32(m, o);
  o += 4;
  for (uint32_t i = 0; i < ntev; ++i, o += 20) {
    out.tev.push_back({R32(m, o), R32(m, o + 4), R32(m, o + 8), R32(m, o + 12)});
  }
  for (uint32_t i = 0; i < ntev; ++i, o += 4) {
    out.tevTex.push_back({R8(m, o), R8(m, o + 1), R8(m, o + 2), R8(m, o + 3)});
  }
  const uint32_t ntg = R32(m, o);
  o += 4;
  for (uint32_t i = 0; i < ntg; ++i, o += 4) {
    out.texgen.push_back(R32(m, o));
  }
  // The UV animations load the matrices those texgens name: without them a texgen
  // reads whatever matrix the material drawn before left.
  if (o <= m.n && m.n - o >= 8) {
    const Span anim = Slice(m, o, 4 + size_t(R32(m, o)));
    if (anim.n >= 8 && anim.n == 4 + size_t(R32(m, o))) {
      out.uvAnim.assign(anim.p, anim.p + anim.n);
    }
  }
  return out;
}

MaterialSet ParseSet(Span sec) {
  MaterialSet out;
  const uint32_t nt = R32(sec, 0);
  size_t o = 4;
  for (uint32_t i = 0; i < nt; ++i, o += 4) {
    out.tex.push_back(R32(sec, o));
  }
  const uint32_t nm = R32(sec, o);
  o += 4;
  const size_t base = o + size_t(nm) * 4;
  uint32_t start = 0;
  for (uint32_t i = 0; i < nm; ++i, o += 4) {
    const uint32_t end = R32(sec, o);
    if (end < start) {
      throw Fail{"a retail material table is out of order"};
    }
    out.mats.push_back(Sub(sec, base + start, end - start));
    start = end;
  }
  return out;
}

// Bytes per vertex in a display list: matrix indices are u8, attributes u16.
size_t VtxStride(uint32_t vtx) {
  size_t n = 0;
  for (int bit = 24; bit < 32; ++bit) {
    if (vtx & (1u << bit)) {
      ++n;
    }
  }
  for (int sh = 0; sh < 24; sh += 2) {
    if ((vtx >> sh) & 3) {
      n += 2;
    }
  }
  return n;
}

int Popcount(uint32_t v) {
  int n = 0;
  for (; v; v &= v - 1) {
    ++n;
  }
  return n;
}

// Position indices of a display list, as a flat triangle list.
void Triangles(Span dl, size_t stride, size_t mtx, std::vector<uint32_t>& tris) {
  size_t o = 0;
  std::vector<uint32_t> idx;
  while (o + 3 <= dl.n) {
    const uint8_t op = dl.p[o] & 0xF8;
    if (op != 0x80 && op != 0x90 && op != 0x98 && op != 0xA0 && op != 0xA8 && op != 0xB0 && op != 0xB8) {
      break;
    }
    const size_t n = R16(dl, o + 1);
    o += 3;
    if (n * stride > dl.n - o || (n && stride < mtx + 2)) {
      break;
    }
    idx.resize(n);
    for (size_t k = 0; k < n; ++k) {
      idx[k] = R16(dl, o + k * stride + mtx);
    }
    o += n * stride;
    if (op == 0x90) {
      tris.insert(tris.end(), idx.begin(), idx.begin() + n / 3 * 3);
    } else if (op == 0x98) {
      for (size_t k = 0; k + 2 < n; ++k) {
        if (k % 2 == 0) {
          tris.insert(tris.end(), {idx[k], idx[k + 1], idx[k + 2]});
        } else {
          tris.insert(tris.end(), {idx[k + 1], idx[k], idx[k + 2]});
        }
      }
    } else if (op == 0xA0) {
      for (size_t k = 1; k + 1 < n; ++k) {
        tris.insert(tris.end(), {idx[0], idx[k], idx[k + 1]});
      }
    } else if (op == 0x80) {
      for (size_t k = 0; k + 3 < n; k += 4) {
        tris.insert(tris.end(), {idx[k], idx[k + 1], idx[k + 2], idx[k], idx[k + 2], idx[k + 3]});
      }
    }
  }
}

const char* RoleName(Role r) {
  switch (r) {
  case Role::EnvMap:
    return "envmap";
  case Role::Lightmap:
    return "lightmap";
  case Role::Reflect:
    return "reflect";
  case Role::Emissive:
    return "emissive";
  case Role::Diffuse:
    return "diffuse";
  default:
    return "keep";
  }
}

// What each textured TEV stage of a retail material does with its texture.
std::vector<SlotRole> SlotRoles(const RetailMaterial& m) {
  // TEV colour inputs: CPREV APREV C0 A0 C1 A1 C2 A2 TEXC TEXA RASC RASA ONE HALF KONST ZERO
  enum { CPREV = 0, TEXC = 8, RASC = 10, ONE = 12 };
  std::vector<SlotRole> out;
  const size_t n = std::min(m.tev.size(), m.tevTex.size());
  for (size_t i = 0; i < n; ++i) {
    const uint32_t slot = m.tevTex[i][2], coord = m.tevTex[i][3];
    if (slot == 0xFF || coord >= m.texgen.size()) {
      continue;
    }
    const uint32_t tg = m.texgen[coord];
    const uint32_t src = (tg >> 4) & 31;
    const uint32_t c = m.tev[i].color;
    const uint32_t b = (c >> 5) & 15, cc = (c >> 10) & 15, d = (c >> 15) & 15;
    const uint32_t outReg = (m.tev[i].colorOp >> 9) & 3;
    Role role;
    if (src < 4) {
      role = Role::EnvMap;  // a sphere-mapped texgen over position/normal
    } else if (outReg == 1) {
      role = Role::Lightmap;
    } else if (outReg == 3) {
      role = Role::Reflect;
    } else if ((b == CPREV || d == CPREV) && (b == TEXC || d == TEXC) && cc == ONE) {
      role = Role::Emissive;
    } else if (d == TEXC && b == ONE && cc == RASC) {
      role = Role::Emissive;
    } else {
      role = Role::Diffuse;
    }
    out.push_back({slot, coord, role, int(src) - 4});
  }
  return out;
}

std::vector<SkinGroup> ReadCskr(Span d) {
  std::vector<SkinGroup> groups;
  const uint32_t n = R32(d, 0);
  size_t o = 4;
  for (uint32_t i = 0; i < n; ++i) {
    SkinGroup g;
    const uint32_t nw = R32(d, o);
    o += 4;
    for (uint32_t k = 0; k < nw; ++k, o += 8) {
      g.weights.emplace_back(R32(d, o), RF(d, o));
      g.weights.back().second = RF(d, o + 4);
    }
    g.count = R32(d, o);
    o += 4;
    groups.push_back(std::move(g));
  }
  return groups;
}

std::vector<uint32_t> SkinBones(Span d) {
  std::vector<uint32_t> bones;
  for (const SkinGroup& g : ReadCskr(d)) {
    for (const auto& w : g.weights) {
      bones.push_back(w.first);
    }
  }
  std::sort(bones.begin(), bones.end());
  bones.erase(std::unique(bones.begin(), bones.end()), bones.end());
  return bones;
}

}  // namespace ConvertDetail

}  // namespace PortRemastered
