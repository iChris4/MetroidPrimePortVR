#include "gpu_prof.hpp"

#include "../internal.hpp"
#include "gpu.hpp"

#include <tracy/Tracy.hpp>

#ifdef TRACY_ENABLE
#include <tracy/TracyC.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <magic_enum.hpp>

namespace aurora::webgpu::gpu_prof {
namespace {
Module Log("aurora::webgpu::gpu_prof");

// Each zone consumes a begin/end timestamp pair.
// The last pair is reserved for the frame zone.
constexpr uint32_t MaxZones = 127;
constexpr uint32_t QueryCount = MaxZones * 2 + 2;
constexpr uint32_t FrameBeginQuery = MaxZones * 2;
constexpr uint32_t FrameEndQuery = MaxZones * 2 + 1;
constexpr uint64_t ReadbackSize = QueryCount * sizeof(uint64_t);
constexpr size_t RingDepth = 4;
constexpr uint8_t ContextId = 0;
constexpr uint64_t SaneFrameGapNs = UINT64_C(5'000'000'000);
constexpr uint32_t WindowFrames = 60;
// A window also closes once its frames' spans add up to this much GPU time, so a slow GPU
// (dynamic resolution reads these once a second) does not wait many seconds for 60 frames,
// while a fast one still averages its full 60.
constexpr uint64_t WindowNs = UINT64_C(500'000'000);

enum class EventKind : uint8_t {
  ZoneBegin,
  PassBegin,
  End,
};

struct Event {
  const char* name; // static lifetime; nullptr for End
  uint32_t query;
  EventKind kind;
};

enum class SlotState : uint8_t {
  Free,
  Recording,
  InFlight,
  Mapped,
  Failed,
};

struct Slot {
  wgpu::Buffer readback;
  std::vector<Event> events;
  uint32_t passCount = 0;
  uint32_t queryCount = 0; // queries resolved into the readback
  int64_t submitNs = 0;
  std::atomic<SlotState> state{SlotState::Free};
};

struct TimestampBounds {
  uint64_t begin = 0;
  uint64_t end = 0;

  bool valid() const { return begin != 0 && end > begin; }
};

// Timing is on for Tracy builds whenever the device supports it, and for others only while
// set_enabled(true). Encoder-level timestamps (Zone, frame bounds) are Tracy-only.
bool g_enabled = false;
bool g_timestampsEnabled = false;
std::atomic<bool> g_wanted{false};
std::atomic<bool> g_resetWindow{false};
bool g_active = false; // latched per frame in frame_begin
wgpu::QuerySet g_querySet;
wgpu::Buffer g_resolveBuffer;
std::array<Slot, RingDepth> g_slots;
size_t g_recordSlot = 0;
size_t g_emitSlot = 0;
bool g_frameActive = false;
bool g_framePending = false;
uint32_t g_zoneCount = 0;

struct Accum {
  const char* name;
  double ns = 0.0;
  uint64_t count = 0;
};
std::mutex g_resultMutex;
Result g_result;
std::vector<Accum> g_accum;
double g_accumSpanNs = 0.0;
uint32_t g_accumFrames = 0;

#ifdef TRACY_ENABLE
bool g_contextEmitted = false;
uint16_t g_queryId = 0;
uint64_t g_lastEmittedTs = 0;
uint64_t g_lastFrameEnd = 0;
#endif

int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Tracy keeps references to zone names; intern them for static lifetime.
const char* intern_name(std::string_view name) {
  static absl::flat_hash_map<std::string, const char*> names;
  const auto it = names.find(name);
  if (it != names.end()) {
    return it->second;
  }
  char* stable = new char[name.size() + 1];
  std::memcpy(stable, name.data(), name.size());
  stable[name.size()] = '\0';
  names.emplace(name, stable);
  return stable;
}

#ifdef TRACY_ENABLE
// tracy::GpuContextType not exposed through TracyC.h
uint8_t tracy_context_type(wgpu::BackendType backend) {
  switch (backend) {
  case wgpu::BackendType::OpenGL:
  case wgpu::BackendType::OpenGLES:
    return 1; // OpenGl
  case wgpu::BackendType::Vulkan:
    return 2; // Vulkan
  case wgpu::BackendType::D3D12:
    return 4; // Direct3D12
  case wgpu::BackendType::D3D11:
    return 5; // Direct3D11
  case wgpu::BackendType::Metal:
    return 6; // Metal
  default:
    return 7; // Custom
  }
}

void emit_context(const Slot& slot, uint64_t frameBegin) {
  // Tracy has no notion of WebGPU's opaque timestamp epoch, so the context
  // anchor pairs a GPU timestamp with "now" at emission. Shift the timestamp
  // by the time elapsed since this frame was submitted, so the frame zone
  // lands at the submit point on the timeline instead of trailing it by the
  // readback latency. Residual error is the GPU's submit-to-execute delay.
  const int64_t anchor = static_cast<int64_t>(frameBegin) + (now_ns() - slot.submitNs);
  ___tracy_emit_gpu_new_context_serial({
      .gpuTime = anchor,
      .period = 1.0f,
      .context = ContextId,
      .flags = 0,
      .type = tracy_context_type(g_backendType),
  });
  const std::string name =
      fmt::format("{} ({})", std::string_view{g_adapterInfo.device}, magic_enum::enum_name(g_backendType));
  ___tracy_emit_gpu_context_name_serial({
      .context = ContextId,
      .name = name.c_str(),
      .len = static_cast<uint16_t>(std::min<size_t>(name.size(), UINT16_MAX)),
  });
}

void emit_zone_begin(const char* name, uint64_t gpuNs) {
  const uint64_t srcloc = ___tracy_alloc_srcloc_name(0, "aurora", 6, "gpu_prof", 8, name, std::strlen(name), 0);
  const uint16_t queryId = g_queryId++;
  ___tracy_emit_gpu_zone_begin_alloc_serial({.srcloc = srcloc, .queryId = queryId, .context = ContextId});
  ___tracy_emit_gpu_time_serial({.gpuTime = int64_t(gpuNs), .queryId = queryId, .context = ContextId});
}

void emit_zone_end(uint64_t gpuNs) {
  const uint16_t queryId = g_queryId++;
  ___tracy_emit_gpu_zone_end_serial({.queryId = queryId, .context = ContextId});
  ___tracy_emit_gpu_time_serial({.gpuTime = int64_t(gpuNs), .queryId = queryId, .context = ContextId});
}

TimestampBounds event_bounds(const Slot& slot, const uint64_t* ts) {
  TimestampBounds bounds;
  for (const auto& event : slot.events) {
    const uint64_t t = ts[event.query];
    if (t == 0) {
      continue;
    }
    if (bounds.begin == 0 || t < bounds.begin) {
      bounds.begin = t;
    }
    if (t > bounds.end) {
      bounds.end = t;
    }
  }
  return bounds;
}

void emit_frame(Slot& slot, const uint64_t* ts) {
  // Prefer recorded work bounds. Some backends expose encoder timestamps but
  // report startup/epoch values that are not stable enough to anchor Tracy's
  // GPU context, while pass timestamps are already what the emitted zones use.
  const TimestampBounds zoneBounds = event_bounds(slot, ts);
  uint64_t frameBegin = zoneBounds.begin;
  uint64_t frameEnd = zoneBounds.end;
  if (!zoneBounds.valid() && g_timestampsEnabled) {
    frameBegin = ts[FrameBeginQuery];
    frameEnd = ts[FrameEndQuery];
  }
  if (frameBegin == 0 || frameEnd <= frameBegin) {
    return;
  }

  const uint64_t lastFrameEnd = std::exchange(g_lastFrameEnd, frameEnd);
  if (lastFrameEnd != 0) {
    if (frameBegin < lastFrameEnd) {
      if (frameEnd <= lastFrameEnd || frameEnd - lastFrameEnd >= SaneFrameGapNs) {
        return;
      }
      // Some Dawn/Metal timestamp query begins can remain pinned to an old
      // value while end timestamps advance normally. Use consecutive frame
      // ends to keep Tracy's timeline monotonic in that case.
      frameBegin = lastFrameEnd;
    } else if (frameBegin - lastFrameEnd >= SaneFrameGapNs) {
      return;
    }
  }

  if (!TracyIsConnected) {
    return;
  }

  if (!g_contextEmitted) {
    if (lastFrameEnd == 0) {
      return;
    }
    emit_context(slot, frameBegin);
    g_contextEmitted = true;
  }

  // Tracy requires GPU zones within a context to be properly nested in
  // time, and treats large backward jumps as timer wraparound. The recorded
  // events mirror encode order, which matches GPU execution order. Clamp
  // to keep the stream monotonic.
  uint64_t prev = std::max(g_lastEmittedTs, frameBegin);
  const uint64_t endBound = std::max(frameEnd, prev);
  const auto clamped = [&prev, endBound](uint64_t t) {
    prev = std::min(std::max(t, prev), endBound);
    return prev;
  };

  emit_zone_begin(intern_name("Frame"), clamped(frameBegin));
  uint32_t depth = 0;
  uint64_t topLevelEnd = prev;
  uint64_t idleNs = 0;
  // Zones whose timestamps were never written resolve to 0 (e.g. Metal
  // cannot sample encoder-level timestamps); drop those to keep the stream
  // balanced.
  std::bitset<MaxZones> dropped;
  for (const auto& event : slot.events) {
    if (event.kind == EventKind::End) {
      if (dropped[event.query / 2]) {
        continue;
      }
      const uint64_t t = clamped(ts[event.query]);
      emit_zone_end(t);
      if (--depth == 0) {
        topLevelEnd = t;
      }
    } else {
      if (ts[event.query] == 0 && ts[event.query + 1] == 0) {
        dropped[event.query / 2] = true;
        continue;
      }
      const uint64_t t = clamped(ts[event.query]);
      if (depth++ == 0 && t > topLevelEnd) {
        idleNs += t - topLevelEnd;
      }
      emit_zone_begin(event.name, t);
    }
  }
  const uint64_t end = clamped(frameEnd);
  if (end > topLevelEnd) {
    idleNs += end - topLevelEnd;
  }
  emit_zone_end(end);
  g_lastEmittedTs = prev;

  TracyPlot("aurora: gpuFrameMs", double(frameEnd - frameBegin) * 1e-6);
  TracyPlot("aurora: gpuIdleMs", double(idleNs) * 1e-6);
  TracyPlot("aurora: gpuPasses", int64_t(slot.passCount));
}

#endif // TRACY_ENABLE

// Adds one frame's pass durations to the window; every WindowFrames frames, or WindowNs of GPU
// time, publishes averages.
void aggregate_frame(const Slot& slot, const uint64_t* ts) {
  if (g_resetWindow.exchange(false, std::memory_order_relaxed)) {
    g_accum.clear();
    g_accumSpanNs = 0.0;
    g_accumFrames = 0;
  }
  uint64_t spanBegin = 0;
  uint64_t spanEnd = 0;
  bool any = false;
  for (const auto& event : slot.events) {
    if (event.kind != EventKind::PassBegin) {
      continue;
    }
    const uint64_t begin = ts[event.query];
    const uint64_t end = ts[event.query + 1];
    if (begin == 0 || end <= begin || end - begin >= SaneFrameGapNs) {
      continue;
    }
    auto it = std::find_if(g_accum.begin(), g_accum.end(), [&](const Accum& a) { return a.name == event.name; });
    if (it == g_accum.end()) {
      g_accum.push_back({event.name});
      it = g_accum.end() - 1;
    }
    it->ns += double(end - begin);
    ++it->count;
    if (!any || begin < spanBegin) {
      spanBegin = begin;
    }
    if (!any || end > spanEnd) {
      spanEnd = end;
    }
    any = true;
  }
  if (!any) {
    return;
  }
  g_accumSpanNs += double(spanEnd - spanBegin);
  if (++g_accumFrames < WindowFrames && g_accumSpanNs < double(WindowNs)) {
    return;
  }
  const double inv = 1.0 / double(g_accumFrames);
  Result result;
  result.supported = true;
  result.frames = g_accumFrames;
  double total = 0.0;
  for (const auto& a : g_accum) {
    total += a.ns;
    if (a.count != 0) {
      result.entries.push_back({a.name, float(a.ns * inv * 1e-6), float(double(a.count) * inv)});
    }
  }
  std::sort(result.entries.begin(), result.entries.end(),
            [](const Entry& l, const Entry& r) { return l.msPerFrame > r.msPerFrame; });
  result.totalMs = float(total * inv * 1e-6);
  result.spanMs = float(g_accumSpanNs * inv * 1e-6);
  g_accum.clear();
  g_accumSpanNs = 0.0;
  g_accumFrames = 0;
  std::lock_guard lock{g_resultMutex};
  g_result = std::move(result);
}

void process_frame(Slot& slot) {
  const auto* ts = static_cast<const uint64_t*>(slot.readback.GetConstMappedRange(0, slot.queryCount * sizeof(uint64_t)));
  if (ts == nullptr) {
    return;
  }
  aggregate_frame(slot, ts);
#ifdef TRACY_ENABLE
  emit_frame(slot, ts);
#endif
}

Slot& record_slot() { return g_slots[g_recordSlot]; }

uint32_t alloc_zone() {
  if (!g_frameActive || g_zoneCount >= MaxZones) {
    return UINT32_MAX;
  }
  return g_zoneCount++;
}
} // namespace

void initialize() {
  g_enabled = g_device.HasFeature(wgpu::FeatureName::TimestampQuery);
  if (!g_enabled) {
    Log.info("Timestamp queries unsupported; GPU profiling disabled");
    return;
  }
#ifdef TRACY_ENABLE
  g_timestampsEnabled = true; // TODO: check if allow_unsafe_apis enabled?
#endif
  constexpr wgpu::QuerySetDescriptor querySetDescriptor{
      .label = "GPU profiler timestamps",
      .type = wgpu::QueryType::Timestamp,
      .count = QueryCount,
  };
  g_querySet = g_device.CreateQuerySet(&querySetDescriptor);
  constexpr wgpu::BufferDescriptor resolveDescriptor{
      .label = "GPU profiler resolve",
      .usage = wgpu::BufferUsage::QueryResolve | wgpu::BufferUsage::CopySrc,
      .size = ReadbackSize,
  };
  g_resolveBuffer = g_device.CreateBuffer(&resolveDescriptor);
  constexpr wgpu::BufferDescriptor readbackDescriptor{
      .label = "GPU profiler readback",
      .usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst,
      .size = ReadbackSize,
  };
  for (auto& slot : g_slots) {
    slot.readback = g_device.CreateBuffer(&readbackDescriptor);
    slot.events.reserve(MaxZones * 2);
    slot.state = SlotState::Free;
  }
  g_recordSlot = 0;
  g_emitSlot = 0;
  g_framePending = false;
#ifdef TRACY_ENABLE
  TracyPlotConfig("aurora: gpuFrameMs", tracy::PlotFormatType::Number, false, true, 0);
  TracyPlotConfig("aurora: gpuIdleMs", tracy::PlotFormatType::Number, false, true, 0);
  TracyPlotConfig("aurora: gpuPasses", tracy::PlotFormatType::Number, true, true, 0);
#endif
  Log.info("GPU profiling enabled ({} zones max)", MaxZones);
}

void shutdown() {
  g_querySet = {};
  g_resolveBuffer = {};
  for (auto& slot : g_slots) {
    slot.readback = {};
    slot.events.clear();
    slot.passCount = 0;
    slot.state = SlotState::Free;
  }
  g_enabled = false;
  g_timestampsEnabled = false;
  g_active = false;
  g_frameActive = false;
  g_framePending = false;
}

void frame_begin(const wgpu::CommandEncoder& encoder) {
  if (!g_enabled) {
    return;
  }
#ifdef TRACY_ENABLE
  g_active = true;
#else
  g_active = g_wanted.load(std::memory_order_relaxed);
#endif
  g_frameActive = false;
  if (!g_active) {
    return;
  }
  auto& slot = record_slot();
  if (slot.state != SlotState::Free) {
    return;
  }
  slot.state = SlotState::Recording;
  slot.events.clear();
  slot.passCount = 0;
  g_zoneCount = 0;
  g_frameActive = true;
  if (g_timestampsEnabled) {
    encoder.WriteTimestamp(g_querySet, FrameBeginQuery);
  }
}

void frame_end(const wgpu::CommandEncoder& encoder) {
  if (!g_enabled || !g_frameActive) {
    return;
  }
  g_frameActive = false;
  auto& slot = record_slot();
  if (slot.events.empty() && !g_timestampsEnabled) {
    slot.state = SlotState::Free;
    return;
  }
  if (g_timestampsEnabled) {
    encoder.WriteTimestamp(g_querySet, FrameEndQuery);
  }
  // Tracy reads the frame bounds at the end of the set; pass timestamps alone are packed at the start.
  const uint32_t queryCount = g_timestampsEnabled ? QueryCount : g_zoneCount * 2;
  if (queryCount == 0) {
    slot.state = SlotState::Free;
    return;
  }
  slot.queryCount = queryCount;
  encoder.ResolveQuerySet(g_querySet, 0, queryCount, g_resolveBuffer, 0);
  encoder.CopyBufferToBuffer(g_resolveBuffer, 0, slot.readback, 0, queryCount * sizeof(uint64_t));
  g_framePending = true;
}

void after_submit() {
  if (!g_enabled) {
    return;
  }
  if (!g_framePending && !g_active && g_slots[g_emitSlot].state.load(std::memory_order_acquire) == SlotState::Free) {
    return; // off, and nothing in flight
  }
  if (g_framePending) {
    g_framePending = false;
    auto& slot = record_slot();
    slot.submitNs = now_ns();
    slot.state = SlotState::InFlight;
    slot.readback.MapAsync(wgpu::MapMode::Read, 0, slot.queryCount * sizeof(uint64_t), wgpu::CallbackMode::AllowSpontaneous,
                           [&slot](wgpu::MapAsyncStatus status, wgpu::StringView) {
                             slot.state =
                                 status == wgpu::MapAsyncStatus::Success ? SlotState::Mapped : SlotState::Failed;
                           });
    g_recordSlot = (g_recordSlot + 1) % RingDepth;
  }
  {
    ZoneScopedN("ProcessEvents");
    g_instance.ProcessEvents();
  }
  while (true) {
    auto& slot = g_slots[g_emitSlot];
    const auto state = slot.state.load(std::memory_order_acquire);
    if (state == SlotState::Mapped) {
      process_frame(slot);
      slot.readback.Unmap();
    } else if (state != SlotState::Failed) {
      break;
    }
    slot.state.store(SlotState::Free, std::memory_order_release);
    g_emitSlot = (g_emitSlot + 1) % RingDepth;
  }
}

const wgpu::PassTimestampWrites* pass_writes(std::string_view name) {
  const uint32_t index = alloc_zone();
  if (index == UINT32_MAX) {
    return nullptr;
  }
  auto& slot = record_slot();
  slot.events.push_back({intern_name(name), index * 2, EventKind::PassBegin});
  slot.events.push_back({nullptr, index * 2 + 1, EventKind::End});
  ++slot.passCount;

  static std::array<wgpu::PassTimestampWrites, 4> writes;
  static size_t writesIndex = 0;
  auto& out = writes[writesIndex++ % writes.size()];
  out = {
      .querySet = g_querySet,
      .beginningOfPassWriteIndex = index * 2,
      .endOfPassWriteIndex = index * 2 + 1,
  };
  return &out;
}

Zone::Zone(const wgpu::CommandEncoder& encoder, std::string_view name) {
  if (!g_timestampsEnabled || !g_frameActive) {
    return;
  }
  const uint32_t index = alloc_zone();
  if (index == UINT32_MAX) {
    return;
  }
  record_slot().events.push_back({intern_name(name), index * 2, EventKind::ZoneBegin});
  encoder.WriteTimestamp(g_querySet, index * 2);
  m_encoder = &encoder;
  m_endQuery = index * 2 + 1;
}

Zone::~Zone() {
  if (m_encoder == nullptr) {
    return;
  }
  record_slot().events.push_back({nullptr, m_endQuery, EventKind::End});
  m_encoder->WriteTimestamp(g_querySet, m_endQuery);
}

void set_enabled(bool on) {
  g_wanted.store(on, std::memory_order_relaxed);
  // Restart the window and drop the old numbers, so stale ones don't mix with new ones.
  g_resetWindow.store(true, std::memory_order_relaxed);
  std::lock_guard lock{g_resultMutex};
  g_result = {};
  g_result.supported = g_enabled;
}

bool supported() { return g_enabled; }

Result results() {
  std::lock_guard lock{g_resultMutex};
  Result out = g_result;
  out.supported = g_enabled;
  return out;
}

} // namespace aurora::webgpu::gpu_prof
