#pragma once

// Which opacity form a VMAT draw of a Remastered "OpacityFresnel" recipe uses.
//
// Those shaders compute their fade from two PMTR angles (the vertex stage takes their cosine) in one
// of two ways: the sprite permutations (000..) fold the normal term to a constant — aurora's
// OpacityFade, d = 0 — and the model permutations (016/017) test the interpolated view-space
// normal, d = |n.z|/|n| — aurora's OpacityFresnel. It is the same formula with those two `d` values,
// so a model draw (which always carries a normal) must use the fresnel feature: at the Power charge
// spheres' angles (90/0, that is A = 0, B = 1) the sprite form's fade is exactly 0 and the mesh
// draws nothing at all. Source formulas and the perm split:
// build/s-powercharge-20261016/spheres/FADE-AUDIT.md sections 1-2.
//
// VMAT v2/v3 records a material's recipe as its feature word, not as its shader id, so the
// model form is selected by the exact feature word of the one recipe whose model permutation was
// read against the shader: 0xe899b1df (VFX_ColorRamp_Indirect_OpacityFresnel_Unlit). Every other
// recipe carrying OpacityFade keeps the sprite form it has today — their model permutations have not
// been read, so nothing is claimed about them (FADE-AUDIT.md section 4 lists them). The word is a
// shared key: a row added to the converter's table with this word silently inherits this mesh form,
// so add one only together with its own mesh form. The guard is that count:
// tests/port_vfx_opacity_wiring.py parses the table and asserts exactly one row (0xe899b1df) carries
// the word, with and without the exposure policy bit. tests/port_remastered_effect_convert.cpp pins
// that recipe's converted word to this constant and tests/port_vfx_opacity.cpp the selection and the
// resulting alpha; neither of them would notice a new colliding row.
//
// VMAT v3 preserves this shader's CCH0 gain/bias, including SMTR overrides:
// gain * clamp(smooth3(f) - bias, 0, 1). Both forms share these scalar values.

#include <cstdint>

#include "aurora/vfx.hpp"

// 0xe899b1df's recipe: Ramp | Indirect | DepthSoften | OpacityFade. The policy bits a recipe may
// carry (aurora::gfx::vfx::AlreadyExposed) are not part of it.
inline constexpr uint32_t kPortVfxModelOpacityFeatures =
    uint32_t(aurora::gfx::vfx::Ramp) | uint32_t(aurora::gfx::vfx::Indirect) |
    uint32_t(aurora::gfx::vfx::DepthSoften) | uint32_t(aurora::gfx::vfx::OpacityFade);

// True when a material's features word is that one recipe's. `features` is the raw word as
// serialized (port_vfx_particles.cpp hands over CPortVfxMat::features, which still carries the
// exposure policy bit).
inline bool PortVfxUsesModelOpacity(uint32_t features) {
  return (features & ~uint32_t(aurora::gfx::vfx::AlreadyExposed)) == kPortVfxModelOpacityFeatures;
}

// The model (mesh) draw's opacity form: swaps the normal-free fade for the normal test, carrying the
// same two parameters over (the recipe maps PMTR row 2 comps 0/1 onto both forms, so the copies keep
// the shader's A = .x / B = .y order). Everything else in `desc` — blend, depth policy, textures,
// slots, the other features and the other sources — is untouched. Returns true when it changed the
// description.
//
// Only the mesh path calls this (CElementGen's model particles); a sprite draw of the same material
// keeps the form its VMAT carries, so sprites are unchanged.
inline bool PortVfxSelectModelOpacity(uint32_t features, aurora::gfx::vfx::DrawDesc& desc) {
  if (!PortVfxUsesModelOpacity(features) || (desc.features & aurora::gfx::vfx::OpacityFade) == 0) {
    return false;
  }
  desc.features = (desc.features & ~uint32_t(aurora::gfx::vfx::OpacityFade)) |
                  uint32_t(aurora::gfx::vfx::OpacityFresnel);
  desc.fresnelX = desc.fadeX;
  desc.fresnelY = desc.fadeY;
  // opacityGain/Bias are scalar material values shared by both forms; leave them untouched.
  return true;
}
