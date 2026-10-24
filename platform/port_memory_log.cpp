#include "port_memory_log.h"

#include "port_log.h"

#if defined(__linux__)
#include <aurora/gfx.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#endif

#if defined(__ANDROID__)
#include <SDL3/SDL_system.h>
#include <jni.h>

#include <string>
#endif

namespace PortMemoryLog {
#if defined(__linux__)
namespace {

constexpr uint64_t kSampleNs = 1000000000ull;
constexpr uint64_t kPeriodNs = 30 * kSampleNs;
constexpr int64_t kProcessJumpKb = 64 * 1024;
constexpr int64_t kSystemJumpKb = 128 * 1024;

// Resident size of this process, in KiB; -1 when /proc can't say.
int64_t ProcessKb() {
  FILE* file = std::fopen("/proc/self/statm", "r");
  if (file == nullptr) {
    return -1;
  }
  long long size = 0;
  long long resident = 0;
  const int read = std::fscanf(file, "%lld %lld", &size, &resident);
  std::fclose(file);
  return read == 2 ? resident * (sysconf(_SC_PAGESIZE) / 1024) : -1;
}

struct SystemMemory {
  int64_t totalKb = -1;
  int64_t availableKb = -1;
};

SystemMemory ReadSystem() {
  SystemMemory memory;
  FILE* file = std::fopen("/proc/meminfo", "r");
  if (file == nullptr) {
    return memory;
  }
  char line[128];
  while (std::fgets(line, sizeof(line), file) != nullptr) {
    if (std::strncmp(line, "MemTotal:", 9) == 0) {
      memory.totalKb = std::strtoll(line + 9, nullptr, 10);
    } else if (std::strncmp(line, "MemAvailable:", 13) == 0) {
      memory.availableKb = std::strtoll(line + 13, nullptr, 10);
    }
  }
  std::fclose(file);
  return memory;
}

long long Mib(int64_t kb) { return kb < 0 ? -1 : static_cast< long long >(kb / 1024); }

// Pipeline retention under memory pressure (SetPipelinePressure). The check reuses Tick's
// once-a-second /proc/meminfo sample, so it reads nothing of its own; a trim only raises Aurora's
// request flag, and the eviction runs at the next frame start, as onTrimMemory's does.
constexpr uint64_t kPressurePollNs = 5 * kSampleNs;
// A level that holds trims again this often, so a moderate trim catches what went idle since.
constexpr uint64_t kPressureRepeatNs = 30 * kSampleNs;
constexpr uint32_t kModerateIdleFrames = 3600; // a minute at 60 fps: areas left a while ago
constexpr uint32_t kSevereIdleFrames = 16;     // everything this frame and the last few don't draw
int64_t sPressureModerateKb = 0;
int64_t sPressureSevereKb = 0;

void CheckPipelinePressure(uint64_t nowNs, int64_t availableKb) {
  static uint64_t sLastPollNs = 0;
  static uint64_t sLastTrimNs = 0;
  static int sLastLevel = 0;
  if ((sPressureModerateKb <= 0 && sPressureSevereKb <= 0) || availableKb < 0 ||
      (sLastPollNs != 0 && nowNs - sLastPollNs < kPressurePollNs)) {
    return;
  }
  sLastPollNs = nowNs;
  const int level = sPressureSevereKb > 0 && availableKb < sPressureSevereKb       ? 2
                    : sPressureModerateKb > 0 && availableKb < sPressureModerateKb ? 1
                                                                                   : 0;
  if (level == 0) {
    sLastLevel = 0;
    return;
  }
  if (level <= sLastLevel && nowNs - sLastTrimNs < kPressureRepeatNs) {
    return;
  }
  sLastLevel = level;
  sLastTrimNs = nowNs;
  const uint32_t idle = level == 2 ? kSevereIdleFrames : kModerateIdleFrames;
  // Aurora's "pipeline cache: evicted N" line at the next frame start gives the count.
  PortLog::Write("port: memory pressure: %s, system %lld MiB available (under %lld): trimming pipelines unused "
                 "for %u frames\n",
                 level == 2 ? "severe" : "moderate", Mib(availableKb),
                 Mib(level == 2 ? sPressureSevereKb : sPressureModerateKb), idle);
  aurora_trim_pipelines(idle);
}

void WriteLine(int64_t processKb, const SystemMemory& system, bool textures) {
  char gpu[320] = "";
  if (textures) {
    AuroraTextureStats stats{};
    aurora_get_texture_stats(&stats);
    int n = std::snprintf(gpu, sizeof(gpu), ", textures %u (%llu MiB), targets %u (%llu MiB), lighting %u (%llu MiB)",
                          stats.count[0], static_cast< unsigned long long >(stats.bytes[0] >> 20), stats.count[1],
                          static_cast< unsigned long long >(stats.bytes[1] >> 20), stats.count[2],
                          static_cast< unsigned long long >(stats.bytes[2] >> 20));
    AuroraDawnMemory dawn{};
    if (n > 0 && static_cast< size_t >(n) < sizeof(gpu) && aurora_get_dawn_memory(&dawn)) {
      std::snprintf(gpu + n, sizeof(gpu) - n, ", dawn %llu MiB (textures %llu, buffers %llu, allocated %llu)",
                    static_cast< unsigned long long >(dawn.total >> 20),
                    static_cast< unsigned long long >(dawn.textures >> 20),
                    static_cast< unsigned long long >(dawn.buffers >> 20),
                    static_cast< unsigned long long >(dawn.allocatorAllocated >> 20));
    }
  }
  PortLog::Write("port: memory: process %lld MiB, system %lld of %lld MiB available%s\n", Mib(processKb),
                 Mib(system.availableKb), Mib(system.totalKb), gpu);
}

#if defined(__ANDROID__)
// MetroidPrimeActivity.exitReport(): built in onCreate, before the log runs.
void LogPreviousExit() {
  JNIEnv* env = static_cast< JNIEnv* >(SDL_GetAndroidJNIEnv());
  jobject activity = static_cast< jobject >(SDL_GetAndroidActivity());
  if (env == nullptr || activity == nullptr) {
    return;
  }
  std::string report;
  jclass cls = env->GetObjectClass(activity);
  jmethodID method = cls != nullptr ? env->GetStaticMethodID(cls, "exitReport", "()Ljava/lang/String;") : nullptr;
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    method = nullptr;
  }
  if (method != nullptr) {
    auto text = static_cast< jstring >(env->CallStaticObjectMethod(cls, method));
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
      text = nullptr;
    }
    if (text != nullptr) {
      if (const char* chars = env->GetStringUTFChars(text, nullptr); chars != nullptr) {
        report = chars;
        env->ReleaseStringUTFChars(text, chars);
      }
      env->DeleteLocalRef(text);
    }
  }
  if (cls != nullptr) {
    env->DeleteLocalRef(cls);
  }
  env->DeleteLocalRef(activity);
  if (!report.empty()) {
    PortLog::Write("%s\n", report.c_str());
  }
}

