#include "rstl/vector.hpp"

class CQuaternion;

// Same body as the generic constructor; MSVC's linker rejects this strong copy
// next to the inline instantiations (LNK2005), so cl.exe uses the generic one.
#ifndef _MSC_VER
// Declare the specialization before CInputStream defines the generic inline constructor.
template <>
rstl::vector< CQuaternion >::vector(CInputStream& in, const rmemory_allocator& allocator);
#endif

#include "Kyoto/Math/CQuaternion.hpp"
#include "Kyoto/Streams/CInputStream.hpp"

#ifndef _MSC_VER
template <>
rstl::vector< CQuaternion >::vector(CInputStream& in, const rmemory_allocator& allocator)
: x4_count(0), x8_capacity(0), xc_items(nullptr) {
  int count = in.Get(TGetType(0));
  reserve(count);
  for (int i = 0; i < count; i++) {
    push_back(in.Get< CQuaternion >());
  }
}
#endif
