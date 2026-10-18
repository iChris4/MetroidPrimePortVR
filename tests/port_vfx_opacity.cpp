// The model (mesh) opacity form of Remastered's "OpacityFresnel" recipes: which material selects it,
// what it changes in the draw description and what alpha it produces. The formulas are the shader's
// own (extern/aurora/lib/gfx/vfx.cpp: feature 2048 OpacityFade uses d = 0, feature 256 OpacityFresnel
// d = |n.z|/|n|; both then gain * sat(smooth3(sat((d - A) / (B - A))) - bias)), and the material is the
// Power charge sphere's (build/s-powercharge-20261016/spheres/FADE-AUDIT.md).

#include "port_vfx_opacity.h"
#include "port_test.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>

using aurora::gfx::vfx::Blend;
using aurora::gfx::vfx::DrawDesc;
using aurora::gfx::vfx::Feature;
using aurora::gfx::vfx::Src;
using aurora::gfx::vfx::select_model_depth;

namespace {

// The shader's own arithmetic (extern/aurora/lib/gfx/vfx.cpp) is float and its degree constant is
// 0.0174532942, so cos(90) is -1.7e-7 rather than 0: comparisons that are "zero" use this bound.
constexpr float kTiny = 1e-6f;

float Sat(float x) { return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); }

float Smooth3(float x) { return x * x * (3.f - 2.f * x); }

// vfx.cpp src(): the two fade angles are degrees and the shader takes their cosine. The component the
// source names (row, comp of the per-particle extras) is the angle: the Power material's PMTR row 2
// comps 0/1 are 90 and 0, which is what the test's Src values carry.
float AngleDegrees(const Src& src) { return src.value; }

// The alpha factor of the fade feature the description carries, at d = |n.z| / |n| (the sprite form
// has no normal, so its d is 0).
float FadeFactor(const DrawDesc& desc, float d) {
  const bool fresnel = (desc.features & uint32_t(Feature::OpacityFresnel)) != 0;
  const bool fade = (desc.features & uint32_t(Feature::OpacityFade)) != 0;
  if (!fresnel && !fade) {
    return 1.f;
  }
  const Src& x = fresnel ? desc.fresnelX : desc.fadeX;
  const Src& y = fresnel ? desc.fresnelY : desc.fadeY;
  const float a = std::cos(AngleDegrees(x) * 0.0174532942f);
  const float b = std::cos(AngleDegrees(y) * 0.0174532942f);
  const float term = fresnel ? d : 0.f;
  const float q = b - a;
  return desc.opacityGain * Sat(Smooth3(q != 0.f ? Sat((term - a) / q) : (term >= a ? 1.f : 0.f)) - desc.opacityBias);
}

// The Power charge sphere's VMAT, as the loader builds it: PMTR row 2 comps 0/1 are the fade angles
// (90/0 in the source dump), blend alpha (PBDM 0), and the recipe's feature word.
DrawDesc PowerSphereDesc(uint32_t features = kPortVfxModelOpacityFeatures,
                         uint32_t blend = uint32_t(Blend::Alpha)) {
  DrawDesc desc;
  desc.features = features & ~uint32_t(Feature::AlreadyExposed);
  desc.blend = static_cast< Blend >(blend);
  desc.rampSlot = 0;
  desc.indirectSlot = 1;
  desc.modulate = 1.f;
  desc.rampRow[0] = 0;
  desc.rampRow[1] = 1;
  desc.addRow = -1;
  desc.fadeX = Src{2, 0, 90.f};
  desc.fadeY = Src{2, 1, 0.f};
  desc.tex[0].uvSet = 0;
  desc.tex[0].cols = 2;
  return desc;
}

constexpr uint32_t kAllFeatures = uint32_t(Feature::ColorTex) | uint32_t(Feature::OpacityTex) |
                                  uint32_t(Feature::Erosion) | uint32_t(Feature::Ramp) |
                                  uint32_t(Feature::Indirect) | uint32_t(Feature::DepthSoften) |
                                  uint32_t(Feature::Thresholding) | uint32_t(Feature::DualMod) |
                                  uint32_t(Feature::OpacityFresnel) | uint32_t(Feature::ColorIndexing) |
                                  uint32_t(Feature::AddColor) | uint32_t(Feature::OpacityFade) |
                                  uint32_t(Feature::ColorRgbOnly);

