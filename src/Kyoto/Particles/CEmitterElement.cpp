#include "Kyoto/Particles/CEmitterElement.hpp"
#include "Kyoto/CRandom16.hpp"
#include "Kyoto/Math/CMath.hpp"
#include "Kyoto/Math/CRelAngle.hpp"
#include "Kyoto/Math/CVector3f.hpp"
#ifdef TARGET_PC
#include "Kyoto/Particles/CElementGen.hpp"
#include "Kyoto/Particles/CParticleGlobals.hpp"
#include "Kyoto/Particles/PortParticleMath.hpp"
#endif

CEESimpleEmitter::CEESimpleEmitter(CVectorElement* pos, CVectorElement* vel)
: x4_pos(pos), x8_vel(vel) {}

CEESimpleEmitter::~CEESimpleEmitter() {
  delete x4_pos;
  delete x8_vel;
}

bool CEESimpleEmitter::GetValue(int frame, CVector3f& pPos, CVector3f& pVel) const {
  x4_pos->GetValue(frame, pPos);
  if (x8_vel != NULL) {
    x8_vel->GetValue(frame, pVel);
  } else {
    pVel = CVector3f::Zero();
  }

  return false;
}

CVESphere::CVESphere(CVectorElement* origin, CRealElement* radius, CRealElement* velocityMag)
: x4_sphereOrigin(origin), x8_sphereRadius(radius), xc_velocityMag(velocityMag) {}

CVESphere::~CVESphere() {
  delete x4_sphereOrigin;
  delete x8_sphereRadius;
  delete xc_velocityMag;
}
bool CVESphere::GetValue(int frame, CVector3f& pPos, CVector3f& pVel) const {
  CVector3f origin = CVector3f::Zero();
  float radius;
  float mag;
  x4_sphereOrigin->GetValue(frame, origin);
  x8_sphereRadius->GetValue(frame, radius);

  CVector3f normVec1 = CVector3f(CRandom16::GetRandomNumber()->Range(-100, 100),
                                 CRandom16::GetRandomNumber()->Range(-100, 100),
                                 CRandom16::GetRandomNumber()->Range(-100, 100));
  normVec1 = (normVec1.CanBeNormalized() ? (0.01f * normVec1).AsNormalized() : (0.01f * normVec1));
  pPos = origin + (normVec1 * radius);

  CVector3f offset = (pPos - origin);
  CVector3f direction = offset.CanBeNormalized() ? offset.AsNormalized() : offset;
  xc_velocityMag->GetValue(frame, mag);
  pVel = mag * direction;

  return false;
}

CVEAngleSphere::CVEAngleSphere(CVectorElement* origin, CRealElement* radius,
                               CRealElement* velocityMag, CRealElement* angleXBias,
                               CRealElement* angleYBias, CRealElement* angleXRange,
                               CRealElement* angleYRange)
: x4_sphereOrigin(origin)
, x8_sphereRadius(radius)
, xc_velocityMag(velocityMag)
, x10_angleXBias(angleXBias)
, x14_angleYBias(angleYBias)
, x18_angleXRange(angleXRange)
, x1c_angleYRange(angleYRange) {}

CVEAngleSphere::~CVEAngleSphere() {
  delete x4_sphereOrigin;
  delete x8_sphereRadius;
  delete xc_velocityMag;
  delete x10_angleXBias;
  delete x14_angleYBias;
  delete x18_angleXRange;
  delete x1c_angleYRange;
}

bool CVEAngleSphere::GetValue(int frame, CVector3f& pPos, CVector3f& pVel) const {
  CVector3f origin = CVector3f::Zero();
  float radius;
  float mag;
  float xBias;
  float xRange;
  float yBias;
  float yRange;
  x4_sphereOrigin->GetValue(frame, origin);
  x8_sphereRadius->GetValue(frame, radius);

  x10_angleXBias->GetValue(frame, xBias);

  x14_angleYBias->GetValue(frame, yBias);

  x18_angleXRange->GetValue(frame, xRange);

  x1c_angleYRange->GetValue(frame, yRange);

  xBias += ((0.5f * xRange) - (CRandom16::GetRandomNumber()->Float() * xRange));
  xBias *= (M_PIF / 180.f);

  yBias += ((0.5f * yRange) - (CRandom16::GetRandomNumber()->Float() * yRange));
  yBias *= (M_PIF / 180.f);

  CVector3f vec(-CMath::FastSinR(yBias) * CMath::FastCosR(xBias), CMath::FastSinR(xBias),
                CMath::FastCosR(xBias) * CMath::FastCosR(yBias));
  pPos = origin + (radius * vec);
  CVector3f dir = (pPos - origin).AsNormalized();
  xc_velocityMag->GetValue(frame, mag);
  pVel = mag * dir;

  return false;
}

