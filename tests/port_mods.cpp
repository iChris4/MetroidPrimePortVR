#include "port_mods.h"
#include "port_test.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

void Put32(std::vector<uint8_t>& out, uint32_t value) {
  out.push_back(uint8_t(value >> 24));
  out.push_back(uint8_t(value >> 16));
  out.push_back(uint8_t(value >> 8));
  out.push_back(uint8_t(value));
}

uint32_t Get32(const std::vector<uint8_t>& data, size_t at) {
  return (uint32_t(data[at]) << 24) | (uint32_t(data[at + 1]) << 16) | (uint32_t(data[at + 2]) << 8) | data[at + 3];
}

constexpr uint32_t kTXTR = 0x54585452;
constexpr uint32_t kSTRG = 0x53545247;

// A PAK with one named resource and three table entries; resource 0x11 is
// listed twice, as retail PAKs do. Data is filled with each entry's index + 1.
std::vector<uint8_t> MakePak(size_t& headerEnd) {
  std::vector<uint8_t> pak;
  Put32(pak, 0x00030005);
  Put32(pak, 0);
  Put32(pak, 1);
  Put32(pak, kSTRG);
  Put32(pak, 0x22);
  Put32(pak, 5);
  for (const char c : std::string("Hello")) {
    pak.push_back(uint8_t(c));
  }
  Put32(pak, 3);
  const size_t table = pak.size();
  headerEnd = table + 3 * 20;
  const uint32_t data = uint32_t((headerEnd + 31) & ~size_t(31));
  const uint32_t entries[3][3] = {{kTXTR, 0x11, 64}, {kSTRG, 0x22, 32}, {kTXTR, 0x11, 64}};
  const uint32_t offsets[3] = {data, data + 64, data};
  for (int i = 0; i < 3; ++i) {
    Put32(pak, i == 1 ? 1 : 0);
    Put32(pak, entries[i][0]);
    Put32(pak, entries[i][1]);
    Put32(pak, entries[i][2]);
    Put32(pak, offsets[i]);
  }
  pak.resize(data, 0);
  pak.resize(data + 64, 1);
  pak.resize(data + 96, 2);
  pak.resize(data + 100, 3); // an unaligned tail
  return pak;
}

// The "disc" a Reader's kSource segments read.
const std::vector<uint8_t>* sSource = nullptr;
int sOpens = 0;

void* OpenMemory(const PortMods::VirtualFile&) {
  ++sOpens;
  return const_cast<std::vector<uint8_t>*>(sSource);
}

int64_t ReadMemory(void* handle, uint64_t offset, uint8_t* buffer, size_t length) {
  const auto& data = *static_cast<const std::vector<uint8_t>*>(handle);
  if (offset >= data.size()) {
    return 0;
  }
  const size_t n = std::min<size_t>(length, data.size() - size_t(offset));
  std::memcpy(buffer, data.data() + offset, n);
  return int64_t(n);
}

void CloseMemory(void*) {}

const PortMods::SourceIo sIo{OpenMemory, ReadMemory, CloseMemory};

std::vector<uint8_t> ReadAll(const PortMods::VirtualFile& file, size_t chunk) {
  PortMods::Reader reader(std::make_shared<const PortMods::VirtualFile>(file), &sIo);
  std::vector<uint8_t> out(file.size + 16, 0xEE);
  size_t done = 0;
  for (;;) {
    const int64_t got = reader.Read(out.data() + done, std::min(chunk, out.size() - done));
    if (got <= 0) {
      break;
    }
    done += size_t(got);
  }
  out.resize(done);
  return out;
}

