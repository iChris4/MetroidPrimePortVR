#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Remastered keeps many retail models under their retail ids and places each object's copy with
// an entity of its own. Where those entities don't all put the model at one transform against
// their retail objects (Remastered moved some of them on their own), the import converts the
// model once in its own space and lists, per object, the transform its entity adds: the object's
// inverse editor transform times the entity's (port_remastered_room.cpp, PairRetailModels). The
// object then draws the model through it: world = xf * scale * correction * v.
//
// The import writes them to retailplacements.bin, little endian:
//   'MPRP', u32 version (1), u32 count, then per record: u32 MREA id, u32 editor id (layer bits
//   included), u32 CMDL id, f32 correction[12] (a 3x4 matrix by rows: the orient's rows, each
//   followed by that row's offset; GC axes, the object's space).
namespace PortRetailPlacement {

constexpr uint32_t kFileMagic = 0x5052504D;  // 'MPRP'
constexpr uint32_t kFileVersion = 1;
constexpr const char* kFileName = "retailplacements.bin";
// The editor id bits a placement is keyed by: the area and instance, not the layer.
constexpr uint32_t kEditorIdMask = 0x3FFFFFF;

struct Record {
  uint32_t mrea = 0;
  uint32_t editorId = 0;
  uint32_t model = 0;
  std::array<float, 12> correction{};
};

std::vector<uint8_t> Encode(const std::vector<Record>& records);
// False (and `records` cleared) when the bytes are not a whole file of this version.
bool Decode(const uint8_t* data, size_t size, std::vector<Record>& records);

// out = xf * diag(scale) * correction, all 3x4 by rows: the model matrix of an object's draw.
void Compose(const float xf[12], const float scale[3], const float correction[12], float out[12]);

// Runtime (port_retail_placement.cpp): the correction an object's model draws through, from
// the mods' retailplacements.bin (loaded on first use, again when the mods change). False when
// none: the model draws as retail places it.
bool Find(uint32_t mrea, uint32_t editorId, uint32_t model, float correction[12]);
// Whether some object's copy of `model` has a correction: one without draws misplaced.
bool Placed(uint32_t model);

}  // namespace PortRetailPlacement
