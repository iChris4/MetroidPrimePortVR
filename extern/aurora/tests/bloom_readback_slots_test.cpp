// The readback slots' locking, on the CPU: shutting them down while a map is still in flight is
// what used to deadlock the game on exit, and it needs no GPU to reproduce.
//
// A buffer here stands in for a wgpu::Buffer. What matters is that the last handle to a buffer
// with a map pending on it runs that map's callback, on the thread that dropped it, as Dawn does
// when a buffer with a map in flight is destroyed - and that the callback takes the slots' lock,
// which is what the deadlock was. The callback is given the slot's key and nothing else, as the
// port gives it: a pending map that held its buffer would keep it alive past the slot, and a
// callback that arrived late could then touch whatever that slot holds now.
#include "gfx/readback_slots.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <ranges>
#include <span>
#include <thread>
#include <vector>

namespace aurora::gfx::detail::testing {
namespace {

enum class MapStatus { Success, CallbackCancelled };
enum class MapState { Idle, Mapping, Mapped, Unmapped };

struct Buffer;
using Callback = std::function<void(MapStatus)>;

struct BufferState {
  int id = 0;
  std::atomic<int> refs{0};
  Callback callback; // the map callback waiting on this buffer
  std::atomic<MapState> map{MapState::Idle};
  std::atomic<int> unmaps{};
};

struct Buffer {
  Buffer() = default;
  explicit Buffer(BufferState* state) : m_state(state) {
    if (m_state != nullptr) {
      m_state->refs.fetch_add(1);
    }
  }
  Buffer(const Buffer& other) : Buffer(other.m_state) {}
  Buffer& operator=(const Buffer& other) {
    if (this != &other) {
      release();
      m_state = other.m_state;
      if (m_state != nullptr) {
        m_state->refs.fetch_add(1);
      }
    }
    return *this;
  }
  Buffer(Buffer&& other) noexcept : m_state(other.m_state) { other.m_state = nullptr; }
  Buffer& operator=(Buffer&& other) noexcept {
    if (this != &other) {
      release();
      m_state = other.m_state;
      other.m_state = nullptr;
    }
    return *this;
  }
  ~Buffer() { release(); }

  explicit operator bool() const { return m_state != nullptr; }
  int id() const { return m_state != nullptr ? m_state->id : 0; }

  // What Dawn does: a map is asked for, and it comes back later. Only the latest one is kept -
  // the tests deliver callbacks themselves, which is the point: a callback can arrive late or
  // twice, so what identifies a map is its key and not this.
  void MapAsync(Callback callback) const {
    m_state->callback = std::move(callback);
    m_state->map.store(MapState::Mapping);
  }

  // What complete_readback does with the mapped range; nothing to read when it is not mapped.
  const float* MappedRange() const {
    if (m_state->map.load() == MapState::Idle || m_state->map.load() == MapState::Unmapped) {
      return nullptr;
    }
    m_state->map.store(MapState::Mapped);
    return &m_value;
  }
  void Unmap() const {
    m_state->unmaps.fetch_add(1);
    m_state->map.store(MapState::Unmapped);
  }

private:
  void release() {
    if (m_state == nullptr) {
      return;
    }
    BufferState* state = m_state;
    m_state = nullptr;
    if (state->refs.fetch_sub(1) != 1) {
      return; // another handle is still open; nothing is destroyed
    }
    state->map.store(MapState::Unmapped);
    if (auto callback = std::move(state->callback)) {
      callback(MapStatus::CallbackCancelled);
    }
  }

  BufferState* m_state = nullptr;
  float m_value = 1.f;
};

// The device, standing in for g_device: one BufferState per buffer made, ids counting up from 1.
struct Device {
  int made = 0;
  std::vector<BufferState*> states;

  Buffer make() {
    states.push_back(new BufferState{.id = ++made});
    return Buffer(states.back());
  }
  BufferState& state(int id) { return *states[static_cast<size_t>(id) - 1]; }
  void forget() {
    for (BufferState* state : states) {
      delete state;
    }
    states.clear();
  }
};

using Slots = ReadbackSlots<Buffer, 3>;
Device g_device;
Slots g_slots([]() { return g_device.make(); });

// A map as the tests keep a note of: the key its callback carries, and which buffer it was
// issued against.
struct MapId {
  Slots::Key key;
  int buffer = 0;
};

class ReadbackSlotsTest : public ::testing::Test {
protected:
  void SetUp() override {
    g_device = {};
    g_radiance = 0.f;
    g_radianceSerial = 0;
  }
  // The slots and the device are global and outlive every test, so the buffers are all released
  // while the states behind them are still there.
  void TearDown() override {
    retire();
    m_maps.clear();
    g_device.forget();
  }

