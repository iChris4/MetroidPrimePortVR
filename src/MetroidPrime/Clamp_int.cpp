#include <Kyoto/Math/CMath.hpp>
#include <rstl/math.hpp>

// The generic inline Clamp gives the same result; MSVC's linker rejects this
// strong copy next to the inline ones (LNK2005).
#ifndef _MSC_VER
template <>
const int& CMath::Clamp< int >(const int& min, const int& val, const int& max) {
  return min > val ? min : max < val ? max : val;
}
#endif
