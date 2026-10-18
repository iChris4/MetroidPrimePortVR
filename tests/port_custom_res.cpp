#include "port_custom_res.h"
#include "port_test.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

void Put32(std::vector<uint8_t>& out, uint32_t value) {
  out.push_back(uint8_t(value >> 24));
  out.push_back(uint8_t(value >> 16));
  out.push_back(uint8_t(value >> 8));
  out.push_back(uint8_t(value));
}

uint32_t Get32(const std::vector<uint8_t>& data, size_t at) {
  return (uint32_t(data[at]) << 24) | (uint32_t(data[at + 1]) << 16) | (uint32_t(data[at + 2]) << 8) | data[at + 3];
}

constexpr uint32_t kCMDL = 0x434D444C;
constexpr uint32_t kANCS = 0x414E4353;

// A CMDL with two sections (sizes only) and material set 0 holding textures
// 0x100 + i; its data starts at 64, the next 32-byte boundary after 52.
std::vector<uint8_t> MakeCmdl(uint32_t textures) {
  std::vector<uint8_t> cmdl;
  Put32(cmdl, 0xDEADBABE);
  Put32(cmdl, 2);
  Put32(cmdl, 0);
  for (int i = 0; i < 6; ++i)
    Put32(cmdl, 0);
  Put32(cmdl, 2); // sections
  Put32(cmdl, 1); // material sets
  Put32(cmdl, 4 + 4 * textures);
  Put32(cmdl, 0);
  cmdl.resize(64, 0);
  Put32(cmdl, textures);
  for (uint32_t i = 0; i < textures; ++i)
    Put32(cmdl, 0x100 + i);
  return cmdl;
}

std::vector<uint8_t> MakeAncs(uint32_t model) {
  std::vector<uint8_t> ancs = {0, 1, 0, 1};
  Put32(ancs, 1);  // characters
  Put32(ancs, 0);  // character id
  ancs.push_back(0);
  ancs.push_back(6);
  for (char c : std::string("Node1_11"))
    ancs.push_back(uint8_t(c));
  ancs.push_back(0);
  Put32(ancs, model);
  Put32(ancs, 0x1234); // skin
  return ancs;
}
} // namespace

