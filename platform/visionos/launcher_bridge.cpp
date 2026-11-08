// SPDX-License-Identifier: GPL-3.0-or-later
//
// visionos/launcher_bridge.h over launcher/core, as launcher/jni/primedgun_launcher_jni.cpp does for
// the Quest panel.

#include "visionos/launcher_bridge.h"

#include "disc_probe.h"
#include "launcher_keys.h"
#include "port_settings_file.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>

// The game, as the launcher's messages name it (Echoes' CMakeLists.txt defines its own).
#ifndef MP_VISIONOS_GAME_NAME
#define MP_VISIONOS_GAME_NAME "Metroid Prime"
#endif

namespace {

std::mutex g_mutex;
std::string g_last_error;
// Null-terminated copies of the key table's strings, made once per key.
std::unordered_map<std::string, std::pair<std::string, std::string>> g_key_strings;

void SetError(std::string message) {
    std::lock_guard lock(g_mutex);
    g_last_error = std::move(message);
}

void CopyOut(const std::string& value, char* out, size_t out_size) {
    if (out == nullptr || out_size == 0) {
        return;
    }
    const size_t length = std::min(value.size(), out_size - 1);
    std::memcpy(out, value.data(), length);
    out[length] = '\0';
}

std::filesystem::path Path(const char* path) { return std::filesystem::path(path != nullptr ? path : ""); }

} // namespace

extern "C" {

int mp_launcher_probe_disc(const char* path, char* game_id, size_t game_id_size, int* revision) {
    const PrimedGunLauncher::DiscInfo info = PrimedGunLauncher::ProbeDisc(Path(path));
    CopyOut(info.gameId, game_id, game_id_size);
    if (revision != nullptr) {
        *revision = info.revision;
    }
    return static_cast<int>(info.check);
}

const char* mp_launcher_disc_check_message(int check) {
    switch (check) {
    case MP_DISC_OK:
        return MP_VISIONOS_GAME_NAME ", ready to play.";
    case MP_DISC_WRONG_GAME:
        return "This image is not " MP_VISIONOS_GAME_NAME " for the GameCube.";
    case MP_DISC_WRONG_REVISION:
        return "This revision of " MP_VISIONOS_GAME_NAME " is not one the port plays (see the supported discs).";
    case MP_DISC_UNSUPPORTED_FORMAT:
        return "This image format is not one the port opens (see the supported discs).";
    case MP_DISC_UNVERIFIED:
        return "This image's header cannot be checked here; the game checks it when it starts.";
    default:
        return "The image could not be read.";
    }
}

bool mp_launcher_key_info(const char* key, MpLauncherKeyInfo* info) {
    if (key == nullptr || info == nullptr) {
        return false;
    }
    const PrimedGunLauncher::KeyInfo* found = PrimedGunLauncher::FindKey(key);
    if (found == nullptr) {
        return false;
    }
    std::lock_guard lock(g_mutex);
    auto [it, inserted] = g_key_strings.try_emplace(
        key, std::string(found->defaultValue), std::string(found->choices));
    info->kind = static_cast<int>(found->kind);
    info->default_value = it->second.first.c_str();
    info->minimum = found->uiMin;
    info->maximum = found->uiMax;
    info->step = found->uiStep;
    info->choices = it->second.second.c_str();
    return true;
}

bool mp_launcher_setting_get(const char* path, const char* key, char* out, size_t out_size) {
    if (key == nullptr) {
        return false;
    }
    const PrimedGunLauncher::KeyInfo* found = PrimedGunLauncher::FindKey(key);
    PrimedGunLauncher::PortSettingsFile file;
    if (!file.Load(Path(path))) {
        SetError(std::string("could not read ") + (path != nullptr ? path : "the settings file"));
        return false;
    }
    if (const auto value = file.Get(key)) {
        CopyOut(found != nullptr ? PrimedGunLauncher::Canonical(*found, *value) : *value, out, out_size);
        return true;
    }
    if (found == nullptr) {
        return false;
    }
    CopyOut(std::string(found->defaultValue), out, out_size);
    return true;
}

bool mp_launcher_setting_set(const char* path, const char* key, const char* value) {
    if (key == nullptr || value == nullptr) {
        SetError("no key or value");
        return false;
    }
    PrimedGunLauncher::PortSettingsFile file;
    if (!file.Load(Path(path))) {
        SetError(std::string("could not read ") + (path != nullptr ? path : "the settings file"));
        return false;
    }
    const PrimedGunLauncher::KeyInfo* found = PrimedGunLauncher::FindKey(key);
    file.Set(key, found != nullptr ? PrimedGunLauncher::Canonical(*found, value) : std::string(value));
    std::string error;
    if (!file.Save(Path(path), error)) {
        SetError(error);
        return false;
    }
    return true;
}

const char* mp_launcher_last_error(void) {
    std::lock_guard lock(g_mutex);
    return g_last_error.c_str();
}

} // extern "C"
