// The Dread Samus import in the game (port_remastered_import.h, StartDreadSamusImport): the user's
// Metroid Dread image (or, for development, an extracted RomFS in MP_DREAD_ROMFS) and the open disc,
// through PortDreadSamus::Import (port_dread_samus.h), into <mods>/dread-samus. It shares the
// Remastered import's state, thread and cancel flag, so the panel's progress, notes and mods
// unload/reload serve both; only one of the two runs at a time.

#include "port_remastered_import_internal.h"

#include "port_dread_samus.h"

namespace PortRemastered {
namespace ImportDetail {
namespace {

constexpr const char* kDreadStaging = ".dread-samus.staging";
constexpr const char* kDreadOld = ".dread-samus.old";
constexpr const char* kDreadDone = ".done";  // in the staging folder once it is complete

fs::path Utf8Path(const std::string& text) { return fs::path(std::u8string(text.begin(), text.end())); }

std::string Utf8(const fs::path& path) {
  const std::u8string text = path.u8string();
  return std::string(text.begin(), text.end());
}

bool WriteFile(const fs::path& path, const std::vector<uint8_t>& data) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
  return bool(out);
}

// The finished staging folder in place of <mods>/dread-samus. False (staging kept, for the next
// start's ApplyPendingImport) when a rename fails, as it can on Windows with a file still open.
bool MoveDreadIntoPlace(const fs::path& mods) {
  std::error_code ec;
  const fs::path staging = mods / kDreadStaging;
  if (!fs::exists(staging / kDreadDone, ec)) {
    return false;
  }
  const fs::path target = mods / PortDreadSamus::kModName;
  const fs::path old = mods / kDreadOld;
  fs::remove_all(old, ec);
  ec.clear();
  const bool had = fs::exists(target, ec);
  if (had) {
    fs::rename(target, old, ec);
    if (ec) {
      PortLog::Write("dread: could not set aside %s: %s\n", Utf8(target).c_str(), ec.message().c_str());
      return false;
    }
  }
  fs::rename(staging, target, ec);
  if (ec) {
    PortLog::Write("dread: could not move the import to %s: %s\n", Utf8(target).c_str(), ec.message().c_str());
    if (had) {
      std::error_code restore;
      fs::rename(old, target, restore);
    }
    return false;
  }
  fs::remove(target / kDreadDone, ec);
  fs::remove_all(old, ec);
  return true;
}

void RunDread(std::string nspPath, std::string keysPath, std::string romfsDir, fs::path mods) {
  auto fail = [](const std::string& message) { Finish(false, sCancel ? std::string("Cancelled.") : message); };
  SetMessage("Opening the Dread image");
  std::string error;
  Nsp nsp;
  if (romfsDir.empty()) {
    if (!nsp.Open(nspPath, keysPath, error)) {
      fail(error);
      return;
    }
    if (nsp.ProgramId() != kDreadProgramId) {
      char text[128];
      std::snprintf(text, sizeof(text), "This is not Metroid Dread (title %016llX).",
                    static_cast<unsigned long long>(nsp.ProgramId()));
      fail(text);
      return;
    }
  }
  SetMessage("Reading the disc");
  Retail retail;
  if (!retail.Index(error)) {
    fail(error);
    return;
  }
  const fs::path staging = mods / kDreadStaging;
  std::error_code ec;
  fs::remove_all(staging, ec);
  ec.clear();
  fs::create_directories(staging, ec);
  if (ec) {
    fail("Cannot write " + Utf8(staging) + ": " + ec.message());
    return;
  }
  PortDreadSamus::Io io;
  if (romfsDir.empty()) {
    io.dread = [&nsp](const std::string& path, std::vector<uint8_t>& out, std::string& why) {
      const RomfsFile* file = nsp.Find(path);
      if (file == nullptr) {
        why = "not in the image";
        return false;
      }
      out.resize(size_t(file->size));
      return nsp.Read(*file, 0, out.data(), out.size(), why);
    };
  } else {
    const fs::path root = Utf8Path(romfsDir);
    io.dread = [root](const std::string& path, std::vector<uint8_t>& out, std::string& why) {
      std::ifstream in(root / Utf8Path(path), std::ios::binary);
      if (!in) {
        why = "not in " + Utf8(root);
        return false;
      }
      out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
      return true;
    };
  }
  io.retail = [&retail](uint32_t type, uint32_t id, std::vector<uint8_t>& out) { return retail.Read(type, id, out); };
  io.retailId = [&retail](uint32_t id) { return retail.HasId(id); };
  io.write = [&staging](const std::string& name, const std::vector<uint8_t>& data) {
    return WriteFile(staging / Utf8Path(name), data);
  };
  io.log = [](const std::string& line) {
    PortLog::Write("%s\n", line.c_str());
    AddLine(line);
  };
  io.detail = [](const std::string& line) { PortLog::Write("%s\n", line.c_str()); };
  io.cancelled = [] { return sCancel.load(); };
  io.progress = [](size_t done, size_t total) {
    {
      std::lock_guard<std::mutex> lock(sStateMutex);
      sState.done = int(done);
      sState.total = int(total);
    }
    SetMessage("Converting Samus's models (" + std::to_string(done) + " of " + std::to_string(total) + ")");
  };
  PortDreadSamus::Result result;
  const bool ok = PortDreadSamus::Import(io, result, error);
  {
    std::lock_guard<std::mutex> lock(sStateMutex);
    sState.failed = int(result.failed);
  }
  if (!ok) {
    fs::remove_all(staging, ec);
    fail(error);
    return;
  }
  const std::string stamp = std::to_string(PortDreadSamus::kVersion) + "\n" + std::string(MP_BUILD_REVISION) + "\n";
  if (!WriteFile(staging / PortDreadSamus::kStampName, std::vector<uint8_t>(stamp.begin(), stamp.end())) ||
      !WriteFile(staging / kDreadDone, {})) {
    fs::remove_all(staging, ec);
    fail("Cannot write " + Utf8(staging));
    return;
  }
  if (sCancel.load()) {
    fs::remove_all(staging, ec);
    fail("cancelled");
    return;
  }
  const bool placed = MoveDreadIntoPlace(mods);
  std::string message = std::to_string(result.converted) + " Samus models converted";
  if (result.failed > 0) {
    message += ", " + std::to_string(result.failed) + " failed";
  }
  message += placed ? "." : "; it is installed at the next start.";
  Finish(true, message);
}

}  // namespace

bool ApplyPendingDreadImport() {
  const std::string mods = PortMods::Folder();
  return !mods.empty() && MoveDreadIntoPlace(Utf8Path(mods));
}

}  // namespace ImportDetail

