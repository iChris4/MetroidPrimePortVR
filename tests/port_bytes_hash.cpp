// port_bytes.h's readers and cursor and port_hash.h's hashes. The hash values
// are the published test vectors and the values the copies these replaced
// gave, since ids and fingerprints made with them are stored in files.

#include "port_bytes.h"
#include "port_hash.h"
#include "port_strings.h"
#include "port_synthetic_effect.h"
#include "port_test.h"

#include <cstdio>
#include <string>
#include <vector>

int main() {
  const uint8_t bytes[] = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0, 0x3F, 0x80, 0x00, 0x00, 'h', 'i', 0};

  CHECK_MSG(port::ReadBE16(bytes) == 0x1234, "ReadBE16");
  CHECK_MSG(port::ReadBE32(bytes) == 0x12345678u, "ReadBE32");
  CHECK_MSG(port::ReadBE64(bytes) == 0x123456789ABCDEF0ull, "ReadBE64");
  CHECK_MSG(port::ReadLE32(bytes) == 0x78563412u, "ReadLE32");
  CHECK_MSG(port::ReadBEFloat(bytes + 8) == 1.0f, "ReadBEFloat");
  uint8_t out[4] = {};
  port::WriteBE32(out, 0xCAFEF00Du);
  CHECK_MSG(port::ReadBE32(out) == 0xCAFEF00Du, "WriteBE32");
  port::WriteLE32(out, 0xCAFEF00Du);
  CHECK_MSG(port::ReadLE32(out) == 0xCAFEF00Du, "WriteLE32");
  port::WriteBEFloat(out, -2.5f);
  CHECK_MSG(port::ReadBEFloat(out) == -2.5f, "WriteBEFloat");

  static_assert(port::FourCC('C', 'M', 'D', 'L') == 0x434D444Cu, "FourCC");
  static_assert(port::FourCC("RFRM") == 0x5246524Du, "FourCC string");

  // The cursor: both byte orders, zeros past the end, and a sticky failure.
  port::ByteCursor be{bytes, sizeof(bytes)};
  CHECK_MSG(be.U16() == 0x1234 && be.U16() == 0x5678 && be.U32() == 0x9ABCDEF0u, "cursor big-endian");
  CHECK_MSG(be.Float() == 1.0f && be.CString() == "hi" && be.ok && be.Remaining() == 0, "cursor float and string");
  CHECK_MSG(be.U8() == 0 && !be.ok, "cursor overrun");
  be.at = 0;
  CHECK_MSG(be.U8() == 0 && !be.ok, "cursor failure is sticky");
  port::ByteCursor le{bytes, 8, 0, false};
  CHECK_MSG(le.U32() == 0x78563412u && le.U64() == 0 && !le.ok && le.at == 4, "cursor little-endian and short read");
  port::ByteCursor cut{bytes + 12, 2};
  CHECK_MSG(cut.CString() == "hi" && !cut.ok, "cursor unterminated string");

  // FNV-1a test vectors.
  CHECK_MSG(port::Fnv1a32("") == 0x811C9DC5u, "fnv32 empty");
  CHECK_MSG(port::Fnv1a32("a") == 0xE40C292Cu, "fnv32 a");
  CHECK_MSG(port::Fnv1a32("foobar") == 0xBF9CF968u, "fnv32 foobar");
  CHECK_MSG(port::Fnv1a64("") == 0xCBF29CE484222325ull, "fnv64 empty");
  CHECK_MSG(port::Fnv1a64("a") == 0xAF63DC4C8601EC8Cull, "fnv64 a");
  CHECK_MSG(port::Fnv1a64("foobar") == 0x85944171F73967E8ull, "fnv64 foobar");
  const std::string text = "foobar";
  CHECK_MSG(port::Fnv1a64(reinterpret_cast<const uint8_t*>(text.data()), text.size()) == port::Fnv1a64(text),
        "fnv64 bytes and text agree");
  CHECK_MSG(port::Fnv1a32("bar", port::Fnv1a32("foo")) == port::Fnv1a32("foobar"), "fnv32 continues");
  // The synthetic effect id folds case before hashing.
  static_assert(PortRemastered::SyntheticEffectId("BombAttract") == PortRemastered::SyntheticEffectId("bombattract"),
                "synthetic id ignores case");
  CHECK_MSG(PortRemastered::SyntheticEffectId("bombattract") == (0xFE000000u | (port::Fnv1a32("bombattract") & 0xFFFFFFu)),
        "synthetic id");

  CHECK_MSG(port::Crc32("123456789") == 0xCBF43926u, "crc32 check value");
  CHECK_MSG(port::Crc32("") == 0, "crc32 empty");

  // String helpers.
  CHECK_MSG(port::Trim("  a b \t\r\n") == "a b" && port::Trim(" \t ").empty(), "Trim");
  CHECK_MSG(port::TrimView("\ta\r", " \t") == "a\r", "TrimView with a set");
  CHECK_MSG(port::StartsWith("prefix", "pre") && !port::StartsWith("pr", "pre"), "StartsWith");
  const std::vector<std::string> fields = port::Split("a,,b", ',');
  CHECK_MSG(fields.size() == 3 && fields[1].empty() && port::Split("", ',').size() == 1, "Split");

  if (port_test::Failures() == 0) std::puts("port_bytes_hash: all passed");
  return PORT_TEST_RESULT();
}
