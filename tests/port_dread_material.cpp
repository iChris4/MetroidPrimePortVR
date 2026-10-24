// Dread suit material -> synthetic Remastered PBR material (platform/port_dread_material.cpp).
//
// Fixtures are synthetic: tiny RGBA8 .bctex blobs (built here, the way port_dread_readers_a
// does) and a Bsmat filled in memory. The synthetic ModelMaterial is also run through the
// converter's ReadMaterial to check the maps it makes and that their ids resolve through
// FindTexture. The real-data check is opt-in: PORT_DREAD_SAMUS_PKG_DIR names the extracted
// Samus pkg (build/dread/samus_pkg) and the textures come from PORT_DREAD_TEXTURES_DIR, else
// $PORT_DREAD_ROMFS/textures, else <pkg dir>/../romfs/textures; otherwise it prints SKIP.
#include "port_dread_material.h"
#include "port_remastered_convert_internal.h"
#include "port_remastered_pak.h"
#include "port_test.h"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace PortDread;
using PortRemastered::Image;
using PortRemastered::ModelUuid;
namespace CD = PortRemastered::ConvertDetail;

namespace {

using Blob = std::vector<uint8_t>;

void Put32(Blob& out, uint32_t v) {
  for (int s = 0; s < 32; s += 8) {
    out.push_back(uint8_t(v >> s));
  }
}
void Put64(Blob& out, uint64_t v) {
  Put32(out, uint32_t(v));
  Put32(out, uint32_t(v >> 32));
}

// Tegra block-linear tiling of one mip: the inverse of the port's untiling, from the GOB address
// formula (x in bytes) in kb format/dread-bctex.md. `blockHeight` is in GOBs.
Blob SwizzleMip(const Blob& linear, size_t w, size_t h, size_t bpp, size_t blockHeight) {
  const size_t rowBytes = w * bpp;
  const size_t gobsX = (rowBytes + 63) / 64;
  const size_t gobRows = (h + 7) / 8;
  const size_t blockRows = (gobRows + blockHeight - 1) / blockHeight;
  Blob out(blockRows * blockHeight * gobsX * 512, 0xEE);
  for (size_t y = 0; y < h; ++y) {
    for (size_t x = 0; x < rowBytes; ++x) {
      const size_t gobRow = y / 8;
      const size_t gobOff = ((x % 64) / 32) * 256 + ((y % 8) / 2) * 64 + ((x % 32) / 16) * 32 + (y % 2) * 16 + x % 16;
      const size_t at = ((gobRow / blockHeight) * gobsX * blockHeight + (x / 64) * blockHeight + gobRow % blockHeight) * 512 + gobOff;
      out[at] = linear[y * rowBytes + x];
    }
  }
  return out;
}

Blob Gzip(const Blob& in) {
  z_stream zs = {};
  deflateInit2(&zs, 6, Z_DEFLATED, 31, 8, Z_DEFAULT_STRATEGY);
  Blob out(deflateBound(&zs, uLong(in.size())) + 64);
  zs.next_in = const_cast<Bytef*>(in.data());
  zs.avail_in = uInt(in.size());
  zs.next_out = out.data();
  zs.avail_out = uInt(out.size());
  deflate(&zs, Z_FINISH);
  out.resize(out.size() - zs.avail_out);
  deflateEnd(&zs);
  return out;
}

struct TexSpec {
  uint32_t w = 16, h = 16, mips = 1, format = 0x42, layout1 = 0, bpp = 8;
  bool compressed = true;
};

// Builds a .bctex whose mip k holds `linear[k]`; returns the file.
Blob MakeBctex(const TexSpec& s, const std::vector<Blob>& linear) {
  Blob surface;
  uint32_t mipOffset[17] = {};
  for (uint32_t k = 0; k < s.mips; ++k) {
    const size_t mw = std::max(s.w >> k, 1u), mh = std::max(s.h >> k, 1u);
    const size_t bw = s.compressed ? (mw + 3) / 4 : mw, bh = s.compressed ? (mh + 3) / 4 : mh;
    size_t blockHeight = size_t(1) << (s.layout1 & 7);
    while (bh <= (blockHeight / 2) * 8 && blockHeight > 1) {
      blockHeight /= 2;
    }
    mipOffset[k] = uint32_t(surface.size());
    const Blob tiled = SwizzleMip(linear[k], bw, bh, s.bpp, blockHeight);
    surface.insert(surface.end(), tiled.begin(), tiled.end());
  }
  Blob texHdr;
  Put64(texHdr, surface.size());
  for (uint32_t v : {512u, s.w, s.h, 1u, 1u, s.format, s.mips, uint32_t(surface.size())}) {
    Put32(texHdr, v);
  }
  for (uint32_t v : mipOffset) {
    Put32(texHdr, v);
  }
  Put32(texHdr, s.layout1);
  Put32(texHdr, 0x00010007);
  Put32(texHdr, 0);
  texHdr.resize(0x78, 0);

  Blob xtx;
  xtx.insert(xtx.end(), {'D', 'F', 'v', 'N'});
  Put32(xtx, 16);
  Put32(xtx, 1);
  Put32(xtx, 1);
  auto block = [&](const Blob& payload, uint32_t type, size_t dataOff) {
    const size_t start = xtx.size();
    xtx.insert(xtx.end(), {'H', 'B', 'v', 'N'});
    Put32(xtx, 0x24);
    Put64(xtx, payload.size());
    Put64(xtx, dataOff);
    Put32(xtx, type);
    Put32(xtx, 0);
    Put32(xtx, 0);
    xtx.resize(start + dataOff, 0);
    xtx.insert(xtx.end(), payload.begin(), payload.end());
  };
  block(texHdr, 2, 0x24);
  block(surface, 3, 0x154); // the data block starts a little way in, as in the game's files
  xtx.resize(xtx.size() + 60, 0);

  Blob inflated;
  Put64(inflated, 0x0002010300000000ull);
  for (uint32_t v : {s.w, s.h, s.mips, 0xFFFFFFFFu}) {
    Put32(inflated, v);
  }
  Put32(inflated, 0); // name offset (unused)
  Put32(inflated, 0);
  Put32(inflated, 0x80); // xtx_off: the XTX sits at xtx_off - 8
  Put32(inflated, 0);
  Put32(inflated, uint32_t(xtx.size()));
  inflated.resize(0x78, 0);
  inflated.insert(inflated.end(), xtx.begin(), xtx.end());

  Blob file = {'M', 'T', 'X', 'T'};
  Put32(file, 0x80001);
  const Blob gz = Gzip(inflated);
  file.insert(file.end(), gz.begin(), gz.end());
  return file;
}


// --- fixtures -----------------------------------------------------------------

// An RGBA8 bctex of w x h whose pixel i is px(i).
template <class F> Bctex MakeTex(uint32_t w, uint32_t h, F px) {
  TexSpec s;
  s.w = w, s.h = h, s.mips = 1, s.format = kBctexRgba8, s.bpp = 4, s.compressed = false;
  Blob lin;
  for (uint32_t i = 0; i < w * h; ++i) {
    const std::array<uint8_t, 4> p = px(i);
    lin.insert(lin.end(), p.begin(), p.end());
  }
  const Blob file = MakeBctex(s, {lin});
  Bctex t;
  std::string err;
  REQUIRE_MSG(t.Parse(file.data(), file.size(), err), err);
  return t;
}

constexpr uint32_t kN = 16;  // 4 x 4 bc and at; nm is 8 x 4 (sizes may differ)
std::array<uint8_t, 4> BcPx(uint32_t i) { return {uint8_t(i * 10 + 5), uint8_t(200 - i * 5), 100, uint8_t(i % 2 ? 128 : 255)}; }
std::array<uint8_t, 4> AtPx(uint32_t i) { return {77, uint8_t(i % 2 ? 10 : 200), uint8_t(i * 3), uint8_t(250 - i)}; }
std::array<uint8_t, 4> NmPx(uint32_t i) { return {uint8_t(i * 7), uint8_t(255 - i * 7), 99, 33}; }

Bsmat MakeBsmat(const char* name, double factor, std::vector<float> aec, std::vector<std::pair<const char*, std::vector<float>>> extra = {}) {
  Bsmat b;
  b.name = name;
  b.shader_path = "system/shd/mp_samuspbr.bshdat";
  b.cull_mode = kCullFront;
  BsmatStage st;
  auto uni = [&](const char* n, std::vector<float> v) {
    BsmatUniform u;
    u.name = n, u.type = 'f', u.f = std::move(v);
    st.uniforms.push_back(u);
  };
  uni("vAlbedoEmissiveColor", std::move(aec));
  uni("fAlbedoEmissiveFactor", {float(factor)});
  for (auto& e : extra) {
    uni(e.first, e.second);
  }
  const char* names[3] = {"texBaseColor", "texAttributes", "texNormals"};
  const char* suf[3] = {"bc", "at", "nm"};
  for (int i = 0; i < 3; ++i) {
    BsmatSampler s;
    s.name = names[i], s.index = uint32_t(i);
    s.filepath = std::string("actors/characters/samus/models/textures/synth_") + suf[i] + ".bctex";
    s.wrap_u = i == 0 ? kWrapClamp : kWrapRepeat;
    s.wrap_v = kWrapRepeat;
    st.samplers.push_back(s);
  }
  b.stages.push_back(st);
  return b;
}

struct Fixture {
  Bctex bc = MakeTex(4, 4, BcPx), at = MakeTex(4, 4, AtPx), nm = MakeTex(8, 4, NmPx);
};

const Image* Img(const SuitMaterial& m, SuitSlot slot) {
  for (const SuitImage& i : m.images) {
    if (i.id == SuitTextureId(m.name, slot)) {
      return &i.image;
    }
  }
  return nullptr;
}

double Srgb(double l) { return l <= 0.0031308 ? l * 12.92 : 1.055 * std::pow(l, 1.0 / 2.4) - 0.055; }
int Byte(double v) { return int(std::clamp(std::lround(v), 0l, 255l)); }

// --- tests --------------------------------------------------------------------

void TestChannels() {
  Fixture f;
  SuitMaterial m;
  std::string err;
  const Bsmat b = MakeBsmat("synth_power", 50, {0.04f, 1.f, 0.01f, 1.f}, {{"vConstant0", {0, 0, 0, 0.5f}}});
  REQUIRE_MSG(BuildSuitMaterial(b, f.bc, f.at, f.nm, m, &err), err);
  CHECK(m.cullFront);
  CHECK_EQ(m.name, std::string("synth_power"));
  CHECK_EQ(m.images.size(), size_t(4));
  const Image* bclr = Img(m, kSuitBclr);
  const Image* metl = Img(m, kSuitMetl);
  const Image* nmap = Img(m, kSuitNmap);
  const Image* ican = Img(m, kSuitIcan);
  REQUIRE(bclr && metl && nmap && ican);
  CHECK(bclr->width == 4 && bclr->height == 4 && metl->width == 4 && nmap->width == 8 && nmap->height == 4 && ican->width == 4);
  CHECK(bclr->srgb && ican->srgb && !metl->srgb && !nmap->srgb);
  for (uint32_t i = 0; i < kN; ++i) {
    const auto bc = BcPx(i), at = AtPx(i);
    // BCLR = bc.rgb x vConstant0.w (0.5), alpha 255.
    for (int k = 0; k < 3; ++k) {
      CHECK_EQ(int(bclr->rgba[i * 4 + k]), Byte(bc[k] * 0.5));
    }
    CHECK_EQ(int(bclr->rgba[i * 4 + 3]), 255);
    // METL: R = AO (at.a), G = max(at.g, 26), B = metal (at.b).
    CHECK_EQ(int(metl->rgba[i * 4 + 0]), int(at[3]));
    CHECK_EQ(int(metl->rgba[i * 4 + 1]), std::max(int(at[1]), 26));
    CHECK_EQ(int(metl->rgba[i * 4 + 2]), int(at[2]));
    CHECK_EQ(int(metl->rgba[i * 4 + 3]), 255);
  }
  CHECK_EQ(int(metl->rgba[1 * 4 + 1]), 26);   // at.g 10 floored to round(0.1 * 255)
  CHECK_EQ(int(metl->rgba[0 * 4 + 1]), 200);  // above the floor: untouched
  for (uint32_t i = 0; i < 32; ++i) {
    const auto nm = NmPx(i);
    CHECK_EQ(int(nmap->rgba[i * 4 + 0]), int(nm[0]));
    CHECK_EQ(int(nmap->rgba[i * 4 + 1]), int(nm[1]));
    CHECK_EQ(int(nmap->rgba[i * 4 + 2]), 0);
    CHECK_EQ(int(nmap->rgba[i * 4 + 3]), 255);
  }
  // Wrap: texBaseColor clamps in U, repeats in V.
  CHECK_EQ(m.material.data[0].texture.wrapX, 0);
  CHECK_EQ(m.material.data[0].texture.wrapY, 1);
  // Brightening past 255 clamps.
  const Bsmat bright = MakeBsmat("synth_bright", 0, {0, 0, 0, 0}, {{"vConstant0", {0, 0, 0, 3.f}}});
  SuitMaterial mb;
  REQUIRE_MSG(BuildSuitMaterial(bright, f.bc, f.at, f.nm, mb, &err), err);
  CHECK_EQ(int(Img(mb, kSuitBclr)->rgba[15 * 4 + 0]), 255);
  CHECK_EQ(int(Img(mb, kSuitBclr)->rgba[0 * 4 + 0]), 15);
}

void TestEmissive() {
  Fixture f;
  std::string err;
  // Power body: (0.04, 1, 0.01) x 50 -> C = (2, 50, 0.5), peak 50.
  {
    SuitMaterial m;
    REQUIRE_MSG(BuildSuitMaterial(MakeBsmat("synth_pb", 50, {0.04f, 1.f, 0.01f, 1.f}), f.bc, f.at, f.nm, m, &err), err);
    CHECK(std::fabs(m.emissiveLinear[0] - 2.f) < 1e-4f && std::fabs(m.emissiveLinear[1] - 50.f) < 1e-3f &&
          std::fabs(m.emissiveLinear[2] - 0.5f) < 1e-4f);
    const Image* ican = Img(m, kSuitIcan);
    REQUIRE(ican != nullptr);
    // bc.a 255 (pixel 0): colour = C / 50 = (0.04, 1, 0.01), sRGB encoded.
    CHECK_EQ(int(ican->rgba[0]), Byte(255 * Srgb(0.04)));
    CHECK_EQ(int(ican->rgba[0]), 56);
    CHECK_EQ(int(ican->rgba[1]), 255);
    CHECK_EQ(int(ican->rgba[2]), Byte(255 * Srgb(0.01)));
    CHECK_EQ(int(ican->rgba[3]), 255);
    // bc.a 128 (pixel 1) scales the linear colour first.
    CHECK_EQ(int(ican->rgba[4 + 1]), Byte(255 * Srgb(128.0 / 255.0)));
    const ModelUuid none{};
    bool found = false;
    double icnc = -1;
    for (const auto& d : m.material.data) {
      if (d.usage == port::FourCC('I', 'C', 'N', 'C')) {
        found = true;
        icnc = d.color[0];
        CHECK(d.color[0] == d.color[1] && d.color[1] == d.color[2] && d.color[3] == 1.f);
      }
    }
    CHECK(found && std::fabs(icnc - 50.0) < 1e-4);
    // REFL: the default cube, or the converter drops the env specular (black metal).
    bool refl = false;
    for (const auto& d : m.material.data) {
      if (d.usage == port::FourCC('R', 'E', 'F', 'L')) {
        refl = d.kind == PortRemastered::ModelMaterialData::Kind::Texture &&
               PortRemastered::IdToString(d.texture.id).rfind("7b98170f", 0) == 0;
      }
    }
    CHECK(refl);
  }
  // Hyper: no base emissive, vConstant3 (28, 0.08, 0.12) carries it: peak 28.
  {
    SuitMaterial m;
    const Bsmat b = MakeBsmat("synth_hyper", 40, {0, 0, 0, 1}, {{"vConstant3", {28.f, 0.08f, 0.12f}}});
    REQUIRE_MSG(BuildSuitMaterial(b, f.bc, f.at, f.nm, m, &err), err);
    CHECK(std::fabs(m.emissiveLinear[0] - 28.f) < 1e-4f);
    const Image* ican = Img(m, kSuitIcan);
    REQUIRE(ican != nullptr);
    CHECK_EQ(int(ican->rgba[0]), 255);
    CHECK_EQ(int(ican->rgba[1]), Byte(255 * Srgb(0.08 / 28.0)));
  }
  // vConstant4.r dims the base term (and not vConstant3): 0.5 x 50 -> peak 25, plus vC5.
  {
    SuitMaterial m;
    const Bsmat b = MakeBsmat("synth_dim", 50, {0.04f, 1.f, 0.01f, 1.f},
                              {{"vConstant4", {0.5f, 0, 0, 0}}, {"vConstant5", {0, 1.f, 0}}});
    REQUIRE_MSG(BuildSuitMaterial(b, f.bc, f.at, f.nm, m, &err), err);
    CHECK(std::fabs(m.emissiveLinear[1] - 26.f) < 1e-4f);
  }
  // No emissive at all: no ICAN image or slot.
  {
    SuitMaterial m;
    REQUIRE_MSG(BuildSuitMaterial(MakeBsmat("synth_cannon0", 11, {0, 0, 0, 1}), f.bc, f.at, f.nm, m, &err), err);
    CHECK_EQ(m.images.size(), size_t(3));
    CHECK(Img(m, kSuitIcan) == nullptr);
    for (const auto& d : m.material.data) {
      CHECK(d.usage != port::FourCC('I', 'C', 'A', 'N'));
    }
    CHECK(m.emissiveLinear[0] == 0.f && m.emissiveLinear[1] == 0.f && m.emissiveLinear[2] == 0.f);
  }
}

void TestIdsAndErrors() {
  Fixture f;
  std::string err;
  std::vector<ModelUuid> ids;
  for (const char* n : {"a", "b", "samuspower_powerbody"}) {
    for (uint32_t s = 0; s < 4; ++s) {
      const ModelUuid id = SuitTextureId(n, SuitSlot(s));
      CHECK(id != ModelUuid{});
      CHECK(id == SuitTextureId(n, SuitSlot(s)));  // deterministic
      CHECK(std::find(ids.begin(), ids.end(), id) == ids.end());
      ids.push_back(id);
    }
  }
  CHECK(ToModelOrder(ToModelOrder(ids[0])) == ids[0]);
  CHECK(ToModelOrder(ids[0]) != ids[0]);

  SuitMaterial m;
  Bsmat b = MakeBsmat("x", 1, {1, 1, 1, 1});
  b.shader_path = "system/shd/mp_opaque.bshdat";
  CHECK(!BuildSuitMaterial(b, f.bc, f.at, f.nm, m, &err));
  CHECK_MSG(err.find("unsupported shader") != std::string::npos, err);
  Bctex empty;
  err.clear();
  CHECK(!BuildSuitMaterial(MakeBsmat("x", 1, {1, 1, 1, 1}), f.bc, empty, f.nm, m, &err));
  CHECK(!err.empty());

  std::string pbc, pat, pnm;
  CHECK(SuitTexturePaths(MakeBsmat("x", 1, {1, 1, 1, 1}), "/tex", pbc, pat, pnm, &err));
  CHECK_EQ(pbc, std::string("/tex/actors/characters/samus/models/textures/synth_bc.bctex"));
  CHECK_EQ(pat, std::string("/tex/actors/characters/samus/models/textures/synth_at.bctex"));
  CHECK_EQ(pnm, std::string("/tex/actors/characters/samus/models/textures/synth_nm.bctex"));
  Bsmat nopath = MakeBsmat("x", 1, {1, 1, 1, 1});
  nopath.stages[0].samplers[1].filepath.clear();
  CHECK(!SuitTexturePaths(nopath, "/tex/", pbc, pat, pnm, &err));
}

// The synthetic ModelMaterial through the converter's own reader.
void TestHyperBall() {
  Fixture f;
  std::string err;
  Bsmat b = MakeBsmat("hyper_ball", 50, {0,0,0,1},
                      {{"vAlbedoEmissiveColorMultiplier", {0.8f,0.003f,0.003f,1}},
                       {"vConstant0", {1,1,1,1}}});
  b.shader_path = "system/shd/mp_opaque_constant_selfilum.bshdat";
  BsmatUniform tint;
  tint.name = "iTintAlbedo";
  tint.type = 'i';
  tint.i = {0};
  b.stages[0].uniforms.push_back(tint);
  SuitMaterial m;
  REQUIRE_MSG(BuildSuitMaterial(b, f.bc, f.at, f.nm, m, &err), err);
  CHECK(std::fabs(m.emissiveLinear[0] - 40) < 1e-5);
  CHECK(std::fabs(m.emissiveLinear[1] - .15) < 1e-5);
  const auto* base = Img(m, kSuitBclr);
  const auto* orm = Img(m, kSuitMetl);
  const auto* glow = Img(m, kSuitIcan);
  REQUIRE(base && orm && glow);
  for (uint32_t i = 0; i < kN; ++i) {
    const auto bc = BcPx(i), at = AtPx(i);
    for (int c = 0; c < 3; ++c) CHECK(base->rgba[4*i+c] == bc[c]);
    CHECK(base->rgba[4*i+3] == 255);
    CHECK(orm->rgba[4*i] == at[3]);
    CHECK(orm->rgba[4*i+1] == std::max<int>(at[1], Byte(.05*255)));
    CHECK(orm->rgba[4*i+2] == at[2]);
    const double mask = std::pow(bc[3]/255.0, 2.2);
    CHECK(glow->rgba[4*i] == Byte(255*Srgb(mask)));
    CHECK(glow->rgba[4*i+1] == Byte(255*Srgb(mask*.15/40)));
  }
  b.stages[0].uniforms.back().i = {1};
  CHECK(!BuildSuitMaterial(b, f.bc, f.at, f.nm, m, &err));
  b.stages[0].uniforms.back().i = {0};
  for (const char* path : {"system/shd/mp_samuspbr_extra.bshdat", "system/shd/mp_opaque_constant_selfilum_extra.bshdat", "mp_samuspbr"}) {
    b.shader_path = path;
    CHECK(!BuildSuitMaterial(b, f.bc, f.at, f.nm, m, &err));
  }
}

void TestReadMaterial() {
  Fixture f;
  std::string err;
  std::vector<SuitMaterial> mats(2);
  REQUIRE_MSG(BuildSuitMaterial(MakeBsmat("synth_pb", 50, {0.04f, 1.f, 0.01f, 1.f}), f.bc, f.at, f.nm, mats[0], &err), err);
  REQUIRE_MSG(BuildSuitMaterial(MakeBsmat("synth_none", 0, {0, 0, 0, 1}), f.bc, f.at, f.nm, mats[1], &err), err);
  const PortRemastered::ConvertOptions opt;
  {
    const CD::RemMaterial r = CD::ReadMaterial(mats[0].material, opt);
    CHECK_EQ(r.shader, kSuitShaderId);
    CHECK(!r.blended && !r.cutout && !r.unlit && !r.additive && !r.tinted);
    CHECK(std::fabs(r.emissive - 50.0) < 1e-4);
    const struct {
      int map;
      SuitSlot slot;
    } want[] = {{CD::kBase, kSuitBclr}, {CD::kMr, kSuitMetl}, {CD::kNormal, kSuitNmap}, {CD::kEmissive, kSuitIcan}};
    for (const auto& w : want) {
      const CD::MapRef& mr = r.maps[w.map];
      CHECK_MSG(mr.has, CD::kMapName[w.map]);
      // After ReadMaterial's swap the id is the pak-order one the provider is keyed by.
      CHECK(mr.id == SuitTextureId("synth_pb", w.slot));
      Image img;
      CHECK(FindTexture(mats, mr.id, img) && img.width > 0 && !img.rgba.empty());
      CHECK_EQ(mr.coord, uint32_t(0));
    }
    CHECK_EQ(r.maps[CD::kBase].wrap[0], 0);
    CHECK_EQ(r.maps[CD::kBase].wrap[1], 1);
    CHECK(!r.maps[CD::kBase].raw);
  }
  {
    const CD::RemMaterial r = CD::ReadMaterial(mats[1].material, opt);
    CHECK(r.maps[CD::kBase].has && r.maps[CD::kMr].has && r.maps[CD::kNormal].has && !r.maps[CD::kEmissive].has);
    CHECK(!r.blended && !r.cutout && !r.unlit);
  }
  // An id nobody made is not found.
  Image none;
  CHECK(!FindTexture(mats, SuitTextureId("other", kSuitBclr), none));
}

bool ReadFile(const std::string& path, Blob& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    return false;
  }
  out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return true;
}

