#pragma once

// Port-only PKYF (Remastered's KEYF, CREKeyframeInput ctor 0x296490, GetValue 0x29661c): keys read
// at an input element's value instead of the age, wrapped from the loop start once past the last
// key when looping, and blended linearly between neighbours. D is 1 / R, divided once. Header
// only, so a test can link it alone.

#include "Kyoto/Particles/CRealElement.hpp"

#include <utility>
#include <vector>

class CREPortKeyframeInput : public CRealElement {
  bool x4_loop;
  int x8_count;
  float xc_loopStart;
  float x10_start;
  float x14_rate;
  float x18_step;
  std::vector< float > x1c_keys;
  CRealElement* x20_input;

public:
  CREPortKeyframeInput(bool loop, int count, float loopStart, float start, float rate,
                       std::vector< float > keys, CRealElement* input)
  : x4_loop(loop)
  , x8_count(count)
  , xc_loopStart(loopStart)
  , x10_start(start)
  , x14_rate(rate)
  , x18_step(1.f / rate)
  , x1c_keys(std::move(keys))
  , x20_input(input) {}
  ~CREPortKeyframeInput() override { delete x20_input; }

  bool GetValue(int frame, float& valOut) const override {
    float t = 0.f;
    if (x20_input) {
      x20_input->GetValue(frame, t);
    }
    valOut = Evaluate(t);
    return false;
  }

  // The value for the input t. Each product and sum is its own statement: the original does not
  // fuse them.
  float Evaluate(float t) const {
    if (x8_count <= 0 || x1c_keys.size() < static_cast< size_t >(x8_count)) {
      return 0.f;
    }
    const int last = x8_count - 1;
    if (x4_loop) {
      const float lastStep = x18_step * static_cast< float >(last);
      const float end = x10_start + lastStep;
      if (t >= end) {
        const float span = end - xc_loopStart;
        t = t - end;
        // An empty loop span would never leave the loop; the original hangs there.
        while (t >= span && span > 0.f) {
          t = t - span;
        }
        t = xc_loopStart + t;
      }
    }
    const float u = t - x10_start;
    const float scaled = x14_rate * u;
    // fcvtzs: truncation that saturates, NaN is 0.
    int index = 0;
    if (scaled >= 2147483648.f) {
      index = 0x7fffffff;
    } else if (scaled > -2147483648.f) {
      index = static_cast< int >(scaled);
    } else if (scaled == scaled) {
      index = static_cast< int >(0x80000000u);
    }
    int k = x8_count > index ? index : last;
    if (index < 0) {
      k = 0;
    }
    if (k < 0 || last <= k) {
      return x1c_keys[k];
    }
    const float stepK = x18_step * static_cast< float >(k);
    const float rest = u - stepK;
    float f = x14_rate * rest;
    if (0.f - f >= 0.f) {
      f = 0.f;
    }
    if (f + -1.f >= 0.f) {
      f = 1.f;
    }
    const float below = x1c_keys[k] * (1.f - f);
    const float above = x1c_keys[k + 1] * f;
    return above + below;
  }
};
