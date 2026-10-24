#include "Kyoto/CResFactory.hpp"
#include "Kyoto/Basics/CBasics.hpp"

#include "Kyoto/Alloc/CMemory.hpp"
#include "Kyoto/Basics/CStopwatch.hpp"
#include "Kyoto/CDvdRequest.hpp"
#include "Kyoto/Streams/CZipSupport.hpp"
#include "rstl/math.hpp"
#include "zlib/zlib.h"

#include <dolphin/ar.h>
#include <dolphin/os.h>

#ifdef TARGET_PC
#include "Kyoto/CPakFile.hpp"
#include "port_log.h"

#include <cstring>
#include <exception>
#include <stdexcept>
#endif

static const int kDecompChunkSize = 4096;

// Written by CMFGameLoader; its purpose is not yet identified.
int gResFactoryUnknown = 2;

#ifdef TARGET_PC
static void PortReportBadResource(CResLoader& loader, const SObjectTag& tag, int size, int compression,
                                  const uint* head, const char* what) {
  const uint type = tag.GetType();
  const uint id = tag.GetId();
  const uchar* bytes = reinterpret_cast< const uchar* >(head);
  PortLog::Write("resource: %c%c%c%c %08X failed to load (%s): %d byte(s) given, compression %d, "
                 "first bytes %02X%02X%02X%02X %02X%02X%02X%02X\n",
                 char(type >> 24), char(type >> 16), char(type >> 8), char(type), id, what, size, compression,
                 bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7]);
  for (int i = 0; i < loader.GetPakCount(); ++i) {
    CPakFile* pak = loader.GetPakFile(i);
    const CPakFile::SResInfo* info = pak->GetResInfo(id);
    if (info != nullptr) {
      PortLog::Write("resource:   in %s: offset %u, size %u, %s%s\n", pak->GetDvdFile().GetFilename().data(),
                     info->GetOffset(), info->GetSize(), info->IsCompressed() ? "compressed" : "uncompressed",
                     pak->IsWorldPak() ? ", world PAK" : "");
    } else if (pak->IsStashedInARAM()) {
      PortLog::Write("resource:   %s: table stashed in ARAM\n", pak->GetDvdFile().GetFilename().data());
    }
  }
}
#endif

CResFactory::CResFactory() {}

CResFactory::~CResFactory() {}

void CResFactory::AddToLoadList(const SLoadingData& data) {
  AUTO(pos, x84_loadList.end());
  if (data.x8_dvdReq->GetMediaType() == 0) {
    pos = x84_loadList.begin();
    while (pos != x84_loadList.end()) {
      if (pos->x8_dvdReq->GetMediaType() == 1) {
        break;
      }
      ++pos;
    }
  }
  AUTO(it, x84_loadList.insert(pos, data));
  x9c_loadMap.insert(rstl::pair< SObjectTag, LoadList::iterator >(data.x0_tag, it));
}

void CResFactory::EraseFromLoadList(const LoadList::iterator& it) {
  x9c_loadMap.erase(it->x0_tag);
  x84_loadList.erase(it);
}

CResFactory::LoadList::iterator CResFactory::FindInLoadList(const SObjectTag& tag) {
  rstl::map< SObjectTag, LoadList::iterator >::const_iterator it = x9c_loadMap.find(tag);
  if (it == x9c_loadMap.end()) {
    return x84_loadList.end();
  }
  return it->second;
}

rstl::auto_ptr< IObj > CResFactory::Build(const SObjectTag& tag, const CVParamTransfer& params) {
  AUTO(it, FindInLoadList(tag));
  if (it != x84_loadList.end()) {
    IObj** target = it->x10_target;
    while (*target == nullptr) {
      ARQPoll();
      while (!PumpResource(it, 0)) {
        ARQPoll();
      }
    }
    return rstl::auto_ptr< IObj >(*target);
  }
  return BuildSync(tag, params);
}

rstl::auto_ptr< IObj > CResFactory::BuildSync(const SObjectTag& tag,
                                              const CVParamTransfer& params) {
  if (x5c_factoryMgr.CanMakeMemory(tag)) {
    char* buffer;
    int size;
    x4_resLoader.LoadMemResourceSync(tag, &buffer, &size);
    return x5c_factoryMgr.MakeObjectFromMemory(tag, buffer, size,
                                               x4_resLoader.GetResourceCompression(tag) !=
                                                   CResLoader::kCompressionType_Uncompressed,
                                               params);
  }
  CInputStream* in = x4_resLoader.LoadNewResourceSync(tag, nullptr);
  rstl::auto_ptr< IObj > result = x5c_factoryMgr.MakeObject(tag, *in, params);
  delete in;
  return result;
}