#ifdef TARGET_PC
CEEPortAngleSphere::CEEPortAngleSphere(CVectorElement* origin, CRealElement* rotateX,
                                       CRealElement* rotateY, CRealElement* rotateZ,
                                       CRealElement* range1, CRealElement* range2,
                                       CRealElement* radius, CRealElement* speed)
: mOrigin(origin)
, mRotate{rotateX, rotateY, rotateZ}
, mRange1(range1)
, mRange2(range2)
, mRadius(radius)
, mSpeed(speed) {}

CEEPortAngleSphere::~CEEPortAngleSphere() {
  delete mOrigin;
  for (CRealElement* angle : mRotate) {
    delete angle;
  }
  delete mRange1;
  delete mRange2;
  delete mRadius;
  delete mSpeed;
}

// Children in Remastered's order (origin, radius, speed, rotation, ranges), each from 0, then
// the two draws. Position is radius * d + origin and velocity d * speed for the normalised d.
bool CEEPortAngleSphere::GetValue(int frame, CVector3f& pPos, CVector3f& pVel) const {
  CVector3f origin = CVector3f::Zero();
  float radius = 0.f;
  float speed = 0.f;
  mOrigin->GetValue(frame, origin);
  mRadius->GetValue(frame, radius);
  mSpeed->GetValue(frame, speed);
  float angles[3] = {0.f, 0.f, 0.f};
  for (int i = 0; i < 3; ++i) {
    mRotate[i]->GetValue(frame, angles[i]);
  }
  const PortParticleMath::Quat q = PortParticleMath::EulerQuat(angles[0], angles[1], angles[2]);
  float range1 = 0.f;
  float range2 = 0.f;
  mRange1->GetValue(frame, range1);
  mRange2->GetValue(frame, range2);
  CRandom16* random = CRandom16::GetRandomNumber();
  const float u = random->PortFloat28();
  const float v = random->PortFloat28();
  const CVector3f d = PortParticleMath::AngleSphereDirection(q, range1, range2, u, v);
  // The basis is a sign and a swap, so it commutes with the products and sums exactly.
  const float dx = -d.GetX();
  const float dy = d.GetZ();
  const float dz = d.GetY();
  const float px = radius * dx;
  const float py = radius * dy;
  const float pz = radius * dz;
  pPos = CVector3f(px + origin.GetX(), py + origin.GetY(), pz + origin.GetZ());
  pVel = CVector3f(dx * speed, dy * speed, dz * speed);
  return false;
}
CEEPortAngleSphereVariance::CEEPortAngleSphereVariance(CVectorElement* origin, CRealElement* rotateX,
                                                       CRealElement* rotateY, CRealElement* rotateZ,
                                                       CRealElement* range1, CRealElement* range2,
                                                       CIntElement* polarCount, CRealElement* polarVariance,
                                                       CRealElement* azimuthRange, CIntElement* azimuthCount,
                                                       CRealElement* azimuthVariance, CRealElement* radius,
                                                       CRealElement* speed, u8 shuffle)
: mOrigin(origin)
, mRotate{rotateX, rotateY, rotateZ}
, mRange1(range1)
, mRange2(range2)
, mPolarCount(polarCount)
, mPolarVariance(polarVariance)
, mAzimuthRange(azimuthRange)
, mAzimuthCount(azimuthCount)
, mAzimuthVariance(azimuthVariance)
, mRadius(radius)
, mSpeed(speed)
, mShuffle(shuffle) {}

