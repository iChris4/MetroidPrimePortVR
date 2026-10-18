// #include "dolphin/os/OSArena.h"
#include "dolphin/types.h"
#include "port_env.h"
#include "stddef.h"
#include <stdio.h>
#include <dolphin/os.h>
#include <Kyoto/Alloc/CGameAllocator.hpp>

#include <Kyoto/Alloc/CCallStack.hpp>
#include <Kyoto/Alloc/CMediumAllocPool.hpp>
#include <Kyoto/Alloc/CSmallAllocPool.hpp>
#include <Kyoto/Basics/COsContext.hpp>
#include <Kyoto/Basics/CStopwatch.hpp>

#include <stdint.h>

#ifdef TARGET_PC
#include <stdlib.h>
#ifdef __linux__
#include <sys/uio.h>
#include <unistd.h>
#endif

#include "port_log.h"
#endif

/* Here just to make sure the data section matches */
static const char* string_NULL = "<NULL>";
static const char* string_SOURCE_MODULE_UNLOADED = "<SOURCE MODULE UNLOADED>";
static const char* string_ = "";
static int gAllocatorTime = 0;

template < typename U1, typename U2 >
static inline U1 T_round_up(U2 val, int align) {
  return (val + (align - 1)) & ~(align - 1);
}

CGameAllocator::SGameMemInfo* CGameAllocator::GetMemInfoFromBlockPtr(const void* ptr) const {
  return (SGameMemInfo*)((uchar*)(ptr) - sizeof(SGameMemInfo));
}

CGameAllocator::CGameAllocator()
: x4_(0)
, x8_heapSize(0)
, xc_first(nullptr)
, x10_last(nullptr)
, x54_(0)
, x58_oomCallback(nullptr)
, x5c_oomTarget(nullptr)
, x60_smallAllocPool(nullptr)
, x64_smallAllocMainData(nullptr)
, x68_smallAllocBookKeeping(nullptr)
, x6c_(false)
, x70_(0)
, x74_mediumPool(nullptr)
, x80_(0)
, x84_(0)
, x88_(0)
, x8c_(0)
, x90_heapSize2(0)
, x94_(0)
, x98_(0)
, x9c_(0)
, xa0_(0)
, xa4_(0)
, xa8_(0)
, xac_(0)
, xb0_(0)
, xb4_physicalAddr(nullptr)
, xb8_fakeStatics(0)
, xbc_(0) {}

CGameAllocator::~CGameAllocator() {
  if (x74_mediumPool) {
    x74_mediumPool->ClearPuddles();
    FreeNormalAllocation(x74_mediumPool);
    x74_mediumPool = nullptr;
  }
}

