// CHPR animation reader on small synthetic files: a blob with two stream infos (a bone
// stream and a bool-only stream whose op stream holds a sub-op 4), an init-dequant offset
// other than 0x28, and a record chain followed by a false record with a tiny length.
#include "port_remastered_anim.h"
#include "port_test.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

// LSB-first bit writer, the order the game's bit streams are read in.
struct Bits {
  std::vector<uint8_t> bytes;
  size_t n = 0;
  void Put(uint32_t value, int count) {
    for (int i = 0; i < count; ++i, ++n) {
      if (n / 8 >= bytes.size()) {
        bytes.push_back(0);
      }
      bytes[n / 8] |= uint8_t(((value >> i) & 1u) << (n % 8));
    }
  }
};

void Le(std::vector<uint8_t>& v, uint32_t x, int bytes) {
  for (int i = 0; i < bytes; ++i) {
    v.push_back(uint8_t(x >> (8 * i)));
  }
}

// Stream 0: no tracks, one op stream group `sub-op 3, 1 entry` (a rotation and a translation
// pool reference). Stream 1: one type 3 track (bool words, extra 1) and an op stream with a
// sub-op 4 (a byte, the track index, one u16 word).
std::vector<uint8_t> Blob(uint32_t ctx0, bool mixed) {
  Bits h;  // header bits
  h.Put(2, 4);
  h.Put(1, 10);
  h.Put(0, 4);
  const uint32_t pad = mixed ? 32 : 0;  // room for the longer header and init-dequant bits
  const uint32_t w1[2] = {0x34 + pad, 0x44 + pad};
  const uint32_t w2[2] = {0x38 + pad, 0x48 + pad};
  for (int s = 0; s < 2; ++s) {
    h.Put(w1[s], 20);
    h.Put(w2[s], 20);
    h.Put(s == 0 && mixed ? 3 : s, 4);  // tracks
    h.Put(0, 1);
    h.Put(0, 1);
    h.Put(1, 4);  // v
    h.Put(0, 4);  // u6
    h.Put(s == 0 ? ctx0 : 0, 4);  // constant pool index
    if (s == 0 && mixed) {
      // Variable values next to the bone ops: real32, int32 and a one-bit value.
      h.Put(1, 10);
      h.Put(4, 3);
      h.Put(0, 1);
      h.Put(1, 10);
      h.Put(5, 3);
      h.Put(0, 1);
      h.Put(1, 10);
      h.Put(3, 3);
      h.Put(0, 1);
      h.Put(1, 6);
    }
    if (s == 1) {
      h.Put(1, 10);  // count
      h.Put(3, 3);   // type: bool words
      h.Put(0, 1);
      h.Put(1, 6);  // word count
    }
  }
  Bits init;
  if (mixed) {
    init.Put(0, 4);  // select stream 0
    init.Put(0, 4);
    init.Put(4, 4);  // track 0: real32, 8 bits, exponent 2, signed
    init.Put(0, 4);
    init.Put(8, 5);
    init.Put(2, 5);
    init.Put(1, 1);
    init.Put(6, 4);  // track 1: int32, 6 bits, signed
    init.Put(1, 4);
    init.Put(6, 5);
    init.Put(1, 1);
  }
  init.Put(0, 4);  // select stream
  init.Put(1, 4);
  init.Put(5, 4);  // bool values
  init.Put(0, 4);  // track 0
  init.Put(1, 12);
  init.Put(1, 1);
  init.Put(0x1234, 16);
  init.Put(1, 4);  // done
  std::vector<uint8_t> b;
  Le(b, 0x54 + pad, 4);
  Le(b, 0x18, 2);
  Le(b, 0x2c + pad / 2, 2);  // init-dequant offset: not the usual 0x28
  Le(b, 0, 4);
  Le(b, 0x3f800000, 4);  // rate 1.0
  Le(b, 1, 2);           // frames
  Le(b, 1, 4);
  Le(b, 0, 2);
  b.insert(b.end(), h.bytes.begin(), h.bytes.end());
  b.resize(0x2c + pad / 2);
  b.insert(b.end(), init.bytes.begin(), init.bytes.end());
  b.resize(0x34 + pad);
  b.resize(0x38 + pad, 0);  // stream 0 data
  const uint8_t ops0[] = {1, 8, 0, 3, 1, 0, 0, 0, 0, 1, 0, 0};
  b.insert(b.end(), ops0, ops0 + sizeof ops0);
  b.resize(0x48 + pad, 0);  // stream 1 data
  const uint8_t ops1[] = {1, 5, 0, 4, 0, 0, 0x34, 0x12, 0};
  b.insert(b.end(), ops1, ops1 + sizeof ops1);
  b.resize(0x54 + pad, 0);
  return b;
}

std::vector<uint8_t> File(int records, bool tinyTail, bool pools, bool mixed) {
  std::vector<uint8_t> f(0x100, 0);
  std::memcpy(f.data(), "RFRM", 4);
  std::memcpy(f.data() + 0x14, "CHPR", 4);
  if (pools) {
    // Pool list well before the first record: bool, count, then (u16 a, u16 n, n floats) each.
    size_t p = 0x40;
    f[p++] = 1;
    f[p++] = 2;  // count, u32
    p += 3;
    for (int k = 0; k < 2; ++k) {
      f[p++] = 0;
      f[p++] = 0;
      f[p++] = 4;
      f[p++] = 0;
      const uint32_t one = 0x3f800000;
      for (int b = 0; b < 4; ++b) {
        f[p++] = uint8_t(one >> (8 * b));
      }
      p += 12;
    }
  }
  const std::vector<uint8_t> blob = Blob(pools ? 1 : 0, mixed);
  for (int r = 0; r < records; ++r) {
    f.push_back(1);
    Le(f, uint32_t(r + 1), 4);
    Le(f, uint32_t(blob.size()), 4);
    // The blob's own size field is the record length: the length is at record + 5.
    f.resize(f.size() - 4);
    f.insert(f.end(), blob.begin(), blob.end());
    f.resize(f.size() + 7, 0);
  }
  if (tinyTail) {
    f.push_back(1);
    Le(f, 20, 4);
    Le(f, 2, 4);
    f.resize(f.size() + 16, 0);
  }
  return f;
}