void TestNames() {
  uint32_t type = 0;
  uint32_t id = 0;
  CHECK_MSG(PortMods::ParseLooseName("1a2B3c4D.txtr", type, id) && type == kTXTR && id == 0x1A2B3C4D, "loose name");
  CHECK_MSG(!PortMods::ParseLooseName("1A2B3C4.TXTR", type, id), "short id");
  CHECK_MSG(!PortMods::ParseLooseName("1A2B3C4G.TXTR", type, id), "bad hex");
  CHECK_MSG(!PortMods::ParseLooseName("1A2B3C4D.TXT", type, id), "short type");
  CHECK_MSG(!PortMods::ParseLooseName("Metroid1.pak", type, id), "disc name");
  CHECK_MSG(PortMods::ParseNativeTextureName("1a2B3c4D.DdS", id) && id == 0x1A2B3C4D, "native texture name");
  CHECK_MSG(!PortMods::ParseNativeTextureName("1A2B3C4D.png", id), "native texture: not a dds");
  CHECK_MSG(!PortMods::ParseNativeTextureName("1A2B3C4G.dds", id), "native texture: bad hex");
  CHECK_MSG(!PortMods::ParseNativeTextureName("tex1_64x64_0123456789abcdef_14.dds", id), "native texture: a texture pack name");
  CHECK_MSG(!PortMods::ParseLooseName("1A2B3C4D.dds", type, id), "native texture is not a loose resource");
  CHECK_MSG(PortMods::FourCCString(kTXTR) == "TXTR", "fourcc");
  CHECK_MSG(PortMods::SplitDisabled("a/b//c") == std::vector<std::string>({"a", "b", "c"}), "split");
  CHECK_MSG(PortMods::JoinDisabled({"a", "", "b/c", "d"}) == "a/d", "join");
}

void TestParse() {
  size_t headerEnd = 0;
  const std::vector<uint8_t> pak = MakePak(headerEnd);
  PortMods::PakTable table;
  size_t needed = 0;
  CHECK_MSG(PortMods::ParsePakTable(pak.data(), pak.size(), table, needed), "parse");
  CHECK_MSG(table.headerEnd == headerEnd && table.resources.size() == 3, "table size");
  CHECK_MSG(table.resources[1].compressed == 1 && table.resources[1].id == 0x22, "entry fields");
  for (size_t cut = 0; cut < headerEnd; cut += 7) {
    CHECK_MSG(!PortMods::ParsePakTable(pak.data(), cut, table, needed) && needed > cut && needed <= headerEnd,
              "truncated header asks for more");
  }
  std::vector<uint8_t> bad = pak;
  bad[3] = 6;
  CHECK_MSG(!PortMods::ParsePakTable(bad.data(), bad.size(), table, needed) && needed == 0, "wrong version");
}

void TestPatch(const fs::path& dir) {
  size_t headerEnd = 0;
  const std::vector<uint8_t> pak = MakePak(headerEnd);
  PortMods::PakTable table;
  size_t needed = 0;
  PortMods::ParsePakTable(pak.data(), pak.size(), table, needed);

  // 40 bytes of 'A': padded to 64 in the patched PAK.
  const fs::path loosePath = dir / "00000011.TXTR";
  std::ofstream(loosePath, std::ios::binary) << std::string(40, 'A');
  PortMods::LooseResource loose{kTXTR, 0x11, loosePath.string(), 40, "test"};
  PortMods::LooseResource unused{kTXTR, 0x99, loosePath.string(), 40, "test"};
  const PortMods::VirtualFile file = PortMods::PatchPak(pak, table, pak.size(), {&loose, &unused});
  const uint64_t appended = (pak.size() + 31) & ~uint64_t(31);
  CHECK_MSG(file.size == appended + 64, "patched size");

  sSource = &pak;
  sOpens = 0;
  for (const size_t chunk : {size_t(1), size_t(13), size_t(4096)}) {
    const std::vector<uint8_t> out = ReadAll(file, chunk);
    CHECK_MSG(out.size() == file.size, "read whole file");
    if (out.size() != file.size) {
      continue;
    }
    // Both entries of 0x11 now point at the appended copy, uncompressed.
    for (const size_t entry : {table.resources[0].entryOffset, table.resources[2].entryOffset}) {
      CHECK_MSG(Get32(out, entry) == 0 && Get32(out, entry + 12) == 64 && Get32(out, entry + 16) == appended,
                "patched entry");
    }
    // The other entry and the original data are untouched.
    const size_t other = table.resources[1].entryOffset;
    CHECK_MSG(std::memcmp(out.data() + other, pak.data() + other, 20) == 0, "untouched entry");
    CHECK_MSG(std::memcmp(out.data() + headerEnd, pak.data() + headerEnd, pak.size() - headerEnd) == 0,
              "original data");
    bool padding = true;
    for (uint64_t i = pak.size(); i < appended; ++i) {
      padding = padding && out[i] == 0;
    }
    CHECK_MSG(padding, "alignment padding");
    CHECK_MSG(std::string(out.begin() + appended, out.begin() + appended + 40) == std::string(40, 'A'), "loose data");
    bool tail = true;
    for (uint64_t i = appended + 40; i < file.size; ++i) {
      tail = tail && out[i] == 0;
    }
    CHECK_MSG(tail, "loose padding");
  }
  CHECK_MSG(sOpens == 3, "source opened once per reader");

  // Seek to the middle of a segment boundary and read across it.
  PortMods::Reader reader(std::make_shared<const PortMods::VirtualFile>(file), &sIo);
  CHECK_MSG(reader.Seek(-8, 2) == int64_t(file.size - 8), "seek from end");
  CHECK_MSG(reader.Seek(int64_t(appended) - 4, 0) == int64_t(appended - 4), "seek set");
  uint8_t cross[8];
  CHECK_MSG(reader.Read(cross, 8) == 8 && cross[3] == 0 && cross[4] == 'A', "read across segments");
  CHECK_MSG(reader.Seek(-100000, 1) == -1, "seek before start");

  // A PAK replaced whole by a mod: the original data comes from its host file.
  const fs::path hostPak = dir / "Metroid1.pak";
  std::ofstream(hostPak, std::ios::binary).write(reinterpret_cast<const char*>(pak.data()), std::streamsize(pak.size()));
  const PortMods::VirtualFile hosted = PortMods::PatchPak(pak, table, pak.size(), {&loose}, hostPak.string());
  sSource = nullptr;
  sOpens = 0;
  const std::vector<uint8_t> out = ReadAll(hosted, 4096);
  CHECK_MSG(sOpens == 0 && out.size() == hosted.size, "hosted PAK reads without the disc");
  CHECK_MSG(out.size() == hosted.size &&
                std::memcmp(out.data() + headerEnd, pak.data() + headerEnd, pak.size() - headerEnd) == 0,
            "hosted original data");
}

