// See port_remastered_nso.h.

#include "port_remastered_nso.h"
#include "port_bytes.h"

#include <cstring>

namespace PortRemastered {
namespace {

using port::ReadLE32;
using port::ReadLE64;

constexpr uint32_t kMaxPfs0Files = 4096;
constexpr uint32_t kMaxPfs0Strings = 1u << 20;

} // namespace

bool Lz4Decode(const uint8_t* src, size_t size, size_t outSize, std::vector<uint8_t>& out, std::string& error) {
  out.clear();
  out.reserve(outSize);
  size_t i = 0;
  auto fail = [&](const char* why) {
    error = std::string("bad LZ4 stream: ") + why;
    out.clear();
    return false;
  };
  while (i < size) {
    const uint8_t token = src[i++];
    size_t literals = token >> 4;
    if (literals == 15) {
      for (;;) {
        if (i >= size) {
          return fail("truncated length");
        }
        const uint8_t b = src[i++];
        literals += b;
        if (literals > outSize) {
          return fail("literal run too long");
        }
        if (b != 255) {
          break;
        }
      }
    }
    if (literals > size - i || literals > outSize - out.size()) {
      return fail("literal run out of bounds");
    }
    out.insert(out.end(), src + i, src + i + literals);
    i += literals;
    if (i >= size) {
      break;  // the last sequence has no match
    }
    if (size - i < 2) {
      return fail("truncated offset");
    }
    const size_t offset = src[i] | src[i + 1] << 8;
    i += 2;
    size_t match = token & 15;
    if (match == 15) {
      for (;;) {
        if (i >= size) {
          return fail("truncated length");
        }
        const uint8_t b = src[i++];
        match += b;
        if (match > outSize) {
          return fail("match too long");
        }
        if (b != 255) {
          break;
        }
      }
    }
    match += 4;
    if (offset == 0 || offset > out.size()) {
      return fail("match before the start");
    }
    if (match > outSize - out.size()) {
      return fail("match past the end");
    }
    size_t from = out.size() - offset;
    for (size_t k = 0; k < match; ++k) {  // may overlap, so byte by byte
      out.push_back(out[from + k]);
    }
  }
  if (out.size() != outSize) {
    return fail("wrong decoded size");
  }
  return true;
}

size_t Pfs0HeaderSize(const uint8_t* h) {
  if (std::memcmp(h, "PFS0", 4) != 0) {
    return 0;
  }
  const uint32_t count = ReadLE32(h + 4), strings = ReadLE32(h + 8);
  if (count == 0 || count > kMaxPfs0Files || strings > kMaxPfs0Strings) {
    return 0;
  }
  return 16 + size_t(count) * 24 + strings;
}

bool Pfs0Parse(const uint8_t* data, size_t size, std::vector<Pfs0Entry>& out, std::string& error) {
  out.clear();
  if (size < 16) {
    error = "PFS0 too small";
    return false;
  }
  const size_t header = Pfs0HeaderSize(data);
  if (header == 0 || header > size) {
    error = "implausible PFS0 header";
    return false;
  }
  const uint32_t count = ReadLE32(data + 4), stringSize = ReadLE32(data + 8);
  const uint8_t* strings = data + 16 + size_t(count) * 24;
  const uint64_t dataStart = header;
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* e = data + 16 + size_t(i) * 24;
    const uint32_t nameOffset = ReadLE32(e + 16);
    if (nameOffset >= stringSize) {
      error = "bad PFS0 name offset";
      return false;
    }
    Pfs0Entry entry;
    entry.name.assign(reinterpret_cast<const char*>(strings + nameOffset),
                      strnlen(reinterpret_cast<const char*>(strings + nameOffset), stringSize - nameOffset));
    entry.offset = dataStart + ReadLE64(e);
    entry.size = ReadLE64(e + 8);
    out.push_back(std::move(entry));
  }
  return true;
}

bool NsoReadImage(const std::vector<uint8_t>& nso, uint32_t memOffset, size_t size, std::vector<uint8_t>& out,
                  std::string& error) {
  if (nso.size() < 0x70 || std::memcmp(nso.data(), "NSO0", 4) != 0) {
    error = "not an NSO";
    return false;
  }
  const uint32_t flags = ReadLE32(nso.data() + 0xC);
  for (int i = 0; i < 3; ++i) {
    const uint8_t* seg = nso.data() + 0x10 + i * 0x10;
    const uint64_t fileOff = ReadLE32(seg), memOff = ReadLE32(seg + 4), segSize = ReadLE32(seg + 8);
    if (memOffset < memOff || uint64_t(memOffset) + size > memOff + segSize) {
      continue;
    }
    const uint64_t stored = (flags >> i & 1) ? ReadLE32(nso.data() + 0x60 + i * 4) : segSize;
    if (fileOff > nso.size() || stored > nso.size() - fileOff) {
      error = "NSO segment outside the file";
      return false;
    }
    std::vector<uint8_t> image;
    if (flags >> i & 1) {
      if (!Lz4Decode(nso.data() + fileOff, size_t(stored), size_t(segSize), image, error)) {
        return false;
      }
    } else {
      image.assign(nso.begin() + fileOff, nso.begin() + fileOff + segSize);
    }
    out.assign(image.begin() + (memOffset - memOff), image.begin() + (memOffset - memOff) + size);
    return true;
  }
  error = "range is not inside one NSO segment";
  return false;
}

} // namespace PortRemastered
