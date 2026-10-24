// The Remastered readers that take bytes straight from the game image: the
// LZSS decompressor, the pak reader and the texture decoder. Built from
// synthetic files, then every truncation and a run of corrupted copies of them,
// which must be refused or read, never crash or read outside the buffer.

#include "port_remastered_lzss.h"
#include "port_remastered_pak.h"
#include "port_remastered_txtr.h"
#include "port_test.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace PortRemastered;

namespace {

using Blob = std::vector<uint8_t>;

void Big32(Blob& out, uint32_t v) {
  for (int shift = 24; shift >= 0; shift -= 8) {
    out.push_back(uint8_t(v >> shift));
  }
}
void Little32(Blob& out, uint32_t v) {
  for (int shift = 0; shift < 32; shift += 8) {
    out.push_back(uint8_t(v >> shift));
  }
}
void Little64(Blob& out, uint64_t v) {
  for (int shift = 0; shift < 64; shift += 8) {
    out.push_back(uint8_t(v >> shift));
  }
}
void PutLittle64(Blob& out, size_t at, uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    out[at + size_t(i)] = uint8_t(v >> (8 * i));
  }
}
uint32_t Tag(const char* text) {
  return uint32_t(uint8_t(text[0])) << 24 | uint32_t(uint8_t(text[1])) << 16 | uint32_t(uint8_t(text[2])) << 8 |
         uint32_t(uint8_t(text[3]));
}

// A greedy LZSS encoder for `mode`, the inverse of LzssDecompress: a back
// reference wherever the last 4095 groups repeat for at least the shortest
// count, a literal group otherwise. `in` must be whole groups.
Blob LzssCompress(uint32_t mode, const Blob& in) {
  const size_t group = size_t(1) << (mode - 1);
  const size_t minCount = 4 - mode;
  const size_t maxCount = 15 + minCount;
  Blob out;
  size_t at = 0;
  while (at < in.size()) {
    const size_t headerAt = out.size();
    out.push_back(0);
    for (int bit = 0; bit < 8 && at < in.size(); ++bit) {
      size_t bestCount = 0, bestDistance = 0;
      for (size_t distance = 1; distance <= 4095 && distance * group <= at; ++distance) {
        size_t count = 0;
        while (count < maxCount && at + (count + 1) * group <= in.size() &&
               std::memcmp(&in[at + count * group], &in[at + count * group - distance * group], group) == 0) {
          ++count;
        }
        if (count > bestCount) {
          bestCount = count;
          bestDistance = distance;
        }
      }
      if (bestCount >= minCount) {
        out[headerAt] |= uint8_t(0x80 >> bit);
        out.push_back(uint8_t((bestCount - minCount) << 4 | bestDistance >> 8));
        out.push_back(uint8_t(bestDistance));
        at += bestCount * group;
      } else {
        out.insert(out.end(), in.begin() + long(at), in.begin() + long(at + group));
        at += group;
      }
    }
  }
  return out;
}

// Runs and noise, so both kinds of group come up.
Blob Sample(size_t size, uint32_t seed) {
  std::mt19937 rng(seed);
  Blob out;
  while (out.size() < size) {
    const size_t run = rng() % 40 + 1;
    const uint8_t byte = uint8_t(rng());
    const bool repeat = rng() % 2 == 0;
    for (size_t i = 0; i < run && out.size() < size; ++i) {
      out.push_back(repeat ? byte : uint8_t(rng()));
    }
  }
  return out;
}

