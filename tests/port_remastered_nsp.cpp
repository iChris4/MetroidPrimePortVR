// The .nsp reader (port_remastered_nsp.h) against an image made here: a PFS0
// holding one program NCA, its header AES-XTS encrypted with a made-up header
// key, its content key wrapped in the key area, and a RomFS section in AES-CTR.
// The made-up keys go in a prod.keys of their own, so no console's keys are
// needed. Then the same image with a field broken, and with random bytes
// changed, which must be refused or read, never crash.

#include "port_remastered_nsp.h"
#include "port_test.h"

#include <openssl/evp.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <random>
#include <string>
#include <vector>

using namespace PortRemastered;

namespace {

using Blob = std::vector<uint8_t>;
constexpr uint32_t kEmpty = 0xFFFFFFFF;

void Put32(Blob& out, size_t at, uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    out[at + size_t(i)] = uint8_t(v >> (8 * i));
  }
}
void Put64(Blob& out, size_t at, uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    out[at + size_t(i)] = uint8_t(v >> (8 * i));
  }
}
size_t Align(size_t v, size_t a) { return (v + a - 1) / a * a; }

// The made-up keys.
const uint8_t kHeaderKey[32] = {1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16,
                                17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32};
const uint8_t kKeyAreaKey[16] = {0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7,
                                 0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF};
const uint8_t kContentKey[16] = {0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
                                 0x59, 0x5A, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F, 0x60};
const uint8_t kNonce[8] = {9, 8, 7, 6, 5, 4, 3, 2};

std::string Hex(const uint8_t* data, size_t size) {
  static const char kDigits[] = "0123456789abcdef";
  std::string out;
  for (size_t i = 0; i < size; ++i) {
    out += kDigits[data[i] >> 4];
    out += kDigits[data[i] & 15];
  }
  return out;
}

// The inverses of the reader's DecryptHeader, DecryptEcb and DecryptCtr.
void EncryptHeader(uint8_t* data, size_t size) {
  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  for (size_t sector = 0; sector * 0x200 < size; ++sector) {
    uint8_t tweak[16] = {};
    for (int i = 15, shift = 0; i >= 8; --i, shift += 8) {
      tweak[i] = uint8_t(uint64_t(sector) >> shift);
    }
    int len = 0;
    EVP_EncryptInit_ex(ctx, EVP_aes_128_xts(), nullptr, kHeaderKey, tweak);
    EVP_EncryptUpdate(ctx, data + sector * 0x200, &len, data + sector * 0x200, 0x200);
  }
  EVP_CIPHER_CTX_free(ctx);
}

void EncryptEcb(const uint8_t* key, const uint8_t* in, uint8_t* out) {
  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  int len = 0;
  EVP_EncryptInit_ex(ctx, EVP_aes_128_ecb(), nullptr, key, nullptr);
  EVP_CIPHER_CTX_set_padding(ctx, 0);
  EVP_EncryptUpdate(ctx, out, &len, in, 16);
  EVP_CIPHER_CTX_free(ctx);
}

void Ctr(uint64_t ncaOffset, uint8_t* data, size_t size) {
  uint8_t iv[16];
  for (int i = 0; i < 8; ++i) {
    iv[i] = kNonce[7 - i];
  }
  uint64_t block = ncaOffset >> 4;
  for (int i = 15; i >= 8; --i) {
    iv[i] = uint8_t(block);
    block >>= 8;
  }
  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  int len = 0;
  EVP_EncryptInit_ex(ctx, EVP_aes_128_ctr(), nullptr, kContentKey, iv);
  EVP_EncryptUpdate(ctx, data, &len, data, int(size));
  EVP_CIPHER_CTX_free(ctx);
}

Blob Content(size_t size, uint32_t seed) {
  std::mt19937 rng{seed};
  Blob out(size);
  for (uint8_t& b : out) {
    b = uint8_t(rng());
  }
  return out;
}

// Changes to the plaintext before it is encrypted, for the refusal cases.
struct Breakage {
  std::function<void(Blob& header)> header;
  std::function<void(Blob& romfs)> romfs;
};

constexpr size_t kRomfsInSection = 0x200;

