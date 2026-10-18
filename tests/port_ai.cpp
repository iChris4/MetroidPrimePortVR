#include "port_test.h"

#include <SDL3/SDL.h>
#include <dolphin/ai.h>
#include <cstdio>
#include <cstdlib>

extern "C" void AIPortPoll();
extern "C" void AIPortSetOutputEnabled(int);
extern "C" void AIPortShutdown();
extern "C" uintptr_t AIPortGetDMAStartAddr();
namespace {
int calls = 0;
bool unregister = false;
bool fedOwnBuffer = false;
alignas(32) unsigned char samples[640]{};
void Callback() {
  ++calls;
  // Each callback must start from a fresh (silent) buffer, never the one it
  // submitted last time, or a mixer that adds to its input feeds back.
  if (AIPortGetDMAStartAddr() == reinterpret_cast<uintptr_t>(samples))
    fedOwnBuffer = true;
  AIInitDMA(reinterpret_cast<uintptr_t>(samples), sizeof(samples));
  if (unregister)
    AIRegisterDMACallback(nullptr);
}
}
int main() {
  SDL_SetEnvironmentVariable(SDL_GetEnvironment(), "SDL_AUDIO_DRIVER", "dummy", true);
  SDL_SetEnvironmentVariable(SDL_GetEnvironment(), "MP_DISABLE_AI_AUDIO", "1", true);
  const uintptr_t samplesAddr = reinterpret_cast<uintptr_t>(samples);
  AIInitDMA(samplesAddr, sizeof(samples));
  REQUIRE(AIPortGetDMAStartAddr() == samplesAddr);
  AIInitDMA(samplesAddr, 0x7FFF * 32 + 32); // past the hardware limit: silence instead
  REQUIRE(AIPortGetDMAStartAddr() != samplesAddr);
  AIRegisterDMACallback(Callback);
  AIPortPoll();
  const int silentCalls = calls;
  REQUIRE(silentCalls > 0);
  AIPortSetOutputEnabled(1);
  AIPortPoll();
  REQUIRE(calls - silentCalls >= 8); // enabling must create and prefill a stream
  REQUIRE(!fedOwnBuffer);
  AIPortSetOutputEnabled(0);
  SDL_Delay(30);
  unregister = true;
  AIPortPoll(); // callback unregisters itself during a catch-up batch
  AIPortShutdown();
  AIPortShutdown();
  std::puts("AI enable, callback lifetime and shutdown regressions passed");
}
