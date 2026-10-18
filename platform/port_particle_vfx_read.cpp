// CParticleDataFactory's reader of the port-only effect properties
// (PortReadVfxProperty: VMAT, VMSH, VTMT, VPMT, VSMT, SSZE, ITEN, VORN) and its
// blob readers, moved out of src/Kyoto/Particles/CParticleDataFactory.cpp so the
// decomp file keeps only its hooks (see docs/NATIVE_PORT.md, "Where port code
// goes"). The count bounds both files use are in port_vfx_read.h.
//
// Built as part of mp_game, with the same flags and includes as the file it
// came from; the code was moved verbatim.

#include "Kyoto/Particles/CParticleDataFactory.hpp"

#include <algorithm>

#ifdef TARGET_PC
#include "aurora/vfx.hpp"
#include "Kyoto/Particles/CPortVfxMat.hpp"
#include "port_vfx_read.h"
#endif
#include "Kyoto/CFactoryFnReturn.hpp"
#include "Kyoto/CRandom16.hpp"
#include "Kyoto/CSimplePool.hpp"
#include "Kyoto/CVParamTransfer.hpp"
#include "Kyoto/Graphics/CTexture.hpp"
#include "Kyoto/Particles/CColorElement.hpp"
#include "Kyoto/Particles/CEmitterElement.hpp"
#include "Kyoto/Particles/CGenDescription.hpp"
#include "Kyoto/Particles/CIntElement.hpp"
#include "Kyoto/Particles/CModVectorElement.hpp"
#include "Kyoto/Particles/CParticleGen.hpp"
#include "Kyoto/Particles/CPortParticleVars.hpp"
#include "Kyoto/Particles/CRealElement.hpp"
#include "Kyoto/Particles/CSpawnSystemKeyframeData.hpp"
#include "Kyoto/Particles/CSwooshDescription.hpp"
#include "Kyoto/Particles/CUVElement.hpp"
#include "Kyoto/Particles/CVectorElement.hpp"
#include "Kyoto/Particles/IElement.hpp"
#include "Kyoto/SObjectTag.hpp"
#include "Kyoto/Streams/CInputStream.hpp"
#include "Kyoto/TToken.hpp"
#include "dolphin/types.h"
#include "rstl/algorithm.hpp"
#include "rstl/list.hpp"
#include "rstl/optional_object.hpp"
#define SBIG(v) v

CTexture* CreateTexture(int value);

#ifdef TARGET_PC
// The VMSH blob v1 (build/mpr/vfx/DESIGN.md section 1). Anything unreadable leaves the PART
// without a mesh, so its model particles draw as retail.
static bool PortReadVfxMesh(CPortVfxData& vfx, CInputStream& in, u32 nbytes) {
  u32 consumed = 0;
  auto skip = [&]() {
    for (; consumed + 4 <= nbytes; consumed += 4) {
      in.ReadLong();
    }
  };
  vfx.meshVerts = vfx.meshTris = 0;
  vfx.meshV.clear();
  vfx.meshIdx16.clear();
  vfx.meshIdx32.clear();
  if (nbytes < 12) {
    skip();
    return true;
  }
  const u32 version = in.ReadLong();
  const u32 nv = in.ReadLong();
  const u32 nt = in.ReadLong();
  consumed = 12;
  const bool wide = nv > 65535;
  const u64 expect = 12 + u64(nv) * 32 + u64(nt) * 3 * (wide ? 4 : 2);
  if (version != 1 || nv == 0 || nt == 0 || expect != nbytes) {
    skip();
    return true;
  }
  vfx.meshV.resize(size_t(nv) * 8);
  for (float& f : vfx.meshV) {
    f = in.ReadFloat();
  }
  consumed += nv * 32;
  if (wide) {
    vfx.meshIdx32.resize(size_t(nt) * 3);
    for (u32& i : vfx.meshIdx32) {
      i = in.ReadLong();
    }
  } else {
    vfx.meshIdx16.resize(size_t(nt) * 3);
    for (u16& i : vfx.meshIdx16) {
      i = in.ReadShort();
    }
  }
  // The batch indexes its per-particle vertex copy with these, so they must stay inside it.
  const auto outOfRange = [nv](auto i) { return u32(i) >= nv; };
  if (std::any_of(vfx.meshIdx32.begin(), vfx.meshIdx32.end(), outOfRange) ||
      std::any_of(vfx.meshIdx16.begin(), vfx.meshIdx16.end(), outOfRange)) {
    vfx.meshV.clear();
    vfx.meshIdx16.clear();
    vfx.meshIdx32.clear();
    return true;
  }
  vfx.meshVerts = nv;
  vfx.meshTris = nt;
  return true;
}

