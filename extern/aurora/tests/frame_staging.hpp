#pragma once

#include "gfx/frame_packet.hpp"
#include "gfx/resources.hpp"

#include <cstdint>
#include <deque>
#include <memory>

namespace aurora::gfx::detail::testing {

// Gives frame packets the fixed-size staging memory begin_frame() maps for them. Without it a
// bare FramePacket has capacity 0, so every push spills (spillUniforms etc.) and frame.uniforms
// stays empty. Must outlive the packets it was attached to.
class StagingPool {
public:
  void attach(FramePacket& frame) {
    const auto& sizes = frame_buffer_sizes();
    frame.verts = wrap(sizes.vertex);
    frame.uniforms = wrap(sizes.uniform);
    frame.indices = wrap(sizes.index);
    frame.storage = wrap(sizes.storage);
  }

private:
  ByteBuffer wrap(uint64_t size) {
    auto& block = m_blocks.emplace_back(new uint8_t[size]);
    return ByteBuffer{block.get(), size_t(size)};
  }

  std::deque<std::unique_ptr<uint8_t[]>> m_blocks;
};

} // namespace aurora::gfx::detail::testing
