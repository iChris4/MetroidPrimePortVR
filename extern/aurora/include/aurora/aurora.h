#ifndef AURORA_AURORA_H
#define AURORA_AURORA_H

#ifdef __cplusplus
#include <cstddef>
#include <cstdint>

extern "C" {
#else
#include "stdbool.h"
#include "stddef.h"
#include "stdint.h"
#endif

typedef enum {
  SAMPLER_BILINEAR,
  SAMPLER_AREA,
} AuroraSampler;

typedef enum {
  BACKEND_AUTO,
  BACKEND_D3D11,
  BACKEND_D3D12,
  BACKEND_METAL,
  BACKEND_VULKAN,
  BACKEND_OPENGL,
  BACKEND_OPENGLES,
  BACKEND_WEBGPU,
  BACKEND_NULL,
} AuroraBackend;

typedef enum {
  LOG_DEBUG,
  LOG_INFO,
  LOG_WARNING,
  LOG_ERROR,
  LOG_FATAL,
} AuroraLogLevel;

typedef struct {
  int32_t x;
  int32_t y;
} AuroraWindowPos;

typedef struct {
  uint32_t width;
  uint32_t height;

  /**
   * Width of the main GX framebuffer.
   */
  uint32_t fb_width;

  /**
   * Height of the main GX framebuffer.
   */
  uint32_t fb_height;

  /**
   * The size of the framebuffer used to present to the operating system.
   * May differ from fb_width if Aurora is instructed to force an aspect ratio or resolution configuration.
   */
  uint32_t native_fb_width;

  /**
   * The size of the framebuffer used to present to the operating system.
   * May differ from fb_height if Aurora is instructed to force an aspect ratio or resolution configuration.
   */
  uint32_t native_fb_height;
  float scale;
} AuroraWindowSize;

typedef struct SDL_Window SDL_Window;
typedef struct AuroraEvent AuroraEvent;

typedef void (*AuroraLogCallback)(AuroraLogLevel level, const char* module, const char* message, unsigned int len);
typedef void (*AuroraImGuiInitCallback)(const AuroraWindowSize* size);

/* ------------------------------------------------------------------------- */
/* Stereo (OpenXR) frames, lifted from Wiicompiled VR's aurora fork.         */
/* ------------------------------------------------------------------------- */

enum { AURORA_STEREO_EYE_COUNT = 2 };

/**
 * One eye of a stereo frame supplied by the host application.
 *
 * projection is row-major and supplies the OpenXR frustum's X/Y scale and
 * asymmetric-center terms at [0][0], [0][2], [1][1], and [1][2]. Aurora
 * applies those four values to each perspective GX draw while preserving the
 * draw's own depth mapping and renderer depth-range adjustment.
 *
 * viewFromCenter is a row-major affine 3x4 transform from the center-eye view
 * space into this eye's view space. Identity keeps the recorded view. That
 * center-eye space is the game's recorded view space unless
 * aurora_set_stereo_scene_anchor() relocated the camera for the sealed frame.
 *
 * Both transforms are ignored in AURORA_STEREO_FRAME_VIRTUAL_SCREEN mode.
 */
typedef struct {
  uint32_t width;
  uint32_t height;
  float projection[16];
  float viewFromCenter[12];
} AuroraStereoEye;

typedef enum {
  // Replay perspective GX draws with the supplied per-eye transforms.
  AURORA_STEREO_FRAME_IMMERSIVE_REPLAY = 0,
  // Copy the completed mono present source to the eye output. The OpenXR
  // backend presents it as a compositor quad layer.
  AURORA_STEREO_FRAME_VIRTUAL_SCREEN = 1,
} AuroraStereoFrameMode;

// aurora_end_frame() uses this sentinel when its caller cannot associate a
// sealed frame with an application safety state. Immersive packets are only
// accepted through aurora_end_frame_tagged() with an exact matching tag.
#define AURORA_STEREO_CONTENT_TAG_UNKNOWN UINT64_MAX

/**
 * Stereo data for one sealed GX frame. frameToken is opaque to Aurora and is
 * forwarded unchanged to the internal stereo output sink. contentTag must
 * match the tag latched by aurora_end_frame_tagged() for immersive replay.
 */
typedef struct {
  uint64_t frameToken;
  AuroraStereoEye eyes[AURORA_STEREO_EYE_COUNT];
  AuroraStereoFrameMode mode;
  uint64_t contentTag;
  // Predicted display time converted to std::chrono::steady_clock nanoseconds.
  uint64_t displayTimeNanos;
  // Changes whenever the eyes' centre space is set again (a recentre, a height set, a new reference space):
  // a held screen anchor (aurora_set_stereo_screen_anchor) is placed again in the new space.
  uint64_t originGeneration;
  // Immersive replay only: each eye keeps what it sees through the 2D layer's
  // screen and is transparent elsewhere (not ported yet; must be false).
  bool window;
} AuroraStereoFrame;

/**
 * Called on Aurora's frame worker immediately before a GX frame is sealed.
 * Return false to render that logical frame in mono only. The callback must
 * be non-blocking and must not call back into Aurora.
 */
typedef bool (*AuroraStereoFrameProvider)(uint32_t logicalFrame, AuroraStereoFrame* frame, void* userdata);

#define MEM1_DEFAULT_SIZE (24 * 1024 * 1024)
#define ARAM_DEFAULT_SIZE (16 * 1024 * 1024)

typedef struct {
  const char* appName;
  const char* userPath;
  const char* cachePath;
  const char* resourcesPath;
  AuroraBackend desiredBackend;
  uint32_t msaa;
  uint16_t maxTextureAnisotropy;
  bool vsync;
  bool startFullscreen;
  bool allowJoystickBackgroundEvents;
  bool pauseOnFocusLost;
  bool allowTextureDumps;
  bool allowCpuAdapter;
  int32_t windowPosX;
  int32_t windowPosY;
  uint32_t windowWidth;
  uint32_t windowHeight;
  void* iconRGBA8;
  uint32_t iconWidth;
  uint32_t iconHeight;
  AuroraLogCallback logCallback;
  AuroraLogLevel logLevel;
  AuroraImGuiInitCallback imGuiInitCallback;

  /*
   * The size of the GameCube's main memory, or MEM1 on the Wii.
   * Note that it will not be allocated at the exact 0x80000000 address, as that cannot be guaranteed.
   * This can be set to 0 to disable allocating this region.
   */
  uint32_t mem1Size;

  /*
   * The size of the GameCube's ARAM, or MEM2 on the Wii.
   * This can be set to 0 to disable allocating this region.
   */
  uint32_t mem2Size;

  /*
   * Multiplies what one frame can hold of vertex, index and array data (5, 2 and 8 MiB at 1),
   * and doubles the 24 MiB of uniforms when above 1. 0 means 1. A frame that outgrows
   * these limits drops the draws that no longer fit and logs a warning once, so raise this for
   * content denser than the original game's.
   */
  uint32_t frameBufferScale;

  // Enables renderer features needed by an external XR compositor. The normal
  // desktop path is unchanged when false.
  bool xrInterop;
  // Asks for fragment density maps on the Vulkan device (foveated eyes). Only a
  // Dawn built with Aurora's patches has them; not used on the desktop.
  bool xrFragmentDensityMap;
  // Optional OpenXR-selected D3D adapter. Supplying the runtime's LUID before
  // device creation keeps Dawn and the compositor on the same physical GPU.
  bool hasD3D12AdapterLuid;
  uint32_t d3d12AdapterLuidLow;
  int32_t d3d12AdapterLuidHigh;

  /*
   * Like frameBufferScale, for the array (storage) buffer alone: 8 MiB at 1. 0 follows
   * frameBufferScale. Room geometry kept resident only sends the arrays that miss its
   * region, but those can outgrow the vertex scale.
   */
  uint32_t frameStorageScale;

  /*
   * MiB of the shared vertex, index and array buffers set aside for data kept across frames
   * (GXPortRetainResident), on top of what a frame holds. 0 sets none aside; the device's
   * limits may allow less.
   */
  uint32_t residentGeometryMiB;

  /*
   * An initial pipeline cache database held in memory (must stay valid for the whole run). When set
   * it is used instead of <resourcesPath>/initial_pipeline_cache.db, so a stale file left beside the
   * application cannot override it.
   */
  const uint8_t* pipelineCacheSeedData;
  size_t pipelineCacheSeedSize;
  /* Merge no initial pipeline cache at all (recording a new seed from an empty cache). */
  bool pipelineCacheNoSeed;
  /*
   * PrimedGun: a second seed, merged after the initial one, with the configs the fork's draws ask
   * for (de-indexed vertices, the headset's eye pipelines). Held in memory when the data is set,
   * else read from <resourcesPath>/<pipelineCacheExtraSeedName> when that is set. Its SQLite
   * user_version must be the cache's GX layout epoch, or the seed is ignored. Its GX rows take the
   * device's MSAA sample count, and only the eye pipelines this device draws with are kept.
   */
  const uint8_t* pipelineCacheExtraSeedData;
  size_t pipelineCacheExtraSeedSize;
  const char* pipelineCacheExtraSeedName;
  /* The headset will run, so the extra seed's eye pipelines are wanted; without it they are left out. */
  bool pipelineCacheExtraSeedStereo;
  /*
   * Keep the pipeline cache (and the merged seed) but do not compile its rows at startup:
   * pipelines are built on first use. For devices that cannot spare the memory the startup
   * compile takes (Dawn/driver state per pipeline).
   */
  bool pipelineCacheLazy;
  /* Release built pipelines unused for this many frames (0 = never); see aurora_set_pipeline_eviction. */
  uint32_t pipelineEvictFrames;
  /*
   * Do not compile the cached rows whose shader is PBR at startup: the caller knows nothing
   * can draw PBR (no mod brings a PBR material). The rows stay in the database, and a PBR
   * draw still builds its pipeline on first use. pipelineCacheSkipPbrReason is the log text.
   */
  bool pipelineCacheSkipPbr;
  const char* pipelineCacheSkipPbrReason;

  /*
   * A directory (with a trailing slash) Dawn searches first for the Vulkan library, e.g. a shim
   * for a custom driver. Null searches the usual places only.
   */
  const char* vulkanLibraryDir;

  /*
   * Dawn device toggles to turn on and off, comma-separated (null or empty for none): driver
   * workarounds chosen by the application. MP_DAWN_ENABLE / MP_DAWN_DISABLE apply after these.
   */
  const char* dawnEnableToggles;
  const char* dawnDisableToggles;

  /*
   * The per-frame staging buffers hold the full frameBufferScale/frameStorageScale sizes. When
   * false they start smaller (4, 8, 2 and 8 MiB of vertices, uniforms, indices and arrays, 8
   * of texture uploads), and a frame that needs more spills into memory allocated for that
   * frame, up to the scaled sizes.
   */
  bool frameBuffersPinned;

  /*
   * Android, where the device buffers are taken at their frame limit up front: hold the
   * frameBufferScale/frameStorageScale limits back and run at the unscaled ones (what a scale of
   * 1 takes) until aurora_reserve_frame_buffers() asks for them, then take them whole between
   * two frames. Elsewhere, or with the scale pinned, it does nothing.
   */
  bool frameBuffersDeferred;

  /*
   * When set and only the Null backend started (no GPU and no software adapter), this message is
   * shown in an error box and the process exits with status 1, instead of running with no picture.
   */
  const char* noGraphicsMessage;
} AuroraConfig;

typedef struct {
  AuroraBackend backend;
  const char* userPath;
  const char* cachePath;
  SDL_Window* window;
  AuroraWindowSize windowSize;
} AuroraInfo;

AuroraInfo aurora_initialize(int argc, char* argv[], const AuroraConfig* config);
void aurora_shutdown();
const AuroraEvent* aurora_update();
bool aurora_begin_frame();
// aurora_begin_frame() with the application safety tag the frame will be
// sealed with. The stereo packet is consumed here, so an immersive one is
// accepted for per-eye replay only when its contentTag matches this tag (and
// aurora_end_frame_tagged() must then seal the frame with the same tag).
bool aurora_begin_frame_tagged(uint64_t contentTag);
void aurora_end_frame();
// Seal the current frame with an opaque application safety tag. Aurora rejects
// an immersive provider packet unless its contentTag matches this exact frame.
void aurora_end_frame_tagged(uint64_t contentTag);
// aurora_end_frame_tagged() plus a host-owned ImGui frame to present with it
// (not supported in this lineage: the handle must be NULL).
void aurora_end_frame_ex(uint64_t contentTag, void* imguiFrame);
// When the host pumps SDL events itself and drives begin/end frame from another
// thread, this stops those calls from pumping events.
void aurora_set_host_event_pump(bool hostPumps);

// Registering nullptr restores the ordinary mono-only render path. Replace or
// unregister a provider only while Aurora's frame worker is idle.
void aurora_set_stereo_frame_provider(AuroraStereoFrameProvider provider, void* userdata);
// Wake retained stereo replay after publishing a packet (no-op until retained
// replay is ported; the next sealed frame consumes the packet).
void aurora_notify_stereo_frame();
// Shows the headset settings panel as the OpenXR backend's own compositor quad
// layer instead of drawing it into the eyes. Any thread.
void aurora_set_stereo_panel_layer(bool enabled);
// The headset panel layer's image: width x height RGBA8 pixels (bytes R, G, B,
// A, straight alpha), copied before this returns and uploaded by the frame
// worker into the image it hands the panel layer. The layer shows it only at
// exactly the size it asks for; anything else, or null, leaves it transparent.
// Any thread; call it when the image changes, not every frame.
void aurora_set_stereo_panel_image(const void* rgba8, uint32_t width, uint32_t height);
// Relocates the recorded camera for the frame about to be sealed: a row-major
// affine 3x4 from the recorded view space to the anchored view space. Latched
// by the next aurora_end_frame*() and then cleared; null disables it.
void aurora_set_stereo_scene_anchor(const float anchorFromScene[12]);
// As above, also naming the world units per metre the anchor was built with.
void aurora_set_stereo_scene_anchor_scaled(const float anchorFromScene[12], float unitsPerMeter);
// Recorded world-to-view camera, for camera-separated interpolation (unused
// until retained replay is ported).
void aurora_set_stereo_scene_view(const float viewFromWorld[12]);
void aurora_wait_for_frame_worker();
bool aurora_wait_for_frame_worker_for(uint32_t timeoutMicros);
// Producer-thread shutdown barrier: waits until the frame worker is idle.
void aurora_quiesce_frame_worker();
void aurora_store_pipeline_caches();
void aurora_request_pipeline_cache_store();
void aurora_set_pipeline_cache_idle_store(bool allowed);
// The native thread id of Aurora's frame worker, or 0 where there is none.
uint32_t aurora_get_frame_worker_native_thread_id(void);
// The native thread id of Aurora's GX FIFO processor, or 0 where there is none.
uint32_t aurora_get_gx_worker_native_thread_id(void);
// Absolute schedule for the next sealed frame on steady_clock (retained replay only).
void aurora_set_present_schedule(uint64_t baseNanos, uint64_t intervalNanos);
void aurora_report_producer_paced(bool paced);
// Drops the swapchain if the window's surface went away, without starting a frame.
// For a main thread that waits outside the frame loop (a file dialog, a long copy):
// Android's surfaceDestroyed waits for this, and a swapchain left on a destroyed
// window can lose the device. Cheap when nothing changed.
void aurora_release_lost_surface();
// True while the app is backgrounded or has no surface (Android pause): the main loop
// may legitimately not run then. Atomics only, so any thread can ask.
bool aurora_is_suspended();

void aurora_set_log_level(AuroraLogLevel level);
void aurora_set_pause_on_focus_lost(bool value);
void aurora_set_background_input(bool value);
void aurora_set_resampler(AuroraSampler sampler);
/** Sharpens the frame (CAS-style contrast-adaptive, 0 = off, 1 = most) when it is drawn smaller than
 * the window and stretched to it; a frame at or above the window's size is never touched. */
void aurora_set_present_sharpness(float sharpness);
/** Sets MSAA samples (1 or 4) and max texture anisotropy (1-16); applied at the next frame start. */
void aurora_set_graphics_quality(uint32_t msaa, uint16_t maxTextureAnisotropy);
/** Evaluates GX lighting per pixel instead of per vertex (default off); lit pipelines rebuild on next use. */
void aurora_set_per_pixel_lighting(bool enabled);
/** Sets the clock timescale. Default 1.0f. 0.0f is paused. Range 0.0f-16.0f. */
void aurora_set_timescale(float scale);

/**
 * Holds background pipeline builds (the cache and shipped seed, compiled ahead of use) while paused.
 * Pipelines a draw looks up still compile. Starts paused under D3D11, where a build blocks every
 * other object creation; the app resumes them when it no longer loads much.
 */
void aurora_set_background_pipelines_paused(bool paused);

/**
 * Stops aurora writing the controller mapping, keyboard binding and controller port files for the
 * rest of the process. For an app that has just moved them away (a configuration reset).
 */
void aurora_disable_config_persistence();

AuroraBackend aurora_get_backend();
// The driver's description from the graphics adapter (e.g. "Turnip Mesa driver 25.1.0"); "" before init.
const char* aurora_get_gpu_driver();
// True when AuroraConfig::vulkanLibraryDir's Vulkan library failed and the system's was tried instead.
bool aurora_vulkan_library_failed();
const AuroraBackend* aurora_get_available_backends(size_t* count);
float aurora_get_timescale();

/**
 * GPU self-test: renders known patterns offscreen through the game's GX path, reads them back and logs
 * "gpu selftest: <case>: PASS/FAIL" lines. Call inside a frame (after aurora_begin_frame, before the frame's
 * own draws); it changes GX state, so the caller must reset its cached state afterwards. Returns false if a
 * run is already pending or the test could not start. Results arrive a frame or two later. Each case's
 * pipeline is waited for (a blocking lookup, so a compile can delay the call), so no case's draw is
 * skipped for a pipeline that is not ready: call once with warmup (nothing logged) to build and wait for
 * the cases' pipelines, then call again for the real run.
 */
bool aurora_gpu_selftest_run(bool warmup);
bool aurora_gpu_selftest_pending();
/** Copies the last run's one-line summary ("X/Y passed ...", empty before the first run); returns its length. */
size_t aurora_gpu_selftest_summary(char* buf, size_t size);

#ifdef __cplusplus
}
#endif

#endif
