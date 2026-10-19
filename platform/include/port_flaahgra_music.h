#pragma once

// The full Flaahgra boss theme for a USA disc.
//
// The NTSC GameCube discs (USA 1.00, 1.01, 1.02) ship Audio/rui_flaaghraL.dsp
// and ...R.dsp cut to the theme's first 36.2 s, so the fight loops that short
// piece. The PAL and Japanese GameCube discs and Metroid Prime Trilogy have the
// whole 114.7 s track in the same format. The import copies those two files
// off the player's own second image into the mod folder "flaahgra-full-theme"
// of the mods folder, where the mod loader serves them in place of the disc's
// files (docs/MODS.md). Nothing of it ships with the port. A mod, so Reload
// mods (F1 > Mods) or a restart picks it up, and turning mods off turns it off.

#include <cstddef>
#include <cstdint>
#include <string>

namespace PortFlaahgraMusic {

// The NTSC discs' sample count; a full track has more.
constexpr uint32_t kTruncatedSamples = 1159268;
constexpr uint32_t kSampleRate = 32000;
constexpr size_t kDspHeaderSize = 0x60;

// Checks the standard DSP ADPCM header at the start of `data` (`size` bytes
// read, `fileSize` the whole file) of a full-length Flaahgra channel: ADPCM, 32 kHz,
// looping, the loop inside the data, the file as long as the header says, and
// more than kTruncatedSamples. Returns nullptr when it is, else why not;
// `samples` is the header's count when it could be read.
const char* CheckChannel(const uint8_t* data, size_t size, uint64_t fileSize, uint32_t& samples);
// The left and right channels must be the same length.
const char* CheckPair(uint32_t leftSamples, uint32_t rightSamples);

struct State {
  bool running = false;
  bool finished = false;  // a run ended; ok and message are its result
  bool ok = false;
  std::string message;
};

// Imports the two files of the image at `path` (a GameCube or Wii image; an
// Android content:// address works) into the mods folder, in the background.
// False when a run is already going.
bool Start(const std::string& path);
State Status();
// Whether both files are in the mods folder.
bool Installed();
// Deletes the imported mod folder; mods already loaded keep serving it until
// the next reload.
bool Remove();
// Imports in the foreground. True on success, else `error` says why.
bool Import(const std::string& path, std::string& error);
// --import-flaahgra-music <image>: Import for the command line; the exit code.
int RunFromCommandLine(const std::string& path);

}  // namespace PortFlaahgraMusic
