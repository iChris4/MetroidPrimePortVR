#pragma once

// The decompiled code targets the original Dolphin SDK. Aurora mirrors the SDK
// API but (a) hides OSContext/renames PADStatus members under TARGET_PC, (b)
// omits some Retro Studios convenience macros, and (c) does not aggregate the
// individual GX/SI/PAD headers the way the original umbrella headers did. This
// header is force-included ahead of every game translation unit (see
// CMakeLists.txt) to restore the SDK-facing view.

#include <math.h>
#include <stdlib.h>
#include <string.h>

// The native build uses host new/delete. CMemory allocations have a separate
// lifetime and must be returned explicitly through CMemory::Free.
#include <Kyoto/Alloc/CMemory.hpp>

// MWCC-isms used throughout the decompiled code.
#define nofralloc
#define __abs(x) ((x) < 0 ? -(x) : (x))

// Umbrella inclusion, as the original SDK headers provided transitively.
#include <dolphin/gx.h>
#include <dolphin/gx/GXShims.h>
#include <dolphin/si.h>
#include <dolphin/pad.h>
#include <dolphin/os.h>
#include <dolphin/card.h>

#ifdef __cplusplus
extern "C" {
#endif
// Port: drains Aurora's deferred ARQ completion callbacks (see AR.cpp).
void ARQPoll(void);
#ifdef __cplusplus
}
#endif

// Aurora names the PADStatus triggers triggerLeft/triggerRight under
// TARGET_PC; the game uses the SDK names triggerL/triggerR.
#ifdef TARGET_PC
#define triggerL triggerLeft
#define triggerR triggerRight
#endif



#ifndef AUTO
// MSVC has no __typeof__, and without /Zc:__cplusplus says C++98.
#if defined(__cplusplus) && (__cplusplus >= 201103L || defined(_MSC_VER))
#define AUTO(name, val) auto name = val
#define AUTO_REF(name, val) auto& name = val
#define AUTO_CONST_REF(name, val) const auto& name = val
#else
#define AUTO(name, val) __typeof__(val) name = val
#define AUTO_REF(name, val) __typeof__(val)& name = val
#define AUTO_CONST_REF(name, val) const __typeof__(val)& name = val
#endif
#endif

// The decompiled sources are written for a signed plain `char` (the PowerPC
// original and the x86 hosts the port is verified on), and several of them use
// -1 as a sentinel in `char` fields. ARM compilers default `char` to unsigned,
// where that sentinel reads back as 255 -- which silently broke streamed audio
// (see mp_signed_char in CMakeLists.txt). If the build ever loses that flag,
// fail loudly here instead of shipping quiet music.
#if defined(__cplusplus)
static_assert((char)-1 < 0, "plain char must be signed; see mp_signed_char in CMakeLists.txt");
#else
_Static_assert((char)-1 < 0, "plain char must be signed; see mp_signed_char in CMakeLists.txt");
#endif
