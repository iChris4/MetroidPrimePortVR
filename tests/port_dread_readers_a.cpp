// Metroid Dread readers, part A: the CRC-64 path hash, the .pkg table, the .bctex texture
// (MTXT/XTX, block-linear, BC1/BC3/BC5) and the title id of an opened image.
//
// Synthetic fixtures are built here (nothing from the game is committed). Three opt-in checks
// use the user's own data and say so when they skip:
//   PORT_DREAD_ROMFS=<dir>        an extracted RomFS (build/dread/romfs): samus.pkg and Samus textures;
//                                 PORT_DREAD_REF=<dir> (default /tmp/opencode/dread-ref) holds
//                                 <name>.rgba references made by build/s-dread-20261024/bctex_dump.py
//   PORT_DREAD_NSP + PORT_SWITCH_KEYS    the Dread .nsp and the user's key file (never printed)

#include "port_dread_bctex.h"
#include "port_dread_pkg.h"
#include "port_remastered_nsp.h"
#include "port_test.h"

#include <zlib.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using namespace PortDread;

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

const char* Env(const char* name) {
  const char* v = std::getenv(name);
  return v && *v ? v : nullptr;
}

// --- CRC-64 -----------------------------------------------------------------

void TestCrc() {
  // Computed with the python reference (build/dread/refs/fmt_mmdl.py hashFunction).
  constexpr uint64_t kExpected = 0x35e392aa8caf7331ull;
  CHECK_EQ(PathCrc64("actors/characters/samus/models/samuspower.bcmdl"), kExpected);
  // The path is lower-cased first.
  CHECK_EQ(PathCrc64("Actors/Characters/SAMUS/models/SamusPower.BCMDL"), kExpected);
  CHECK_EQ(PathCrc64(""), ~uint64_t(0));
  CHECK(PathCrc64("a") != PathCrc64("b"));
}

// --- PKG --------------------------------------------------------------------

struct PkgFixture {
  Blob file;
  uint64_t ids[3];
  uint32_t starts[3];
  uint32_t ends[3];
};

PkgFixture MakePkg() {
  PkgFixture f;
  const char* names[3] = {"actors/a.bcmdl", "actors/b.bsmat", "scripts/c.lc"};
  const char* magics[3] = {"MMDL", "MSUR", "\x1b" "Lua"};
  const size_t sizes[3] = {24, 8, 40};
  const size_t tableEnd = 12 + 3 * 16;
  const size_t dataStart = (tableEnd + 127) / 128 * 128;
  Blob data;
  for (int i = 0; i < 3; ++i) {
    f.ids[i] = PathCrc64(names[i]);
    f.starts[i] = uint32_t(dataStart + data.size());
    Blob body(sizes[i], uint8_t(0x40 + i));
    std::memcpy(body.data(), magics[i], 4);
    data.insert(data.end(), body.begin(), body.end());
    f.ends[i] = uint32_t(dataStart + data.size());
    while (data.size() % 8) {
      data.push_back(0);
    }
  }
  Put32(f.file, uint32_t(dataStart - 4));
  Put32(f.file, uint32_t(data.size()));
  Put32(f.file, 3);
  for (int i = 0; i < 3; ++i) {
    Put64(f.file, f.ids[i]);
    Put32(f.file, f.starts[i]);
    Put32(f.file, f.ends[i]);
  }
  f.file.resize(dataStart, 0);
  f.file.insert(f.file.end(), data.begin(), data.end());
  return f;
}

