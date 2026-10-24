#pragma once

#include "types.hpp"

#include <functional>

namespace aurora::gfx::clear {
struct PipelineConfig;
} // namespace aurora::gfx::clear

namespace aurora::gfx::vfx {
struct PipelineConfig;
} // namespace aurora::gfx::vfx

namespace aurora::gfx::water {
struct PipelineConfig;
} // namespace aurora::gfx::water

namespace aurora::gx {
struct PipelineConfig;
} // namespace aurora::gx

namespace aurora::rmlui {
struct PipelineConfig;
} // namespace aurora::rmlui

namespace aurora::gfx {

enum class ShaderType : uint8_t {
  Clear = 0,
  GX = 1,
  Rml = 2,
  Vfx = 3,
  Water = 4,
};

enum class PipelinePriority {
  Background, // loaded from cache
  Normal,     // async skip draw
  Blocking,   // block until compiled
};

using NewPipelineCallback = std::function<wgpu::RenderPipeline()>;

void initialize_pipeline_cache();
void shutdown_pipeline_cache();
void begin_pipeline_frame();
// Forgets every built and queued pipeline; the next lookup rebuilds them (shader reload).
void drop_pipelines();
// Holds background (cache-seed) builds while paused; pipelines a lookup needs still compile.
void set_background_pipelines_paused(bool paused);
void end_pipeline_frame();

template <typename Config>
PipelineRef find_pipeline(ShaderType type, const Config& config, NewPipelineCallback&& cb);

// As above, with an explicit priority. A Blocking lookup takes a queued build to the front of the
// queue and waits for it, so a caller that must not lose its draw (the GPU self-test) finds the
// pipeline built when it returns; Normal is the default, async, build-ahead-a-frame behaviour.
PipelineRef find_pipeline(ShaderType type, const gx::PipelineConfig& config, NewPipelineCallback&& cb,
                          PipelinePriority priority);

bool get_pipeline(PipelineRef ref, wgpu::RenderPipeline& pipeline);

// Render thread: a draw was dropped because its pipeline wasn't built (aurora_get_pipeline_stats).
void note_skipped_draw();

// Draw thread: whether `ref`, returned by an earlier find_pipeline, is still built or queued (a
// queued background build moves up, as a lookup would). False after drop_pipelines(), so the
// caller looks the config up again.
bool touch_pipeline(PipelineRef ref);

// Render thread (custom draw callbacks): the pipeline for `config`, waiting for it if the cache
// hasn't built it yet (a queued one moves to the front). Null if creation failed.
template <typename Config>
wgpu::RenderPipeline require_pipeline(ShaderType type, const Config& config, NewPipelineCallback&& cb);

} // namespace aurora::gfx