bool CGameAllocator::Initialize(COsContext& ctx) {
  if (ctx.GetBaseFreeRam() <= 4 * sizeof(SGameMemInfo)) {
    throw std::bad_alloc();
  }
  x8_heapSize = ctx.GetBaseFreeRam() - 2 * sizeof(SGameMemInfo);
  xc_first = static_cast< SGameMemInfo* >(OSAllocFromArenaLo(x8_heapSize, sizeof(SGameMemInfo)));
  if (xc_first == nullptr) {
    throw std::bad_alloc();
  }
  fprintf(stderr, "CGameAllocator: baseFreeRam=%u heapSize=0x%x first=%p arenaLo=%p arenaHi=%p\n",
          static_cast< unsigned >(ctx.GetBaseFreeRam()), static_cast< unsigned >(x8_heapSize),
          static_cast< void* >(xc_first), OSGetArenaLo(), OSGetArenaHi());
  xb4_physicalAddr =
      reinterpret_cast< void* >(reinterpret_cast< intptr_t >(xc_first) -
                                (reinterpret_cast< uintptr_t >(xc_first) & 0xf0000000));
  OSGetArenaLo();
  x10_last =
      reinterpret_cast< SGameMemInfo* >(reinterpret_cast< char* >(xc_first - 1) + x8_heapSize);

  const SGameMemInfo& head = SGameMemInfo(
      nullptr, x10_last, x10_last, x8_heapSize - sizeof(SGameMemInfo) * 2, "MemHead", "MemHead");
  *xc_first = head;
  const SGameMemInfo& tail = SGameMemInfo(xc_first, nullptr, nullptr, 0, "MemTail", "MemTail");
  *x10_last = tail;
  for (uint i = 0; i < 16; i++) {
    x14_bins[i] = nullptr;
  }

  AddFreeEntryToFreeList(xc_first);
  x80_ = 0;
  x84_ = 0;
  x88_ = 0;
  x8c_ = 0;
  x90_heapSize2 = x8_heapSize;
  x94_ = 0;
  x98_ = 0;
  x9c_ = 0;
  xa0_ = 0;
  xa4_ = 0;
  xa8_ = 0;
  x4_ = 1;

  const auto requireAllocation = [&](size_t size, const char* label) {
    void* ptr = Alloc(size, kHI_None, kSC_Unk1, kTP_Heap,
                      CCallStack(0xffffffff, label, " - Ignore"));
    if (ptr == nullptr) throw std::bad_alloc();
    return ptr;
  };
  try {
    x64_smallAllocMainData = requireAllocation(0xb0000, "SmallAllocMainData");
    x68_smallAllocBookKeeping = requireAllocation(0x16000, "SmallAllocBookKeeping");
    x60_smallAllocPool = new (requireAllocation(sizeof(CSmallAllocPool), "SmallAllocClass"))
        CSmallAllocPool(0x2c000, x64_smallAllocMainData, x68_smallAllocBookKeeping);
    x74_mediumPool = new (requireAllocation(sizeof(CMediumAllocPool), "MediumAllocClass"))
        CMediumAllocPool();
    uint mediumSize = CMediumAllocPool::GetAllocMemoryRequired(0x1000);
    mediumSize += CMediumAllocPool::GetBookKeepingMemoryRequired(0x1000);
    x78_ = requireAllocation(mediumSize, "MediumAllocMainData");
  } catch (...) {
    ReleaseAll();
    x60_smallAllocPool = nullptr;
    x4_ = 0;
    throw;
  }
  x84_ -= 4;
  xbc_ = 0xc6000;
  return true;
}

void CGameAllocator::Shutdown() {
  ReleaseAll();
  x4_ = 0;
  x54_ = 0;
}