// Only this material selects the model form; every other recipe keeps the description it has.
void TestSelectionIsThePinnedRecipe() {
  CHECK_MSG(PortVfxUsesModelOpacity(kPortVfxModelOpacityFeatures), "the recipe's own word selects the model form");
  CHECK_MSG(PortVfxUsesModelOpacity(kPortVfxModelOpacityFeatures | uint32_t(Feature::AlreadyExposed)),
            "the exposure policy bit is not part of the recipe");
  // Every other recipe that carries OpacityFade today (FADE-AUDIT.md section 4); none selects it.
  // A hand copy, so it proves only these words: a new converter row reusing the key would fail
  // tests/port_vfx_opacity_wiring.py, not this list.
  const uint32_t otherFadeRecipes[] = {
      uint32_t(Feature::Ramp) | uint32_t(Feature::Indirect) | uint32_t(Feature::OpacityFade),          // 0x818
      uint32_t(Feature::ColorTex) | uint32_t(Feature::OpacityTex) | uint32_t(Feature::Erosion) |
          uint32_t(Feature::DepthSoften) | uint32_t(Feature::OpacityFade),                             // 0x827
      uint32_t(Feature::ColorTex) | uint32_t(Feature::Erosion) | uint32_t(Feature::OpacityFade),       // 0x805
      uint32_t(Feature::Ramp) | uint32_t(Feature::Indirect) | uint32_t(Feature::DualMod) |
          uint32_t(Feature::OpacityFade),                                                              // 0x898
      uint32_t(Feature::OpacityTex) | uint32_t(Feature::Ramp) | uint32_t(Feature::DualMod) |
          uint32_t(Feature::OpacityFade),                                                              // 0x88a
      uint32_t(Feature::ColorTex) | uint32_t(Feature::OpacityTex) | uint32_t(Feature::Erosion) |
          uint32_t(Feature::OpacityFade),                                                              // 0x807
  };
  for (const uint32_t word : otherFadeRecipes) {
    CHECK_MSG(!PortVfxUsesModelOpacity(word), "another OpacityFade recipe does not select the model form");
    CHECK_MSG(!PortVfxUsesModelOpacity(word | uint32_t(Feature::AlreadyExposed)), "nor with the policy bit");
    DrawDesc desc = PowerSphereDesc(word);
    const DrawDesc before = desc;
    CHECK_MSG(!PortVfxSelectModelOpacity(word, desc), "another OpacityFade recipe's mesh draw is left alone");
    CHECK_MSG(std::memcmp(&desc, &before, sizeof(desc)) == 0, "and its description is byte-identical");
  }
  // A material with no fade feature has nothing to swap, and a second call changes nothing.
  DrawDesc plain;
  plain.features = kPortVfxModelOpacityFeatures & ~uint32_t(Feature::OpacityFade);
  const DrawDesc plainBefore = plain;
  CHECK_MSG(!PortVfxSelectModelOpacity(kPortVfxModelOpacityFeatures, plain), "no fade feature, nothing to swap");
  CHECK_MSG(std::memcmp(&plain, &plainBefore, sizeof(plain)) == 0, "and the description is byte-identical");
  DrawDesc already = PowerSphereDesc();
  CHECK_MSG(PortVfxSelectModelOpacity(kPortVfxModelOpacityFeatures, already), "the first call swaps");
  const DrawDesc swapped = already;
  CHECK_MSG(!PortVfxSelectModelOpacity(kPortVfxModelOpacityFeatures, already), "the second call is a no-op");
  CHECK_MSG(std::memcmp(&already, &swapped, sizeof(already)) == 0, "and changes nothing");
}

