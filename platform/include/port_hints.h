#ifndef METROID_PRIME_PORT_PORT_HINTS_H
#define METROID_PRIME_PORT_PORT_HINTS_H
#include <cstdint>
#include <string>

// Randomized text: a pickup's scan names what it now holds, and the Artifact
// Temple totems say where each artifact went. Archipelago first, then the
// offline seed; retail text when neither applies.
namespace PortHints {

// The SCAN to give a pickup at this location holding `itemType` (the item
// after the offline seed's rewrite; `randomized` says whether it applied).
// False to keep the pickup's retail scan. The scan's STRG is then watched.
bool PickupScan(uint32_t world, uint32_t area, uint32_t entity, int itemType, bool randomized,
                uint32_t& scanId);

// The player finished scanning an object whose scan text is strgId. A totem
// asks the Archipelago server to hint its artifact; anything else is ignored.
void ScanCompleted(uint32_t strgId);

// Whether strgId's last string follows the randomizer: one of the twelve
// Artifact Temple totems, or a pickup scan from PickupScan. Its text can
// arrive (or change) after the table loaded, so tables re-ask WatchedText.
bool IsWatched(uint32_t strgId);

// UTF-16 text for the last string of a watched STRG (randomprime replaces the
// same one on a totem). False for any other STRG, or when there is nothing to
// say (yet).
bool WatchedText(uint32_t strgId, std::u16string& text);

} // namespace PortHints

#endif
