#ifndef _CEMITTERELEMENT
#define _CEMITTERELEMENT

#include "Kyoto/Particles/IElement.hpp"
#ifdef TARGET_PC
#include "Kyoto/Particles/PortParticleMath.hpp"
#endif

class CEESimpleEmitter : public CEmitterElement {
public:
  CEESimpleEmitter(CVectorElement* pos, CVectorElement* vel);
  ~CEESimpleEmitter();

  bool GetValue(int frame, CVector3f& pPos, CVector3f& pVel) const;

private:
  CVectorElement* x4_pos;
  CVectorElement* x8_vel;
};

class CVESphere : public CEmitterElement {
public:
  CVESphere(CVectorElement* origin, CRealElement* radius, CRealElement* velocityMag);
  ~CVESphere();

  bool GetValue(int frame, CVector3f& pPos, CVector3f& pVel) const;

private:
  CVectorElement* x4_sphereOrigin;
  CRealElement* x8_sphereRadius;
  CRealElement* xc_velocityMag;
};

class CVEAngleSphere : public CEmitterElement {
public:
  CVEAngleSphere(CVectorElement* origin, CRealElement* radius, CRealElement* velocityMag,
                 CRealElement* angleXBias, CRealElement* angleYBias, CRealElement* angleXRange,
                 CRealElement* angleYRange);
  ~CVEAngleSphere();

  bool GetValue(int frame, CVector3f& pPos, CVector3f& pVel) const;

private:
  CVectorElement* x4_sphereOrigin;
  CRealElement* x8_sphereRadius;
  CRealElement* xc_velocityMag;
  CRealElement* x10_angleXBias;
  CRealElement* x14_angleYBias;
  CRealElement* x18_angleXRange;
  CRealElement* x1c_angleYRange;
};

#ifdef TARGET_PC
// Remastered ASPR (CVEAngleSphere::GetValue 0x2cb100) with its REUL rotation (axis order 0)
// given as the three angles. The origin, position and velocity are in the port's space; the
// direction is drawn in Remastered's and turned by the basis (-x, z, y).
class CEEPortAngleSphere : public CEmitterElement {
public:
  CEEPortAngleSphere(CVectorElement* origin, CRealElement* rotateX, CRealElement* rotateY,
                     CRealElement* rotateZ, CRealElement* range1, CRealElement* range2,
                     CRealElement* radius, CRealElement* speed);
  ~CEEPortAngleSphere();

  bool GetValue(int frame, CVector3f& pPos, CVector3f& pVel) const;

private:
  CVectorElement* mOrigin;
  CRealElement* mRotate[3];
  CRealElement* mRange1;
  CRealElement* mRange2;
  CRealElement* mRadius;
  CRealElement* mSpeed;
};

// Remastered ASPV (CVEAngleSphereWithVariance::GetValue 0x2cb6b0), the REUL as three angles: ASPR's
// sphere split into polar bands by azimuth sectors, each cell jittered by a draw, the cell the creation
// count or a shuffler's pick (as PSPV). The origin, position and velocity are in the port's space.
class CEEPortAngleSphereVariance : public CEmitterElement {
public:
  CEEPortAngleSphereVariance(CVectorElement* origin, CRealElement* rotateX, CRealElement* rotateY,
                             CRealElement* rotateZ, CRealElement* range1, CRealElement* range2, CIntElement* polarCount,
                             CRealElement* polarVariance, CRealElement* azimuthRange, CIntElement* azimuthCount,
                             CRealElement* azimuthVariance, CRealElement* radius, CRealElement* speed, u8 shuffle);
  ~CEEPortAngleSphereVariance();

  bool GetValue(int frame, CVector3f& pPos, CVector3f& pVel) const;

private:
  CVectorElement* mOrigin;
  CRealElement* mRotate[3];
  CRealElement* mRange1;
  CRealElement* mRange2;
  CIntElement* mPolarCount;
  CRealElement* mPolarVariance;
  CRealElement* mAzimuthRange;
  CIntElement* mAzimuthCount;
  CRealElement* mAzimuthVariance;
  CRealElement* mRadius;
  CRealElement* mSpeed;
  u8 mShuffle; // the size of Remastered's index shuffler, 0 for none
  mutable PortParticleMath::IndexShuffler mFallbackShuffler;
};

