// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Apple Vision Pro app's way into the game (visionos/visionos_host.h), ported from
// Wiicompiled VR's runtime/src/platform/visionos/visionos_host.mm.

#include "visionos/visionos_host.h"

#include "vr/visionos/xr_visionos.h"

#include <aurora/main.h>
#undef main

#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#import <Foundation/Foundation.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include <pthread.h>

// The environment prefix of the game's switches (MP_ here, MP2_ for Echoes).
#ifndef MP_VISIONOS_ENV_PREFIX
#define MP_VISIONOS_ENV_PREFIX "MP_"
#endif
#ifndef MP_VISIONOS_SETTINGS_FILE
#define MP_VISIONOS_SETTINGS_FILE "port_settings.ini"
#endif
#ifndef MP_VISIONOS_PROGRAM_NAME
#define MP_VISIONOS_PROGRAM_NAME "metroid_prime_port"
#endif

namespace {

// The game thread's own stack. The desktop main thread the game was written for has 8 MiB, and
// a few deep host paths (shader compilation, the decompressors) come close to that; secondary
// threads on the iOS family default to 512 KiB.
constexpr size_t kGameThreadStackSize = 64u << 20;

std::mutex g_mutex;
std::condition_variable g_exited;
std::atomic_bool g_running{false};
std::atomic_bool g_started{false};
int g_exit_code = 0;
std::string g_last_error;
std::string g_disc_path;
std::string g_error_storage;

void SetError(std::string message) {
    std::lock_guard lock(g_mutex);
    g_last_error = std::move(message);
}

void SetEnv(const char* name, const char* value) {
    const std::string full = std::string(MP_VISIONOS_ENV_PREFIX) + name;
    if (value != nullptr) {
        setenv(full.c_str(), value, 1);
    } else {
        unsetenv(full.c_str());
    }
}

void* GameThreadMain(void*) {
    pthread_setname_np("Metroid Prime game");
    static char program_name[] = MP_VISIONOS_PROGRAM_NAME;
    std::string disc;
    {
        std::lock_guard lock(g_mutex);
        disc = g_disc_path;
    }
    // The disc travels as the first argument, the one main.cpp's ResolveDiscPath reads first.
    char* argv[] = {program_name, disc.data(), nullptr};
    const int code = aurora_main(disc.empty() ? 1 : 2, argv);
    {
        std::lock_guard lock(g_mutex);
        g_exit_code = code;
        g_running.store(false, std::memory_order_release);
    }
    g_exited.notify_all();
    return nullptr;
}

void PushQuit() {
    // Aurora turns SDL's quit into AURORA_EXIT, the path a closed desktop window takes: the game
    // saves its settings and shuts down in order.
    if (SDL_WasInit(SDL_INIT_EVENTS) != 0) {
        SDL_Event event{};
        event.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&event);
    }
}

} // namespace

extern "C" {

void mp_visionos_set_directories(const char* user_dir, const char* cache_dir) {
    // PortPaths::UserFolder and main.cpp's cache folder both honour these first.
    SetEnv("USER_PATH", user_dir != nullptr && user_dir[0] != '\0' ? user_dir : nullptr);
    SetEnv("CACHE_PATH", cache_dir != nullptr && cache_dir[0] != '\0' ? cache_dir : nullptr);
}

const char* mp_visionos_settings_file_name(void) { return MP_VISIONOS_SETTINGS_FILE; }

void mp_visionos_set_option(const char* name, const char* value) {
    if (name == nullptr || name[0] == '\0') {
        return;
    }
    // Accept either spelling: "VR_LOG" or "MP_VR_LOG".
    const std::string prefix = MP_VISIONOS_ENV_PREFIX;
    std::string bare = name;
    if (bare.rfind(prefix, 0) == 0) {
        bare = bare.substr(prefix.size());
    }
    SetEnv(bare.c_str(), value);
}

void mp_visionos_set_layer_renderer(void* layer_renderer) { xr_visionos_set_layer_renderer(layer_renderer); }

bool mp_visionos_layer_invalidated(void) { return xr_visionos_layer_invalidated(); }

void mp_visionos_spatial_event(uint64_t event_id, int phase, int chirality, bool has_ray, float origin_x,
                               float origin_y, float origin_z, float direction_x, float direction_y,
                               float direction_z, bool has_pose, float pose_x, float pose_y, float pose_z) {
    xr_visionos_spatial_event(event_id, phase, chirality, has_ray, origin_x, origin_y, origin_z, direction_x,
                              direction_y, direction_z, has_pose, pose_x, pose_y, pose_z);
}

bool mp_visionos_start_game(const char* disc_path) {
    if (g_running.exchange(true, std::memory_order_acq_rel)) {
        SetError("the game is already running");
        return false;
    }
    if (g_started.exchange(true, std::memory_order_acq_rel)) {
        // The game keeps process-wide state (its heaps, Aurora's device, static singletons of the
        // decompiled engine) that a second run in the same process would trip over.
        g_running.store(false, std::memory_order_release);
        SetError("the game already ran once since the app started; close the app and open it again to play");
        return false;
    }
    {
        std::lock_guard lock(g_mutex);
        g_exit_code = 0;
        g_last_error.clear();
        g_disc_path = disc_path != nullptr ? disc_path : "";
    }
    // The launcher always names the disc, and no UIKit window exists for the game's own picker.
    SetEnv("NO_DISC_DIALOG", "1");
    SetEnv("DISC", disc_path != nullptr && disc_path[0] != '\0' ? disc_path : nullptr);
    // VR is what the app is for; the log goes beside the saves, where the Files app shows it.
    SetEnv("VR", "1");
    SetEnv("LOG_FILE", "1");
    // main() is SwiftUI's, so SDL is told the platform initialisation it would do from SDL_main
    // already happened. The offscreen video driver Aurora picks on this platform
    // (extern/aurora/lib/window.cpp) needs nothing from UIKit.
    SDL_SetMainReady();
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, kGameThreadStackSize);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    const int result = pthread_create(&thread, &attributes, &GameThreadMain, nullptr);
    pthread_attr_destroy(&attributes);
    if (result != 0) {
        g_running.store(false, std::memory_order_release);
        g_started.store(false, std::memory_order_release);
        SetError(std::string("could not create the game thread: ") + std::strerror(result));
        return false;
    }
    return true;
}

bool mp_visionos_game_running(void) { return g_running.load(std::memory_order_acquire); }

int mp_visionos_exit_code(void) {
    std::lock_guard lock(g_mutex);
    return g_exit_code;
}

void mp_visionos_request_quit(void) {
    if (g_running.load(std::memory_order_acquire)) {
        PushQuit();
    }
}

void mp_visionos_request_quit_with_error(const char* error) {
    if (error != nullptr && error[0] != '\0') {
        SetError(error);
    }
    PushQuit();
}

bool mp_visionos_wait_for_exit(uint32_t timeout_ms) {
    std::unique_lock lock(g_mutex);
    return g_exited.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                             [] { return !g_running.load(std::memory_order_acquire); });
}

const char* mp_visionos_last_error(void) {
    // The provider's own last error (xr_visionos_last_error) is not reported here: it also records
    // failures the game recovers from. The log has it.
    std::lock_guard lock(g_mutex);
    g_error_storage = g_last_error;
    return g_error_storage.c_str();
}

} // extern "C"
