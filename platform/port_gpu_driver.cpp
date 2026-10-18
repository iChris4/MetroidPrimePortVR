#include "port_gpu_driver.h"
#include "port_strings.h"
#include "port_bytes.h"

// PrimedGun: keyed on the build, not the platform. CMakeLists.txt defines
// MP_CUSTOM_GPU_DRIVERS for upstream's arm64 phone build only, where adrenotools is
// linked; the Quest build (MP_ENABLE_OPENXR) gets the stubs below.
#if defined(MP_CUSTOM_GPU_DRIVERS)
#include "port_json.h"
#include "port_log.h"

#include <SDL3/SDL.h>
#include <adrenotools/driver.h>
#include <dlfcn.h>
#include <elf.h>
#include <zlib.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#endif

namespace PortGpuDriver {
namespace {
std::string sActive;
std::string sLoadError;
} // namespace

const std::string& Active() { return sActive; }
const std::string& LoadError() { return sLoadError; }
void SetLoadError(std::string why) {
  sActive.clear();
  sLoadError = std::move(why);
}

#if defined(MP_CUSTOM_GPU_DRIVERS)
namespace {
namespace fs = std::filesystem;

std::string InternalDir() {
  const char* root = SDL_GetAndroidInternalStoragePath();
  return root != nullptr ? root : ".";
}

fs::path DriversDir() { return fs::path(InternalDir()) / "gpu_drivers"; }

// The APK's extracted native libraries (useLegacyPackaging): adrenotools' hooks and
// the Vulkan shim are there, next to this library.
std::string NativeLibDir() {
  Dl_info info{};
  if (dladdr(reinterpret_cast< void* >(&NativeLibDir), &info) == 0 || info.dli_fname == nullptr) {
    return {};
  }
  return fs::path(info.dli_fname).parent_path().string();
}

bool ReadMeta(const std::string& text, Driver& out, std::string& error) {
  PortJson::Value meta;
  size_t offset = 0;
  const char* reason = nullptr;
  if (!PortJson::Parse(text, meta, offset, &reason) || !meta.IsObject()) {
    error = "meta.json is not valid JSON";
    return false;
  }
  out.name = meta.StringOr("name");
  out.description = meta.StringOr("description");
  out.version = meta.StringOr("driverVersion", meta.StringOr("packageVersion").c_str());
  out.library = meta.StringOr("libraryName");
  if (out.library.empty() || out.library.find('/') != std::string::npos) {
    error = "meta.json names no driver library";
    return false;
  }
  if (out.name.empty()) {
    out.name = out.library;
  }
  return true;
}

std::string ReadFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return port::ReadAllText(in);
}

// Qualcomm driver packages name their library like the phone's own Vulkan driver
// (vulkan.adreno.so, which the UI's renderer has already loaded), and the linker then
// hands back the loaded one by its SONAME. Such a library is installed under, and
// renamed inside to, "mportv.<rest>" (same length, so the ELF patches in place).
constexpr char kSystemPrefix[] = "vulkan.";
constexpr char kLocalPrefix[] = "mportv.";
static_assert(sizeof(kSystemPrefix) == sizeof(kLocalPrefix));

std::string LocalLibrary(const std::string& library) {
  return library.rfind(kSystemPrefix, 0) == 0 ? kLocalPrefix + library.substr(sizeof(kSystemPrefix) - 1) : library;
}

// Rewrites a 64-bit ELF's DT_SONAME "vulkan.*" to "mportv.*". False only for a
// malformed file; one without such a SONAME is left alone.
bool RenameSoname(std::vector< uint8_t >& elf) {
  const auto fits = [&](uint64_t offset, uint64_t size) { return offset <= elf.size() && size <= elf.size() - offset; };
  if (!fits(0, sizeof(Elf64_Ehdr)) || std::memcmp(elf.data(), ELFMAG, SELFMAG) != 0 || elf[EI_CLASS] != ELFCLASS64) {
    return false;
  }
  Elf64_Ehdr eh;
  std::memcpy(&eh, elf.data(), sizeof(eh));
  if (eh.e_phentsize != sizeof(Elf64_Phdr) || !fits(eh.e_phoff, uint64_t(eh.e_phnum) * sizeof(Elf64_Phdr))) {
    return false;
  }
  std::vector< Elf64_Phdr > phdrs(eh.e_phnum);
  std::memcpy(phdrs.data(), elf.data() + eh.e_phoff, phdrs.size() * sizeof(Elf64_Phdr));
  uint64_t strtab = 0, soname = 0;
  bool hasSoname = false;
  for (const Elf64_Phdr& ph : phdrs) {
    if (ph.p_type != PT_DYNAMIC || !fits(ph.p_offset, ph.p_filesz)) {
      continue;
    }
    for (uint64_t at = ph.p_offset; at + sizeof(Elf64_Dyn) <= ph.p_offset + ph.p_filesz; at += sizeof(Elf64_Dyn)) {
      Elf64_Dyn dyn;
      std::memcpy(&dyn, elf.data() + at, sizeof(dyn));
      if (dyn.d_tag == DT_NULL) {
        break;
      }
      if (dyn.d_tag == DT_STRTAB) {
        strtab = dyn.d_un.d_ptr;
      } else if (dyn.d_tag == DT_SONAME) {
        soname = dyn.d_un.d_val;
        hasSoname = true;
      }
    }
  }
  if (!hasSoname) {
    return true;
  }
  if (soname >= elf.size()) {
    return false;
  }
  // The string table's address to its place in the file.
  for (const Elf64_Phdr& ph : phdrs) {
    if (ph.p_type == PT_LOAD && fits(ph.p_offset, ph.p_filesz) && strtab >= ph.p_vaddr &&
        strtab - ph.p_vaddr < ph.p_filesz) {
      const uint64_t at = ph.p_offset + (strtab - ph.p_vaddr) + soname;
      const size_t prefix = sizeof(kSystemPrefix) - 1;
      if (!fits(at, prefix)) {
        return false;
      }
      if (std::memcmp(elf.data() + at, kSystemPrefix, prefix) == 0) {
        std::memcpy(elf.data() + at, kLocalPrefix, prefix);
      }
      return true;
    }
  }
  return false;
}

bool ReadDriver(const fs::path& dir, Driver& out) {
  std::string error;
  out.id = dir.filename().string();
  std::error_code ec;
  return ReadMeta(ReadFile(dir / "meta.json"), out, error) &&
         fs::is_regular_file(dir / LocalLibrary(out.library), ec);
}

using port::ReadLE16;
using port::ReadLE32;

struct ZipEntry {
  std::string name;
  uint16_t flags = 0;
  uint16_t method = 0;
  uint32_t crc = 0;
  uint32_t compressedSize = 0;
  uint32_t size = 0;
  uint32_t localOffset = 0;
};

// The central directory of a zip (no zip64: driver packages are a few MB).
bool ListZip(const std::vector< uint8_t >& zip, std::vector< ZipEntry >& entries) {
  if (zip.size() < 22) {
    return false;
  }
  size_t eocd = SIZE_MAX;
  const size_t stop = zip.size() > 22 + 0xFFFF ? zip.size() - 22 - 0xFFFF : 0;
  for (size_t i = zip.size() - 22 + 1; i-- > stop;) {
    if (ReadLE32(&zip[i]) == 0x06054b50) {
      eocd = i;
      break;
    }
  }
  if (eocd == SIZE_MAX) {
    return false;
  }
  const uint16_t count = ReadLE16(&zip[eocd + 10]);
  size_t at = ReadLE32(&zip[eocd + 16]);
  for (uint16_t i = 0; i < count; ++i) {
    if (at + 46 > zip.size() || ReadLE32(&zip[at]) != 0x02014b50) {
      return false;
    }
    ZipEntry e;
    e.flags = ReadLE16(&zip[at + 8]);
    e.method = ReadLE16(&zip[at + 10]);
    e.crc = ReadLE32(&zip[at + 16]);
    e.compressedSize = ReadLE32(&zip[at + 20]);
    e.size = ReadLE32(&zip[at + 24]);
    const uint16_t nameLen = ReadLE16(&zip[at + 28]);
    const size_t skip = 46u + nameLen + ReadLE16(&zip[at + 30]) + ReadLE16(&zip[at + 32]);
    e.localOffset = ReadLE32(&zip[at + 42]);
    if (at + skip > zip.size()) {
      return false;
    }
    e.name.assign(reinterpret_cast< const char* >(&zip[at + 46]), nameLen);
    entries.push_back(std::move(e));
    at += skip;
  }
  return true;
}

// Real driver libraries are a few tens of MB; anything far bigger is not one.
constexpr size_t kMaxZipBytes = 256u << 20;

bool InflateZip(const std::vector< uint8_t >& zip, const ZipEntry& e, std::vector< uint8_t >& out);

// One entry's bytes, CRC-checked: a corrupt driver would only show as a crash at the next start.
bool ExtractZip(const std::vector< uint8_t >& zip, const ZipEntry& e, std::vector< uint8_t >& out) {
  return InflateZip(zip, e, out) && crc32(0, out.data(), uInt(out.size())) == e.crc;
}

bool InflateZip(const std::vector< uint8_t >& zip, const ZipEntry& e, std::vector< uint8_t >& out) {
  const size_t local = e.localOffset;
  if ((e.flags & 1) != 0 || e.size > kMaxZipBytes || local + 30 > zip.size() ||
      ReadLE32(&zip[local]) != 0x04034b50) {
    return false;
  }
  const size_t data = local + 30 + ReadLE16(&zip[local + 26]) + ReadLE16(&zip[local + 28]);
  if (data + e.compressedSize > zip.size()) {
    return false;
  }
  out.resize(e.size);
  if (e.method == 0) {
    if (e.compressedSize != e.size) {
      return false;
    }
    std::memcpy(out.data(), &zip[data], e.size);
    return true;
  }
  if (e.method != 8) {
    return false;
  }
  z_stream stream{};
  if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
    return false;
  }
  stream.next_in = const_cast< Bytef* >(&zip[data]);
  stream.avail_in = e.compressedSize;
  stream.next_out = out.data();
  stream.avail_out = e.size;
  const int result = inflate(&stream, Z_FINISH);
  inflateEnd(&stream);
  return result == Z_STREAM_END && stream.total_out == e.size;
}

