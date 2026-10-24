#include "port_dread_bsmat.h"

#include <cstdio>
#include <cstring>

namespace PortDread {
namespace {

constexpr uint32_t kMaxEntries = 1u << 16;

// Sequential bounds-checked reader.
class Reader {
public:
  Reader(const uint8_t* d, size_t n) : d_(d), n_(n) {}
  const std::string& error() const { return err_; }
  size_t pos() const { return p_; }
  bool Fail(const std::string& what) {
    if (err_.empty()) {
      err_ = what + " (at offset " + std::to_string(p_) + ")";
    }
    return false;
  }
  bool Need(size_t n, const char* what) {
    if (n > n_ - p_) {
      return Fail(std::string("truncated reading ") + what);
    }
    return true;
  }
  bool U8(uint8_t& v, const char* w) {
    if (!Need(1, w)) {
      return false;
    }
    v = d_[p_++];
    return true;
  }
  bool Flag(bool& v, const char* w) {
    uint8_t b;
    if (!U8(b, w)) {
      return false;
    }
    v = b != 0;
    return true;
  }
  bool U32(uint32_t& v, const char* w) {
    if (!Need(4, w)) {
      return false;
    }
    v = uint32_t(d_[p_]) | (uint32_t(d_[p_ + 1]) << 8) | (uint32_t(d_[p_ + 2]) << 16) |
        (uint32_t(d_[p_ + 3]) << 24);
    p_ += 4;
    return true;
  }
  bool F32(float& v, const char* w) {
    uint32_t u;
    if (!U32(u, w)) {
      return false;
    }
    std::memcpy(&v, &u, 4);
    return true;
  }
  // NUL-terminated string.
  bool Str(std::string& s, const char* w) {
    const void* nul = p_ < n_ ? std::memchr(d_ + p_, 0, n_ - p_) : nullptr;
    if (nul == nullptr) {
      return Fail(std::string("unterminated string reading ") + w);
    }
    const size_t len = size_t(static_cast<const uint8_t*>(nul) - (d_ + p_));
    s.assign(reinterpret_cast<const char*>(d_ + p_), len);
    p_ += len + 1;
    return true;
  }
  bool Count(uint32_t& n, size_t min_elem, const char* w) {
    if (!U32(n, w)) {
      return false;
    }
    if (n > kMaxEntries || uint64_t(n) * min_elem > n_ - p_) {
      return Fail(std::string("count out of range reading ") + w);
    }
    return true;
  }

private:
  const uint8_t* d_;
  size_t n_;
  size_t p_ = 0;
  std::string err_;
};

bool ReadUniform(Reader& r, BsmatUniform& u) {
  uint8_t t;
  uint32_t n;
  if (!r.Str(u.name, "uniform name") || !r.U8(t, "uniform type")) {
    return false;
  }
  u.type = char(t);
  if (t != 'f' && t != 'i' && t != 'u') {
    return r.Fail("unknown uniform type '" + std::string(1, char(t)) + "'");
  }
  if (!r.Count(n, 4, "uniform value count")) {
    return false;
  }
  for (uint32_t k = 0; k < n; ++k) {
    uint32_t raw;
    if (!r.U32(raw, "uniform value")) {
      return false;
    }
    if (t == 'f') {
      float f;
      std::memcpy(&f, &raw, 4);
      u.f.push_back(f);
    } else if (t == 'i') {
      u.i.push_back(int32_t(raw));
    } else {
      u.u.push_back(raw);
    }
  }
  return true;
}

bool ReadSampler(Reader& r, BsmatSampler& s) {
  if (!r.Str(s.name, "sampler name") || !r.Str(s.sampler, "sampler sampler") ||
      !r.Str(s.type, "sampler type") || !r.U32(s.index, "sampler index") ||
      !r.Str(s.filepath, "sampler filepath") || !r.U32(s.min_filter, "min filter") ||
      !r.U32(s.max_filter, "max filter") || !r.U32(s.mip_filter, "mip filter") ||
      !r.U32(s.cmp_mode, "cmp mode") || !r.U32(s.wrap_u, "wrap u") || !r.U32(s.wrap_v, "wrap v")) {
    return false;
  }
  for (int i = 0; i < 4; ++i) {
    if (!r.U8(s.border_color[i], "border color")) {
      return false;
    }
  }
  return r.F32(s.min_lod, "min lod") && r.F32(s.lod_bias, "lod bias") &&
         r.F32(s.anisotropic, "anisotropic") && r.F32(s.max_mip_level, "max mip level") &&
         r.F32(s.max_anisotropy, "max anisotropy");
}

bool ReadUniforms(Reader& r, std::vector<BsmatUniform>& v, const char* w) {
  uint32_t n;
  if (!r.Count(n, 6, w)) {
    return false;
  }
  v.resize(n);
  for (auto& u : v) {
    if (!ReadUniform(r, u)) {
      return false;
    }
  }
  return true;
}

bool ReadSamplers(Reader& r, std::vector<BsmatSampler>& v, const char* w) {
  uint32_t n;
  if (!r.Count(n, 50, w)) {
    return false;
  }
  v.resize(n);
  for (auto& s : v) {
    if (!ReadSampler(r, s)) {
      return false;
    }
  }
  return true;
}

template <class T> const T* Find(const std::vector<T>& v, const std::string& name) {
  for (const T& e : v) {
    if (e.name == name) {
      return &e;
    }
  }
  return nullptr;
}

} // namespace

const BsmatUniform* BsmatStage::FindUniform(const std::string& n) const { return Find(uniforms, n); }
const BsmatSampler* BsmatStage::FindSampler(const std::string& n) const { return Find(samplers, n); }

const BsmatSampler* Bsmat::FindSampler(const std::string& n) const {
  for (const BsmatStage& s : stages) {
    if (const BsmatSampler* p = s.FindSampler(n)) {
      return p;
    }
  }
  return nullptr;
}

const BsmatUniform* Bsmat::FindUniform(const std::string& n) const {
  for (const BsmatStage& s : stages) {
    if (const BsmatUniform* p = s.FindUniform(n)) {
      return p;
    }
  }
  return nullptr;
}

bool ParseBsmat(const uint8_t* data, size_t size, Bsmat& out, std::string* error) {
  out = Bsmat{};
  Reader r(data == nullptr ? reinterpret_cast<const uint8_t*>("") : data, data == nullptr ? 0 : size);
  auto done = [&](bool ok) {
    if (!ok && error != nullptr) {
      *error = r.error().empty() ? "bad bsmat" : r.error();
    }
    return ok;
  };
  if (data == nullptr || size < 8 || std::memcmp(data, "MSUR", 4) != 0) {
    r.Fail("not an MSUR file");
    return done(false);
  }
  uint32_t ver, magic;
  r.U32(magic, "magic");
  r.U32(ver, "version");
  // Version word bytes 02 00 11 00: u16 major, u8 minor, u8 patch (mercury VersionAdapter).
  out.version_major = uint16_t(ver & 0xFFFF);
  out.version_minor = uint8_t((ver >> 16) & 0xFF);
  out.version_patch = uint8_t(ver >> 24);
  if (out.version_major != 2 || out.version_minor != 17 || out.version_patch != 0) {
    char b[48];
    std::snprintf(b, sizeof b, "unsupported MSUR version %u.%u.%u", out.version_major,
                  out.version_minor, out.version_patch);
    r.Fail(b);
    return done(false);
  }
  uint32_t k0, k1, k2, stage_count;
  const bool ok =
      r.Str(out.name, "name") && r.U32(out.translucency_type, "translucency type") &&
      r.U32(out.render_layer, "render layer") && r.Str(out.shader_path, "shader path") &&
      // blend state
      r.Flag(out.blend_enabled, "blend enabled") && r.U32(out.blend_op, "blend op") &&
      r.U32(out.blend_src, "blend src") && r.U32(out.blend_dst, "blend dst") &&
      r.U32(out.cull_mode, "cull mode") &&
      // stencil
      r.Flag(out.stencil_enabled, "stencil enabled") && r.U32(out.stencil_mask, "stencil mask") &&
      r.U32(out.stencil_ref, "stencil ref") && r.U32(out.stencil_fail, "stencil fail") &&
      r.U32(out.stencil_success, "stencil success") &&
      r.U32(out.stencil_depth_fail, "stencil depth fail") &&
      r.U32(out.stencil_depth_success, "stencil depth success") &&
      r.U32(out.stencil_cmp, "stencil cmp") &&
      // alpha test, fill mode, depth state
      r.Flag(out.alpha_test_enabled, "alpha test enabled") &&
      r.U32(out.alpha_test_func, "alpha test func") && r.F32(out.alpha_test_ref, "alpha test ref") &&
      r.U32(out.fill_mode, "fill mode") && r.Flag(out.depth_test, "depth test") &&
      r.Flag(out.depth_write, "depth write") && r.U32(out.depth_mode, "depth mode") &&
      r.Flag(out.z_prepass, "z prepass") &&
      // constants 0, 2, 0
      r.U32(k0, "const0") && r.U32(k1, "const1") && r.U32(k2, "const2") &&
      ReadUniforms(r, out.extra_uniforms, "extra uniform count") &&
      ReadSamplers(r, out.extra_samplers, "extra sampler count") &&
      r.Count(stage_count, 8, "stage count");
  if (!ok) {
    return done(false);
  }
  if (k0 != 0 || k1 != 2 || k2 != 0) {
    r.Fail("unexpected header constants");
    return done(false);
  }
  out.stages.resize(stage_count);
  for (BsmatStage& st : out.stages) {
    if (!ReadUniforms(r, st.uniforms, "stage uniform count") ||
        !ReadSamplers(r, st.samplers, "stage sampler count")) {
      return done(false);
    }
  }
  return done(true);
}

} // namespace PortDread
