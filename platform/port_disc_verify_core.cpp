#include "port_disc_verify.h"

#include <zlib.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace PortDiscVerify {

namespace {
uint32_t Rotl(uint32_t value, int bits) { return (value << bits) | (value >> (32 - bits)); }
uint32_t Be32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | p[3];
}
} // namespace

Sha1Stream::Sha1Stream() : mState{0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0}, mBuffer{} {}

void Sha1Stream::Block(const uint8_t* block) {
  uint32_t words[80];
  for (size_t i = 0; i < 16; ++i) {
    words[i] = Be32(block + i * 4);
  }
  for (size_t i = 16; i < 80; ++i) {
    words[i] = Rotl(words[i - 3] ^ words[i - 8] ^ words[i - 14] ^ words[i - 16], 1);
  }
  uint32_t a = mState[0], b = mState[1], c = mState[2], d = mState[3], e = mState[4];
  for (uint32_t i = 0; i < 80; ++i) {
    uint32_t f, k;
    if (i < 20) {
      f = (b & c) | (~b & d);
      k = 0x5a827999;
    } else if (i < 40) {
      f = b ^ c ^ d;
      k = 0x6ed9eba1;
    } else if (i < 60) {
      f = (b & c) | (b & d) | (c & d);
      k = 0x8f1bbcdc;
    } else {
      f = b ^ c ^ d;
      k = 0xca62c1d6;
    }
    const uint32_t t = Rotl(a, 5) + f + e + k + words[i];
    e = d;
    d = c;
    c = Rotl(b, 30);
    b = a;
    a = t;
  }
  mState[0] += a;
  mState[1] += b;
  mState[2] += c;
  mState[3] += d;
  mState[4] += e;
}

void Sha1Stream::Update(const void* data, size_t size) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  mLength += size;
  if (mFill != 0) {
    const size_t take = std::min(size, sizeof(mBuffer) - mFill);
    std::memcpy(mBuffer + mFill, bytes, take);
    mFill += take;
    bytes += take;
    size -= take;
    if (mFill < sizeof(mBuffer)) {
      return;
    }
    Block(mBuffer);
    mFill = 0;
  }
  for (; size >= 64; size -= 64, bytes += 64) {
    Block(bytes);
  }
  if (size != 0) {
    std::memcpy(mBuffer, bytes, size);
    mFill = size;
  }
}

void Sha1Stream::Final(uint8_t out[20]) {
  const uint64_t bits = mLength * 8;
  const uint8_t pad = 0x80;
  Update(&pad, 1);
  const uint8_t zero = 0;
  while (mFill != 56) {
    Update(&zero, 1);
  }
  uint8_t length[8];
  for (int i = 0; i < 8; ++i) {
    length[i] = static_cast<uint8_t>(bits >> (56 - i * 8));
  }
  Update(length, 8);
  for (int i = 0; i < 5; ++i) {
    out[i * 4] = static_cast<uint8_t>(mState[i] >> 24);
    out[i * 4 + 1] = static_cast<uint8_t>(mState[i] >> 16);
    out[i * 4 + 2] = static_cast<uint8_t>(mState[i] >> 8);
    out[i * 4 + 3] = static_cast<uint8_t>(mState[i]);
  }
}

std::string Sha1Stream::FinalHex() {
  uint8_t digest[20];
  Final(digest);
  char hex[41];
  for (int i = 0; i < 20; ++i) {
    std::snprintf(hex + i * 2, 3, "%02x", digest[i]);
  }
  return std::string(hex, 40);
}

const Revision* FindRevision(const char* id6, unsigned version) {
  struct Entry {
    const char* id;
    unsigned version;
    const char* name;
    unsigned long long size;
    const char* sha1;
    unsigned count;
    std::vector<uint32_t> crc;
  };
  static const std::vector<Revision> table = [] {
    const std::vector<Entry> entries = {
#include "port_disc_verify_table.inc"
    };
    std::vector<Revision> out;
    for (const Entry& e : entries) {
      out.push_back({e.id, e.version, e.name, e.size, e.sha1, e.crc});
    }
    return out;
  }();
  if (id6 == nullptr) {
    return nullptr;
  }
  for (const Revision& r : table) {
    if (std::memcmp(r.id6.data(), id6, 6) == 0 && r.version == version) {
      return &r;
    }
  }
  return nullptr;
}

