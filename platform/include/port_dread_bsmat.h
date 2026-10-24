#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Reader for Metroid Dread material files (.bsmat, magic "MSUR", v2.17.0) from the user's own
// copy of the game. Field order follows mercury-engine-data-structures' BSMAT construct
// (MIT); layout notes: build/mpr/kb/format/dread-bsmat.md. Every count is bounds-checked
// against the span handed to ParseBsmat; bad data returns false with a message.
namespace PortDread {

// Enum values as stored (u32). Not every value is known; unknown ones pass through.
enum BsmatCullMode : uint32_t { kCullBack = 2, kCullFront = 3, kCullOff = 4 };
enum BsmatWrap : uint32_t { kWrapClamp = 0, kWrapClampColor = 1, kWrapRepeat = 2, kWrapMirror = 3 };

struct BsmatUniform {
  std::string name;
  char type = 'f';            // 'f' float, 'i' int32, 'u' uint32
  std::vector<float> f;       // filled when type == 'f'
  std::vector<int32_t> i;     // 'i'
  std::vector<uint32_t> u;    // 'u'
};

struct BsmatSampler {
  std::string name;           // e.g. texBaseColor
  std::string sampler;        // e.g. sampler0
  std::string type;           // e.g. texture2D
  uint32_t index = 0;
  std::string filepath;       // e.g. actors/.../textures/powerbody_bc.bctex (may be empty)
  uint32_t min_filter = 0, max_filter = 0, mip_filter = 0, cmp_mode = 0;
  uint32_t wrap_u = 0, wrap_v = 0; // BsmatWrap
  uint8_t border_color[4] = {};
  float min_lod = 0, lod_bias = 0, anisotropic = 0, max_mip_level = 0, max_anisotropy = 0;
};

struct BsmatStage {
  std::vector<BsmatUniform> uniforms;
  std::vector<BsmatSampler> samplers;
  const BsmatUniform* FindUniform(const std::string& name) const;
  const BsmatSampler* FindSampler(const std::string& name) const;
};

struct Bsmat {
  uint16_t version_major = 0;
  uint8_t version_minor = 0, version_patch = 0;
  std::string name;
  uint32_t translucency_type = 0;  // 0..3 tiling-style enum in mercury; 8 seen (additive-ish)
  uint32_t render_layer = 0;
  std::string shader_path;         // e.g. system/shd/mp_samuspbr.bshdat
  // Blend state.
  bool blend_enabled = false;
  uint32_t blend_op = 0, blend_src = 0, blend_dst = 0;
  uint32_t cull_mode = 0;          // BsmatCullMode
  // Stencil state.
  bool stencil_enabled = false;
  uint32_t stencil_mask = 0, stencil_ref = 0, stencil_fail = 0, stencil_success = 0,
           stencil_depth_fail = 0, stencil_depth_success = 0, stencil_cmp = 0;
  // Alpha test.
  bool alpha_test_enabled = false;
  uint32_t alpha_test_func = 0;
  float alpha_test_ref = 0;
  uint32_t fill_mode = 0;          // 0 solid, 1 wire
  // Depth state.
  bool depth_test = false, depth_write = false, z_prepass = false;
  uint32_t depth_mode = 0;
  std::vector<BsmatUniform> extra_uniforms;
  std::vector<BsmatSampler> extra_samplers;
  std::vector<BsmatStage> stages;

  // Look a sampler/uniform up in every stage (first match), or nullptr.
  const BsmatSampler* FindSampler(const std::string& name) const;
  const BsmatUniform* FindUniform(const std::string& name) const;
};

// On failure returns false, sets *error (if non-null) and leaves `out` unspecified. Trailing
// bytes after the last stage are ignored.
bool ParseBsmat(const uint8_t* data, size_t size, Bsmat& out, std::string* error);

} // namespace PortDread
