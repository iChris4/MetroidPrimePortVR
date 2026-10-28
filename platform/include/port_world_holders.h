#pragma once

// The port's own model holders, and the order a mod reload runs in.
//
// PortThirdPersonBody and PortViewModel keep CModelData and model/skin tokens in statics that
// outlive a world, and gpSimplePool keeps an entry alive while a token references it: a holder that
// survives its world hands the next world the outgoing mods' models (with a mod switched off, ones
// whose textures no PAK serves any more: a missing-TXTR abort). They go at world teardown, and again
// before a mod reload, which a front end makes with no world to tear down. Both rebuild on demand.
namespace PortWorldHolders {

// Drops the third-person body (with its camera's per-world flags) and the debug viewmodel.
void Release();

// A mod reload: the holders go while the outgoing files are still served, then the PAKs reopen.
// Header-only so a test can drive it; production passes &Release and the PAK reopen.
inline void ReloadMods(void (*releaseHolders)(), void (*reopenPaks)()) {
  releaseHolders();
  reopenPaks();
}

} // namespace PortWorldHolders
