// The gallery's JPEG round trip (see port_gallery.h).

#include "port_gallery.h"

#include "port_remastered_jpeg.h"

#include <algorithm>
#include <cmath>

// aurora's THP decoder (extern/aurora/lib/dolphin/thp/THPDec.cpp). Declared here with a const input; the game's
// own header takes a plain pointer and drags the whole THP player along.
extern "C" int THPVideoDecodeBounded(const void* file, size_t size, void* tileY, void* tileU, void* tileV, uint32_t width,
                                     uint32_t height);

namespace PortGallery {
namespace {

uint8_t Clamp8(int value) { return uint8_t(std::clamp(value, 0, 255)); }

// A plane the decoder wrote in GX I8 tiles (8x4 texels), read back by texel.
struct TiledPlane {
  std::vector<uint8_t> data;
  int width = 0;
  int height = 0;

  void Allocate(int w, int h) {
    width = w;
    height = h;
    data.assign(size_t((w + 7) / 8) * ((h + 3) / 4) * 32, 0);
  }

  // Coordinates are clamped, so a chroma sample just past the edge repeats the last one.
  int At(int x, int y) const {
    x = std::clamp(x, 0, width - 1);
    y = std::clamp(y, 0, height - 1);
    return data[(size_t(y / 4) * ((width + 7) / 8) + x / 8) * 32 + (y & 3) * 8 + (x & 7)];
  }
};

// The picture's size from its baseline frame header, and where its scan data starts. False when either is missing.
bool ReadHeader(const uint8_t* data, size_t size, int& width, int& height, size_t& scanStart) {
  if (size < 4 || data[0] != 0xFF || data[1] != 0xD8) {
    return false;
  }
  bool haveFrame = false;
  size_t at = 2;
  while (at + 4 <= size) {
    if (data[at] != 0xFF) {
      return false;
    }
    const uint8_t marker = data[at + 1];
    if (marker == 0xFF) {
      ++at;
      continue;
    }
    const size_t length = size_t(data[at + 2]) << 8 | data[at + 3];
    if (marker == 0xD9 || length < 2 || at + 2 + length > size) {
      return false;
    }
    if (marker == 0xC0) {
      // Length, precision, height, width, component count, then the components with their sampling.
      if (length < 17 || data[at + 4] != 8 || data[at + 9] != 3 || data[at + 11] != 0x22 || data[at + 14] != 0x11 ||
          data[at + 17] != 0x11) {
        return false;
      }
      height = data[at + 5] << 8 | data[at + 6];
      width = data[at + 7] << 8 | data[at + 8];
      haveFrame = width > 0 && height > 0;
    } else if (marker == 0xDA) {
      scanStart = at + 2 + length;
      return haveFrame;
    }
    at += 2 + length;
  }
  return false;
}

} // namespace

bool EncodeGalleryJpeg(const uint8_t* rgba, int width, int height, std::vector<uint8_t>& jpeg) {
  if (rgba == nullptr || width <= 0 || height <= 0) {
    return false;
  }
  const double scale = std::min({1.0, double(kMaxWidth) / width, double(kMaxHeight) / height});
  const auto even = [scale](int source) { return std::max(2, int(std::lround(source * scale / 2.0)) * 2); };
  PortRemastered::MovieFormat format;
  format.width = even(width);
  format.height = even(height);

  // BT.601 over the full range, the chroma as the mean of each 2x2 (the edge's repeated).
  const int chromaWidth = (width + 1) / 2;
  const int chromaHeight = (height + 1) / 2;
  std::vector<uint8_t> luma(size_t(width) * height);
  std::vector<uint8_t> cb(size_t(chromaWidth) * chromaHeight);
  std::vector<uint8_t> cr(cb.size());
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const uint8_t* p = rgba + (size_t(y) * width + x) * 4;
      luma[size_t(y) * width + x] = Clamp8(int(std::lround(0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2])));
    }
  }
  for (int y = 0; y < chromaHeight; ++y) {
    for (int x = 0; x < chromaWidth; ++x) {
      int sum[3] = {0, 0, 0};
      for (int i = 0; i < 4; ++i) {
        const int sx = std::min(x * 2 + i % 2, width - 1);
        const int sy = std::min(y * 2 + i / 2, height - 1);
        const uint8_t* p = rgba + (size_t(sy) * width + sx) * 4;
        sum[0] += p[0];
        sum[1] += p[1];
        sum[2] += p[2];
      }
      const double r = sum[0] / 4.0;
      const double g = sum[1] / 4.0;
      const double b = sum[2] / 4.0;
      cb[size_t(y) * chromaWidth + x] = Clamp8(int(std::lround(128.0 - 0.168736 * r - 0.331264 * g + 0.5 * b)));
      cr[size_t(y) * chromaWidth + x] = Clamp8(int(std::lround(128.0 + 0.5 * r - 0.418688 * g - 0.081312 * b)));
    }
  }

  PortRemastered::Picture picture;
  picture.y = luma.data();
  picture.cb = cb.data();
  picture.cr = cr.data();
  picture.stride = width;
  picture.chromaStride = chromaWidth;
  picture.width = width;
  picture.height = height;
  picture.bt709 = false;
  picture.fullRange = true;
  PortRemastered::MovieEncoder encoder(format, 90);
  encoder.Encode(picture, jpeg);
  return !jpeg.empty();
}