void* CGameAllocator::Alloc(size_t size, const EHint hint, const EScope scope, const EType type,
                            const CCallStack& callstack) {
  if (size > static_cast<size_t>(UINT32_MAX) - 31) {
    return nullptr;
  }
  const OSTick startTick = OSGetTick();

  if (hint & kHI_RoundUpLen) {
    size = T_round_up< size_t, size_t >(size, 32);
  }

  bool bVar1 = size <= 56 && !(hint & (kHI_RoundUpLen | kHI_TopOfHeap)) && x60_smallAllocPool;

  if (bVar1 && x70_ > 0) {
    bVar1 = false;
    --x70_;
  }

  if (bVar1) {
    void* buf = x60_smallAllocPool->Alloc(size);
    uint tmp = x60_smallAllocPool->GetAllocatedSize();
    if (xac_ < tmp) {
      xac_ = tmp;
      static int sLastSmallAllocSize = 0;
      if (sLastSmallAllocSize + 128 < tmp) {
        sLastSmallAllocSize = tmp;
      }
    }

    if (buf != nullptr) {
      gAllocatorTime += (OSGetTick() - startTick);
      return buf;
    }
    x70_ = 25;
    x6c_ = true;
  }

  if (x74_mediumPool && size <= 0x400 && !(hint & kHI_TopOfHeap)) {
    void* buf = nullptr;
    if (!x74_mediumPool->HasPuddles()) {
      x74_mediumPool->AddPuddle(0x1000, x78_, 0);
      x78_ = nullptr;
    }

    buf = x74_mediumPool->Alloc(size);

    if (buf == nullptr) {
      buf = Alloc(CMediumAllocPool::GetAllocMemoryRequired(0x1000) +
                      CMediumAllocPool::GetBookKeepingMemoryRequired(0x1000),
                  kHI_None, kSC_Unk1, kTP_Heap,
                  CCallStack(-1, "MediumAllocMainData   ", " - Ignore"));
      if (buf == nullptr) return nullptr;
      x74_mediumPool->AddPuddle(0x1000, buf, 1);
      buf = x74_mediumPool->Alloc(size);
    }

    if (buf != nullptr) {
      gAllocatorTime += OSGetTick() - startTick;
      return buf;
    }

    if (!x7c_) {
      x74_mediumPool->GetTotalEntries();
      x74_mediumPool->GetNumAllocs();
      x74_mediumPool->GetNumBlocksAvailable();
    }
    x7c_ = true;
  }

  const bool topOfHeap = (hint & kHI_TopOfHeap) != 0;
  uint roundedSize = T_round_up< uint, size_t >(size, 32);
  SGameMemInfo* info = nullptr;

  if (topOfHeap) {
    info = FindFreeBlockFromTopOfHeap(roundedSize);
  } else {
    info = FindFreeBlock(roundedSize);
  }

  if (info == nullptr) {
    void* mediumBuf = nullptr;
    if (x58_oomCallback) {
      x58_oomCallback(x5c_oomTarget, size);

      static bool bTriedCallback = false;
      if (!bTriedCallback) {
        bTriedCallback = true;
        mediumBuf = Alloc(size, hint, scope, type, callstack);
        bTriedCallback = false;
      } else {
        return nullptr;
      }
    }
    if (mediumBuf == nullptr) {
      callstack.GetFileAndLineText();
      callstack.GetTypeText();
      fprintf(stderr, "CGameAllocator OOM: size=0x%x remaining=0x%x bins:",
              static_cast< unsigned >(size), static_cast< unsigned >(x90_heapSize2));
      for (int b = 0; b < 16; b++) {
        int n = 0;
        for (SGameMemInfo* it = x14_bins[b]; it != nullptr; it = it->GetNextFree()) {
          ++n;
        }
        fprintf(stderr, " %d", n);
      }
      fprintf(stderr, "\n");
      DumpAllocations();
      return nullptr;
    }
    return mediumBuf;
  }

  uint tmp = FixupAllocPtrs(info, size, roundedSize, hint, callstack);
  if (topOfHeap && !info->IsAllocated()) {
    info = info->GetNext();
  }

  UpdateAllocDebugStats(size, roundedSize, tmp);
  gAllocatorTime += OSGetTick() - startTick;
  return ++info;
}

CGameAllocator::SGameMemInfo* CGameAllocator::FindFreeBlock(uint len) {
  uint delta;
  CGameAllocator::SGameMemInfo* ret = nullptr;
  uint binIndex = GetFreeBinEntryForSize(len);

  uint chosenBin = 0;
  SGameMemInfo* previous = nullptr;
  uint bestDelta = 0x10000000;

  for (; binIndex < 16 && !ret; ++binIndex) {
    SGameMemInfo* candidate = x14_bins[binIndex];
    SGameMemInfo* last = nullptr;
    for (; candidate; last = candidate, candidate = candidate->GetNextFree()) {
      if (!candidate->IsAllocated() && candidate->x4_len >= len) {
        delta = candidate->x4_len - len;
        if (delta < bestDelta && candidate->GetNext()) {
          ret = candidate;
          previous = last;
          bestDelta = delta;
          chosenBin = binIndex;
          if (delta < sizeof(SGameMemInfo)) {
            break;
          }
        }
      }
    }
  }

  if (ret) {
    if (previous == NULL) {
      x14_bins[chosenBin] = ret->GetNextFree();
    } else {
      previous->SetNextFree(ret->GetNextFree());
    }
  }
  return ret;
}

