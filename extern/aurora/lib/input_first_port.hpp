#pragma once

#include <cstddef>
#include <cstdint>

// Port: who holds the first pad port (player 1) after the saved port preferences were applied.
//
// The Android touch overlay is an SDL virtual gamepad, attached on the first touch. Once it holds
// player 1, a real pad connected later gets player 2, which a single-player game never reads. So a
// real pad takes player 1 from a fallback pad (a virtual one, or the Steam Deck's built-in
// controller, which ranks with them), and a fallback pad fills player 1 whenever it is empty.
//
// A saved choice for player 1 only says which real pad is wanted there: while that pad is away
// another free pad stands in (the saved one takes the port back when it reconnects), and a saved
// fallback pad counts as no choice (#120: picking the touch pad, or a pad since unplugged, left
// player 1 without a controller until it was picked again). "None" keeps real pads off the port;
// a fallback pad still fills it, as the touch overlay has no other way in. A pad the user put on
// another port is never taken from it.
namespace aurora::input {

struct FirstPortCandidate {
  uint32_t id = 0;       // orders the candidates (SDL instance id): the lowest wins
  int32_t port = -1;     // SDL player index, negative for none
  bool fallback = false; // virtual pad or the Deck's built-in controller
  bool assigned = false; // on a port (other than the first) the user configured
};

struct FirstPortMove {
  int take = -1; // index of the candidate that gets the first port, or -1 for no change
  int give = -1; // index of the candidate holding it now (it gets `take`'s old port), or -1
};

inline FirstPortMove choose_first_port(const FirstPortCandidate* pads, size_t count, bool firstPortIsNone) noexcept {
  int first = -1;
  int real = -1;
  int virt = -1;
  for (size_t i = 0; i < count; ++i) {
    const auto& pad = pads[i];
    if (pad.port == 0) {
      first = static_cast<int>(i);
      continue;
    }
    if (pad.assigned) {
      continue;
    }
    int& slot = pad.fallback ? virt : real;
    if (slot < 0 || pad.id < pads[slot].id) {
      slot = static_cast<int>(i);
    }
  }

  if (first >= 0 && !pads[first].fallback) {
    return {}; // a real pad on the first port stays
  }
  if (real >= 0 && !firstPortIsNone) {
    return {real, first};
  }
  if (first < 0 && virt >= 0) {
    return {virt, -1};
  }
  return {};
}

// Port: where the pad that held port 0 goes when the pad just used takes it (single-player:
// every pad plays). It gets the taker's old port, else the first empty one of 1-3, else none.
inline int32_t displaced_port(int32_t takerPort, const bool occupied[4]) noexcept {
  if (takerPort > 0 && takerPort < 4) return takerPort;
  for (int32_t port = 1; port < 4; ++port) {
    if (!occupied[port]) return port;
  }
  return -1;
}
} // namespace aurora::input
