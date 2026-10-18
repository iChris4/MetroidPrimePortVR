#include "port_gallery.h"
#include "port_test.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

// A gradient with a few coloured blocks, which is what a picture of flat areas and edges asks of the codec.
std::vector<uint8_t> Picture(int width, int height) {
  std::vector<uint8_t> rgba(size_t(width) * height * 4);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      uint8_t* p = &rgba[(size_t(y) * width + x) * 4];
      p[0] = uint8_t(x * 255 / width);
      p[1] = uint8_t(y * 255 / height);
      p[2] = uint8_t(128 + (x + y) % 32);
      p[3] = 255;
      const bool block = x > width / 8 && x < width / 4 && y > height / 8 && y < height / 3;
      const bool block2 = x > width / 2 && x < width * 5 / 8 && y > height / 2 && y < height * 3 / 4;
      if (block) {
        p[0] = 220;
        p[1] = 30;
        p[2] = 30;
      } else if (block2) {
        p[0] = 20;
        p[1] = 40;
        p[2] = 230;
      }
    }
  }
  return rgba;
}

void RoundTrip(int width, int height, int expectWidth, int expectHeight, const char* what) {
  const std::vector<uint8_t> source = Picture(width, height);
  std::vector<uint8_t> jpeg;
  CHECK_MSG(PortGallery::EncodeGalleryJpeg(source.data(), width, height, jpeg), what);
  int w = 0;
  int h = 0;
  std::vector<uint8_t> out;
  CHECK_MSG(PortGallery::DecodeGalleryJpeg(jpeg.data(), jpeg.size(), w, h, out), "decodes");
  CHECK_MSG(w == expectWidth && h == expectHeight, what);
  CHECK_MSG(w % 2 == 0 && h % 2 == 0, "even size");
  CHECK_MSG(w <= PortGallery::kMaxWidth && h <= PortGallery::kMaxHeight, "within the limit");
  CHECK_MSG(std::fabs(double(w) / h / (double(width) / height) - 1.0) < 0.01, "aspect kept");
  if (w != width || h != height) {
    return; // Scaled: only the size is checked.
  }
  double error[3] = {0, 0, 0};
  for (size_t i = 0; i < size_t(w) * h; ++i) {
    for (int c = 0; c < 3; ++c) {
      error[c] += std::abs(int(out[i * 4 + c]) - int(source[i * 4 + c]));
    }
    CHECK_MSG(out[i * 4 + 3] == 255, "alpha 255");
    if (port_test::Failures() > 20) {
      return;
    }
  }
  for (int c = 0; c < 3; ++c) {
    const double mean = error[c] / (double(w) * h);
    std::printf("%s: channel %d mean error %.2f\n", what, c, mean);
    CHECK_MSG(mean < 6.0, "round trip error");
  }
}

void TestBadData() {
  int w = 0;
  int h = 0;
  std::vector<uint8_t> out;
  const uint8_t junk[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
  CHECK_MSG(!PortGallery::DecodeGalleryJpeg(junk, sizeof(junk), w, h, out), "junk refused");
  CHECK_MSG(!PortGallery::DecodeGalleryJpeg(junk, 0, w, h, out), "empty refused");
  CHECK_MSG(!PortGallery::DecodeGalleryJpeg(nullptr, 0, w, h, out), "null refused");
  const std::vector<uint8_t> source = Picture(64, 64);
  std::vector<uint8_t> jpeg;
  PortGallery::EncodeGalleryJpeg(source.data(), 64, 64, jpeg);
  CHECK_MSG(!PortGallery::DecodeGalleryJpeg(jpeg.data(), 20, w, h, out), "truncated header refused");
  // Cut inside the scan: whatever it does, it must return rather than read on.
  PortGallery::DecodeGalleryJpeg(jpeg.data(), jpeg.size() / 2, w, h, out);
  CHECK_MSG(!PortGallery::EncodeGalleryJpeg(source.data(), 0, 64, jpeg), "zero width refused");
}
} // namespace

int main() {
  RoundTrip(640, 360, 640, 360, "640x360");
  RoundTrip(2401, 1353, 1916, 1080, "2401x1353");
  RoundTrip(301, 201, 302, 202, "301x201");
  TestBadData();
  if (port_test::Failures() == 0) {
    std::puts("port_gallery_tests: ok");
  }
  return PORT_TEST_RESULT();
}
