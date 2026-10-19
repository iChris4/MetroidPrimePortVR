#ifndef _CEMITTERELEMENT
#define _CEMITTERELEMENT

#include "Kyoto/Particles/IElement.hpp"

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
#endif

#endif // _CEMITTERELEMENT
