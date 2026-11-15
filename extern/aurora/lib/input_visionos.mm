// SPDX-License-Identifier: GPL-3.0-or-later
//
// Apple Vision Pro: which SDL gamepads are the VR layer's controllers (lib/input.cpp). SDL's Apple
// driver names a GameController device after its vendorName, so a gamepad whose name is such a
// device's is one; the VR layer reads those itself. They are the PlayStation VR2 Sense controllers
// (spatial controllers), and a combined Joy-Con pair while the visionOS OpenXR provider plays it as
// two hand-tracked controllers (aurora_visionos_set_joycons_claimed).

#include "aurora/aurora.h"

#import <GameController/GameController.h>

#include <atomic>

namespace aurora::input {
namespace {

std::atomic_bool g_joyconsClaimed{false};

NSString* const kJoyConCategoryPrefix = @"Nintendo Switch Joy-Con";

// Whether `wanted` (an SDL gamepad name) names a connected GameController device that `pick` accepts.
template <typename Pick>
bool names_controller(const char* name, Pick pick) noexcept {
  if (name == nullptr || name[0] == '\0') {
    return false;
  }
  @autoreleasepool {
    NSString* wanted = [NSString stringWithUTF8String:name];
    if (wanted == nil) {
      return false;
    }
    for (GCController* controller in GCController.controllers) {
      if (!pick(controller)) {
        continue;
      }
      // SDL may tidy the name it makes from the vendor name; either containing the other counts.
      for (NSString* candidate in @[ controller.vendorName ?: @"", controller.productCategory ?: @"" ]) {
        if (candidate.length != 0 && ([candidate isEqualToString:wanted] || [candidate containsString:wanted] ||
                                      [wanted containsString:candidate])) {
          return true;
        }
      }
    }
  }
  return false;
}

} // namespace

bool visionos_is_spatial_controller(const char* name) noexcept {
  return names_controller(name, [](GCController* controller) {
    return [controller.productCategory isEqualToString:GCProductCategorySpatialController];
  });
}

bool visionos_joycons_claimed() noexcept { return g_joyconsClaimed.load(); }

bool visionos_is_claimed_joycon(const char* name) noexcept {
  if (!g_joyconsClaimed.load()) {
    return false;
  }
  // The pair is what the VR layer claims; any Joy-Con counts while it does, so that the halves
  // never show up a second time as gamepads of their own.
  return names_controller(
      name, [](GCController* controller) { return [controller.productCategory hasPrefix:kJoyConCategoryPrefix]; });
}

} // namespace aurora::input

void aurora_visionos_set_joycons_claimed(bool claimed) { aurora::input::g_joyconsClaimed.store(claimed); }
