#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <deque>
#include <type_traits>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

// Port extension: the slots that carry a frame's data out of a GPU buffer and back. A slot goes
// Available -> CopySubmitted (the copy is encoded) -> MapPending (the submit has happened, the
// map is asked for) -> Available (the map has come back and been read).
//
// The bookkeeping lives here, apart from the buffer type, so its locking can be tested on the
// CPU. Three things make that discipline necessary rather than stylistic:
//  - a map callback can run on any thread, and the game thread's encode, submit and shutdown
//    all touch the same slots, so every transition is under one lock;
//  - releasing a buffer with a map still in flight runs that map's callback right there on the
//    releasing thread (Dawn cancels it), so a shutdown that drops buffers under the lock
//    deadlocks against the callback's own lock: retire() hands the handles back instead, and the
//    caller releases them once it is out of the lock;
//  - that callback must name a slot without holding the buffer, or the pending map would keep a
//    buffer alive that the slot has let go of, and a callback that arrives late (or twice) could
//    then read or unmap whatever that slot holds now. So a map is identified by a Key, which
//    carries no reference, and every use of a slot takes a new generation.
// `Count` slots exist from the start. A growing set (the constructor's `grow`) adds one when every
// slot is busy instead of skipping the frame, as Remastered's staging pool does; each claim also
// takes an order, so a reader can keep the newest of several maps that finish together.
namespace aurora::gfx::detail {

enum class ReadbackState { Available, CopySubmitted, MapPending };

template <typename Buffer, size_t Count> class ReadbackSlots {
public:
  // Which slot, and which use of it. Holding no buffer, this is all a map's callback carries.
  struct Key {
    size_t index = 0;
    uint64_t generation = 0;
  };

  // A slot's buffer to map, with the key its callback carries. The buffer is the caller's to
  // release when it is done asking; the slot has one of its own all along.
  struct Mapping {
    Key key;
    Buffer buffer{};
  };

  // The slots whose copy has been submitted: `count` of them, in `mappings`. Mapping happens
  // outside the lock, as a map can complete straight away and its callback takes the lock again.
  struct Submitted {
    std::vector<Mapping> mappings;
    size_t count = 0;
  };

  // `make` builds a slot's buffer the first time that slot is used.
  explicit ReadbackSlots(Buffer (*make)(), bool grow = false) : m_make(make), m_grow(grow), m_slots(Count) {}

  // A free slot's buffer, now marked as copying into it, or an empty buffer when every slot is
  // busy and the set does not grow: the caller then skips its copy for a frame. `exposure` is kept for the map's caller.
  // A claimed slot always has a buffer, so every mapping submit() hands back has one too.
  Buffer claim(float exposure) {
    std::lock_guard lock(m_mutex);
    Buffer buffer{};
    if (m_grow && std::none_of(m_slots.begin(), m_slots.end(),
                               [](const Slot& slot) { return slot.state == ReadbackState::Available; })) {
      m_next = m_slots.size();
      m_slots.emplace_back();
    }
    const size_t count = m_slots.size();
    for (size_t i = 0; i < count && !buffer; ++i) {
      const size_t index = (m_next + i) % count;
      Slot& slot = m_slots[index];
      if (slot.state != ReadbackState::Available) {
        continue;
      }
      if (!slot.buffer) {
        slot.buffer = m_make();
      }
      // Every claim is a new use of the slot, buffer or not, so a callback left over from an
      // earlier one cannot reach this use.
      ++slot.generation;
      slot.state = ReadbackState::CopySubmitted;
      slot.exposure = exposure;
      slot.order = ++m_claims;
      buffer = slot.buffer;
      m_next = (index + 1) % count;
    }
    return buffer;
  }

  Submitted submit() {
    std::lock_guard lock(m_mutex);
    Submitted submitted;
    for (size_t index = 0; index < m_slots.size(); ++index) {
      Slot& slot = m_slots[index];
      if (slot.state != ReadbackState::CopySubmitted) {
        continue;
      }
      slot.state = ReadbackState::MapPending;
      submitted.mappings.push_back(
          Mapping{.key = Key{.index = index, .generation = slot.generation}, .buffer = slot.buffer});
      ++submitted.count;
    }
    return submitted;
  }

  // Finishes the map a key was issued for. False when the slot is not waiting on that map: it has
  // been retired, taken again since (which takes a new generation), or is not mapping at all, as
  // a repeated delivery of one callback is. A callback that fails this must not read, unmap or
  // release what the slot holds now. `underLock(buffer, exposure)` runs with the slot still busy,
  // so no frame can claim it before the caller has read and unmapped it, and the slot is free
  // again afterwards. `underLock(buffer, exposure, order)` also gets the claim's order.
  template <typename F> bool complete(const Key& key, F&& underLock) {
    std::lock_guard lock(m_mutex);
    if (key.index >= m_slots.size()) {
      return false;
    }
    Slot& slot = m_slots[key.index];
    if (slot.state != ReadbackState::MapPending || slot.generation != key.generation) {
      return false;
    }
    if constexpr (std::is_invocable_v<F, const Buffer&, float, uint64_t>) {
      std::forward<F>(underLock)(slot.buffer, slot.exposure, slot.order);
    } else {
      std::forward<F>(underLock)(slot.buffer, slot.exposure);
    }
    slot.state = ReadbackState::Available;
    return true;
  }

  // Every buffer, out of the slots and with a new generation, so the caller can release it with
  // no lock held: see the note above. `underLock` runs once the slots are retired, for state the
  // caller keeps beside them.
  template <typename F> std::vector<Buffer> retire(F&& underLock) {
    std::vector<Buffer> retired;
    std::lock_guard lock(m_mutex);
    retired.reserve(m_slots.size());
    for (Slot& slot : m_slots) {
      retired.push_back(std::move(slot.buffer));
      ++slot.generation;
      slot.state = ReadbackState::Available;
      slot.exposure = 0.f;
    }
    std::forward<F>(underLock)();
    return retired;
  }

private:
  struct Slot {
    Buffer buffer{};
    ReadbackState state = ReadbackState::Available;
    float exposure = 0.f;
    uint64_t generation = 0;
    uint64_t order = 0;
  };

  std::mutex m_mutex;
  Buffer (*m_make)() = nullptr;
  bool m_grow = false;
  // A deque, so a slot grown in never moves the others.
  std::deque<Slot> m_slots;
  size_t m_next = 0;
  uint64_t m_claims = 0;
};
} // namespace aurora::gfx::detail