void TestLzss() {
  for (uint32_t mode = 1; mode <= 3; ++mode) {
    const Blob plain = Sample(4096, mode);
    const Blob packed = LzssCompress(mode, plain);
    CHECK_MSG(packed.size() < plain.size(), "the sample compresses");
    Blob out(plain.size());
    CHECK_MSG(LzssDecompress(mode, packed.data(), packed.size(), out.data(), out.size()) && out == plain,
              "LZSS round trip");
    Blob shorter(plain.size() - 1);
    CHECK_MSG(!LzssDecompress(mode, packed.data(), packed.size(), shorter.data(), shorter.size()),
              "a size that does not match is refused");
    for (size_t cut = 0; cut < packed.size(); cut += 7) {
      LzssDecompress(mode, packed.data(), cut, out.data(), out.size());
    }
  }
  Blob out(16);
  // A reference before anything was written.
  const uint8_t early[] = {0x80, 0x00, 0x01};
  CHECK_MSG(!LzssDecompress(3, early, sizeof early, out.data(), out.size()), "reference before the start is refused");
  // One literal group of four, then a reference with distance 0.
  const uint8_t zero[] = {0x40, 1, 2, 3, 4, 0x00, 0x00};
  CHECK_MSG(!LzssDecompress(3, zero, sizeof zero, out.data(), 8), "distance 0 is refused");
  // One literal group, then a run of it: four groups at distance one.
  const uint8_t run[] = {0x40, 7, 8, 9, 10, 0x30, 0x01};
  Blob runOut(20);
  CHECK_MSG(LzssDecompress(3, run, sizeof run, runOut.data(), runOut.size()) && runOut[16] == 7 && runOut[19] == 10,
            "overlapping reference repeats a group");
  CHECK_MSG(!LzssDecompress(0, run, sizeof run, runOut.data(), runOut.size()), "mode 0 is not LZSS");
  CHECK_MSG(!LzssDecompress(4, run, sizeof run, runOut.data(), runOut.size()), "mode 4 is refused");
}

// A form header: magic, size, unknown, id, reader version, writer version.
void Form(Blob& out, uint64_t size, const char* id, uint32_t reader, uint32_t writer) {
  Big32(out, Tag("RFRM"));
  Little64(out, size);
  Little64(out, 0);
  Big32(out, Tag(id));
  Little32(out, reader);
  Little32(out, writer);
}

void Chunk(Blob& out, const char* id, const Blob& body) {
  Big32(out, Tag(id));
  Little64(out, body.size());
  Little32(out, 0);
  Little64(out, 0);
  out.insert(out.end(), body.begin(), body.end());
}

