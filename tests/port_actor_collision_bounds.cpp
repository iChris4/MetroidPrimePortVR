// The reader that gives a solid actor its disc model's collision box, over
// synthetic PAKs instead of a disc: an original model (compressed or not) is
// read, a mod's own file never is, and every malformed shape falls back rather
// than answering a box the disc never had.

#include "port_actor_collision_bounds.h"
#include "port_test.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace {

void Put32(std::vector<uint8_t>& out, uint32_t value) {
  out.push_back(uint8_t(value >> 24));
  out.push_back(uint8_t(value >> 16));
  out.push_back(uint8_t(value >> 8));
  out.push_back(uint8_t(value));
}

void PutFloat(std::vector<uint8_t>& out, float value) {
  uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  Put32(out, bits);
}

void PutFloatAt(std::vector<uint8_t>& out, size_t at, float value) {
  uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  for (int i = 0; i < 4; ++i) {
    out[at + size_t(i)] = uint8_t(bits >> (24 - 8 * i));
  }
}

constexpr uint32_t kCMDL = 0x434D444C;
constexpr uint32_t kTXTR = 0x54585452;

// A CMDL header carrying `bounds` (min xyz then max xyz), with a plausible tail.
std::vector<uint8_t> MakeCmdl(const float bounds[6], uint32_t magic = 0xDEADBABE, uint32_t version = 2,
                              size_t pad = 64) {
  std::vector<uint8_t> out;
  Put32(out, magic);
  Put32(out, version);
  Put32(out, 0x1083);
  for (int i = 0; i < 6; ++i) {
    PutFloat(out, bounds[i]);
  }
  Put32(out, 4);
  Put32(out, 1);
  for (int i = 0; i < 4; ++i) {
    Put32(out, 0);
  }
  out.resize(out.size() + pad, 0x5A);
  return out;
}

// A PAK whose table points at the blobs given, in order. `compressed` says
// whether each blob is a zlib stream rather than the resource itself.
struct Entry {
  uint32_t id = 0;
  std::vector<uint8_t> data;
  bool compressed = false;
};

std::vector<uint8_t> MakePak(const std::vector< Entry >& entries) {
  std::vector<uint8_t> pak;
  Put32(pak, 0x00030005);
  Put32(pak, 0);
  Put32(pak, 0); // no named resources
  Put32(pak, uint32_t(entries.size()));
  const size_t table = pak.size();
  const size_t headerEnd = table + entries.size() * 20;
  const size_t data = (headerEnd + 31) & ~size_t(31);
  size_t at = data;
  std::vector< uint32_t > offsets;
  for (const Entry& entry : entries) {
    offsets.push_back(uint32_t(at));
    at += entry.data.size();
  }
  for (size_t i = 0; i < entries.size(); ++i) {
    Put32(pak, entries[i].compressed ? 1u : 0u);
    Put32(pak, kCMDL);
    Put32(pak, entries[i].id);
    Put32(pak, uint32_t(entries[i].data.size()));
    Put32(pak, offsets[i]);
  }
  pak.resize(data, 0);
  for (const Entry& entry : entries) {
    pak.insert(pak.end(), entry.data.begin(), entry.data.end());
  }
  return pak;
}

// The "disc": one file per entry number, with a PAK in the first few.
struct Disc {
  std::vector< std::vector<uint8_t> > files;
  std::vector< std::pair< int32_t, std::string > > paks;
  int32_t baseEntries = 0;
  int opens = 0;
};

Disc sDisc;

void* OpenEntry(int32_t entry) {
  if (entry < 0 || size_t(entry) >= sDisc.files.size() || sDisc.files[size_t(entry)].empty()) {
    return nullptr;
  }
  ++sDisc.opens;
  return &sDisc.files[size_t(entry)];
}

int64_t ReadEntry(void* handle, uint64_t offset, uint8_t* buffer, size_t length) {
  const auto& file = *static_cast< const std::vector<uint8_t>* >(handle);
  if (offset >= file.size()) {
    return 0;
  }
  const size_t n = std::min<size_t>(length, file.size() - size_t(offset));
  std::memcpy(buffer, file.data() + offset, n);
  return int64_t(n);
}

void CloseEntry(void*) {}

PortActorCollisionBounds::SourceIo MakeIo() {
  PortActorCollisionBounds::SourceIo io;
  io.discPaks = [] { return sDisc.paks; };
  io.entryCount = [] { return sDisc.baseEntries; };
  io.open = OpenEntry;
  io.readAt = ReadEntry;
  io.close = CloseEntry;
  return io;
}

// A zlib stream: the big-endian length, two header bytes, a stored DEFLATE
// block and the adler the decoder never reaches - the shape a Prime 1 PAK
// stores a compressed resource in.
std::vector<uint8_t> Deflate(const std::vector<uint8_t>& raw) {
  std::vector<uint8_t> out;
  Put32(out, uint32_t(raw.size()));
  out.push_back(0x78);
  out.push_back(0x01);
  size_t at = 0;
  do {
    const size_t n = std::min<size_t>(65535, raw.size() - at);
    const bool last = at + n >= raw.size();
    out.push_back(last ? 1 : 0); // BFINAL, BTYPE 00 (stored), byte aligned
    out.push_back(uint8_t(n));
    out.push_back(uint8_t(n >> 8));
    out.push_back(uint8_t(~n));
    out.push_back(uint8_t((~n) >> 8));
    out.insert(out.end(), raw.begin() + long(at), raw.begin() + long(at + n));
    at += n;
  } while (at < raw.size());
  Put32(out, 0); // adler32, never reached
  return out;
}