bool WriteFile(const fs::path& path, const void* data, size_t size) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(static_cast< const char* >(data), std::streamsize(size));
  out.close();
  return !out.fail();
}

// Folder name for a driver: its name and version, letters, digits, '.', '-' and '_' only.
std::string MakeId(const Driver& driver) {
  std::string id = driver.name + (driver.version.empty() ? "" : "-" + driver.version);
  for (char& c : id) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '.' || c == '-' || c == '_';
    c = ok ? c : '_';
  }
  if (id.empty() || id[0] == '.') {
    id.insert(0, "driver");
  }
  return id.substr(0, 96);
}

// Dawn only takes a directory to find "libvulkan.so" in, so the shim goes into one
// under that name. Copied fresh every start: the APK it comes from may have changed.
std::string InstallShim(const std::string& nativeLibDir) {
  const fs::path dir = fs::path(InternalDir()) / "vkshim";
  std::error_code ec;
  fs::create_directories(dir, ec);
  const fs::path temp = dir / "libvulkan.so.tmp";
  const fs::path shim = dir / "libvulkan.so";
  fs::copy_file(fs::path(nativeLibDir) / "libmport_vkshim.so", temp, fs::copy_options::overwrite_existing, ec);
  if (ec) {
    return {};
  }
  fs::rename(temp, shim, ec);
  return ec ? std::string() : shim.string();
}
} // namespace

