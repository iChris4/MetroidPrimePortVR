// LZ4 block decoding, PFS0 parsing and NSO segment reads on synthetic data.

#include "port_remastered_nso.h"
#include "port_test.h"

#include <cstdio>
#include <cstring>

using namespace PortRemastered;

static void Put32(std::vector<uint8_t>& v, uint32_t x) {
  for (int i = 0; i < 4; ++i) {
    v.push_back(uint8_t(x >> (8 * i)));
  }
}
static void Put64(std::vector<uint8_t>& v, uint64_t x) {
  Put32(v, uint32_t(x));
  Put32(v, uint32_t(x >> 32));
}
static void Set32(std::vector<uint8_t>& v, size_t at, uint32_t x) {
  for (int i = 0; i < 4; ++i) {
    v[at + i] = uint8_t(x >> (8 * i));
  }
}

// "abcd" then a 12-byte overlapping match at offset 4, then literals "xyz".
static std::vector<uint8_t> SampleLz4() {
  std::vector<uint8_t> s = {0x48, 'a', 'b', 'c', 'd', 4, 0, 0x30, 'x', 'y', 'z'};
  return s;
}
static const char kSampleOut[] = "abcdabcdabcdabcdxyz";

static void TestLz4() {
  std::vector<uint8_t> out;
  std::string err;
  const auto src = SampleLz4();
  CHECK(Lz4Decode(src.data(), src.size(), 19, out, err));
  CHECK(out.size() == 19 && std::memcmp(out.data(), kSampleOut, 19) == 0);
  CHECK(!Lz4Decode(src.data(), src.size(), 18, out, err));  // wrong size
  CHECK(!Lz4Decode(src.data(), src.size(), 20, out, err));
  for (size_t cut = 1; cut + 1 < src.size(); ++cut) {  // truncation: error or exact size, never a crash
    Lz4Decode(src.data(), cut, 19, out, err);
  }
  const uint8_t badOffset[] = {0x10, 'a', 9, 0};  // match 4 at offset 9 with 1 byte out
  CHECK(!Lz4Decode(badOffset, sizeof badOffset, 5, out, err));
  const uint8_t zeroOffset[] = {0x10, 'a', 0, 0};
  CHECK(!Lz4Decode(zeroOffset, sizeof zeroOffset, 5, out, err));
  const uint8_t hugeLiterals[] = {0xF0, 255, 255, 'a'};
  CHECK(!Lz4Decode(hugeLiterals, sizeof hugeLiterals, 8, out, err));
  const uint8_t openLength[] = {0xF0, 255};
  CHECK(!Lz4Decode(openLength, sizeof openLength, 300, out, err));
  // A long run uses extended lengths: 20 literals = 0xF0, 5, then data.
  std::vector<uint8_t> longRun = {0xF0, 5};
  for (int i = 0; i < 20; ++i) {
    longRun.push_back(uint8_t('A' + i));
  }
  CHECK(Lz4Decode(longRun.data(), longRun.size(), 20, out, err) && out[19] == 'T');
  CHECK(Lz4Decode(nullptr, 0, 0, out, err));
}

static std::vector<uint8_t> SamplePfs0() {
  std::vector<uint8_t> v = {'P', 'F', 'S', '0'};
  Put32(v, 2);
  Put32(v, 12);
  Put32(v, 0);
  Put64(v, 0), Put64(v, 5), Put32(v, 0), Put32(v, 0);
  Put64(v, 5), Put64(v, 3), Put32(v, 5), Put32(v, 0);
  const char names[12] = {'n', 'p', 'd', 'm', 0, 'm', 'a', 'i', 'n', 0, 0, 0};
  v.insert(v.end(), names, names + 12);
  const char data[] = "hellobye";
  v.insert(v.end(), data, data + 8);
  return v;
}

static void TestPfs0() {
  const auto pfs = SamplePfs0();
  CHECK(Pfs0HeaderSize(pfs.data()) == 16 + 48 + 12);
  std::vector<Pfs0Entry> entries;
  std::string err;
  CHECK(Pfs0Parse(pfs.data(), 16 + 48 + 12, entries, err));
  CHECK(entries.size() == 2);
  CHECK(entries[1].name == "main" && entries[1].offset == 76 + 5 && entries[1].size == 3);
  CHECK(std::memcmp(pfs.data() + entries[1].offset, "bye", 3) == 0);
  CHECK(!Pfs0Parse(pfs.data(), 40, entries, err));  // header cut short
  auto bad = pfs;
  Set32(bad, 16 + 16, 99);  // name offset past the strings
  CHECK(!Pfs0Parse(bad.data(), 76, entries, err));
  bad = pfs;
  Set32(bad, 4, 0x7FFFFFFF);
  CHECK(Pfs0HeaderSize(bad.data()) == 0);
  bad[0] = 'X';
  CHECK(Pfs0HeaderSize(bad.data()) == 0);
}

static void TestNso() {
  // Segment 0 stored raw at memory 0x100, segment 1 LZ4 at memory 0x1000.
  const auto lz4 = SampleLz4();
  std::vector<uint8_t> nso(0x100, 0);
  std::memcpy(nso.data(), "NSO0", 4);
  Set32(nso, 0xC, 2);  // segment 1 compressed
  Set32(nso, 0x10, 0x100), Set32(nso, 0x14, 0x100), Set32(nso, 0x18, 8);
  Set32(nso, 0x20, 0x108), Set32(nso, 0x24, 0x1000), Set32(nso, 0x28, 19);
  Set32(nso, 0x60, 8), Set32(nso, 0x64, uint32_t(lz4.size()));
  const char raw[8] = {0, 1, 2, 3, 4, 5, 6, 7};
  nso.resize(0x108 + lz4.size());
  std::memcpy(nso.data() + 0x100, raw, 8);
  std::memcpy(nso.data() + 0x108, lz4.data(), lz4.size());
  std::vector<uint8_t> out;
  std::string err;
  CHECK(NsoReadImage(nso, 0x102, 4, out, err) && out.size() == 4 && out[0] == 2 && out[3] == 5);
  CHECK(NsoReadImage(nso, 0x1004, 8, out, err) && std::memcmp(out.data(), "abcdabcd", 8) == 0);
  CHECK(!NsoReadImage(nso, 0x1010, 8, out, err));  // runs past the segment
  CHECK(!NsoReadImage(nso, 0x50, 4, out, err));    // in no segment
  auto bad = nso;
  bad.resize(0x110);  // compressed data cut off
  CHECK(!NsoReadImage(bad, 0x1004, 8, out, err));
  std::vector<uint8_t> notNso(0x100, 0);
  CHECK(!NsoReadImage(notNso, 0x100, 4, out, err));
}

int main() {
  TestLz4();
  TestPfs0();
  TestNso();
  if (port_test::Failures() == 0) {
    std::printf("port_remastered_nso_tests: ok\n");
  }
  return PORT_TEST_RESULT();
}