// Mesh versus sprite: the same material, only the fade form differs, and its parameters carry over.
void TestMeshVersusSprite() {
  const DrawDesc sprite = PowerSphereDesc(); // the sprite path draws this, unmodified
  DrawDesc mesh = sprite;
  CHECK_MSG(PortVfxSelectModelOpacity(kPortVfxModelOpacityFeatures | uint32_t(Feature::AlreadyExposed), mesh),
            "the mesh draw of the Power material swaps the form");

  const uint32_t moved = sprite.features ^ mesh.features;
  CHECK_MSG((sprite.features & uint32_t(Feature::OpacityFade)) != 0, "the sprite keeps the normal-free fade");
  CHECK_MSG((mesh.features & uint32_t(Feature::OpacityFade)) == 0, "the mesh drops it");
  CHECK_MSG((mesh.features & uint32_t(Feature::OpacityFresnel)) != 0, "the mesh carries the normal test");
  CHECK_MSG(moved == (uint32_t(Feature::OpacityFade) | uint32_t(Feature::OpacityFresnel)),
            "no other feature flag moves");
  CHECK_MSG((sprite.features & uint32_t(Feature::OpacityFresnel)) == 0, "the sprite never carries the normal test");
  CHECK_MSG((sprite.features & ~kAllFeatures) == 0 && (mesh.features & ~kAllFeatures) == 0,
            "neither draw carries a bit outside the feature set");

  // The parameters carry over unswapped: A is PMTR row 2 comp 0, B is comp 1 in both forms.
  CHECK_MSG(mesh.fresnelX.row == sprite.fadeX.row && mesh.fresnelX.comp == sprite.fadeX.comp &&
                mesh.fresnelX.value == sprite.fadeX.value && mesh.fresnelY.row == sprite.fadeY.row &&
                mesh.fresnelY.comp == sprite.fadeY.comp && mesh.fresnelY.value == sprite.fadeY.value,
            "the two angle parameters are copied over, A then B");

  // 90/0 degrees: the sprite form has no normal term and its fade is 0 for every facing — the shell
  // is invisible, which is what the Power charge sphere did before this selection existed.
  for (float d = 0.f; d <= 1.f; d += 0.25f) {
    CHECK_MSG(std::fabs(FadeFactor(sprite, d)) < kTiny, "the sprite's fade is 0 at 90/0 degrees, for every facing");
  }
  // The mesh form is the facing gradient: opaque head-on, 0 at the silhouette.
  CHECK_MSG(std::fabs(FadeFactor(mesh, 1.f) - 1.f) < kTiny, "the mesh is opaque head-on");
  CHECK_MSG(std::fabs(FadeFactor(mesh, 0.f)) < kTiny, "the mesh fades out at the silhouette");
  CHECK_MSG(std::fabs(FadeFactor(mesh, 0.5f) - 0.5f) < kTiny, "and is smooth in between (d = 0.5 at half facing)");
  CHECK_MSG(FadeFactor(mesh, 0.25f) > 0.1f, "a slightly facing fragment is visible");
  // The forms agree where d = 0: the model form is the only difference, which is why a recipe whose
  // mesh permutation was not read keeps the sprite form.
  CHECK_MSG(std::fabs(FadeFactor(mesh, 0.f) - FadeFactor(sprite, 0.f)) < kTiny, "the two forms agree where d = 0");
}