void TestAdd(const fs::path& dir) {
  size_t headerEnd = 0;
  const std::vector<uint8_t> pak = MakePak(headerEnd);
  PortMods::PakTable table;
  size_t needed = 0;
  PortMods::ParsePakTable(pak.data(), pak.size(), table, needed);

  const fs::path replacePath = dir / "00000011.TXTR";
  const fs::path newPath = dir / "00000033.TXTR";
  const fs::path smallPath = dir / "00000044.CMDL";
  std::ofstream(replacePath, std::ios::binary) << std::string(40, 'A');
  std::ofstream(newPath, std::ios::binary) << std::string(40, 'B');
  std::ofstream(smallPath, std::ios::binary) << std::string(10, 'C');
  PortMods::LooseResource replace{kTXTR, 0x11, replacePath.string(), 40, "test"};
  PortMods::LooseResource added{kTXTR, 0x33, newPath.string(), 40, "test"};
  PortMods::LooseResource smallRes{0x434D444C, 0x44, smallPath.string(), 10, "test"};
  const PortMods::VirtualFile file = PortMods::PatchPak(pak, table, pak.size(), {&replace}, {}, {&added, &smallRes});

  // Two entries (40 bytes) move the data down by 64.
  const uint64_t shift = 64;
  const uint64_t appended = (pak.size() + shift + 31) & ~uint64_t(31);
  CHECK_MSG(file.size == appended + 64 + 64 + 32, "grown size");
  sSource = &pak;
  const std::vector<uint8_t> out = ReadAll(file, 4096);
  if (out.size() != file.size) {
    CHECK_MSG(false, "read grown file");
    return;
  }
  PortMods::PakTable grown;
  CHECK_MSG(PortMods::ParsePakTable(out.data(), out.size(), grown, needed) && grown.resources.size() == 5 &&
                grown.headerEnd == headerEnd + 40,
            "grown table");
  if (grown.resources.size() != 5) {
    return;
  }
  const PortMods::PakResource& strg = grown.resources[1];
  CHECK_MSG(strg.compressed == 1 && strg.offset == table.resources[1].offset + shift, "shifted entry");
  CHECK_MSG(std::memcmp(out.data() + strg.offset, pak.data() + table.resources[1].offset, 32) == 0, "shifted data");
  CHECK_MSG(std::memcmp(out.data() + headerEnd + shift, pak.data() + headerEnd, pak.size() - headerEnd) == 0,
            "original data moved whole");
  CHECK_MSG(grown.resources[0].offset == appended && grown.resources[2].offset == appended, "replaced entry");
  const PortMods::PakResource& first = grown.resources[3];
  const PortMods::PakResource& second = grown.resources[4];
  CHECK_MSG(first.type == kTXTR && first.id == 0x33 && first.compressed == 0 && first.size == 64 &&
                first.offset == appended + 64,
            "added entry");
  CHECK_MSG(second.type == 0x434D444C && second.id == 0x44 && second.size == 32 && second.offset == appended + 128,
            "second added entry");
  CHECK_MSG(std::string(out.begin() + first.offset, out.begin() + first.offset + 40) == std::string(40, 'B'),
            "added data");
  CHECK_MSG(std::string(out.begin() + second.offset, out.begin() + second.offset + 10) == std::string(10, 'C'),
            "second added data");
  bool gap = true;
  for (size_t i = grown.headerEnd; i < headerEnd + shift; ++i) {
    gap = gap && out[i] == 0;
  }
  CHECK_MSG(gap, "table padding");
}

