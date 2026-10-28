#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace PortQueenBarrier {
bool ValidatePart(const std::vector<uint8_t>& part, uint32_t feature,
                  const std::vector<std::array<uint8_t, 16>>& realVars,
                  const std::function<bool(uint32_t, uint32_t)>& dependency, std::string& error);
}
