#include "port_disc_verify.h"

#include "port_log.h"

#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_thread.h>
#include <SDL3/SDL_timer.h>
#include <nod.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>

namespace PortDiscVerify {

namespace {

std::mutex sMutex;
std::mutex sThreadMutex; // Start and Shutdown
std::string sImage;
Result sResult;
std::thread sThread;
std::atomic<bool> sCancel{false};

// The check reads through its own handle onto the same file: aurora's mounted
// disc (and so the game's reads) is never touched.
int64_t ReadAt(void* user, uint64_t offset, void* out, size_t len) {
  auto* io = static_cast<SDL_IOStream*>(user);
  if (io == nullptr || SDL_SeekIO(io, static_cast<Sint64>(offset), SDL_IO_SEEK_SET) < 0) {
    return -1;
  }
  size_t total = 0;
  while (total < len) {
    const size_t n = SDL_ReadIO(io, static_cast<uint8_t*>(out) + total, len - total);
    if (n == 0) {
      break;
    }
    total += n;
  }
  return static_cast<int64_t>(total);
}
int64_t Len(void* user) {
  const Sint64 size = user != nullptr ? SDL_GetIOSize(static_cast<SDL_IOStream*>(user)) : -1;
  return size < 0 ? -1 : size;
}
void Close(void* user) {
  if (user != nullptr) {
    SDL_CloseIO(static_cast<SDL_IOStream*>(user));
  }
}

// Reads len bytes from the disc handle's current position; fewer means the end or a failure.
size_t ReadFull(NodHandle* disc, uint8_t* buf, size_t len) {
  size_t total = 0;
  while (total < len) {
    const int64_t n = nod_read(disc, buf + total, len - total);
    if (n <= 0) {
      break;
    }
    total += static_cast<size_t>(n);
  }
  return total;
}

void Fail(const std::string& error) {
  std::lock_guard lock(sMutex);
  sResult.state = State::Error;
  sResult.error = error;
}

void Run(std::string path) {
  SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_LOW);
  const Uint64 started = SDL_GetTicks();
  SDL_IOStream* io = SDL_IOFromFile(path.c_str(), "rb");
  if (io == nullptr) {
    return Fail("the disc image can't be opened");
  }
  const NodDiscStream stream{.user_data = io, .read_at = ReadAt, .stream_len = Len, .close = Close};
  const NodDiscOptions options{.preloader_threads = 1};
  NodHandle* disc = nullptr;
  if (nod_disc_open_stream(&stream, &options, &disc) != NOD_RESULT_OK || disc == nullptr) {
    return Fail("the disc image can't be read as a GameCube disc");
  }

  std::vector<uint8_t> buf(kChunkSize);
  size_t got = ReadFull(disc, buf.data(), buf.size());
  if (got < 0x440) {
    nod_free(disc);
    return Fail("the disc image is too short");
  }
  // The revision comes from the header, not the hash, so a damaged image is
  // still compared with the right one.
  const Revision* revision = FindRevision(reinterpret_cast<const char*>(buf.data()), buf[7]);
  const bool known = revision != nullptr && buf[6] == 0;
  if (!known) {
    revision = nullptr;
  }
  {
    std::lock_guard lock(sMutex);
    sResult.known = known;
    sResult.revision = known ? revision->name : "unknown disc";
    sResult.bytesTotal = known ? revision->size : 0;
    sResult.chunks = known ? static_cast<uint32_t>(revision->crc.size()) : 0;
  }

  Sha1Stream sha;
  std::vector<BadChunk> bad;
  uint64_t done = 0;
  uint32_t index = 0;
  bool cancelled = false;
  for (;;) {
    if (sCancel.load()) {
      cancelled = true;
      break;
    }
    if (got == 0) {
      break;
    }
    if (known) {
      CompareChunk(*revision, index, buf.data(), got, bad);
    }
    sha.Update(buf.data(), got);
    done += got;
    ++index;
    {
      std::lock_guard lock(sMutex);
      sResult.bytesDone = done;
    }
    if (got < kChunkSize || (known && done >= revision->size)) {
      break;
    }
    got = ReadFull(disc, buf.data(), buf.size());
  }
  // A short image: the chunks that were never read are bad.
  if (known && !cancelled) {
    for (uint32_t i = index; i < revision->crc.size(); ++i) {
      bad.push_back({i, false, true});
    }
  }

