#ifndef _CGUNWEAPON
#define _CGUNWEAPON

#include "types.h"

#include "MetroidPrime/CDamageInfo.hpp"
#include "MetroidPrime/CModelData.hpp"
#include "MetroidPrime/Player/CPlayerState.hpp"
#include "MetroidPrime/Weapons/GunController/CGunMotion.hpp"
#include "MetroidPrime/Weapons/WeaponCommon.hpp"
#include "MetroidPrime/Weapons/WeaponTypes.hpp"

#include "Kyoto/Math/CAABox.hpp"
#include "Kyoto/Math/CTransform4f.hpp"
#include "Kyoto/Math/CVector3f.hpp"
#include "Kyoto/TToken.hpp"

#include "Collision/CMaterialList.hpp"

#include "rstl/optional_object.hpp"
#include "rstl/reserved_vector.hpp"
#include "rstl/single_ptr.hpp"

class CGenDescription;
class CGunController;
class CAnimCharacterSet;
class CWeaponDescription;
class CElementGen;
class CRainSplashGenerator;
class CModelFlags;
class CActorLights;
class SWeaponInfo;

#ifdef TARGET_PC
namespace NPortChargeFxDetail {
// CGunWeapon's muzzle-effect slots: 0 is the normal muzzle flash and 1 the charge effect
// (`ActivateCharge` switches between them; see `GetChargeMuzzleFx`).
inline constexpr unsigned kMuzzleEffectChargeSlot = 1;

// Preserve the existing Rx(+90) policy for converted non-Power charge effects. Their effective
// native attachment frames have not been established by the Power-only source audit below.
// Compose on the sampled locator's own side, keeping its animated translation unchanged.
inline CTransform4f ConvertedChargeMuzzleFrame(const CTransform4f& locator) {
  const CVector3f translation = locator.GetTranslation();
  return CTransform4f(locator.Get00(), locator.Get02(), -locator.Get01(), translation.GetX(),
                      locator.Get10(), locator.Get12(), -locator.Get11(), translation.GetY(),
                      locator.Get20(), locator.Get22(), -locator.Get21(), translation.GetZ());
}

// Power's effective native LBEAM includes the legacy wrapper's right-multiplied rotation
// correction; raw/bind bone axes alone incorrectly suggested authored EMTR +Z pointed back.
// The corrected native frame is X left, Y up, Z forward; retail is X right, Y forward, Z up.
// R_sampled * B, B = [-1 0 0; 0 0 1; 0 1 0], therefore has columns (-col0,col2,col1).
// B is a proper rotation (det +1): EMTR (0,0,.15) at scale 2 goes .30 forward, not backward.
// This is only a Power charge-slot policy, not native animation retargeting or pose parity.
// Source contract: build/s-powercharge-native-20261017/{legacy-locator,emitter-sign}/REPORT.md.
inline CTransform4f ConvertedPowerChargeMuzzleFrame(const CTransform4f& locator) {
  const CVector3f translation = locator.GetTranslation();
  return CTransform4f(-locator.Get00(), locator.Get02(), locator.Get01(), translation.GetX(),
                      -locator.Get10(), locator.Get12(), locator.Get11(), translation.GetY(),
                      -locator.Get20(), locator.Get22(), locator.Get21(), translation.GetZ());
}

// Retail keeps translation only; converted normal flashes keep the full sampled locator.
template < typename Generator >
void SetLocalLocator(Generator& generator, const CTransform4f& locator, const bool chargeEffect,
                     const bool isPowerBeam) {
  if (!generator.PortIsRemastered()) {
    generator.SetGlobalTranslation(locator.GetTranslation());
  } else if (chargeEffect) {
    generator.SetGlobalOrientAndTrans(isPowerBeam ? ConvertedPowerChargeMuzzleFrame(locator)
                                                 : ConvertedChargeMuzzleFrame(locator));
  } else {
    // The sampled locator itself, no copy.
    generator.SetGlobalOrientAndTrans(locator);
  }
}
} // namespace NPortChargeFxDetail
#endif

enum EFrozenFxType {
  kFFT_None,
  kFFT_Frozen,
  kFFT_Thawed,
};

class CVelocityInfo {
public:
  ~CVelocityInfo();

