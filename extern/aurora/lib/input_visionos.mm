// SPDX-License-Identifier: GPL-3.0-or-later
//
// Apple Vision Pro: which SDL gamepads are the PlayStation VR2 Sense controllers (lib/input.cpp).
// SDL's Apple driver names a GameController device after its vendorName, so a gamepad whose name
// is a connected spatial controller's is one; the VR layer reads those itself.

#import <GameController/GameController.h>

namespace aurora::input {

bool visionos_is_spatial_controller(const char* name) noexcept {
  if (name == nullptr || name[0] == '\0') {
    return false;
  }
  @autoreleasepool {
    NSString* wanted = [NSString stringWithUTF8String:name];
    if (wanted == nil) {
      return false;
    }
    for (GCController* controller in GCController.controllers) {
      if (![controller.productCategory isEqualToString:GCProductCategorySpatialController]) {
        continue;
      }
      NSString* vendor = controller.vendorName ?: controller.productCategory;
      // SDL may tidy the name it makes from the vendor name; either containing the other counts.
      if (vendor != nil && ([vendor isEqualToString:wanted] || [vendor containsString:wanted] ||
                            [wanted containsString:vendor])) {
        return true;
      }
    }
  }
  return false;
}

} // namespace aurora::input
