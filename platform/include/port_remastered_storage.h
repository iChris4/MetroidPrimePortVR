#pragma once

// Where the built-in Remastered import lives, and how the mod scan picks it up.
//
// Desktop: in the mods folder, as it has always been - <mods>/remastered-models, staged in
// <mods>/.remastered-models.importing. MP_MODS and MP_USER_PATH move it with everything else and
// nothing here changes that.
//
// Android: in the app's own internal storage, <internal>/remastered-imports/{remastered-models,
// .remastered-models.importing, .remastered-models.importing.held, .remastered-models.old,
// .remastered-models.trash-*}. The mods folder there is in the data folder, which the player may
// have moved to the shared folder on the card (port_data_folder.h) to add mods with a file
// manager: in shared storage every file of an import goes through Android's media provider and a
// folder move is a file-by-file walk, which is where an install spent its time. Beside the app the
// same import is plain file-system writes, and its install is one rename. The import is the app's
// own data and stays with it: it follows neither
// MP_USER_PATH nor MP_MODS, and the data folder setting (port_data_folder.h) leaves it where it is,
// including when the data folder is the app's private storage itself. Android's backup does not take
// it (the manifest sets android:allowBackup="false"); an APK update keeps it, and an uninstall (unless
// the uninstall dialog is asked to keep the app's data) or a "clear data" deletes it. When the app's
// internal storage cannot be reached the import is not read or written at all, with a clear reason -
// never through the shared mods folder.
//
// Shared (user) mods keep loading from the mods folder exactly as before, and a scan loads both:
// the built-in import takes part in the same name order (a later name wins), and a shared mod of
// the same name never shadows it. A legacy <mods>/remastered-models from before this - or a first
// private import - keeps standing in until a full import has written the private one: it is read
// (a stage's files are hard-linked, or copied where the file system has no links, and it is never
// modified) and nothing migrates. Once the private import is installed, usable and not being
// written, the legacy copy is a dead duplicate (the private one shadows it) and the app deletes it
// at startup (ShouldDeleteLegacy); only a real folder is removed, never a symlink, and never the
// private import itself. Desktop never deletes: there the import is <mods>/remastered-models.
//
// Header-only and free of platform calls, so the policy is tested on the host
// (tests/port_remastered_storage.cpp).

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace PortRemasteredStorage {

// --- Names ------------------------------------------------------------------

// The folder inside Android's internal storage holding the import's own folders.
inline constexpr const char* kRootFolderName = "remastered-imports";
// The mod a finished import is installed as.
inline constexpr const char* kInstalledModName = "remastered-models";
// Where an import is made; the mod loader skips it (a leading dot), and the marker inside says
// whether the run finished.
inline constexpr const char* kStagingName = ".remastered-models.importing";
// A finished import set aside while the next one reuses its files.
inline constexpr const char* kHeldName = ".remastered-models.importing.held";
// The installed mod kept as a backup while a new one takes its place.
inline constexpr const char* kBackupName = ".remastered-models.old";
// An old import on its way out; deleted on a thread (port_remastered_import.cpp).
inline constexpr const char* kTrashPrefix = ".remastered-models.trash-";

inline std::string WithSeparator(std::string dir) {
  if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') {
    dir += '/';
  }
  return dir;
}

// --- The root ---------------------------------------------------------------

enum class Root {
  // The mods folder: the import is one of the mods there (desktop).
  kModsFolder,
  // The app's internal storage, under kRootFolderName (Android).
  kPrivateStorage,
};

struct RootChoice {
  Root kind = Root::kModsFolder;
  // The folder the import's own folders sit in, with a trailing separator; empty when there is none.
  std::string folder;
  // Why there is none; shown to the player (and the import's message).
  std::string error;
};

// Works out the root from the two things the platform knows: whether this is Android, the app's
// internal storage (SDL_GetAndroidInternalStoragePath), and the mods folder shared mods live in.
// Desktop keeps the mods folder whatever the strings say. Android always answers with the app's
// storage: with no internal storage the result is empty and `error` says so, never the mods folder.
inline RootChoice ResolveRoot(bool android, const std::string& internalStorage, const std::string& modsFolder) {
  RootChoice choice;
  if (!android) {
    choice.kind = Root::kModsFolder;
    choice.folder = WithSeparator(modsFolder);
    return choice;
  }
  choice.kind = Root::kPrivateStorage;
  if (internalStorage.empty()) {
    choice.error = "The app's internal storage is not available, so the Remastered import cannot be read or "
                   "written. Close and open the app again; the models have to be imported into the app's own "
                   "storage (a full import).";
    return choice;
  }
  choice.folder = WithSeparator(internalStorage) + kRootFolderName + "/";
  return choice;
}

// --- The folders inside a root ----------------------------------------------

inline std::string StagingFolder(const std::string& root) { return root + kStagingName; }
inline std::string HeldFolder(const std::string& root) { return root + kHeldName; }
inline std::string InstalledFolder(const std::string& root) { return root + kInstalledModName; }
inline std::string BackupFolder(const std::string& root) { return root + kBackupName; }

// --- The mod scan -----------------------------------------------------------

// One folder the scan loads as a mod.
struct ModFolder {
  std::string name;
  std::string path;
  bool builtin = false;  // the built-in import, which lives outside the mods folder
};

// The built-in barrier now uses roomfx effects; never let its obsolete platform
// override replace the disc fallback. Other mods' deliberate replacements stay.
inline bool RejectObsoleteBarrierModel(bool builtin, uint32_t type, uint32_t id) {
  return builtin && type == 0x434D444C && id == 0x6556EEC5;
}

