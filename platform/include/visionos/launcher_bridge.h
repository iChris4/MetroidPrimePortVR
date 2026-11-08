// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The Apple Vision Pro launcher's C view of the PrimedGun launcher core (launcher/core): the disc
// check and line-by-line edits of the game's settings file, the same code the PC launcher and the
// Quest panel (launcher/jni) use. Built into the game framework (CMakeLists.txt).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MP_LAUNCHER_API __attribute__((visibility("default")))

// PrimedGunLauncher::DiscCheck.
enum {
    MP_DISC_OK = 0,
    MP_DISC_WRONG_GAME = 1,
    MP_DISC_WRONG_REVISION = 2,
    MP_DISC_UNSUPPORTED_FORMAT = 3,
    MP_DISC_UNVERIFIED = 4,
    MP_DISC_UNREADABLE = 5,
};

// Checks the disc image at `path`. `game_id` (at least 7 bytes) gets the six-character id when the
// header was found ("" otherwise), `revision` the disc revision (-1 when unknown).
MP_LAUNCHER_API int mp_launcher_probe_disc(const char* path, char* game_id, size_t game_id_size, int* revision);
// A sentence for the player about a check's result.
MP_LAUNCHER_API const char* mp_launcher_disc_check_message(int check);

// MP_LAUNCHER_KEY_*: how a key's value is edited.
enum {
    MP_LAUNCHER_KEY_BOOL = 0,
    MP_LAUNCHER_KEY_FLOAT = 1,
    MP_LAUNCHER_KEY_INT = 2,
    MP_LAUNCHER_KEY_CHOICE = 3,
};

// A key the launcher edits (launcher/core/launcher_keys.cpp): its kind, its default as the game
// writes it, its slider's range and step, and for a choice the accepted names, comma separated.
// The strings stay valid for the process's lifetime. False for a key the launcher does not know.
typedef struct {
    int kind;
    const char* default_value;
    float minimum;
    float maximum;
    float step;
    const char* choices;
} MpLauncherKeyInfo;
MP_LAUNCHER_API bool mp_launcher_key_info(const char* key, MpLauncherKeyInfo* info);

// The value the game would read for `key` in the settings file at `path` (its default when no line
// sets it), copied into `out`. False when the file cannot be read or the key is unknown.
MP_LAUNCHER_API bool mp_launcher_setting_get(const char* path, const char* key, char* out, size_t out_size);
// Rewrites the line of `key` (or adds one) and saves the file, leaving every other byte alone.
// The value is normalised to the game's spelling first. False with mp_launcher_last_error set.
MP_LAUNCHER_API bool mp_launcher_setting_set(const char* path, const char* key, const char* value);

MP_LAUNCHER_API const char* mp_launcher_last_error(void);

#ifdef __cplusplus
}
#endif