// An asset as the game's readers see it: an RFRM form of `type` with a body.
Blob Resource(const char* type, const Blob& body) {
  Blob out;
  Form(out, body.size(), type, 2, 3);
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

struct TestPak {
  Blob bytes;
  Blob stored;     // asset 0, as it is
  Blob compressed; // asset 1, LZSS mode 3
  size_t adirAt = 0;
};

// A pak with two assets: one stored, one compressed; metadata for the second,
// a name for each.
TestPak MakePak() {
  TestPak pak;
  pak.stored = Resource("CMDL", Sample(64, 10));
  Blob body = Sample(400, 11);
  body.resize(body.size() / 4 * 4);
  pak.compressed = Resource("TXTR", body);
  Blob packed;
  Little32(packed, 3);
  const Blob lz = LzssCompress(3, pak.compressed);
  packed.insert(packed.end(), lz.begin(), lz.end());

  const uint8_t ids[2][16] = {{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16},
                              {21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36}};
  const char* types[2] = {"CMDL", "TXTR"};
  const Blob* data[2] = {&pak.stored, &packed};
  const uint64_t sizes[2] = {pak.stored.size(), pak.compressed.size()};

  // The asset offsets depend on the table's size, which does not depend on them.
  Blob adir;
  Little32(adir, 2);
  for (int i = 0; i < 2; ++i) {
    Big32(adir, Tag(types[i]));
    adir.insert(adir.end(), ids[i], ids[i] + 16);
    Little32(adir, 2);
    Little32(adir, 3);
    Little64(adir, 0); // offset, filled in below
    Little64(adir, sizes[i]);
    Little64(adir, data[i]->size());
  }
  Blob meta;
  Little32(meta, 1);
  meta.insert(meta.end(), ids[1], ids[1] + 16);
  Little32(meta, 24); // the length word right after the table
  Little32(meta, 3);
  meta.insert(meta.end(), {'a', 'b', 'c'});
  Blob strg;
  Little32(strg, 2);
  for (int i = 0; i < 2; ++i) {
    Big32(strg, Tag(types[i]));
    strg.insert(strg.end(), ids[i], ids[i] + 16);
    Little32(strg, 5);
    strg.insert(strg.end(), {'n', 'a', 'm', 'e', uint8_t('0' + i)});
  }
  Blob tocc;
  Chunk(tocc, "ADIR", adir);
  Chunk(tocc, "META", meta);
  Chunk(tocc, "STRG", strg);
  const size_t header = 32 + 32 + tocc.size();
  const size_t adirOffsets = 32 + 32 + 24 + 4 + 28;
  uint64_t offset = header;
  Blob& out = pak.bytes;
  Form(out, 32 + tocc.size() + pak.stored.size() + packed.size(), "PACK", 1, 1);
  Form(out, tocc.size(), "TOCC", 3, 3);
  out.insert(out.end(), tocc.begin(), tocc.end());
  for (int i = 0; i < 2; ++i) {
    PutLittle64(out, adirOffsets + size_t(i) * 52, offset);
    out.insert(out.end(), data[i]->begin(), data[i]->end());
    offset += data[i]->size();
  }
  pak.adirAt = adirOffsets - 28;
  return pak;
}

ReadFn Reader(const Blob& bytes) {
  return [&bytes](uint64_t offset, void* out, size_t size) {
    if (offset > bytes.size() || size > bytes.size() - offset) {
      return false;
    }
    std::memcpy(out, bytes.data() + offset, size);
    return true;
  };
}

// Opens and reads everything; the result does not matter, only that it returns.
void Exercise(const Blob& bytes) {
  Pak pak;
  std::string error;
  if (!pak.Open(Reader(bytes), bytes.size(), error)) {
    return;
  }
  for (const PakAsset& asset : pak.Assets()) {
    Blob out;
    pak.ReadAsset(asset, out, error);
  }
}

void TestPakRead() {
  const TestPak test = MakePak();
  Pak pak;
  std::string error;
  CHECK_MSG(pak.Open(Reader(test.bytes), test.bytes.size(), error), "synthetic pak opens");
  CHECK_MSG(pak.Assets().size() == 2, "two assets");
  if (pak.Assets().size() == 2) {
    Blob out;
    CHECK_MSG(pak.ReadAsset(pak.Assets()[0], out, error) && out.size() > test.stored.size() &&
                  std::equal(test.stored.begin(), test.stored.end(), out.begin()),
              "stored asset reads back");
    CHECK_MSG(pak.ReadAsset(pak.Assets()[1], out, error) && out.size() > test.compressed.size() &&
                  std::equal(test.compressed.begin(), test.compressed.end(), out.begin()),
              "compressed asset reads back");
    CHECK_MSG(pak.Assets()[1].hasMeta && pak.Assets()[0].names.size() == 1 && pak.Assets()[0].names[0] == "name0",
              "metadata and names");
  }

  // A decompressed size no data could expand to is refused, not allocated.
  Blob huge = test.bytes;
  PutLittle64(huge, test.adirAt + 52 + 36, uint64_t(1) << 31);
  Pak hugePak;
  CHECK_MSG(hugePak.Open(Reader(huge), huge.size(), error), "huge-size pak still opens");
  if (hugePak.Assets().size() == 2) {
    Blob out;
    CHECK_MSG(!hugePak.ReadAsset(hugePak.Assets()[1], out, error), "impossible decompressed size is refused");
  }

  // A metadata entry pointing outside its table.
  Blob badMeta = test.bytes;
  const size_t metaEntryOffset = 32 + 32 + 24 + 4 + 2 * 52 + 24 + 4 + 16;
  badMeta[metaEntryOffset] = 0xF0;
  Pak badMetaPak;
  CHECK_MSG(!badMetaPak.Open(Reader(badMeta), badMeta.size(), error), "metadata outside its table is refused");

  for (size_t cut = 0; cut < test.bytes.size(); ++cut) {
    Exercise(Blob(test.bytes.begin(), test.bytes.begin() + long(cut)));
  }
  std::mt19937 rng(1);
  for (int round = 0; round < 3000; ++round) {
    Blob broken = test.bytes;
    const int flips = int(rng() % 4) + 1;
    for (int i = 0; i < flips; ++i) {
      broken[rng() % broken.size()] = uint8_t(rng());
    }
    Exercise(broken);
  }
}

void TestTxtrGarbage() {
  // No synthetic texture yet; the decoder must refuse what is not one.
  std::mt19937 rng(2);
  TxtrImage image;
  std::string error;
  const Blob form = Resource("TXTR", Sample(512, 12));
  for (size_t cut = 0; cut <= form.size(); cut += 3) {
    DecodeTxtr(form.data(), cut, image, error);
    ReadTxtrInfo(form.data(), cut, image, error);
  }
  for (int round = 0; round < 2000; ++round) {
    Blob broken = form;
    for (int i = 0; i < 8; ++i) {
      broken[32 + rng() % (broken.size() - 32)] = uint8_t(rng());
    }
    DecodeTxtr(broken.data(), broken.size(), image, error);
  }
  CHECK_MSG(!DecodeTxtr(form.data(), form.size(), image, error), "noise is not a texture");
}

// A 16x16 RGBA8 texture whose 64 KB surface comes in two stored buffers: the
// first holds the top mip (and more), the second the rest. `second` is that
// buffer's bytes as written, mode word included, so it can be made undecodable.
// A 16x16 texture of `format` (RGBA8 by default) with the given mip sizes, whose surface is
// two stored buffers.
Blob Texture(const Blob& first, const Blob& second, uint32_t secondSize, uint32_t format = 12,
             std::vector<uint32_t> mipSizes = {1024u}) {
  Blob head;
  for (uint32_t v : {0u, format, 16u, 16u, 1u, 0u, 0u, uint32_t(mipSizes.size())}) {
    Little32(head, v); // kind, format, width, height, layers, tile, swizzle, mips
  }
  for (uint32_t v : mipSizes) {
    Little32(head, v);
  }
  Blob body;
  Chunk(body, "HEAD", head);
  Blob out;
  Form(out, body.size(), "TXTR", 47, 51);
  out.insert(out.end(), body.begin(), body.end());

  const uint32_t firstSize = uint32_t(first.size()) - 4;
  Blob meta;
  for (uint32_t v : {0u, 0u, 0u, 0u, 0u, firstSize + secondSize, 1u}) {
    Little32(meta, v); // ..., decompressed size, read count
  }
  // One read covering both buffers; its offset is filled in once the footer's size is known.
  const size_t readAt = meta.size();
  meta.push_back(0);
  Little32(meta, 0);
  Little32(meta, uint32_t(first.size() + second.size()));
  Little32(meta, 2);
  for (const auto& [offset, size, dest, destSize] :
       {std::array<uint32_t, 4>{0, uint32_t(first.size()), 0, firstSize},
        std::array<uint32_t, 4>{uint32_t(first.size()), uint32_t(second.size()), firstSize, secondSize}}) {
    for (uint32_t v : {0u, offset, size, dest, destSize}) {
      Little32(meta, v);
    }
  }
  Blob footBody;
  Chunk(footBody, "META", meta);
  Blob foot;
  Form(foot, footBody.size(), "FOOT", 1, 1);
  const size_t footStart = out.size();
  foot.insert(foot.end(), footBody.begin(), footBody.end());
  out.insert(out.end(), foot.begin(), foot.end());
  const uint32_t texels = uint32_t(out.size());
  const size_t metaInFile = footStart + 32 + 24;
  for (int i = 0; i < 4; ++i) {
    out[metaInFile + readAt + 1 + size_t(i)] = uint8_t(texels >> (8 * i));
  }
  out.insert(out.end(), first.begin(), first.end());
  out.insert(out.end(), second.begin(), second.end());
  return out;
}

Blob Stored(const Blob& bytes) {
  Blob out;
  Little32(out, 0);
  out.insert(out.end(), bytes.begin(), bytes.end());
  return out;
}

void TestTxtrTopMipOnly() {
  const Blob first = Stored(Sample(32768, 21));
  const uint32_t secondSize = 32768;
  const Blob good = Texture(first, Stored(Sample(secondSize, 22)), secondSize);
  // An LZSS buffer that cannot decompress to its size.
  Blob broken{1, 0, 0, 0, 0x80, 0xFF, 0xFF};
  const Blob noSecond = Texture(first, broken, secondSize);
  TxtrImage a, b;
  std::string error;
  CHECK_MSG(DecodeTxtr(good.data(), good.size(), a, error), "synthetic texture decodes");
  CHECK_MSG(a.width == 16 && a.height == 16 && a.rgba.size() == 16 * 16 * 4, "its size");
  // Only the top mip is decoded, and only its buffer is decompressed: a broken buffer of the
  // smaller mips does not matter, and the image is the same.
  CHECK_MSG(DecodeTxtr(noSecond.data(), noSecond.size(), b, error) && b.rgba == a.rgba,
            "a buffer past the top mip is not decompressed");
  const Blob noFirst = Texture(broken, Stored(Sample(secondSize, 22)), 32768);
  CHECK_MSG(!DecodeTxtr(noFirst.data(), noFirst.size(), b, error), "a broken top-mip buffer still fails");
}
// A BC6H mode 11 block (one region, 10-bit endpoints) whose two endpoints are both x in every
// channel, so each texel decodes to the same half.
std::array<uint8_t, 16> FlatBc6h(uint32_t x) {
  std::array<uint8_t, 16> block{};
  const auto put = [&](uint32_t at, uint32_t bits, uint32_t value) {
    for (uint32_t i = 0; i < bits; ++i) {
      if ((value >> i) & 1) {
        block[(at + i) / 8] |= uint8_t(1u << ((at + i) % 8));
      }
    }
  };
  put(0, 5, 0b00011);
  for (uint32_t e = 0; e < 6; ++e) {
    put(5 + e * 10, 10, x);
  }
  return block;
}

// BC6H's 8-bit form is its value clamped to [0, 1], not the half's high byte; and a plain 2D
// BC6H texture's mips come out as the blocks they are, for writing on as BC6H.
void TestBc6h() {
  const auto one = FlatBc6h(495), mid = FlatBc6h(462), bright = FlatBc6h(700);
  uint16_t rgba[64];
  DecodeBc6hImage(one.data(), 4, 4, false, rgba);
  CHECK_MSG(rgba[0] == 0x3C00 && rgba[5] == 0x3C00 && rgba[63] == 0x3C00, "the synthetic block is 1.0");
  DecodeBc6hImage(mid.data(), 4, 4, false, rgba);
  CHECK_MSG(rgba[0] >= 0x37FF && rgba[0] <= 0x3802, "the second synthetic block is 0.5");
  uint8_t px[64];
  DecodeBc6hBlockRgba8(one.data(), false, px);
  CHECK_MSG(px[0] == 255 && px[1] == 255 && px[2] == 255 && px[3] == 255 && px[60] == 255, "1.0 is 255");
  DecodeBc6hBlockRgba8(mid.data(), false, px);
  CHECK_MSG(px[0] == 128 && px[2] == 128 && px[3] == 255, "0.5 is 128 (the high byte gave 56)");
  DecodeBc6hBlockRgba8(bright.data(), false, px);
  CHECK_MSG(px[0] == 255, "over 1 clamps to 255");
  CHECK_MSG(HalfToUnormByte(0x3800) == 128 && HalfToUnormByte(0x3400) == 64 && HalfToUnormByte(0x0000) == 0 &&
                HalfToUnormByte(0xBC00) == 0 && HalfToUnormByte(0x7C00) == 255 && HalfToUnormByte(0x7E00) == 255,
            "half to byte: by value, negatives to 0, clamped at 1 (inf and NaN read as the largest half)");

  // 16x16, two mips: each mip's surface is one GOB (512 bytes) of the same block, so the
  // tiling cannot move anything.
  Blob mip0, mip1;
  for (int i = 0; i < 32; ++i) {
    mip0.insert(mip0.end(), one.begin(), one.end());
    mip1.insert(mip1.end(), mid.begin(), mid.end());
  }
  const Blob tex = Texture(Stored(mip0), Stored(mip1), 512, kTxtrFormatBc6hUfloat, {512u, 512u});
  TxtrImage image;
  std::string error;
  CHECK_MSG(DecodeTxtr(tex.data(), tex.size(), image, error), "a BC6H texture decodes");
  CHECK_MSG(image.width == 16 && image.rgba.size() == 16 * 16 * 4 && image.rgba[0] == 255 && !image.srgb,
            "its 8-bit form is 1.0 as 255, linear");
  TxtrMipsBc6h mips;
  CHECK_MSG(ReadTxtrMipsBc6h(tex.data(), tex.size(), mips, error), "its mips read");
  CHECK_MSG(mips.width == 16 && mips.height == 16 && !mips.isSigned && mips.mips.size() == 2, "both mips");
  if (mips.mips.size() == 2) {
    CHECK_MSG(mips.mips[0].size() == 16 * 16 && mips.mips[1].size() == 4 * 16, "the mips' block counts");
    CHECK_MSG(std::equal(one.begin(), one.end(), mips.mips[0].begin()) &&
                  std::equal(mid.begin(), mid.end(), mips.mips[1].begin() + 48),
              "each mip's blocks pass through as stored");
  }
  const Blob rgba8 = Texture(Stored(Sample(1024, 3)), Stored(Sample(16, 4)), 16);
  CHECK_MSG(!ReadTxtrMipsBc6h(rgba8.data(), rgba8.size(), mips, error), "an RGBA8 texture is not BC6H");
}
} // namespace

int main() {
  TestLzss();
  TestPakRead();
  TestTxtrGarbage();
  TestTxtrTopMipOnly();
  TestBc6h();
  if (port_test::Failures() == 0) {
    std::printf("port_remastered_parsers: ok\n");
  }
  return PORT_TEST_RESULT();
}
