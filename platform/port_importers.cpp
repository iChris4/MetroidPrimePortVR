// Mod importers (port_importers.h): finds the user's importer programs, runs
// one as a child process and collects its output.

#include "port_importers.h"

#include "port_mods.h"
#include "port_paths.h"
#include "port_utf8_path.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

namespace fs = std::filesystem;

namespace PortImporters {
namespace {

State sState;
std::string sPartial;
SDL_Process* sProcess = nullptr;

using PortUtf8::ToUtf8;

using PortUtf8::FromUtf8;

bool IsImporter(const fs::directory_entry& entry) {
  std::error_code ec;
  if (!entry.is_regular_file(ec)) {
    return false;
  }
#if defined(_WIN32)
  std::string ext = ToUtf8(entry.path().extension());
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return ext == ".exe" || ext == ".bat" || ext == ".cmd";
#else
  const fs::perms perms = entry.status(ec).permissions();
  return !ec && (perms & (fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec)) != fs::perms::none;
#endif
}

SDL_Process* Spawn(const std::string& name, const std::string& argument, bool piped, std::string& error) {
  const std::string folder = Folder();
  const std::vector<std::string> known = List();
  if (folder.empty() || std::find(known.begin(), known.end(), name) == known.end()) {
    error = "no importer named " + name;
    return nullptr;
  }
  const std::string program = ToUtf8(FromUtf8(folder) / FromUtf8(name));
  const char* args[3] = {program.c_str(), argument.empty() ? nullptr : argument.c_str(), nullptr};

  SDL_Environment* env = SDL_CreateEnvironment(true);
  SDL_SetEnvironmentVariable(env, "MP_MODS_DIR", PortMods::Folder().c_str(), true);
  const SDL_PropertiesID props = SDL_CreateProperties();
  SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, const_cast<char**>(args));
  SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ENVIRONMENT_POINTER, env);
  SDL_SetStringProperty(props, SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING, folder.c_str());
  SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
  if (piped) {
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
  }
  // Without this, Windows opens a console window for every run (aurora's SDL patch).
  SDL_SetBooleanProperty(props, "SDL.process.create.windows.no_window", true);
  SDL_Process* process = SDL_CreateProcessWithProperties(props);
  if (process == nullptr) {
    error = std::string("could not start ") + name + ": " + SDL_GetError();
  }
  SDL_DestroyProperties(props);
  SDL_DestroyEnvironment(env);
  return process;
}

// Moves what the pipe holds into the state. Never waits: a child the importer
// left behind can hold the pipe open long after the importer itself has gone.
void ReadOutput() {
  SDL_IOStream* out = SDL_GetProcessOutput(sProcess);
  if (out == nullptr) {
    return;
  }
  char buffer[4096];
  for (size_t got; (got = SDL_ReadIO(out, buffer, sizeof(buffer))) != 0;) {
    SplitLines(buffer, got, sPartial, sState.lines);
  }
}

} // namespace

std::string Folder() {
#if defined(__ANDROID__)
  return {};
#else
  std::string dir = PortPaths::UserFolder();
  if (dir.empty()) {
    return {};
  }
  dir += "importers";
  std::error_code ec;
  fs::create_directories(FromUtf8(dir), ec);
  return dir;
#endif
}

std::vector<std::string> List() {
  std::vector<std::string> names;
  const std::string folder = Folder();
  if (folder.empty()) {
    return names;
  }
  std::error_code ec;
  for (const fs::directory_entry& entry : fs::directory_iterator(FromUtf8(folder), ec)) {
    if (IsImporter(entry)) {
      names.push_back(ToUtf8(entry.path().filename()));
    }
  }
  std::sort(names.begin(), names.end());
  return names;
}

bool Start(const std::string& name, const std::string& argument) {
  if (sState.running) {
    return false;
  }
  sState = State();
  sState.name = name;
  sPartial.clear();
  std::string error;
  sProcess = Spawn(name, argument, true, error);
  if (sProcess == nullptr) {
    sState.finished = true;
    sState.exitCode = -1;
    sState.lines.push_back(error);
    return false;
  }
  sState.running = true;
  return true;
}

const State& Poll() {
  if (!sState.running) {
    return sState;
  }
  ReadOutput();
  int code = 0;
  if (SDL_WaitProcess(sProcess, false, &code)) {
    ReadOutput();
    if (!sPartial.empty()) {
      sState.lines.push_back(sPartial);
      sPartial.clear();
    }
    SDL_DestroyProcess(sProcess);
    sProcess = nullptr;
    sState.running = false;
    sState.finished = true;
    sState.exitCode = code;
  }
  return sState;
}

void Cancel() {
  if (sState.running) {
    // The importer is usually a script with children of its own; a plain
    // terminate lets it take them down, where a forced kill would orphan them.
    SDL_KillProcess(sProcess, false);
    sState.cancelled = true;
  }
}

int RunFromCommandLine(int argc, char** argv) {
  if (argc < 3) {
    const std::vector<std::string> names = List();
    std::printf("Importers in %s:\n", Folder().c_str());
    for (const std::string& name : names) {
      std::printf("  %s\n", name.c_str());
    }
    if (names.empty()) {
      std::printf("  (none)\n");
    }
    std::printf("Run one with: %s --import <name> [argument]\n", argv[0]);
    return 0;
  }
  std::string error;
  // Not piped: the importer writes straight to this terminal.
  SDL_Process* process = Spawn(argv[2], argc > 3 ? argv[3] : "", false, error);
  if (process == nullptr) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  int code = 1;
  SDL_WaitProcess(process, true, &code);
  SDL_DestroyProcess(process);
  if (code == 0) {
    std::printf("Import finished; the mod loads the next time the game starts.\n");
  }
  return code;
}

} // namespace PortImporters
