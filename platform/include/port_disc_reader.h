#pragma once

// Every resource of the base disc's PAKs, read on demand: the generated
// layers (port_ball_looks.h, port_suit_colours.h) make their resources from
// the retail ones. Needs the disc open and PortMods::DiscPaks().

#include <cstdint>
#include <map>
#include <unordered_set>
#include <vector>

class PortDiscReader {
public:
  PortDiscReader() = default;
  PortDiscReader(const PortDiscReader&) = delete;
  PortDiscReader& operator=(const PortDiscReader&) = delete;
  ~PortDiscReader();

  // Reads the PAK tables; once, before the rest.
  void Index();
  // Whether any PAK holds a resource of that id, of any type.
  bool HasId(uint32_t id) const { return m_ids.count(id) != 0; }
  // The resource, decompressed. False when no PAK has it or it cannot be read.
  bool Read(uint32_t type, uint32_t id, std::vector<uint8_t>& out);

private:
  struct Where {
    int32_t entry = -1;
    uint32_t offset = 0;
    uint32_t size = 0;
    bool compressed = false;
  };

  static uint64_t Key(uint32_t type, uint32_t id) { return (uint64_t(type) << 32) | id; }
  static size_t ReadAt(void* handle, uint64_t offset, uint8_t* out, size_t size);

  std::map<int32_t, void*> m_handles;
  std::map<uint64_t, Where> m_where;
  std::unordered_set<uint32_t> m_ids;
};
