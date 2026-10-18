#pragma once

// Pure geometry for pointer-driven menus: where the game image sits in the
// window, and how a GUI widget's local bounds land on the EFB. No game types,
// so tests/port_pointer.cpp can check it on its own.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace PortPointerMath {

struct Rect {
  float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;

  bool Valid() const { return x1 > x0 && y1 > y0; }
  bool Contains(float x, float y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
  void Add(float x, float y) {
    x0 = std::min(x0, x);
    y0 = std::min(y0, y);
    x1 = std::max(x1, x);
    y1 = std::max(y1, y);
  }
  float Area() const { return Valid() ? (x1 - x0) * (y1 - y0) : 0.f; }
};

struct Viewport {
  float left = 0.f, top = 0.f, width = 0.f, height = 0.f;
};

// Aurora's present fit (webgpu::calculate_present_viewport): the content keeps
// its aspect, centred, on whole pixels.
inline Viewport PresentViewport(float surfaceW, float surfaceH, float contentW, float contentH) {
  if (surfaceW <= 0.f || surfaceH <= 0.f || contentW <= 0.f || contentH <= 0.f) {
    return {};
  }
  float w = surfaceW;
  float h = std::min(surfaceH, std::max(1.f, std::round(w * contentH / contentW)));
  if (h == surfaceH) {
    w = std::min(surfaceW, std::max(1.f, std::round(h * contentW / contentH)));
  }
  return {std::floor((surfaceW - w) / 2.f), std::floor((surfaceH - h) / 2.f), w, h};
}

// A surface pixel to content (EFB) coordinates. False outside the image (the
// letterbox bars); the coordinates are still written, clamped to the image.
inline bool SurfaceToContent(const Viewport& present, float contentW, float contentH, float sx,
                             float sy, float& cx, float& cy) {
  if (present.width <= 0.f || present.height <= 0.f) {
    cx = cy = 0.f;
    return false;
  }
  const float u = (sx - present.left) / present.width;
  const float v = (sy - present.top) / present.height;
  cx = std::clamp(u, 0.f, 1.f) * contentW;
  cy = std::clamp(v, 0.f, 1.f) * contentH;
  return u >= 0.f && u < 1.f && v >= 0.f && v < 1.f;
}

// CGraphics' projection state (CGraphics::SetOrtho / SetPerspective).
struct Projection {
  bool perspective = false;
  float left = -1.f, right = 1.f, top = 1.f, bottom = -1.f, znear = 0.1f, zfar = 100.f;
};

// A point in the game's eye space (x right, y forward, z up) to viewport
// pixels (top-left origin, in EFB coordinates). False behind the eye.
inline bool EyeToViewport(const Projection& proj, const Viewport& vp, float ex, float ey, float ez,
                          float& px, float& py) {
  float x = ex, z = ez;
  if (proj.perspective) {
    if (ey <= 1e-6f) {
      return false;
    }
    x = ex * proj.znear / ey;
    z = ez * proj.znear / ey;
  }
  const float w = proj.right - proj.left;
  const float h = proj.top - proj.bottom;
  if (w == 0.f || h == 0.f) {
    return false;
  }
  const float u = (x - proj.left) / w;  // 0 left .. 1 right
  const float v = (proj.top - z) / h;   // 0 top .. 1 bottom
  px = vp.left + u * vp.width;
  py = vp.top + v * vp.height;
  return true;
}

// Screen rects of the GUI widgets drawn in the last complete frame, in draw
// order. Widgets are opaque keys; each entry keeps its ancestor chain so a
// lookup by a parent (a table group, a tab) needs no live widget. A frame's
// list is complete once a draw with a newer frame number begins; lookups made
// before then (input runs before drawing) read the list being built only when
// it is from an older frame.
class HitList {
public:
  static constexpr int kMaxDepth = 8;
  static constexpr size_t kMaxEntries = 1024;

