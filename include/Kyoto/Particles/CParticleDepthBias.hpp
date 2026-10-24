#pragma once

// Port-only: Remastered's particle depth bias (DBIS -> PDBS) around a generator's draws.
// CParticleSceneNode::ConstructDepthBias 0x2b1004 wraps the draw in a CScopedGraphicsDepthBias
// computed at the bounds centre's view depth; see PortParticleMath::DepthBiasedProjM22.
#ifdef TARGET_PC

#include "Kyoto/Graphics/CGraphics.hpp"
#include "Kyoto/Math/CAABox.hpp"
#include "Kyoto/Particles/CParticleGen.hpp"

class CPortParticleDepthBias {
public:
  // `gen`'s draws until the scope ends; nested scopes (child generators) restore the parent's bias.
  CPortParticleDepthBias(const CParticleGen& gen, float bias) {
    State& s = state();
    prev = s;
    if (bias == 0.f && prev.bias == 0.f) {
      return;
    }
    State next;
    if (bias != 0.f) {
      const rstl::optional_object< CAABox > bounds = gen.GetBounds();
      if (bounds) {
        next.bias = bias;
        next.depth = (CGraphics::GetViewMatrix() * bounds->GetCenterPoint()).GetY();
      }
    }
    apply(next);
    active = true;
  }
  ~CPortParticleDepthBias() {
    if (active) {
      apply(prev);
    }
  }
  CPortParticleDepthBias(const CPortParticleDepthBias&) = delete;
  CPortParticleDepthBias& operator=(const CPortParticleDepthBias&) = delete;

private:
  struct State {
    float bias = 0.f;
    float depth = 0.f;
  };
  static State& state() {
    static thread_local State s;
    return s;
  }
  static void apply(const State& next) {
    state() = next;
    CGraphics::PortSetProjectionDepthBias(next.bias, next.depth);
  }
  State prev;
  bool active = false;
};

#endif