using namespace ImportDetail;

bool StartDreadSamusImport(const std::string& nspPath, const std::string& keysPath) {
  {
    std::lock_guard<std::mutex> lock(sStateMutex);
    if (sState.running) {
      return false;
    }
  }
  if (sThread.joinable()) {
    sThread.join();
  }
  const char* dev = port::EnvString("MP_DREAD_ROMFS");
  const std::string romfs = dev != nullptr ? dev : "";
  const std::string mods = PortMods::Folder();
  std::lock_guard<std::mutex> lock(sStateMutex);
  sState = {};
  if (mods.empty()) {
    sState.finished = true;
    sState.message = "There is no mods folder to write to.";
    return false;
  }
  if (romfs.empty() && (nspPath.empty() || keysPath.empty())) {
    sState.finished = true;
    sState.message = "Pick the Metroid Dread image and the key file.";
    return false;
  }
  bool bc = false, astc = false;
  aurora_get_texture_support(&bc, &astc);
  SetGpuTextureSupport(bc, astc);
  PortLog::Write("dread: importing Samus from %s, %s textures\n", romfs.empty() ? "the image" : romfs.c_str(),
                 TextureFormatName());
  sCancel = false;
  sState.running = true;
  sState.message = "Starting";
  sThread = std::thread(RunDread, nspPath, keysPath, romfs, Utf8Path(mods));
  return true;
}

int RunDreadSamusImportFromCommandLine(const std::string& nspPath, const std::string& keysPath) {
  const std::string keys = keysPath.empty() ? DefaultKeysPath() : keysPath;
  if (!StartDreadSamusImport(nspPath, keys)) {
    std::fprintf(stderr, "%s\n", ImportStatus().message.c_str());
    return 1;
  }
  std::string shown;
  for (;;) {
    const ImportState state = ImportStatus();
    if (state.message != shown) {
      shown = state.message;
      std::printf("%s\n", shown.c_str());
      std::fflush(stdout);
    }
    if (!state.running) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  StopImport();
  const ImportState state = ImportStatus();
  for (const std::string& line : state.lines) {
    std::printf("  %s\n", line.c_str());
  }
  return state.ok ? 0 : 1;
}

}  // namespace PortRemastered
