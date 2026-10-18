#include "port_remastered_jpeg.h"
#include "port_remastered_movie.h"
#include "port_test.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <vector>

namespace {

uint32_t BE32(const std::string& data, size_t at) {
  return uint32_t(uint8_t(data[at])) << 24 | uint32_t(uint8_t(data[at + 1])) << 16 |
         uint32_t(uint8_t(data[at + 2])) << 8 | uint32_t(uint8_t(data[at + 3]));
}

// A picture with one table segment, a scan header and `scan` as its data.
std::vector<uint8_t> Jpeg(const std::vector<uint8_t>& scan) {
  std::vector<uint8_t> jpeg = {0xFF, 0xD8, 0xFF, 0xDB, 0x00, 0x04, 0xFF, 0xD9, 0xFF, 0xDA, 0x00, 0x03, 0x01};
  jpeg.insert(jpeg.end(), scan.begin(), scan.end());
  jpeg.push_back(0xFF);
  jpeg.push_back(0xD9);
  return jpeg;
}

void TestSplitter() {
  // The first table segment holds FF D9 as data; the scan has a stuffed FF.
  const std::vector<uint8_t> first = Jpeg({0x11, 0xFF, 0x00, 0x22});
  const std::vector<uint8_t> second = Jpeg({0x33});
  std::vector<uint8_t> stream = first;
  stream.insert(stream.end(), second.begin(), second.end());

  // Whatever the chunking, the same two pictures come out.
  for (size_t chunk = 1; chunk <= stream.size(); ++chunk) {
    PortRemastered::JpegSplitter splitter;
    std::vector<std::vector<uint8_t>> frames;
    bool ok = true;
    for (size_t at = 0; at < stream.size() && ok; at += chunk) {
      const size_t size = stream.size() - at < chunk ? stream.size() - at : chunk;
      ok = splitter.Feed(stream.data() + at, size, [&](const std::vector<uint8_t>& frame) {
        frames.push_back(frame);
        return true;
      });
    }
    CHECK_MSG(ok && splitter.Idle(), "a stream of two pictures parses");
    CHECK_MSG(frames.size() == 2, "two pictures");
    if (frames.size() == 2) {
      const std::vector<uint8_t> unstuffed = {0xFF, 0xD8, 0xFF, 0xDB, 0x00, 0x04, 0xFF, 0xD9, 0xFF,
                                              0xDA, 0x00, 0x03, 0x01, 0x11, 0xFF, 0x22, 0xFF, 0xD9};
      CHECK_MSG(frames[0] == unstuffed, "the scan loses its stuffing, the header keeps its bytes");
      CHECK_MSG(frames[1] == second, "a scan without stuffing is unchanged");
    }
  }

  PortRemastered::JpegSplitter splitter;
  const auto ignore = [](const std::vector<uint8_t>&) { return true; };
  CHECK_MSG(splitter.Feed(first.data(), first.size() - 1, ignore) && !splitter.Idle(), "a cut picture is not idle");
  const uint8_t text[] = {'n', 'o', 'p', 'e'};
  PortRemastered::JpegSplitter other;
  CHECK_MSG(!other.Feed(text, sizeof(text), ignore), "text is refused");
  PortRemastered::JpegSplitter stopping;
  CHECK_MSG(!stopping.Feed(stream.data(), stream.size(), [](const std::vector<uint8_t>&) { return false; }),
            "a failing sink stops the stream");

  // A scan that never ends is refused once it outgrows any picture, not held.
  PortRemastered::JpegSplitter endless;
  const std::vector<uint8_t> head(first.begin(), first.begin() + 13);
  bool fed = endless.Feed(head.data(), head.size(), ignore);
  const std::vector<uint8_t> filler(1 << 20, 0x55);
  size_t total = head.size();
  while (fed && total <= PortRemastered::JpegSplitter::kMaxPicture) {
    fed = endless.Feed(filler.data(), filler.size(), ignore);
    total += filler.size();
  }
  const size_t limit = PortRemastered::JpegSplitter::kMaxPicture;
  CHECK_MSG(!fed && total > limit && total <= limit + filler.size(), "a picture without an end is refused past the limit");
  // Many pictures in one call are not one picture, however many bytes they add up to.
  std::vector<uint8_t> many;
  const std::vector<uint8_t> big = Jpeg(std::vector<uint8_t>(1 << 20, 0x55));
  while (many.size() <= PortRemastered::JpegSplitter::kMaxPicture) {
    many.insert(many.end(), big.begin(), big.end());
  }
  PortRemastered::JpegSplitter bulk;
  size_t pictures = 0;
  CHECK_MSG(bulk.Feed(many.data(), many.size(), [&](const std::vector<uint8_t>& frame) {
              pictures += frame == big ? 1 : 0;
              return true;
            }) && bulk.Idle(),
            "pictures past the limit in one call parse");
  CHECK_MSG(pictures == many.size() / big.size(), "every picture of a large call comes out");
  // A picture split across calls, after one that ended in the same call.
  PortRemastered::JpegSplitter carried;
  std::vector<uint8_t> partial = second;
  partial.insert(partial.end(), first.begin(), first.begin() + 15);
  std::vector<std::vector<uint8_t>> carriedFrames;
  const auto keep = [&](const std::vector<uint8_t>& frame) {
    carriedFrames.push_back(frame);
    return true;
  };
  bool carriedOk = carried.Feed(partial.data(), partial.size(), keep) && !carried.Idle();
  carriedOk = carriedOk && carried.Feed(first.data() + 15, first.size() - 15, keep) && carried.Idle();
  CHECK_MSG(carriedOk && carriedFrames.size() == 2 && carriedFrames[0] == second,
            "a picture carried over a call comes out whole");
}

void TestWriter() {
  std::stringstream out;
  PortRemastered::ThpWriter writer(out, 1600, 900, 30.f);
  // 12 + 20 fills a 32 byte frame exactly, 12 + 21 needs a second block, then one block again.
  const size_t sizes[] = {20, 21, 5};
  for (const size_t size : sizes) {
    writer.Add(std::vector<uint8_t>(size, uint8_t(size)));
  }
  CHECK_MSG(writer.Finish() && writer.Frames() == 3, "three frames written");
  const std::string data = out.str();
  CHECK_MSG(data.size() == 0x4C + 32 + 64 + 32, "frames are padded to 32 bytes");
  CHECK_MSG(std::memcmp(data.data(), "THP\0", 4) == 0 && BE32(data, 0x04) == 0x00010000, "magic and version");
  CHECK_MSG(BE32(data, 0x08) == 64, "largest frame");
  CHECK_MSG(BE32(data, 0x0C) == 0, "no audio");
  CHECK_MSG(BE32(data, 0x10) == 0x41F00000, "30 frames a second");
  CHECK_MSG(BE32(data, 0x14) == 3, "frame count");
  CHECK_MSG(BE32(data, 0x18) == 32, "first frame size");
  CHECK_MSG(BE32(data, 0x1C) == 128, "data size");
  CHECK_MSG(BE32(data, 0x20) == 0x30 && BE32(data, 0x24) == 0 && BE32(data, 0x28) == 0x4C, "offsets");
  CHECK_MSG(BE32(data, 0x2C) == 0x4C + 96, "last frame offset");
  CHECK_MSG(BE32(data, 0x30) == 1 && uint8_t(data[0x34]) == 0 && uint8_t(data[0x35]) == 0xFF &&
                uint8_t(data[0x43]) == 0xFF,
            "one video component");
  CHECK_MSG(BE32(data, 0x44) == 1600 && BE32(data, 0x48) == 900, "picture size");
  // next, previous (both wrapping around), picture size
  CHECK_MSG(BE32(data, 0x4C) == 64 && BE32(data, 0x50) == 32 && BE32(data, 0x54) == 20, "first frame");
  CHECK_MSG(BE32(data, 0x6C) == 32 && BE32(data, 0x70) == 32 && BE32(data, 0x74) == 21, "second frame");
  CHECK_MSG(BE32(data, 0xAC) == 32 && BE32(data, 0xB0) == 64 && BE32(data, 0xB4) == 5, "third frame");
  CHECK_MSG(uint8_t(data[0x78]) == 21 && uint8_t(data[0x78 + 20]) == 21 && data[0x78 + 21] == 0, "picture, then padding");

  std::stringstream empty;
  PortRemastered::ThpWriter none(empty, 16, 16, 30.f);
  CHECK_MSG(!none.Finish(), "a movie needs a frame");
}

void TestFormat() {
  PortRemastered::MovieFormat format;
  CHECK_MSG(format.width == 1600 && format.height == 900 && format.fps == 30, "default format");
  CHECK_MSG(PortRemastered::ParseMovieFormat("1280x720@60", format) && format.width == 1280 && format.height == 720 &&
                format.fps == 60,
            "size and rate");
  CHECK_MSG(PortRemastered::ParseMovieFormat("640x480", format) && format.width == 640 && format.fps == 60, "size alone");
  CHECK_MSG(PortRemastered::ParseMovieFormat("@30", format) && format.width == 640 && format.fps == 30, "rate alone");
  CHECK_MSG(!PortRemastered::ParseMovieFormat("", format), "nothing");
  CHECK_MSG(!PortRemastered::ParseMovieFormat("641x480", format), "odd width");
  CHECK_MSG(!PortRemastered::ParseMovieFormat("640x480@0", format), "no rate");
  CHECK_MSG(!PortRemastered::ParseMovieFormat("640@30", format), "no height");
  CHECK_MSG(format.width == 640 && format.height == 480 && format.fps == 30, "a bad format changes nothing");
}

void TestStream() {
  std::vector<uint8_t> asset(32 + 16, 0);
  std::memcpy(asset.data(), "RFRM", 4);
  asset[4] = 12; // the form's size, little endian, short of the bytes after it
  std::memcpy(asset.data() + 20, "FMV0", 4);
  size_t offset = 0;
  size_t length = 0;
  CHECK_MSG(PortRemastered::MovieStream(asset.data(), asset.size(), offset, length) && offset == 32 && length == 12,
            "the stream is the form's body");
  asset[4] = 17;
  CHECK_MSG(!PortRemastered::MovieStream(asset.data(), asset.size(), offset, length), "a form longer than the asset");
  asset[4] = 12;
  asset[23] = '1';
  CHECK_MSG(!PortRemastered::MovieStream(asset.data(), asset.size(), offset, length), "another asset type");
}

void TestTable() {
  int names = 0;
  for (const PortRemastered::Movie& movie : PortRemastered::Movies()) {
    CHECK_MSG(std::strlen(movie.id) == 36 && !movie.names.empty(), "an id and a name");
    names += int(movie.names.size());
  }
  CHECK_MSG(names == 16, "sixteen disc movies replaced");
}

void TestPacer() {
  // 60 pictures a second played at 30: every other one.
  PortRemastered::FramePacer half(30);
  int kept = 0;
  bool alternate = true;
  for (int i = 0; i < 120; ++i) {
    // The times a container gives are rounded down to its clock.
    const int copies = half.Copies(int64_t(i) * 1000000 / 60);
    alternate = alternate && copies == (i % 2 == 0 ? 1 : 0);
    kept += copies;
  }
  CHECK_MSG(alternate && kept == 60, "half the rate keeps every other picture");
  // 24 played at 30 repeats one in four.
  PortRemastered::FramePacer faster(30);
  kept = 0;
  for (int i = 0; i < 24; ++i) {
    kept += faster.Copies(int64_t(i) * 1000000 / 24);
  }
  CHECK_MSG(kept == 29, "a slower stream has pictures repeated");
}

// A picture of one colour, with its chroma as planes or interleaved.
std::vector<uint8_t> Encode(PortRemastered::MovieEncoder& encoder, int width, int height, bool interleaved,
                            uint8_t luma) {
  const int chromaWidth = (width + 1) / 2;
  const int chromaHeight = (height + 1) / 2;
  std::vector<uint8_t> y(size_t(width) * height);
  for (size_t i = 0; i < y.size(); ++i) {
    y[i] = uint8_t(luma + i % 7);
  }
  std::vector<uint8_t> chroma(size_t(chromaWidth) * chromaHeight * 2);
  PortRemastered::Picture picture;
  picture.width = width;
  picture.height = height;
  picture.stride = width;
  picture.y = y.data();
  for (int i = 0; i < chromaWidth * chromaHeight; ++i) {
    const uint8_t cb = uint8_t(100 + i % 5);
    const uint8_t cr = uint8_t(150 + i % 3);
    if (interleaved) {
      chroma[size_t(i) * 2] = cb;
      chroma[size_t(i) * 2 + 1] = cr;
    } else {
      chroma[size_t(i)] = cb;
      chroma[size_t(chromaWidth) * chromaHeight + i] = cr;
    }
  }
  picture.cb = chroma.data();
  picture.cr = interleaved ? chroma.data() + 1 : chroma.data() + size_t(chromaWidth) * chromaHeight;
  picture.chromaStride = interleaved ? chromaWidth * 2 : chromaWidth;
  picture.chromaStep = interleaved ? 2 : 1;
  std::vector<uint8_t> jpeg;
  encoder.Encode(picture, jpeg);
  return jpeg;
}

void TestEncoder() {
  PortRemastered::MovieFormat format;
  format.width = 40;
  format.height = 26;
  PortRemastered::MovieEncoder encoder(format);
  const std::vector<uint8_t> jpeg = Encode(encoder, 40, 26, false, 90);
  CHECK_MSG(jpeg.size() > 600 && jpeg[0] == 0xFF && jpeg[1] == 0xD8 && jpeg[jpeg.size() - 2] == 0xFF &&
                jpeg[jpeg.size() - 1] == 0xD9,
            "a picture from start marker to end marker");
  // Tables (2 + 2 + 130), then the frame header: 8 bits, 26 by 40, 4:2:0.
  const std::vector<uint8_t> frame = {0xFF, 0xC0, 0x00, 0x11, 8, 0, 26, 0, 40, 3, 1, 0x22, 0, 2, 0x11, 1, 3, 0x11, 1};
  CHECK_MSG(jpeg.size() > 136 + frame.size() && std::equal(frame.begin(), frame.end(), jpeg.begin() + 136),
            "the frame header says what the game's decoder takes");
  CHECK_MSG(Encode(encoder, 40, 26, true, 90) == jpeg, "interleaved chroma reads the same as planes");
  CHECK_MSG(Encode(encoder, 40, 26, false, 160) != jpeg, "a brighter picture is another picture");
  // The splitter that reads ffmpeg's output takes these too.
  PortRemastered::JpegSplitter splitter;
  int pictures = 0;
  CHECK_MSG(splitter.Feed(jpeg.data(), jpeg.size(),
                          [&](const std::vector<uint8_t>& picture) {
                            ++pictures;
                            return picture.size() <= jpeg.size();
                          }) &&
                splitter.Idle() && pictures == 1,
            "the splitter takes the encoder's picture");
  // Another size than the stream's: the header carries the format's.
  const std::vector<uint8_t> shrunk = Encode(encoder, 80, 50, false, 90);
  CHECK_MSG(shrunk.size() > 136 + frame.size() && std::equal(frame.begin(), frame.end(), shrunk.begin() + 136),
            "a larger stream is brought to the format's size");
  const std::vector<uint8_t> grown = Encode(encoder, 18, 14, true, 90);
  CHECK_MSG(grown.size() > 136 + frame.size() && std::equal(frame.begin(), frame.end(), grown.begin() + 136),
            "and a smaller one too");
}
} // namespace

int main() {
  TestPacer();
  TestEncoder();
  TestSplitter();
  TestWriter();
  TestFormat();
  TestStream();
  TestTable();
  if (port_test::Failures() == 0) {
    std::printf("port_remastered_movie: ok\n");
  }
  return PORT_TEST_RESULT();
}