int main() {
  using namespace PortCustomRes;

  {
    std::vector<uint8_t> cmdl = MakeCmdl(4);
    CHECK_MSG(SetCmdlTexture(cmdl, 3, 0xCAFE) && Get32(cmdl, 64 + 4 + 12) == 0xCAFE &&
                  Get32(cmdl, 64 + 4) == 0x100,
              "a CMDL texture slot is replaced in material set 0");
    const std::vector<uint8_t> before = cmdl;
    CHECK_MSG(!SetCmdlTexture(cmdl, 4, 0xCAFE) && cmdl == before, "a slot past the texture count is refused");
    std::vector<uint8_t> bad = cmdl;
    bad[0] = 0;
    CHECK_MSG(!SetCmdlTexture(bad, 0, 1), "a CMDL without the magic is refused");
    std::vector<uint8_t> tiny(20, 0);
    CHECK_MSG(!SetCmdlTexture(tiny, 0, 1), "a truncated CMDL is refused");
    std::vector<uint8_t> cut(cmdl.begin(), cmdl.begin() + 66);
    CHECK_MSG(!SetCmdlTexture(cut, 0, 1), "a CMDL cut inside the material set is refused");
  }
  {
    std::vector<uint8_t> ancs = MakeAncs(0x95946E41);
    CHECK_MSG(SetAncsModel(ancs, 0x95946E41, 0xDEAF0005) && Get32(ancs, ancs.size() - 8) == 0xDEAF0005 &&
                  Get32(ancs, ancs.size() - 4) == 0x1234,
              "an ANCS character 0 model is replaced");
    CHECK_MSG(!SetAncsModel(ancs, 0x95946E41, 1), "an ANCS whose model isn't the expected one is refused");
    std::vector<uint8_t> cut(ancs.begin(), ancs.begin() + 20);
    CHECK_MSG(!SetAncsModel(cut, 0xDEAF0005, 1), "an ANCS cut inside the name is refused");
  }
  {
    // One CMPR block: red and blue, four-colour mode, every index once.
    std::vector<uint8_t> txtr;
    Put32(txtr, 10);
    Put32(txtr, 0x00040004);
    Put32(txtr, 1);
    const uint8_t block[8] = {0xF8, 0x00, 0x00, 0x1F, 0x1B, 0x1B, 0x1B, 0x1B};
    txtr.insert(txtr.end(), block, block + 8);
    const float same[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    std::vector<uint8_t> kept = txtr;
    CHECK_MSG(TintTxtr(kept, same) && kept == txtr, "an identity tint changes nothing");
    const float swapped[3][3] = {{0, 0, 1}, {0, 1, 0}, {1, 0, 0}};
    std::vector<uint8_t> tinted = txtr;
    CHECK_MSG(TintTxtr(tinted, swapped) && Get32(tinted, 12) == 0xF800001F &&
                  Get32(tinted, 16) == 0x4E4E4E4E,
              "a tint that reverses a block's colours keeps its mode and remaps the indices");
    const float black[3][3] = {};
    CHECK_MSG(TintTxtr(tinted, black) && Get32(tinted, 12) == 0 && Get32(tinted, 16) == 0,
              "a block tinted to one colour stays opaque");
    txtr[3] = 6;
    CHECK_MSG(!TintTxtr(txtr, same), "only CMPR textures are tinted");

    const DiscReader disc = [&](uint32_t id, std::vector<uint8_t>& out) {
      if (id == 0xEFDFFB8C) { // the missile blast shield
        out = MakeCmdl(4);
        return true;
      }
      if (id == 0x5B97098E || id == 0x5C7B215C || id == 0x6E09EA6B || id == 0xFA0C2AE8) {
        out = kept;
        return true;
      }
      return false;
    };
    const uint32_t cmdl = ShieldCmdl(kShieldWavebuster);
    const Resource* shield = Find(cmdl, disc);
    bool textures = shield != nullptr && shield->type == kCMDL;
    for (uint32_t i = 0; textures && i < 4; ++i) {
      const Resource* part = Find(cmdl - 7 + i, disc);
      textures = Get32(shield->data, 64 + 4 + 4 * i) == cmdl - 7 + i && part != nullptr &&
                 part->type == 0x54585452 && part->data.size() == kept.size();
    }
    CHECK_MSG(textures, "a blast shield is the missile one with its four textures tinted");
    CHECK_MSG(Find(cmdl - 3, disc) == nullptr && Find(kShieldEnd, disc) == nullptr,
              "ids between and past the shields' are nothing");
  }
  {
    int reads = 0;
    const DiscReader disc = [&](uint32_t id, std::vector<uint8_t>& out) {
      ++reads;
      if (id == 0x2F976E86) { // Metroid.CMDL
        out = MakeCmdl(8);
        return true;
      }
      if (id == 0x27A97006) { // Node1_11.ANCS
        out = MakeAncs(0x95946E41);
        return true;
      }
      return false;
    };
    const Resource* nothing = Find(kNothingCmdl, disc);
    bool allNothing = nothing != nullptr && nothing->type == kCMDL;
    for (uint32_t i = 0; allNothing && i < 8; ++i)
      allNothing = Get32(nothing->data, 64 + 4 + 4 * i) == kNothingTxtr;
    CHECK_MSG(allNothing, "Nothing is the Metroid model with every texture replaced");
    const Resource* anim = Find(kNothingAncs, disc);
    CHECK_MSG(anim != nullptr && anim->type == kANCS && Get32(anim->data, anim->data.size() - 8) == kNothingCmdl,
              "Nothing's ANCS draws the Nothing model");
    CHECK_MSG(Find(kNothingCmdl, disc) == nothing, "a built resource is kept");
    const Resource* cog = Find(kCogCmdl, disc);
    const Resource* zoomer = Find(kZoomerCmdl, disc);
    CHECK_MSG(cog != nullptr && Get32(cog->data, 0) == 0xDEADBABE && zoomer != nullptr &&
                  Get32(zoomer->data, 0) == 0xDEADBABE,
              "the embedded Cog and Zoomer models build");
    const Resource* txtr = Find(kNothingTxtr, disc);
    CHECK_MSG(txtr != nullptr && txtr->type == 0x54585452 && !txtr->data.empty(), "the embedded textures build");
    const int readsBefore = reads;
    CHECK_MSG(Find(kThermalCmdl, disc) == nullptr && Find(kThermalCmdl, disc) == nullptr &&
                  reads == readsBefore + 1,
              "a missing disc source fails once and stays failed");
    const Resource* holorim = Find(kDoorPowerHolorimTxtr, disc);
    CHECK_MSG(holorim != nullptr && holorim->type == 0x54585452 && !holorim->data.empty() &&
                  Find(kDoorBombColorTxtr, disc) != nullptr,
              "the door shield textures build");
    CHECK_MSG(Find(0xDEAF0F00, disc) == nullptr && Find(0x12345678, disc) == nullptr,
              "unknown and non-custom ids have no resource");

    // Scan text: a SCAN and STRG pair per distinct text.
    CHECK_MSG(Utf16("Bob's \xC3\xA9\xF0\x9F\x98\x80") == u"Bob's é\U0001F600" && Utf16("a\xFF") == u"a�" &&
                  Utf16("\xE2\x82") == u"��" && Utf16("\xC0\xAF") == u"��",
              "UTF-8 becomes UTF-16, bad bytes a replacement character");
    // Keyed by location: a key keeps its id when its text changes.
    const uint32_t scanId = TextScan(1, "Archipelago item");
    CHECK_MSG(scanId == kTextBase && TextScan(2, "Energy Tank") == kTextBase + 2 &&
                  TextScan(1, "Bob's Hookshot") == scanId,
              "each key gets one SCAN id, two apart");
    const Resource* scan = Find(scanId, disc);
    CHECK_MSG(scan != nullptr && scan->type == 0x5343414E && scan->data.size() == 0xA0 &&
                  Get32(scan->data, 0) == 5 && Get32(scan->data, 12) == scanId + 1 &&
                  Get32(scan->data, 25 + 3 * 28 + 4) == 0x3F800000,
              "the SCAN is a retail-sized v5 scan pointing at its STRG");
    const Resource* strg = Find(scanId + 1, disc);
    const std::u16string text = u"Bob's Hookshot";
    bool textMatches = strg != nullptr && strg->data.size() == 32 + (text.size() + 1) * 2;
    for (size_t i = 0; textMatches && i <= text.size(); ++i) {
      const char16_t unit = char16_t((strg->data[32 + 2 * i] << 8) | strg->data[33 + 2 * i]);
      textMatches = unit == (i < text.size() ? text[i] : 0);
    }
    CHECK_MSG(textMatches && strg->type == 0x53545247 && Get32(strg->data, 0) == 0x87654321 &&
                  Get32(strg->data, 12) == 1 && Get32(strg->data, 24) == 4 + (text.size() + 1) * 2 &&
                  Get32(strg->data, 28) == 4,
              "the STRG holds the text as string 0, UTF-16BE");
    CHECK_MSG(Find(kTextBase + 100, disc) == nullptr, "an unregistered text id has no resource");

    // A scan scouted after its STRG was first built shows the new text.
    const Resource* before = Find(scanId + 1, disc);
    CHECK_MSG(TextScan(1, "Bob's Hookshot") == scanId && Find(scanId + 1, disc) == before,
              "the same text keeps the built STRG");
    CHECK_MSG(TextScan(1, "Missile") == scanId, "the key keeps its id when the text changes");
    const Resource* after = Find(scanId + 1, disc);
    const std::u16string missile = u"Missile";
    bool missileMatches = after != nullptr && after->data.size() == 32 + (missile.size() + 1) * 2;
    for (size_t i = 0; missileMatches && i <= missile.size(); ++i) {
      const char16_t unit = char16_t((after->data[32 + 2 * i] << 8) | after->data[33 + 2 * i]);
      missileMatches = unit == (i < missile.size() ? missile[i] : 0);
    }
    CHECK_MSG(missileMatches, "a changed text rebuilds the STRG");
    CHECK_MSG(before != nullptr && before->type == 0x53545247, "the old STRG stays readable for a loader holding it");
    // An id asked for before it was registered is found once it is.
    const uint32_t lateId = kTextBase + 6;
    CHECK_MSG(Find(lateId + 1, disc) == nullptr && TextScan(3, "Bombs") == kTextBase + 4 &&
                  TextScan(4, "Grapple Beam") == lateId && Find(lateId + 1, disc) != nullptr,
              "a text id asked for too early is built once registered");
  }

  if (port_test::Failures() == 0)
    std::printf("port_custom_res_tests: all passed\n");
  return PORT_TEST_RESULT();
}