// MetroidPrimeActivity.lowRamDevice(), set in onCreate.
bool ActivityLowRam() {
  JNIEnv* env = static_cast< JNIEnv* >(SDL_GetAndroidJNIEnv());
  jobject activity = static_cast< jobject >(SDL_GetAndroidActivity());
  if (env == nullptr || activity == nullptr) {
    return false;
  }
  bool low = false;
  jclass cls = env->GetObjectClass(activity);
  jmethodID method = cls != nullptr ? env->GetStaticMethodID(cls, "lowRamDevice", "()Z") : nullptr;
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    method = nullptr;
  }
  if (method != nullptr) {
    low = env->CallStaticBooleanMethod(cls, method) == JNI_TRUE;
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
      low = false;
    }
  }
  if (cls != nullptr) {
    env->DeleteLocalRef(cls);
  }
  env->DeleteLocalRef(activity);
  return low;
}
#endif

} // namespace

#if defined(__ANDROID__)
bool LowRamDevice(char* why, size_t whySize) {
  constexpr int64_t kLowRamKb = 4096ll * 1024;
  const int64_t totalKb = ReadSystem().totalKb;
  if (ActivityLowRam()) {
    std::snprintf(why, whySize, "ActivityManager.isLowRamDevice()");
    return true;
  }
  if (totalKb > 0 && totalKb < kLowRamKb) {
    std::snprintf(why, whySize, "%lld MiB of RAM, under 4096", Mib(totalKb));
    return true;
  }
  std::snprintf(why, whySize, "%lld MiB of RAM", Mib(totalKb));
  return false;
}
#else
bool LowRamDevice(char* why, size_t whySize) {
  if (whySize != 0) {
    why[0] = '\0';
  }
  return false;
}
#endif