bool Supported() { return true; }

std::vector< Driver > List() {
  std::vector< Driver > drivers;
  std::error_code ec;
  for (const fs::directory_entry& entry : fs::directory_iterator(DriversDir(), ec)) {
    Driver driver;
    // ".tmp" folders are installs that never finished.
    if (entry.is_directory(ec) && entry.path().extension() != ".tmp" && ReadDriver(entry.path(), driver)) {
      drivers.push_back(std::move(driver));
    }
  }
  std::sort(drivers.begin(), drivers.end(), [](const Driver& a, const Driver& b) { return a.id < b.id; });
  return drivers;
}

static std::string InstallZip(const std::string& zipPath, std::string& error) {
  std::vector< uint8_t > zip;
  {
    size_t size = 0;
    void* data = SDL_LoadFile(zipPath.c_str(), &size);
    if (data == nullptr) {
      error = std::string("couldn't read the file: ") + SDL_GetError();
      return {};
    }
    if (size > kMaxZipBytes) {
      SDL_free(data);
      error = "the file is too big for a driver package";
      return {};
    }
    zip.assign(static_cast< uint8_t* >(data), static_cast< uint8_t* >(data) + size);
    SDL_free(data);
  }
  std::vector< ZipEntry > entries;
  if (!ListZip(zip, entries)) {
    error = "not a zip file";
    return {};
  }
  // Files by their base name: packages put them at the root, or in one folder.
  const auto baseName = [](const std::string& name) { return name.substr(name.find_last_of('/') + 1); };
  const auto find = [&](const std::string& base) -> const ZipEntry* {
    for (const ZipEntry& e : entries) {
      if (!e.name.empty() && e.name.back() != '/' && baseName(e.name) == base) {
        return &e;
      }
    }
    return nullptr;
  };
  const ZipEntry* metaEntry = find("meta.json");
  std::vector< uint8_t > bytes;
  if (metaEntry == nullptr || !ExtractZip(zip, *metaEntry, bytes)) {
    error = "no meta.json in the zip (is it an Adreno driver package?)";
    return {};
  }
  Driver driver;
  if (!ReadMeta(std::string(bytes.begin(), bytes.end()), driver, error)) {
    return {};
  }
  // Every file next to meta.json: some drivers ship extra libraries.
  const std::string top = metaEntry->name.substr(0, metaEntry->name.size() - std::strlen("meta.json"));
  const ZipEntry* library = find(driver.library);
  if (library == nullptr || library->name != top + driver.library) {
    error = "the zip has no " + driver.library + " next to its meta.json";
    return {};
  }
  driver.id = MakeId(driver);
  const fs::path dir = DriversDir() / driver.id;
  if (driver.id == sActive) {
    error = "this driver is in use; switch to another and restart first";
    return {};
  }
  // Unpacked beside it first, so a failed reinstall keeps the old copy.
  const fs::path temp = DriversDir() / (driver.id + ".tmp");
  std::error_code ec;
  fs::remove_all(temp, ec);
  fs::create_directories(temp, ec);
  for (const ZipEntry& e : entries) {
    const std::string base = baseName(e.name);
    if (base.empty() || e.name != top + base || base == "." || base == "..") {
      continue;
    }
    const bool isLibrary = base == driver.library;
    if (!ExtractZip(zip, e, bytes) || (isLibrary && !RenameSoname(bytes)) ||
        !WriteFile(temp / (isLibrary ? LocalLibrary(base) : base), bytes.data(), bytes.size())) {
      error = "couldn't unpack " + e.name;
      fs::remove_all(temp, ec);
      return {};
    }
  }
  fs::remove_all(dir, ec);
  fs::rename(temp, dir, ec);
  if (ec) {
    error = "couldn't move it into place: " + ec.message();
    fs::remove_all(temp, ec);
    return {};
  }
  PortLog::Write("port: installed GPU driver %s (%s)\n", driver.id.c_str(), driver.library.c_str());
  return driver.id;
}

