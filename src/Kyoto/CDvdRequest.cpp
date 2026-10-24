#include "Kyoto/CDvdRequest.hpp"

#include "Kyoto/CARAMManager.hpp"

#include "Kyoto/Alloc/CMemory.hpp"
#include <dolphin/os.h>
#include <stdexcept>

CRealDvdRequest::~CRealDvdRequest() {
  // DVDClose drains the worker, including its callback. Do not call the
  // throwing completion check while unwinding a failed resource load.
  DVDClose(&mFileInfo);
}

void CRealDvdRequest::WaitUntilComplete() {
  while (!CRealDvdRequest::IsComplete()) {
    OSYieldThread();
  }
}

bool CRealDvdRequest::IsComplete()  {
  s32 status = DVDGetCommandBlockStatus(&mFileInfo.cb);
  if (status == DVD_STATE_FATAL_ERROR) {
    throw std::runtime_error("Asynchronous disc read failed");
  }
  bool ret = false;
  if (status == DVD_STATE_END || status == DVD_STATE_CANCELED) {
    ret = true;
  }

  return ret;
}

void CRealDvdRequest::PostCancelRequest() {
  DVDCancelAsync(&mFileInfo.cb, nullptr);
}

int CRealDvdRequest::GetMediaType() const {
  return 1;
}

#ifdef TARGET_PC
int CRealDvdRequest::PortTransferred() {
  return PortCancelled() ? 0 : static_cast< int >(mFileInfo.cb.transferredSize);
}

bool CRealDvdRequest::PortCancelled() {
  return DVDGetCommandBlockStatus(&mFileInfo.cb) == DVD_STATE_CANCELED;
}
#endif

void CARAMDvdRequest::WaitUntilComplete() {
  if (CARAMManager::GetInvalidDMAHandle() == x4_dmaReq) {
    return;
  }

  CARAMManager::WaitForDMACompletion(x4_dmaReq);
  x4_dmaReq = CARAMManager::GetInvalidDMAHandle();
}

bool CARAMDvdRequest::IsComplete() {
  if (x4_dmaReq != CARAMManager::GetInvalidDMAHandle()) {
    if (!CARAMManager::IsDMACompleted(x4_dmaReq)) {
      return false;
    }

    x4_dmaReq = CARAMManager::GetInvalidDMAHandle();
  }

  return true;
}

void CARAMDvdRequest::PostCancelRequest() {
  
}

int CARAMDvdRequest::GetMediaType() const {
  return 0;
}
