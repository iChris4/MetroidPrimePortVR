// The Flaahgra theme import: the image, the background run and the files it writes.

#include "port_flaahgra_music.h"

#include "port_gci.h"
#include "port_mods.h"

#include <aurora/dvd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace PortFlaahgraMusic {
namespace {

std::mutex sMutex;
State sState;

constexpr const char* kModName = "flaahgra-full-theme";
constexpr const char* kFiles[2] = {"rui_flaaghraL.dsp", "rui_flaaghraR.dsp"};

fs::path ModFolder() { return PortGci::PathFromString(PortMods::Folder()) / kModName; }

void SetMessage(const std::string& message) {
  std::lock_guard lock(sMutex);
  sState.message = message;
}

bool SameName(const char* a, const char* b) {
  const char* slash = std::strrchr(a, '/');
  a = slash != nullptr ? slash + 1 : a;
  for (; *a != '\0' && *b != '\0'; ++a, ++b) {
    if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b))) {
      return false;
    }
  }
  return *a == *b;
}

struct Found {
  u32 index = 0;
  u32 size = 0;
  bool present = false;
};

// The whole file `index` of `image`.
bool ReadWhole(AuroraDiscImage* image, const Found& found, std::vector<uint8_t>& out) {
  void* file = aurora_disc_image_file_open(image, found.index);
  if (file == nullptr) {
    return false;
  }
  out.resize(found.size);
  size_t done = 0;
  while (done < out.size()) {
    const int64_t got = aurora_dvd_base_read(file, out.data() + done, out.size() - done);
    if (got <= 0) {
      break;
    }
    done += size_t(got);
  }
  aurora_dvd_base_close(file);
  return done == out.size();
}

}  // namespace

bool Import(const std::string& path, std::string& error) {
  AuroraDiscImage* image = aurora_disc_image_open(path.c_str(), nullptr, nullptr, nullptr);
  if (image == nullptr) {
    error = "Could not open the image (not a GameCube or Wii disc image, or unreadable).";
    return false;
  }
  struct Closer {
    AuroraDiscImage* image;
    ~Closer() { aurora_disc_image_close(image); }
  } closer{image};

  Found found[2];
  struct Context {
    Found* found;
  } context{found};
  aurora_disc_image_list(
      image,
      [](u32 index, const char* name, u32 size, void* user) {
        Found* found = static_cast<Context*>(user)->found;
        for (int i = 0; i < 2; ++i) {
          if (!found[i].present && SameName(name, kFiles[i])) {
            found[i] = {index, size, true};
          }
        }
      },
      &context);

  std::vector<uint8_t> data[2];
  uint32_t samples[2] = {};
  for (int i = 0; i < 2; ++i) {
    if (!found[i].present) {
      error = std::string("The image has no ") + kFiles[i] + ": it is not a Metroid Prime disc.";
      return false;
    }
    SetMessage(std::string("Reading ") + kFiles[i]);
    if (!ReadWhole(image, found[i], data[i])) {
      error = std::string("Could not read ") + kFiles[i] + " from the image.";
      return false;
    }
    if (const char* why = CheckChannel(data[i].data(), data[i].size(), data[i].size(), samples[i])) {
      error = std::string(kFiles[i]) + " " + why +
              ". Pick the PAL or Japanese GameCube disc or Metroid Prime Trilogy.";
      return false;
    }
  }
  if (const char* why = CheckPair(samples[0], samples[1])) {
    error = why;
    return false;
  }

  // Each file is written under a temporary name and renamed over the final one.
  SetMessage("Writing the mod");
  const fs::path folder = ModFolder() / "Audio";
  std::error_code ec;
  fs::create_directories(folder, ec);
  if (ec) {
    error = "Could not create " + PortGci::PathString(folder) + ".";
    return false;
  }
  for (int i = 0; i < 2; ++i) {
    const fs::path temp = folder / (std::string(kFiles[i]) + ".importing");
    {
      std::ofstream out(temp, std::ios::binary);
      out.write(reinterpret_cast<const char*>(data[i].data()), std::streamsize(data[i].size()));
      out.flush();
      if (!out) {
        error = "Could not write " + PortGci::PathString(temp) + ".";
        fs::remove(temp, ec);
        return false;
      }
    }
    fs::rename(temp, folder / kFiles[i], ec);
    if (ec) {
      error = "Could not move the file into " + PortGci::PathString(folder) + ".";
      fs::remove(temp, ec);
      return false;
    }
  }
  return true;
}

bool Start(const std::string& path) {
  std::lock_guard lock(sMutex);
  if (sState.running) {
    return false;
  }
  sState = {};
  sState.running = true;
  sState.message = "Opening the image";
  // Detached: a static joinable thread would call std::terminate at exit.
  std::thread([path] {
    std::string error;
    const bool ok = Import(path, error);
    std::lock_guard lock(sMutex);
    sState.running = false;
    sState.finished = true;
    sState.ok = ok;
    sState.message = ok ? "Added the full Flaahgra theme. Use Reload mods (F1 > Mods) or restart the game to hear it."
                        : error;
  }).detach();
  return true;
}

State Status() {
  std::lock_guard lock(sMutex);
  return sState;
}

bool Installed() {
  std::error_code ec;
  for (const char* name : kFiles) {
    if (!fs::is_regular_file(ModFolder() / "Audio" / name, ec)) {
      return false;
    }
  }
  return true;
}

bool Remove() {
  {
    std::lock_guard lock(sMutex);
    if (sState.running) {
      return false;
    }
    sState = {};
  }
  std::error_code ec;
  fs::remove_all(ModFolder(), ec);
  return !ec;
}

int RunFromCommandLine(const std::string& path) {
  std::string error;
  if (!Import(path, error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  std::printf("Added the full Flaahgra theme in %s\n", PortGci::PathString(ModFolder()).c_str());
  return 0;
}

}  // namespace PortFlaahgraMusic
