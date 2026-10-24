#include "port_disc_replace.h"

#include "port_debug.h"
#include "port_disc.h"
#include "port_disc_verify.h"
#include "port_log.h"
#include "port_paths.h"

#include <aurora/aurora.h>
#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_video.h>

#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>


namespace PortDiscReplace {

namespace {

std::mutex sMutex;
std::mutex sThreadMutex; // Start and Shutdown
Status sStatus;
bool sApplied = true;
std::thread sThread;
std::atomic<bool> sCancel{false};
std::atomic<uint64_t> sDone{0};
std::atomic<uint64_t> sTotal{0};

bool IsContentUri(const std::string& s) { return s.compare(0, 10, "content://") == 0; }

// Containers keep the disc header elsewhere than the first bytes.
bool IsContainer(const uint8_t* h) {
  return std::memcmp(h, "RVZ", 3) == 0 || std::memcmp(h, "WIA", 3) == 0 || std::memcmp(h, "WBFS", 4) == 0 ||
         std::memcmp(h, "CISO", 4) == 0 || (h[0] == 0x01 && h[1] == 0xC0 && h[2] == 0x0B && h[3] == 0xB1);
}

void Finish(State state, const std::string& message, const std::string& path = {}, const std::string& revision = {}) {
  std::lock_guard lock(sMutex);
  sStatus.state = state;
  sStatus.copying = false;
  sStatus.message = message;
  sStatus.path = path;
  sStatus.revision = revision;
  sApplied = state != State::Done;
}

void Run(std::string picked) {
  std::string revision;
  if (const std::string bad = CheckImage(picked, &revision); !bad.empty()) {
    PortLog::Write("metroid_prime_port: not using the picked disc image: %s\n", bad.c_str());
    return Finish(State::Failed, bad);
  }
  std::string path = picked;
  // Only a content:// address needs the copy; a path is remembered as it is.
  if (IsContentUri(picked)) {
    {
      std::lock_guard lock(sMutex);
      sStatus.copying = true;
    }
    if (const std::string bad = CopyToUserFolder(picked, &path, &sDone, &sTotal, &sCancel); !bad.empty()) {
      return Finish(State::Failed, sCancel ? "Cancelled; the disc image is unchanged." : bad);
    }
  }
  Finish(State::Done, "Restart the game to use the new disc image.", path, revision);
}

} // namespace

bool IsSupportedId(const char* id6, unsigned diskNumber, unsigned version) {
  return PortDisc::IsAccepted(PortDisc::Identify(id6, diskNumber, version));
}

std::string DescribeDisc(const char* id6, unsigned version) {
  char id[7] = {};
  for (int i = 0; i < 6; ++i) {
    const char c = id6[i];
    id[i] = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ? c : '?';
  }
  if (std::memcmp(id, "GM8", 3) == 0) {
    const char* region = id[3] == 'E' ? "USA" : id[3] == 'P' ? "European" : id[3] == 'J' ? "Japanese" : "another";
    return std::string("This is the ") + region + " release of Metroid Prime (" + id + ", revision " +
           std::to_string(version) + ").";
  }
  return std::string("This disc image is not Metroid Prime (game id ") + id + ").";
}

std::string CheckImage(const std::string& path, std::string* revision) {
  uint8_t header[8] = {};
  SDL_IOStream* io = SDL_IOFromFile(path.c_str(), "rb");
  if (io == nullptr) {
    return "The file can't be opened.";
  }
  const size_t got = SDL_ReadIO(io, header, sizeof(header));
  SDL_CloseIO(io);
  if (got != sizeof(header)) {
    return "The file is too short to be a disc image.";
  }
  if (IsContainer(header)) {
    if (const std::string bad = PortDiscVerify::ReadDiscHeader(path, header); !bad.empty()) {
      return bad.back() == '.' ? bad : bad + ".";
    }
  }
  const char* id6 = reinterpret_cast<const char*>(header);
  if (!IsSupportedId(id6, header[6], header[7])) {
    return DescribeDisc(id6, header[7]);
  }
  if (revision != nullptr) {
    *revision = PortDisc::Name(PortDisc::Identify(id6, header[6], header[7]));
  }
  return {};
}

std::string CopyToUserFolder(const std::string& source, std::string* target, std::atomic<uint64_t>* doneOut,
                             std::atomic<uint64_t>* totalOut, const std::atomic<bool>* cancel) {
  const std::string& folder = PortPaths::UserFolder();
  if (folder.empty()) {
    PortLog::Write("metroid_prime_port: no data folder to copy the disc into\n");
    return "There is no data folder to copy the disc image into.";
  }
  const std::filesystem::path destination = PortPaths::detail::FromUtf8(folder) / "disc.iso";
  // Copied under another name and renamed when complete: a copy killed part
  // way (the app closed during a multi-minute copy) must not leave a
  // truncated disc.iso, which ResolveDiscPath would prefer on every launch.
  const std::filesystem::path partial = PortPaths::detail::FromUtf8(folder) / "disc.iso.part";

  SDL_IOStream* in = SDL_IOFromFile(source.c_str(), "rb");
  if (in == nullptr) {
    PortLog::Write("metroid_prime_port: could not read the picked image: %s: %s\n", source.c_str(), SDL_GetError());
    return "The picked image can't be read.";
  }
  // A plain image names its game in the first bytes: refuse a wrong one
  // before minutes of copying. Containers (RVZ, WBFS, CISO) are checked once
  // mounted instead (or by CheckImage before this).
  Uint8 header[8] = {};
  if (SDL_ReadIO(in, header, sizeof(header)) == sizeof(header)) {
    if (!IsContainer(header) && !IsSupportedId(reinterpret_cast<const char*>(header), header[6], header[7])) {
      const std::string why = DescribeDisc(reinterpret_cast<const char*>(header), header[7]);
      PortLog::Write("metroid_prime_port: not copying the picked image: %s\n", why.c_str());
      SDL_CloseIO(in);
      return why;
    }
  }
  if (SDL_SeekIO(in, 0, SDL_IO_SEEK_SET) != 0) {
    // Some providers hand out a pipe, which cannot seek: start over.
    SDL_CloseIO(in);
    in = SDL_IOFromFile(source.c_str(), "rb");
    if (in == nullptr) {
      PortLog::Write("metroid_prime_port: could not reopen the picked image: %s\n", SDL_GetError());
      return "The picked image can't be read.";
    }
  }
  const Sint64 total = SDL_GetIOSize(in);
  if (totalOut != nullptr) {
    totalOut->store(total > 0 ? static_cast<uint64_t>(total) : 0);
  }
  if (doneOut != nullptr) {
    doneOut->store(0);
  }
  SDL_IOStream* out = SDL_IOFromFile(PortPaths::detail::ToUtf8(partial).c_str(), "wb");
  if (out == nullptr) {
    PortLog::Write("metroid_prime_port: could not create %s: %s\n", PortPaths::detail::ToUtf8(partial).c_str(), SDL_GetError());
    SDL_CloseIO(in);
    return "The copy can't be created in the data folder.";
  }

  std::string failure;
  char buffer[1 << 16];
  Sint64 done = 0;
  int lastPercent = -1;
  for (;;) {
    if (cancel != nullptr && cancel->load()) {
      failure = "cancelled";
      break;
    }
    const size_t got = SDL_ReadIO(in, buffer, sizeof(buffer));
    if (got == 0) {
      // Zero is also what a failed read returns; only the stream status
      // tells end of file from an error that would leave a truncated copy.
      if (SDL_GetIOStatus(in) != SDL_IO_STATUS_EOF) {
        PortLog::Write("metroid_prime_port: reading the picked image failed: %s\n", SDL_GetError());
        failure = "Reading the picked image failed.";
      }
      break;
    }
    if (SDL_WriteIO(out, buffer, got) != got) {
      PortLog::Write("metroid_prime_port: writing %s failed: %s\n", PortPaths::detail::ToUtf8(partial).c_str(), SDL_GetError());
      failure = "Writing the copy failed; the device may be full.";
      break;
    }
    done += static_cast<Sint64>(got);
    if (doneOut != nullptr) {
      doneOut->store(static_cast<uint64_t>(done));
    }
#if defined(__ANDROID__)
    // The copy takes minutes on a device, long enough for the screen to go
    // off; nothing else drops a destroyed surface. (Only the boot copy has
    // no frames; harmless while the game runs.)
    if (cancel == nullptr) {
      aurora_release_lost_surface();
    }
#endif
    if (total > 0) {
      const int percent = static_cast<int>(done * 100 / total);
      // Every 5% rather than every chunk: a 1.5 GB image would otherwise
      // put 3000 lines in the log.
      if (percent / 5 != lastPercent / 5) {
        lastPercent = percent;
        PortLog::Write("metroid_prime_port: copying the disc image, %d%%\n", percent);
      }
    }
  }
  if (!SDL_FlushIO(out) && failure.empty()) {
    PortLog::Write("metroid_prime_port: flushing %s failed: %s\n", PortPaths::detail::ToUtf8(partial).c_str(), SDL_GetError());
    failure = "Writing the copy failed.";
  }
  if (!SDL_CloseIO(in)) {
    PortLog::Write("metroid_prime_port: closing the picked image failed: %s\n", SDL_GetError());
  }
  if (!SDL_CloseIO(out) && failure.empty()) {
    PortLog::Write("metroid_prime_port: closing %s failed: %s\n", PortPaths::detail::ToUtf8(partial).c_str(), SDL_GetError());
    failure = "Writing the copy failed.";
  }
  std::error_code ec;
  if (!failure.empty()) {
    std::filesystem::remove(partial, ec);
    return failure;
  }
  // What landed on disk, not what was handed to the writer: a short write that
  // only fails at close looks identical to a good copy otherwise, and a
  // truncated image fails to parse as a disc with no further clue.
  const auto written = std::filesystem::file_size(partial, ec);
  if (ec || static_cast<Sint64>(written) != done || (total > 0 && done != total)) {
    PortLog::Write("metroid_prime_port: %s is %lld bytes on disk, copied %lld of %lld\n", PortPaths::detail::ToUtf8(partial).c_str(),
                   static_cast<long long>(written), static_cast<long long>(done), static_cast<long long>(total));
    std::filesystem::remove(partial, ec);
    return "The copy is incomplete.";
  }
  std::filesystem::rename(partial, destination, ec);
  if (ec) {
    PortLog::Write("metroid_prime_port: could not rename %s to %s: %s\n", PortPaths::detail::ToUtf8(partial).c_str(),
                   PortPaths::detail::ToUtf8(destination).c_str(), ec.message().c_str());
    std::filesystem::remove(partial, ec);
    return "The copy can't be put in place.";
  }
  PortLog::Write("metroid_prime_port: copied the disc image to %s (%lld bytes)\n",
                 PortPaths::detail::ToUtf8(destination).c_str(), static_cast<long long>(done));
  if (target != nullptr) {
    *target = PortPaths::detail::ToUtf8(destination);
  }
  return {};
}

void Adopt(const std::string& path, const std::string& revision) {
  PortDebug::SetDiscPath(path.c_str());
  // Persist at once: the settings are otherwise written from the overlay's draw path.
  PortDebug::SaveSettingsNow();
  PortLog::Write("metroid_prime_port: disc image replaced: %s (%s)\n", path.c_str(), revision.c_str());
}

bool Start(const std::string& picked) {
  std::lock_guard threadLock(sThreadMutex);
  {
    std::lock_guard lock(sMutex);
    if (sStatus.state == State::Running) {
      return false;
    }
  }
  if (sThread.joinable()) {
    sThread.join();
  }
  sCancel = false;
  sDone = 0;
  sTotal = 0;
  {
    std::lock_guard lock(sMutex);
    sStatus = Status{};
    sStatus.state = State::Running;
    sApplied = true;
  }
  sThread = std::thread([picked] { Run(picked); });
  return true;
}

void Cancel() { sCancel = true; }

void Shutdown() {
  Cancel();
  std::lock_guard threadLock(sThreadMutex);
  if (sThread.joinable()) {
    sThread.join();
  }
  // A replacement that finished while the menu was closed is still remembered.
  Poll();
}

Status Snapshot() {
  std::lock_guard lock(sMutex);
  Status s = sStatus;
  s.bytesDone = sDone;
  s.bytesTotal = sTotal;
  return s;
}

bool Poll() {
  Status s;
  {
    std::lock_guard lock(sMutex);
    if (sApplied) {
      return false;
    }
    sApplied = true;
    s = sStatus;
  }
  Adopt(s.path, s.revision);
  return true;
}

void PickAsync() {
  {
    std::lock_guard lock(sMutex);
    if (sStatus.state == State::Running) {
      return;
    }
  }
  int windowCount = 0;
  SDL_Window** windows = SDL_GetWindows(&windowCount);
  SDL_Window* window = windows != nullptr && windowCount > 0 ? windows[0] : nullptr;
  SDL_free(windows);
  // Static: SDL reads the filters until the dialog closes.
  static const SDL_DialogFileFilter filters[] = {
      {"Metroid Prime disc image (iso, gcm, rvz, wia, gcz, wbfs, ciso, nkit)", "iso;gcm;rvz;wia;gcz;wbfs;ciso;nkit"},
      {"All files", "*"},
  };
  const SDL_PropertiesID props = SDL_CreateProperties();
  SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_FILTERS_POINTER, const_cast<SDL_DialogFileFilter*>(filters));
  SDL_SetNumberProperty(props, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER, 2);
  SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_WINDOW_POINTER, window);
  SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_TITLE_STRING, "Select your Metroid Prime disc image");
  SDL_ShowFileDialogWithProperties(
      SDL_FILEDIALOG_OPENFILE,
      [](void*, const char* const* files, int) {
        if (files == nullptr) {
          Finish(State::Failed, std::string("The file dialog failed: ") + SDL_GetError());
        } else if (files[0] != nullptr) {
          Start(files[0]);
        } // an empty list is a cancel
      },
      nullptr, props);
  SDL_DestroyProperties(props);
}

} // namespace PortDiscReplace