  // What bloom::encode_average does, handle dropped when it is done with it, as it is there. The
  // buffer's id, or 0 when every slot is busy and the frame's copy is skipped.
  static int claim(float exposure) {
    const Buffer buffer = g_slots.claim(exposure);
    return buffer.id(); // the handle goes when this statement ends, as it does in the port
  }

  // What bloom::after_submit does: the key is handed to the callback and the handle is let go of
  // here, so the slot's is the only one left open. The maps are kept in m_maps and handed back as
  // a span, good until the next call: m_maps grows under it.
  std::span<const MapId> map_all() {
    const size_t first = m_maps.size();
    const auto submitted = g_slots.submit();
    for (const auto& mapping : submitted.mappings) {
      if (!mapping.buffer) {
        continue;
      }
      const MapId id{mapping.key, mapping.buffer.id()};
      mapping.buffer.MapAsync([key = mapping.key](MapStatus status) { complete(key, status); });
      m_maps.push_back(id);
    }
    return {m_maps.data() + first, m_maps.size() - first};
  }

  // What bloom::complete_readback does, minus the warning.
  static bool complete(const Slots::Key& key, MapStatus status) {
    return g_slots.complete(key, [status](const Buffer& buffer, float exposure) {
      if (status != MapStatus::Success) {
        return;
      }
      const auto* texels = buffer.MappedRange();
      if (texels != nullptr && exposure > 0.f) {
        g_radiance += texels[0] / exposure; // the exposure the frame was drawn at, undone
        ++g_radianceSerial;
      }
      buffer.Unmap();
    });
  }

  // What bloom::shutdown does: the handles come out of the slots and are dropped with the lock
  // already gone.
  static void retire() {
    auto retired = g_slots.retire([] { g_radianceSerial = 0; });
    retired.clear();
  }

  std::vector<MapId> m_maps;
  static float g_radiance;
  static uint32_t g_radianceSerial;
};
float ReadbackSlotsTest::g_radiance = 0.f;
uint32_t ReadbackSlotsTest::g_radianceSerial = 0;

// Retiring under the lock hung instead of returning: dropping a handle ran the cancelled map's
// callback there and then, and the callback took that same lock. It runs on a thread of its own
// with a deadline, so a regression fails this test instead of wedging it.
TEST_F(ReadbackSlotsTest, RetiringRunsTheCancelledCallbackWithNoLockHeld) {
  ASSERT_EQ(claim(2.f), 1);
  const auto maps = map_all();
  ASSERT_EQ(maps.size(), 1u);

  std::atomic<bool> done{false};
  std::thread worker([&] {
    retire();
    done.store(true);
  });

  if (!done.load()) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done.load() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!done.load()) {
      std::fprintf(stderr,
                   "retire() never returned: the cancelled map's callback deadlocked against the\n"
                   "slots' lock. Leaving the process rather than wait for a thread that never wakes.\n");
      std::fflush(nullptr);
      std::_Exit(1);
    }
  }
  worker.join();

  // The callback ran, found the slot's buffer already retired, and read nothing.
  EXPECT_EQ(g_radiance, 0.f);
  EXPECT_EQ(g_radianceSerial, 0u);
  EXPECT_EQ(g_device.state(1).unmaps.load(), 0);
}

// A callback that turns up after its slot has been retired must leave the slot alone.
TEST_F(ReadbackSlotsTest, AStaleCallbackIsIgnored) {
  ASSERT_EQ(claim(2.f), 1);
  const MapId stale = map_all().front();

  retire();

  EXPECT_FALSE(complete(stale.key, MapStatus::Success));
  EXPECT_EQ(g_radiance, 0.f);
  EXPECT_EQ(g_radianceSerial, 0u);
}

