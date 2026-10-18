#pragma once

#include "types.hpp"

namespace aurora::gfx {

BindGroupRef bind_group_ref(const WGPUBindGroupDescriptor& descriptor);
// Same, with an id the caller derived from a key that identifies the descriptor (cheaper than hashing every
// entry); the descriptor is only read when the bind group isn't cached yet.
BindGroupRef bind_group_ref(BindGroupRef id, const WGPUBindGroupDescriptor& descriptor);
wgpu::BindGroup find_bind_group(BindGroupRef id);
// Keeps a cached bind group from expiring this frame; false once it has.
bool touch_bind_group(BindGroupRef id);
wgpu::Sampler sampler_ref(const wgpu::SamplerDescriptor& descriptor);

namespace detail {
void clear_bind_group_cache();
// After the render worker finished frame `finishedFrame` (its current_frame() when recorded).
void expire_cached_bind_groups(uint32_t finishedFrame);
// After an EFB resize that kept the cache: once the worker has finished the first two frames
// at the new size, drop what they did not use.
void expire_unused_bind_groups_soon();
void shutdown_resource_cache();
} // namespace detail

} // namespace aurora::gfx