  struct Entry {
    const void* owner = nullptr;  // the CGuiFrame
    const void* chain[kMaxDepth] = {};  // the widget, then its parents
    int depth = 0;
    Rect rect;
  };

  void Add(int frameCounter, const void* owner, const void* const* chain, int depth, const Rect& rect) {
    if (frameCounter != mBuildingFrame) {
      mDone.swap(mBuilding);
      mDoneFrame = mBuildingFrame;
      mBuilding.clear();
      mBuildingFrame = frameCounter;
    }
    if (!rect.Valid() || mBuilding.size() >= kMaxEntries) {
      return;
    }
    Entry e;
    e.owner = owner;
    e.depth = std::min(depth, kMaxDepth);
    for (int i = 0; i < e.depth; ++i) {
      e.chain[i] = chain[i];
    }
    e.rect = rect;
    mBuilding.push_back(e);
  }

  // The finished list as seen at frameCounter (the frame being drawn now or
  // about to be): the list being built when it is older, else the previous.
  const std::vector< Entry >& Current(int frameCounter) const {
    if (mBuildingFrame != kNoFrame && mBuildingFrame < frameCounter) {
      return mBuilding;
    }
    return mDone;
  }
  // Only lists from the last couple of frames count: a frame not drawn since
  // (a closed screen) has no hits.
  bool Fresh(int frameCounter) const {
    const int listFrame = mBuildingFrame < frameCounter ? mBuildingFrame : mDoneFrame;
    return listFrame != kNoFrame && frameCounter - listFrame <= 2;
  }

  // The last-drawn widget of owner under the point, or null.
  const Entry* Pick(int frameCounter, const void* owner, float x, float y) const {
    if (!Fresh(frameCounter)) {
      return nullptr;
    }
    const std::vector< Entry >& list = Current(frameCounter);
    for (size_t i = list.size(); i-- > 0;) {
      const Entry& e = list[i];
      if ((owner == nullptr || e.owner == owner) && e.rect.Contains(x, y)) {
        return &e;
      }
    }
    return nullptr;
  }

  // The union of the rects drawn for widget (and, with children, everything
  // under it). False if none was drawn.
  bool Bounds(int frameCounter, const void* widget, bool children, Rect& out) const {
    out = Rect{};
    if (!Fresh(frameCounter)) {
      return false;
    }
    for (const Entry& e : Current(frameCounter)) {
      const int depth = children ? e.depth : std::min(e.depth, 1);
      for (int i = 0; i < depth; ++i) {
        if (e.chain[i] == widget) {
          out.Add(e.rect.x0, e.rect.y0);
          out.Add(e.rect.x1, e.rect.y1);
          break;
        }
      }
    }
    return out.Valid();
  }

  // The last rect drawn for widget alone (its glyphs, for a text pane, which
  // records its pane first and its text second). False if it drew none.
  bool LastBounds(int frameCounter, const void* widget, Rect& out) const {
    out = Rect{};
    if (!Fresh(frameCounter)) {
      return false;
    }
    const std::vector< Entry >& list = Current(frameCounter);
    for (size_t i = list.size(); i-- > 0;) {
      const Entry& e = list[i];
      if (e.depth > 0 && e.chain[0] == widget && e.rect.Valid()) {
        out = e.rect;
        return true;
      }
    }
    return false;
  }

  static bool Under(const Entry& e, const void* ancestor) {
    for (int i = 0; i < e.depth; ++i) {
      if (e.chain[i] == ancestor) return true;
    }
    return false;
  }

  int BuildingFrame() const { return mBuildingFrame; }
  int DoneFrame() const { return mDoneFrame; }

  void Clear() {
    mBuilding.clear();
    mDone.clear();
    mBuildingFrame = mDoneFrame = kNoFrame;
  }

private:
  static constexpr int kNoFrame = -0x7fffffff;
  std::vector< Entry > mBuilding, mDone;
  int mBuildingFrame = kNoFrame, mDoneFrame = kNoFrame;
};

} // namespace PortPointerMath
