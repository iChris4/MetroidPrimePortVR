#include "GameVersions.h"

#include "Kyoto/CARAMToken.hpp"
#include "Kyoto/CDvdFile.hpp"
#include "Kyoto/CDvdRequest.hpp"

#include "Kyoto/CARAMManager.hpp"

#include "dolphin/os/OSCache.h"
#include "rstl/math.hpp"

#include "dolphin/arq.h"
#include "dolphin/dvd.h"
#include "dolphin/os.h"

#include "string.h"

#ifdef TARGET_PC
#include "port_log.h"
#endif

#include <mutex>
#include <stdexcept>
#include <string>

static CDvdFile* sFirstARAM = nullptr;

namespace {
// The ARAM file state is touched from both the DVD worker thread
// (`HandleDVDInterrupt`) and the main thread (`HandleARAMInterrupt` via the ARQ
// callback). The guest guarded this with `OSDisableInterrupts`, which is a no-op
// on PC, so serialize it here. Recursive because `DVDClose` can synchronously
// deliver nested DVD callbacks that re-enter these paths.
std::recursive_mutex& AramFileStateMutex() {
  static std::recursive_mutex sMutex;
  return sMutex;
}
} // namespace

struct CDvdFileARAM {
  CDvdFileARAM()
  : mGotARAMInterrupt(false)
  , mGotDvdInterrupt(false)
  , mFileSize1(0)
  , mFileSize2(0)
  , mCurBufferLen(0)
  , mAramOffset(0)
  , mBufferLen(0)
  , mBufferIndex(0) {}
  ARQRequest mARQRequest;
  struct SDvdInfo {
    SDvdInfo() : mDvdFileInfo(), mDvdFile(nullptr), mNextfile(nullptr) {}
    DVDFileInfo mDvdFileInfo;
    CDvdFile* mDvdFile;
    CDvdFile* mNextfile;
  } mInfo;
  rstl::reserved_vector< rstl::auto_ptr< uchar[] >, 2 > mBuffers;
  bool mGotARAMInterrupt;
  bool mGotDvdInterrupt;
  uint mFileSize1;
  uint mFileSize2;
  int mCurBufferLen;
  uint mAramOffset;
  int mBufferLen;
  uint mBufferIndex;
};

const char* DecodeARAMFile(const char* filename) {
  if (!strncmp(filename, "aram:", 5)) {
    return filename + 5;
  }

  return filename;
}

void CDvdFile::DVDARAMXferCallback(s32 result, DVDFileInfo* info) {
  CDvdFileARAM::SDvdInfo* ptr = reinterpret_cast< CDvdFileARAM::SDvdInfo* >(info);
#ifdef TARGET_PC
  // Port: a read can end short (even at 0 bytes) without an error. That chunk of
  // the ARAM copy would stay unfilled, and every resource in it would read as
  // zeros (issue #176), so read the chunk again; fail the file after a few tries.
  static int sRetries = 0; // one ARAM file loads at a time
  const s32 wanted = static_cast< s32 >(info->cb.length);
  DVDClose(&ptr->mDvdFileInfo);
  if (result >= 0 && result < wanted) {
    std::lock_guard< std::recursive_mutex > guard(AramFileStateMutex());
    CDvdFile* file = ptr->mDvdFile;
    CDvdFileARAM* arFile = file->mARAMFile.get();
    const uint offset = arFile->mFileSize2 - wanted;
    PortLog::Write("resource: ARAM copy of %s: read ended short, %d of %d bytes at %u; reading it again\n",
                   file->mFilename.data(), result, wanted, offset);
    if (++sRetries <= 3 && DVDFastOpen(file->mFileEntry, &ptr->mDvdFileInfo) &&
        DVDReadAsync(&ptr->mDvdFileInfo, arFile->mBuffers[arFile->mBufferIndex].get(), wanted, offset,
                     DVDARAMXferCallback)) {
      return;
    }
    sRetries = 0;
    ptr->mDvdFile->HandleDVDInterrupt(false);
    return;
  }
  sRetries = 0;
#else
  DVDClose(&ptr->mDvdFileInfo);
#endif
  ptr->mDvdFile->HandleDVDInterrupt(result >= 0);
}

