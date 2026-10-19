#pragma once

#include <atomic>
#include <cstdint>

#include <webgpu/webgpu_cpp.h>

namespace aurora::gfx::texture_sites {
// A direct wgpu::Device::CreateTexture call site that is not a TextureRef: how many textures it has made and their
// size (cumulative; Dawn does not say when one dies). Listed by `stats textures`.
struct Site {
  const char* name;
  std::atomic<uint64_t> count{0};
  std::atomic<uint64_t> bytes{0};
  std::atomic<bool> registered{false};
  Site* next = nullptr;
};

void note(Site& site, const wgpu::TextureDescriptor* descriptor) noexcept;
} // namespace aurora::gfx::texture_sites

// Put before a CreateTexture call that takes `descriptor` (a pointer); costs two relaxed atomic adds.
#define AURORA_NOTE_TEXTURE(descriptor)                                                                             \
  do {                                                                                                              \
    static ::aurora::gfx::texture_sites::Site auroraTextureSite{__FILE__ ":" AURORA_NOTE_STR(__LINE__)};             \
    ::aurora::gfx::texture_sites::note(auroraTextureSite, descriptor);                                              \
  } while (0)
#define AURORA_NOTE_STR(x) AURORA_NOTE_STR2(x)
#define AURORA_NOTE_STR2(x) #x