  // Name the files under the bad chunks from the disc's own FST.
  std::vector<std::string> files;
  if (!bad.empty() && !cancelled) {
    uint8_t header[0x430];
    if (nod_seek(disc, 0, SEEK_SET) == 0 && ReadFull(disc, header, sizeof(header)) == sizeof(header)) {
      const uint32_t fstOffset = (header[0x424] << 24) | (header[0x425] << 16) | (header[0x426] << 8) | header[0x427];
      const uint32_t fstSize = (header[0x428] << 24) | (header[0x429] << 16) | (header[0x42a] << 8) | header[0x42b];
      std::vector<uint8_t> fst(fstSize);
      if (fstSize != 0 && fstSize <= (16u << 20) && nod_seek(disc, fstOffset, SEEK_SET) == fstOffset &&
          ReadFull(disc, fst.data(), fst.size()) == fst.size()) {
        const std::vector<FstFile> all = ParseFst(fst.data(), fst.size());
        for (const BadChunk& b : bad) {
          for (std::string& name : FilesOverlapping(all, uint64_t(b.index) * kChunkSize, uint64_t(b.index + 1) * kChunkSize)) {
            if (std::find(files.begin(), files.end(), name) == files.end()) {
              files.push_back(std::move(name));
            }
          }
        }
      }
    }
  }
  nod_free(disc);

  Result final;
  {
    std::lock_guard lock(sMutex);
    final = sResult;
  }
  final.state = cancelled ? State::Cancelled : State::Done;
  final.bytesDone = done;
  final.sha1 = sha.FinalHex();
  final.sha1Matches = known && final.sha1 == revision->sha1;
  final.bad = bad;
  for (const BadChunk& b : bad) {
    final.zeroed += b.zeroed;
  }
  final.files = files;
  final.seconds = (SDL_GetTicks() - started) / 1000.0;
  PortLog::Write("metroid_prime_port: %s (%.1f s)\n", Summary(final).c_str(), final.seconds);
  std::lock_guard lock(sMutex);
  sResult = final;
}

} // namespace

void SetImagePath(const std::string& path) {
  std::lock_guard lock(sMutex);
  sImage = path;
}

std::string ImagePath() {
  std::lock_guard lock(sMutex);
  return sImage;
}

std::string ReadDiscHeader(const std::string& path, uint8_t out[8]) {
  SDL_IOStream* io = SDL_IOFromFile(path.c_str(), "rb");
  if (io == nullptr) {
    return "the disc image can't be opened";
  }
  const NodDiscStream stream{.user_data = io, .read_at = ReadAt, .stream_len = Len, .close = Close};
  const NodDiscOptions options{.preloader_threads = 1};
  NodHandle* disc = nullptr;
  if (nod_disc_open_stream(&stream, &options, &disc) != NOD_RESULT_OK || disc == nullptr) {
    return "this is not a GameCube disc image";
  }
  const bool ok = ReadFull(disc, out, 8) == 8;
  nod_free(disc);
  return ok ? std::string() : "the disc image is too short";
}

bool Start() {
  std::lock_guard threadLock(sThreadMutex);
  std::string path;
  {
    std::lock_guard lock(sMutex);
    if (sResult.state == State::Running || sImage.empty()) {
      return false;
    }
    path = sImage;
  }
  if (sThread.joinable()) {
    sThread.join();
  }
  sCancel = false;
  {
    std::lock_guard lock(sMutex);
    sResult = Result{};
    sResult.state = State::Running;
  }
  sThread = std::thread([path] { Run(path); });
  return true;
}

void Cancel() { sCancel = true; }

void Shutdown() {
  Cancel();
  std::lock_guard threadLock(sThreadMutex);
  if (sThread.joinable()) {
    sThread.join();
  }
}

Result Snapshot() {
  std::lock_guard lock(sMutex);
  return sResult;
}

} // namespace PortDiscVerify