void CDvdFile::ARAMARAMXferCallback(uintptr_t addr) {
  reinterpret_cast< CDvdFileARAM* >(addr)->mInfo.mDvdFile->HandleARAMInterrupt();
}

void CDvdFile::HandleARAMInterrupt() {
  std::lock_guard< std::recursive_mutex > guard(AramFileStateMutex());
  BOOL enabled = OSDisableInterrupts();
  CDvdFileARAM* arFile = mARAMFile.get();

  arFile->mGotARAMInterrupt = true;

  if (arFile->mGotARAMInterrupt && arFile->mGotDvdInterrupt) {
    PingARAMTransfer();
  }

  OSRestoreInterrupts(enabled);
}

void CDvdFile::HandleDVDInterrupt(bool success) {
  std::lock_guard< std::recursive_mutex > guard(AramFileStateMutex());
  BOOL enabled = OSDisableInterrupts();
  CDvdFileARAM* arFile = mARAMFile.get();

  if (!success) {
    mARAMFailed = true;
    arFile->mBufferLen = 0;
    arFile->mCurBufferLen = 0;
  }

  arFile->mGotDvdInterrupt = true;

  if (arFile->mGotARAMInterrupt && arFile->mGotDvdInterrupt) {
    PingARAMTransfer();
  }

  OSRestoreInterrupts(enabled);
}

void CDvdFile::PingARAMTransfer() {
  std::lock_guard< std::recursive_mutex > guard(AramFileStateMutex());
  CDvdFileARAM* aramFile = mARAMFile.get();

  if (aramFile->mBufferLen <= 0) {
    PopARAMFileLoad();
    return;
  }

  int length = rstl::min_val(65536, aramFile->mBufferLen);
  ARQPostRequest(&aramFile->mARQRequest, 0, ARQ_TYPE_MRAM_TO_ARAM, ARQ_PRIORITY_HIGH,
                 reinterpret_cast< uintptr_t >(aramFile->mBuffers[aramFile->mBufferIndex].get()),
                 reinterpret_cast< uintptr_t >(mARAMBuffer + aramFile->mAramOffset), length,
                 ARAMARAMXferCallback);

  aramFile->mBufferLen -= length;
  aramFile->mAramOffset += length;
  aramFile->mGotARAMInterrupt = false;
  aramFile->mBufferIndex ^= 1;

  if (aramFile->mCurBufferLen > 0) {
    int length2 = rstl::min_val(65536, aramFile->mCurBufferLen);
    const uint readOffset = aramFile->mFileSize2;
    aramFile->mFileSize2 += length2;
    aramFile->mCurBufferLen -= length2;
    aramFile->mGotDvdInterrupt = false;
    if (!DVDFastOpen(mFileEntry, &aramFile->mInfo.mDvdFileInfo) ||
        !DVDReadAsync(&aramFile->mInfo.mDvdFileInfo, aramFile->mBuffers[aramFile->mBufferIndex].get(),
                      length2, readOffset, DVDARAMXferCallback)) {
      HandleDVDInterrupt(false);
    }
  }
}

void CDvdFile::TryARAMFile() {
  std::lock_guard< std::recursive_mutex > guard(AramFileStateMutex());
  mARAMBuffer = static_cast< uchar* >(CARAMManager::Alloc(mSize));
  if (CARAMManager::GetInvalidAlloc() == mARAMBuffer) {
    return;
  }
  mARAMFile = rs_new CDvdFileARAM();
  CDvdFileARAM* arfile = mARAMFile.get();
  arfile->mInfo.mDvdFile = this;
  arfile->mGotARAMInterrupt = true;
  arfile->mFileSize1 = arfile->mCurBufferLen = arfile->mBufferLen = GetFileSize();
  mARAMAllocated = true;
  PushARAMFileLoad();
}