// Remastered PLNE (0x2cc2ec): a point in a plane of half sizes (wx, wz), turned by the Euler
// matrix, with a spread of the velocity toward the plane's normal. All in Remastered's axes;
// the position and velocity are turned by the basis (-x, z, y) on the way out.
class CEEPortPlane : public CEmitterElement {
public:
  CEEPortPlane(CVectorElement* position, CVectorElement* euler, CVectorElement* velocity,
               CRealElement* wx, CRealElement* wz, CRealElement* spread);
  ~CEEPortPlane();

  bool GetValue(int frame, CVector3f& pPos, CVector3f& pVel) const;

private:
  CVectorElement* mPosition;
  CVectorElement* mEuler;
  CVectorElement* mVelocity;
  CRealElement* mWx;
  CRealElement* mWz;
  CRealElement* mSpread;
};

// Port-only PLNV (Remastered's plane with variance, GetValue 0x2cc9b8): a grid of nx by nz cells
// walked by the creation count (or shuffled), jittered by the variances. Same axes as PLNE.
class CEEPortPlaneVariance : public CEmitterElement {
public:
  CEEPortPlaneVariance(CVectorElement* position, CVectorElement* euler, CRealElement* velocityY,
                       CRealElement* wx, CRealElement* wz, CRealElement* spread,
                       CIntElement* countX, CRealElement* varianceX, CIntElement* countZ,
                       CRealElement* varianceZ, u8 shuffle);
  ~CEEPortPlaneVariance();

  bool GetValue(int frame, CVector3f& pPos, CVector3f& pVel) const;

private:
  CVectorElement* mPosition;
  CVectorElement* mEuler;
  CRealElement* mVelocityY;
  CRealElement* mWx;
  CRealElement* mWz;
  CRealElement* mSpread;
  CIntElement* mCountX;
  CRealElement* mVarianceX;
  CIntElement* mCountZ;
  CRealElement* mVarianceZ;
  u8 mShuffle; // the size of Remastered's index shuffler (SetUpShuffledIndexing 0x26ffe4), 0 for none
  // The shuffler when there is no current generator.
  mutable PortParticleMath::IndexShuffler mFallbackShuffler;
};

// Port-only SPEV (Remastered's sphere with variance, GetValue 0x2ca1c4): n1 polar bands by n2
// azimuth sectors, walked by the creation count (or shuffled), jittered by the variances.
class CEEPortSphereVariance : public CEmitterElement {
public:
  CEEPortSphereVariance(CVectorElement* position, CRealElement* radius, CRealElement* speed, CIntElement* count1,
                        CRealElement* variance1, CIntElement* count2, CRealElement* variance2, u8 shuffle);
  ~CEEPortSphereVariance();

  bool GetValue(int frame, CVector3f& pPos, CVector3f& pVel) const;

private:
  CVectorElement* mPosition;
  CRealElement* mRadius;
  CRealElement* mSpeed;
  CIntElement* mCount1;
  CRealElement* mVariance1;
  CIntElement* mCount2;
  CRealElement* mVariance2;
  u8 mShuffle; // the size of Remastered's index shuffler, 0 for none
  mutable PortParticleMath::IndexShuffler mFallbackShuffler;
};

// Port-only ELPS (Remastered's ellipsoid, GetValue 0x2ca73c) with a zero euler: three draws pick
// a direction, which the size stretches (three more draws unless surface-only).
class CEEPortEllipsoid : public CEmitterElement {
public:
  CEEPortEllipsoid(CVectorElement* centre, CVectorElement* size, CRealElement* speed, bool surfaceOnly);
  ~CEEPortEllipsoid();

  bool GetValue(int frame, CVector3f& pPos, CVector3f& pVel) const;

private:
  CVectorElement* mCentre;
  CVectorElement* mSize;
  CRealElement* mSpeed;
  bool mSurfaceOnly;
};
#endif

#endif // _CEMITTERELEMENT
