#include "port_randomizer.h"

#include "MetroidPrime/Player/CPlayerState.hpp"
#include "port_test.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <system_error>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

// A Windows fast-fail kills the process before anything buffered is written, so
// every phase marker is flushed as it is reached. Without them a crash reports
// as a bare exit code with no clue which phase it happened in.
void Phase(const char* message) {
  std::fprintf(stderr, "[randomizer-tests] %s\n", message);
  std::fflush(stderr);
}

bool Contains(const std::string& text, const std::string& value) {
  return text.find(value) != std::string::npos;
}

// The seed file grammar, through ParseSeedFile: what is accepted, what each
// accepted file holds, and what is refused.
bool ParsesSeed(const std::string& text, PortRandomizer::SeedFile& out) {
  size_t offset = 0;
  std::string reason;
  return PortRandomizer::ParseSeedFile(text, out, offset, reason);
}

bool RefusesSeed(const std::string& text, std::string* reasonOut = nullptr) {
  PortRandomizer::SeedFile out;
  size_t offset = 0;
  std::string reason;
  const bool ok = PortRandomizer::ParseSeedFile(text, out, offset, reason);
  if (reasonOut != nullptr)
    *reasonOut = reason;
  return !ok && !reason.empty() &&
         (offset <= text.size() || offset == PortRandomizer::kSeedNoOffset);
}