std::string Install(const std::string& zipPath, std::string& error) {
  try {
    return InstallZip(zipPath, error);
  } catch (const std::exception& e) {
    error = e.what();
    return {};
  }
}

bool Remove(const std::string& id) {
  if (id.empty() || id == sActive || id.find('/') != std::string::npos || id == "." || id == "..") {
    return false;
  }
  std::error_code ec;
  return fs::remove_all(DriversDir() / id, ec) > 0 && !ec;
}

std::string Prepare(const std::string& id) {
  sActive.clear();
  sLoadError.clear();
  if (id.empty()) {
    return {};
  }
  const auto fail = [&](std::string why) {
    sLoadError = std::move(why);
    PortLog::Write("port: GPU driver %s not loaded (%s); using the system driver\n", id.c_str(), sLoadError.c_str());
    return std::string();
  };
  Driver driver;
  const fs::path dir = DriversDir() / id;
  if (id.find('/') != std::string::npos || !ReadDriver(dir, driver)) {
    return fail("not installed");
  }
  const std::string libDir = NativeLibDir();
  if (libDir.empty()) {
    return fail("no native library folder");
  }
  // Only Android 9 (no memfd) needs a folder for the patched libraries.
  const fs::path tempDir = fs::path(InternalDir()) / "gpu_driver_tmp";
  std::error_code ec;
  fs::create_directories(tempDir, ec);
  void* vulkan = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, (tempDir.string() + "/").c_str(),
                                            (libDir + "/").c_str(), (dir.string() + "/").c_str(),
                                            LocalLibrary(driver.library).c_str(), nullptr, nullptr);
  if (vulkan == nullptr) {
    const char* why = dlerror();
    return fail(std::string("adrenotools couldn't open it: ") + (why != nullptr ? why : "unknown error"));
  }
  void* gipa = dlsym(vulkan, "vkGetInstanceProcAddr");
  if (gipa == nullptr) {
    return fail("its loader has no vkGetInstanceProcAddr");
  }
  const std::string shimPath = InstallShim(libDir);
  void* shim = shimPath.empty() ? nullptr : dlopen(shimPath.c_str(), RTLD_NOW | RTLD_LOCAL);
  using SetFn = void (*)(void*);
  const auto set = shim != nullptr ? reinterpret_cast< SetFn >(dlsym(shim, "mport_vkshim_set")) : nullptr;
  if (set == nullptr) {
    return fail("couldn't set up the Vulkan shim");
  }
  set(gipa);
  // Both stay loaded for the whole run; Dawn's dlopen of the shim gets this same handle.
  sActive = id;
  PortLog::Write("port: GPU driver %s (%s %s) loaded through adrenotools\n", id.c_str(), driver.name.c_str(),
                 driver.version.c_str());
  return fs::path(shimPath).parent_path().string() + "/";
}

#else

bool Supported() { return false; }
std::vector< Driver > List() { return {}; }
std::string Install(const std::string&, std::string& error) {
  error = "custom GPU drivers are only supported on Android";
  return {};
}
bool Remove(const std::string&) { return false; }
std::string Prepare(const std::string&) { return {}; }

#endif

} // namespace PortGpuDriver