  CVector3f& Velocity(int i) { return x0_vel[i]; }
  const CVector3f& GetVelocity(int i) const { return x0_vel[i]; }
  bool GetTargetHoming(int i) const { return x1c_targetHoming[i]; }

  void Clear();

  void AddVelocity(const CVector3f& vel) { x0_vel.push_back(vel); }
#if VERSION >= VERSION_GM8P_00 && VERSION != VERSION_GM8E_02
  void AddTargetHoming(const bool homing) { x1c_targetHoming.push_back(homing); }
#else
  void AddTargetHoming(const bool& homing) { x1c_targetHoming.push_back(homing); }
#endif
  void AddTrat(const float& trat) { x24_trat.push_back(trat); }

private:
  rstl::reserved_vector< CVector3f, 2 > x0_vel;
  rstl::reserved_vector< bool, 2 > x1c_targetHoming;
  rstl::reserved_vector< float, 2 > x24_trat;
};

class CGunWeapon {
  friend class CPlayerGun;

public:
  CGunWeapon(CAssetId ancsId, EWeaponType type, TUniqueId playerId, EMaterialTypes playerMaterial,
             const CVector3f& scale);
  virtual ~CGunWeapon();

  enum ESecondaryFxType {
    kSFT_None,
    kSFT_Charge,
    kSFT_ToCombo,
    kSFT_CancelCharge,
  };

  // Virtual Methods
  virtual void Reset(CStateManager& mgr);
  virtual void PlayAnim(const NWeaponTypes::EGunAnimType type, bool loop);
  virtual void PreRenderGunFx(const CStateManager& mgr, const CTransform4f& xf) {}
  virtual void PostRenderGunFx(const CStateManager& mgr, const CTransform4f& xf);
  virtual void UpdateGunFx(const bool shotSmoke, const float dt, const CStateManager& mgr,
                           const CTransform4f& xf);
  virtual void Fire(const bool underwater, const float dt,
                    const CPlayerState::EChargeStage chargeState, const CTransform4f& xf,
                    CStateManager& mgr, const TUniqueId homingTarget, const float chargeFactor1,
                    const float chargeFactor2);
  virtual void EnableFx(const bool enable) {}
  virtual void EnableSecondaryFx(const ESecondaryFxType type) {
    x1cc_enabledSecondaryEffect = type;
  }
  virtual void Draw(const bool drawSuitArm, const CStateManager& mgr, const CTransform4f& xf,
                    const CModelFlags& flags, const CActorLights* lights) const;
  virtual void DrawMuzzleFx(const CStateManager& mgr) const;
  virtual void Update(const float dt, CStateManager& mgr);
  virtual void Load(CStateManager& mgr, const bool subtypeBasePose);
  virtual void Unload(CStateManager& mgr);
  virtual bool IsLoaded() const;

  const CVelocityInfo& GetVelocityInfo() const { return x1d0_velInfo; }
  rstl::optional_object< CModelData >& SolidModelData() { return x10_solidModelData; }
  const CModelData& GetSolidModelData() const { return x10_solidModelData.data(); }

  EWeaponType GetType() const { return x1c0_weaponType; }
  TUniqueId GetPlayerId() const { return x1c4_playerId; }
  EMaterialTypes GetPlayerMaterial() const { return x1c8_playerMaterial; }

  CAABox GetBounds() const;
  CAABox GetBounds(const CTransform4f& xf) const;
  const SWeaponInfo& GetWeaponInfo() const;
  void ActivateCharge(bool enable, bool resetEffect);
  bool IsCharged() const { return x218_25_enableCharge; }
  void EnableCharge(bool enable) { x218_25_enableCharge = enable; }
  bool PlayPasAnim(SamusGun::EAnimationState state, CStateManager& mgr, float angle);
  bool IsChargeAnimOver() const;
  void UpdateMuzzleFx(const float dt, const CVector3f& scale, const CVector3f& pos,
                      const bool emitting);
#ifdef TARGET_PC
  void UpdateMuzzleFx(const float dt, const CVector3f& scale, const CTransform4f& locator,
                      const bool emitting);
#endif
  CElementGen* GetChargeMuzzleFx() const;
  void DrawHologram(const CStateManager& mgr, const CTransform4f& xf,
                    const CModelFlags& flags) const;
  void ReturnToDefault(CStateManager& mgr);
  bool ComboFireOver() const;
  void EnterFidget(CStateManager& mgr, SamusGun::EFidgetType type, int parm2);
  void Touch(const CStateManager& mgr);
  void TouchHolo(const CStateManager& mgr);
  void AsyncLoadSuitArm(CStateManager& mgr);
  void AsyncLoadFidget(CStateManager& mgr, SamusGun::EFidgetType type, int animSet);
  void UnLoadFidget();
  bool IsFidgetLoaded();
  void EnableFrozenEffect(EFrozenFxType type);

