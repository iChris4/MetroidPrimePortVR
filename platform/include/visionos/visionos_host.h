// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The bridge between the Apple Vision Pro app (visionos/, SwiftUI) and the game.
//
// The game is a framework on this platform (MetroidPrimeGame.framework, CMakeLists.txt) that the
// app embeds and links. main() belongs to SwiftUI: the game runs on a thread of its own, started
// once the immersive space's CompositorLayer exists, since the OpenXR provider
// (platform/vr/visionos) needs the layer renderer before the game creates its session. Ported
// from Wiicompiled VR's visionos_host.h.
//
// Every function is safe to call from the main thread.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MP_VISIONOS_API __attribute__((visibility("default")))

// Where the game keeps its files: `user_dir` holds the settings, saves, mods and log (the app's
// Documents folder, which the Files app shows); `cache_dir` the shader caches (Library/Caches).
// Call before mp_visionos_start_game.
MP_VISIONOS_API void mp_visionos_set_directories(const char* user_dir, const char* cache_dir);

// The settings file's name inside user_dir (the game rewrites it as it saves).
MP_VISIONOS_API const char* mp_visionos_settings_file_name(void);

// An environment override for the game's next start (the MP_* switches docs/SETTINGS.md lists),
// or NULL `value` to clear it. Call before mp_visionos_start_game.
MP_VISIONOS_API void mp_visionos_set_option(const char* name, const char* value);

// The cp_layer_renderer_t of the immersive space's CompositorLayer, retained by the provider. Set
// it before starting the game; setting NULL detaches it.
MP_VISIONOS_API void mp_visionos_set_layer_renderer(void* layer_renderer);

// A spatial event of the immersive space (the CompositorLayer's onSpatialEvent): visionOS's
// look-and-pinch selection, which the provider turns into the menu pointer and select. `phase`:
// 0 active, 1 ended, 2 cancelled; `chirality`: 0 unknown, 1 left, 2 right; the ray (when
// `has_ray`) is the event's selectionRay and the pose (when `has_pose`) its inputDevicePose's
// position, both in the space's coordinates.
MP_VISIONOS_API void mp_visionos_spatial_event(uint64_t event_id, int phase, int chirality, bool has_ray,
                                               float origin_x, float origin_y, float origin_z, float direction_x,
                                               float direction_y, float direction_z, bool has_pose, float pose_x,
                                               float pose_y, float pose_z);

// True once the layer renderer was invalidated: the immersive space was dismissed. The game then
// has no display, so the app asks it to quit (mp_visionos_request_quit).
MP_VISIONOS_API bool mp_visionos_layer_invalidated(void);

// Starts the game on its thread with the disc image at `disc_path`. False when it is already
// running, ran once in this process already, or the thread could not be created (see
// mp_visionos_last_error).
MP_VISIONOS_API bool mp_visionos_start_game(const char* disc_path);

MP_VISIONOS_API bool mp_visionos_game_running(void);

// The game's exit code once it returned; 0 before.
MP_VISIONOS_API int mp_visionos_exit_code(void);

// Asks the game to quit the way closing its window would (it saves its settings and shuts down
// in order). Returns once the request is posted, not once the game stopped.
MP_VISIONOS_API void mp_visionos_request_quit(void);

// The same from inside the game, with the reason the VR session ended (empty for a normal end),
// which mp_visionos_last_error then reports. Called by the OpenXR layer (OpenXRRequestAppQuit).
MP_VISIONOS_API void mp_visionos_request_quit_with_error(const char* error);

// Blocks until the game thread returned, or `timeout_ms` elapsed (false).
MP_VISIONOS_API bool mp_visionos_wait_for_exit(uint32_t timeout_ms);

// The last error recorded here (a failed start, the reason the VR session ended), or "".
MP_VISIONOS_API const char* mp_visionos_last_error(void);

#ifdef __cplusplus
}
#endif