void LogStartup() {
#if defined(__ANDROID__)
  LogPreviousExit();
#endif
  WriteLine(ProcessKb(), ReadSystem(), false);
}

void Tick(uint64_t nowNs) {
  static uint64_t sLastSampleNs = 0;
  static uint64_t sLastLineNs = 0;
  static int64_t sLineProcessKb = -1;
  static int64_t sLineAvailableKb = -1;
  if (sLastSampleNs != 0 && nowNs - sLastSampleNs < kSampleNs) {
    return;
  }
  sLastSampleNs = nowNs;
  const int64_t processKb = ProcessKb();
  const SystemMemory system = ReadSystem();
  CheckPipelinePressure(nowNs, system.availableKb);
  const bool due = sLastLineNs == 0 || nowNs - sLastLineNs >= kPeriodNs ||
                   std::llabs(processKb - sLineProcessKb) >= kProcessJumpKb ||
                   std::llabs(system.availableKb - sLineAvailableKb) >= kSystemJumpKb;
  if (!due) {
    return;
  }
  sLastLineNs = nowNs;
  sLineProcessKb = processKb;
  sLineAvailableKb = system.availableKb;
  WriteLine(processKb, system, true);
}
#else
bool LowRamDevice(char* why, size_t whySize) {
  if (whySize != 0) {
    why[0] = '\0';
  }
  return false;
}
void LogStartup() {}
void Tick(uint64_t) {}
#endif

void SetPipelinePressure(int64_t moderateMib, int64_t severeMib) {
#if defined(__linux__)
  sPressureModerateKb = moderateMib > 0 ? moderateMib * 1024 : 0;
  sPressureSevereKb = severeMib > 0 ? severeMib * 1024 : 0;
#else
  (void)moderateMib;
  (void)severeMib;
#endif
}
} // namespace PortMemoryLog

#if defined(__ANDROID__)
// Android's memory warnings (onTrimMemory/onLowMemory), from the UI thread. Android 14
// no longer sends the "running low" levels to a visible app; older versions do.
extern "C" JNIEXPORT void JNICALL Java_org_metroidprime_port_MetroidPrimeActivity_nativeMemoryWarning(
    JNIEnv*, jclass, jint level, jlong availableKb, jlong thresholdKb, jboolean low) {
  const char* name = "unknown";
  switch (level) {
  case -1: name = "low memory"; break;
  case 5: name = "running moderate"; break;
  case 10: name = "running low"; break;
  case 15: name = "running critical"; break;
  case 20: name = "ui hidden"; break;
  case 40: name = "background"; break;
  case 60: name = "moderate"; break;
  case 80: name = "complete"; break;
  default: break;
  }
  PortLog::Write("port: memory warning: %s (level %d), process %lld MiB, system %lld MiB available, "
                 "low-memory threshold %lld MiB%s\n",
                 name, static_cast< int >(level), PortMemoryLog::Mib(PortMemoryLog::ProcessKb()),
                 static_cast< long long >(availableKb / 1024), static_cast< long long >(thresholdKb / 1024),
                 low ? " (system is low on memory)" : "");
  // Running low or critical, or the app is in the background: release built pipelines no recent frame
  // drew with (issue #162). The frame loop does it at the next frame start; they rebuild on demand.
  if (level == -1 || level >= 10) {
    aurora_trim_pipelines(16);
    aurora_reduce_gpu_memory();
  }
}
#endif