bool Box(PortActorCollisionBounds::Reader& reader, uint32_t id, float out[6]) {
  return reader.Bounds(id, out);
}

void Reset() {
  sDisc = Disc();
}

// The lava crust: the disc's model is flat in Z, a replacement draws a few
// centimetres of thickness. The collider has to be the flat one.
constexpr float kFlat[6] = {-10.5f, -35.3f, 0.0f, 10.5f, 35.3f, 0.0f};
constexpr float kThick[6] = {-10.5f, -35.3f, -2.85f, 10.5f, 35.3f, 2.85f};

void TestOriginalVsReplacement() {
  Reset();
  sDisc.paks = {{0, "Metroid1.pak"}};
  sDisc.baseEntries = 4;
  sDisc.files.resize(4);
  sDisc.files[0] = MakePak({
      Entry{0xC8C4E650, MakeCmdl(kFlat)},
      Entry{0x11111111, Deflate(MakeCmdl(kFlat)), true},
      Entry{0x22222222, MakeCmdl(kThick)},
  });

  PortActorCollisionBounds::Reader reader(MakeIo());
  CHECK_MSG(reader.Build(), "the index builds over a synthetic PAK");
  CHECK_MSG(reader.Indexed() && reader.ModelCount() == 3, "every model is indexed");
  CHECK_MSG(reader.ModelCount() == 3 && sDisc.opens == 1, "one file is opened once");

  float bounds[6] = {};
  CHECK_MSG(Box(reader, 0xC8C4E650, bounds) && bounds[0] == kFlat[0] && bounds[5] == kFlat[5],
            "an uncompressed model gives the disc's flat box, not a thick one");
  CHECK_MSG(bounds[2] == 0.f && bounds[5] == 0.f, "the disc's flat box stays flat in Z");
  CHECK_MSG(Box(reader, 0x11111111, bounds) && bounds[0] == kFlat[0] && bounds[4] == kFlat[4],
            "a compressed model gives the same box as an uncompressed one");
  CHECK_MSG(Box(reader, 0x22222222, bounds) && bounds[5] == kThick[5], "a thick model on the disc is read as it is");

  // A model the disc does not have is not invented.
  float missing[6];
  CHECK_MSG(!Box(reader, 0xDEADBEEF, missing), "an id the disc lacks has no box");
  CHECK_MSG(!Box(reader, 0, missing), "id zero has no box");

  // Reading again must not touch the file: every answer is cached.
  const int opens = sDisc.opens;
  float again[6] = {};
  CHECK_MSG(Box(reader, 0xC8C4E650, again) && again[5] == kFlat[5], "a cached box is the same box");
  CHECK_MSG(Box(reader, 0xDEADBEEF, missing) == false, "a cached miss stays a miss");
  CHECK_MSG(sDisc.opens == opens, "cached answers do not reopen or reread");
}

void TestModFilesAreNotCanonical() {
  Reset();
  // Entry 1 is a PAK a mod brought and entry 3 one it replaced: neither is a
  // base entry, so neither may contribute a box.
  sDisc.paks = {{0, "Metroid1.pak"}, {1, "PortMods1.pak"}, {3, "Metroid2.pak"}};
  sDisc.baseEntries = 2;
  sDisc.files.resize(4);
  sDisc.files[0] = MakePak({Entry{0xC8C4E650, MakeCmdl(kFlat)}});
  sDisc.files[1] = MakePak({Entry{0xC8C4E650, MakeCmdl(kThick)}});
  sDisc.files[3] = MakePak({Entry{0x99999999, MakeCmdl(kThick)}});

  PortActorCollisionBounds::Reader reader(MakeIo());
  CHECK_MSG(reader.Build(), "the index builds with mod PAKs present");
  CHECK_MSG(reader.ModelCount() == 1, "only the base PAK's models are indexed");
  float bounds[6] = {};
  CHECK_MSG(Box(reader, 0xC8C4E650, bounds) && bounds[5] == kFlat[5], "the base PAK's box wins over a mod's");
  CHECK_MSG(!Box(reader, 0x99999999, bounds), "a model only a mod brings is not read");
}

