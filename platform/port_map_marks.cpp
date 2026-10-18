#include "port_map_marks.h"

#include "MetroidPrime/CMemoryCard.hpp"
#include "MetroidPrime/Player/CGameState.hpp"

namespace PortMapMarks {

bool WorldIndex(uint32_t worldId, unsigned& index) {
  if (gpMemoryCard == nullptr) {
    return false;
  }
  const rstl::vector< CMemoryCard::MemoryWorld >& worlds = gpMemoryCard->GetMemoryWorlds();
  for (int i = 0; i < worlds.size(); ++i) {
    if (worlds[i].first == worldId) {
      index = static_cast< unsigned >(i);
      return true;
    }
  }
  return false;
}

const List* Current() { return gpGameState != nullptr ? &gpGameState->PortMapMarkList() : nullptr; }

bool IsMarked(uint32_t worldId, int area) {
  unsigned world = 0;
  if (gpGameState == nullptr || area < 0 || !WorldIndex(worldId, world) ||
      !Valid(world, static_cast< unsigned >(area))) {
    return false;
  }
  const List& list = gpGameState->PortMapMarkList();
  return list.count != 0 && list.Has(Pack(world, static_cast< unsigned >(area)));
}

int Toggle(uint32_t worldId, int area) {
  unsigned world = 0;
  if (gpGameState == nullptr || area < 0 || !WorldIndex(worldId, world) ||
      !Valid(world, static_cast< unsigned >(area))) {
    return 0;
  }
  return gpGameState->PortMapMarkList().Toggle(Pack(world, static_cast< unsigned >(area)));
}

void Clear() {
  if (gpGameState != nullptr) {
    gpGameState->PortMapMarkList() = List();
  }
}

} // namespace PortMapMarks
