#pragma once

// Tags the pipelines the draws inside a scope look up with a context (issue #187): an area, or
// GLOBAL for what every area draws (Samus, the gun, the HUD), so a lazy pipeline cache can build
// an area's pipelines in the background before it streams in. Draw thread only.

#include <aurora/gfx.h>

#include <cstdint>

class CPortPipelineContextScope {
public:
  explicit CPortPipelineContextScope(uint64_t context) : xPrev(aurora_pipeline_set_context(context)) {}
  ~CPortPipelineContextScope() { aurora_pipeline_set_context(xPrev); }
  CPortPipelineContextScope(const CPortPipelineContextScope&) = delete;
  CPortPipelineContextScope& operator=(const CPortPipelineContextScope&) = delete;

private:
  uint64_t xPrev;
};

inline uint64_t PortPipelineAreaContext(uint32_t mlvl, int32_t area) {
  return AURORA_PIPELINE_CONTEXT_AREA(mlvl, static_cast< uint32_t >(area));
}
