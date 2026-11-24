#pragma once

#include <filesystem>
#include <string>

// The port keeps every path as UTF-8 text. A narrow std::filesystem::path is the ANSI code page on
// Windows (non-ASCII folders mangle, .string() can throw), so build and read paths through these.
namespace PortUtf8 {

inline std::filesystem::path FromUtf8(const std::string& text) {
  return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

inline std::string ToUtf8(const std::filesystem::path& path) {
  const std::u8string text = path.u8string();
  return std::string(text.begin(), text.end());
}

} // namespace PortUtf8
