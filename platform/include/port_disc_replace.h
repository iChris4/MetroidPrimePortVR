#pragma once
// Replacing the disc image from the running game (F1 > System > Disc image, console `disc replace`):
// the picked file is checked to be a supported Metroid Prime disc before anything changes, then
// remembered for the next start. On Android the picker hands back a content:// URI, which is copied
// into the data folder on a background thread (disc.iso.part, renamed when complete). The mounted
// disc is never swapped live.
#include <atomic>
#include <cstdint>
#include <string>

namespace PortDiscReplace {

// Whether the port accepts a disc with this header, and what the wrong one is, in words.
bool IsSupportedId(const char* id6, unsigned diskNumber, unsigned version);
std::string DescribeDisc(const char* id6, unsigned version);

// The picked file is a supported disc: "" and its revision ("USA 1.02"), else the reason.
std::string CheckImage(const std::string& path, std::string* revision = nullptr);

// Copies a picked file (a path, or a content:// URI) to <user folder>/disc.iso through disc.iso.part.
// Reports bytes done and total to progress; returns early if cancel turns true. "" on success, else why not.
std::string CopyToUserFolder(const std::string& source, std::string* target, std::atomic<uint64_t>* done,
                             std::atomic<uint64_t>* total, const std::atomic<bool>* cancel);

// Remembers a checked image for the next start and logs it.
void Adopt(const std::string& path, const std::string& revision);

enum class State { Idle, Running, Done, Failed };

struct Status {
  State state = State::Idle;
  uint64_t bytesDone = 0;
  uint64_t bytesTotal = 0;
  bool copying = false;
  std::string message; // the failure, or what to do after a success
  std::string path;
  std::string revision;
};

// Checks (and on Android, copies) a picked file on its own thread. False if one is running.
bool Start(const std::string& picked);
void Cancel();
// Cancels and joins; call before the process ends.
void Shutdown();
Status Snapshot();
// Call from the main thread: remembers the image once a run has finished well. True the one time it does.
bool Poll();
// Asks for a disc image with the platform's file dialog, without blocking; the pick goes to Start.
void PickAsync();

} // namespace PortDiscReplace
