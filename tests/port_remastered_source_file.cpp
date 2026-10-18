// SourceFile (port_remastered_nsp.h): the game image read at any offset from
// several threads at once, as the import's workers do. On Windows this is the
// overlapped handle, elsewhere pread; the bytes must be the file's either way,
// and a read at or past the end must come back short instead of failing badly.

#include "port_remastered_nsp.h"
#include "port_test.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace PortRemastered;

namespace {

uint8_t ByteAt(uint64_t offset) { return uint8_t((offset * 2654435761u) >> 13); }
} // namespace

int main() {
  namespace fs = std::filesystem;
  // A name outside ASCII, as a user's folder may have: the path is UTF-8.
  const std::string name = "port_source_file_test_\xC3\xA9.bin";
  const fs::path path = fs::temp_directory_path() / fs::path(std::u8string(name.begin(), name.end()));
  constexpr size_t kSize = 3 * 1024 * 1024 + 17;
  {
    std::vector<uint8_t> data(kSize);
    for (size_t i = 0; i < kSize; ++i) {
      data[i] = ByteAt(i);
    }
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
  }
  const std::u8string utf8 = path.u8string();
  SourceFile file;
  CHECK_MSG(file.Open(std::string(utf8.begin(), utf8.end())), "opens a UTF-8 path");

  std::atomic<int> bad{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&, t] {
      std::mt19937 rng{uint32_t(t)};
      std::vector<uint8_t> buffer;
      for (int i = 0; i < 200; ++i) {
        const uint64_t offset = rng() % kSize;
        const size_t size = std::min<size_t>(rng() % (256 * 1024) + 1, kSize - size_t(offset));
        buffer.assign(size, 0);
        if (!file.ReadAt(offset, buffer.data(), size)) {
          ++bad;
          continue;
        }
        for (size_t j = 0; j < size; ++j) {
          if (buffer[j] != ByteAt(offset + j)) {
            ++bad;
            break;
          }
        }
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  CHECK_MSG(bad == 0, "reads from eight threads at once return the file's bytes");

  uint8_t tail[64];
  CHECK_MSG(file.ReadSome(kSize - 10, tail, sizeof tail) == 10, "a read over the end comes back short");
  CHECK_MSG(tail[9] == ByteAt(kSize - 1), "the last byte");
  CHECK_MSG(file.ReadSome(kSize, tail, sizeof tail) == 0, "a read at the end reads nothing");
  CHECK_MSG(file.ReadSome(kSize + 4096, tail, sizeof tail) == 0, "a read past the end reads nothing");
  CHECK_MSG(!file.ReadAt(kSize - 1, tail, 2), "ReadAt over the end fails");

  file.Close();
  SourceFile missing;
  CHECK_MSG(!missing.Open((fs::temp_directory_path() / "port_source_file_test_missing.bin").string()),
            "a missing file does not open");
  std::error_code ec;
  fs::remove(path, ec);
  if (port_test::Failures() == 0) {
    std::printf("port_remastered_source_file: ok\n");
  }
  return PORT_TEST_RESULT();
}