bool LoadTex(const std::string& path, Bctex& t, std::string& err) {
  Blob b;
  if (!ReadFile(path, b)) {
    err = "cannot read " + path;
    return false;
  }
  return t.Parse(b.data(), b.size(), err);
}

void TestRealData() {
  const char* pkg = std::getenv("PORT_DREAD_SAMUS_PKG_DIR");
  if (pkg == nullptr || *pkg == '\0') {
    std::printf("SKIP: real-data suit material checks (PORT_DREAD_SAMUS_PKG_DIR not set)\n");
    return;
  }
  namespace fs = std::filesystem;
  std::string tex;
  if (const char* t = std::getenv("PORT_DREAD_TEXTURES_DIR"); t && *t) {
    tex = t;
  } else if (const char* r = std::getenv("PORT_DREAD_ROMFS"); r && *r) {
    tex = std::string(r) + "/textures";
  } else {
    tex = (fs::path(pkg) / ".." / "romfs" / "textures").string();
  }
  const fs::path imats = fs::path(pkg) / "actors/characters/samus/models/imats";
  if (!fs::is_directory(imats) || !fs::is_directory(tex)) {
    std::printf("SKIP: real-data suit material checks (%s or %s missing)\n", imats.string().c_str(), tex.c_str());
    return;
  }
  std::vector<fs::path> files;
  for (const auto& e : fs::directory_iterator(imats)) {
    if (e.path().extension() == ".bsmat") {
      files.push_back(e.path());
    }
  }
  std::sort(files.begin(), files.end());
  int built = 0;
  for (const fs::path& p : files) {
    Blob data;
    Bsmat b;
    std::string err;
    if (!ReadFile(p.string(), data) || !ParseBsmat(data.data(), data.size(), b, &err)) {
      CHECK_MSG(false, p.string() + ": " + err);
      continue;
    }
    if (b.shader_path.find("mp_samuspbr") == std::string::npos) {
      continue;
    }
    std::string pbc, pat, pnm;
    Bctex bc, at, nm;
    SuitMaterial m;
    if (!SuitTexturePaths(b, tex, pbc, pat, pnm, &err) || !LoadTex(pbc, bc, err) || !LoadTex(pat, at, err) ||
        !LoadTex(pnm, nm, err) || !BuildSuitMaterial(b, bc, at, nm, m, &err)) {
      CHECK_MSG(false, b.name + ": " + err);
      continue;
    }
    ++built;
    const float peak = std::max({m.emissiveLinear[0], m.emissiveLinear[1], m.emissiveLinear[2]});
    std::printf("%-30s C=(%g, %g, %g) ICNC=%g images=%zu bc=%dx%d at=%dx%d nm=%dx%d cullFront=%d\n", b.name.c_str(),
                m.emissiveLinear[0], m.emissiveLinear[1], m.emissiveLinear[2], peak, m.images.size(), m.images[0].image.width,
                m.images[0].image.height, m.images[1].image.width, m.images[1].image.height, m.images[2].image.width,
                m.images[2].image.height, int(m.cullFront));
    CHECK(m.cullFront);
    CHECK(m.images.size() == (peak > 0 ? 4u : 3u));
    for (const SuitImage& si : m.images) {
      CHECK(si.image.width > 0 && si.image.height > 0 &&
            si.image.rgba.size() == size_t(si.image.width) * size_t(si.image.height) * 4);
    }
    if (b.name == "samuspower_powerbody") {
      CHECK(std::fabs(peak - 50.f) < 1e-3f);
      CHECK(std::fabs(m.emissiveLinear[0] - 2.f) < 1e-3f && std::fabs(m.emissiveLinear[2] - 0.5f) < 1e-3f);
    } else if (b.name == "samusvaria_variabody") {
      CHECK(std::fabs(peak - 20.f) < 1e-3f);
    } else if (b.name == "samushyper_hyperbody") {
      CHECK(std::fabs(m.emissiveLinear[0] - 28.f) < 1e-3f);
    }
    const CD::RemMaterial r = CD::ReadMaterial(m.material, PortRemastered::ConvertOptions{});
    CHECK(r.maps[CD::kBase].has && r.maps[CD::kMr].has && r.maps[CD::kNormal].has);
    CHECK(r.maps[CD::kEmissive].has == (peak > 0));
    CHECK(std::fabs(r.emissive - double(peak)) < 1e-3 * std::max(1.0, double(peak)));
  }
  CHECK_MSG(built >= 10, "built " + std::to_string(built) + " suit materials");
  std::printf("real data: %d mp_samuspbr materials built\n", built);
}

} // namespace

int main() {
  TestChannels();
  TestEmissive();
  TestIdsAndErrors();
  TestReadMaterial();
  TestHyperBall();
  TestRealData();
  return PORT_TEST_RESULT();
}
