// A mod's bar shapes for a HUD frame, by frame. See port_hud_bars.h.
#include "port_hud_bars.h"
#include "port_log.h"

#include "port_mods.h"

#include <cstdio>
#include <unordered_map>

namespace PortHudBars {
namespace {

// Null for a frame no mod has bars for, so its file is looked for once.
std::unordered_map<uint32_t, std::shared_ptr<const Bars>> sFrames;

std::shared_ptr<const Bars> Load(uint32_t frame) {
  const std::string path = PortMods::HudBarsPath(frame);
  if (path.empty()) {
    return nullptr;
  }
  std::vector<uint8_t> data;
  if (std::FILE* file = std::fopen(path.c_str(), "rb")) {
    uint8_t chunk[16384];
    size_t got;
    while ((got = std::fread(chunk, 1, sizeof(chunk), file)) > 0) {
      data.insert(data.end(), chunk, chunk + got);
    }
    std::fclose(file);
  }
  auto bars = std::make_shared<Bars>();
  if (!ParseFile(data.data(), data.size(), *bars)) {
    PortLog::Write("mods: %s is not a hud bars file\n", path.c_str());
    return nullptr;
  }
  return bars;
}

} // namespace

std::shared_ptr<const Bars> ForFrame(uint32_t frame) {
  const auto found = sFrames.find(frame);
  if (found != sFrames.end()) {
    return found->second;
  }
  return sFrames[frame] = Load(frame);
}

void Reset() { sFrames.clear(); }

} // namespace PortHudBars