bool DecodeGalleryJpeg(const uint8_t* data, size_t size, int& width, int& height, std::vector<uint8_t>& rgba) {
  size_t scanStart = 0;
  if (data == nullptr || !ReadHeader(data, size, width, height, scanStart)) {
    return false;
  }
  // The decoder is the console's: it wants the scan without its FF 00 stuffing (as THP stores it), and trusts the
  // data to end in a marker, so a truncated file gets one, and zeros ahead of it.
  std::vector<uint8_t> padded(data, data + scanStart);
  padded.reserve(size + 66);
  for (size_t i = scanStart; i < size; ++i) {
    padded.push_back(data[i]);
    if (data[i] == 0xFF && i + 1 < size && data[i + 1] == 0x00) {
      ++i;
    }
  }
  padded.insert(padded.end(), 64, 0);
  padded.push_back(0xFF);
  padded.push_back(0xD9);

  TiledPlane planes[3];
  planes[0].Allocate(width, height);
  planes[1].Allocate((width + 1) / 2, (height + 1) / 2);
  planes[2].Allocate((width + 1) / 2, (height + 1) / 2);
  if (THPVideoDecodeBounded(padded.data(), padded.size(), planes[0].data.data(), planes[1].data.data(),
                            planes[2].data.data(), uint32_t(width), uint32_t(height)) != 0) {
    return false;
  }

  rgba.resize(size_t(width) * height * 4);
  for (int y = 0; y < height; ++y) {
    // The chroma sits between the luma rows and columns: blend the two nearest samples, three to one.
    const int y0 = y / 2;
    const int y1 = y % 2 == 0 ? y0 - 1 : y0 + 1;
    for (int x = 0; x < width; ++x) {
      const int x0 = x / 2;
      const int x1 = x % 2 == 0 ? x0 - 1 : x0 + 1;
      const auto sample = [&](const TiledPlane& plane) {
        return (9 * plane.At(x0, y0) + 3 * plane.At(x1, y0) + 3 * plane.At(x0, y1) + plane.At(x1, y1) + 8) / 16 - 128;
      };
      const int luma = planes[0].At(x, y);
      const int b = sample(planes[1]);
      const int r = sample(planes[2]);
      uint8_t* out = rgba.data() + (size_t(y) * width + x) * 4;
      out[0] = Clamp8(int(std::lround(luma + 1.402 * r)));
      out[1] = Clamp8(int(std::lround(luma - 0.344136 * b - 0.714136 * r)));
      out[2] = Clamp8(int(std::lround(luma + 1.772 * b)));
      out[3] = 255;
    }
  }
  return true;
}

} // namespace PortGallery