CGameAllocator::SGameMemInfo* CGameAllocator::FindFreeBlockFromTopOfHeap(uint size) {
  SGameMemInfo* iter = x10_last;
  SGameMemInfo* ret = nullptr;

  while (iter != nullptr) {
    if (!iter->IsAllocated() && iter->GetLength() >= size) {
      ret = iter;
      break;
    }
    iter = iter->GetPrev();
  }

  RemoveFreeEntryFromFreeList(ret);
  return ret;
}

uint CGameAllocator::FixupAllocPtrs(SGameMemInfo* info, const uint len, uint roundedLen, EHint hint,
                                    const CCallStack& cs) {

  const bool topOfHeap = (hint & kHI_TopOfHeap) != 0;
  uint ret = 0;
  const size_t blockLength = info->x4_len;
#ifdef TARGET_PC
  // Port: requests round up to 32 bytes but the block header is 64 bytes here
  // (32 on the GameCube, where a remainder was therefore always either one
  // whole header, absorbed below, or enough for a block). A free block 32
  // bytes larger than the request would be split into a block of -32 bytes,
  // which corrupts the chain. Absorb any remainder too small for a header.
  if (blockLength != roundedLen && blockLength < roundedLen + sizeof(SGameMemInfo) + 32) {
    ret = static_cast< uint >(blockLength - roundedLen);
    roundedLen = static_cast< uint >(blockLength);
  }
#else
  if (blockLength == roundedLen + sizeof(SGameMemInfo)) {
    ret = sizeof(SGameMemInfo);
    roundedLen += sizeof(SGameMemInfo);
  }
#endif

  SGameMemInfo* newPtr = info;
#ifdef TARGET_PC
  // Port: the header is 0x40 bytes on x64 but blocks are 32-byte granular. A free block with 0x20
  // spare bytes can't hold a remainder header; splitting it would write the new 0x40-byte header
  // 0x20 bytes into the next block's header. Keep the slack in the allocation instead.
  const bool canSplit = blockLength >= roundedLen + sizeof(SGameMemInfo);
#else
  const bool canSplit = true;
#endif
  if (blockLength != roundedLen && canSplit) {
    SGameMemInfo* newInfo;

    SGameMemInfo* infoNext = info->GetNext();
    if (topOfHeap) {
      newInfo =
          reinterpret_cast< SGameMemInfo* >(reinterpret_cast< char* >(infoNext) - roundedLen) - 1;
      const SGameMemInfo& block = SGameMemInfo(info, infoNext, nullptr, len, "", "");
      *newInfo = block;
      info->x4_len -= roundedLen + sizeof(SGameMemInfo);
      AddFreeEntryToFreeList(info);
      newPtr = newInfo;
    } else {
      uint offset = roundedLen + sizeof(SGameMemInfo);
      newInfo = reinterpret_cast< SGameMemInfo* >(reinterpret_cast< char* >(info) + offset);
      const SGameMemInfo& block =
          SGameMemInfo(info, infoNext, info->GetNextFree(),
                       info->x4_len - roundedLen - sizeof(SGameMemInfo), "", "");
      *newInfo = block;
      AddFreeEntryToFreeList(newInfo);
    }
    newPtr->x8_fileAndLine = cs.GetFileAndLineText();
    newPtr->xc_type = cs.GetTypeText();
    ret = sizeof(SGameMemInfo);

    infoNext->SetPrev(newInfo);
    info->SetNext(newInfo);
  } else {
    info->x8_fileAndLine = cs.GetFileAndLineText();
    info->xc_type = cs.GetTypeText();
  }

  newPtr->SetTopOfHeapAllocated(topOfHeap);
  newPtr->SetAllocated(true);
  newPtr->x4_len = len;
  return ret;
}

void CGameAllocator::UpdateAllocDebugStats(uint len, uint roundedLen, uint offset) {
  ++x84_;
  ++x80_;
  x88_ += len;
  x8c_ += roundedLen + offset;
  x90_heapSize2 -= roundedLen + offset;

  if (x84_ > x94_) {
    x94_ = x84_;
  }

  if (x8c_ > x98_) {
    x98_ = x8c_;
  }

  if (len < x9c_) {
    x9c_ = len;
  }

  if (len > xa0_) {
    xa0_ = len;
  }
  xa4_ = (len + xa4_ * (x80_ - 1)) / x80_;
  if (len > 56) {
    return;
  }

  ++xa8_;
}