CEEPortAngleSphereVariance::~CEEPortAngleSphereVariance() {
  delete mOrigin;
  for (CRealElement* angle : mRotate) {
    delete angle;
  }
  delete mRange1;
  delete mRange2;
  delete mPolarCount;
  delete mPolarVariance;
  delete mAzimuthRange;
  delete mAzimuthCount;
  delete mAzimuthVariance;
  delete mRadius;
  delete mSpeed;
}

// ASPV GetValue 0x2cb6b0: children in order (origin, rotation, range 1, range 2, polar count, polar
// variance, azimuth range, azimuth count, azimuth variance, radius, speed; a count below 2 is 1), the
// cell index (the creation count modulo the cells, or the shuffler's pick) before the two draws,
// polar first.
bool CEEPortAngleSphereVariance::GetValue(int frame, CVector3f& pPos, CVector3f& pVel) const {
  CVector3f origin = CVector3f::Zero();
  float angles[3] = {0.f, 0.f, 0.f};
  float range1 = 0.f;
  float range2 = 0.f;
  int polarCount = 0;
  float polarVariance = 0.f;
  float azimuthRange = 0.f;
  int azimuthCount = 0;
  float azimuthVariance = 0.f;
  float radius = 0.f;
  float speed = 0.f;
  mOrigin->GetValue(frame, origin);
  for (int i = 0; i < 3; ++i) {
    mRotate[i]->GetValue(frame, angles[i]);
  }
  mRange1->GetValue(frame, range1);
  mRange2->GetValue(frame, range2);
  mPolarCount->GetValue(frame, polarCount);
  mPolarVariance->GetValue(frame, polarVariance);
  mAzimuthRange->GetValue(frame, azimuthRange);
  mAzimuthCount->GetValue(frame, azimuthCount);
  mAzimuthVariance->GetValue(frame, azimuthVariance);
  mRadius->GetValue(frame, radius);
  mSpeed->GetValue(frame, speed);
  polarCount = polarCount < 2 ? 1 : polarCount;
  azimuthCount = azimuthCount < 2 ? 1 : azimuthCount;
  CRandom16* random = CRandom16::GetRandomNumber();
  CElementGen* generator = nullptr;
  const CParticleGlobals::SParticleSystem* system = CParticleGlobals::GetCurrentParticleSystem();
  if (system != nullptr && CParticleGlobals::xPortSwooshEval == nullptr) {
    generator = system->x4_system;
  }
  int index;
  if (mShuffle != 0) {
    PortParticleMath::IndexShuffler& shuffler =
        generator != nullptr ? generator->PortShuffler(this) : mFallbackShuffler;
    index = shuffler.Next(mShuffle, [random] { return uint32_t(random->Next()); }) & 0xff;
  } else {
    // Retail counts a particle before its emitter runs; Remastered's total is one less.
    const int total = generator != nullptr ? std::max(generator->GetCumulativeParticleCount() - 1, 0) : 0;
    index = total % (polarCount * azimuthCount);
  }
  const float d1 = random->PortFloat28();
  const float d2 = random->PortFloat28();
  const PortParticleMath::Quat q = PortParticleMath::EulerQuat(angles[0], angles[1], angles[2]);
  const PortParticleMath::SpherePoint point = PortParticleMath::AngleSphereWithVariance(
      origin, q, range1, range2, polarCount, polarVariance, azimuthRange, azimuthCount, azimuthVariance, radius,
      speed, index, d1, d2);
  pPos = point.position;
  pVel = point.velocity;
  return false;
}

namespace {
// The Euler matrix and spread both Remastered's factory folds at construction.
void PortPlaneMatrix(const CVectorElement* euler, float m[9]) {
  CVector3f angles = CVector3f::Zero();
  if (euler != nullptr) {
    euler->GetValue(0, angles);
  }
  PortParticleMath::EulerMatrix(angles.GetX(), angles.GetY(), angles.GetZ(), m);
}

float PortPlaneSpreadAngle(const CRealElement* spread) {
  float degrees = 0.f;
  if (spread != nullptr) {
    spread->GetValue(0, degrees);
  }
  return degrees;
}

// B = (-x, z, y), the basis from Remastered's particle space to the port's.
CVector3f PortPlaneBasis(const CVector3f& v) { return CVector3f(-v.GetX(), v.GetZ(), v.GetY()); }
} // namespace

