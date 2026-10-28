#ifndef AURORA_GFX_H
#define AURORA_GFX_H

#ifdef __cplusplus
#include <cstddef>
#include <cstdint>

extern "C" {
#else
#include "stddef.h"
#include "stdint.h"
#endif

#if !defined(NDEBUG) && !defined(AURORA_GFX_DEBUG_GROUPS)
#define AURORA_GFX_DEBUG_GROUPS
#endif

void push_debug_group(const char* label);
void pop_debug_group();

typedef struct {
  uint32_t queuedPipelines;
  uint32_t createdPipelines;
  uint32_t drawCallCount;
  uint32_t mergedDrawCallCount;
  uint32_t renderPassCount; // render passes the frame's EFB recording encoded
  uint32_t lastVertSize;
  uint32_t lastUniformSize;
  uint32_t lastIndexSize;
  uint32_t lastStorageSize;
  uint32_t lastTextureUploadSize;
} AuroraStats;

const AuroraStats* aurora_get_stats();
// Live GPU textures: [0] sampled only, [1] render targets/copies, [2] Remastered lighting (room cubes,
// ambient volumes, baked lightmaps and the fixed BRDF/empty ones). Bytes are the mip chain's size.
typedef struct {
  uint32_t count[3];
  uint64_t bytes[3];
} AuroraTextureStats;
void aurora_get_texture_stats(AuroraTextureStats* out);
// Dawn's own estimate of the device's GPU memory (all textures and buffers, whoever made them) and what its
// backend allocator holds. false until the device exists.
typedef struct {
  uint64_t total, textures, buffers; // ComputeEstimatedMemoryUsageInfo
  uint64_t allocatorUsed, allocatorAllocated; // GetAllocatorMemoryInfo
} AuroraDawnMemory;
bool aurora_get_dawn_memory(AuroraDawnMemory* out);
// Any thread: within a second, hand the GPU memory Dawn keeps for reuse back to the driver. The submit
// thread also does it on its own once that memory grows by 64 MiB.
void aurora_reduce_gpu_memory(void);
// Pipeline eviction (the pipeline cache keeps its rows, so an evicted pipeline rebuilds on its next draw).
// Idle eviction: built pipelines no draw used for `idleFrames` frames are released; 0 turns it off.
void aurora_set_pipeline_eviction(uint32_t idleFrames);
// Any thread: at the next frame start, release the built pipelines unused for `idleFrames` frames
// (at least 16 are always kept). Android onTrimMemory and the console use it.
void aurora_trim_pipelines(uint32_t idleFrames);
// Built pipelines held now.
uint32_t aurora_get_pipeline_count(void);
// Pipeline use contexts. Draws record the pipelines they use under the current context, so a later
// session can build them before they are needed. GLOBAL pipelines (player, gun, HUD) are built at
// startup and never evicted; FRONTEND is the default; an area is (uint64_t(mlvl) << 32) | areaIdx.
#define AURORA_PIPELINE_CONTEXT_GLOBAL 0ull
#define AURORA_PIPELINE_CONTEXT_FRONTEND 1ull
#define AURORA_PIPELINE_CONTEXT_AREA(mlvl, area) ((((uint64_t)(uint32_t)(mlvl)) << 32) | (uint32_t)(area))
// Draw thread: the context the draws that follow record under; returns the previous one. Every frame
// starts in FRONTEND.
uint64_t aurora_pipeline_set_context(uint64_t context);
// Any thread: queue background builds of the pipelines `context` used before, plus those shared by
// several areas. Lazy pipeline cache only; a context prefetched in the last 600 frames is skipped.
void aurora_pipeline_prefetch(uint64_t context);
typedef struct {
  uint64_t skippedDraws; // draws dropped because their pipeline was not built yet
  uint64_t newMisses;    // draw lookups of a pipeline the cache had never recorded
  uint64_t knownMisses;  // draw lookups of a recorded pipeline that was not built or queued
  uint64_t lateMisses;   // draw lookups of a pipeline still waiting in the background queue
  uint64_t prefetches;   // prefetch requests run
  uint64_t prefetchQueued; // rows those requests queued for a build
  uint32_t pinned;       // GLOBAL pipelines kept from eviction
  uint32_t known;        // pipelines the cache has rows for (lazy only)
  uint32_t pending;      // pipelines queued or building
} AuroraPipelineStats;
void aurora_get_pipeline_stats(AuroraPipelineStats* out);
// Live sampled/target TextureRefs grouped by creation label, size and format, most numerous first. Fills up to
// `cap` groups and returns how many exist; `totals` (optional) gets the count and bytes over all of them.
typedef struct {
  const char* label;
  char format[24];
  uint32_t width, height, mips;
  uint32_t count;
  uint64_t bytes; // all of the group's textures together
} AuroraTextureGroup;
uint32_t aurora_get_texture_groups(AuroraTextureGroup* out, uint32_t cap);
// Textures made by each direct CreateTexture call site that is not a TextureRef (cumulative, not live).
typedef struct AuroraTextureSite {
  const char* site; // file:line under extern/aurora/lib
  uint64_t count;
  uint64_t bytes;
} AuroraTextureSite;
// Returns the number of sites; fills up to `cap`.
uint32_t aurora_get_texture_sites(AuroraTextureSite* out, uint32_t cap);
// The AuroraConfig::frameBufferScale in use: the device may allow less than was asked for.
uint32_t aurora_get_frame_buffer_scale();
// Takes the frame limits AuroraConfig::frameBuffersDeferred held back, at the start of the next
// frame (with no frame in flight). Safe from any thread; nothing when none are held.
void aurora_reserve_frame_buffers();
// The AuroraConfig::frameStorageScale in use (the array buffer's, in units of its 8 MiB at 1).
uint32_t aurora_get_frame_storage_scale();
// The MiB set aside for AuroraConfig::residentGeometryMiB: the device may allow less.
uint32_t aurora_get_resident_geometry_mib();
// The bytes of it in use, as of the last frame processed.
uint64_t aurora_get_resident_geometry_used();
float aurora_get_fps();
// Whether the device samples BC and ASTC 4x4 compressed textures (false until it exists).
void aurora_get_texture_support(bool* bc, bool* astc);
// A GX texture object's (GXTexObj*) base level decoded to RGBA8 into `out` (cap bytes); false for
// palette or compressed PC formats and when it does not fit.
bool aurora_gx_texobj_rgba8(const void* obj, uint32_t* width, uint32_t* height, uint8_t* out, size_t cap);

void aurora_enable_vsync(bool enabled);

/* Stereo (OpenXR) controls, lifted from Wiicompiled VR's aurora fork. */

// Live scene interpolation at the headset's display deadlines (retained replay;
// not ported yet, so the getter stays false).
void aurora_set_stereo_frame_interpolation(bool enabled);
bool aurora_get_stereo_frame_interpolation();
void aurora_set_stereo_motion_logging(bool enabled);
// GX draw commands issued so far (GXBegin and display list calls), a running
// count the game samples around a section to know what it draws.
uint32_t aurora_gx_draw_commands_issued(void);
// How each immersive eye is replayed (retained for the replay path).
void aurora_set_stereo_stop_at_display_copy(bool enabled);
void aurora_set_stereo_skip_copy_clears(bool enabled);
void aurora_set_stereo_single_pass_eyes(bool enabled);
// Fixed foveated rendering of the immersive eyes: 0 off, 1 low, 2 medium, 3
// high (gfx/foveation.hpp's rings; menus on the virtual screen never are).
// Live, but the Vulkan device only gets fragment density maps when
// AuroraConfig::xrFragmentDensityMap asked for them at creation (the Quest
// build's patched Dawn); aurora_stereo_foveation_available says whether this
// session has them. Unavailable on the desktop.
void aurora_set_stereo_foveation(uint32_t level);
uint32_t aurora_get_stereo_foveation();
bool aurora_stereo_foveation_available();
// The virtual screen the immersive frame's 2D layer is folded onto, in metres.
void aurora_set_stereo_hud_screen(bool enabled, float widthMeters, float distanceMeters);
bool aurora_get_stereo_hud_screen_enabled();
// The game picture's width over height, and the desktop snapshot's that Aurora
// letterboxes the picture into. False before the first frame.
bool aurora_get_stereo_screen_aspects(float* pictureAspect, float* snapshotAspect);

typedef enum {
  AURORA_STEREO_MIRROR_NORMAL = 0,
  AURORA_STEREO_MIRROR_BOTH = 1,
  AURORA_STEREO_MIRROR_LEFT = 2,
  AURORA_STEREO_MIRROR_RIGHT = 3,
  AURORA_STEREO_MIRROR_NONE = 4,
} AuroraStereoMirrorView;
// What the desktop window shows while the headset is running.
void aurora_set_stereo_mirror_view(AuroraStereoMirrorView view);

// --- stereo replay draw routes ---------------------------------------------
// How the per-eye replay treats the GX draws that follow the marker
// (AuroraSetStereoDrawRoute in dolphin/gx/GXAurora.h, ordered with the draws
// through the GX FIFO). Orthographic draws are always identical in both eyes.
typedef enum AuroraStereoDrawRoute {
  AURORA_STEREO_ROUTE_WORLD = 0,           // a game camera draw: eye frustum and eye pose composed in
  AURORA_STEREO_ROUTE_HEAD_LOCKED = 1,     // a camera-space draw (HUD), scaled by the head-locked scale
  // A 2D layout drawn for the whole picture (Metroid Prime's morph ball HUD): its mono picture, perspective and
  // orthographic draws alike, is laid on the virtual screen (aurora_set_stereo_screen_2d) hung ahead in the game
  // camera's space, so its corners stay in sight and the head looks around it. Drawn like FULLSCREEN without one.
  AURORA_STEREO_ROUTE_SCREEN_2D = 2,
  AURORA_STEREO_ROUTE_FULLSCREEN = 3,      // identical in both eyes, game projection untouched
  AURORA_STEREO_ROUTE_PER_EYE_RESOLVE = 4, // reserved: the pass's EFB copy replayed per eye (drawn like WORLD)
  // Not drawn in the eyes. An EFB copy made under it is taken from the mono EFB only and the eyes sample that copy:
  // an image that is not a view, such as a shadow rendered from a light. When that copy also clears the whole EFB,
  // its render pass is not replayed into the eyes at all, since nothing could see it there.
  AURORA_STEREO_ROUTE_SKIP = 5,
  // HEAD_LOCKED, with its orthographic draws laid on the head-locked plane (AuroraSetStereoHeadLockedPlane in
  // dolphin/gx/GXAurora.h) and its EFB copies taken from each eye through that plane: a 2D window onto the view,
  // such as Metroid Prime's scan visor, keeps pointing where the head does and magnifies each eye's own view.
  AURORA_STEREO_ROUTE_HEAD_LOCKED_2D = 6,
  // A draw attached to the camera that stands for infinity, such as Metroid Prime's sky (a dome of some sixty
  // units centred on the camera, pushed into the far depth range): the eyes see it with the head's rotation
  // only, no eye offset, so it has no disparity and does not shift when the head moves.
  AURORA_STEREO_ROUTE_SKY = 7,
  // Detached 2D plane that gently follows the full headset pose (morph-ball HUD).
  AURORA_STEREO_ROUTE_FLOATING_HUD = 8,
  // Laid on a plane of the virtual screen's size and distance like AURORA_STEREO_ROUTE_FLOATING_HUD, but the
  // plane stays where it was placed: upright ahead of the head's facing, at the head's position, on the first
  // immersive frame after aurora_set_stereo_screen_anchor holds it, until it is released (Metroid Prime's
  // message boxes and fixed morph ball HUD). With no plane placed, drawn like AURORA_STEREO_ROUTE_SCREEN_2D.
  AURORA_STEREO_ROUTE_ANCHORED_SCREEN = 9,
  // A camera-space draw (perspective) placed in the held anchor's space instead of the game camera's: as if the
  // camera stood where AURORA_STEREO_ROUTE_ANCHORED_SCREEN's plane was placed, looking along its facing. True depth
  // and parallax, held still while the head looks around it (Metroid Prime's map in the headset). With no anchor
  // placed, drawn like WORLD. AuroraSetStereoPlacement3D (dolphin/gx/GXAurora.h) can scale and move the
  // draw's camera space within the anchor's first.
  AURORA_STEREO_ROUTE_ANCHORED_3D = 10,
  // AURORA_STEREO_ROUTE_ANCHORED_3D through AURORA_STEREO_ROUTE_FLOATING_HUD's easing pose instead of the held
  // anchor (a 3D model standing on the floating plane's screen).
  AURORA_STEREO_ROUTE_FLOATING_3D = 11,
} AuroraStereoDrawRoute;
// AURORA_STEREO_ROUTE_HEAD_LOCKED draws are scaled about the camera origin
// before the eye offset: sizeScale sets their angular size, distanceScale
// their distance (0.75 and 0.75: the HUD at three quarters of its authored
// size and distance). Read at frame begin.
void aurora_set_stereo_head_locked(float sizeScale, float distanceScale);
// The virtual screen AURORA_STEREO_ROUTE_SCREEN_2D draws are laid on: widthMeters across and distanceMeters
// ahead of the game camera, its height following the picture's aspect, the game drawing unitsPerMeter units to
// the metre. A zero turns it off. Read at frame begin.
void aurora_set_stereo_screen_2d(float widthMeters, float distanceMeters, float unitsPerMeter);
// Holds AURORA_STEREO_ROUTE_ANCHORED_SCREEN's plane: while held, the first immersive frame places it ahead of the
// head and later frames keep it there; released, or on a frame that is not immersive, it is dropped, and the
// next hold places it again. Read at frame begin.
void aurora_set_stereo_screen_anchor(bool held);
// Whether immersive packets are replayed per eye (default true). Off, they are
// shown as the mono image on the virtual screen, which is the fail-safe path.
void aurora_set_stereo_immersive_replay(bool enabled);
bool aurora_get_stereo_immersive_replay(void);
// Whether an immersive frame draws both eyes in one Vulkan multiview render pass
// when the device can (default true). Read at frame begin.
void aurora_set_stereo_multiview(bool enabled);
// The device can (PrimedGun's patched Dawn on Vulkan, with the OpenXR interop).
bool aurora_get_stereo_multiview_available(void);
// Whether the FIFO processor resolves indexed vertex attributes into direct ones
// before the upload (default false): one fetch per attribute in the vertex shader
// instead of an index and a dependent array fetch, which stalls the Quest's GPU,
// for more CPU work per vertex. Takes effect at the next frame.
void aurora_set_gx_deindex_vertices(bool enabled);

#ifdef __cplusplus
}
#endif

#endif
