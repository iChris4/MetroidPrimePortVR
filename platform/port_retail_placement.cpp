#include "port_retail_placement.h"

#include "port_log.h"
#include "port_mods.h"
#include "port_strings.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <tuple>

namespace PortRetailPlacement {
namespace {

bool sLoaded = false;
std::string sPath;
std::map<std::tuple<uint32_t, uint32_t, uint32_t>, std::array<float, 12>> sByObject;
std::set<uint32_t> sModels;

void Load() {
  const std::string path = PortMods::RetailPlacementPath();
  if (sLoaded && path == sPath) {
    return;
  }
  sLoaded = true;
  sPath = path;
  sByObject.clear();
  sModels.clear();
  if (path.empty()) {
    return;
  }
  std::ifstream in(std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::binary);
  const std::vector<uint8_t> bytes = port::ReadAll(in);
  std::vector<Record> records;
  if (!Decode(bytes.data(), bytes.size(), records)) {
    PortLog::Write("retail placements: %s is not a version %u file; ignored\n", path.c_str(), kFileVersion);
    return;
  }
  for (const Record& r : records) {
    sByObject[{r.mrea, r.editorId & kEditorIdMask, r.model}] = r.correction;
    sModels.insert(r.model);
  }
  PortLog::Write("retail placements: %zu objects, %zu models\n", sByObject.size(), sModels.size());
}

}  // namespace

bool Find(uint32_t mrea, uint32_t editorId, uint32_t model, float correction[12]) {
  Load();
  const auto found = sByObject.find({mrea, editorId & kEditorIdMask, model});
  if (found == sByObject.end()) {
    // A model converted in model space draws misplaced without its object's placement.
    static std::set<uint32_t> sReported;
    if (sModels.count(model) != 0 && sReported.insert(model).second) {
      PortLog::Write("retail placements: model %08X has no placement for object %08X in %08X\n", model,
                     editorId, mrea);
    }
    return false;
  }
  std::copy(found->second.begin(), found->second.end(), correction);
  return true;
}

bool Placed(uint32_t model) {
  Load();
  return sModels.count(model) != 0;
}

}  // namespace PortRetailPlacement