void TestPkg() {
  const PkgFixture f = MakePkg();
  Pkg pkg;
  std::string error;
  REQUIRE_MSG(pkg.Parse(f.file.data(), f.file.size(), error), error);
  CHECK_EQ(pkg.Count(), size_t(3));
  const PkgEntry* b = pkg.Find("actors/B.bsmat");
  REQUIRE(b != nullptr);
  CHECK_EQ(b->assetId, f.ids[1]);
  CHECK_EQ(b->start, f.starts[1]);
  CHECK_EQ(b->Size(), 8u);
  CHECK(std::memcmp(f.file.data() + b->start, "MSUR", 4) == 0);
  CHECK(pkg.Find(f.ids[2]) != nullptr);
  CHECK(pkg.Find("missing/x.bin") == nullptr);
  CHECK(pkg.Find(uint64_t(0)) == nullptr);

  // Header only, with the real file size given.
  size_t headerBytes = 0;
  CHECK(PkgHeaderBytes(f.file.data(), 12, headerBytes));
  CHECK_EQ(headerBytes, size_t(12 + 3 * 16));
  Pkg partial;
  CHECK_MSG(partial.Parse(f.file.data(), headerBytes, error, f.file.size()), error);
  CHECK(partial.Find("scripts/c.lc") != nullptr);
  // Without the file size, the entries point past the bytes given.
  CHECK(!Pkg().Parse(f.file.data(), headerBytes, error));

  // Refusals.
  Pkg bad;
  CHECK(!bad.Parse(f.file.data(), 11, error));
  CHECK(!bad.Parse(f.file.data(), headerBytes - 1, error));
  CHECK(!bad.Parse(nullptr, 0, error));
  {
    Blob huge = f.file; // count beyond the cap
    huge[8] = huge[9] = huge[10] = huge[11] = 0xFF;
    CHECK(!bad.Parse(huge.data(), huge.size(), error));
    CHECK(bad.Count() == 0);
  }
  {
    Blob pastEnd = f.file; // entry 0's end beyond the file
    pastEnd[12 + 12] = 0xFF;
    pastEnd[12 + 13] = 0xFF;
    CHECK(!bad.Parse(pastEnd.data(), pastEnd.size(), error));
  }
  {
    Blob inTable = f.file; // entry 1 starting inside the table
    inTable[12 + 16 + 8] = 4;
    inTable[12 + 16 + 9] = 0;
    inTable[12 + 16 + 10] = 0;
    inTable[12 + 16 + 11] = 0;
    CHECK(!bad.Parse(inTable.data(), inTable.size(), error));
  }
  {
    Blob inverted = f.file; // end < start
    std::swap(inverted[12 + 8], inverted[12 + 12]);
    CHECK(!bad.Parse(inverted.data(), inverted.size(), error));
  }
  // Every truncation and a run of corrupted copies: refused or read, never a crash.
  std::mt19937 rng(7);
  for (size_t n = 0; n <= f.file.size(); ++n) {
    Pkg p;
    p.Parse(f.file.data(), n, error);
  }
  for (int i = 0; i < 500; ++i) {
    Blob c = f.file;
    for (int k = 0; k < 4; ++k) {
      c[rng() % 12 + (rng() % 2 ? 0 : (rng() % (3 * 16)))] = uint8_t(rng());
    }
    Pkg p;
    if (p.Parse(c.data(), c.size(), error)) {
      for (const PkgEntry& e : p.Entries()) {
        CHECK(e.start <= e.end && e.end <= c.size());
        CHECK(p.Find(e.assetId) != nullptr);
      }
    }
  }
}

// --- BCTEX ------------------------------------------------------------------

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

Blob RandomBlob(size_t n, uint32_t seed) {
  std::mt19937 rng(seed);
  Blob b(n);
  for (auto& v : b) {
    v = uint8_t(rng());
  }
  return b;
}

