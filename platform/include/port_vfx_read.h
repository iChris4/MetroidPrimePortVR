#pragma once

// Bounds shared by CParticleDataFactory.cpp and platform/port_particle_vfx_read.cpp
// on the counts in a converted effect's port-only blocks.

#include "dolphin/types.h"

// Bounds on the port blocks' counts: a VMAT has three UV transforms, and the real particles carry
// a handful of VPMT/VSMT rows.
static constexpr u32 kPortVfxMaxVtmt = 3;
static constexpr u32 kPortVfxMaxElems = 64;