void CDvdFile::PushARAMFileLoad() {
  std::lock_guard< std::recursive_mutex > guard(AramFileStateMutex());
  BOOL enabled = OSDisableInterrupts();
  CDvdFile* file = sFirstARAM;
  if (file == NULL) {
    sFirstARAM = this;
    StartARAMFileLoad();
  } else {
    for (CDvdFile* p = file; p != nullptr; p = p->mARAMFile->mInfo.mNextfile) {
      if (p->mARAMFile->mInfo.mNextfile == nullptr) {
        p->mARAMFile->mInfo.mNextfile = this;
        break;
      }
    }
  }
  OSRestoreInterrupts(enabled);
}

void CDvdFile::PopARAMFileLoad() {
  std::lock_guard< std::recursive_mutex > guard(AramFileStateMutex());
  BOOL enabled = OSDisableInterrupts();
  CDvdFile* file = mARAMFile->mInfo.mNextfile;
  mARAMPopped = true;
  sFirstARAM = file;
  if (file != nullptr) {
    file->StartARAMFileLoad();
  }

  OSRestoreInterrupts(enabled);
}

bool CDvdFile::IsARAMFileLoaded() {
  std::lock_guard< std::recursive_mutex > guard(AramFileStateMutex());
  if (!mARAMAllocated) {
    return true;
  }

  if (!mARAMPopped) {
    return false;
  }

  mARAMFile = nullptr;

  return true;
}

void CDvdFile::StartARAMFileLoad() {
  std::lock_guard< std::recursive_mutex > guard(AramFileStateMutex());
  CDvdFileARAM* aramFile = mARAMFile.get();
  // Host arrays have array owners; the game-heap allocator is a separate domain.
  aramFile->mBuffers.push_back(rs_new uchar[0x10000]);
  aramFile->mBuffers.push_back(rs_new uchar[0x10000]);

  int len = rstl::min_val(mSize, 65536);
  aramFile->mCurBufferLen -= len;
  aramFile->mFileSize2 = len;
  if (!DVDFastOpen(mFileEntry, &aramFile->mInfo.mDvdFileInfo) ||
      !DVDReadAsync(&aramFile->mInfo.mDvdFileInfo, aramFile->mBuffers[0].get(), len, 0,
                    DVDARAMXferCallback)) {
    HandleDVDInterrupt(false);
  }
}

void CDvdFile::StallForARAMFile() {
  while (!IsARAMFileLoaded()) {
    ARQPoll();
    OSYieldThread();
  }
  if (mARAMFailed) {
    throw std::runtime_error(std::string("Failed to load ARAM file: ") + mFilename.data());
  }
}

CDvdFile::CDvdFile(const char* filename)
: mFileEntry(-1)
, mARAMBuffer(0)
, mARAMAllocated(false)
, mARAMPopped(false)
, mARAMFailed(false)
, mARAMFile(nullptr)
, mOffset(0)
, mSize(0)
, mFilename(filename, -1) {
  const char* decodedName = DecodeARAMFile(filename);
  mFileEntry = DVDConvertPathToEntrynum(const_cast< char* >(decodedName));
  DVDFileInfo fileInfo{};
  if (mFileEntry == -1 || !DVDFastOpen(mFileEntry, &fileInfo)) {
    throw std::runtime_error(std::string("Disc file not found: ") + decodedName);
  }

  mSize = fileInfo.length;
  DVDClose(&fileInfo);

  if (filename != decodedName) {
    TryARAMFile();
  }
}

CDvdFile::~CDvdFile() { CloseFile(); }

CDvdRequest* CDvdFile::SyncRead(void* dest, uint len) {
  return AsyncSeekRead(dest, len, kSO_Current, 0);
}