static CPortVfxData& PortVfxData(std::unique_ptr< CPortVfxData >& vfx) {
  if (!vfx) {
    vfx.reset(new CPortVfxData);
  }
  return *vfx;
}

// The port-only material properties a converted PART or SWSH carries (VMAT, VMSH, VTMT, VPMT,
// VSMT, SSZE, ITEN, VORN). False when the stream can't be resynchronised.
bool CParticleDataFactory::PortReadVfxProperty(FourCC clsId, std::unique_ptr< CPortVfxData >& vfxPtr,
                                               CInputStream& in, CSimplePool* pool) {
  switch (clsId) {
  case SBIG('VMAT'): {
    GetClassID(in);
    const u32 nbytes = in.ReadLong();
    if (!PortReadVfxMat(PortVfxData(vfxPtr), in, nbytes, pool)) {
      return false;
    }
  } break;
  case SBIG('VMSH'): {
    GetClassID(in);
    const u32 nbytes = in.ReadLong();
    if (!PortReadVfxMesh(PortVfxData(vfxPtr), in, nbytes)) {
      return false;
    }
  } break;
  case SBIG('VTMT'): {
    CPortVfxData& vfx = PortVfxData(vfxPtr);
    GetClassID(in);
    for (CRealElement* e : vfx.vtmt) {
      delete e;
    }
    vfx.vtmt.clear();
    vfx.vtmtCount = in.ReadLong();
    if (vfx.vtmtCount > kPortVfxMaxVtmt) {
      // The stream can't be resynchronised; drop the port material so the PART draws as retail.
      vfxPtr.reset();
      return false;
    }
    for (u32 i = 0; i < vfx.vtmtCount * 6; ++i) {
      vfx.vtmt.push_back(GetRealElement(in));
    }
  } break;
  case SBIG('VPMT'):
  case SBIG('VSMT'): {
    // VPMT: {kind row comp element}; VSMT: {target kind element}
    const bool isVpmt = clsId == SBIG('VPMT');
    std::vector< CPortVfxElem >& list = isVpmt ? PortVfxData(vfxPtr).vpmt : PortVfxData(vfxPtr).vsmt;
    GetClassID(in);
    const u32 count = in.ReadLong();
    if (count > kPortVfxMaxElems) {
      vfxPtr.reset();
      return false;
    }
    for (u32 i = 0; i < count; ++i) {
      CPortVfxElem elem;
      if (isVpmt) {
        elem.kind = in.ReadLong();
        elem.a = in.ReadLong();
        elem.b = in.ReadLong();
      } else {
        elem.a = in.ReadLong();
        elem.kind = in.ReadLong();
      }
      switch (elem.kind) {
      case CPortVfxElem::kReal:
        elem.real = GetRealElement(in);
        break;
      case CPortVfxElem::kVector:
        elem.vec = GetVectorElement(in);
        break;
      case CPortVfxElem::kInt:
        elem.integer = GetIntElement(in);
        break;
      case CPortVfxElem::kColor:
        elem.color = GetColorElement(in);
        break;
      default:
        vfxPtr.reset(); // frees the elements read so far
        return false;
      }
      list.push_back(elem);
    }
  } break;
  case SBIG('SSZE'): {
    CPortVfxData& vfx = PortVfxData(vfxPtr);
    delete vfx.ssze;
    vfx.ssze = GetRealElement(in);
  } break;
  case SBIG('ITEN'): {
    CPortVfxData& vfx = PortVfxData(vfxPtr);
    delete vfx.iten;
    vfx.iten = GetRealElement(in);
  } break;
  case SBIG('VORN'):
    GetClassID(in);
    PortVfxData(vfxPtr).vorn = in.ReadLong();
    break;
  default:
    break;
  }
  return true;
}
#endif
