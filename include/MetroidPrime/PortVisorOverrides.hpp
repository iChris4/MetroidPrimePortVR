#pragma once

// Port-only: Remastered's visor effect overrides (CVisorEffectManagerMP1, Switch 1.0.0): the HUD
// billboards whose PART carries VE* data drive the radial damage tint's three colours and its
// blend float. Pure, so it can be tested alone; the caller supplies each billboard's data.
// Update 0xd0ef2c, UpdateVisorEffectOverrides 0xd0f868, AddBillboardEffect 0xd0fc34,
// RemoveBillboardEffect 0xd0fffc, HasDamageTintOverride 0xd10b78, ctor 0xd0ea0c,
// SVisorEffectData_MP1::ExtractCurrentData 0x2e09d0.
// kb: build/mpr/kb/topic/visorfx-damage-tint-steam-overrides.md "Slot B trace".
#ifdef TARGET_PC

#include <vector>

#include "MetroidPrime/TGameTypes.hpp"

namespace PortVisorOverrides {

// ExtractCurrentData's out struct: VESC, VEMC, VETC (unclamped), VEBS and VEDT (ticks, -1 never
// expires). A generator without visor effect data is not valid.
struct Data {
  float c0[4] = {1.f, 1.f, 1.f, 1.f};
  float c1[4] = {1.f, 1.f, 1.f, 1.f};
  float c2[4] = {1.f, 1.f, 1.f, 1.f};
  float f = 1.f;
  int ticks = -1;
  bool valid = false;
};

// One registered billboard: its id, the clock when it was added and its priority (always 5).
struct Entry {
  TUniqueId id;
  float regTime;
  int priority;
};

struct State {
  std::vector< Entry > list;
  float clock = 0.f;                     // +0x110
  TUniqueId cur = kInvalidUniqueId;      // +0x114
  float dur = 0.f;                       // +0x118
  float elapsed = 0.f;                   // +0x11c
  // c0 (VESC), c1 (VEMC), c2 (VETC): from +0x120/+0x150/+0x180, target +0x130/+0x160/+0x190,
  // live +0x140/+0x170/+0x1a0. CDamageTintColor::skDefaultColor is (0,0,0,0); c2 rests at white.
  float from[3][4] = {{0.f, 0.f, 0.f, 0.f}, {0.f, 0.f, 0.f, 0.f}, {1.f, 1.f, 1.f, 1.f}};
  float target[3][4] = {{0.f, 0.f, 0.f, 0.f}, {0.f, 0.f, 0.f, 0.f}, {1.f, 1.f, 1.f, 1.f}};
  float live[3][4] = {{0.f, 0.f, 0.f, 0.f}, {0.f, 0.f, 0.f, 0.f}, {1.f, 1.f, 1.f, 1.f}};
  // VEBS: +0x1b0/+0x1b4/+0x1b8. Remastered writes the live value to PostFXManager+0xaa0, whose
  // reader is not traced, so the port only keeps it.
  float fromF = 1.f;
  float targetF = 1.f;
  float liveF = 1.f;
};

// AddBillboardEffect: append {id, clock, 5} and sort by priority. Every entry has priority 5 and
// the sort keeps equal keys in order, so the list stays in registration order.
inline void Add(State& s, TUniqueId id) {
  s.list.push_back(Entry{id, s.clock, 5});
}

// RemoveBillboardEffect: the last entry takes the removed one's slot.
inline void Remove(State& s, TUniqueId id) {
  for (size_t i = 0; i < s.list.size(); ++i) {
    if (s.list[i].id == id) {
      s.list[i] = s.list.back();
      s.list.pop_back();
      return;
    }
  }
}

inline bool HasDamageTintOverride(const State& s) {
  return s.cur != kInvalidUniqueId || s.elapsed != s.dur;
}

inline void SetTargets(State& s, const Data& d) {
  for (int i = 0; i < 4; ++i) {
    s.target[0][i] = d.c0[i];
    s.target[1][i] = d.c1[i];
    s.target[2][i] = d.c2[i];
  }
  s.targetF = d.f;
}

inline void StartBlend(State& s, float dur) {
  for (int c = 0; c < 3; ++c) {
    for (int i = 0; i < 4; ++i) {
      s.from[c][i] = s.live[c][i];
    }
  }
  s.fromF = s.liveF;
  s.dur = dur;
  s.elapsed = 0.f;
}

// UpdateVisorEffectOverrides. lookup(id, data, active) fills the billboard's data and whether its
// generator is live, and returns false when id is no HUD billboard.
template < typename Lookup >
void UpdateOverrides(State& s, float dt, bool unmorphed, Lookup&& lookup) {
  TUniqueId pick = kInvalidUniqueId;
  if (unmorphed && !s.list.empty()) {
    for (const Entry& e : s.list) {
      Data d;
      bool active = false;
      if (!lookup(e.id, d, active)) {
        continue;
      }
      const bool expired = !d.valid || d.ticks == 0
                               ? true
                               : d.ticks == -1 ? false
                                               : static_cast< float >((1.0 / 60.0) * d.ticks) <
                                                     s.clock - e.regTime;
      if (!expired && active) {
        pick = e.id;
        if (pick != s.cur) {
          StartBlend(s, 0.1f);
          SetTargets(s, d);
        }
        break;
      }
    }
  }
  if (pick == kInvalidUniqueId) {
    if (s.cur != kInvalidUniqueId) {
      StartBlend(s, 0.25f);
      Data rest;
      for (int i = 0; i < 4; ++i) {
        rest.c0[i] = rest.c1[i] = 0.f;
      }
      SetTargets(s, rest);
    }
    s.cur = kInvalidUniqueId;
    if (s.elapsed == s.dur) {
      return;
    }
  } else {
    s.cur = pick;
    Data d;
    bool active = false;
    if (lookup(pick, d, active)) {
      SetTargets(s, d);
    }
  }
  s.elapsed = s.dur - (s.elapsed + dt) < 0.f ? s.dur : s.elapsed + dt;
  const float t = s.dur != 0.f ? s.elapsed / s.dur : 1.f;
  const float u = 1.f - t;
  for (int c = 0; c < 3; ++c) {
    for (int i = 0; i < 4; ++i) {
      s.live[c][i] = s.from[c][i] * u + s.target[c][i] * t;
    }
  }
  s.liveF = s.fromF * u + s.targetF * t;
}

// Update: the clock runs while any billboard is registered and restarts at 0 when none is.
template < typename Lookup >
void Update(State& s, float dt, bool unmorphed, Lookup&& lookup) {
  if (s.list.empty()) {
    s.clock = 0.f;
  } else {
    s.clock += dt;
  }
  UpdateOverrides(s, dt, unmorphed, lookup);
}

} // namespace PortVisorOverrides

#endif
