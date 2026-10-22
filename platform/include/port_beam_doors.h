#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// "Unlock beam doors once opened" (setting beam_doors_stay_unlocked): a door
// locked to the Wave, Ice, Plasma or Power Beam stays a plain blue one, on both
// sides, once its force field was broken. Which doorways are open is one bit
// per beam doorway of the disc (PortApWorld::BeamDoorwayBit), kept per save in
// CGameState (the memory card slot has no room for it next to an Archipelago
// trailer, so nothing of it goes into the slot itself).
//
// Persistence is a small file per slot of the memory card in use, next to the
// map side files (port_beam_doors_side.cpp):
// <user folder>/mapdata/beam-doors-<card key>-slot<N>.bin. Layout, little
// endian: "BDSF", version 1, a 64-bit fingerprint, the bit count n (the highest
// set bit + 1) and ceil(n / 8) bytes of bits, bit 0 first.
//
// The fingerprint is FNV-1a over the slot buffer as written to the card (the
// bytes SGameFileSlot::PutTo writes), the same one PortMapSide uses: the file is
// used only when the slot read back hashes the same, so a slot overwritten by a
// new save, an import, a conversion or another program's card write ignores the
// file and starts with no unlocks. The slot bytes change with every save, which
// is fine because the file is rewritten at the same moment, after the card
// write succeeded. A copy of only the card file loses the unlocks (the sidecar
// stays in the user folder); a copy of both keeps them. A file left under another
// card key (a moved user folder) is still used when its fingerprint matches;
// the next save writes it under the current key.
class CStateManager;

namespace PortBeamDoors {

constexpr int kWords = 4;
constexpr size_t kMaxBits = kWords * 32;

inline bool Test(const uint32_t* words, int bit) {
  return bit >= 0 && static_cast< size_t >(bit) < kMaxBits && ((words[bit / 32] >> (bit % 32)) & 1);
}

inline void Set(uint32_t* words, int bit) {
  if (bit >= 0 && static_cast< size_t >(bit) < kMaxBits)
    words[bit / 32] |= 1u << (bit % 32);
}

// Highest set bit + 1; 0 when none is set.
inline size_t UsedBits(const uint32_t* words) {
  for (size_t bit = kMaxBits; bit > 0; --bit) {
    if (Test(words, static_cast< int >(bit - 1)))
      return bit;
  }
  return 0;
}

constexpr uint32_t kSideMagic = 0x46534442; // "BDSF" read as a little endian word
constexpr uint32_t kSideVersion = 1;
constexpr size_t kSideHeaderBytes = 20;
constexpr size_t kSideMaxBytes = kSideHeaderBytes + kMaxBits / 8;

inline void PutU32(std::vector< uint8_t >& out, uint32_t v) {
  for (int i = 0; i < 4; ++i)
    out.push_back(static_cast< uint8_t >(v >> (8 * i)));
}

inline std::vector< uint8_t > Encode(uint64_t fingerprint, const uint32_t* words) {
  const size_t used = UsedBits(words);
  std::vector< uint8_t > out;
  out.reserve(kSideMaxBytes);
  PutU32(out, kSideMagic);
  PutU32(out, kSideVersion);
  PutU32(out, static_cast< uint32_t >(fingerprint));
  PutU32(out, static_cast< uint32_t >(fingerprint >> 32));
  PutU32(out, static_cast< uint32_t >(used));
  for (size_t byte = 0; byte < (used + 7) / 8; ++byte) {
    uint8_t v = 0;
    for (int b = 0; b < 8; ++b)
      v |= Test(words, static_cast< int >(byte * 8 + b)) ? 1u << b : 0u;
    out.push_back(v);
  }
  return out;
}

// False (words untouched) for a wrong magic or version, another fingerprint, a
// bit count above kMaxBits or a truncated or longer file.
inline bool Decode(const uint8_t* bytes, size_t size, uint64_t fingerprint, uint32_t* words) {
  if (size < kSideHeaderBytes || size > kSideMaxBytes)
    return false;
  auto u32 = [bytes](size_t at) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
      v |= static_cast< uint32_t >(bytes[at + i]) << (8 * i);
    return v;
  };
  if (u32(0) != kSideMagic || u32(4) != kSideVersion)
    return false;
  if ((static_cast< uint64_t >(u32(8)) | (static_cast< uint64_t >(u32(12)) << 32)) != fingerprint)
    return false;
  const uint32_t used = u32(16);
  if (used > kMaxBits || size != kSideHeaderBytes + (used + 7) / 8)
    return false;
  uint32_t result[kWords] = {};
  for (uint32_t bit = 0; bit < used; ++bit) {
    if ((bytes[kSideHeaderBytes + bit / 8] >> (bit % 8)) & 1)
      Set(result, static_cast< int >(bit));
  }
  for (int i = 0; i < kWords; ++i)
    words[i] = result[i];
  return true;
}

#ifdef TARGET_PC
// ---- The sidecar files (port_beam_doors_side.cpp) ----

struct SlotBytes {
  const uint8_t* bytes = nullptr;
  size_t size = 0;
};

// The path of a slot's file for a card identity (PortMapSide::CardIdentity) in a
// user folder; empty when either is empty.
std::string SidePath(const std::string& userFolder, const std::string& identity, int slot);

// Where a slot's buffer is rebuilt from the live game (not yet on the card):
// remembers the doorways to write for it. All-zero bits stage a delete of the
// file instead (nothing is written for a save with no unlocks).
void Stage(int slot, const uint8_t* slotBytes, size_t size, const uint32_t* bits);
// The slot is being erased from the card.
void StageErase(int slot);
// After the card write succeeded: writes or deletes the file of each staged slot
// whose bytes in the written file are the staged ones. An erase only deletes once
// the written file really has no such slot. Returns the files touched.
int Flush(const SlotBytes* slots, int slotCount);
// After a slot was read back from the card and loaded: fills `bits` (kWords) from
// the file when it matches the slot's bytes. A missing file is the usual case and
// silent; an unreadable, wrong-version or mismatching one is logged once.
bool Apply(int slot, const uint8_t* slotBytes, size_t size, uint32_t* bits);
#endif

// ---- The running game (port_beam_doors.cpp; not in the unit tests) ----

// The setting.
bool Enabled();
// The current game's open doorways copied into `copy` (kWords words) when the
// setting is on and a game is loaded, else nullptr. For PortApWorld::Doors.
const uint32_t* Unlocked(uint32_t* copy);
// A damageable trigger of area `mrea` with this editor id went down: when the
// setting is on and it is a beam door's force field, record the doorway and
// restyle it everywhere it is loaded once the shield has faded (see OnForceFaded),
// so the shield does not flip colour while it fades out.
void OnForceBroken(CStateManager& mgr, uint32_t mrea, uint32_t editorId);
// The broken force field finished fading (its trigger and shield just went
// inactive): restyles the doorway that OnForceBroken recorded, if one is pending.
void OnForceFaded(CStateManager& mgr, uint32_t mrea, uint32_t editorId);
// Marks doorway `bit` open and restyles its loaded doors; false when it was
// already open, the bit is out of range or no game is loaded.
bool Unlock(CStateManager& mgr, int bit);
// Once per game frame, on the game thread: when the setting was toggled since
// the last call, restyles the loaded doors of every open doorway to blue (on) or
// back to their locked type (off), and refreshes the map icons. The first call
// only records the setting.
void Tick(CStateManager& mgr);
// Closes every doorway again (keeps nothing); false outside a game.
bool Reset();

} // namespace PortBeamDoors