void TestNewPak(const fs::path& dir) {
  const PortMods::VirtualFile empty = PortMods::NewPak({});
  std::vector<uint8_t> out = ReadAll(empty, 4096);
  PortMods::PakTable table;
  size_t needed = 0;
  CHECK_MSG(out.size() == empty.size && PortMods::ParsePakTable(out.data(), out.size(), table, needed) &&
                table.resources.empty(),
            "empty new PAK");

  const fs::path path = dir / "00000055.TXTR";
  std::ofstream(path, std::ios::binary) << std::string(40, 'D');
  PortMods::LooseResource res{kTXTR, 0x55, path.string(), 40, "test"};
  const PortMods::VirtualFile file = PortMods::NewPak({&res});
  out = ReadAll(file, 4096);
  CHECK_MSG(out.size() == file.size && PortMods::ParsePakTable(out.data(), out.size(), table, needed) &&
                table.resources.size() == 1 && table.resources[0].id == 0x55 &&
                std::string(out.begin() + table.resources[0].offset, out.begin() + table.resources[0].offset + 40) ==
                    std::string(40, 'D'),
            "new PAK with a resource");
}

void TestSplit() {
  const uint64_t big = 0x30000000; // 768 MiB
  PortMods::LooseResource a{kTXTR, 1, {}, big, "t"}, b{kTXTR, 2, {}, big, "t"}, c{kTXTR, 3, {}, big, "t"};
  PortMods::LooseResource huge{kTXTR, 4, {}, PortMods::kMaxFileSize, "t"};
  auto groups = PortMods::SplitAdded({&a, &b, &c, &huge}, 0x10000000);
  CHECK_MSG(groups.size() == 2 && groups[0].size() == 2 && groups[1].size() == 1 && groups[1][0] == &c,
            "overflow into a second group, too big skipped");
  groups = PortMods::SplitAdded({&a}, PortMods::kMaxFileSize - 64);
  CHECK_MSG(groups.size() == 2 && groups[0].empty() && groups[1].size() == 1, "full home PAK");
  groups = PortMods::SplitAdded({}, 0);
  CHECK_MSG(groups.size() == 1 && groups[0].empty(), "nothing added");
}
} // namespace

void TestImportStamp() {
  CHECK_MSG(PortMods::ImportStampStale("", 2), "missing stamp is stale");
  CHECK_MSG(PortMods::ImportStampStale("1\nabc123\n", 2), "older is stale");
  CHECK_MSG(!PortMods::ImportStampStale("2\nabc123\n", 2), "equal is current");
  CHECK_MSG(!PortMods::ImportStampStale("3", 2), "newer is current");
  CHECK_MSG(!PortMods::ImportStampStale("  2 \n", 2), "leading blanks");
  CHECK_MSG(PortMods::ImportStampStale("garbage", 2), "garbage is stale");
  CHECK_MSG(PortMods::ImportStampStale("-5", 2), "negative is stale");
  CHECK_MSG(PortMods::ImportStampStale("\n2", 2), "number on a later line is not the stamp");
}

int main() {
  const fs::path dir = fs::temp_directory_path() / "port_mods_tests";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  TestNames();
  TestParse();
  TestPatch(dir);
  TestAdd(dir);
  TestNewPak(dir);
  TestSplit();
  TestImportStamp();
  fs::remove_all(dir, ec);
  if (port_test::Failures() == 0) {
    std::puts("port_mods_tests: ok");
  }
  return PORT_TEST_RESULT();
}
