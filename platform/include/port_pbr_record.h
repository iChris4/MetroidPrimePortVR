#pragma once
// The port's PBR material record: what a converter appends to a PBR material and
// CCubeModel::PortReadPBRMaterial reads back from the material's end (the layouts are
// described there). Kept apart from the game classes so a test can read a record too.

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "port_bytes.h"

namespace PortPbrRecord {

inline uint32_t Be32(const uint8_t* p) { return port::ReadBE32(p); }

inline float BeFloat(const uint8_t* p) { return port::ReadBEFloat(p); }

// Reads the record that ends at `end` (one past its tag) in a material of `size` bytes.
// values gets the 19 floats (neutral where the record is short or absent), wrap the maps'
// wrap word (every axis repeat without one) and lightScale the diffuse and F0 factors of a
// back-facing copy (1, 1 without 'PBR6'), and cube the id of the material's own reflection
// cube (0 without 'PBR7'). Returns how many floats the record held. A TEV material may end
// in a record of its own: the wrap word and 'WRAP', no floats.
// A PBR material may end in an 'LMUV' trailer (a big-endian word: the vertex texcoord slot
// 0..7 holding the model's lightmap UV, then the tag), after everything else. Read() strips it
// first, so it does not disturb the record; LightmapSlot() returns the slot, or -1 without one.
// A kind 14-19 record ends in a trailer, 32 floats and 'PBR8' (the boundary shield's CCH0..CCH6
// and DIFC; the holograms' ICNC + ICMC in row 6), read into `shield` when it is given (all zero without one); the record before it
// is read as usual.
inline int Read(const uint8_t* end, size_t size, float values[19], uint32_t* wrap,
                float lightScale[2], uint32_t* cube = nullptr, float* shield = nullptr) {
  if (size >= 8 && std::memcmp(end - 4, "LMUV", 4) == 0) {
    end -= 8;
    size -= 8;
  }
  if (shield != nullptr) {
    for (int i = 0; i < 32; ++i) {
      shield[i] = 0.f;
    }
  }
  if (size >= 4 + 128 + 4 && std::memcmp(end - 4, "PBR8", 4) == 0) {
    if (shield != nullptr) {
      for (int i = 0; i < 32; ++i) {
        shield[i] = BeFloat(end - 4 - 128 + i * 4);
      }
    }
    end -= 4 + 128;
    size -= 4 + 128;
  }
  for (int i = 0; i < 19; ++i) {
    values[i] = i < 3 ? 1.f : 0.f;
  }
  if (wrap != nullptr) {
    *wrap = 0x55555555;
  }
  if (lightScale != nullptr) {
    lightScale[0] = lightScale[1] = 1.f;
  }
  if (cube != nullptr) {
    *cube = 0;
  }
  int floats = 0;
  const uint8_t* floatsEnd = end - 4; // the tag's start, until a longer record moves it
  // 'PBR7' is 'PBR6' with the cube's id between the factors and the tag.
  const bool withCube = size >= 96 && std::memcmp(end - 4, "PBR7", 4) == 0;
  if (withCube) {
    if (cube != nullptr) {
      *cube = Be32(end - 8);
    }
    end -= 4;
  }
  if (withCube || (size >= 92 && std::memcmp(end - 4, "PBR6", 4) == 0)) {
    if (lightScale != nullptr) {
      lightScale[0] = BeFloat(end - 12);
      lightScale[1] = BeFloat(end - 8);
    }
    floatsEnd = end - 16;
    if (wrap != nullptr) {
      *wrap = Be32(end - 16);
    }
    floats = 19;
  } else if (size >= 84 && std::memcmp(end - 4, "PBR5", 4) == 0) {
    if (wrap != nullptr) {
      *wrap = Be32(end - 8);
    }
    floatsEnd = end - 8;
    floats = 19;
  } else if (size >= 80 && std::memcmp(end - 4, "PBR4", 4) == 0) {
    floats = 19;
  } else if (size >= 56 && std::memcmp(end - 4, "PBR3", 4) == 0) {
    floats = 13;
  } else if (size >= 36 && std::memcmp(end - 4, "PBR2", 4) == 0) {
    floats = 8;
  } else if (size >= 28 && std::memcmp(end - 4, "PBRM", 4) == 0) {
    floats = 6;
  } else if (size >= 8 && std::memcmp(end - 4, "WRAP", 4) == 0) {
    // A TEV material's: only the wrap word, its slots' modes, and no floats.
    if (wrap != nullptr) {
      *wrap = Be32(end - 8);
    }
  }
  const uint8_t* record = floatsEnd - floats * 4;
  for (int i = 0; i < floats; ++i) {
    values[i] = BeFloat(record + i * 4);
  }
  return floats;
}

// The vertex texcoord slot of the 'LMUV' trailer ending at `end`, or -1 if there is none.
inline int LightmapSlot(const uint8_t* end, size_t size) {
  if (size >= 8 && std::memcmp(end - 4, "LMUV", 4) == 0) {
    const uint32_t slot = Be32(end - 8);
    return slot < 8 ? int(slot) : -1;
  }
  return -1;
}

} // namespace PortPbrRecord
