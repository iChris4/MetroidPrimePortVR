// SLTW in a component real: the port-only PSLW with its two children converted.
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

void PutCnst(std::vector<uint8_t>& out, float value) {
  PutFourCC(out, "CNST");
  Put32(out, Bits(value));
}

void TestSltw() {
  std::vector<uint8_t> out = Header();
  PutProperty(out, "SIZE", 3);
  PutFourCC(out, "SLTW");
  PutCnst(out, 0.f);
  PutCnst(out, 1.f);
  PutTexr(out, 0x1234ABCD);
  PutProperty(out, "_END", 4);
  const std::vector<ConvertedPart> parts = Convert(out);
  std::vector<uint8_t> want = {'S', 'I', 'Z', 'E', 'P', 'S', 'L', 'W', 'C', 'N', 'S', 'T'};
  Append(want, BE(Bits(0.f)));
  Append(want, {'C', 'N', 'S', 'T'});
  Append(want, BE(Bits(1.f)));
  CHECK_MSG(parts.size() == 1 && Contains(parts[0].part, want), "SLTW maps onto the port-only PSLW");
  if (parts.size() == 1) {
    CHECK_MSG(parts[0].dropped.empty(), "nothing is dropped");
    CHECK_MSG(!Contains(parts[0].part, {'S', 'L', 'T', 'W'}), "no SLTW left");
  }
}

}  // namespace

int main() {
  TestSltw();
  return PORT_TEST_RESULT();
}