bool CGameAllocator::Free(const void* ptr) {
  if (ptr == nullptr) {
    return true;
  }

  if (x60_smallAllocPool && x60_smallAllocPool->PtrWithinPool(ptr)) {
    return x60_smallAllocPool->Free(ptr);
  }

  if (x74_mediumPool) {
    int tmp = x74_mediumPool->Free(ptr);
    if (tmp != 1) {
      return tmp > 0;
    }
  }
  return FreeNormalAllocation(ptr);
}

#ifdef TARGET_PC
bool CGameAllocator::HeapCheckEnabled() {
  static const bool sEnabled = port::EnvFlag("MP_HEAP_CHECK");
  return sEnabled;
}

// A block header, or one of its strings, may itself be garbage: copy what looks like text, reading
// through process_vm_readv where available so a wild pointer can't crash the report.
static void HeapCheckText(const char* s, char* out, size_t cap) {
  if (s == nullptr) {
    snprintf(out, cap, "<null>");
    return;
  }
  size_t n = 0;
#ifdef __linux__
  char buf[128];
  if (cap > sizeof(buf)) {
    cap = sizeof(buf);
  }
  iovec local{buf, cap - 1};
  iovec remote{const_cast< char* >(s), cap - 1};
  ssize_t got = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
  if (got > 0) {
    while (static_cast< ssize_t >(n) < got && buf[n] >= 0x20 && buf[n] < 0x7f) {
      out[n] = buf[n];
      ++n;
    }
  }
#else
  (void)n;
#endif
  out[n] = '\0';
  if (n == 0) {
    snprintf(out, cap, "<%p: not text>", static_cast< const void* >(s));
  }
}

const char* CGameAllocator::ValidateBlock(const SGameMemInfo* info, bool requireAllocated,
                                          bool deep) const {
  if (xc_first == nullptr) {
    return nullptr; // heap already released
  }
  const uintptr_t addr = reinterpret_cast< uintptr_t >(info);
  if (addr < reinterpret_cast< uintptr_t >(xc_first) ||
      addr > reinterpret_cast< uintptr_t >(x10_last)) {
    return "block header outside the heap";
  }
  if (addr & 31) {
    return "block header misaligned";
  }
  if (!info->IsPriorGuardIntact()) {
    return "prior guard (+0x0) overwritten";
  }
  if (!info->IsPostGuardIntact()) {
    return "post guard (+0x38) overwritten";
  }
  if (requireAllocated && !info->IsAllocated()) {
    return "block is not marked allocated (double free?)";
  }
  if (!deep) {
    return nullptr;
  }
  const SGameMemInfo* prev = info->GetPrev();
  const SGameMemInfo* next = info->GetNext();
  const auto inHeap = [&](const SGameMemInfo* p) {
    return reinterpret_cast< uintptr_t >(p) >= reinterpret_cast< uintptr_t >(xc_first) &&
           reinterpret_cast< uintptr_t >(p) <= reinterpret_cast< uintptr_t >(x10_last);
  };
  if (prev != nullptr) {
    if (!inHeap(prev) || prev >= info) {
      return "prev link outside the heap or not before the block";
    }
    if (prev->GetNext() != info) {
      return "prev block does not link forward to this block";
    }
  } else if (info != xc_first) {
    return "no prev link but not the first block";
  }
  if (next != nullptr) {
    if (!inHeap(next) || next <= info) {
      return "next link outside the heap or not after the block";
    }
    if (next->GetPrev() != info) {
      return "next block does not link back to this block";
    }
    if (info->IsAllocated() &&
        info->x4_len > reinterpret_cast< uintptr_t >(next) - addr - sizeof(SGameMemInfo)) {
      return "length exceeds the space up to the next block";
    }
  } else if (info != x10_last) {
    return "no next link but not the last block";
  }
  return nullptr;
}

