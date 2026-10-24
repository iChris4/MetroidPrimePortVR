#ifndef _DOLPHIN_THPVIDEODECODE
#define _DOLPHIN_THPVIDEODECODE

#include "dolphin/thp/THPRead.h"

static void* VideoDecoder(void*);
static void* VideoDecoderForOnMemory(void*);
static void VideoDecode(THPReadBuffer*);

#ifdef __cplusplus
extern "C" {
#endif // ifdef __cplusplus
s32 THPVideoDecode(void* file, void* tileY, void* tileU, void* tileV, void* work);
#ifdef TARGET_PC
// aurora's THPDec.cpp (dolphin/thp.h): decodes within `size` bytes into planes for a width x height picture.
s32 THPVideoDecodeBounded(const void* file, size_t size, void* tileY, void* tileU, void* tileV, u32 width,
                          u32 height);
#endif
BOOL CreateVideoDecodeThread(OSPriority priority, void* task);
void VideoDecodeThreadStart();
void VideoDecodeThreadCancel();
OSMessage PopFreeTextureSet();
BOOL PushFreeTextureSet(OSMessage*);
OSMessage PopDecodedTextureSet(s32 flags);
BOOL PushDecodedTextureSet(OSMessage*);

extern BOOL VideoDecodeThreadCreated;
extern OSMessageQueue FreeTextureSetQueue;
extern OSMessageQueue DecodedTextureSetQueue;
extern OSThread VideoDecodeThread;

#ifdef __cplusplus
};
#endif

#endif // _DOLPHIN_THPVIDEODECODE
