// KEYF (version 2) in a component real: the port-only PKYF with the loop byte, N, A, B, R, the keys
// and the input element; another version, or a key count that differs from N, is dropped.
#include "port_remastered_effect_convert.h"
#include "port_retail_check.h"
#include "port_test.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace PortRemastered;

namespace {

void Put32(std::vector<uint8_t>& out, uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    out.push_back(uint8_t(value >> (i * 8)));
  }
}

uint32_t Bits(float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, 4);
  return bits;
}

void PutFourCC(std::vector<uint8_t>& out, const char* text) {
  for (int i = 3; i >= 0; --i) {
    out.push_back(uint8_t(text[i]));
  }
}

void PutProperty(std::vector<uint8_t>& out, const char* name, uint8_t tag) {
  PutFourCC(out, name);
  out.push_back(tag);
}

std::vector<uint8_t> Header() {
  std::vector<uint8_t> out(0x3c, 0);
  std::memcpy(out.data(), "RFRM", 4);
  std::memcpy(out.data() + 0x14, "GENP", 4);
  PutFourCC(out, "GPSM");
  out.resize(out.size() + 17);
  Put32(out, 0);
  return out;
}

// A retail texture id, so the generator draws something.
void PutTexr(std::vector<uint8_t>& out, uint32_t retail) {
  PutProperty(out, "TEXR", 0);
  PutFourCC(out, "CNST");
  const uint8_t guid[12] = {0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0xf0, 0xf0, 0x00, 0x00, 0x00};
  out.insert(out.end(), guid, guid + 12);
  for (int shift = 24; shift >= 0; shift -= 8) {
    out.push_back(uint8_t(retail >> shift));
  }
  PutFourCC(out, "NONE");
}

std::vector<uint8_t> BE(uint32_t word) { return {uint8_t(word >> 24), uint8_t(word >> 16), uint8_t(word >> 8), uint8_t(word)}; }

bool Contains(const std::vector<uint8_t>& hay, const std::vector<uint8_t>& needle) {
  for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
    if (std::memcmp(hay.data() + i, needle.data(), needle.size()) == 0) {
      return true;
    }
  }
  return false;
}

void Append(std::vector<uint8_t>& to, const std::vector<uint8_t>& from) { to.insert(to.end(), from.begin(), from.end()); }

std::vector<ConvertedPart> Convert(std::vector<uint8_t>& data) {
  EffectNode effect;
  std::string error;
  CHECK_MSG(ParseEffect(data.data(), data.size(), effect, error), "effect parses");
  return PortRemastered::CheckedConvert(effect, data.data());
}

// A KEYF block: header (version, loop, N, A, B, R, count) and the keys, then SCAL-less CNST input.
void PutKeyf(std::vector<uint8_t>& out, uint32_t version, bool loop, uint32_t n, uint32_t count,
             const std::vector<float>& keys) {
  PutFourCC(out, "KEYF");
  Put32(out, version);
  Put32(out, 0);
  out.push_back(loop ? 1 : 0);
  out.push_back(0);
  Put32(out, n);
  Put32(out, Bits(1.0f));  // A
  Put32(out, Bits(0.5f));  // B
  Put32(out, Bits(2.0f));  // R
  Put32(out, count);
  for (float key : keys) {
    Put32(out, Bits(key));
  }
  PutFourCC(out, "CNST");
  Put32(out, Bits(3.0f));
}

std::vector<ConvertedPart> ConvertSize(uint32_t version, bool loop, uint32_t n, uint32_t count,
                                       const std::vector<float>& keys) {
  std::vector<uint8_t> out = Header();
  PutProperty(out, "SIZE", 3);
  PutKeyf(out, version, loop, n, count, keys);
  PutTexr(out, 0x1234ABCD);
  PutProperty(out, "_END", 4);
  return Convert(out);
}

void TestKeyf() {
  const std::vector<ConvertedPart> parts = ConvertSize(2, true, 3, 3, {0.f, 10.f, 20.f});
  std::vector<uint8_t> want = {'S', 'I', 'Z', 'E', 'P', 'K', 'Y', 'F', 1};
  Append(want, BE(3));
  Append(want, BE(Bits(1.0f)));
  Append(want, BE(Bits(0.5f)));
  Append(want, BE(Bits(2.0f)));
  for (float key : {0.f, 10.f, 20.f}) {
    Append(want, BE(Bits(key)));
  }
  Append(want, {'C', 'N', 'S', 'T'});
  Append(want, BE(Bits(3.0f)));
  CHECK_MSG(parts.size() == 1 && Contains(parts[0].part, want), "KEYF maps onto the port-only PKYF");
  if (parts.size() == 1) {
    CHECK_MSG(parts[0].dropped.empty(), "nothing is dropped");
  }
}

void TestKeyfNoLoop() {
  const std::vector<ConvertedPart> parts = ConvertSize(2, false, 1, 1, {7.f});
  const std::vector<uint8_t> want = {'P', 'K', 'Y', 'F', 0, 0, 0, 0, 1};
  CHECK_MSG(parts.size() == 1 && Contains(parts[0].part, want), "a non-looping KEYF has loop byte 0");
}

void TestKeyfDropped() {
  std::vector<ConvertedPart> parts = ConvertSize(1, false, 2, 2, {0.f, 1.f});
  CHECK_MSG(parts.size() == 1 && !Contains(parts[0].part, {'P', 'K', 'Y', 'F'}) &&
                !parts[0].dropped.empty(),
            "another version is dropped with a reason");
  parts = ConvertSize(2, false, 2, 3, {0.f, 1.f, 2.f});
  CHECK_MSG(parts.size() == 1 && !Contains(parts[0].part, {'P', 'K', 'Y', 'F'}) &&
                !parts[0].dropped.empty(),
            "a key count that differs from N is dropped with a reason");
}

}  // namespace

int main() {
  TestKeyf();
  TestKeyfNoLoop();
  TestKeyfDropped();
  return PORT_TEST_RESULT();
}