inline std::filesystem::path PathFromUtf8(const std::string& text) {
  return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

inline std::string PathToUtf8(const std::filesystem::path& path) {
  const std::u8string text = path.u8string();
  return std::string(text.begin(), text.end());
}

// The last part of a path, with any trailing separators ignored.
inline std::string FileName(const std::string& path) {
  size_t end = path.size();
  while (end > 0 && (path[end - 1] == '/' || path[end - 1] == '\\')) {
    --end;
  }
  const size_t slash = path.find_last_of("/\\", end == 0 ? 0 : end - 1);
  return path.substr(slash == std::string::npos ? 0 : slash + 1, end - (slash == std::string::npos ? 0 : slash + 1));
}

// A folder only counts as a mod when its name does not start with a dot, which is how the scan has
// always left the import's own working folders (staging, held, backup, trash) alone.
inline bool IsModFolderName(const std::string& name) { return !name.empty() && name[0] != '.'; }

// Where a mod goes in the order apart from its name: the Dread Samus import ("dread-samus",
// port_dread_samus_mod.h) loads after every other mod, so its bodies win over the Remastered
// import's Samus whatever the names. Every other mod is 0.
inline int ModPrecedence(const std::string& name) { return name == "dread-samus" ? 1 : 0; }

// The folders a scan loads, in the order their files are applied: the mods folder's own folders plus
// the built-in import when there is one (`builtinPath`, empty when not). A shared folder of the same
// name is dropped, so the app's own import wins it; everything else keeps name order, so a later name
// still overrides an earlier one exactly as it did when the mods folder was all there was. A hidden
// folder is never a mod, so a staging folder handed in by mistake stays out of the scan.
inline std::vector<ModFolder> OrderMods(std::vector<ModFolder> shared, const std::string& builtinPath) {
  std::vector<ModFolder> out;
  const std::string builtinName = builtinPath.empty() ? std::string() : FileName(builtinPath);
  const bool hasBuiltin = IsModFolderName(builtinName);
  for (ModFolder& mod : shared) {
    if (!IsModFolderName(mod.name)) {
      continue;
    }
    if (hasBuiltin && mod.name == builtinName) {
      continue;  // the app's own import has that name
    }
    out.push_back(std::move(mod));
  }
  if (hasBuiltin) {
    out.push_back(ModFolder{builtinName, builtinPath, true});
  }
  std::stable_sort(out.begin(), out.end(), [](const ModFolder& a, const ModFolder& b) {
    return ModPrecedence(a.name) != ModPrecedence(b.name) ? ModPrecedence(a.name) < ModPrecedence(b.name)
                                                          : a.name < b.name;
  });
  return out;
}

// The filesystem half of the scan: the mod folders under `sharedFolder` (its own subfolders, hidden
// ones left out) with the built-in import merged in by OrderMods. A folder that cannot be listed is
// simply not there. `builtinPath` is the installed import the caller found, not a staging one.
inline std::vector<ModFolder> ScanFolders(const std::string& sharedFolder, const std::string& builtinPath) {
  std::vector<ModFolder> shared;
  std::error_code ec;
  if (!sharedFolder.empty()) {
    const std::filesystem::path base = PathFromUtf8(sharedFolder);
    std::filesystem::directory_iterator it(base, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
      std::error_code entryEc;
      if (!it->is_directory(entryEc)) {
        continue;
      }
      const std::string name = PathToUtf8(it->path().filename());
      if (!IsModFolderName(name)) {
        continue;
      }
      shared.push_back(ModFolder{name, PathToUtf8(it->path()), false});
    }
  }
  return OrderMods(std::move(shared), builtinPath);
}

// Which folder a run reuses its unchanged stages from: the installed import, else - when it has
// none, or nothing usable to read - the legacy one in the shared mods folder, which is only read.
// Empty when neither is a source, and the run converts everything.
inline std::string ReuseFolder(const std::string& installedFolder, bool installedUsable,
                               const std::string& legacyFolder, bool legacyUsable) {
  if (installedUsable) {
    return installedFolder;
  }
  return legacyUsable ? legacyFolder : std::string();
}

// Whether the legacy <mods>/remastered-models is deleted at startup: only with the private root, when
// the private import is installed and usable (and not being written), and the legacy path is a real
// folder (not a symlink) that is not the private import itself.
inline bool ShouldDeleteLegacy(bool privateRoot, bool installedUsable, bool legacyIsRealDir, bool samePath) {
  return privateRoot && installedUsable && legacyIsRealDir && !samePath;
}

// What a movies-only import writes into: the staged (finished but not yet installed) import is the
// newer one, else the installed mod. `legacyFolder` is the shared mods folder's own import: on
// Android it is never written to - the movies belong in the app's copy of the import - so a player
// who only has it is asked for a full import instead. Nothing is written and `error` says why when
// there is no target.
struct MovieTarget {
  std::string folder;
  std::string error;
};

inline MovieTarget ChooseMovieTarget(bool privateRoot, const std::string& stagedFolder, bool hasStaged,
                                     const std::string& installedFolder, bool hasInstalled,
                                     const std::string& legacyFolder, bool hasLegacy) {
  if (hasStaged) {
    return {stagedFolder, {}};
  }
  if (hasInstalled) {
    return {installedFolder, {}};
  }
  if (privateRoot && hasLegacy) {
    return {{},
            "The Remastered models are still in the shared mods folder. The movies are written into the app's "
            "own copy of the import, so import the models again (a full import) and then the movies."};
  }
  return {{}, "Import the models first: the movies go into that mod."};
}

}  // namespace PortRemasteredStorage