void CDvdFile::SyncSeekRead(void* dest, uint len, ESeekOrigin origin, int offset) {
  StallForARAMFile();
  CalcFileOffset(offset, origin);

  if (mARAMAllocated) {
    uint roundedLen = (len + 31) & ~31;
    DCFlushRange(dest, roundedLen);
    CARAMManager::WaitForDMACompletion(CARAMManager::DMAToMRAM(
        mARAMBuffer + mOffset, dest, roundedLen, CARAMManager::kDMAPrio_One));
  } else {
    DVDFileInfo info{};
    if (!DVDFastOpen(mFileEntry, &info)) {
      throw std::runtime_error(std::string("Could not open disc file: ") + mFilename.data());
    }
    const int result = DVDReadPrio(&info, dest, (len + 31) & ~31, mOffset, 2);
    DVDClose(&info);
    if (result < 0) {
      throw std::runtime_error(std::string("Disc read failed: ") + mFilename.data());
    }
  }

  UpdateFilePos(len);
}

CDvdRequest* CDvdFile::SyncReadBlocking(void* dest, uint len) {
  StallForARAMFile();
  const int roundedLen = (len + 31) & ~31;
  CDvdRequest* request;
  if (mARAMAllocated) {
    DCFlushRange(dest, roundedLen);
    request = rs_new CARAMDvdRequest(CARAMManager::DMAToMRAM(mARAMBuffer + mOffset, dest, roundedLen,
                                                             CARAMManager::kDMAPrio_One));
  } else {
    CRealDvdRequest* req = rs_new CRealDvdRequest();
    DVDFileInfo* info = req->FileInfo();
    if (!DVDFastOpen(mFileEntry, info)) {
      delete req;
      throw std::runtime_error(std::string("Could not open disc file: ") + mFilename.data());
    }
    // DVDReadPrio waits for the worker, so the data is resident on return.
    if (DVDReadPrio(info, dest, roundedLen, mOffset, 2) < 0) {
      delete req;
      throw std::runtime_error(std::string("Disc read failed: ") + mFilename.data());
    }
    request = req;
  }
  UpdateFilePos(len);
  return request;
}

CDvdRequest* CDvdFile::AsyncSeekRead(void* dest, uint len, ESeekOrigin origin, int offset) {
  StallForARAMFile();
  CalcFileOffset(offset, origin);
  CDvdRequest* request;
  if (mARAMAllocated) {
    const int roundedLen = (len + 31) & ~31;
    DCFlushRange(dest, roundedLen);
    request = rs_new CARAMDvdRequest(CARAMManager::DMAToMRAM(
        mARAMBuffer + mOffset, dest, roundedLen, CARAMManager::kDMAPrio_One));
  } else {
    CRealDvdRequest* req = rs_new CRealDvdRequest();
    DVDFileInfo* info = req->FileInfo();
    if (!DVDFastOpen(mFileEntry, info) ||
        !DVDReadAsync(info, dest, (len + 31) & ~31, mOffset, internalCallback)) {
      delete req;
      throw std::runtime_error(std::string("Could not start disc read: ") + mFilename.data());
    }
    request = req;
  }

  UpdateFilePos(len);

  return request;
}

void CDvdFile::CloseFile() {
  if (!mARAMAllocated) {
    return;
  }

  // Destruction must drain callback owners even on a failed read, and must not
  // throw while another resource error is already unwinding the stack.
  while (!IsARAMFileLoaded()) {
    ARQPoll();
    OSYieldThread();
  }
  CARAMManager::Free(mARAMBuffer);
  mARAMAllocated = false;
}

bool CDvdFile::FileExists(const char* filename) {
  return DVDConvertPathToEntrynum(const_cast< char* >(DecodeARAMFile(filename))) != -1;
}

void CDvdFile::internalCallback(s32 res, DVDFileInfo* info) {
#if VERSION >= VERSION_GM8E_01
  DCInvalidateRange((void*)info->cb.addr, info->cb.length);
#endif
}

void CDvdFile::CalcFileOffset(int offset, ESeekOrigin origin) {
  switch (origin) {
  case kSO_Begin:
    mOffset = offset;
    break;
  case kSO_Current:
    mOffset += offset;
    break;
  case kSO_End:
    mOffset = offset + mSize;
    break;
  }
}

void CDvdFile::UpdateFilePos(int pos) {
  mOffset += (pos + 31) & ~31;
  int filesize = GetFileSize();
  if (mOffset > filesize) {
    mOffset = filesize;
  }
}
