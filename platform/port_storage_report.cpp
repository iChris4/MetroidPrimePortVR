#include "port_storage_report.h"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

#if defined(__ANDROID__)
#include <SDL3/SDL.h>
#endif

#include "port_log.h"
#include "port_mods.h"
#include "port_remastered_import.h"

namespace fs = std::filesystem;

namespace PortStorageReport {
namespace {

// The folders come as UTF-8; a narrow path is the ANSI code page on Windows.
fs::path FromUtf8(const std::string& text) { return fs::path(std::u8string(text.begin(), text.end())); }

std::string ToUtf8(const fs::path& path) {
  const std::u8string text = path.u8string();
  return std::string(text.begin(), text.end());
}

struct Totals {
  uint64_t bytes = 0;
  uint64_t files = 0;
};

// Hard-linked files (a stage reuses the previous import's) count once per inode.
struct Walker {
  std::set<std::pair<uint64_t, uint64_t>> seen;
  uint64_t bytes = 0;

  void Add(const fs::path& file, const fs::directory_entry& entry, Totals& totals) {
    std::error_code ec;
#if !defined(_WIN32)
    struct stat st {};
    if (::lstat(file.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
      return;
    }
    if (!seen.insert({static_cast<uint64_t>(st.st_dev), static_cast<uint64_t>(st.st_ino)}).second) {
      return;
    }
    const uint64_t size = static_cast<uint64_t>(st.st_blocks) * 512u;
#else
    const uint64_t size = entry.file_size(ec);
    if (ec) {
      return;
    }
#endif
    totals.bytes += size;
    totals.files += 1;
    bytes += size;
  }

  Totals Measure(const fs::path& path) {
    Totals totals;
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(path, ec);
    if (ec || fs::is_symlink(status)) {
      return totals;
    }
    if (!fs::is_directory(status)) {
      Add(path, fs::directory_entry(path, ec), totals);
      return totals;
    }
    fs::recursive_directory_iterator it(path, fs::directory_options::skip_permission_denied, ec);
    for (const fs::recursive_directory_iterator end; !ec && it != end; it.increment(ec)) {
      std::error_code entryError;
      if (it->is_symlink(entryError) || !it->is_regular_file(entryError)) {
        continue;
      }
      Add(it->path(), *it, totals);
    }
    return totals;
  }

  // One line per direct child of `root`, except `skip`.
  void ReportChildren(const fs::path& root, const fs::path& skip) {
    std::error_code ec;
    std::vector<fs::path> children;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
      if (!skip.empty() && it->path() == skip) {
        continue;
      }
      children.push_back(it->path());
    }
    if (ec) {
      PortLog::Write("port: storage: cannot list %s: %s\n", ToUtf8(root).c_str(), ec.message().c_str());
    }
    for (const fs::path& child : children) {
      const Totals totals = Measure(child);
      PortLog::Write("port: storage: %s %llu MiB (%llu files)\n", ToUtf8(child).c_str(),
                     static_cast<unsigned long long>(totals.bytes >> 20),
                     static_cast<unsigned long long>(totals.files));
    }
  }
};

bool Inside(const fs::path& path, const fs::path& parent) {
  auto p = path.begin();
  for (auto q = parent.begin(); q != parent.end(); ++q, ++p) {
    if (p == path.end() || *p != *q) {
      return false;
    }
  }
  return true;
}

void Run() {
  std::error_code ec;
  fs::path internal;
#if defined(__ANDROID__)
  if (const char* storage = SDL_GetAndroidInternalStoragePath()) {
    internal = fs::weakly_canonical(fs::path(storage), ec);
  }
#endif
  const std::string importText = PortRemastered::ImportRootFolder();
  fs::path importRoot = importText.empty() ? fs::path() : fs::weakly_canonical(FromUtf8(importText), ec);
  // The root comes with a trailing separator; without it the path compares equal to a listed child.
  if (!importRoot.empty() && importRoot.filename().empty()) {
    importRoot = importRoot.parent_path();
  }
  fs::path dataRoot;
  const std::string mods = PortMods::Folder();
  if (!mods.empty()) {
    dataRoot = fs::weakly_canonical(FromUtf8(mods), ec).parent_path();
  }

  Walker walker;
  if (!importRoot.empty()) {
    walker.ReportChildren(importRoot, fs::path());
  }
  if (!internal.empty()) {
    // The import root, if inside, was listed above.
    walker.ReportChildren(internal, importRoot);
  }
  if (!dataRoot.empty() && (internal.empty() || !Inside(dataRoot, internal)) &&
      dataRoot != importRoot) {
    walker.ReportChildren(dataRoot, importRoot);
  }
  PortLog::Write("port: storage: total %llu MiB\n",
                 static_cast<unsigned long long>(walker.bytes >> 20));
}

} // namespace

void StartLogging() {
  try {
    std::thread([] {
      try {
        Run();
      } catch (const std::exception& e) {
        PortLog::Write("port: storage: report failed: %s\n", e.what());
      } catch (...) {
        PortLog::Write("port: storage: report failed\n");
      }
    }).detach();
  } catch (...) {
  }
}

} // namespace PortStorageReport