void SeedParserTests() {
  using PortRandomizer::SeedFile;
  Phase("seed parser");
  {
    SeedFile seed;
    CHECK_MSG(ParsesSeed(R"( {"seed":"abc","locations":{"0xab-1":{"item":"Missiles","amount":5,"capacity":-5},)"
                     R"("k2":{"item":"MorphBall"}},"models":{"Missiles":{"model":"0x1234ABCD","character":2,)"
                     R"("animation":0},"MorphBall":{"acs":"beef"}},"extra":{"x":true,"y":-1,"z":"s","w":{}}} )",
                     seed),
          "full seed parses");
    CHECK_MSG(seed.name == "abc", "seed name");
    CHECK_MSG(seed.placements.size() == 2, "two placements");
    const auto missiles = seed.placements.find("0XAB-1");
    CHECK_MSG(missiles != seed.placements.end(), "location keys are upper-cased");
    if (missiles != seed.placements.end()) {
      CHECK_MSG(missiles->second.itemType == CPlayerState::kIT_Missiles, "placement item");
      CHECK_MSG(missiles->second.hasAmount && missiles->second.amount == 5, "placement amount");
      CHECK_MSG(missiles->second.hasCapacity && missiles->second.capacity == -5, "placement capacity");
    }
    const auto ball = seed.placements.find("K2");
    CHECK_MSG(ball != seed.placements.end() && ball->second.itemType == CPlayerState::kIT_MorphBall &&
                  !ball->second.hasAmount && !ball->second.hasCapacity,
              "placement without amount or capacity");
    const auto model = seed.models.find(CPlayerState::kIT_Missiles);
    CHECK_MSG(model != seed.models.end() && model->second.model == 0x1234ABCDu &&
                  model->second.acs == 0 && model->second.character == 2 && model->second.animation == 0,
              "model entry");
    const auto acs = seed.models.find(CPlayerState::kIT_MorphBall);
    CHECK_MSG(acs != seed.models.end() && acs->second.acs == 0xBEEFu && acs->second.model == 0,
              "acs-only model entry");
  }
  {
    SeedFile seed;
    CHECK_MSG(ParsesSeed("{}", seed) && seed.name.empty() && seed.placements.empty() && seed.models.empty(),
              "empty seed parses");
    CHECK_MSG(ParsesSeed("\t{ }\r\n", seed), "whitespace around the object");
    CHECK_MSG(ParsesSeed(R"({"seed":"café 😀"})", seed) &&
              seed.name == "caf\xc3\xa9 \xf0\x9f\x98\x80",
          "unicode escapes become UTF-8");
    CHECK_MSG(ParsesSeed(R"({"locations":{"a":{"item":"Missiles"},"A":{"item":"IceBeam"}}})", seed) &&
              seed.placements.size() == 1 && seed.placements["A"].itemType == CPlayerState::kIT_IceBeam,
          "a repeated location keeps the last one");
    CHECK_MSG(ParsesSeed(R"({"locations":{"a":{"item":"Missiles","item":"WaveBeam"}}})", seed) &&
              seed.placements["A"].itemType == CPlayerState::kIT_WaveBeam,
          "a repeated field keeps the last one");
    CHECK_MSG(ParsesSeed(R"({"locations":{"a":{"item":"Missiles","amount":-9999,"capacity":9999}}})", seed) &&
              seed.placements["A"].amount == -9999 && seed.placements["A"].capacity == 9999,
          "amount and capacity bounds are inclusive");
    std::string nested = "{\"x\":";
    for (int i = 0; i < 20; ++i)
      nested += "{\"x\":";
    nested += "1";
    for (int i = 0; i < 21; ++i)
      nested += "}";
    CHECK_MSG(ParsesSeed(nested, seed), "moderate nesting in an unknown key");
  }
  std::string reason;
  CHECK_MSG(RefusesSeed(""), "empty text");
  CHECK_MSG(RefusesSeed("[]"), "top level array");
  CHECK_MSG(RefusesSeed("\"seed\""), "top level string");
  CHECK_MSG(RefusesSeed("{} x"), "trailing data");
  CHECK_MSG(RefusesSeed("{\"seed\":\"a\",}"), "trailing comma");
  CHECK_MSG(RefusesSeed("{/* c */}"), "comment");
  CHECK_MSG(RefusesSeed("{\"seed\":1}"), "seed name not a string");
  CHECK_MSG(RefusesSeed("{\"locations\":[]}"), "locations not an object");
  CHECK_MSG(RefusesSeed("{\"seed\":\"a\x01\"}"), "control character in a string");
  CHECK_MSG(RefusesSeed(R"({"seed":"\q"})"), "invalid escape");
  CHECK_MSG(RefusesSeed(R"({"seed":"\udc00"})"), "lone low surrogate");
  CHECK_MSG(RefusesSeed(R"({"seed":"\ud800"})"), "lone high surrogate");
  CHECK_MSG(RefusesSeed(R"({"seed":"abc)"), "unterminated string");
  CHECK_MSG(RefusesSeed(R"({"locations":{"a":{}}})", &reason) && Contains(reason, "item"),
        "placement without an item");
  CHECK_MSG(RefusesSeed(R"({"locations":{"a":{"item":"Banana"}}})", &reason) && Contains(reason, "item"),
        "unknown item name");
  CHECK_MSG(RefusesSeed(R"({"locations":{"a":{"item":7}}})"), "item not a string");
  CHECK_MSG(RefusesSeed(R"({"locations":{"a":{"item":"Missiles","amount":10000}}})", &reason) &&
            Contains(reason, "9999"),
        "amount above the bound");
  CHECK_MSG(RefusesSeed(R"({"locations":{"a":{"item":"Missiles","capacity":-10000}}})"),
        "capacity below the bound");
  CHECK_MSG(RefusesSeed(R"({"locations":{"a":{"item":"Missiles","amount":1.5}}})"), "fractional amount");
  CHECK_MSG(RefusesSeed(R"({"locations":{"a":{"item":"Missiles","amount":"5"}}})"), "amount as a string");
  CHECK_MSG(RefusesSeed(R"({"locations":{"a":{"item":"Missiles","amount":05}}})"), "leading zero");
  CHECK_MSG(RefusesSeed(R"({"models":{"Missiles":{"character":1}}})", &reason) && Contains(reason, "model"),
        "model entry without a model or acs");
  CHECK_MSG(RefusesSeed(R"({"models":{"Banana":{"model":"1"}}})"), "unknown item name in models");
  CHECK_MSG(RefusesSeed(R"({"models":{"Missiles":{"model":"0x"}}})"), "empty hex asset");
  CHECK_MSG(RefusesSeed(R"({"models":{"Missiles":{"model":"123456789"}}})"), "nine hex digits");
  CHECK_MSG(RefusesSeed(R"({"models":{"Missiles":{"model":"xyz"}}})"), "non-hex asset");
  CHECK_MSG(RefusesSeed(R"({"models":{"Missiles":{"model":"1","character":-1}}})"), "negative character");
  CHECK_MSG(RefusesSeed(R"({"models":{"Missiles":{"model":"1","animation":99999999999}}})"),
        "animation out of range");
  std::string deep = "{\"x\":";
  for (int i = 0; i < 300; ++i)
    deep += "{\"x\":";
  deep += "1";
  for (int i = 0; i < 301; ++i)
    deep += "}";
  CHECK_MSG(RefusesSeed(deep), "nesting limit");

  // Since the move onto PortJson: unknown keys may hold any JSON value, and a
  // problem in a well-formed file names where it is rather than a byte offset.
  {
    SeedFile seed;
    CHECK_MSG(ParsesSeed(R"({"note":null,"tags":["a",1,{}],"locations":{"a":{"item":"Missiles","x":[null]}}})",
                     seed) &&
              seed.placements.size() == 1,
          "arrays and null in unknown keys");
    size_t offset = 0;
    std::string why;
    CHECK_MSG(!PortRandomizer::ParseSeedFile(R"({"locations":{"Chozo-1":{"item":"Banana"}}})", seed, offset,
                                         why) &&
              offset == PortRandomizer::kSeedNoOffset && Contains(why, "Chozo-1") && Contains(why, "Banana"),
          "an invalid seed names the location and the item");
    CHECK_MSG(!PortRandomizer::ParseSeedFile("{} x", seed, offset, why) && offset == 3,
              "malformed JSON reports the byte offset");
  }
}

// These tests set environment variables, which Windows spells differently and
// which needs the _s form to be safe.
bool SetEnv(const char* name, const char* value) {
#ifdef _WIN32
  return _putenv_s(name, value) == 0;
#else
  return setenv(name, value, 1) == 0;
#endif
}

bool UnsetEnv(const char* name) {
#ifdef _WIN32
  return _putenv_s(name, "") == 0;
#else
  return unsetenv(name) == 0;
#endif
}

void SleepMilliseconds(int milliseconds) {
#ifdef _WIN32
  Sleep(static_cast<DWORD>(milliseconds));
#else
  usleep(static_cast<useconds_t>(milliseconds) * 1000);
#endif
}

unsigned long ProcessId() {
#ifdef _WIN32
  return static_cast<unsigned long>(GetCurrentProcessId());
#else
  return static_cast<unsigned long>(getpid());
#endif
}

// Several cases here must run in a process that has not already loaded a seed:
// the loader keeps its state in a function-local static, so a second load in the
// same process would reuse the first one's result and prove nothing. Rather than
// reach for a reload API that production has no use for, the test re-runs itself
// with the case name and the shared directory as arguments. fork/execv on POSIX,
// CreateProcessW on Windows.
int RunChild(const char* self, const char* name, const std::filesystem::path& testDir) {
  const std::string dir = testDir.string();
#ifdef _WIN32
  // The ANSI entry point takes the command line as plain bytes, which is what
  // the paths here are; the wide one would need a real encoding conversion for
  // no gain in a test.
  std::string command = "\"";
  command += self;
  command += "\" ";
  command += name;
  command += " \"";
  command += dir;
  command += "\"";
  std::vector<char> mutableCommand(command.begin(), command.end());
  mutableCommand.push_back('\0');
  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (CreateProcessA(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                     &startup, &process) == 0) {
    std::fprintf(stderr, "[randomizer-tests] CreateProcessA failed for %s: %lu\n", name,
                 static_cast<unsigned long>(GetLastError()));
    return -1;
  }
  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(process.hProcess, &code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return static_cast<int>(code);
#else
  const pid_t child = fork();
  if (child < 0)
    return -1;
  if (child == 0) {
    std::vector<char> selfPath(self, self + std::strlen(self));
    selfPath.push_back('\0');
    std::vector<char> caseName(name, name + std::strlen(name));
    caseName.push_back('\0');
    std::vector<char> caseDir(dir.begin(), dir.end());
    caseDir.push_back('\0');
    char* args[] = {selfPath.data(), caseName.data(), caseDir.data(), nullptr};
    execv(selfPath.data(), args);
    _exit(127);
  }
  int status = 0;
  if (waitpid(child, &status, 0) != child)
    return -1;
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

} // namespace

int main(int argc, char** argv) try {
  using namespace PortRandomizer;
  Phase(argc == 3 ? "child start" : "parent start");

  // ctest may launch the test by a bare name, in which case argv[0] is not a
  // path this process can spawn itself by. Resolve it to the running
  // executable's own path first; that is also what makes a Windows spawn work,
  // where the working directory is not searched the way execv's is.
  std::string selfPath = argc > 0 && argv[0] != nullptr ? argv[0] : std::string();
#ifdef _WIN32
  {
    char buffer[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameA(nullptr, buffer, MAX_PATH);
    if (length != 0 && length < MAX_PATH)
      selfPath.assign(buffer, length);
  }
#endif

  // Child mode: a single case, named on the command line, sharing the parent's
  // temporary directory so the seed files it reads are the ones just written.
  if (argc == 3) {
    const std::string which = argv[1];
    const std::filesystem::path childDir = argv[2];
    // Set the user directory here rather than relying on inheriting the parent's
    // MP_USER_PATH: this is what decides where the dump and check logs land, and
    // a child that wrote them somewhere else would leave the parent reading an
    // empty file and looking for a defect that is not there.
    SetEnv("MP_USER_PATH", childDir.string().c_str());
    Phase("child user directory set");
    if (which == "malformed") {
      if (!SetEnv("MP_RANDO_SEED", (childDir / "malformed.json").string().c_str()))
        return 2;
      EnsureLoaded();
      int item = CPlayerState::kIT_Missiles;
      int capacity = 10;
      int amount = 5;
      const bool applied = ApplyPickup(1, 2, 3, item, capacity, amount);
      return !Enabled() && !applied && item == CPlayerState::kIT_Missiles && CheckCount() == 0 ? 0 : 1;
    }
    if (which == "no-seed") {
      if (!UnsetEnv("MP_RANDO_SEED"))
        return 2;
      EnsureLoaded();
      int item = CPlayerState::kIT_Missiles;
      int capacity = 10;
      int amount = 5;
      const bool applied = ApplyPickup(1, 2, 3, item, capacity, amount);
      RecordCheck(1, 2, 3, item);
      const bool untouched = !Enabled() && !DumpEnabled() && !applied &&
                             item == CPlayerState::kIT_Missiles && capacity == 10 && amount == 5 &&
                             CheckCount() == 0 &&
                             !std::filesystem::exists(childDir / "randomizer_checks.log");
      return untouched ? 0 : 1;
    }
    if (which == "dump") {
      if (!UnsetEnv("MP_RANDO_SEED") || !SetEnv("MP_RANDO_DUMP", "1"))
        return 2;
      EnsureLoaded();
      int item = CPlayerState::kIT_Missiles;
      int capacity = 5;
      int amount = 5;
      PickupModel model;
      model.model = 0x0000ABCD;
      const bool applied = ApplyPickup(1, 2, 3, item, capacity, amount, model);
      return !applied && DumpEnabled() ? 0 : 1;
    }
    if (which == "bad-model") {
      if (!SetEnv("MP_RANDO_SEED", (childDir / "bad-model.json").string().c_str()))
        return 2;
      EnsureLoaded();
      int item = CPlayerState::kIT_Missiles;
      int capacity = 10;
      int amount = 5;
      const bool applied = ApplyPickup(1, 2, 3, item, capacity, amount);
      return !Enabled() && !applied && item == CPlayerState::kIT_Missiles ? 0 : 1;
    }
    return 3;
  }

  SeedParserTests();

  struct ExpectedItem {
    const char* name;
    int value;
  };
  const ExpectedItem items[] = {
      {"PowerBeam", CPlayerState::kIT_PowerBeam},
      {"IceBeam", CPlayerState::kIT_IceBeam},
      {"WaveBeam", CPlayerState::kIT_WaveBeam},
      {"PlasmaBeam", CPlayerState::kIT_PlasmaBeam},
      {"Missiles", CPlayerState::kIT_Missiles},
      {"ScanVisor", CPlayerState::kIT_ScanVisor},
      {"MorphBallBombs", CPlayerState::kIT_MorphBallBombs},
      {"PowerBombs", CPlayerState::kIT_PowerBombs},
      {"Flamethrower", CPlayerState::kIT_Flamethrower},
      {"ThermalVisor", CPlayerState::kIT_ThermalVisor},
      {"ChargeBeam", CPlayerState::kIT_ChargeBeam},
      {"SuperMissile", CPlayerState::kIT_SuperMissile},
      {"GrappleBeam", CPlayerState::kIT_GrappleBeam},
      {"XRayVisor", CPlayerState::kIT_XRayVisor},
      {"IceSpreader", CPlayerState::kIT_IceSpreader},
      {"SpaceJumpBoots", CPlayerState::kIT_SpaceJumpBoots},
      {"MorphBall", CPlayerState::kIT_MorphBall},
      {"CombatVisor", CPlayerState::kIT_CombatVisor},
      {"BoostBall", CPlayerState::kIT_BoostBall},
      {"SpiderBall", CPlayerState::kIT_SpiderBall},
      {"PowerSuit", CPlayerState::kIT_PowerSuit},
      {"GravitySuit", CPlayerState::kIT_GravitySuit},
      {"VariaSuit", CPlayerState::kIT_VariaSuit},
      {"PhazonSuit", CPlayerState::kIT_PhazonSuit},
      {"EnergyTanks", CPlayerState::kIT_EnergyTanks},
      {"UnknownItem1", CPlayerState::kIT_UnknownItem1},
      {"HealthRefill", CPlayerState::kIT_HealthRefill},
      {"UnknownItem2", CPlayerState::kIT_UnknownItem2},
      {"Wavebuster", CPlayerState::kIT_Wavebuster},
      {"Truth", CPlayerState::kIT_Truth},
      {"Strength", CPlayerState::kIT_Strength},
      {"Elder", CPlayerState::kIT_Elder},
      {"Wild", CPlayerState::kIT_Wild},
      {"Lifegiver", CPlayerState::kIT_Lifegiver},
      {"Warrior", CPlayerState::kIT_Warrior},
      {"Chozo", CPlayerState::kIT_Chozo},
      {"Nature", CPlayerState::kIT_Nature},
      {"Sun", CPlayerState::kIT_Sun},
      {"World", CPlayerState::kIT_World},
      {"Spirit", CPlayerState::kIT_Spirit},
      {"Newborn", CPlayerState::kIT_Newborn},
  };
  CHECK_MSG(CPlayerState::kIT_Max == 41, "retail item enum max drifted");
  for (int i = 0; i < CPlayerState::kIT_Max; ++i) {
    CHECK_MSG(ItemFromName(ItemName(i)) == i, "item name/type round-trip failed");
    CHECK_MSG(items[i].value == i, "retail item enum order drifted");
    CHECK_MSG(ItemFromName(items[i].name) == items[i].value, "retail item mapping drifted");
    CHECK_MSG(std::string(ItemName(items[i].value)) == items[i].name,
              "retail item display name drifted");
  }
  CHECK_MSG(ItemFromName("mIsSiLeS") == CPlayerState::kIT_Missiles,
            "item-name lookup should be case-insensitive");
  CHECK_MSG(ItemFromName("Missiles ") == -1 && ItemFromName("not an item") == -1,
            "item-name lookup should require an exact match");
  CHECK_MSG(ItemName(-1) == std::string("Unknown") && ItemName(41) == std::string("Unknown"),
            "out-of-range item should be Unknown");

  Phase("item table checked");
  const std::filesystem::path testDir = std::filesystem::temp_directory_path() /
                                        ("mp-rando-test-" +
                                         std::to_string(static_cast<long long>(ProcessId())));
  std::filesystem::remove_all(testDir);
  std::filesystem::create_directories(testDir);
  const std::filesystem::path validSeed = testDir / "valid.json";
  const std::filesystem::path malformedSeed = testDir / "malformed.json";
  {
    std::ofstream out(validSeed);
    out << R"({"seed":"test-seed","ignored":false,"locations":{
      "00000001:00000002:00000003":{"item":"EnergyTanks","amount":4,"capacity":6,"extra":{"ok":true}},
      "00000001:00000002:00000004":{"item":"PowerBombs"}
    },"models":{
      "EnergyTanks":{"model":"0x0000ABCD","acs":"0","character":0,"animation":0},
      "PowerBombs":{"model":"00000010","acs":"00000020","character":3,"animation":7}
    }})";
  }
  {
    std::ofstream out(malformedSeed);
    out << R"({"seed":"bad","locations":{"00000001:00000002:00000003":{"item":"Missiles"},}})";
  }
  const std::filesystem::path badModelSeed = testDir / "bad-model.json";
  {
    std::ofstream out(badModelSeed);
    out << R"({"seed":"bad-model","locations":{},"models":{"Missiles":{"acs":"0"}}})";
  }
  Phase("seed files written");
  CHECK_MSG(SetEnv("MP_USER_PATH", testDir.string().c_str()), "set MP_USER_PATH");
  CHECK_MSG(SetEnv("MP_RANDO_SEED", validSeed.string().c_str()), "set valid MP_RANDO_SEED");
  CHECK_MSG(UnsetEnv("MP_RANDO_DUMP"), "unset MP_RANDO_DUMP");

  // Each of these needs a process that has not already loaded a seed, so the
  // test re-runs itself rather than exposing a production reload API. The exit
  // code is reported because a spawn that never happened and a child that died
  // both look like a bare failure otherwise.
  const auto runCase = [&](const char* name) {
    const int code = RunChild(selfPath.c_str(), name, testDir);
    if (code != 0) {
      std::fprintf(stderr, "[randomizer-tests] case %s exited with %d\n", name, code);
    }
    return code;
  };
  Phase("running sub-cases");
  CHECK_MSG(runCase("malformed") == 0,
            "malformed seed must disable randomizer without partial application");
  CHECK_MSG(runCase("no-seed") == 0,
            "without seed/dump, hooks must leave pickups untouched and not log checks");
  const int dumpResult = runCase("dump");
  CHECK_MSG(dumpResult == 0, "dump mode must log the location and not apply a placement");
  if (dumpResult == 0) {
    // The child appends to this log and has only just exited. Windows can still
    // be holding the file open, so the read retries and the removal is best
    // effort; failing to tidy up is not a test failure, and the whole
    // directory is removed at the end anyway. The read is in text mode on
    // purpose: the port appends through a text-mode ofstream, so on Windows the
    // file holds CRLF and a binary read would hand back a \r the expected line
    // does not have.
    std::string dumpText;
    for (int attempt = 0; attempt < 20 && dumpText.empty(); ++attempt) {
      std::ifstream dumpLog(testDir / "randomizer_locations.log");
      dumpText.assign(std::istreambuf_iterator<char>(dumpLog), std::istreambuf_iterator<char>());
      if (dumpText.empty()) {
        Phase("dump log not readable yet; retrying");
        SleepMilliseconds(50);
      }
    }
    const std::string expectedLine =
        "LOC 00000001:00000002:00000003 Missiles amount=5 capacity=5 "
        "model=0000ABCD acs=00000000 character=0 animation=0\n";
    if (!Contains(dumpText, expectedLine)) {
      // Show what was actually written: an empty read, a partial line and a
      // differently formatted one are three different problems, and only the
      // text tells them apart.
      std::string shown = dumpText.substr(0, 200);
        for (char& c : shown) {
        if (c == '\n')
          c = '|';
        if (c == '\r')
          c = '^';
        if (static_cast<unsigned char>(c) < 0x20)
          c = '?';
      }
      Phase(("dump log held: \"" + shown + "\"").c_str());
    }
    CHECK_MSG(Contains(dumpText, expectedLine), "dump line should carry the pickup model");
    std::error_code removeError;
    std::filesystem::remove(testDir / "randomizer_locations.log", removeError);
  }
  CHECK_MSG(runCase("bad-model") == 0,
            "a model entry without a model or acs asset must disable the seed");

  Phase("sub-cases done; checking the loaded seed");
  EnsureLoaded();
  CHECK_MSG(Enabled(), "valid seed with placements should enable randomizer");
  CHECK_MSG(std::string(SeedName()) == "test-seed", "seed name should load");
  CHECK_MSG(!DumpEnabled(), "dump mode should default off");
  char formatted[32];
  FormatLocationKey(1, 2, 3, formatted, sizeof(formatted));
  CHECK_MSG(std::string(formatted) == "00000001:00000002:00000003", "location key format mismatch");

  int item = CPlayerState::kIT_Missiles;
  int capacity = 2;
  int amount = 3;
  CHECK_MSG(ApplyPickup(1, 2, 3, item, capacity, amount), "known placement should apply");
  CHECK_MSG(item == CPlayerState::kIT_EnergyTanks && capacity == 6 && amount == 4,
            "placement should rewrite item, capacity, and amount");

  item = CPlayerState::kIT_Missiles;
  capacity = 7;
  amount = 8;
  CHECK_MSG(!ApplyPickup(9, 9, 9, item, capacity, amount), "unknown location should not apply");
  CHECK_MSG(item == CPlayerState::kIT_Missiles && capacity == 7 && amount == 8,
            "unknown location should leave pickup fields untouched");

  item = CPlayerState::kIT_Missiles;
  capacity = 7;
  amount = 8;
  CHECK_MSG(ApplyPickup(1, 2, 4, item, capacity, amount), "optional-field placement should apply");
  CHECK_MSG(item == CPlayerState::kIT_PowerBombs && capacity == 7 && amount == 8,
            "omitted capacity and amount must remain untouched");

  PickupModel pickupModel;
  CHECK_MSG(ModelForItem(CPlayerState::kIT_EnergyTanks, pickupModel) &&
                pickupModel.model == 0x0000ABCD && pickupModel.acs == 0 &&
                pickupModel.character == 0 && pickupModel.animation == 0,
            "seed model should load, accepting a 0x prefix");
  CHECK_MSG(ModelForItem(CPlayerState::kIT_PowerBombs, pickupModel) &&
                pickupModel.model == 0x00000010 && pickupModel.acs == 0x00000020 &&
                pickupModel.character == 3 && pickupModel.animation == 7,
            "seed model should carry an animated model's acs/character/animation");
  CHECK_MSG(!ModelForItem(CPlayerState::kIT_Missiles, pickupModel),
            "an item the seed has no model for should report none");

  CHECK_MSG(CheckCount() == 0, "check count starts at zero");
  RecordCheck(1, 2, 3, item);
  CHECK_MSG(CheckCount() == 1, "recorded check increments count");
  CHECK_MSG(Contains(StatusText(), "rando: test-seed, 1 checks"), "overlay status should report seed/count");
  std::ifstream checks(testDir / "randomizer_checks.log");
  const std::string checkLog((std::istreambuf_iterator<char>(checks)), std::istreambuf_iterator<char>());
  CHECK_MSG(Contains(checkLog, "CHECK 00000001:00000002:00000003 PowerBombs\n"),
            "collected check should be logged");

  Phase("cleaning up");
  // Best effort: a just-exited child can still hold a file on Windows, and a
  // temporary directory that survives one run is not a test failure.
  {
    std::error_code removeError;
    std::filesystem::remove_all(testDir, removeError);
    if (removeError)
      Phase("temporary directory left behind (ignored)");
  }
  UnsetEnv("MP_USER_PATH");
  UnsetEnv("MP_RANDO_SEED");
  UnsetEnv("MP_RANDO_DUMP");
  if (port_test::Failures() == 0) {
    std::puts("[randomizer-tests] passed");
  }
  return PORT_TEST_RESULT();
} catch (const std::exception& error) {
  std::fprintf(stderr, "[randomizer-tests] threw: %s\n", error.what());
  return 2;
} catch (...) {
  std::fputs("[randomizer-tests] threw an unknown exception\n", stderr);
  return 2;
}
