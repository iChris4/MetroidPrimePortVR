// The data folder move's copy engine (port_data_folder.h): what it leaves out,
// that it replaces what is there, and that a cancelled copy leaves nothing
// half-written.

#include "port_data_folder.h"
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

} // namespace

int main(int argc, char** argv) {
  const fs::path root = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "port-data-folder-test";
  fs::remove_all(root);
  const fs::path from = root / "private";
  const fs::path to = root / "shared";

  CHECK_MSG(PortDataFolder::IsSkipped("textures"), "built-in textures skipped");
  CHECK_MSG(PortDataFolder::IsSkipped("textures/xbox/a.png"), "inside the built-in textures skipped");
  CHECK_MSG(PortDataFolder::IsSkipped("initial_pipeline_cache.db"), "initial pipeline cache skipped");
  CHECK_MSG(PortDataFolder::IsSkipped("data_folder.txt"), "marker skipped");
  CHECK_MSG(PortDataFolder::IsSkipped("user_textures.partial"), "texture pack staging skipped");
  CHECK_MSG(PortDataFolder::IsSkipped("disc.iso.part"), "partial disc copy skipped");
  CHECK_MSG(PortDataFolder::IsSkipped("mods/a.CMDL.mpcopy"), "half-copied file skipped");
  CHECK_MSG(!PortDataFolder::IsSkipped("user_textures/textures/a.png"), "a pack's own textures folder kept");
  CHECK_MSG(!PortDataFolder::IsSkipped("mods/textures"), "a nested textures folder kept");
  CHECK_MSG(!PortDataFolder::IsSkipped("port_settings.ini"), "settings kept");

  CHECK_MSG(!PortDataFolder::HasData((from).string()), "a missing folder holds no data");
  Write(from / "textures" / "xbox" / "a.png", "built-in");
  Write(from / "initial_pipeline_cache.db", "cache");
  CHECK_MSG(!PortDataFolder::HasData(from.string()), "only skipped files is no data");

  Write(from / "port_settings.ini", "settings");
  Write(from / "MemoryCardA.USA.raw", std::string(3 << 20, 'c'));
  Write(from / "mods" / "remastered-models" / "1234.CMDL", "model");
  Write(from / "user_textures" / "textures" / "b.png", "pack");
  Write(to / "port_settings.ini", "old settings");

  const PortDataFolder::Totals totals = PortDataFolder::Measure(from.string());
  CHECK_MSG(totals.files == 4, "four files to move");
  CHECK_MSG(totals.bytes == 8 + (3 << 20) + 5 + 4, "their size");
  CHECK_MSG(PortDataFolder::HasData(to.string()), "a folder with settings holds data");

  PortDataFolder::Progress progress;
  std::string error;
  CHECK_MSG(PortDataFolder::CopyTree(from.string(), to.string(), progress, error), "copy succeeds");
  CHECK_MSG(progress.filesDone == 4 && progress.bytesDone == totals.bytes, "progress counts everything");
  CHECK_MSG(Read(to / "port_settings.ini") == "settings", "existing file replaced");
  CHECK_MSG(Read(to / "mods" / "remastered-models" / "1234.CMDL") == "model", "nested file copied");
  CHECK_MSG(fs::file_size(to / "MemoryCardA.USA.raw") == (3u << 20), "large file copied whole");
  CHECK_MSG(!fs::exists(to / "textures"), "built-in textures not copied");
  CHECK_MSG(!fs::exists(to / "initial_pipeline_cache.db"), "pipeline cache not copied");

  const fs::path cancelled = root / "cancelled";
  PortDataFolder::Progress stop;
  stop.cancel = true;
  CHECK_MSG(!PortDataFolder::CopyTree(from.string(), cancelled.string(), stop, error), "cancelled copy fails");
  bool leftovers = false;
  if (fs::exists(cancelled)) {
    for (const auto& entry : fs::recursive_directory_iterator(cancelled)) {
      leftovers |= entry.is_regular_file() && entry.path().filename() != ".mpcopy_incomplete";
    }
  }
  CHECK_MSG(!leftovers, "cancelled copy leaves no files");
  CHECK_MSG(!PortDataFolder::IsComplete(cancelled.string()), "cancelled copy marked incomplete");
  CHECK_MSG(PortDataFolder::IsComplete(to.string()), "finished copy complete");
  CHECK_MSG(!PortDataFolder::HasData(cancelled.string()), "the incomplete flag is not data");

  // The pass before the switch copies only what changed since.
  Write(from / "port_settings.ini", "newer settings");
  PortDataFolder::Progress sync;
  CHECK_MSG(PortDataFolder::CopyTree(from.string(), to.string(), sync, error, true), "sync succeeds");
  CHECK_MSG(Read(to / "port_settings.ini") == "newer settings", "changed file re-copied");
  CHECK_MSG(sync.filesDone == 4 && sync.bytesDone == 14, "unchanged files skipped");
  CHECK_MSG(PortDataFolder::IsComplete(to.string()), "synced copy complete");

  CHECK_MSG(PortDataFolder::IsSkipped("pipeline_cache.db"), "shader cache skipped");
  CHECK_MSG(PortDataFolder::IsSkipped("pipeline_cache.db-wal"), "shader cache journal skipped");
  CHECK_MSG(PortDataFolder::IsSkipped("dawn_cache.db"), "Dawn cache skipped");
  CHECK_MSG(PortDataFolder::IsSkipped("texture_dumps/a.png"), "texture dumps skipped");

  // The built-in Remastered import belongs to the app, not to the data folder
  // (port_remastered_storage.h), also when the data folder is the app's own storage, which is
  // where the import sits: it is neither counted, copied nor deleted with the data.
  Write(from / "remastered-imports" / "remastered-models" / "1234.CMDL", "private import");
  Write(from / "remastered-imports" / ".remastered-models.importing" / "part.CMDL", "half an import");
  CHECK_MSG(PortDataFolder::IsSkipped("remastered-imports"), "the import's root skipped");
  CHECK_MSG(PortDataFolder::IsSkipped("remastered-imports/remastered-models/1234.CMDL"), "inside the import skipped");
  CHECK_MSG(PortDataFolder::IsSkipped("remastered-imports/.remastered-models.importing/part.CMDL"),
            "a staged import skipped");
  CHECK_MSG(PortDataFolder::Measure(from.string()).files == totals.files, "the import is not data to move");
  PortDataFolder::Progress afterImport;
  CHECK_MSG(PortDataFolder::CopyTree(from.string(), to.string(), afterImport, error, true), "the next copy succeeds");
  CHECK_MSG(afterImport.bytesDone == 0 && afterImport.filesDone == totals.files, "the import is not copied");
  CHECK_MSG(!fs::exists(to / "remastered-imports"), "nothing of the import reaches the other folder");

  Write(from / "disc.iso", "disc");
  PortDataFolder::DeleteData(from.string(), [](const std::string& rel) { return rel == "disc.iso"; });
  CHECK_MSG(Read(from / "disc.iso") == "disc", "kept file survives the delete");
  PortDataFolder::DeleteData(from.string());
  CHECK_MSG(!PortDataFolder::HasData(from.string()), "old copy deleted");
  CHECK_MSG(fs::exists(from / "textures" / "xbox" / "a.png"), "built-in textures survive the delete");
  CHECK_MSG(fs::exists(from / "initial_pipeline_cache.db"), "pipeline cache survives the delete");
  CHECK_MSG(!fs::exists(from / "mods"), "emptied folders removed");
  CHECK_MSG(Read(from / "remastered-imports" / "remastered-models" / "1234.CMDL") == "private import",
            "the app's own import survives the delete");
  CHECK_MSG(fs::exists(from / "remastered-imports" / ".remastered-models.importing" / "part.CMDL"),
            "a staged import survives the delete");

  fs::remove_all(root);
  if (port_test::Failures() == 0) {
    std::printf("port_data_folder_tests: all passed\n");
  }
  return PORT_TEST_RESULT();
}
