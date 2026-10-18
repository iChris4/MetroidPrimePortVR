// The configuration reset (port_config_reset.h): what it moves, what it leaves,
// that nothing is deleted or overwritten, and that a failure moves it all back.

#include "port_config_reset.h"
#include "port_test.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

void Write(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << text;
}

std::string Read(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  return std::string(std::istreambuf_iterator< char >(file), {});
}

void Populate(const fs::path& dir) {
  Write(dir / "port_settings.ini", "aspect=2\ndisc_path=/games/mp.iso\nremastered_nsp=/x/r.nsp\nvsync=0\nvr_cannon_texture_slot=3\n");
  Write(dir / "controls.toml", "toml");
  Write(dir / "imgui.ini", "imgui");
  Write(dir / "controller_ports.dat", "ports");
  Write(dir / "keyboard_bindings.dat", "kb");
  Write(dir / "Pad_045E_0B12.controller", "pad1");
  Write(dir / "GameCube Adapter_057E_0337.controller", "pad2");
  Write(dir / "MemoryCardA.USA.raw", "card");
  Write(dir / "data_folder.txt", "marker");
  Write(dir / "mods" / "a.CMDL", "mod");
  Write(dir / "savestates" / "s.bin", "state");
  Write(dir / "pipeline_cache.db", "cache");
  Write(dir / "notes.controller.bak", "not a mapping");
  Write(dir / "config-backup-old" / "port_settings.ini", "older backup");
}

} // namespace