void Run(const char* what, int records, bool tinyTail, bool pools = false, bool mixed = false) {
  PortRemasteredAnim::Character c;
  std::string error;
  const bool ok = PortRemasteredAnim::ReadCharacter(File(records, tinyTail, pools, mixed), c, error);
  if (!ok) {
    std::fprintf(stderr, "%s: %s\n", what, error.c_str());
  }
  CHECK_MSG(ok, what);
  if (ok) {
    CHECK_MSG(c.anims.size() == size_t(records), "one animation per record");
    CHECK_MSG(!c.anims.empty() && c.anims[0].frames == 1 && c.anims[0].fps == 60.0f, "frames and rate");
    CHECK_MSG(!c.anims.empty() && !c.anims[0].tracked.empty() && c.anims[0].tracked[0], "bone 0 is driven");
  }
}

// The model list: the header after SChannel::Load (here at 0x86 of an all-zero skeleton:
// u8 nModels, u8, u8 nAnchors, u16 nAnims) gives the anchor count; the last record ends at
// its size field + size, nAnchors 12-byte SAnchorInfo follow, then SModelNode: CPooledName,
// i32, i32, the node guid and the SMDL guid. The 7 bytes File() leaves after a record are
// the next CAnim's SBaseInfo, which the last record does not have.
std::vector<uint8_t> ModelFile(size_t anchors, size_t headerAnims, bool nameFlag) {
  std::vector<uint8_t> f = File(1, false, false, false);
  f[0x86] = 1;
  f[0x88] = uint8_t(anchors);
  f[0x89] = uint8_t(headerAnims);
  f.resize(f.size() - 7);
  f.resize(f.size() + 12 * anchors, 0xee);
  f.push_back(nameFlag ? 1 : 0);
  if (nameFlag) {
    Le(f, 0x1234, 4);
  }
  Le(f, 0, 4);
  Le(f, 0xffffffffu, 4);
  for (int i = 0; i < 16; ++i) {
    f.push_back(uint8_t(0x10 + i));  // the node guid
  }
  for (int i = 0; i < 16; ++i) {
    f.push_back(uint8_t(0xa0 + i));  // the SMDL
  }
  f.resize(f.size() + 64, 0);
  return f;
}

void RunAnchors(size_t anchors, bool nameFlag) {
  PortRemasteredAnim::Character c;
  std::string error;
  CHECK_MSG(PortRemasteredAnim::ReadCharacter(ModelFile(anchors, 4, nameFlag), c, error), "anchor test reads");
  CHECK_MSG(c.skinnedModel[0] == 0xa0 && c.skinnedModel[15] == 0xaf, "SMDL is the node's second guid");
}

// A header with fewer animations than records leaves no model instead of guessing one.
void RunMismatch() {
  PortRemasteredAnim::Character c;
  std::string error;
  CHECK_MSG(PortRemasteredAnim::ReadCharacter(ModelFile(2, 0, true), c, error), "mismatch test reads");
  CHECK_MSG(c.skinnedModel[0] == 0 && c.skinnedModel[15] == 0, "no SMDL when the header counts fewer animations than records");
}

// Joint scale in SkinPose: a parent scaled by 2 along x carries its child's matrix with it
// (abs_bone_rel, 0xba19a0); an inverse-scale child (abs op 6, 0xba1bf0) divides the
// parent's scale back out of its rows, leaving its translation alone.
void RunScale() {
  using namespace PortRemasteredAnim;
  for (int inverse = 0; inverse < 2; ++inverse) {
    Character c;
    c.bones.resize(2);
    c.bones[1].parent = 0;
    c.bones[1].absOp = inverse ? 6 : 4;
    c.bones[1].invScale = inverse != 0;
    c.bones[1].bind.translation[0] = 1.0f;
    c.animBone = {0, 1};
    c.jointBone = {1};
    c.inverseBind = {{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}};
    Anim a;
    a.frames = 1;
    a.fps = 30.f;
    a.bones.assign(2, std::vector<Key>(1));
    a.tracked = {true, true};
    a.bones[0][0].scale[0] = 2.0f;
    std::vector<std::array<float, 12>> out;
    CHECK_MSG(SkinPose(c, a, 0, out) && out.size() == 1, "scaled pose resolves");
    if (out.size() != 1) {
      continue;
    }
    // Translation: the parent's scaled x on the child's bind offset.
    CHECK_MSG(out[0][3] == 2.0f, "the child's offset is scaled by the parent");
    CHECK_MSG(out[0][0] == (inverse ? 1.0f : 2.0f), inverse ? "inverse scale cancels the parent's" : "scale is inherited");
    CHECK_MSG(out[0][5] == 1.0f && out[0][10] == 1.0f, "unscaled axes stay");
  }
}
}  // namespace

int main() {
  RunScale();
  Run("single record, init-dequant offset 0x2c", 1, false);
  Run("two records and a tiny false record", 2, true);
  Run("stream names pool 1 of 2, list far before the records", 1, false, true);
  Run("bone ops beside real32, int32 and one-bit values", 1, false, false, true);
  RunAnchors(0, true);
  RunAnchors(1, true);
  RunAnchors(2, false);
  RunMismatch();
  return PORT_TEST_RESULT();
}
