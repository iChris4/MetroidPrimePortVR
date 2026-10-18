#pragma once

class CInputStream;
class CSimplePool;
struct CPortVfxData;

// Read a VMAT blob after its length word. Unsupported/malformed lengths leave the
// material absent and consume the declared blob, retaining the retail fallback.
bool PortReadVfxMat(CPortVfxData& vfx, CInputStream& in, unsigned int nbytes, CSimplePool* pool);