// The same, a frame later, with every slot taken again. Reading or unmapping what a slot holds
// now would both be wrong: it is not this frame's data, and its map is still in flight.
TEST_F(ReadbackSlotsTest, AStaleCallbackLeavesAReusedSlotAlone) {
  ASSERT_EQ(claim(2.f), 1);
  const MapId stale = map_all().front();

  retire();

  ASSERT_EQ(claim(2.f), 2);
  ASSERT_EQ(claim(2.f), 3);
  ASSERT_EQ(claim(2.f), 4);
  const auto reused = map_all();
  ASSERT_EQ(reused.size(), 3u);

  // The slot the stale map names holds a new buffer, which is mapping as the stale callback lands.
  const auto same = std::ranges::find_if(reused, [&](const MapId& m) { return m.key.index == stale.key.index; });
  ASSERT_NE(same, reused.end());
  const MapId live = *same;
  EXPECT_EQ(g_device.made, 4); // three fresh ones; a retired slot's buffer is replaced
  EXPECT_NE(live.buffer, stale.buffer);
  EXPECT_EQ(g_device.state(live.buffer).map.load(), MapState::Mapping);

  EXPECT_FALSE(complete(stale.key, MapStatus::Success));
  EXPECT_EQ(g_radiance, 0.f);
  EXPECT_EQ(g_radianceSerial, 0u);
  EXPECT_EQ(g_device.state(live.buffer).unmaps.load(), 0);
  EXPECT_EQ(g_device.state(live.buffer).map.load(), MapState::Mapping);

  // And each live callback still reads its own buffer, at its own exposure.
  for (const auto& map : reused) {
    EXPECT_TRUE(complete(map.key, MapStatus::Success));
  }
  EXPECT_FLOAT_EQ(g_radiance, 1.5f);
  EXPECT_EQ(g_radianceSerial, 3u);
  EXPECT_EQ(g_device.state(live.buffer).unmaps.load(), 1);
}

// No retirement between the two: the slot keeps its buffer, and a callback that arrives late (or
// is delivered twice) must not read or unmap the map the slot is waiting on now.
TEST_F(ReadbackSlotsTest, AStaleCallbackCannotTouchTheNextUseOfTheSameBuffer) {
  ASSERT_EQ(claim(2.f), 1);
  ASSERT_EQ(claim(4.f), 2);
  ASSERT_EQ(claim(8.f), 3);
  const auto first = map_all();
  ASSERT_EQ(first.size(), 3u);
  for (const auto& map : first) {
    ASSERT_TRUE(complete(map.key, MapStatus::Success));
  }
  EXPECT_FLOAT_EQ(g_radiance, 0.5f + 0.25f + 0.125f);

  // Round robin comes back to the first slot, which keeps its buffer, with a new generation.
  ASSERT_EQ(claim(4.f), 1);
  EXPECT_EQ(g_device.made, 3);
  const MapId second = map_all().front();
  EXPECT_EQ(second.key.index, first[0].key.index);
  EXPECT_EQ(second.buffer, first[0].buffer);
  EXPECT_NE(second.key.generation, first[0].key.generation);

  // The first slot's callback, delivered again, must leave the map now in flight alone: not read
  // it, and not unmap the buffer the encoder is about to copy into.
  EXPECT_FALSE(complete(first[0].key, MapStatus::Success));
  EXPECT_FLOAT_EQ(g_radiance, 0.5f + 0.25f + 0.125f);
  EXPECT_EQ(g_radianceSerial, 3u);
  EXPECT_EQ(g_device.state(1).unmaps.load(), 1);
  EXPECT_EQ(g_device.state(1).map.load(), MapState::Mapping);

  // And the second map still reads, at its own exposure.
  EXPECT_TRUE(complete(second.key, MapStatus::Success));
  EXPECT_FLOAT_EQ(g_radiance, 0.5f + 0.25f + 0.125f + 0.25f);
  EXPECT_EQ(g_radianceSerial, 4u);
  EXPECT_EQ(g_device.state(1).unmaps.load(), 2);
  EXPECT_EQ(g_device.state(1).map.load(), MapState::Unmapped);
}

// A callback can be delivered twice. The second one has no map left to finish: its slot is
// waiting on nothing, and its buffer is not mapped, so it must not read or unmap it.
TEST_F(ReadbackSlotsTest, ADuplicateCallbackIsIgnored) {
  ASSERT_EQ(claim(2.f), 1);
  const MapId first = map_all().front();
  ASSERT_TRUE(complete(first.key, MapStatus::Success));
  EXPECT_FLOAT_EQ(g_radiance, 0.5f);

  EXPECT_FALSE(complete(first.key, MapStatus::Success));
  EXPECT_FLOAT_EQ(g_radiance, 0.5f);
  EXPECT_EQ(g_radianceSerial, 1u);
  EXPECT_EQ(g_device.state(1).unmaps.load(), 1);
}