uint32_t Crc32(const void* data, size_t size) {
  uint32_t crc = static_cast<uint32_t>(crc32(0L, nullptr, 0));
  const auto* bytes = static_cast<const Bytef*>(data);
  while (size != 0) {
    const uInt n = static_cast<uInt>(std::min<size_t>(size, 1u << 30));
    crc = static_cast<uint32_t>(crc32(crc, bytes, n));
    bytes += n;
    size -= n;
  }
  return crc;
}

bool AllZero(const uint8_t* data, size_t size) {
  return size == 0 || (data[0] == 0 && std::memcmp(data, data + 1, size - 1) == 0);
}

std::vector<FstFile> ParseFst(const uint8_t* fst, size_t size) {
  std::vector<FstFile> files;
  if (size < 12 || fst[0] != 1) {
    return files;
  }
  const uint32_t count = Be32(fst + 8);
  if (count == 0 || static_cast<uint64_t>(count) * 12 > size) {
    return files;
  }
  const uint8_t* names = fst + static_cast<size_t>(count) * 12;
  const size_t namesSize = size - static_cast<size_t>(count) * 12;
  auto nameOf = [&](const uint8_t* entry) {
    const size_t at = Be32(entry) & 0x00ffffff;
    if (at >= namesSize) {
      return std::string();
    }
    return std::string(reinterpret_cast<const char*>(names + at), strnlen(reinterpret_cast<const char*>(names + at), namesSize - at));
  };
  // Each open directory: the entry index it ends at, and its path.
  std::vector<std::pair<uint32_t, std::string>> dirs{{count, std::string()}};
  for (uint32_t i = 1; i < count; ++i) {
    const uint8_t* entry = fst + static_cast<size_t>(i) * 12;
    while (dirs.size() > 1 && i >= dirs.back().first) {
      dirs.pop_back();
    }
    const std::string path = dirs.back().second + nameOf(entry);
    if (entry[0] != 0) {
      dirs.emplace_back(Be32(entry + 8), path + "/");
    } else {
      files.push_back({Be32(entry + 4), Be32(entry + 8), path});
    }
  }
  return files;
}

std::vector<std::string> FilesOverlapping(const std::vector<FstFile>& files, uint64_t begin, uint64_t end) {
  std::vector<std::string> out;
  for (const FstFile& f : files) {
    if (f.size != 0 && f.offset < end && f.offset + f.size > begin) {
      out.push_back(f.path);
    }
  }
  return out;
}

void CompareChunk(const Revision& revision, uint32_t index, const uint8_t* data, size_t size,
                  std::vector<BadChunk>& bad) {
  if (index >= revision.crc.size()) {
    return;
  }
  const size_t expected = std::min<uint64_t>(kChunkSize, revision.size - static_cast<uint64_t>(index) * kChunkSize);
  if (size == expected && Crc32(data, size) == revision.crc[index]) {
    return;
  }
  const bool unreadable = size < expected;
  bad.push_back({index, !unreadable && AllZero(data, size), unreadable});
}

std::string Summary(const Result& r, size_t maxFiles) {
  char head[256];
  if (r.state == State::Error) {
    return "disc check: could not run: " + r.error;
  }
  if (r.state == State::Cancelled) {
    return "disc check: cancelled";
  }
  if (!r.known) {
    return "disc check: SHA-1 " + r.sha1 + ", not a disc image the port has a Redump record of";
  }
  if (r.Ok()) {
    return "disc check: " + r.revision + ", SHA-1 " + r.sha1 + " matches Redump";
  }
  if (r.bad.empty()) {
    return "disc check: " + r.revision + ", SHA-1 " + r.sha1 + " does NOT match Redump, though every MiB does (extra data after the end?)";
  }
  uint32_t unreadable = 0;
  for (const BadChunk& b : r.bad) {
    unreadable += b.unreadable;
  }
  std::snprintf(head, sizeof(head), "disc check: %s does NOT match: %zu of %u MiB differ (%u zeroed)", r.revision.c_str(),
                r.bad.size(), r.chunks, r.zeroed);
  std::string text = head;
  if (unreadable != 0) {
    text += ", " + std::to_string(unreadable) + " of them unreadable";
  }
  if (!r.files.empty()) {
    text += " in ";
    for (size_t i = 0; i < r.files.size() && i < maxFiles; ++i) {
      text += (i ? ", " : "") + r.files[i];
    }
    if (r.files.size() > maxFiles) {
      text += ", and " + std::to_string(r.files.size() - maxFiles) + " more";
    }
  }
  return text;
}

} // namespace PortDiscVerify
