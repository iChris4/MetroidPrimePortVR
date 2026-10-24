#ifndef _DOLPHIN_THP_H_
#define _DOLPHIN_THP_H_

#include <dolphin/types.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

BOOL THPInit(void);
s32 THPVideoDecode(const void* file, void* tileY, void* tileU, void* tileV, void* work /* unused */);
// Port: THPVideoDecode reading at most `size` bytes of `file` (zeros past them) and refusing a frame larger
// than the `width` x `height` picture the planes were sized for.
s32 THPVideoDecodeBounded(const void* file, size_t size, void* tileY, void* tileU, void* tileV, u32 width,
                          u32 height);
u32 THPAudioDecode(s16* audioBuffer, const u8* audioFrame, s32 flag);

#ifdef __cplusplus
}
#endif

#endif // _DOLPHIN_THP_H_