// a.bin in the root, dir/b.bin under it; b.bin spans more than one of the
// reader's 1 MB decryption pieces.
Blob MakeImage(const Blob& a, const Blob& b, const Breakage& breakage = {}) {
  // --- RomFS ------------------------------------------------------------------
  Blob dirs(24 + 28, 0);
  Put32(dirs, 0, 0);       // root: parent
  Put32(dirs, 4, kEmpty);  // sibling
  Put32(dirs, 8, 24);      // child: "dir"
  Put32(dirs, 12, 0);      // first file: a.bin
  Put32(dirs, 16, kEmpty);
  Put32(dirs, 20, 0);
  Put32(dirs, 24, 0);
  Put32(dirs, 28, kEmpty);
  Put32(dirs, 32, kEmpty);
  Put32(dirs, 36, 40);     // first file: b.bin
  Put32(dirs, 40, kEmpty);
  Put32(dirs, 44, 3);
  std::memcpy(dirs.data() + 48, "dir", 3);
  const size_t bData = Align(a.size(), 16);
  Blob files(80, 0);
  const auto file = [&](size_t at, uint32_t parent, uint64_t offset, uint64_t size, const char* name) {
    Put32(files, at, parent);
    Put32(files, at + 4, kEmpty);
    Put64(files, at + 8, offset);
    Put64(files, at + 16, size);
    Put32(files, at + 24, kEmpty);
    Put32(files, at + 28, uint32_t(std::strlen(name)));
    std::memcpy(files.data() + at + 32, name, std::strlen(name));
  };
  file(0, 0, 0, a.size(), "a.bin");
  file(40, 24, bData, b.size(), "b.bin");
  const size_t dirOffset = 0x50, fileOffset = dirOffset + dirs.size();
  const size_t dataOffset = Align(fileOffset + files.size(), 16);
  Blob romfs(dataOffset + bData + b.size(), 0);
  Put64(romfs, 0, 0x50);
  Put64(romfs, 0x18, dirOffset);
  Put64(romfs, 0x20, dirs.size());
  Put64(romfs, 0x38, fileOffset);
  Put64(romfs, 0x40, files.size());
  Put64(romfs, 0x48, dataOffset);
  std::memcpy(romfs.data() + dirOffset, dirs.data(), dirs.size());
  std::memcpy(romfs.data() + fileOffset, files.data(), files.size());
  std::memcpy(romfs.data() + dataOffset, a.data(), a.size());
  std::memcpy(romfs.data() + dataOffset + bData, b.data(), b.size());
  if (breakage.romfs) {
    breakage.romfs(romfs);
  }

  // --- NCA --------------------------------------------------------------------
  const size_t sectionStart = 0xC00;
  Blob section(Align(kRomfsInSection + romfs.size(), 0x200), 0);
  std::memcpy(section.data() + kRomfsInSection, romfs.data(), romfs.size());
  Ctr(sectionStart, section.data(), section.size());

  Blob header(0xC00, 0);
  std::memcpy(header.data() + 0x200, "NCA3", 4);
  header[0x205] = 0; // program
  header[0x206] = 0; // master key generation 0
  header[0x207] = 0; // application key area key
  Put32(header, 0x240, uint32_t(sectionStart / 0x200));
  Put32(header, 0x244, uint32_t((sectionStart + section.size()) / 0x200));
  uint8_t* fs = header.data() + 0x400;
  fs[2] = 0; // partition
  fs[3] = 3; // RomFS
  fs[4] = 3; // AES-CTR
  std::memcpy(fs + 8, "IVFC", 4);
  Put32(header, 0x400 + 8 + 0xC, 2); // two levels: the last is the RomFS
  Put64(header, 0x400 + 8 + 0x10, kRomfsInSection);
  std::memcpy(fs + 0x140, kNonce, 8);
  EncryptEcb(kKeyAreaKey, kContentKey, header.data() + 0x300 + 2 * 16);
  if (breakage.header) {
    breakage.header(header);
  }
  EncryptHeader(header.data(), header.size());

  // --- PFS0 -------------------------------------------------------------------
  const char name[] = "program.nca";
  const size_t strings = 0x10;
  Blob image(16 + 24 + strings, 0);
  std::memcpy(image.data(), "PFS0", 4);
  Put32(image, 4, 1);
  Put32(image, 8, uint32_t(strings));
  Put64(image, 16, 0);
  Put64(image, 24, header.size() + section.size());
  Put32(image, 32, 0);
  std::memcpy(image.data() + 40, name, sizeof name);
  image.insert(image.end(), header.begin(), header.end());
  image.insert(image.end(), section.begin(), section.end());
  return image;
}

std::filesystem::path Temp(const char* name) { return std::filesystem::temp_directory_path() / name; }

void Write(const std::filesystem::path& path, const void* data, size_t size) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(static_cast<const char*>(data), std::streamsize(size));
}

std::string Keys(bool withKeyArea, const uint8_t* headerKey = kHeaderKey) {
  std::string text = "header_key = " + Hex(headerKey, 32) + "\n";
  if (withKeyArea) {
    text += "key_area_key_application_00 = " + Hex(kKeyAreaKey, 16) + "\n";
  }
  return text;
}

bool OpenImage(const Blob& image, const std::string& keys, Nsp& nsp, std::string& error) {
  Write(Temp("port_nsp_test.nsp"), image.data(), image.size());
  Write(Temp("port_nsp_test.keys"), keys.data(), keys.size());
  return nsp.Open(Temp("port_nsp_test.nsp").string(), Temp("port_nsp_test.keys").string(), error);
}