// A map that never comes back would leave the slot MapPending for good, so a cancelled one frees
// it: the next frame can take the slot, and the buffer is mapped again afterwards.
TEST_F(ReadbackSlotsTest, ACancelledMapFreesItsSlot) {
  for (int slot = 0; slot < 3; ++slot) {
    ASSERT_EQ(claim(2.f), slot + 1);
  }
  EXPECT_EQ(claim(2.f), 0); // all three busy

  // A shutdown cancels the maps in flight and leaves the slots free.
  retire();
  EXPECT_EQ(claim(2.f), 4); // fresh buffers, as a retired slot's are gone

  const auto maps = map_all();
  ASSERT_EQ(maps.size(), 1u);
  EXPECT_TRUE(complete(maps[0].key, MapStatus::Success));
  EXPECT_FLOAT_EQ(g_radiance, 0.5f);
}

// The exposure a frame was measured at reaches the map that reads it, which is what undoes it.
TEST_F(ReadbackSlotsTest, TheExposureReachesTheMapThatReadsIt) {
  ASSERT_EQ(claim(2.f), 1);
  ASSERT_EQ(claim(4.f), 2);

  const auto maps = map_all();
  ASSERT_EQ(maps.size(), 2u);
  for (const auto& map : maps) {
    EXPECT_TRUE(complete(map.key, MapStatus::Success));
  }
  // Both frames' 1.0 texel, each divided by its own exposure.
  EXPECT_FLOAT_EQ(g_radiance, 0.5f + 0.25f);
  EXPECT_EQ(g_radianceSerial, 2u);
}

// The slots as they were: a frame's copy goes in, comes back, and the slot is free for the next.
TEST_F(ReadbackSlotsTest, ASlotIsFreeAgainOnceItsMapIsRead) {
  for (int frame = 0; frame < 3; ++frame) {
    ASSERT_EQ(claim(float(1 << frame)), frame + 1); // round robin over the slots
  }
  // No slot free, so a fourth frame's copy is skipped for a frame.
  EXPECT_EQ(claim(4.f), 0);

  const auto maps = map_all();
  ASSERT_EQ(maps.size(), 3u);
  for (const auto& map : maps) {
    EXPECT_TRUE(complete(map.key, MapStatus::Success));
  }
  EXPECT_EQ(g_radianceSerial, 3u);
  // Each frame's 1.0 texel divided by the exposure that frame was drawn at: 1 + 1/2 + 1/4.
  EXPECT_FLOAT_EQ(g_radiance, 1.75f);

  // The slots keep their buffers: back to the first slot, and to its own buffer.
  EXPECT_EQ(claim(8.f), 1);
  EXPECT_EQ(g_device.made, 3);
}

// The measure's pool (Remastered's staging pool): it grows instead of skipping a frame, and each
// claim carries an order, so a map that finishes after a newer one can be told apart and dropped.
TEST_F(ReadbackSlotsTest, AGrowingPoolNeverSkipsAndOrdersItsClaims) {
  Slots grown([]() { return g_device.make(); }, true);
  for (int frame = 0; frame < 5; ++frame) {
    ASSERT_EQ(grown.claim(1.f).id(), frame + 1); // past the three it started with
  }
  const auto submitted = grown.submit();
  ASSERT_EQ(submitted.count, 5u);
  ASSERT_EQ(submitted.mappings.size(), 5u);
  std::vector<uint64_t> orders;
  for (const auto& mapping : std::views::reverse(submitted.mappings)) {
    EXPECT_TRUE(grown.complete(mapping.key, [&](const Buffer&, float, uint64_t order) { orders.push_back(order); }));
  }
  EXPECT_EQ(orders, (std::vector<uint64_t>{5, 4, 3, 2, 1}));
  // Every slot free again: the next claim reuses one rather than growing.
  EXPECT_EQ(grown.claim(1.f).id(), 1);
  EXPECT_EQ(g_device.made, 5);
  auto retired = grown.retire([] {});
  retired.clear();
}

} // namespace
} // namespace aurora::gfx::detail::testing