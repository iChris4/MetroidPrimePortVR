#include "port_test.h"

#include <aurora/aurora.h>
#include <aurora/card.h>
#include <dolphin/card.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

// Exercise the real CARD filesystem adapter without creating a GPU/window.
namespace aurora {
AuroraConfig g_config{};
char g_gameName[4] = {'G', 'M', '8', 'E'};
void log_internal(AuroraLogLevel, const char*, const char* message, unsigned int length) noexcept {
  std::fprintf(stderr, "%.*s\n", static_cast<int>(length), message);
}
}
int main(int argc, char** argv) {
  REQUIRE(argc == 2);
  const std::string path = argv[1];
  std::filesystem::create_directories(path);
  aurora::g_config.userPath = path.c_str();
  CARDInit("GM8E", "01");
  REQUIRE(CARDMountAsync(0, nullptr, nullptr, nullptr) == CARD_RESULT_READY);
  REQUIRE(CARDCheckAsync(0, nullptr) == CARD_RESULT_READY);
  REQUIRE(CARDCheckExAsync(0, nullptr, nullptr) == CARD_RESULT_READY);
  // This directory belongs only to this regression test, never the game profile.
  REQUIRE(CARDFormatAsync(0, nullptr) == CARD_RESULT_READY);
  CARDFileInfo file{};
  REQUIRE(CARDCreateAsync(0, "port-regression", 8192, &file, nullptr) == CARD_RESULT_READY);
  alignas(32) std::array<unsigned char, 8192> original{}, restored{};
  for (size_t i = 0; i < original.size(); ++i) original[i] = (i * 31) ^ (i >> 4);
  REQUIRE(CARDWriteAsync(&file, original.data(), original.size(), 0, nullptr) == CARD_RESULT_READY);
  REQUIRE(CARDWriteAsync(&file, original.data(), 32, -1, nullptr) == CARD_RESULT_FATAL_ERROR);
  CARDStat status{};
  REQUIRE(CARDGetStatus(0, file.fileNo, &status) == CARD_RESULT_READY);
  REQUIRE(CARDSetStatusAsync(0, file.fileNo, &status, nullptr) == CARD_RESULT_READY);
  REQUIRE(CARDClose(&file) == CARD_RESULT_READY);
  REQUIRE(CARDRenameAsync(0, "port-regression", "port-renamed", nullptr) == CARD_RESULT_READY);
  REQUIRE(CARDUnmount(0) == CARD_RESULT_READY);
  REQUIRE(aurora_card_remount(0)); // force a disk reload, not just an in-memory read
  REQUIRE(CARDMountAsync(0, nullptr, nullptr, nullptr) == CARD_RESULT_READY);
  REQUIRE(CARDOpen(0, "port-renamed", &file) == CARD_RESULT_READY);
  REQUIRE(CARDReadAsync(&file, restored.data(), restored.size(), 0, nullptr) == CARD_RESULT_READY);
  REQUIRE(restored == original);
  REQUIRE(CARDClose(&file) == CARD_RESULT_READY);
  REQUIRE(CARDFastDeleteAsync(0, file.fileNo, nullptr) == CARD_RESULT_READY);
  REQUIRE(CARDDeleteAsync(0, "port-renamed", nullptr) != CARD_RESULT_READY);
  REQUIRE(CARDUnmount(0) == CARD_RESULT_READY);
  std::puts("CARD null-callback and filesystem round-trip regressions passed");
}
