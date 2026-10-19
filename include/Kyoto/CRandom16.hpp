#ifndef _CRANDOM16
#define _CRANDOM16

#include "types.h"

class CRandom16;
class CGlobalRandom {
  static CGlobalRandom* gCurrentGlobalRandom;

public:
  CGlobalRandom(CRandom16& rnd);
  ~CGlobalRandom();

private:
  CRandom16& mRandom;
  bool mIsFirst;
  CGlobalRandom* mPrev;
};

class CRandom16 {
  friend class CGlobalRandom;
  static CRandom16* gRandomNumber;
  static void _SetRandomNumber(CRandom16* rnd);

public:
  CRandom16(uint seed = 99);
  void SetSeed(uint seed);
  int Range(int min, int max);
  float Range(float min, float max);
  int Next();
  const float Float();
#ifdef TARGET_PC
  // Remastered's CRandom16 draw (GetVector3f 0xba108): the same LCG step, but the
  // low 28 bits of the new seed times 2^-28, in [0, 1).
  float PortFloat28() {
    mSeed = mSeed * 0x41c64e6d + 0x3039;
    return static_cast< float >(mSeed & 0xfffffff) * 3.7252902984619140625e-09f;
  }
#endif

  uint GetSeed() const { return mSeed; }
  static CRandom16* GetRandomNumber();

private:
  uint mSeed;
};

#endif // _CRANDOM16