bool Has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }
} // namespace

int main() {
  const Blob a = Content(1000, 1);
  const Blob b = Content((1u << 20) + 5000, 2);
  const Blob image = MakeImage(a, b);
  std::string error;
  {
    Nsp nsp;
    CHECK_MSG(OpenImage(image, Keys(true), nsp, error), "synthetic image opens");
    if (!error.empty() && !nsp.IsOpen()) {
      std::fprintf(stderr, "  %s\n", error.c_str());
    }
    CHECK_MSG(nsp.Files().size() == 2, "two files");
    const RomfsFile* fileA = nsp.Find("a.bin");
    const RomfsFile* fileB = nsp.Find("dir/b.bin");
    CHECK_MSG(fileA != nullptr && fileB != nullptr, "files are found by path");
    if (fileA != nullptr && fileB != nullptr) {
      Blob out(a.size());
      CHECK_MSG(nsp.Read(*fileA, 0, out.data(), out.size(), error) && out == a, "a.bin decrypts");
      // Unaligned, and across the 1 MB pieces the reader decrypts in.
      const size_t from = 77, size = b.size() - 100;
      out.assign(size, 0);
      CHECK_MSG(nsp.Read(*fileB, from, out.data(), size, error) &&
                    std::equal(out.begin(), out.end(), b.begin() + long(from)),
                "b.bin decrypts at an unaligned offset across pieces");
      CHECK_MSG(!nsp.Read(*fileB, b.size() - 1, out.data(), 2, error), "a read past a file's end is refused");
    }
  }
  {
    Nsp nsp;
    uint8_t wrong[32];
    std::memcpy(wrong, kHeaderKey, 32);
    wrong[0] ^= 1;
    CHECK_MSG(!OpenImage(image, Keys(true, wrong), nsp, error) && Has(error, "wrong keys"), "a wrong header key says so");
    CHECK_MSG(!OpenImage(image, Keys(false), nsp, error) && Has(error, "key_area_key_application_00"),
              "a missing key area key is named");
    const std::string titleKeys = "0100a1b2c3d4e5f60000000000000000 = 00112233445566778899aabbccddeeff\n";
    CHECK_MSG(!OpenImage(image, titleKeys, nsp, error) && Has(error, "title.keys"), "a title.keys file is recognised");
  }
  {
    Nsp nsp;
    Breakage outside;
    outside.romfs = [](Blob& romfs) { Put64(romfs, 0x50 + 52 + 40 + 8, uint64_t(1) << 40); };
    CHECK_MSG(!OpenImage(MakeImage(a, b, outside), Keys(true), nsp, error) && Has(error, "outside"),
              "a file outside the RomFS is refused");
    Breakage wrap;
    wrap.romfs = [](Blob& romfs) { Put64(romfs, 0x50 + 52 + 40 + 8, ~uint64_t(0) - 10); };
    CHECK_MSG(!OpenImage(MakeImage(a, b, wrap), Keys(true), nsp, error), "a wrapping file offset is refused");
    Breakage level;
    level.header = [](Blob& header) { Put64(header, 0x400 + 8 + 0x10, uint64_t(1) << 30); };
    CHECK_MSG(!OpenImage(MakeImage(a, b, level), Keys(true), nsp, error) && Has(error, "outside its section"),
              "a RomFS level outside its section is refused");
    Breakage tables;
    tables.romfs = [](Blob& romfs) { Put64(romfs, 0x38, uint64_t(1) << 33); };
    CHECK_MSG(!OpenImage(MakeImage(a, b, tables), Keys(true), nsp, error), "tables outside the RomFS are refused");
  }
  {
    // Random bytes changed anywhere: the header, the tables, the data.
    std::mt19937 rng{3};
    const Blob small = MakeImage(a, Content(3000, 4));
    for (int round = 0; round < 300; ++round) {
      Blob broken = small;
      for (int i = 0; i < 4; ++i) {
        broken[rng() % broken.size()] = uint8_t(rng());
      }
      Nsp nsp;
      if (OpenImage(broken, Keys(true), nsp, error)) {
        for (const RomfsFile& file : nsp.Files()) {
          Blob out(size_t(std::min<uint64_t>(file.size, 1u << 16)));
          nsp.Read(file, 0, out.data(), out.size(), error);
        }
      }
    }
  }
  std::error_code ec;
  std::filesystem::remove(Temp("port_nsp_test.nsp"), ec);
  std::filesystem::remove(Temp("port_nsp_test.keys"), ec);
  if (port_test::Failures() == 0) {
    std::printf("port_remastered_nsp: ok\n");
  }
  return PORT_TEST_RESULT();
}
