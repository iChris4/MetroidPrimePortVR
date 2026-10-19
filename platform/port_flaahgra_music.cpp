// The checks on a Flaahgra DSP file (port_flaahgra_music.h); no disc access, so the tests build it alone.

#include "port_flaahgra_music.h"
#include "port_bytes.h"

namespace PortFlaahgraMusic {

const char* CheckChannel(const uint8_t* data, size_t size, uint64_t fileSize, uint32_t& samples) {
  samples = 0;
  if (data == nullptr || size < kDspHeaderSize || fileSize < kDspHeaderSize) {
    return "is too short to be a DSP file";
  }
  // GameCube DSP ADPCM header: sample count, nibble count, rate, loop flag (u16),
  // format (u16, 0 = ADPCM), then the loop start, loop end and current nibble addresses.
  const uint32_t sampleCount = port::ReadBE32(data);
  const uint32_t nibbles = port::ReadBE32(data + 4);
  const uint32_t rate = port::ReadBE32(data + 8);
  const uint32_t loopFlag = (uint32_t(data[0xC]) << 8) | data[0xD];
  const uint32_t format = (uint32_t(data[0xE]) << 8) | data[0xF];
  const uint32_t loopStart = port::ReadBE32(data + 0x10);
  const uint32_t loopEnd = port::ReadBE32(data + 0x14);
  samples = sampleCount;
  if (format != 0) {
    return "is not DSP ADPCM";
  }
  if (rate != kSampleRate) {
    return "is not 32000 Hz";
  }
  if (loopFlag != 1) {
    return "does not loop";
  }
  if (sampleCount == 0 || nibbles == 0 || loopEnd >= nibbles || loopStart > loopEnd ||
      sampleCount > nibbles) {
    return "has an inconsistent DSP header";
  }
  if (fileSize != kDspHeaderSize + (uint64_t(nibbles) + 1) / 2) {
    return "is not as long as its DSP header says";
  }
  if (sampleCount <= kTruncatedSamples) {
    return "is the shortened 36 s theme of the NTSC discs, not the full one";
  }
  return nullptr;
}

const char* CheckPair(uint32_t leftSamples, uint32_t rightSamples) {
  return leftSamples == rightSamples ? nullptr : "The left and right channels differ in length.";
}

}  // namespace PortFlaahgraMusic
