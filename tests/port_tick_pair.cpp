#include "port_tick_pair.h"
#include "port_test.h"

#include <cstdio>
#include <cstdlib>

int main() {
  // The first value drawn in each generation is kept; a blend needs the
  // previous generation's value.
  {
    PortTickPair< int > p;
    REQUIRE(!p.Note(10, 0));
    REQUIRE(!p.Note(10, 1));
    REQUIRE(!p.Valid(1));
    REQUIRE(p.Note(20, 2));
    REQUIRE(p.prev == 10 && p.cur == 20);
    // Later draws in the same generation don't overwrite it.
    REQUIRE(p.Note(99, 2));
    REQUIRE(p.prev == 10 && p.cur == 20);
    REQUIRE(p.Note(30, 3));
    REQUIRE(p.prev == 20 && p.cur == 30);
  }
  // A skipped generation (not drawn for a tick) snaps instead of blending
  // across two ticks.
  {
    PortTickPair< int > p;
    p.Note(1, 5);
    REQUIRE(p.Note(2, 6));
    REQUIRE(!p.Note(3, 8));
    REQUIRE(p.cur == 3);
    REQUIRE(p.Note(4, 9));
    REQUIRE(p.prev == 3 && p.cur == 4);
  }
  // Valid() only answers for the generation the pair was last noted in.
  {
    PortTickPair< int > p;
    p.Note(1, 1);
    p.Note(2, 2);
    REQUIRE(p.Valid(2));
    REQUIRE(!p.Valid(3));
    p.Reset();
    REQUIRE(!p.Valid(2));
    REQUIRE(!p.Note(5, 3));
  }
  // Types without a default constructor pass a start value.
  {
    struct NoDefault {
      explicit NoDefault(int v) : value(v) {}
      int value;
    };
    PortTickPair< NoDefault > p(NoDefault(7));
    REQUIRE(p.prev.value == 7 && p.cur.value == 7);
    p.Note(NoDefault(1), 1);
    REQUIRE(p.Note(NoDefault(2), 2));
    REQUIRE(p.prev.value == 1 && p.cur.value == 2);
  }
  std::puts("port_tick_pair_tests passed");
  return 0;
}
