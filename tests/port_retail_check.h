#pragma once

// Every converter test reads what it converts back through the retail reader: a part the
// reader rejects (a bare-float CNST, a bad port-only layout) never loads in the game, whatever
// bytes the test looked for in it.

#include <string>
#include <vector>

#include "port_remastered_effect_convert.h"
#include "port_test.h"

namespace PortRemastered {

inline std::vector<ConvertedPart> CheckedConvert(const EffectNode& effect, const uint8_t* data) {
  std::vector<ConvertedPart> parts = ConvertEffect(effect, data, {});
  for (const ConvertedPart& part : parts) {
    std::vector<RetailPartProperty> properties;
    std::string error;
    CHECK_MSG(SplitRetailEffect(part.type, part.part.data(), part.part.size(), properties, error),
              ("a converted part reads as retail: " + error).c_str());
  }
  return parts;
}

}  // namespace PortRemastered