void CGameAllocator::ReportCorruption(const char* what, const void* ptr,
                                      const SGameMemInfo* info) const {
  char fl[128];
  char ty[64];
  PortLog::Write("HEAP CHECK: %s\n", what);
  PortLog::Write("HEAP CHECK: ptr=%p header=%p heap=[%p..%p] allocations=%u\n", ptr,
                 static_cast< const void* >(info), static_cast< const void* >(xc_first),
                 static_cast< const void* >(x10_last), x84_);
  const uintptr_t lo = reinterpret_cast< uintptr_t >(xc_first);
  const uintptr_t hi = reinterpret_cast< uintptr_t >(x10_last) + sizeof(SGameMemInfo);
  const uintptr_t at = reinterpret_cast< uintptr_t >(info);
  if (at >= lo && at + sizeof(SGameMemInfo) <= hi) {
    const uchar* raw = reinterpret_cast< const uchar* >(info);
    for (size_t row = 0; row < sizeof(SGameMemInfo); row += 16) {
      char hex[64];
      for (size_t i = 0; i < 16; ++i) {
        snprintf(hex + i * 3, 4, "%02x ", raw[row + i]);
      }
      PortLog::Write("HEAP CHECK: header +0x%02zx: %s\n", row, hex);
    }
    // Walk from the head to the last block before `info` whose own header is still intact.
    const SGameMemInfo* pred = nullptr;
    const SGameMemInfo* cur = xc_first;
    while (cur != nullptr && cur < info) {
      if (!cur->IsPriorGuardIntact() || !cur->IsPostGuardIntact()) {
        PortLog::Write("HEAP CHECK: walk from the head hit a damaged header at %p first\n",
                       static_cast< const void* >(cur));
        break;
      }
      pred = cur;
      const SGameMemInfo* next = cur->GetNext();
      if (next <= cur || reinterpret_cast< uintptr_t >(next) > hi) {
        break;
      }
      cur = next;
    }
    if (cur != info && pred != nullptr && cur != nullptr && cur > info) {
      PortLog::Write("HEAP CHECK: %p is not a block boundary, it lies inside the block below\n",
                     static_cast< const void* >(info));
    }
    if (pred != nullptr) {
      HeapCheckText(pred->x8_fileAndLine, fl, sizeof(fl));
      HeapCheckText(pred->xc_type, ty, sizeof(ty));
      PortLog::Write(
          "HEAP CHECK: predecessor block %p: len=0x%zx span=0x%zx allocated=%d flags=0x%x "
          "file=\"%s\" type=\"%s\"\n",
          static_cast< const void* >(pred), pred->x4_len,
          static_cast< size_t >(at - reinterpret_cast< uintptr_t >(pred)) - sizeof(SGameMemInfo),
          pred->IsAllocated() ? 1 : 0, pred->GetPrevMaskedFlags(), fl, ty);
      if (at - reinterpret_cast< uintptr_t >(pred) >= 2 * sizeof(SGameMemInfo)) {
        const uchar* tail = raw - 32;
        char hex[16 * 3 * 2 + 1];
        for (size_t i = 0; i < 32; ++i) {
          snprintf(hex + i * 3, 4, "%02x ", tail[i]);
        }
        PortLog::Write("HEAP CHECK: last 32 bytes before the header: %s\n", hex);
      }
    }
    HeapCheckText(info->x8_fileAndLine, fl, sizeof(fl));
    HeapCheckText(info->xc_type, ty, sizeof(ty));
    PortLog::Write("HEAP CHECK: this block's strings: file=\"%s\" type=\"%s\"\n", fl, ty);
  }
  fflush(stderr);
  abort();
}

void CGameAllocator::CheckHeap() const {
  if (xc_first == nullptr) {
    return;
  }
  for (const SGameMemInfo* it = xc_first; it != nullptr; it = it->GetNext()) {
    if (const char* bad = ValidateBlock(it, false, true)) {
      ReportCorruption(bad, it + 1, it);
    }
  }
}

