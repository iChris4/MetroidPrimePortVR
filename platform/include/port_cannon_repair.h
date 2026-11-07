// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "port_custom_res.h"
namespace PortCannonRepair {
constexpr uint32_t kModel = 0x19ae626c, kSkin = 0x7281cdba, kSkeleton = 0x46ed31e0;
struct Result {
  PortCustomRes::Resource model, skin;
  size_t triangles = 0, seamVertices = 0;
};
// Built from the user's disc: clip the good -X side, reflect it, weld the seam and remap bones.
// Fingerprints exclude replacement models/skins/skeletons. No game geometry is bundled.
bool Build(const std::vector< uint8_t >& model, const std::vector< uint8_t >& skin,
           const std::vector< uint8_t >& skeleton, Result& result);
// Rigid suit arms retain their original animated elbow-locator attachment.
bool BuildArm(uint32_t id, const std::vector< uint8_t >& model, Result& result);
bool IsResource(uint32_t id);
const PortCustomRes::Resource* Find(uint32_t id, const PortCustomRes::DiscReader& read);
void Reset();
} // namespace PortCannonRepair