void TestBctex() {
  std::string error;
  // BC1 16x16 with two mips, block height 1 (4x4 blocks).
  {
    TexSpec s;
    s.mips = 2;
    std::vector<Blob> lin = {RandomBlob(16 * 8, 1), RandomBlob(4 * 8, 2)};
    const Blob file = MakeBctex(s, lin);
    Bctex tex;
    REQUIRE_MSG(tex.Parse(file.data(), file.size(), error), error);
    CHECK_EQ(tex.Info().width, 16u);
    CHECK_EQ(tex.Info().height, 16u);
    CHECK_EQ(tex.Info().mipCount, 2u);
    CHECK_EQ(tex.Info().format, uint32_t(kBctexBc1));
    CHECK_EQ(tex.Info().bytesPerBlock, 8u);
    CHECK_EQ(tex.Info().blockHeightGobs, 1u);
    for (uint32_t k = 0; k < 2; ++k) {
      Blob out;
      CHECK_MSG(tex.Deswizzle(k, out, error), error);
      CHECK(out == lin[k]);
    }
    Blob dummy;
    CHECK(!tex.Deswizzle(2, dummy, error));
  }
  // BC3 64x64, three mips, a block height of 2 GOBs that shrinks on the small mips.
  {
    TexSpec s;
    s.w = s.h = 64;
    s.mips = 3;
    s.format = kBctexBc3;
    s.bpp = 16;
    s.layout1 = 1;
    std::vector<Blob> lin = {RandomBlob(16 * 16 * 16, 3), RandomBlob(8 * 8 * 16, 4), RandomBlob(4 * 4 * 16, 5)};
    const Blob file = MakeBctex(s, lin);
    Bctex tex;
    REQUIRE_MSG(tex.Parse(file.data(), file.size(), error), error);
    CHECK_EQ(tex.Info().blockHeightGobs, 2u);
    for (uint32_t k = 0; k < 3; ++k) {
      Blob out;
      CHECK_MSG(tex.Deswizzle(k, out, error), error);
      CHECK(out == lin[k]);
    }
  }
  // BC1 decode: block (0,0) solid red, block (1,0) solid blue with a 1x1... of 8x4 texels.
  {
    TexSpec s;
    s.w = 8;
    s.h = 4;
    Blob lin = {0x00, 0xF8, 0x00, 0x00, 0, 0, 0, 0,   // c0 = 0xF800 red, c1 = 0, all indices 0
                0x1F, 0x00, 0x00, 0x00, 0, 0, 0, 0};  // c0 = 0x001F blue
    const Blob file = MakeBctex(s, {lin});
    Bctex tex;
    REQUIRE_MSG(tex.Parse(file.data(), file.size(), error), error);
    Blob rgba;
    REQUIRE_MSG(tex.DecodeRgba8(0, rgba, error), error);
    REQUIRE(rgba.size() == 8 * 4 * 4);
    const uint8_t red[4] = {255, 0, 0, 255}, blue[4] = {0, 0, 255, 255};
    for (size_t y = 0; y < 4; ++y) {
      for (size_t x = 0; x < 8; ++x) {
        CHECK(std::memcmp(&rgba[(y * 8 + x) * 4], x < 4 ? red : blue, 4) == 0);
      }
    }
  }
  // BC3 decode (alpha endpoints 128/128) and BC5 decode (R 200, G 100), 6x5 texels so the edge
  // blocks are clipped.
  {
    TexSpec s;
    s.w = 6;
    s.h = 5;
    s.format = kBctexBc3;
    s.bpp = 16;
    Blob block = {128, 128, 0, 0, 0, 0, 0, 0, 0x1F, 0x00, 0x00, 0x00, 0, 0, 0, 0};
    Blob lin;
    for (int i = 0; i < 4; ++i) {
      lin.insert(lin.end(), block.begin(), block.end());
    }
    Bctex tex;
    Blob file = MakeBctex(s, {lin});
    REQUIRE_MSG(tex.Parse(file.data(), file.size(), error), error);
    Blob rgba;
    REQUIRE_MSG(tex.DecodeRgba8(0, rgba, error), error);
    REQUIRE(rgba.size() == 6 * 5 * 4);
    const uint8_t px[4] = {0, 0, 255, 128};
    for (size_t i = 0; i < 30; ++i) {
      CHECK(std::memcmp(&rgba[i * 4], px, 4) == 0);
    }

    s.format = kBctexBc5;
    Blob b5 = {200, 200, 0, 0, 0, 0, 0, 0, 100, 100, 0, 0, 0, 0, 0, 0};
    lin.clear();
    for (int i = 0; i < 4; ++i) {
      lin.insert(lin.end(), b5.begin(), b5.end());
    }
    file = MakeBctex(s, {lin});
    REQUIRE_MSG(tex.Parse(file.data(), file.size(), error), error);
    REQUIRE_MSG(tex.DecodeRgba8(0, rgba, error), error);
    const uint8_t px5[4] = {200, 100, 0, 255};
    for (size_t i = 0; i < 30; ++i) {
      CHECK(std::memcmp(&rgba[i * 4], px5, 4) == 0);
    }
  }
  // An uncompressed RGBA8 4x2: stored as is (BGRA8 swaps).
  {
    TexSpec s;
    s.w = 4;
    s.h = 2;
    s.format = kBctexRgba8;
    s.bpp = 4;
    s.compressed = false;
    const Blob lin = RandomBlob(4 * 2 * 4, 9);
    Bctex tex;
    Blob file = MakeBctex(s, {lin});
    REQUIRE_MSG(tex.Parse(file.data(), file.size(), error), error);
    Blob rgba;
    REQUIRE_MSG(tex.DecodeRgba8(0, rgba, error), error);
    CHECK(rgba == lin);
    s.format = kBctexBgra8;
    file = MakeBctex(s, {lin});
    REQUIRE_MSG(tex.Parse(file.data(), file.size(), error), error);
    REQUIRE_MSG(tex.DecodeRgba8(0, rgba, error), error);
    CHECK(rgba[0] == lin[2] && rgba[2] == lin[0] && rgba[1] == lin[1] && rgba[3] == lin[3]);
  }
  // Unknown formats are refused with a message; BC6H parses but does not decode.
  {
    TexSpec s;
    s.format = 0x99;
    const Blob file = MakeBctex(s, {RandomBlob(128, 1)});
    Bctex tex;
    CHECK(!tex.Parse(file.data(), file.size(), error));
    CHECK_MSG(error.find("unsupported format 0x99") != std::string::npos, error);
    CHECK(!tex.Valid());
    s.format = kBctexBc6h;
    s.bpp = 16;
    const Blob file6 = MakeBctex(s, {RandomBlob(16 * 16, 1)});
    CHECK(tex.Parse(file6.data(), file6.size(), error));
    Blob rgba;
    CHECK(!tex.DecodeRgba8(0, rgba, error));
    CHECK_MSG(error.find("cannot decode format 0x50") != std::string::npos, error);
  }
  // Truncations and corruption: refused or read, never a crash.
  {
    TexSpec s;
    s.w = s.h = 32;
    s.mips = 2;
    s.layout1 = 1;
    const Blob file = MakeBctex(s, {RandomBlob(8 * 8 * 8, 1), RandomBlob(4 * 8, 2)});
    for (size_t n = 0; n <= file.size(); n += 3) {
      Bctex t;
      t.Parse(file.data(), n, error);
    }
    // Corrupt the inflated bytes (the gzip would otherwise just fail): rebuild with a mangled
    // outer/XTX header by editing the spec instead.
    std::mt19937 rng(11);
    for (int i = 0; i < 300; ++i) {
      Blob c = file;
      c[rng() % c.size()] ^= uint8_t(1u << (rng() % 8));
      Bctex t;
      std::string e;
      if (t.Parse(c.data(), c.size(), e)) {
        Blob out;
        for (uint32_t k = 0; k < t.Info().mipCount; ++k) {
          t.Deswizzle(k, out, e);
          t.DecodeRgba8(k, out, e);
        }
      }
    }
  }
}