bool CGameAllocator::PeekLiveBlock(const void* ptr, const char** fileAndLine, size_t* len) const {
  if (xc_first == nullptr) {
    return false;
  }
  const SGameMemInfo* info = static_cast< const SGameMemInfo* >(ptr) - 1;
  if (ValidateBlock(info, true, false) != nullptr) {
    return false;
  }
  *fileAndLine = info->x8_fileAndLine;
  *len = info->x4_len;
  return true;
}
#endif

bool CGameAllocator::FreeNormalAllocation(const void* ptr) {
  SGameMemInfo* info = GetMemInfoFromBlockPtr(ptr);
#ifdef TARGET_PC
  if (const char* bad = ValidateBlock(info, true, HeapCheckEnabled())) {
    ReportCorruption(bad, ptr, info);
  }
#endif
  size_t newLen = 0;
  const size_t infoLen = info->x4_len;
  SGameMemInfo* k = info->GetNext();
  size_t len = 0;
  if (k) {
    len = (size_t)k - (size_t)info - sizeof(SGameMemInfo);
  }
  info->SetLength(len);

  SGameMemInfo* prev = info->GetPrev();
  SGameMemInfo* next = info->GetNext();

  if (prev && !prev->IsAllocated()) {
    RemoveFreeEntryFromFreeList(prev);
    prev->SetNext(next);
    if (next) {
      next->SetPrev(prev);
    }
    newLen = sizeof(SGameMemInfo);
    prev->x4_len += info->x4_len + sizeof(SGameMemInfo);
    info = prev;
  }

  if (next && !next->IsAllocated() && next->GetNext()) {
    RemoveFreeEntryFromFreeList(next);
    info->SetNext(next->GetNext());
    if (info->GetNext()) {
      info->GetNext()->SetPrev(info);
    }
    newLen += sizeof(SGameMemInfo);
    info->x4_len += next->x4_len + sizeof(SGameMemInfo);
    info->SetAllocated(false);
  } else {
    info->SetAllocated(false);
  }
  AddFreeEntryToFreeList(info);

  x84_ -= 1;
  x88_ -= infoLen;
  x8c_ -= (len + newLen);
  x90_heapSize2 += (len + newLen);
  if (infoLen <= 56) {
    xa8_ -= 1;
  }

  return true;
};

void CGameAllocator::ReleaseAll() {
  if (x74_mediumPool) {
    x74_mediumPool->ClearPuddles();
    FreeNormalAllocation(x74_mediumPool);
    x74_mediumPool = nullptr;
  }

  SGameMemInfo* iter = xc_first;
  while (iter != nullptr) {
    SGameMemInfo* next = iter->GetNext();
    if (iter->IsAllocated()) {
      FreeNormalAllocation(((uchar*)iter) + sizeof(SGameMemInfo));
    }
    iter = next;
  }

  xc_first = nullptr;
  x10_last = nullptr;
};

void* CGameAllocator::AllocSecondary(size_t size, EHint hint, EScope scope, EType type,
                                     const CCallStack& callstack) {
  return Alloc(size, hint, scope, type, callstack);
};

bool CGameAllocator::FreeSecondary(const void* ptr) { return Free(ptr); };

void CGameAllocator::ReleaseAllSecondary() {};

void CGameAllocator::SetOutOfMemoryCallback(FOutOfMemoryCb cb, const void* target) {
  x58_oomCallback = cb;
  x5c_oomTarget = target;
};

IAllocator::SAllocInfo CGameAllocator::GetAllocInfo(const void* ptr) const {
  SGameMemInfo* info = GetMemInfoFromBlockPtr(ptr);

  return SAllocInfo((const void*)info, info->GetLength(), info->IsAllocated(), false,
                    info->x8_fileAndLine, info->xc_type);
};

