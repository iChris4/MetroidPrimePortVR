#include "port_disc_verify.h"
#include "port_test.h"

#include <algorithm>
#include <cstring>
#include <string>

using namespace PortDiscVerify;

namespace {
void Be32(std::vector<uint8_t>& v, size_t at, uint32_t x) {
  for (int i = 0; i < 4; ++i) v[at + i] = x >> (24 - i * 8);
}
}

int main() {
  // SHA-1 vectors, in one piece and in awkward pieces.
  {
    Sha1Stream s;
    s.Update("abc", 3);
    CHECK_EQ(s.FinalHex(), std::string("a9993e364706816aba3e25717850c26c9cd0d89d"));
    Sha1Stream empty;
    CHECK_EQ(empty.FinalHex(), std::string("da39a3ee5e6b4b0d3255bfef95601890afd80709"));
    const std::string text = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    Sha1Stream pieces;
    for (size_t at = 0, step = 1; at < text.size(); at += step, ++step) {
      pieces.Update(text.data() + at, std::min(step, text.size() - at));
    }
    CHECK_EQ(pieces.FinalHex(), std::string("84983e441c3bd26ebaae4aa1f95129e5e54670f1"));
    Sha1Stream million;
    const std::string block(1000, 'a');
    for (int i = 0; i < 1000; ++i) million.Update(block.data(), block.size());
    CHECK_EQ(million.FinalHex(), std::string("34aa973cd4c4daa4f61eeb2bdbad27316534016f"));
  }
  // CRC32 and the table.
  CHECK_EQ(Crc32("123456789", 9), 0xcbf43926u);
  const Revision* usa102 = FindRevision("GM8E01", 2);
  REQUIRE(usa102 != nullptr);
  CHECK_EQ(usa102->name, std::string("USA 1.02"));
  CHECK_EQ(usa102->size, uint64_t(1459978240));
  CHECK_EQ(usa102->crc.size(), size_t(1393));
  CHECK_EQ(usa102->sha1, std::string("1a737910b55b59c6ad91be9e3e3c43517fd52efb"));
  CHECK(FindRevision("GM8P01", 0) != nullptr);
  CHECK(FindRevision("GM8P01", 1) == nullptr);
  CHECK(FindRevision("GM8E02", 0) == nullptr);
  // Chunk compare on a synthetic revision: chunk 0 is right, chunk 1 zeroed, chunk 2 (partial) different.
  Revision rev{"TEST01", 0, "Test", kChunkSize * 2 + 100, "", {}};
  std::vector<uint8_t> c0(kChunkSize, 7), c1(kChunkSize, 9), c2(100, 5);
  rev.crc = {Crc32(c0.data(), c0.size()), Crc32(c1.data(), c1.size()), Crc32(c2.data(), c2.size())};
  std::vector<BadChunk> bad;
  CompareChunk(rev, 0, c0.data(), c0.size(), bad);
  CHECK(bad.empty());
  std::vector<uint8_t> zeros(kChunkSize, 0);
  CompareChunk(rev, 1, zeros.data(), zeros.size(), bad);
  c2[50] = 6;
  CompareChunk(rev, 2, c2.data(), c2.size(), bad);
  REQUIRE(bad.size() == 2);
  CHECK(bad[0].index == 1 && bad[0].zeroed && !bad[0].unreadable);
  CHECK(bad[1].index == 2 && !bad[1].zeroed);
  CompareChunk(rev, 2, c2.data(), 10, bad); // short read
  CHECK(bad.size() == 3 && bad[2].unreadable);
  // FST: root, dir "a" holding "x.pak" (offset 1.5 MiB, 1 MiB) and "y", then "z" at the root.
  std::vector<uint8_t> fst(5 * 12);
  fst[0] = 1; Be32(fst, 8, 5);
  fst[12] = 1; Be32(fst, 16, 0); Be32(fst, 20, 4);
  Be32(fst, 24, 2); Be32(fst, 28, 0x180000); Be32(fst, 32, 0x100000);
  Be32(fst, 36, 8); Be32(fst, 40, 0x400000); Be32(fst, 44, 10);
  Be32(fst, 48, 10); Be32(fst, 52, 0x500000); Be32(fst, 56, 10);
  const char names[] = "a\0x.pak\0y\0z";
  fst.insert(fst.end(), names, names + sizeof(names));
  const auto files = ParseFst(fst.data(), fst.size());
  REQUIRE(files.size() == 3);
  CHECK_EQ(files[0].path, std::string("a/x.pak"));
  CHECK_EQ(files[1].path, std::string("a/y"));
  CHECK_EQ(files[2].path, std::string("z"));
  const auto hit = FilesOverlapping(files, 1u << 20, 2u << 20);
  CHECK(hit.size() == 1 && hit[0] == "a/x.pak");
  CHECK(FilesOverlapping(files, 0, 0x180000).empty());
  CHECK(FilesOverlapping(files, 0x500000, 0x500001).size() == 1);
  CHECK(ParseFst(fst.data(), 5).empty());
  return PORT_TEST_RESULT();
}