CEEPortPlane::CEEPortPlane(CVectorElement* position, CVectorElement* euler, CVectorElement* velocity,
                           CRealElement* wx, CRealElement* wz, CRealElement* spread)
: mPosition(position), mEuler(euler), mVelocity(velocity), mWx(wx), mWz(wz), mSpread(spread) {}

CEEPortPlane::~CEEPortPlane() {
  delete mPosition;
  delete mEuler;
  delete mVelocity;
  delete mWx;
  delete mWz;
  delete mSpread;
}

// PLNE GetValue 0x2cc2ec: the position and velocity (zero when absent), the half sizes, two
// draws (x then z) across [-w, w), the spread, then both turned by the matrix.
bool CEEPortPlane::GetValue(int frame, CVector3f& pPos, CVector3f& pVel) const {
  float m[9];
  PortPlaneMatrix(mEuler, m);
  const PortParticleMath::PlaneSpread spread =
      PortParticleMath::PlaneSpreadFast(PortPlaneSpreadAngle(mSpread));
  CVector3f position = CVector3f::Zero();
  CVector3f velocity = CVector3f::Zero();
  if (mPosition != nullptr) {
    mPosition->GetValue(frame, position);
  }
  if (mVelocity != nullptr) {
    mVelocity->GetValue(frame, velocity);
  }
  float wx = 0.f;
  float wz = 0.f;
  if (mWx != nullptr) {
    mWx->GetValue(frame, wx);
  }
  if (mWz != nullptr) {
    mWz->GetValue(frame, wz);
  }
  CRandom16* random = CRandom16::GetRandomNumber();
  const float g1 = random->PortFloat28();
  const float g2 = random->PortFloat28();
  const float x = (wx + wx) * g1 - wx;
  const float z = (wz + wz) * g2 - wz;
  const CVector3f local(x, 0.f, z);
  if (spread.enabled) {
    velocity = PortParticleMath::PlaneModifyWithSpread(local, velocity, wx, wz, spread);
  }
  const CVector3f offset = PortParticleMath::MatrixTimes(m, local);
  const CVector3f vel = PortParticleMath::MatrixTimes(m, velocity);
  pPos = PortPlaneBasis(CVector3f(position.GetX() + offset.GetX(), position.GetY() + offset.GetY(),
                                  position.GetZ() + offset.GetZ()));
  pVel = PortPlaneBasis(vel);
  return false;
}

CEEPortPlaneVariance::CEEPortPlaneVariance(CVectorElement* position, CVectorElement* euler,
                                           CRealElement* velocityY, CRealElement* wx,
                                           CRealElement* wz, CRealElement* spread,
                                           CIntElement* countX, CRealElement* varianceX,
                                           CIntElement* countZ, CRealElement* varianceZ,
                                           u8 shuffle)
: mPosition(position)
, mEuler(euler)
, mVelocityY(velocityY)
, mWx(wx)
, mWz(wz)
, mSpread(spread)
, mCountX(countX)
, mVarianceX(varianceX)
, mCountZ(countZ)
, mVarianceZ(varianceZ)
, mShuffle(shuffle) {}

CEEPortPlaneVariance::~CEEPortPlaneVariance() {
  delete mPosition;
  delete mEuler;
  delete mVelocityY;
  delete mWx;
  delete mWz;
  delete mSpread;
  delete mCountX;
  delete mVarianceX;
  delete mCountZ;
  delete mVarianceZ;
}

