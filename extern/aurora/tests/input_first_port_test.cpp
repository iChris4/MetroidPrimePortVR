#include "../lib/input_first_port.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace {

using aurora::input::choose_first_port;
using aurora::input::FirstPortCandidate;
using aurora::input::FirstPortMove;

constexpr bool kFallback = true;
constexpr bool kReal = false;

FirstPortCandidate Pad(uint32_t id, int32_t port, bool fallback, bool assigned = false) {
  FirstPortCandidate pad;
  pad.id = id;
  pad.port = port;
  pad.fallback = fallback;
  pad.assigned = assigned;
  return pad;
}

FirstPortMove Choose(const std::vector<FirstPortCandidate>& pads, bool none = false) {
  return choose_first_port(pads.data(), pads.size(), none);
}

TEST(InputFirstPort, NoPadsNoMove) { EXPECT_EQ(Choose({}).take, -1); }

TEST(InputFirstPort, RealPadTakesThePortFromTheTouchPad) {
  // SDL gave the pad connected second player 2.
  const FirstPortMove move = Choose({Pad(33, 0, kFallback), Pad(34, 1, kReal)});
  EXPECT_EQ(move.take, 1);
  EXPECT_EQ(move.give, 0);
}

TEST(InputFirstPort, RealPadWithoutAPortTakesItFromTheTouchPad) {
  // #120: the touch pad was the saved choice for player 1, so the preferences left the real pad
  // without a port.
  const FirstPortMove move = Choose({Pad(33, 0, kFallback), Pad(34, -1, kReal)});
  EXPECT_EQ(move.take, 1);
  EXPECT_EQ(move.give, 0);
}

TEST(InputFirstPort, TouchPadFillsAnEmptyPort) {
  // #120: the saved pad was unplugged, and the preferences took the touch pad off its port.
  const FirstPortMove move = Choose({Pad(33, -1, kFallback)});
  EXPECT_EQ(move.take, 0);
  EXPECT_EQ(move.give, -1);
}

TEST(InputFirstPort, RealPadOnThePortStays) {
  EXPECT_EQ(Choose({Pad(34, 0, kReal), Pad(35, 1, kReal), Pad(33, -1, kFallback)}).take, -1);
}

TEST(InputFirstPort, TouchPadOnThePortStaysWithoutARealPad) {
  EXPECT_EQ(Choose({Pad(33, 0, kFallback), Pad(40, 1, kFallback)}).take, -1);
}

TEST(InputFirstPort, AnotherRealPadStandsInForTheSavedOne) {
  const FirstPortMove move = Choose({Pad(35, -1, kReal)});
  EXPECT_EQ(move.take, 0);
  EXPECT_EQ(move.give, -1);
}

TEST(InputFirstPort, RealPadBeatsTheTouchPadForAnEmptyPort) {
  const FirstPortMove move = Choose({Pad(33, -1, kFallback), Pad(34, -1, kReal)});
  EXPECT_EQ(move.take, 1);
  EXPECT_EQ(move.give, -1);
}

TEST(InputFirstPort, PadAssignedToAnotherPlayerIsNotTaken) {
  EXPECT_EQ(Choose({Pad(33, 0, kFallback), Pad(34, 1, kReal, true)}).take, -1);
  // ...and with player 1 empty the touch pad fills it, not player 2's pad.
  const FirstPortMove move = Choose({Pad(33, -1, kFallback), Pad(34, 1, kReal, true)});
  EXPECT_EQ(move.take, 0);
  EXPECT_EQ(move.give, -1);
}

TEST(InputFirstPort, NoneKeepsRealPadsOffButNotTheTouchPad) {
  EXPECT_EQ(Choose({Pad(34, -1, kReal)}, true).take, -1);
  EXPECT_EQ(Choose({Pad(33, 0, kFallback), Pad(34, -1, kReal)}, true).take, -1);
  const FirstPortMove move = Choose({Pad(34, -1, kReal), Pad(33, -1, kFallback)}, true);
  EXPECT_EQ(move.take, 1);
  EXPECT_EQ(move.give, -1);
}

TEST(InputFirstPort, LowestIdWinsWhateverTheOrder) {
  EXPECT_EQ(Choose({Pad(36, -1, kReal), Pad(34, -1, kReal), Pad(35, -1, kReal)}).take, 1);
  EXPECT_EQ(Choose({Pad(41, -1, kFallback), Pad(40, -1, kFallback)}).take, 1);
}

TEST(InputFirstPort, DisplacedPadTakesTheTakersPortOrTheFirstEmptyOne) {
  const bool none[4] = {true, false, false, false};
  const bool second[4] = {true, true, false, false};
  const bool full[4] = {true, true, true, true};
  EXPECT_EQ(aurora::input::displaced_port(2, second), 2);
  EXPECT_EQ(aurora::input::displaced_port(-1, none), 1);
  EXPECT_EQ(aurora::input::displaced_port(-1, second), 2);
  EXPECT_EQ(aurora::input::displaced_port(-1, full), -1);
}

} // namespace