void CResFactory::BuildAsync(const SObjectTag& tag, const CVParamTransfer& params, IObj** target) {
  *target = nullptr;
  const uint size = ResourceSize(tag);
  char* buffer = rs_new char[(size + 31) & ~31];
  CDvdRequest* request = LoadResourceAsync(tag, buffer);
  SLoadingData data(tag, request, target, buffer, size, x4_resLoader.GetResourceCompression(tag),
                    params);
  AddToLoadList(data);
}

void CResFactory::CancelBuild(const SObjectTag& tag) {
  AUTO(it, FindInLoadList(tag));
  if (it != x84_loadList.end()) {
    SLoadingData& data = *it;
    data.x8_dvdReq->PostCancelRequest();
    xb0_cancelledList.push_back(data);
    EraseFromLoadList(it);
  }
}

void CResFactory::AsyncIdle(uint time) {
  bool work = true;
  CStopwatch timer;
  AUTO(it, xb0_cancelledList.begin());
  while (it != xb0_cancelledList.end()) {
    AUTO(current, it);
    ++it;
    if (current->x8_dvdReq->IsComplete()) {
      xb0_cancelledList.erase(current);
    }
  }

  while (work) {
    work = false;
    uint elapsed = timer.GetElapsedMicros();
    AUTO(it, x84_loadList.begin());
    while (it != x84_loadList.end() && elapsed < time) {
      if (PumpResource(it, time - elapsed)) {
        work = true;
        break;
      }
      ++it;
      elapsed = timer.GetElapsedMicros();
    }
  }
}

#ifdef TARGET_PC
int CResFactory::PortLoadState(const SObjectTag& tag) {
  AUTO(it, FindInLoadList(tag));
  if (it == x84_loadList.end()) {
    return 0;
  }
  return it->x8_dvdReq->IsComplete() ? 2 : 1;
}

void CResFactory::PortReopenPaks(void (*between)()) {
  // A load reads its whole resource in one request, from the PAK as it is now.
  while (!x84_loadList.empty() || !xb0_cancelledList.empty()) {
    AsyncIdle(1000000);
    ARQPoll();
    OSYieldThread();
  }
  x4_resLoader.PortReopenPaks(between);
}

void CResFactory::PortLoadListCounts(int& total, int& pending) {
  total = 0;
  pending = 0;
  for (AUTO(it, x84_loadList.begin()); it != x84_loadList.end(); ++it) {
    ++total;
    if (!it->x8_dvdReq->IsComplete()) {
      ++pending;
    }
  }
}
#endif

#ifdef TARGET_PC
// Port: a read that ended cancelled or short leaves the buffer unfilled, and the
// factory would parse it anyway: a zeroed compressed TXTR inflates to 0 bytes and
// aborts the game (issue #176). Read it again instead; give up after a few tries.
bool CResFactory::PortRetryFailedRead(SLoadingData& data) {
  if (data.x24_zip.get() != nullptr) {
    return false; // already inflating, so its buffer was checked
  }
  const int transferred = data.x8_dvdReq->PortTransferred();
  const char* how = nullptr;
  if (data.x8_dvdReq->PortCancelled()) {
    how = "cancelled";
  } else if (transferred >= 0 && transferred < data.x2c_size) {
    how = "short";
  } else if (data.x30_compression == CResLoader::kCompressionType_Compressed && data.x2c_size >= 4 &&
             *reinterpret_cast< const uint* >(data.x14_buffer.get()) == 0) {
    how = "with an empty compressed header"; // no compressed resource inflates to 0 bytes
  }
  if (how == nullptr) {
    return false;
  }
  const uint type = data.x0_tag.GetType();
  const uint id = data.x0_tag.GetId();
  const char* pakName = "?";
  for (int i = 0; i < x4_resLoader.GetPakCount(); ++i) {
    CPakFile* pak = x4_resLoader.GetPakFile(i);
    if (pak->GetResInfo(id) != nullptr) {
      pakName = pak->GetDvdFile().GetFilename().data();
      break;
    }
  }
  PortLog::Write("resource: %c%c%c%c %08X read ended %s, %d of %d bytes from %s; reading it again\n",
                 char(type >> 24), char(type >> 16), char(type >> 8), char(type), id, how, transferred,
                 data.x2c_size, pakName);
  if (++data.xPortReadRetries > 3 || !x4_resLoader.ResourceExists(id)) {
    throw std::runtime_error("Disc read failed");
  }
  data.x8_dvdReq = rstl::auto_ptr< CDvdRequest >(LoadResourceAsync(data.x0_tag, data.x14_buffer.get()));
  return true;
}
#endif