IAllocator::SMetrics CGameAllocator::GetMetrics() const {
  uint mediumAllocTotalAllocated =
      x74_mediumPool != nullptr ? x74_mediumPool->GetTotalEntries() * 32 : 0;
  uint mediumAllocBlocksAvailable =
      x74_mediumPool != nullptr ? x74_mediumPool->GetNumBlocksAvailable() : 0;
  uint mediumAllocAllocatedSize =
      x74_mediumPool != nullptr
          ? x74_mediumPool->GetTotalEntries() - x74_mediumPool->GetNumBlocksAvailable()
          : 0;
  uint mediumAllocNumAllocs = x74_mediumPool != nullptr ? x74_mediumPool->GetNumAllocs() : 0;
  SMetrics ret(x8_heapSize, x80_, x84_, x88_, x8c_, x90_heapSize2, x94_, x98_, x9c_, xa0_, xa4_,
               x60_smallAllocPool != nullptr ? x60_smallAllocPool->GetNumAllocs() : 0,
               x60_smallAllocPool != nullptr ? x60_smallAllocPool->GetAllocatedSize() : 0,
               x60_smallAllocPool != nullptr ? x60_smallAllocPool->GetNumBlocksAvailable() : 0,
               mediumAllocNumAllocs, mediumAllocAllocatedSize, mediumAllocBlocksAvailable,
               x80_ - xb0_, (uintptr_t)xb4_physicalAddr, xbc_, mediumAllocTotalAllocated,
               xb8_fakeStatics);
  xb0_ = x80_;
  return ret;
};

int CGameAllocator::EnumAllocations(FEnumAllocationsCb func, const void* ptr, bool b) const {

  int i = 0;
  SGameMemInfo* iter = xc_first;

  while (iter != nullptr) {
    if (!iter->IsPostGuardIntact()) {
      return -1;
    }

    if (!iter->IsPriorGuardIntact()) {
      return -1;
    }

    SGameMemInfo* next = iter->GetNext();
    SAllocInfo alloc((const void*)iter, iter->GetLength(), iter->IsAllocated(), false,
                     iter->x8_fileAndLine, iter->xc_type);
    func(alloc, ptr);
    ++i;
    iter = next;
  }

  return i;
};

uint CGameAllocator::GetFreeBinEntryForSize(uint size) {
  uint maxLen = 0x20;
  uint bin = 0;

  while (maxLen < 0x200000) {
    if (size < maxLen) {
      return bin;
    }

    maxLen <<= 1;
    ++bin;
  }

  return 0xf;
}

void CGameAllocator::AddFreeEntryToFreeList(SGameMemInfo* info) {
  uint bin = GetFreeBinEntryForSize(info->GetLength());
  info->SetNextFree(x14_bins[bin]);
  x14_bins[bin] = info;
}

void CGameAllocator::RemoveFreeEntryFromFreeList(SGameMemInfo* memInfo) {
  uint bin = GetFreeBinEntryForSize(memInfo->GetLength());
  SGameMemInfo* curBin = nullptr;
  SGameMemInfo* binIt = x14_bins[bin];

  while (binIt != nullptr) {
    if (binIt == memInfo) {
      if (curBin == nullptr) {
        x14_bins[bin] = binIt->GetNextFree();
      } else {
        curBin->SetNextFree(binIt->GetNextFree());
      }
      return;
    }

    curBin = binIt;
    binIt = binIt->GetNextFree();
  }
}

static inline bool DoWait(int v) { return (v % 4) == 0; }

void CGameAllocator::DumpAllocations() const {
  GetLargestFreeChunk();
  uint i = 0;
  SGameMemInfo* iter = xc_first;

  while (iter != nullptr) {
    ++i;

    if (DoWait(i)) {
      CStopwatch::Wait(0.005f);
    }
    iter = iter->GetNext();
  }
}

size_t CGameAllocator::GetLargestFreeChunk() const {
  SGameMemInfo* iter = xc_first;
  size_t ret = 0;
  while (iter != nullptr) {
    if (!iter->IsAllocated() && iter->GetLength() > ret) {
      ret = iter->GetLength();
    }
    iter = iter->GetNextFree();
  }

  return ret;
}
void CGameAllocator::OffsetFakeStatics(int offset) { xb8_fakeStatics += offset; }
