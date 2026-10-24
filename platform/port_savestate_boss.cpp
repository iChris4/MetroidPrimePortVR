#include "port_savestate_boss.h"

#include "MetroidPrime/CEntity.hpp"
#include "MetroidPrime/Enemies/CFlaahgra.hpp"
#include "MetroidPrime/Enemies/CMetroidPrimeRelay.hpp"
#include "MetroidPrime/Enemies/CMetroidPrimeStage2.hpp"
#include "MetroidPrime/Enemies/CRidley.hpp"
#include "MetroidPrime/Enemies/CThardus.hpp"

namespace PortSaveState {
namespace {

void PutBossHeader(BlockWriter& w, PortBossState::Kind kind, uint8_t version) {
  w.U8(kind);
  w.U8(version);
}

} // namespace

bool CaptureBossState(CStateManager& mgr, CEntity& ent, std::string& out) {
  BlockWriter w;
  if (CRidley* b = dynamic_cast< CRidley* >(&ent)) {
    PutBossHeader(w, PortBossState::kRidley, PortBossState::kRidleyVersion);
    PortBossState::PutRidley(mgr, *b, w);
  } else if (CThardus* b = dynamic_cast< CThardus* >(&ent)) {
    PutBossHeader(w, PortBossState::kThardus, PortBossState::kThardusVersion);
    PortBossState::PutThardus(mgr, *b, w);
  } else if (CFlaahgra* b = dynamic_cast< CFlaahgra* >(&ent)) {
    PutBossHeader(w, PortBossState::kFlaahgra, PortBossState::kFlaahgraVersion);
    PortBossState::PutFlaahgra(mgr, *b, w);
  } else if (CMetroidPrimeRelay* b = dynamic_cast< CMetroidPrimeRelay* >(&ent)) {
    PutBossHeader(w, PortBossState::kMetroidPrime, PortBossState::kMetroidPrimeVersion);
    if (!PortBossState::PutMetroidPrime(mgr, *b, w))
      return false;
  } else if (CMetroidPrimeStage2* b = dynamic_cast< CMetroidPrimeStage2* >(&ent)) {
    PutBossHeader(w, PortBossState::kMetroidPrimeStage2, PortBossState::kMetroidPrimeStage2Version);
    PortBossState::PutMetroidPrimeStage2(mgr, *b, w);
  } else {
    return false;
  }
  out = w.bytes;
  return true;
}

void RestoreBossState(CStateManager& mgr, CEntity& ent, const std::string* data) {
  // A named string: the reader keeps a reference, which a ternary's temporary would leave dangling.
  const std::string empty;
  BlockReader reader(data != nullptr ? *data : empty);
  const uint8_t kind = data != nullptr ? reader.U8() : 0;
  const uint8_t version = data != nullptr ? reader.U8() : 0;
  // The reader for a block of this kind and version, else null (no block, or one
  // another build wrote).
  auto readerFor = [&](PortBossState::Kind k, uint8_t v) -> BlockReader* {
    return data != nullptr && reader.ok && kind == k && version == v ? &reader : nullptr;
  };
  if (CRidley* b = dynamic_cast< CRidley* >(&ent)) {
    PortBossState::RestoreRidley(mgr, *b,
                                 readerFor(PortBossState::kRidley, PortBossState::kRidleyVersion));
  } else if (CThardus* b = dynamic_cast< CThardus* >(&ent)) {
    PortBossState::RestoreThardus(
        mgr, *b, readerFor(PortBossState::kThardus, PortBossState::kThardusVersion));
  } else if (CFlaahgra* b = dynamic_cast< CFlaahgra* >(&ent)) {
    PortBossState::RestoreFlaahgra(
        mgr, *b, readerFor(PortBossState::kFlaahgra, PortBossState::kFlaahgraVersion));
  } else if (CMetroidPrimeRelay* b = dynamic_cast< CMetroidPrimeRelay* >(&ent)) {
    PortBossState::RestoreMetroidPrime(
        mgr, *b, readerFor(PortBossState::kMetroidPrime, PortBossState::kMetroidPrimeVersion));
  } else if (CMetroidPrimeStage2* b = dynamic_cast< CMetroidPrimeStage2* >(&ent)) {
    PortBossState::RestoreMetroidPrimeStage2(
        mgr, *b,
        readerFor(PortBossState::kMetroidPrimeStage2, PortBossState::kMetroidPrimeStage2Version));
  }
}

} // namespace PortSaveState