int main(int argc, char** argv) {
  const fs::path root = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "port-config-reset-test";
  fs::remove_all(root);

  CHECK_MSG(PortConfigReset::IsResettable("port_settings.ini"), "settings allowed");
  CHECK_MSG(PortConfigReset::IsResettable("X_1_2.controller"), "controller mapping allowed");
  CHECK_MSG(!PortConfigReset::IsResettable(".controller"), "bare suffix refused");
  CHECK_MSG(!PortConfigReset::IsResettable("../port_settings.ini"), "path refused");
  CHECK_MSG(!PortConfigReset::IsResettable("a/b.controller"), "subfolder refused");
  CHECK_MSG(!PortConfigReset::IsResettable("MemoryCardA.USA.raw"), "card refused");
  CHECK_MSG(!PortConfigReset::IsResettable("mods"), "mods refused");
  CHECK_MSG(!PortConfigReset::IsResettable("data_folder.txt"), "data folder marker refused");

  // Success: the config moves, everything else stays.
  {
    const fs::path dir = root / "ok";
    Populate(dir);
    const auto r = PortConfigReset::Reset(dir);
    CHECK_MSG(r.ok && r.moved && r.error.empty(), "reset succeeds");
    const fs::path backup = fs::path(r.backupDir);
    CHECK_MSG(backup.parent_path() == dir, "backup is inside the user folder");
    CHECK_MSG(Read(backup / "controls.toml") == "toml", "controls.toml backed up");
    CHECK_MSG(Read(backup / "imgui.ini") == "imgui", "imgui.ini backed up");
    CHECK_MSG(Read(backup / "controller_ports.dat") == "ports", "ports backed up");
    CHECK_MSG(Read(backup / "keyboard_bindings.dat") == "kb", "keyboard bindings backed up");
    CHECK_MSG(Read(backup / "Pad_045E_0B12.controller") == "pad1", "controller 1 backed up");
    CHECK_MSG(Read(backup / "GameCube Adapter_057E_0337.controller") == "pad2", "controller 2 backed up");
    CHECK_MSG(Read(backup / "port_settings.ini").find("vsync=0") != std::string::npos, "full settings backed up");
    CHECK_MSG(!fs::exists(dir / "controls.toml") && !fs::exists(dir / "imgui.ini") &&
              !fs::exists(dir / "controller_ports.dat") && !fs::exists(dir / "keyboard_bindings.dat") &&
              !fs::exists(dir / "Pad_045E_0B12.controller"),
          "config gone from the user folder");
    CHECK_MSG(Read(dir / "port_settings.ini") ==
              "disc_path=/games/mp.iso\nremastered_nsp=/x/r.nsp\nvr_cannon_texture_slot=3\n",
          "only the disc paths and PrimedGun's cannon slot are kept in the new settings");
    // Preservation.
    CHECK_MSG(Read(dir / "MemoryCardA.USA.raw") == "card", "card untouched");
    CHECK_MSG(Read(dir / "data_folder.txt") == "marker", "marker untouched");
    CHECK_MSG(Read(dir / "mods" / "a.CMDL") == "mod", "mods untouched");
    CHECK_MSG(Read(dir / "savestates" / "s.bin") == "state", "save states untouched");
    CHECK_MSG(Read(dir / "pipeline_cache.db") == "cache", "cache untouched");
    CHECK_MSG(Read(dir / "notes.controller.bak") == "not a mapping", "non-mapping file untouched");
    CHECK_MSG(Read(dir / "config-backup-old" / "port_settings.ini") == "older backup", "older backup untouched");

    // A second reset gets its own backup and keeps the first.
    Write(dir / "controls.toml", "newer");
    const auto again = PortConfigReset::Reset(dir);
    CHECK_MSG(again.ok && again.moved && again.backupDir != r.backupDir, "second reset uses a new backup folder");
    CHECK_MSG(Read(backup / "controls.toml") == "toml", "first backup kept");
    CHECK_MSG(Read(fs::path(again.backupDir) / "controls.toml") == "newer", "second backup has the newer file");
  }

  // No configuration: nothing happens, no backup folder.
  {
    const fs::path dir = root / "none";
    Write(dir / "MemoryCardA.USA.raw", "card");
    const auto r = PortConfigReset::Reset(dir);
    CHECK_MSG(r.ok && !r.moved && r.backupDir.empty(), "no config is a no-op");
    size_t entries = 0;
    for ([[maybe_unused]] const auto& e : fs::directory_iterator(dir)) {
      ++entries;
    }
    CHECK_MSG(entries == 1, "no backup folder created");
    CHECK_MSG(!PortConfigReset::Reset(root / "missing").ok, "missing user folder is an error");
  }

  // Settings without the disc keys: no fresh settings file.
  {
    const fs::path dir = root / "nodisc";
    Write(dir / "port_settings.ini", "aspect=2\n");
    const auto r = PortConfigReset::Reset(dir);
    CHECK_MSG(r.ok && r.moved && !fs::exists(dir / "port_settings.ini"), "no kept keys, no new settings file");
  }

#ifndef _WIN32
  // Symlinks: refused, and nothing moved.
  {
    const fs::path dir = root / "link";
    Populate(dir);
    Write(root / "outside.toml", "outside");
    fs::remove(dir / "controls.toml");
    fs::create_symlink(root / "outside.toml", dir / "controls.toml");
    const auto r = PortConfigReset::Reset(dir);
    CHECK_MSG(!r.ok && !r.moved && !r.error.empty(), "symlinked config refused");
    CHECK_MSG(Read(dir / "imgui.ini") == "imgui" && fs::exists(dir / "port_settings.ini"), "nothing moved on refusal");
    CHECK_MSG(Read(root / "outside.toml") == "outside", "link target untouched");
    size_t backups = 0;
    for (const auto& e : fs::directory_iterator(dir)) {
      if (e.path().filename().string().rfind("config-backup-", 0) == 0) {
        ++backups;
      }
    }
    CHECK_MSG(backups == 1, "no new backup folder on refusal (only the old one)");
    fs::remove(dir / "controls.toml");
    fs::create_symlink(root / "outside.toml", dir / "Pad_link.controller");
    CHECK_MSG(!PortConfigReset::Reset(dir).ok, "symlinked mapping refused");
  }

  // A symlinked user folder itself is fine.
  {
    const fs::path real = root / "real";
    Write(real / "controls.toml", "toml");
    fs::create_directory_symlink(real, root / "linked-user");
    const auto r = PortConfigReset::Reset(root / "linked-user");
    CHECK_MSG(r.ok && r.moved, "symlinked user folder works");
  }

  // Rollback: a read-only folder cannot be written to; use a failing fresh-settings
  // write by making the second move fail instead. A directory named like a
  // mapping is refused as not regular (nothing moved).
  {
    const fs::path dir = root / "dirname";
    Populate(dir);
    fs::create_directories(dir / "Weird.controller");
    const auto r = PortConfigReset::Reset(dir);
    CHECK_MSG(!r.ok && !r.moved, "non-regular config refused");
    CHECK_MSG(Read(dir / "controls.toml") == "toml", "nothing moved on refusal 2");
  }

  // Rollback: the third move fails, the first two go back.
  {
    const fs::path dir = root / "rollback";
    Populate(dir);
    int moves = 0;
    const auto r = PortConfigReset::Reset(dir, [&](const std::string&) { return ++moves < 3; });
    CHECK_MSG(!r.ok && !r.moved && r.backupDir.empty() && !r.error.empty(), "failed move reports an error");
    CHECK_MSG(Read(dir / "port_settings.ini").find("vsync=0") != std::string::npos, "settings restored in full");
    CHECK_MSG(Read(dir / "controls.toml") == "toml" && Read(dir / "imgui.ini") == "imgui" &&
                  Read(dir / "controller_ports.dat") == "ports" && Read(dir / "Pad_045E_0B12.controller") == "pad1",
              "every file back in place");
    size_t backups = 0;
    for (const auto& e : fs::directory_iterator(dir)) {
      if (e.path().filename().string().rfind("config-backup-", 0) == 0) {
        ++backups;
      }
    }
    CHECK_MSG(backups == 1, "the failed backup folder is removed (only the old one remains)");
    const auto retry = PortConfigReset::Reset(dir);
    CHECK_MSG(retry.ok && retry.moved, "a retry after the failure works");
  }
  // Rollback that itself fails: the stuck file stays in the kept backup folder.
  {
    const fs::path dir = root / "stuck";
    Populate(dir);
    int moves = 0;
    const auto r = PortConfigReset::Reset(
        dir, [&](const std::string&) { return ++moves < 4; },
        [](const std::string& name) { return name != "controls.toml"; });
    CHECK_MSG(!r.ok && !r.moved && !r.backupDir.empty(), "incomplete rollback keeps the backup path");
    CHECK_MSG(r.files.size() == 1 && r.files[0] == "controls.toml", "stuck file reported");
    CHECK_MSG(r.error.find("could not restore controls.toml") != std::string::npos, "stuck file in the error");
    CHECK_MSG(Read(fs::path(r.backupDir) / "controls.toml") == "toml", "stuck file is in the backup");
    CHECK_MSG(Read(dir / "imgui.ini") == "imgui", "the others are restored");
  }

  // Unreadable settings (skipped when permissions don't apply, e.g. running as root).
  {
    const fs::path dir = root / "unreadable";
    Populate(dir);
    fs::permissions(dir / "port_settings.ini", fs::perms::none);
    std::ifstream probe(dir / "port_settings.ini");
    if (!probe) {
      const auto r = PortConfigReset::Reset(dir);
      fs::permissions(dir / "port_settings.ini", fs::perms::owner_read | fs::perms::owner_write);
      CHECK_MSG(!r.ok && !r.moved && r.backupDir.empty(), "unreadable settings: reset fails");
      CHECK_MSG(Read(dir / "port_settings.ini").find("disc_path") != std::string::npos &&
                    Read(dir / "controls.toml") == "toml",
                "unreadable settings: everything restored");
    } else {
      fs::permissions(dir / "port_settings.ini", fs::perms::owner_read | fs::perms::owner_write);
    }
  }
#endif

  if (port_test::Failures() == 0) {
    std::puts("port_config_reset_tests: ok");
  }
  return PORT_TEST_RESULT();
}