// PLNV GetValue 0x2cc9b8: children in order (position, velocity y, half sizes, counts and
// variances), the cell index (the creation count modulo the grid, or the shuffler's pick) taken
// before the two draws.
bool CEEPortPlaneVariance::GetValue(int frame, CVector3f& pPos, CVector3f& pVel) const {
  float m[9];
  PortPlaneMatrix(mEuler, m);
  const PortParticleMath::PlaneSpread spread =
      PortParticleMath::PlaneSpreadParabolic(PortPlaneSpreadAngle(mSpread));
  CVector3f position = CVector3f::Zero();
  float velocityY = 0.f;
  float wx = 0.f;
  float wz = 0.f;
  int countX = 0;
  int countZ = 0;
  float varianceX = 0.f;
  float varianceZ = 0.f;
  if (mPosition != nullptr) {
    mPosition->GetValue(frame, position);
  }
  if (mVelocityY != nullptr) {
    mVelocityY->GetValue(frame, velocityY);
  }
  if (mWx != nullptr) {
    mWx->GetValue(frame, wx);
  }
  if (mWz != nullptr) {
    mWz->GetValue(frame, wz);
  }
  if (mCountX != nullptr) {
    mCountX->GetValue(frame, countX);
  }
  if (mVarianceX != nullptr) {
    mVarianceX->GetValue(frame, varianceX);
  }
  if (mCountZ != nullptr) {
    mCountZ->GetValue(frame, countZ);
  }
  if (mVarianceZ != nullptr) {
    mVarianceZ->GetValue(frame, varianceZ);
  }
  countX = countX < 1 ? 1 : countX;
  countZ = countZ < 1 ? 1 : countZ;
  CRandom16* random = CRandom16::GetRandomNumber();
  CElementGen* generator = nullptr;
  const CParticleGlobals::SParticleSystem* system = CParticleGlobals::GetCurrentParticleSystem();
  if (system != nullptr && CParticleGlobals::xPortSwooshEval == nullptr) {
    generator = system->x4_system;
  }
  const int cells = countZ * countX;
  int index;
  if (mShuffle != 0) {
    PortParticleMath::IndexShuffler& shuffler =
        generator != nullptr ? generator->PortShuffler(this) : mFallbackShuffler;
    index = shuffler.Next(mShuffle, [random] { return uint32_t(random->Next()); }) & 0xff;
  } else {
    // Retail counts a particle before its emitter runs; Remastered's total is one less.
    const int total = generator != nullptr ? std::max(generator->GetCumulativeParticleCount() - 1, 0) : 0;
    index = total % cells;
  }
  const int ix = index % countX;
  const int iz = index / countX;
  const float cx = (wx + wx) / float(countX);
  const float hx = cx * 0.5f;
  const float gx = varianceX * hx;
  const float d1 = random->PortFloat28();
  const float x = ((cx * float(ix) + hx) + (((gx + gx) * d1) - gx)) - wx;
  const float cz = (wz + wz) / float(countZ);
  const float hz = cz * 0.5f;
  const float gz = varianceZ * hz;
  const float d2 = random->PortFloat28();
  const float z = ((cz * float(iz) + hz) + (((gz + gz) * d2) - gz)) - wz;
  const CVector3f local(x, 0.f, z);
  CVector3f velocity(0.f, velocityY, 0.f);
  if (spread.enabled) {
    velocity = PortParticleMath::PlaneModifyWithSpread(local, velocity, wx, wz, spread);
  }
  const CVector3f offset = PortParticleMath::MatrixTimes(m, local);
  const CVector3f vel = PortParticleMath::MatrixTimes(m, velocity);
  pPos = PortPlaneBasis(CVector3f(position.GetX() + offset.GetX(), position.GetY() + offset.GetY(),
                                  position.GetZ() + offset.GetZ()));
  pVel = PortPlaneBasis(vel);
  return false;
}

CEEPortSphereVariance::CEEPortSphereVariance(CVectorElement* position, CRealElement* radius, CRealElement* speed,
                                             CIntElement* count1, CRealElement* variance1, CIntElement* count2,
                                             CRealElement* variance2, u8 shuffle)
: mPosition(position)
, mRadius(radius)
, mSpeed(speed)
, mCount1(count1)
, mVariance1(variance1)
, mCount2(count2)
, mVariance2(variance2)
, mShuffle(shuffle) {}

CEEPortSphereVariance::~CEEPortSphereVariance() {
  delete mPosition;
  delete mRadius;
  delete mSpeed;
  delete mCount1;
  delete mVariance1;
  delete mCount2;
  delete mVariance2;
}