void TestMalformed() {
  Reset();
  sDisc.paks = {{0, "Metroid1.pak"}};
  sDisc.baseEntries = 2;
  sDisc.files.resize(2);

  std::vector<uint8_t> nan = MakeCmdl(kFlat);
  PutFloatAt(nan, 0x0c, std::nanf(""));
  float reversed[6] = {10.f, -10.f, 0.f, -10.f, 10.f, 0.f};
  float infinite[6] = {0.f, 0.f, 0.f, std::numeric_limits< float >::infinity(), 0.f, 0.f};
  std::vector<uint8_t> truncated = MakeCmdl(kFlat);
  truncated.resize(0x20); // past the bounds, short of the header
  std::vector<uint8_t> shortStream;
  Put32(shortStream, 64);
  shortStream.push_back(0x78);
  shortStream.push_back(0x01);
  Put32(shortStream, 0xDEADBABE); // not a DEFLATE block

  sDisc.files[0] = MakePak({
      Entry{0x01, nan},
      Entry{0x02, MakeCmdl(kFlat, 0x12345678)},
      Entry{0x03, MakeCmdl(kFlat, 0xDEADBABE, 7)},
      Entry{0x04, MakeCmdl(reversed)},
      Entry{0x05, MakeCmdl(infinite)},
      Entry{0x06, truncated},
      Entry{0x07, shortStream, true}, // a length and two header bytes, then junk
      Entry{0x08, MakeCmdl(kFlat)},
  });
  sDisc.files[1].assign(16, 0); // not a PAK at all

  PortActorCollisionBounds::Reader reader(MakeIo());
  CHECK_MSG(reader.Build(), "the index builds past a PAK that does not parse");
  float bounds[6] = {};
  CHECK_MSG(!Box(reader, 0x01, bounds), "a NaN coordinate is rejected");
  CHECK_MSG(!Box(reader, 0x02, bounds), "a wrong magic is rejected");
  CHECK_MSG(!Box(reader, 0x03, bounds), "an unknown version is rejected");
  CHECK_MSG(!Box(reader, 0x04, bounds), "a reversed box is rejected");
  CHECK_MSG(!Box(reader, 0x05, bounds), "an infinite coordinate is rejected");
  CHECK_MSG(!Box(reader, 0x06, bounds), "a truncated resource is rejected");
  CHECK_MSG(!Box(reader, 0x07, bounds), "a corrupt compressed stream is rejected");
  CHECK_MSG(Box(reader, 0x08, bounds) && bounds[5] == kFlat[5], "a good model beside bad ones still reads");
}

void TestHeaderParser() {
  float bounds[6] = {};
  const std::vector<uint8_t> good = MakeCmdl(kFlat);
  CHECK_MSG(PortActorCollisionBounds::ParseCmdlBounds(good.data(), good.size(), bounds) && bounds[0] == kFlat[0] &&
                bounds[5] == kFlat[5],
            "ParseCmdlBounds reads a box");
  CHECK_MSG(!PortActorCollisionBounds::ParseCmdlBounds(nullptr, good.size(), bounds), "no data, no box");
  CHECK_MSG(!PortActorCollisionBounds::ParseCmdlBounds(good.data(), 0x0c, bounds), "a header cut short has no box");
  CHECK_MSG(!PortActorCollisionBounds::ParseCmdlBounds(good.data(), 0x2b, bounds), "one byte short of the header");
  CHECK_MSG(PortActorCollisionBounds::ParseCmdlBounds(good.data(), 0x2c, bounds), "exactly the header is enough");

  // A box flat in one axis is a real one (the frozen lava's Z).
  const std::vector<uint8_t> flat = MakeCmdl(kFlat);
  CHECK_MSG(PortActorCollisionBounds::ParseCmdlBounds(flat.data(), flat.size(), bounds) && bounds[2] == bounds[5],
            "a flat box parses");
  const float point6[6] = {1.f, 2.f, 3.f, 1.f, 2.f, 3.f};
  const std::vector<uint8_t> point = MakeCmdl(point6);
  CHECK_MSG(PortActorCollisionBounds::ParseCmdlBounds(point.data(), point.size(), bounds) && bounds[0] == 1.f,
            "a box of no extent parses");
  const std::vector<uint8_t> v1 = MakeCmdl(kFlat, 0xDEADBABE, 1);
  CHECK_MSG(PortActorCollisionBounds::ParseCmdlBounds(v1.data(), v1.size(), bounds), "version 1 parses");
}

void TestNoDisc() {
  Reset();
  // Nothing open, and no PAKs at all: the reader has to answer false for
  // everything rather than crash, so the caller keeps the drawn bounds.
  PortActorCollisionBounds::Reader reader(MakeIo());
  CHECK_MSG(!reader.Build(), "an empty disc has no models");
  CHECK_MSG(!reader.Indexed() && reader.ModelCount() == 0, "and says so");
  float bounds[6];
  CHECK_MSG(!Box(reader, 0xC8C4E650, bounds), "no box without a disc");

  PortActorCollisionBounds::SourceIo empty;
  PortActorCollisionBounds::Reader blind(empty);
  CHECK_MSG(!blind.Build(), "a reader with no io builds nothing");
  CHECK_MSG(!blind.Bounds(1, bounds), "and answers nothing");
}
} // namespace

int main() {
  TestOriginalVsReplacement();
  TestModFilesAreNotCanonical();
  TestMalformed();
  TestHeaderParser();
  TestNoDisc();
  if (port_test::Failures() == 0) {
    std::printf("port_actor_collision_bounds: all checks passed\n");
  }
  return PORT_TEST_RESULT();
}