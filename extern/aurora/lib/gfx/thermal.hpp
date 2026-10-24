#pragma once

#include <cstdint>
#include <string>

#include <webgpu/webgpu_cpp.h>

// Port extension: Remastered's thermal visor post (NRenderThermalVisor: cold 0xfb1aa0 / hot 0xfb1cf0 /
// upscale 0xfb2120), run on the HDR scene (see GXPortThermalPass).
namespace aurora::gfx::thermal {
// The task's uniform, word for word.
struct Params {
  float tone[3][4]; // the tone curve the scene was drawn through (as GXSetPBRTone); unused
  float v[4];       // pass (0 cold, 1 hot), area heat, thermal time (s), ghost (1: blend the previous frame)
  float r[4];       // cold: the two CRandom16 draws of this call
};
static_assert(sizeof(Params) == 20 * 4);

// The task's WGSL (exposed for the tests). The passes read and write the RGBA16F exposed linear HDR
// scene the thermal target sits over (kb topic/visor-hdr-pipeline.md), where the cold pass's /E and
// its *E cancel (thermal-post.md).
std::string source();

// The heat gradient: 256x4 RGBA8, as the import writes it. Call before the first pass; false if it
// is not that size.
bool set_lut(const uint8_t* rgba, uint32_t size);
bool has_lut();
// Registers the task (game thread); false if it could not be or there is no gradient.
bool ensure_task();
// Records a pass from the FIFO processor (GX_AURORA_PORT_THERMAL), once ensure_task has returned true.
bool record(const Params& params);
void shutdown();
} // namespace aurora::gfx::thermal
