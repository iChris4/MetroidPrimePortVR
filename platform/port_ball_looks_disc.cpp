// The generated ball looks' sources and cache (port_ball_looks.h): reads the
// retail models and textures from the disc, makes a look once, and keeps it in
// <user folder>/generated/<look>/ under a stamp of the generator version and
// the disc, so a later boot only reads the files.

#include "port_ball_looks.h"

#include "port_disc.h"
#include "port_disc_reader.h"
#include "port_log.h"
#include "port_model_variant.h"
#include "port_paths.h"
#include "port_strings.h"
#include "port_custom_res.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

namespace PortBallLooks {
namespace {

constexpr uint32_t kTXTR = 0x54585452;
constexpr uint32_t kCMDL = 0x434D444C;
constexpr uint32_t kANCS = 0x414E4353;
constexpr const char* kStampName = "stamp.txt";

std::string TypeName(uint32_t type) {
  return {char(type >> 24), char(type >> 16), char(type >> 8), char(type)};
}

std::string FileName(uint32_t type, uint32_t id) {
  char name[16];
  std::snprintf(name, sizeof(name), "%08X.", id);
  return name + TypeName(type);
}

struct Output {
  uint32_t type = 0;
  uint32_t id = 0;
  std::vector<uint8_t> data;
};

// What the player sees when a look cannot be made.
const char* Fallback(Look look) {
  switch (look) {
  case Look::PowerSpider: return "the Power Suit keeps the Gravity Suit's spider ball";
  case Look::Gravity: return "the Gravity Suit keeps the Power Suit's ball";
  case Look::Phazon: return "the Phazon Suit keeps the Power Suit's ball";
  }
  return "";
}

// The cache folder under generated/.
const char* Folder(Look look) {
  switch (look) {
  case Look::PowerSpider: return "power-spider";
  case Look::Gravity: return "gravity-ball";
  case Look::Phazon: return "phazon-ball";
  }
  return "";
}

// Logs why, returns false.
bool Fail(Look look, const std::string& why) {
  PortLog::Write("%s: %s; %s\n", Name(look), why.c_str(), Fallback(look));
  return false;
}

bool ReadImage(PortDiscReader& disc, uint32_t id, PortRemastered::Image& image) {
  std::vector<uint8_t> txtr;
  return disc.Read(kTXTR, id, txtr) && DecodeCmprTxtr(txtr, image);
}

// The resources the cache holds, in a fixed order.
std::vector<std::pair<uint32_t, uint32_t>> OutputKeys(Look look) {
  const uint32_t suit = uint32_t(Suit(look));
  if (look == Look::PowerSpider) {
    return {{kTXTR, PortModelVariant::Id(kSpiderVaria, suit)}, {kTXTR, PortModelVariant::Id(kLowPolyVaria, suit)},
            {kCMDL, PortModelVariant::Id(kSpiderCmdl, suit)},  {kCMDL, PortModelVariant::Id(kLowPolyCmdl, suit)},
            {kCMDL, PortModelVariant::Id(kGlassCmdl, suit)},   {kANCS, PortModelVariant::Id(kSpiderAncs, suit)}};
  }
  return {{kTXTR, PortModelVariant::Id(kBallVaria, suit)},
          {kCMDL, PortModelVariant::Id(kBallCmdl, suit)},
          {kCMDL, PortModelVariant::Id(kBallLowPolyCmdl, suit)},
          {kANCS, PortModelVariant::Id(kBallAncs, suit)}};
}

void LogShift(Look look, const char* what, const Shift& shift) {
  PortLog::Write("%s: %s is hue %+.1f, saturation %+.3f, value %+.3f (%zu texels)\n", Name(look), what, shift.hue,
                 shift.saturation, shift.value, shift.samples);
}

// The move from Varia's colours to the look's suit.
bool FitLook(Look look, PortDiscReader& disc, Shift& shift) {
  switch (look) {
  case Look::PowerSpider: {
    PortRemastered::Image ballPower, ballVaria;
    if (!ReadImage(disc, kBallPower, ballPower) || !ReadImage(disc, kBallVaria, ballVaria)) {
      return Fail(look, "the normal ball's textures are not on the disc");
    }
    shift = FitShift(ballVaria, ballPower);
    if (shift.samples == 0) {
      return Fail(look, "the Varia and Power balls do not differ");
    }
    LogShift(look, "Varia to Power", shift);
    return true;
  }
  case Look::Gravity:
  case Look::Phazon: {
    PortRemastered::Image spiderVaria, spiderGravity;
    if (!ReadImage(disc, kSpiderVaria, spiderVaria) || !ReadImage(disc, kSpiderGravity, spiderGravity)) {
      return Fail(look, "the spider ball's textures are not on the disc");
    }
    if (look == Look::Gravity) {
      shift = FitShift(spiderVaria, spiderGravity);
      if (shift.samples == 0) {
        return Fail(look, "the Varia and Gravity spider balls do not differ");
      }
      LogShift(look, "Varia to Gravity", shift);
      return true;
    }
    // Phazon's spider ball is another model with other UVs: compare Varia's
    // suit-coloured parts with the Phazon shell as a whole.
    PortRemastered::Image phazon;
    if (!ReadImage(disc, kPhazonShell, phazon)) {
      return Fail(look, "the Phazon ball's shell " + FileName(kTXTR, kPhazonShell) + " is not on the disc");
    }
    const Stats from = ColourStats(spiderVaria, &spiderGravity);
    const Stats to = ColourStats(phazon);
    shift = StatsShift(from, to);
    if (shift.samples == 0) {
      return Fail(look, "the Varia and Phazon spider balls have no colours to compare");
    }
    PortLog::Write("%s: Varia's region is hue %.1f, saturation %.3f, value %.3f (%zu, %zu texels); Phazon's "
                   "is hue %.1f, saturation %.3f, value %.3f (%zu, %zu texels)\n",
                   Name(look), from.hue, from.saturation, from.value, from.samples, from.hueSamples, to.hue,
                   to.saturation, to.value, to.samples, to.hueSamples);
    LogShift(look, "Varia to Phazon", shift);
    return true;
  }
  }
  return false;
}

bool Generate(Look look, std::vector<Output>& outputs) {
  PortDiscReader disc;
  disc.Index();
  const uint32_t suit = uint32_t(Suit(look));

  // The new ids must be no retail resource's (PortModelVariant).
  std::unordered_set<uint32_t> ids;
  for (const auto& [type, id] : OutputKeys(look)) {
    if (disc.HasId(id) || !ids.insert(id).second) {
      return Fail(look, "id " + FileName(type, id) + " is taken");
    }
  }

  Shift shift;
  if (!FitLook(look, disc, shift)) {
    return false;
  }

  // Each recoloured texture against the look it keeps apart from, and the
  // models whose Varia set (1) becomes the look's set 0 with it.
  struct Texture {
    uint32_t varia, other;
  };
  std::vector<Texture> textures;
  std::vector<uint32_t> models;
  uint32_t ancs = 0, ancsModel = 0;
  if (look == Look::PowerSpider) {
    textures = {{kSpiderVaria, kSpiderGravity}, {kLowPolyVaria, kLowPolyGravity}};
    models = {kSpiderCmdl, kLowPolyCmdl, kGlassCmdl};
    ancs = kSpiderAncs, ancsModel = kSpiderCmdl;
  } else {
    textures = {{kBallVaria, kBallPower}};
    models = {kBallCmdl, kBallLowPolyCmdl};
    ancs = kBallAncs, ancsModel = kBallCmdl;
  }

  std::map<uint32_t, uint32_t> swap;
  for (const Texture& texture : textures) {
    PortRemastered::Image image, other;
    if (!ReadImage(disc, texture.varia, image) || !ReadImage(disc, texture.other, other)) {
      return Fail(look, "texture " + FileName(kTXTR, texture.varia) + " or " + FileName(kTXTR, texture.other) +
                            " is missing");
    }
    const size_t changed = Recolour(image, other, shift);
    PortLog::Write("%s: %s: %zu of %d texels recoloured\n", Name(look), FileName(kTXTR, texture.varia).c_str(),
                   changed, image.width * image.height);
    swap.emplace(texture.varia, PortModelVariant::Id(texture.varia, suit));
    outputs.push_back({kTXTR, PortModelVariant::Id(texture.varia, suit), PortRemastered::EncodeTxtrCmpr(image, true)});
  }

  for (const uint32_t cmdl : models) {
    std::vector<uint8_t> model;
    if (!disc.Read(kCMDL, cmdl, model) || !CopyMaterialSet(model, 1, 0, swap)) {
      return Fail(look, "model " + FileName(kCMDL, cmdl) + " is missing or not as expected");
    }
    outputs.push_back({kCMDL, PortModelVariant::Id(cmdl, suit), std::move(model)});
  }

  std::vector<uint8_t> character;
  if (!disc.Read(kANCS, ancs, character) ||
      !PortCustomRes::SetAncsModel(character, ancsModel, PortModelVariant::Id(ancsModel, suit))) {
    return Fail(look, "character " + FileName(kANCS, ancs) + " is missing or not as expected");
  }
  outputs.push_back({kANCS, PortModelVariant::Id(ancs, suit), std::move(character)});
  return true;
}

}  // namespace

std::vector<PortMods::LooseResource> Collect(Look look) {
  const std::string& user = PortPaths::UserFolder();
  if (user.empty()) {
    return {};
  }
  const fs::path dir = PortPaths::detail::FromUtf8(user) / "generated" / Folder(look);
  const std::string stamp = std::string(Folder(look)) + " " + std::to_string(kVersion) + " " +
                            PortDisc::Name(PortDisc::Current()) + "\n";
  std::error_code ec;
  const auto cached = [&]() {
    std::vector<PortMods::LooseResource> found;
    for (const auto& [type, id] : OutputKeys(look)) {
      const fs::path file = dir / FileName(type, id);
      const uintmax_t size = fs::file_size(file, ec);
      if (ec || size == 0) {
        return std::vector<PortMods::LooseResource>{};
      }
      found.push_back({type, id, PortPaths::detail::ToUtf8(file), size, "generated"});
    }
    return found;
  };
  {
    std::ifstream in(dir / kStampName);
    if (in.is_open() && port::ReadAllText(in) == stamp) {
      std::vector<PortMods::LooseResource> found = cached();
      if (!found.empty()) {
        PortLog::Write("%s: from the cache in %s\n", Name(look), PortPaths::detail::ToUtf8(dir).c_str());
        return found;
      }
    }
  }

  std::vector<Output> outputs;
  if (!Generate(look, outputs)) {
    return {};
  }
  fs::remove(dir / kStampName, ec);
  fs::create_directories(dir, ec);
  // Each file goes in whole (written aside, then renamed), and the stamp last,
  // so an interrupted write leaves no stamp and is made again.
  const auto put = [&](const std::string& name, const char* data, size_t size) {
    const fs::path file = dir / name;
    fs::path temporary = file;
    temporary += ".tmp";
    {
      std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
      out.write(data, std::streamsize(size));
      out.close();
      if (!out) {
        fs::remove(temporary, ec);
        return Fail(look, "cannot write " + PortPaths::detail::ToUtf8(file));
      }
    }
    fs::rename(temporary, file, ec);
    if (ec) {
      fs::remove(temporary, ec);
      return Fail(look, "cannot write " + PortPaths::detail::ToUtf8(file));
    }
    return true;
  };
  for (const Output& output : outputs) {
    if (!put(FileName(output.type, output.id), reinterpret_cast<const char*>(output.data.data()),
             output.data.size())) {
      return {};
    }
  }
  if (!put(kStampName, stamp.data(), stamp.size())) {
    return {};
  }
  PortLog::Write("%s: made in %s\n", Name(look), PortPaths::detail::ToUtf8(dir).c_str());
  return cached();
}

}  // namespace PortBallLooks
