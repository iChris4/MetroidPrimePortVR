// Where the map side files go, from the game: the port's own user folder and
// the identity of the memory card the driver has mounted.
//
// Split from port_map_side.cpp on purpose: that file holds the format, the file
// layer and the staging, and takes both from here, so its unit test needs
// neither the card stack nor an SDL user folder. The identity is the card's
// resolved directory, the game id and maker its files are created with and the
// card port (CMemoryCardSys::PortCard*, which is where CARDInit is called with
// them); the port's card, a Dolphin or PrimeHack card shared through the
// emulator-card setting, and the card of an Archipelago room each key their own
// files, and so do a PAL and a USA card under one directory. Nothing is ever
// written into the card's (or the emulator's) folder: the side data lives under
// mapdata in the port's user folder, and only the slot's bytes decide whether
// it is used.

#include <string>

#include "Kyoto/CMemoryCardSys.hpp"
#include "port_map_side.h"
#include "port_paths.h"

namespace PortMapSide {

void Location(std::string& userFolder, std::string& cardIdentity) {
  userFolder = PortPaths::UserFolder();
  const std::string idMaker = CMemoryCardSys::PortCardIdMaker();
  // Before the card is set up there is nothing to key on: no file of ours can
  // belong to a card that is not there.
  cardIdentity = idMaker.empty()
                     ? std::string()
                     : CardIdentity(CMemoryCardSys::PortCardResolvedDirectory(), idMaker,
                                    CMemoryCardSys::PortCardChannel());
}

} // namespace PortMapSide
