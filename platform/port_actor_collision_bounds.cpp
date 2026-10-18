#include "port_actor_collision_bounds.h"
#include "port_bytes.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

#include "port_mods.h"
#include "port_ws.h"

// A test drives Reader over synthetic PAKs, so it has no disc and no Aurora to
// link. This build leaves out only the disc-backed SourceIo below; everything
// the reader does with what it reads is still built and tested.
#ifndef PORT_ACTOR_COLLISION_BOUNDS_NO_DVD
#include <aurora/dvd.h>

#include "port_log.h"
#endif

namespace PortActorCollisionBounds {
namespace {

constexpr uint32_t kCMDL = 0x434D444C;
constexpr uint32_t kCmdlMagic = 0xDEADBABE;
// Bounds are six big-endian floats at 0x0c; the header runs to 0x2c.
constexpr size_t kHeaderSize = 0x2c;
constexpr size_t kBoundsOffset = 0x0c;
// A PAK table that needs more than this to parse is not a Prime 1 PAK.
constexpr size_t kMaxTableBytes = 64u << 20;
// A model this big is not a model; a corrupt length must not be allocated.
constexpr uint32_t kMaxResourceBytes = 64u << 20;
// A box further than this from the origin in any axis is malformed, not a
// model the game would have drawn and collided against. Generous enough for
// the disc's largest models.
constexpr float kMaxExtent = 1.0e6f;

using port::ReadBE32;
using port::ReadBEFloat;

bool SaneBox(const float bounds[6]) {
  for (uint32_t i = 0; i < 6; ++i) {
    if (!std::isfinite(bounds[i]) || std::fabs(bounds[i]) > kMaxExtent) {
      return false;
    }
  }
  // Flat in an axis is allowed: a plane's min equals its max there.
  return bounds[0] <= bounds[3] && bounds[1] <= bounds[4] && bounds[2] <= bounds[5];
}

#ifndef PORT_ACTOR_COLLISION_BOUNDS_NO_DVD
bool ReadAt(void* handle, uint64_t offset, uint8_t* out, size_t size) {
  if (handle == nullptr) {
    return false;
  }
  if (aurora_dvd_base_seek(handle, int64_t(offset), 0) != int64_t(offset)) {
    return false;
  }
  size_t done = 0;
  while (done < size) {
    const int64_t got = aurora_dvd_base_read(handle, out + done, size - done);
    if (got <= 0) {
      break;
    }
    done += size_t(got);
  }
  return done == size;
}
#endif // PORT_ACTOR_COLLISION_BOUNDS_NO_DVD

} // namespace

bool ParseCmdlBounds(const uint8_t* data, size_t size, float out[6]) {
  if (data == nullptr || size < kHeaderSize || ReadBE32(data) != kCmdlMagic) {
    return false;
  }
  // Version 1 puts its section sizes at 0x28, version 2 at 0x2c; anything else
  // is not a CMDL the game's model loader reads.
  const uint32_t version = ReadBE32(data + 4);
  if (version != 1 && version != 2) {
    return false;
  }
  for (uint32_t i = 0; i < 6; ++i) {
    out[i] = ReadBEFloat(data + kBoundsOffset + i * 4);
  }
  return SaneBox(out);
}

Reader::Reader(SourceIo io) : m_io(io) {}

Reader::~Reader() {
  for (const auto& [entry, handle] : m_handles) {
    if (handle != nullptr && m_io.close != nullptr) {
      m_io.close(handle);
    }
  }
}

bool Reader::Build() {
  std::lock_guard< std::mutex > lock(m_mutex);
  if (m_indexed) {
    return m_found;
  }
  m_indexed = true;
  if (m_io.discPaks == nullptr || m_io.entryCount == nullptr || m_io.open == nullptr ||
      m_io.readAt == nullptr || m_io.close == nullptr) {
    return false;
  }
  const int32_t baseCount = m_io.entryCount();
  for (const auto& [entry, path] : m_io.discPaks()) {
    // An entry the base disc does not have is a mod's own file, so its bytes
    // are never the disc's.
    if (entry < 0 || entry >= baseCount) {
      continue;
    }
    void* handle = m_io.open(entry);
    if (handle == nullptr) {
      continue;
    }
    auto found = m_handles.find(entry);
    if (found == m_handles.end()) {
      m_handles.emplace(entry, handle);
    } else {
      m_io.close(handle);
      handle = found->second;
    }
    PortMods::PakTable table;
    std::vector<uint8_t> header;
    size_t needed = 0x10000;
    bool parsed = false;
    while (needed <= kMaxTableBytes) {
      header.resize(needed);
      const size_t got = header.empty() ? 0 : size_t(m_io.readAt(handle, 0, header.data(), header.size()));
      if (PortMods::ParsePakTable(header.data(), got, table, needed)) {
        parsed = true;
        break;
      }
      // A short read means the file ends here; a full read means the table
      // simply needs more of it.
      if (got < header.size() || needed <= header.size()) {
        break;
      }
    }
    if (!parsed) {
      continue;
    }
    for (const PortMods::PakResource& res : table.resources) {
      if (res.type == kCMDL && res.size <= kMaxResourceBytes) {
        m_models.emplace(res.id, Where{entry, res.offset, res.size, res.compressed != 0});
      }
    }
  }
  m_found = !m_models.empty();
  return m_found;
}

bool Reader::Indexed() {
  std::lock_guard< std::mutex > lock(m_mutex);
  return m_indexed && m_found;
}

size_t Reader::ModelCount() {
  std::lock_guard< std::mutex > lock(m_mutex);
  return m_models.size();
}

bool Reader::ModelBounds(const Where& where, float out[6]) {
  const auto handle = m_handles.find(where.entry);
  if (handle == m_handles.end()) {
    return false;
  }
  if (!where.compressed) {
    std::vector<uint8_t> raw(where.size);
    if (where.size < kHeaderSize || m_io.readAt(handle->second, where.offset, raw.data(), raw.size()) !=
                                        int64_t(raw.size())) {
      return false;
    }
    return ParseCmdlBounds(raw.data(), raw.size(), out);
  }
  // Read straight into the string the inflater takes, so the blob is not copied
  // into one.
  std::string raw(where.size, '\0');
  if (where.size < 6 || m_io.readAt(handle->second, where.offset, reinterpret_cast<uint8_t*>(raw.data()),
                                    raw.size()) != int64_t(raw.size())) {
    return false;
  }
  // A big-endian length, then a zlib stream: two header bytes, the DEFLATE
  // data, and a checksum the decoder never reaches.
  const size_t length = ReadBE32(reinterpret_cast<const uint8_t*>(raw.data()));
  if (length < kHeaderSize || length > kMaxResourceBytes) {
    return false;
  }
  raw.erase(0, 6);
  PortWs::Inflater inflater;
  inflater.SetKeepWindow(false);
  std::string inflated;
  if (!inflater.InflateMessage(raw, inflated, length) || inflated.size() != length) {
    return false;
  }
  return ParseCmdlBounds(reinterpret_cast<const uint8_t*>(inflated.data()), inflated.size(), out);
}

bool Reader::Bounds(uint32_t id, float out[6]) {
  std::lock_guard< std::mutex > lock(m_mutex);
  const auto cached = m_cache.find(id);
  if (cached != m_cache.end()) {
    if (!cached->second.first) {
      return false;
    }
    std::copy(cached->second.second.begin(), cached->second.second.end(), out);
    return true;
  }
  float bounds[6] = {};
  bool ok = false;
  const auto model = m_models.find(id);
  if (model != m_models.end()) {
    ok = ModelBounds(model->second, bounds);
  }
  std::array< float, 6 > stored{};
  if (ok) {
    std::copy(bounds, bounds + 6, stored.begin());
  }
  m_cache.emplace(id, std::make_pair(ok, stored));
  if (!ok) {
    return false;
  }
  std::copy(stored.begin(), stored.end(), out);
  return true;
}

#ifndef PORT_ACTOR_COLLISION_BOUNDS_NO_DVD
namespace {

std::mutex g_mutex;
std::unique_ptr< Reader > g_reader;
bool g_reported = false;

} // namespace

bool ReadOriginalModelBounds(uint32_t id, float out[6]) {
  std::lock_guard< std::mutex > lock(g_mutex);
  if (g_reader == nullptr) {
    SourceIo io;
    io.discPaks = [] { return PortMods::DiscPaks(); };
    io.entryCount = [] { return int32_t(aurora_dvd_base_entry_count()); };
    io.open = [](int32_t entry) { return aurora_dvd_base_open(entry); };
    io.readAt = [](void* handle, uint64_t offset, uint8_t* buffer, size_t length) -> int64_t {
      if (!ReadAt(handle, offset, buffer, length)) {
        return 0;
      }
      return int64_t(length);
    };
    io.close = [](void* handle) { aurora_dvd_base_close(handle); };
    g_reader = std::make_unique< Reader >(io);
  }
  if (!g_reader->Build()) {
    // One line per run: the disc is not readable (no disc open, a mod replaced
    // every PAK, or an unexpected table), and every actor then keeps the box it
    // would have had anyway.
    if (!g_reported) {
      g_reported = true;
      PortLog::Write("actor collision: no models on the base disc; keeping drawn bounds\n");
    }
    return false;
  }
  return g_reader->Bounds(id, out);
}

void Reset() {
  std::lock_guard< std::mutex > lock(g_mutex);
  g_reader.reset();
  g_reported = false;
}
#endif // PORT_ACTOR_COLLISION_BOUNDS_NO_DVD

} // namespace PortActorCollisionBounds