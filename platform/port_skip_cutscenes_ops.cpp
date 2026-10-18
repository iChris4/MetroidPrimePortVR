#include "port_skip_cutscenes.h"
#include "port_bytes.h"

#include <algorithm>
#include <cstring>
#include <utility>

// The op stream, per room (all big-endian; tools/gen_skippable_cutscenes.py
// writes it and holds the Python twin of ApplyOps):
//   1 CLONE    u8 layer, u32 source, u32 id     copy a disc object, append
//   2 EDIT     u32 id, u16 n, name[n], u16 count, count x {u16 off, u16 len,
//              data[len]}                        patch the properties after the
//                                                name; a non-empty name replaces it
//   3 REMCONN  u32 sender, u32 state, u32 msg, u32 target
//   4 ADDCONN  u32 sender, u32 state, u32 msg, u32 target
//   5 PUSH     u8 layer, u8 type, u32 id, u16 n, n x conn, u32 len, props[len]
//   6 DELETE   u32 id
//   7 MOVE     u8 layer, u32 id                  remove, then append to layer
// Properties are an object's data after its connections: u32 property count,
// the NUL-terminated name, then the rest.
namespace PortSkipCutscenes {
namespace {

enum { kClone = 1, kEdit, kRemConn, kAddConn, kPush, kDelete, kMove };

struct Conn {
  uint32_t state, msg, target;
  bool operator==(const Conn& o) const {
    return state == o.state && msg == o.msg && target == o.target;
  }
};

struct Object {
  uint8_t type;
  uint32_t id;
  std::vector< Conn > conns;
  std::vector< uint8_t > props;
};

struct Layer {
  uint8_t unk;
  std::vector< Object > objects;
};

// Big-endian; a read past the end yields zeros and clears `ok` for good.
using Reader = port::ByteCursor;

using port::AppendBE32;

bool Parse(const uint8_t* scly, size_t size, uint32_t& version, std::vector< Layer >& layers) {
  Reader r{scly, size};
  if (size < 12 || std::memcmp(scly, "SCLY", 4) != 0)
    return false;
  r.at += 4;
  version = r.U32();
  const uint32_t count = r.U32();
  if (count > 64)
    return false;
  std::vector< uint32_t > sizes(count);
  for (uint32_t& s : sizes)
    s = r.U32();
  const uint8_t* layerStart = r.data + r.at;
  layers.resize(count);
  for (uint32_t i = 0; i < count && r.ok; ++i) {
    Reader lr{layerStart, size_t(scly + size - layerStart)};
    if (!lr.Has(sizes[i]))
      return false;
    lr.size = sizes[i];
    layers[i].unk = lr.U8();
    const uint32_t objects = lr.U32();
    for (uint32_t k = 0; k < objects && lr.ok; ++k) {
      Object o;
      o.type = lr.U8();
      const uint32_t len = lr.U32();
      if (!lr.Has(len) || len < 8)
        return false;
      Reader orr{lr.data + lr.at, len};
      lr.at += len;
      o.id = orr.U32();
      const uint32_t conns = orr.U32();
      for (uint32_t c = 0; c < conns && orr.ok; ++c) {
        Conn conn;
        conn.state = orr.U32();
        conn.msg = orr.U32();
        conn.target = orr.U32();
        o.conns.push_back(conn);
      }
      if (!orr.ok)
        return false;
      o.props.assign(orr.data + orr.at, orr.data + orr.size);
      layers[i].objects.push_back(std::move(o));
    }
    if (!lr.ok)
      return false;
    layerStart += sizes[i];
  }
  return r.ok;
}

void Write(uint32_t version, const std::vector< Layer >& layers, std::vector< uint8_t >& out) {
  std::vector< std::vector< uint8_t > > blobs;
  for (const Layer& layer : layers) {
    std::vector< uint8_t > b;
    b.push_back(layer.unk);
    AppendBE32(b, static_cast< uint32_t >(layer.objects.size()));
    for (const Object& o : layer.objects) {
      b.push_back(o.type);
      AppendBE32(b, static_cast< uint32_t >(8 + 12 * o.conns.size() + o.props.size()));
      AppendBE32(b, o.id);
      AppendBE32(b, static_cast< uint32_t >(o.conns.size()));
      for (const Conn& c : o.conns) {
        AppendBE32(b, c.state);
        AppendBE32(b, c.msg);
        AppendBE32(b, c.target);
      }
      b.insert(b.end(), o.props.begin(), o.props.end());
    }
    b.resize((b.size() + 31) & ~size_t(31)); // as randomprime writes them
    blobs.push_back(std::move(b));
  }
  out.clear();
  out.insert(out.end(), {'S', 'C', 'L', 'Y'});
  AppendBE32(out, version);
  AppendBE32(out, static_cast< uint32_t >(layers.size()));
  for (const auto& b : blobs)
    AppendBE32(out, static_cast< uint32_t >(b.size()));
  for (const auto& b : blobs)
    out.insert(out.end(), b.begin(), b.end());
}

Object* Find(std::vector< Layer >& layers, uint32_t id) {
  for (Layer& layer : layers)
    for (Object& o : layer.objects)
      if (o.id == id)
        return &o;
  return nullptr;
}

// Offset of the properties after the name, or 0 if there is no name.
size_t NameEnd(const std::vector< uint8_t >& props) {
  for (size_t i = 4; i < props.size(); ++i)
    if (props[i] == 0)
      return i + 1;
  return 0;
}

} // namespace

int ApplyOps(const uint8_t* scly, size_t size, const uint8_t* ops, size_t opsSize,
             std::vector< uint8_t >& out) {
  uint32_t version = 0;
  std::vector< Layer > layers;
  out.clear();
  if (!Parse(scly, size, version, layers))
    return -1;

  int misses = 0;
  Reader r{ops, opsSize};
  while (r.ok && r.at < r.size) {
    const uint8_t op = r.U8();
    if (op == kClone) {
      const uint8_t li = r.U8();
      const uint32_t src = r.U32();
      const uint32_t id = r.U32();
      const Object* s = Find(layers, src);
      if (!r.ok || s == nullptr || li >= layers.size()) {
        ++misses;
        continue;
      }
      Object copy = *s;
      copy.id = id;
      layers[li].objects.push_back(std::move(copy));
    } else if (op == kEdit) {
      const uint32_t id = r.U32();
      const uint16_t nameLen = r.U16();
      const uint8_t* name = r.Bytes(nameLen);
      const uint16_t count = r.U16();
      struct Patch {
        uint16_t off, len;
        const uint8_t* data;
      };
      std::vector< Patch > patches;
      for (uint16_t i = 0; i < count && r.ok; ++i) {
        Patch patch;
        patch.off = r.U16();
        patch.len = r.U16();
        patch.data = r.Bytes(patch.len);
        patches.push_back(patch);
      }
      if (!r.ok)
        break;
      Object* o = Find(layers, id);
      const size_t tail = o != nullptr ? NameEnd(o->props) : 0;
      bool fits = tail != 0;
      for (const Patch& patch : patches)
        fits = fits && tail + patch.off + patch.len <= o->props.size();
      if (!fits) {
        ++misses;
        continue;
      }
      for (const Patch& patch : patches)
        std::memcpy(o->props.data() + tail + patch.off, patch.data, patch.len);
      if (nameLen != 0) {
        o->props.erase(o->props.begin() + 4, o->props.begin() + tail);
        o->props.insert(o->props.begin() + 4, name, name + nameLen);
      }
    } else if (op == kRemConn || op == kAddConn) {
      const uint32_t id = r.U32();
      Conn conn;
      conn.state = r.U32();
      conn.msg = r.U32();
      conn.target = r.U32();
      Object* o = Find(layers, id);
      if (!r.ok || o == nullptr) {
        ++misses;
        continue;
      }
      if (op == kAddConn) {
        o->conns.push_back(conn);
      } else {
        auto it = std::find(o->conns.begin(), o->conns.end(), conn);
        if (it == o->conns.end())
          ++misses;
        else
          o->conns.erase(it);
      }
    } else if (op == kPush) {
      Object o;
      const uint8_t li = r.U8();
      o.type = r.U8();
      o.id = r.U32();
      const uint16_t conns = r.U16();
      for (uint16_t c = 0; c < conns && r.ok; ++c) {
        Conn conn;
        conn.state = r.U32();
        conn.msg = r.U32();
        conn.target = r.U32();
        o.conns.push_back(conn);
      }
      const uint32_t len = r.U32();
      const uint8_t* props = r.Bytes(len);
      if (!r.ok)
        break;
      if (li >= layers.size()) {
        ++misses;
        continue;
      }
      o.props.assign(props, props + len);
      layers[li].objects.push_back(std::move(o));
    } else if (op == kMove) {
      const uint8_t li = r.U8();
      const uint32_t id = r.U32();
      Object* found = Find(layers, id);
      if (!r.ok || found == nullptr || li >= layers.size()) {
        ++misses;
        continue;
      }
      Object o = std::move(*found);
      for (Layer& layer : layers) {
        auto it = std::find_if(layer.objects.begin(), layer.objects.end(),
                               [&](const Object& x) { return &x == found; });
        if (it != layer.objects.end()) {
          layer.objects.erase(it);
          break;
        }
      }
      layers[li].objects.push_back(std::move(o));
    } else if (op == kDelete) {
      const uint32_t id = r.U32();
      for (Layer& layer : layers)
        layer.objects.erase(std::remove_if(layer.objects.begin(), layer.objects.end(),
                                           [&](const Object& x) { return x.id == id; }),
                            layer.objects.end());
    } else {
      r.ok = false;
    }
  }
  if (!r.ok)
    return -1;
  Write(version, layers, out);
  return misses;
}

bool ScanObjects(const uint8_t* scly, size_t size, std::vector< ScriptObject >& out) {
  uint32_t version = 0;
  std::vector< Layer > layers;
  out.clear();
  if (!Parse(scly, size, version, layers))
    return false;
  for (size_t i = 0; i < layers.size(); ++i) {
    for (Object& o : layers[i].objects) {
      ScriptObject object;
      object.layer = static_cast< int >(i);
      object.type = o.type;
      object.id = o.id;
      for (const Conn& c : o.conns)
        object.connections.push_back({c.state, c.msg, c.target});
      object.props.swap(o.props);
      out.push_back(std::move(object));
    }
  }
  return true;
}

} // namespace PortSkipCutscenes
