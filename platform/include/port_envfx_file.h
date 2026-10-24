#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "port_envfx_assets.h"

// The envfx.bin file (format in port_envfx_file.cpp): written by the Remastered import,
// parsed by PortEnvFx::Get(). Split from port_envfx_assets.h so the import and the tests
// need not link the mod loader.
namespace PortEnvFx {
// The file for `assets`: the four images, in the order gradient, rain, snow, underwater.
std::vector<uint8_t> Serialize(const Assets& assets);
// Parses and validates a file: magic, version, the sizes the draw expects (see Assets), and
// that nothing is truncated or trails. False with `error` set; `out` is then unspecified.
bool Parse(const uint8_t* data, size_t size, Assets& out, std::string& error);
// The images' GUIDs as the Remastered pak lists them, for the importer.
inline constexpr const char* kGradientGuid = "d98eeff1-1d78-4b18-bcf0-e5fc15f6eaa7";
inline constexpr const char* kFlakeGuid[3] = {"d372ef39-8add-4372-9603-f16811c04f62",
                                              "5afbea59-9889-4d21-8da2-b85023c0f5bd",
                                              "8576eb33-d765-499f-97ec-19d1a2365e46"};
} // namespace PortEnvFx