bool CResFactory::PumpResource(const LoadList::iterator& it, uint time) {
  if (it->x8_dvdReq->IsComplete()) {
#ifdef TARGET_PC
    if (PortRetryFailedRead(*it)) {
      return false;
    }
#endif
    if (it->x30_compression == CResLoader::kCompressionType_Compressed &&
        !it->PumpDecompression(time)) {
      return false;
    }
    SLoadingData data(*it);
    EraseFromLoadList(it);
#ifdef TARGET_PC
    // Port: a resource the factory cannot read aborts the game; name it, and
    // where it was read from, so a crash report says which one.
    const SObjectTag tag = data.x0_tag;
    const int size = data.x2c_size;
    const int compression = data.x30_compression;
    uint head[2] = {0, 0};
    if (data.x14_buffer.get() != nullptr) {
      memcpy(head, data.x14_buffer.get(), rstl::min_val< int >(sizeof(head), rstl::max_val(size, 0)));
    }
    try {
#endif
    *data.x10_target =
        x5c_factoryMgr
            .MakeObjectFromMemory(data.x0_tag, data.x14_buffer.release(), data.x2c_size,
                                  data.x30_compression != CResLoader::kCompressionType_Uncompressed,
                                  data.x34_params)
            .release();
#ifdef TARGET_PC
    } catch (const std::exception& e) {
      PortReportBadResource(x4_resLoader, tag, size, compression, head, e.what());
      throw;
    }
#endif
    return true;
  }
  return false;
}

CResFactory::SLoadingData::SLoadingData(const SObjectTag& tag, CDvdRequest* request, IObj** target,
                                        void* buffer, int size,
                                        CResLoader::ECompressionType compression,
                                        const CVParamTransfer& params)
: x0_tag(tag)
, x8_dvdReq(request)
, x10_target(target)
, x14_buffer(static_cast< uchar* >(buffer))
, x2c_size(size)
, x30_compression(compression)
, x34_params(params)
#ifdef TARGET_PC
, xPortReadRetries(0)
#endif
{
}

CResFactory::SLoadingData::~SLoadingData() {
  if (x24_zip.owner()) {
    inflateEnd(x24_zip.get());
  }
}

bool CResFactory::SLoadingData::PumpDecompression(uint time) {
  CStopwatch timer;
  z_stream_s* zip = x24_zip.get();
  uint* buffer = reinterpret_cast< uint* >(x14_buffer.get());
#if TARGET_LITTLE_ENDIAN
  const uint length = CBasics::SwapBytes(*buffer);
#else
  const uint length = *buffer;
#endif
  if (zip == nullptr) {
    zip = rs_new z_stream_s;
    zip->zalloc = CZipSupport::Alloc;
    zip->zfree = CZipSupport::Free;
    zip->opaque = nullptr;
    inflateInit2(zip);
    zip->total_in = 0;
    zip->total_out = 0;
    zip->avail_in = 0;
    zip->avail_out = 0;
    zip->next_in = nullptr;
    zip->next_out = nullptr;
    x24_zip = rstl::auto_ptr< z_stream_s >(zip);
    x1c_decompBuffer = rstl::auto_ptr< uchar[] >(rs_new uchar[length]);
  }

  const int size = x2c_size;
  uchar* compressed = reinterpret_cast< uchar* >(buffer + 1);
  while ((time == 0 || timer.GetElapsedMicros() < time) && length != zip->total_out) {
    if (zip->avail_in == 0) {
      const int amount = rstl::min_val< int >(kDecompChunkSize, size - 4 - zip->total_in);
      if (amount == 0) {
        break;
      }
      zip->next_in = compressed + zip->total_in;
      zip->avail_in = amount;
      zip->next_out = x1c_decompBuffer.get() + zip->total_out;
      zip->avail_out = length - zip->total_out;
    }
    inflate(zip, Z_NO_FLUSH);
  }

  if (length == zip->total_out) {
    inflateEnd(zip);
    x24_zip = rstl::auto_ptr< z_stream_s >();
    x14_buffer = rstl::auto_ptr< uchar[] >(x1c_decompBuffer.release());
    x1c_decompBuffer = rstl::auto_ptr< uchar[] >();
    x30_compression = CResLoader::kCompressionType_Uncompressed;
    x2c_size = length;
    return true;
  }
  return false;
}
