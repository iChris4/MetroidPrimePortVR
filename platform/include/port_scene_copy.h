#ifndef METROID_PRIME_PORT_PORT_SCENE_COPY_H
#define METROID_PRIME_PORT_PORT_SCENE_COPY_H

// The point of CStateManager::DrawWorld where Remastered copies the opaque scene
// (RenderPrimaryPass: CopyOpaqueDepthBuffer, CopyOpaqueColorBuffer): every visible area's
// opaque world and models, the sky and the decals have drawn, and no area's transparents or
// particles yet. The vfx draws after it read the copy (DepthSoften).
#include <aurora/vfx.hpp>

inline void PortSceneCopyPoint() { aurora::gfx::vfx::capture_scene(); }

#endif