// --- Real data (opt-in) -------------------------------------------------------

bool ReadFileRange(const std::filesystem::path& path, uint64_t offset, size_t size, Blob& out) {
  std::ifstream in(path, std::ios::binary);
  out.resize(size);
  in.seekg(std::streamoff(offset));
  in.read(reinterpret_cast<char*>(out.data()), std::streamsize(size));
  return bool(in) && size_t(in.gcount()) == size;
}

Blob ReadWhole(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return Blob(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void TestRealRomfs() {
  const char* root = Env("PORT_DREAD_ROMFS");
  if (!root) {
    std::printf("SKIPPED: real RomFS checks (set PORT_DREAD_ROMFS=build/dread/romfs)\n");
    return;
  }
  namespace fs = std::filesystem;
  const fs::path pkgPath = fs::path(root) / "packs/players/samus.pkg";
  if (!fs::exists(pkgPath)) {
    std::printf("SKIPPED: real RomFS checks (%s has no packs/players/samus.pkg)\n", root);
    return;
  }
  std::string error;
  const uint64_t fileSize = fs::file_size(pkgPath);
  Blob head;
  REQUIRE(ReadFileRange(pkgPath, 0, 12, head));
  size_t headerBytes = 0;
  REQUIRE(PkgHeaderBytes(head.data(), head.size(), headerBytes));
  REQUIRE(ReadFileRange(pkgPath, 0, headerBytes, head));
  Pkg pkg;
  REQUIRE_MSG(pkg.Parse(head.data(), head.size(), error, fileSize), error);
  CHECK_EQ(pkg.Count(), size_t(6513));
  const PkgEntry* power = pkg.Find("actors/characters/samus/models/samuspower.bcmdl");
  REQUIRE(power != nullptr);
  Blob magic;
  REQUIRE(ReadFileRange(pkgPath, power->start, 4, magic));
  CHECK(std::memcmp(magic.data(), "MMDL", 4) == 0);
  std::printf("real samus.pkg: %zu entries, samuspower.bcmdl at %u..%u\n", pkg.Count(), power->start, power->end);

  const fs::path texDir = fs::path(root) / "textures/actors/characters/samus/models/textures";
  if (!fs::exists(texDir)) {
    std::printf("SKIPPED: real texture checks (no %s)\n", texDir.string().c_str());
    return;
  }
  // Every Samus texture parses and untiles all its mips; and block height = layout1 matches the
  // tegra_swizzle rule for the top mip's height.
  size_t textures = 0, decoded = 0;
  for (const auto& e : fs::directory_iterator(texDir)) {
    if (e.path().extension() != ".bctex") {
      continue;
    }
    const Blob file = ReadWhole(e.path());
    Bctex tex;
    if (!tex.Parse(file.data(), file.size(), error)) {
      // A format outside the supported list is acceptable only if reported as such.
      CHECK_MSG(error.find("unsupported format") != std::string::npos, e.path().filename().string() + ": " + error);
      continue;
    }
    ++textures;
    Blob out;
    for (uint32_t k = 0; k < tex.Info().mipCount; ++k) {
      CHECK_MSG(tex.Deswizzle(k, out, error), e.path().filename().string() + ": " + error);
    }
    if (tex.DecodeRgba8(0, out, error)) {
      ++decoded;
    } else {
      CHECK_MSG(error.find("cannot decode") != std::string::npos, error);
    }
  }
  std::printf("real textures: %zu parsed, %zu decoded to RGBA8\n", textures, decoded);
  CHECK(textures >= 30);

  // Pixel comparison against the python reference decode (Pillow's DDS decoder, so the BC
  // endpoint rounding may differ by a step).
  const char* refEnv = Env("PORT_DREAD_REF");
  const fs::path refDir = refEnv ? fs::path(refEnv) : fs::path("/tmp/opencode/dread-ref");
  struct Case { const char* name; uint32_t format; uint32_t channels; };
  const Case cases[] = {{"powerbody_bc", kBctexBc3, 4}, {"samusgravityreward_d", kBctexBc1, 4}, {"powerbody_nm", kBctexBc5, 2}};
  for (const Case& c : cases) {
    const fs::path tp = texDir / (std::string(c.name) + ".bctex");
    if (!fs::exists(tp)) {
      std::printf("SKIPPED: %s (not in the RomFS)\n", c.name);
      continue;
    }
    const Blob file = ReadWhole(tp);
    Bctex tex;
    REQUIRE_MSG(tex.Parse(file.data(), file.size(), error), error);
    CHECK_EQ(tex.Info().format, c.format);
    CHECK_EQ(tex.Info().width, 1024u);
    CHECK_EQ(tex.Info().height, 1024u);
    CHECK_EQ(tex.Info().blockHeightGobs, 16u);
    Blob rgba;
    REQUIRE_MSG(tex.DecodeRgba8(0, rgba, error), error);
    CHECK_EQ(rgba.size(), size_t(1024 * 1024 * 4));
    const fs::path rp = refDir / (std::string(c.name) + ".rgba");
    if (!fs::exists(rp)) {
      std::printf("SKIPPED: pixel comparison of %s (no reference %s)\n", c.name, rp.string().c_str());
      continue;
    }
    const Blob ref = ReadWhole(rp);
    REQUIRE(ref.size() == rgba.size());
    int maxDiff = 0;
    size_t differing = 0;
    for (size_t i = 0; i < rgba.size() / 4; ++i) {
      bool any = false;
      for (uint32_t ch = 0; ch < c.channels; ++ch) {
        const int d = std::abs(int(rgba[i * 4 + ch]) - int(ref[i * 4 + ch]));
        maxDiff = std::max(maxDiff, d);
        any = any || d != 0;
      }
      differing += any;
    }
    std::printf("%s vs reference: max channel diff %d, %zu of %zu pixels differ\n", c.name, maxDiff, differing,
                rgba.size() / 4);
    CHECK(maxDiff <= 2);
  }
}

void TestRealNsp() {
  const char* nsp = Env("PORT_DREAD_NSP");
  const char* keys = Env("PORT_SWITCH_KEYS");
  if (!nsp || !keys) {
    std::printf("SKIPPED: real NSP check (set PORT_DREAD_NSP and PORT_SWITCH_KEYS)\n");
    return;
  }
  PortRemastered::Nsp image;
  std::string error;
  REQUIRE_MSG(image.Open(nsp, keys, error), error);
  CHECK_EQ(image.ProgramId(), PortRemastered::kDreadProgramId);
  const PortRemastered::RomfsFile* file = image.Find("packs/players/samus.pkg");
  REQUIRE(file != nullptr);
  Blob head(12);
  REQUIRE_MSG(image.Read(*file, 0, head.data(), head.size(), error), error);
  size_t headerBytes = 0;
  REQUIRE(PkgHeaderBytes(head.data(), head.size(), headerBytes));
  head.resize(headerBytes);
  REQUIRE_MSG(image.Read(*file, 0, head.data(), head.size(), error), error);
  Pkg pkg;
  REQUIRE_MSG(pkg.Parse(head.data(), head.size(), error, file->size), error);
  CHECK_EQ(pkg.Count(), size_t(6513));
  const PkgEntry* power = pkg.Find("actors/characters/samus/models/samuspower.bcmdl");
  REQUIRE(power != nullptr);
  Blob magic(4);
  REQUIRE_MSG(image.Read(*file, power->start, magic.data(), 4, error), error);
  CHECK(std::memcmp(magic.data(), "MMDL", 4) == 0);
  std::printf("real NSP: program id 0x%016llx, samus.pkg %llu bytes, %zu entries\n",
              static_cast<unsigned long long>(image.ProgramId()), static_cast<unsigned long long>(file->size),
              pkg.Count());
  image.Close();
  CHECK_EQ(image.ProgramId(), uint64_t(0));
}

} // namespace

int main() {
  TestCrc();
  TestPkg();
  TestBctex();
  TestRealRomfs();
  TestRealNsp();
  if (port_test::Failures() == 0) {
    std::printf("port_dread_readers_a: ok\n");
  }
  return PORT_TEST_RESULT();
}
