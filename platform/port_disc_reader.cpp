// The base disc's PAK resources, read on demand (port_disc_reader.h).

#include "port_disc_reader.h"

#include "port_bytes.h"
#include "port_mods.h"
#include "port_ws.h"

#include <aurora/dvd.h>

#include <string>

PortDiscReader::~PortDiscReader() {
  for (auto& [entry, handle] : m_handles) {
    aurora_dvd_base_close(handle);
  }
}

void PortDiscReader::Index() {
  const int32_t baseCount = aurora_dvd_base_entry_count();
  for (const auto& [entry, path] : PortMods::DiscPaks()) {
    if (entry < 0 || entry >= baseCount) {
      continue;  // a PAK only a mod brings
    }
    void* handle = aurora_dvd_base_open(entry);
    if (handle == nullptr) {
      continue;
    }
    m_handles[entry] = handle;
    std::vector<uint8_t> header;
    PortMods::PakTable table;
    size_t needed = 0x10000;
    bool parsed = false;
    while (needed <= (64u << 20)) {
      header.resize(needed);
      const size_t got = ReadAt(handle, 0, header.data(), header.size());
      if (PortMods::ParsePakTable(header.data(), got, table, needed)) {
        parsed = true;
        break;
      }
      if (got < header.size() || needed <= header.size()) {
        break;
      }
    }
    if (!parsed) {
      continue;
    }
    for (const PortMods::PakResource& res : table.resources) {
      m_ids.insert(res.id);
      m_where.emplace(Key(res.type, res.id), Where{entry, res.offset, res.size, res.compressed != 0});
    }
  }
}

bool PortDiscReader::Read(uint32_t type, uint32_t id, std::vector<uint8_t>& out) {
  const auto found = m_where.find(Key(type, id));
  if (found == m_where.end()) {
    return false;
  }
  const Where& where = found->second;
  std::vector<uint8_t> raw(where.size);
  if (ReadAt(m_handles[where.entry], where.offset, raw.data(), raw.size()) != raw.size()) {
    return false;
  }
  if (!where.compressed) {
    out = std::move(raw);
    return true;
  }
  // A big-endian length, then a zlib stream: two header bytes, the DEFLATE
  // data, and a checksum the decoder never reaches.
  if (raw.size() < 6) {
    return false;
  }
  const size_t length = port::ReadBE32(raw.data());
  const std::string deflated(raw.begin() + 6, raw.end());
  PortWs::Inflater inflater;
  inflater.SetKeepWindow(false);
  std::string inflated;
  if (!inflater.InflateMessage(deflated, inflated, length) || inflated.size() != length) {
    return false;
  }
  out.assign(inflated.begin(), inflated.end());
  return true;
}

size_t PortDiscReader::ReadAt(void* handle, uint64_t offset, uint8_t* out, size_t size) {
  if (aurora_dvd_base_seek(handle, int64_t(offset), 0) != int64_t(offset)) {
    return 0;
  }
  size_t done = 0;
  while (done < size) {
    const int64_t got = aurora_dvd_base_read(handle, out + done, size - done);
    if (got <= 0) {
      break;
    }
    done += size_t(got);
  }
  return done;
}
