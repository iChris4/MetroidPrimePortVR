// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace PortVr {
// Consume startup inputs briefly so a launch/skip press cannot answer the
// question before it is visible. Waiting never supplies an answer.
inline int ResumePromptAnswer(unsigned ticks, bool accept, bool decline, bool mouseHeld) {
  if (ticks <= 15 || mouseHeld || accept == decline) {
    return -1;
  }
  return accept ? 1 : 0;
}
} // namespace PortVr
