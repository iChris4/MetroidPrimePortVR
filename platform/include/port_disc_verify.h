#pragma once
// The full disc image check: reads the whole disc in the background and compares
// it with the Redump image of its revision, a MiB at a time, so a damaged copy
// (issue #176: zeros in the middle of Metroid4.pak) is found and named.
// The pure parts (SHA-1, revision table, FST) are in port_disc_verify_core.cpp.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace PortDiscVerify {

inline constexpr size_t kChunkSize = 1u << 20;

// SHA-1 over data fed in any number of pieces.
class Sha1Stream {
public:
  Sha1Stream();
  void Update(const void* data, size_t size);
  void Final(uint8_t out[20]); // ends the stream
  std::string FinalHex();

private:
  void Block(const uint8_t* block);
  uint32_t mState[5];
  uint64_t mLength = 0;
  uint8_t mBuffer[64];
  size_t mFill = 0;
};

// One supported release and what the right image of it is made of.
struct Revision {
  std::string id6;
  unsigned version;
  std::string name; // "USA 1.02"
  uint64_t size;
  std::string sha1; // lower-case hex
  std::vector<uint32_t> crc; // CRC32 of each kChunkSize chunk, the last one partial
};

// From the disc header (game id and version byte); null for any other disc.
const Revision* FindRevision(const char* id6, unsigned version);

uint32_t Crc32(const void* data, size_t size);
bool AllZero(const uint8_t* data, size_t size);

struct FstFile {
  uint64_t offset;
  uint64_t size;
  std::string path; // "Metroid4.pak", "audio/x.dsp"
};
// The files of a raw FST (entries then names), in order of entry. Empty if it is malformed.
std::vector<FstFile> ParseFst(const uint8_t* fst, size_t size);
// The files overlapping [begin, end) of the disc, each once.
std::vector<std::string> FilesOverlapping(const std::vector<FstFile>& files, uint64_t begin, uint64_t end);

struct BadChunk {
  uint32_t index;
  bool zeroed; // all zero bytes, as against other data (or unreadable, which also counts as not zeroed)
  bool unreadable;
};

// The chunks of data (starting at chunk firstIndex) that don't match the table's CRC32s.
void CompareChunk(const Revision& revision, uint32_t index, const uint8_t* data, size_t size,
                  std::vector<BadChunk>& bad);

enum class State { Idle, Running, Done, Cancelled, Error };

struct Result {
  State state = State::Idle;
  uint64_t bytesDone = 0;
  uint64_t bytesTotal = 0; // 0 until known
  bool known = false; // the header names a supported revision
  std::string revision; // "USA 1.02", or "unknown disc"
  std::string sha1; // of what was read
  bool sha1Matches = false;
  uint32_t chunks = 0;
  std::vector<BadChunk> bad;
  uint32_t zeroed = 0;
  std::vector<std::string> files; // damaged files, every one
  std::string error;
  double seconds = 0.0;

  bool Ok() const { return state == State::Done && known && sha1Matches && bad.empty(); }
  double Progress() const { return bytesTotal ? static_cast<double>(bytesDone) / static_cast<double>(bytesTotal) : 0.0; }
};

// The one-line log summary (without the "metroid_prime_port: " prefix) and the plain advice.
std::string Summary(const Result& result, size_t maxFiles = 6);

// The disc image being played, set once it is mounted.
void SetImagePath(const std::string& path);
// The image set above ("" before the disc is mounted).
std::string ImagePath();
// The first eight bytes of the disc (game id, maker, disc number, revision) of a plain image or a
// container (RVZ, WIA, WBFS, CISO, GCZ), read through its own handle. "" on success, else why not.
std::string ReadDiscHeader(const std::string& path, uint8_t out[8]);
// Starts a check on its own thread; false if one is running or no disc is set.
bool Start();
void Cancel();
// Cancels and joins; call before the process ends.
void Shutdown();
Result Snapshot();

} // namespace PortDiscVerify