// Blend (alpha/additive) and the depth policy are not part of the selection.
void TestBlendAndDepthUnchanged() {
  for (uint32_t blend = 0; blend <= 5; ++blend) {
    const DrawDesc sprite = PowerSphereDesc(kPortVfxModelOpacityFeatures, blend);
    DrawDesc mesh = sprite;
    CHECK_MSG(PortVfxSelectModelOpacity(kPortVfxModelOpacityFeatures, mesh), "the swap runs for every blend");
    CHECK_MSG(mesh.blend == sprite.blend, "the blend is untouched");
    CHECK_MSG(mesh.colorSlot == sprite.colorSlot && mesh.opacitySlot == sprite.opacitySlot &&
                  mesh.rampSlot == sprite.rampSlot && mesh.ramp2Slot == sprite.ramp2Slot &&
                  mesh.thresholdSlot == sprite.thresholdSlot && mesh.indirectSlot == sprite.indirectSlot &&
                  mesh.paletteSlot == sprite.paletteSlot,
              "the texture slots are untouched");
    CHECK_MSG(mesh.modulate == sprite.modulate && mesh.depthSoften == sprite.depthSoften &&
                  mesh.alreadyExposed == sprite.alreadyExposed && mesh.rampRow[0] == sprite.rampRow[0] &&
                  mesh.rampRow[1] == sprite.rampRow[1] && mesh.addRow == sprite.addRow,
              "modulate, depth soften, exposure and the extra rows are untouched");
    CHECK_MSG(mesh.erosion.row == sprite.erosion.row && mesh.thrX.comp == sprite.thrX.comp &&
                  mesh.indexScale.value == sprite.indexScale.value,
              "the other sources are untouched");
    CHECK_MSG(mesh.tex[0].uvSet == sprite.tex[0].uvSet && mesh.tex[0].cols == sprite.tex[0].cols,
              "the textures are untouched");
    for (int m = 0; m < 8; ++m) {
      const bool z = m & 1, aaph = m & 2, zbuf = m & 4;
      CHECK_MSG(select_model_depth(mesh.blend, z, aaph, zbuf) == select_model_depth(sprite.blend, z, aaph, zbuf),
                "the depth selection is untouched");
    }
  }
}
void TestGainBias() {
  DrawDesc sprite = PowerSphereDesc();
  CHECK_MSG(sprite.opacityGain == 1.f && sprite.opacityBias == 0.f, "legacy defaults are neutral");
  for (bool model : {false, true}) {
    for (float x : {0.f, 45.f, 90.f, 180.f}) {
      for (float y : {0.f, 45.f, 90.f, 180.f}) {
        DrawDesc identity = sprite;
        identity.fadeX.value = x;
        identity.fadeY.value = y;
        if (model) { PortVfxSelectModelOpacity(kPortVfxModelOpacityFeatures, identity); }
        for (float d : {0.f, 0.25f, 0.5f, 0.75f, 1.f}) {
          const float a = std::cos(x * 0.0174532942f);
          const float b = std::cos(y * 0.0174532942f);
          const float term = model ? d : 0.f;
          const float legacy = Smooth3(b != a ? Sat((term - a) / (b - a)) : (term >= a ? 1.f : 0.f));
          CHECK_MSG(FadeFactor(identity, d) == legacy, "identity exactly preserves legacy ascending/descending/equal angles");
        }
      }
    }
  }
  for (float gain : {0.f, 0.5f, 1.f, 2.f, -1.f}) {
    for (float bias : {-1.f, 0.f, 0.2f, 1.f, 2.f}) {
      sprite.opacityGain = gain;
      sprite.opacityBias = bias;
      DrawDesc mesh = sprite;
      PortVfxSelectModelOpacity(kPortVfxModelOpacityFeatures, mesh);
      CHECK_MSG(mesh.opacityGain == gain && mesh.opacityBias == bias, "model selection preserves gain/bias");
      for (float d : {0.f, 0.25f, 0.5f, 0.75f, 1.f}) {
        const float expected = gain * Sat(Smooth3(d) - bias);
        CHECK_MSG(std::fabs(FadeFactor(mesh, d) - expected) < kTiny, "mesh applies bias inside and gain outside clamp");
        CHECK_MSG(std::fabs(FadeFactor(sprite, d) - gain * Sat(-bias)) < kTiny, "sprite uses the same gain/bias");
      }
    }
  }
  DrawDesc mesh = PowerSphereDesc();
  PortVfxSelectModelOpacity(kPortVfxModelOpacityFeatures, mesh);
  mesh.opacityGain = 2.f;
  mesh.opacityBias = 0.2f;
  CHECK_MSG(std::fabs(FadeFactor(mesh, 1.f) - 1.6f) < kTiny, "gain is not clamped to one");
  mesh.opacityGain = 1.f;
  CHECK_MSG(std::fabs(FadeFactor(mesh, 0.5f) - 0.3f) < kTiny, "held sphere bias is subtracted after smoothing");
  // Degenerate angles retain the previous step boundary, then apply the new policy.
  mesh.fresnelX.value = mesh.fresnelY.value = 0.f;
  CHECK_MSG(FadeFactor(mesh, 0.5f) == 0.f && std::fabs(FadeFactor(mesh, 1.f) - 0.8f) < kTiny,
        "equal-angle boundary remains defined");
}
} // namespace

int main() {
  TestSelectionIsThePinnedRecipe();
  TestMeshVersusSprite();
  TestBlendAndDepthUnchanged();
  TestGainBias();
  if (port_test::Failures() == 0) {
    std::printf("port_vfx_opacity: ok\n");
  }
  return PORT_TEST_RESULT();
}
