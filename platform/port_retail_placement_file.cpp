#include "port_retail_placement.h"

#include <cstring>

namespace PortRetailPlacement {
namespace {

void Put32(std::vector<uint8_t>& out, uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    out.push_back(uint8_t(value >> (8 * i)));
  }
}

uint32_t Get32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

constexpr size_t kRecordSize = 12 + 12 * 4;

}  // namespace

std::vector<uint8_t> Encode(const std::vector<Record>& records) {
  std::vector<uint8_t> out;
  out.reserve(12 + records.size() * kRecordSize);
  Put32(out, kFileMagic);
  Put32(out, kFileVersion);
  Put32(out, uint32_t(records.size()));
  for (const Record& r : records) {
    Put32(out, r.mrea);
    Put32(out, r.editorId);
    Put32(out, r.model);
    for (float f : r.correction) {
      uint32_t bits;
      std::memcpy(&bits, &f, 4);
      Put32(out, bits);
    }
  }
  return out;
}

bool Decode(const uint8_t* data, size_t size, std::vector<Record>& records) {
  records.clear();
  if (size < 12 || Get32(data) != kFileMagic || Get32(data + 4) != kFileVersion) {
    return false;
  }
  const uint32_t count = Get32(data + 8);
  if ((size - 12) / kRecordSize < count || 12 + count * kRecordSize != size) {
    return false;
  }
  records.resize(count);
  const uint8_t* p = data + 12;
  for (Record& r : records) {
    r.mrea = Get32(p);
    r.editorId = Get32(p + 4);
    r.model = Get32(p + 8);
    for (size_t i = 0; i < 12; ++i) {
      const uint32_t bits = Get32(p + 12 + 4 * i);
      std::memcpy(&r.correction[i], &bits, 4);
    }
    p += kRecordSize;
  }
  return true;
}

void Compose(const float xf[12], const float scale[3], const float correction[12], float out[12]) {
  // xf * diag(scale): column c of xf's basis times scale[c]; then times the correction.
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 4; ++col) {
      float sum = col == 3 ? xf[row * 4 + 3] : 0.f;
      for (int k = 0; k < 3; ++k) {
        sum += xf[row * 4 + k] * scale[k] * correction[k * 4 + col];
      }
      out[row * 4 + col] = sum;
    }
  }
}

}  // namespace PortRetailPlacement