  CDamageInfo GetDamageInfo(CStateManager& mgr, CPlayerState::EChargeStage chargeState,
                            float chargeFactor);
  static const char* GetMuzzleLocator() { return skMuzzleLocator; }

protected:
  // x0 is vtable
  CVector3f x4_scale;
  rstl::optional_object< CModelData > x10_solidModelData;
  rstl::optional_object< CModelData > x60_holoModelData;
  rstl::optional_object< CModelData > xb0_suitArmModelData;
  rstl::single_ptr< CGunController > x100_gunController;
  TToken< CAnimCharacterSet > x104_gunCharacter;
  rstl::vector< CToken > x10c_anims;
  rstl::vector< CToken > x11c_unk;
  rstl::vector< CToken > x12c_deps;
  TToken< CAnimCharacterSet > x13c_armCharacter;
  rstl::reserved_vector< TCachedToken< CWeaponDescription >, 2 > x144_weapons;
  TCachedToken< CGenDescription > x160_xferEffect;
  rstl::reserved_vector< TCachedToken< CGenDescription >, 2 > x16c_muzzleEffects;
  rstl::reserved_vector< TCachedToken< CGenDescription >, 2 > x188_frozenEffects;
  rstl::reserved_vector< rstl::auto_ptr< CElementGen >, 2 > x1a4_muzzleGenerators;
  rstl::single_ptr< CElementGen > x1b8_frozenGenerator;
  CRainSplashGenerator* x1bc_rainSplashGenerator;
  EWeaponType x1c0_weaponType;
  TUniqueId x1c4_playerId;
  EMaterialTypes x1c8_playerMaterial;
  ESecondaryFxType x1cc_enabledSecondaryEffect;
  CVelocityInfo x1d0_velInfo;
  CPlayerState::EBeamId x200_beamId;
  EFrozenFxType x204_frozenEffect;
  uint x208_muzzleEffectIdx;
  uint x20c_shaderIdx;
  // 0x1: load request, 0x2: muzzle fx, 0x4: projectile data, 0x8: anims, 0x10: everything else
  int x210_loadFlags;
  CAssetId x214_ancsId;
  bool x218_24 : 1;
  bool x218_25_enableCharge : 1;
  bool x218_26_loaded : 1;
  // Initialize in selected beam's pose, rather than power beam's pose
  bool x218_27_subtypeBasePose : 1;
  bool x218_28_suitArmLocked : 1;
  bool x218_29_drawHologram : 1;

  static const char* const skMuzzleLocator;
  static const char* const skElbowLocator;
  static const int skShootAnim[2];
  static const char* const skMuzzleNames[10];
  static const char* const skFrozenNames[10];
  static const char* const skBeamXferNames[5];
  static const char* const skAnimDependencyNames[5];
  static const char* const skDependencyNames[5];
  static const char* const skSuitArmNames[8];
  static const int skAnimTypeList[11];

  void AllocResPools(CPlayerState::EBeamId beam);
  void FreeResPools();
  static void FillTokenVector(const rstl::vector< SObjectTag >& tags,
                              rstl::vector< CToken >& objects, bool includeTxtr);
  void BuildDependencyList(CPlayerState::EBeamId beam);
  void LoadSuitArm(CStateManager& mgr);
  void LoadGunModels(CStateManager& mgr);
  void LoadAnimations();
  bool IsAnimsLoaded() const;
  void LoadMuzzleFx(float dt);
  void LoadProjectileData(CStateManager& mgr);
  void LoadFxIdle(float dt, CStateManager& mgr);
  void LockTokens(CStateManager& mgr);
  void UnlockTokens();

  static void PointGenerator(void*, const CVector3f*, const CVector3f*, int);
};
CHECK_SIZEOF(CGunWeapon, 0x21c)

#endif // _CGUNWEAPON
