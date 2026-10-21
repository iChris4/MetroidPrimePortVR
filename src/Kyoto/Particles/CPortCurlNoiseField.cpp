#ifdef TARGET_PC
// The token-holding half of CVEPortCurlNoise and the VECF factory, kept apart so the curl
// noise math links without the resource system.
#include "Kyoto/Particles/CPortCurlNoise.hpp"

#include <cstdio>
#include <memory>

#include "Kyoto/CSimplePool.hpp"
#include "Kyoto/SObjectTag.hpp"
#include "Kyoto/Streams/CInputStream.hpp"
#include "Kyoto/TToken.hpp"

struct CVEPortCurlNoise::FieldHandle {
  explicit FieldHandle(const CToken& token) : mToken(token) {}
  TCachedToken< CVectorField > mToken;
};

CVEPortCurlNoise::FieldHandle* CVEPortCurlNoise::MakeHandle(uint assetId) {
  if (gpSimplePool == nullptr) {
    return nullptr;
  }
  return rs_new FieldHandle(gpSimplePool->GetObj(SObjectTag('VECF', assetId)));
}

CVEPortCurlNoise::~CVEPortCurlNoise() {
  delete mP;
  delete mF;
  delete mA;
  delete mHandle;
}

// Resolved on the first evaluation: the token is locked and loaded then, not at parse time.
const CVectorField* CVEPortCurlNoise::ResolveField() const {
  if (mField == nullptr && mHandle != nullptr) {
    mHandle->mToken.Lock();
    mHandle->mToken.ForceCache();
    mField = mHandle->mToken.GetObject();
  }
  return mField;
}

const CFactoryFnReturn FVectorFieldFactory(const SObjectTag& tag, CInputStream& in,
                                           const CVParamTransfer&) {
  CVectorField* field = rs_new CVectorField;
  if (!field->Parse(in)) {
    // An empty field computes zero vectors, so a bad VECF stalls the effect, not the game.
    std::fprintf(stderr, "VECF %08x: bad magic, version or cell count\n", tag.GetId());
  }
  return field;
}
#endif // TARGET_PC