// SPEV GetValue 0x2ca1c4: children in order (position, radius, speed, n1, variance 1, n2,
// variance 2; a count below 2 is 1), the cell index (the creation count modulo n1 * n2, or the
// shuffler's pick) taken before the two draws, polar first.
bool CEEPortSphereVariance::GetValue(int frame, CVector3f& pPos, CVector3f& pVel) const {
  CVector3f position = CVector3f::Zero();
  float radius = 0.f;
  float speed = 0.f;
  int count1 = 0;
  int count2 = 0;
  float variance1 = 0.f;
  float variance2 = 0.f;
  if (mPosition != nullptr) {
    mPosition->GetValue(frame, position);
  }
  if (mRadius != nullptr) {
    mRadius->GetValue(frame, radius);
  }
  if (mSpeed != nullptr) {
    mSpeed->GetValue(frame, speed);
  }
  if (mCount1 != nullptr) {
    mCount1->GetValue(frame, count1);
  }
  if (mVariance1 != nullptr) {
    mVariance1->GetValue(frame, variance1);
  }
  if (mCount2 != nullptr) {
    mCount2->GetValue(frame, count2);
  }
  if (mVariance2 != nullptr) {
    mVariance2->GetValue(frame, variance2);
  }
  count1 = count1 < 2 ? 1 : count1;
  count2 = count2 < 2 ? 1 : count2;
  CRandom16* random = CRandom16::GetRandomNumber();
  CElementGen* generator = nullptr;
  const CParticleGlobals::SParticleSystem* system = CParticleGlobals::GetCurrentParticleSystem();
  if (system != nullptr && CParticleGlobals::xPortSwooshEval == nullptr) {
    generator = system->x4_system;
  }
  int index;
  if (mShuffle != 0) {
    PortParticleMath::IndexShuffler& shuffler =
        generator != nullptr ? generator->PortShuffler(this) : mFallbackShuffler;
    index = shuffler.Next(mShuffle, [random] { return uint32_t(random->Next()); }) & 0xff;
  } else {
    // Retail counts a particle before its emitter runs; Remastered's total is one less.
    const int total = generator != nullptr ? std::max(generator->GetCumulativeParticleCount() - 1, 0) : 0;
    index = total % (count1 * count2);
  }
  const float d1 = random->PortFloat28();
  const float d2 = random->PortFloat28();
  const PortParticleMath::SpherePoint point = PortParticleMath::SphereWithVariance(
      position, radius, speed, count1, variance1, count2, variance2, index, d1, d2);
  pPos = PortPlaneBasis(point.position);
  pVel = PortPlaneBasis(point.velocity);
  return false;
}

CEEPortEllipsoid::CEEPortEllipsoid(CVectorElement* centre, CVectorElement* size, CRealElement* speed,
                                   bool surfaceOnly)
: mCentre(centre), mSize(size), mSpeed(speed), mSurfaceOnly(surfaceOnly) {}

CEEPortEllipsoid::~CEEPortEllipsoid() {
  delete mCentre;
  delete mSize;
  delete mSpeed;
}

// ELPS GetValue 0x2ca73c: centre, size, speed, then three draws (x, y, z) for the direction and,
// unless surface-only, three more that scale the size.
bool CEEPortEllipsoid::GetValue(int frame, CVector3f& pPos, CVector3f& pVel) const {
  CVector3f centre = CVector3f::Zero();
  CVector3f size = CVector3f::Zero();
  float speed = 0.f;
  if (mCentre != nullptr) {
    mCentre->GetValue(frame, centre);
  }
  if (mSize != nullptr) {
    mSize->GetValue(frame, size);
  }
  if (mSpeed != nullptr) {
    mSpeed->GetValue(frame, speed);
  }
  CRandom16* random = CRandom16::GetRandomNumber();
  float draw[3];
  for (float& d : draw) {
    d = random->PortFloat28();
  }
  if (!mSurfaceOnly) {
    const float x = size.GetX() * random->PortFloat28() + 0.f;
    const float y = size.GetY() * random->PortFloat28() + 0.f;
    const float z = size.GetZ() * random->PortFloat28() + 0.f;
    size = CVector3f(x, y, z);
  }
  const PortParticleMath::SpherePoint point = PortParticleMath::EllipsoidPoint(centre, size, speed, draw);
  pPos = PortPlaneBasis(point.position);
  pVel = PortPlaneBasis(point.velocity);
  return false;
}
#endif
