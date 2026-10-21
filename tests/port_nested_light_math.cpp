#include "Kyoto/Particles/CElementGen.hpp"
#include "MetroidPrime/CExplosion.hpp"
#include "port_test.h"

#include <cmath>
#include <cstdio>

namespace {

bool Close(float a, float b) { return std::fabs(a - b) < 1e-5f; }

void TestSourceSpawnOrigin() {
  // A parent at (10,20,30), scaled by (2,3,4), with inherited T_l=(1,2,3).
  // The production origin calculation has no EMTR or PMOP inputs.
  const CVector3f globalTranslation(10.f, 20.f, 30.f);
  const CTransform4f parentScale(2.f, 0.f, 0.f, 0.f, 0.f, 3.f, 0.f, 0.f,
                                 0.f, 0.f, 4.f, 0.f);
  const CVector3f parentT(1.f, 2.f, 3.f);
  const CVector3f sourceSpawnT = parentT;
  const CVector3f parentOrigin =
      CElementGen::PortWorldFromLocal(globalTranslation, parentScale, parentT);
  const CVector3f childOrigin =
      CElementGen::PortWorldFromLocal(globalTranslation, parentScale, sourceSpawnT);
  CHECK_MSG(Close(parentOrigin.GetX(), 12.f) && Close(parentOrigin.GetY(), 26.f) &&
                Close(parentOrigin.GetZ(), 42.f),
            "parent's numeric system origin includes scale and translation");
  CHECK_MSG(Close(childOrigin.GetX(), parentOrigin.GetX()) &&
                Close(childOrigin.GetY(), parentOrigin.GetY()) &&
                Close(childOrigin.GetZ(), parentOrigin.GetZ()),
            "default inherited mode-2 source T_l resolves to the parent origin without EMTR/PMOP offsets");
}

void TestNestedLightSourceIdentity() {
  constexpr CAssetId sourceId = 0x12345678u;
  CHECK_MSG(CExplosion::PortNestedLightSourceId(sourceId) == sourceId,
            "nested proxies keep the explosion's asset source ID for ordinary light grouping");
}

void TestNestedLightOwnerLifecycleKey() {
  alignas(CParticleGen) char liveOwnerStorage[sizeof(CParticleGen)];
  alignas(CParticleGen) char retiredOwnerStorage[sizeof(CParticleGen)];
  const CParticleGen* liveOwner = reinterpret_cast<const CParticleGen*>(liveOwnerStorage);
  const CParticleGen* retiredOwner = reinterpret_cast<const CParticleGen*>(retiredOwnerStorage);
  const CVector3f zero = CVector3f::Zero();
  const CLight light = CLight::BuildPoint(zero, CColor::White());
  const std::vector< CElementGen::CPortNestedLightSnapshot > snapshots = {
      {liveOwner, zero, light},
  };
  CHECK_MSG(CElementGen::PortHasNestedLightSnapshotFor(snapshots, liveOwner),
            "the same child-generator identity keeps its existing light proxy live");
  CHECK_MSG(!CElementGen::PortHasNestedLightSnapshotFor(snapshots, retiredOwner),
            "a child generator absent from the current ancestry is eligible for proxy retirement");
}

void TestBoundedHdrCarrier() {
  const CVector3f zero = CVector3f::Zero();
  CLight nested = CLight::BuildCustom(zero, CVector3f(1.f, 0.f, 0.f), CColor::White(),
                                     0.f, 0.f, 0.f, 1.f, 0.f, 0.f);
  CHECK_MSG(nested.GetRadius() > 1.0e30f, "legacy custom-light radius remains unbounded before PHDR opt-in");

  const float sourceRgba[4] = {1.2f, 0.4f, 0.1f, 1.25f};
  CElementGen::PortSetNestedHdrCarrier(nested, sourceRgba, 20.f * 0.5f, 0.25f, 3.f);
  const float* hdr = nested.GetPortHdrColor();
  CHECK_MSG(Close(hdr[0], 12.f) && Close(hdr[1], 4.f) && Close(hdr[2], 1.f),
            "PBR carrier multiplies evaluated LCLR by keyed LINT without clamping");
  CHECK_MSG(nested.GetPortHdrR0() == 0.25f && nested.GetPortHdrR1() == 3.f &&
                nested.GetPortHdrFalloff() == CLight::kPHF_Smooth,
            "carrier uses the source .25/3 smooth distance sampler");
  CHECK_MSG(nested.GetRadius() == 3.f && nested.GetPosition().GetX() == 0.f &&
                nested.GetPosition().GetY() == 0.f && nested.GetPosition().GetZ() == 0.f,
            "PHDR bounds CActorLights at the outer radius and leaves actor translation to CGameLight");

  CElementGen::PortSetNestedHdrCarrier(nested, sourceRgba, 20.f, 0.25f, 3.f);
  hdr = nested.GetPortHdrColor();
  CHECK_MSG(Close(hdr[0], 24.f) && Close(hdr[1], 8.f) && Close(hdr[2], 2.f),
            "a later KEYE intensity evaluation updates the carrier");

  CLight ordinaryHdr = CLight::BuildCustom(zero, CVector3f(1.f, 0.f, 0.f), CColor::White(),
                                            0.f, 0.f, 0.f, 1.f, 0.f, 0.f);
  const float rgb[3] = {1.f, 1.f, 1.f};
  CElementGen::PortSetNestedHdrCarrier(ordinaryHdr, sourceRgba, 20.f, 0.25f, 3.f);
  CHECK_MSG(ordinaryHdr.GetAngleAttenuationConstant() == 20.f &&
                ordinaryHdr.GetAttenuationConstant() == 1.f && ordinaryHdr.GetAttenuationLinear() == 0.f &&
                Close(ordinaryHdr.GetAttenuationQuadratic(), 1.f / (1.5f * 1.5f)),
            "the retail (TEV) light takes the carrier's intensity, at half strength at half the outer radius");
  ordinaryHdr.SetAttenuation(0.f, 0.f, 0.f);
  ordinaryHdr.SetPortHdr(rgb, zero, 0.25f, 3.f, CLight::kPHF_Smooth);
  CHECK_MSG(ordinaryHdr.GetRadius() > 1.0e30f,
            "a later ordinary HDR assignment clears the nested-light radius override");

  // PHFO: each authored LFOT 0..3 selects its CLight falloff shape.
  const CLight::EPortHdrFalloff shapes[4] = {CLight::kPHF_None, CLight::kPHF_Linear,
                                              CLight::kPHF_Quadratic, CLight::kPHF_Smooth};
  for (u8 lfot = 0; lfot < 4; ++lfot) {
    CLight shaped = CLight::BuildCustom(zero, CVector3f(1.f, 0.f, 0.f), CColor::White(), 0.f, 0.f,
                                        0.f, 1.f, 0.f, 0.f);
    CElementGen::PortSetNestedHdrCarrier(shaped, sourceRgba, 1.f, 0.25f, 3.f, lfot);
    CHECK_MSG(shaped.GetPortHdrFalloff() == shapes[lfot], "PHFO selects the authored falloff");
  }
}
}  // namespace

int main() {
  TestSourceSpawnOrigin();
  TestNestedLightSourceIdentity();
  TestNestedLightOwnerLifecycleKey();
  TestBoundedHdrCarrier();
  if (port_test::Failures() == 0) {
    std::puts("port_nested_light_math: ok");
  }
  return PORT_TEST_RESULT();
}
