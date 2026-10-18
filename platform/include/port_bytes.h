#pragma once

// Little- and big-endian loads and stores for the port's file readers and
// writers. Each helper here used to be a file-local copy in a platform/*.cpp
// file; gathering them keeps the copies from drifting apart. They are exact
// on any host: the bytes are assembled with shifts, and floats go through
// memcpy rather than a reinterpret cast.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace port {

// The bytes at `p`, least significant first.
inline uint16_t ReadLE16(const uint8_t* p) {
  return uint16_t(p[0]) | uint16_t(uint16_t(p[1]) << 8);
}

inline uint32_t ReadLE32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

inline uint64_t ReadLE64(const uint8_t* p) {
  return uint64_t(ReadLE32(p)) | (uint64_t(ReadLE32(p + 4)) << 32);
}

// A little-endian IEEE-754 binary32.
inline float ReadLEFloat(const uint8_t* p) {
  const uint32_t bits = ReadLE32(p);
  float value = 0.0f;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

// The bytes at `p`, most significant first. FourCCs are read this way so
// they stay printable and comparable as one number.
inline uint16_t ReadBE16(const uint8_t* p) {
  return uint16_t((uint16_t(p[0]) << 8) | uint16_t(p[1]));
}

inline uint32_t ReadBE32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

inline uint64_t ReadBE64(const uint8_t* p) {
  return (uint64_t(ReadBE32(p)) << 32) | uint64_t(ReadBE32(p + 4));
}

// A big-endian IEEE-754 binary32.
inline float ReadBEFloat(const uint8_t* p) {
  const uint32_t bits = ReadBE32(p);
  float value = 0.0f;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

// Stores into a buffer the caller has already sized.
inline void WriteLE16(uint8_t* p, uint16_t value) {
  p[0] = uint8_t(value);
  p[1] = uint8_t(value >> 8);
}

inline void WriteLE32(uint8_t* p, uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    p[i] = uint8_t(value >> (i * 8));
  }
}

inline void WriteBE16(uint8_t* p, uint16_t value) {
  p[0] = uint8_t(value >> 8);
  p[1] = uint8_t(value);
}

inline void WriteBE32(uint8_t* p, uint32_t value) {
  p[0] = uint8_t(value >> 24);
  p[1] = uint8_t(value >> 16);
  p[2] = uint8_t(value >> 8);
  p[3] = uint8_t(value);
}

inline void WriteBEFloat(uint8_t* p, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  WriteBE32(p, bits);
}

// A FourCC packed big-endian, as written: FourCC('C', 'M', 'D', 'L') and
// FourCC("CMDL") are both 0x434D444C, the number ReadBE32 gives for the four
// bytes on disk. The string form reads the first four characters.
constexpr uint32_t FourCC(char a, char b, char c, char d) {
  return (uint32_t(uint8_t(a)) << 24) | (uint32_t(uint8_t(b)) << 16) | (uint32_t(uint8_t(c)) << 8) |
         uint32_t(uint8_t(d));
}

constexpr uint32_t FourCC(const char* text) {
  return FourCC(text[0], text[1], text[2], text[3]);
}

// Big-endian words at an offset inside a byte vector, the layout of the
// disc's PAK-like formats.
inline uint32_t GetBE32(const std::vector<uint8_t>& data, size_t at) {
  return (uint32_t(data[at]) << 24) | (uint32_t(data[at + 1]) << 16) | (uint32_t(data[at + 2]) << 8) |
         uint32_t(data[at + 3]);
}

inline float GetBEFloat(const std::vector<uint8_t>& data, size_t at) {
  const uint32_t bits = GetBE32(data, at);
  float value = 0.0f;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

inline void SetBE32(std::vector<uint8_t>& data, size_t at, uint32_t value) {
  data[at] = uint8_t(value >> 24);
  data[at + 1] = uint8_t(value >> 16);
  data[at + 2] = uint8_t(value >> 8);
  data[at + 3] = uint8_t(value);
}

inline void SetBEFloat(std::vector<uint8_t>& data, size_t at, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  SetBE32(data, at, bits);
}

// A bounds-checked read cursor over one byte range, for the parsers that walk
// a file front to back. `big` picks the byte order of the multi-byte reads.
// A read that would run past the end returns zeros (Bytes a null pointer)
// and clears `ok`; the failure is sticky, so every later read fails too and
// a caller can check `ok` once after a run of reads. The fields are public:
// parsers step `at` themselves, and one that resynchronises after a bad
// record sets `ok` again.
struct ByteCursor {
  const uint8_t* data = nullptr;
  size_t size = 0;
  size_t at = 0;
  bool big = true;
  bool ok = true;

  // True when `count` more bytes can be read; otherwise clears `ok`.
  bool Has(size_t count) {
    if (at > size || count > size - at) {
      ok = false;
    }
    return ok;
  }
  size_t Remaining() const { return at <= size ? size - at : 0; }

  const uint8_t* Bytes(size_t count) {
    if (!Has(count)) {
      return nullptr;
    }
    const uint8_t* p = data + at;
    at += count;
    return p;
  }
  void Skip(size_t count) {
    if (Has(count)) {
      at += count;
    }
  }
  uint8_t U8() { return Has(1) ? data[at++] : 0; }
  uint16_t U16() {
    const uint8_t* p = Bytes(2);
    return p == nullptr ? 0 : big ? ReadBE16(p) : ReadLE16(p);
  }
  uint32_t U32() {
    const uint8_t* p = Bytes(4);
    return p == nullptr ? 0 : big ? ReadBE32(p) : ReadLE32(p);
  }
  uint64_t U64() {
    const uint8_t* p = Bytes(8);
    return p == nullptr ? 0 : big ? ReadBE64(p) : ReadLE64(p);
  }
  float Float() {
    const uint32_t bits = U32();
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
  }
  // The bytes up to a NUL, which is consumed; a string the range cuts off
  // before its NUL clears `ok` and returns what was there.
  std::string CString() {
    std::string s;
    while (Has(1) && data[at] != 0) {
      s.push_back(char(data[at++]));
    }
    Skip(1);
    return s;
  }
};

// The appends take any byte container with push_back: std::vector<uint8_t>
// for blobs, std::string for the savestate writer.
template <typename Bytes>
void AppendLE16(Bytes& out, uint16_t value) {
  out.push_back(uint8_t(value));
  out.push_back(uint8_t(value >> 8));
}

template <typename Bytes>
void AppendLE32(Bytes& out, uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    out.push_back(uint8_t(value >> (i * 8)));
  }
}

template <typename Bytes>
void AppendLE64(Bytes& out, uint64_t value) {
  AppendLE32(out, uint32_t(value));
  AppendLE32(out, uint32_t(value >> 32));
}

template <typename Bytes>
void AppendBE16(Bytes& out, uint16_t value) {
  out.push_back(uint8_t(value >> 8));
  out.push_back(uint8_t(value));
}

template <typename Bytes>
void AppendBE32(Bytes& out, uint32_t value) {
  out.push_back(uint8_t(value >> 24));
  out.push_back(uint8_t(value >> 16));
  out.push_back(uint8_t(value >> 8));
  out.push_back(uint8_t(value));
}

template <typename Bytes>
void AppendLEFloat(Bytes& out, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  AppendLE32(out, bits);
}

// A double is narrowed to binary32 first, matching what the formats store.
template <typename Bytes>
void AppendLEFloat(Bytes& out, double value) {
  AppendLEFloat(out, float(value));
}

template <typename Bytes>
void AppendBEFloat(Bytes& out, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  AppendBE32(out, bits);
}

template <typename Bytes>
void AppendBEFloat(Bytes& out, double value) {
  AppendBEFloat(out, float(value));
}

}  // namespace port
