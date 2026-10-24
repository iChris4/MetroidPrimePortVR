#pragma once
// Kept out of port_room_geo.h, whose tests build without the game's headers.
#include "Kyoto/Math/CTransform4f.hpp"

class CGameArea;
class CModel;

namespace PortRoomGeo {

// The skies of an alive area, in place of the world's or of the room's own sky actors
// (CWorld::DrawSky): its room's sky instances that are shown, on a layer that is on, outermost
// first. `orient` is a layer's turn and scale in the world, with no translation, and
// `radiance` its Instance::skyRadiance (0 for not known). 0 for none, or while one loads.
// `world`: the world's sky instead (Instance::worldSky), for where retail draws its own.
struct SkyLayer {
  const CModel* model = nullptr;
  CTransform4f orient = CTransform4f::Identity();
  float radiance[3] = {};
};
enum { kMaxSkyLayers = 8 };
int Skies(const CGameArea& area, SkyLayer (&out)[kMaxSkyLayers], bool world = false);

} // namespace PortRoomGeo
