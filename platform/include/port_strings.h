#pragma once

// Small string and file helpers shared by the port's readers and widgets.
// Each one used to be a file-local copy in a platform/*.cpp file; gathering
// them keeps the copies from drifting apart. Nothing here is locale-aware on
// purpose: the inputs are file names, ids and tokens, all ASCII.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace port {

// One hex digit's value, or -1. Both cases are accepted; nothing else is.
inline int HexDigit(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

// Eight uppercase hex digits, the way resource ids print in logs and names.
inline std::string Hex8(uint32_t v) {
  char text[16];
  std::snprintf(text, sizeof(text), "%08X", v);
  return text;
}

// ASCII-only lowercase. Bytes outside A-Z pass through untouched, so unlike
// tolower this cannot change behaviour under a non-C locale.
inline std::string Lower(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') {
      c = char(c - 'A' + 'a');
    }
  }
  return s;
}

inline bool EndsWith(const std::string& s, const char* tail) {
  const size_t n = std::strlen(tail);
  return s.size() >= n && s.compare(s.size() - n, n, tail) == 0;
}

inline bool StartsWith(std::string_view s, std::string_view prefix) {
  return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

// `s` without the leading and trailing bytes found in `chars`. The view
// points into `s`; Trim makes a copy for callers that keep the result.
inline std::string_view TrimView(std::string_view s, std::string_view chars = " \t\r\n") {
  const size_t begin = s.find_first_not_of(chars);
  if (begin == std::string_view::npos) {
    return {};
  }
  return s.substr(begin, s.find_last_not_of(chars) - begin + 1);
}

inline std::string Trim(std::string_view s, std::string_view chars = " \t\r\n") {
  return std::string(TrimView(s, chars));
}

// The fields of `line` between each `sep`, empty ones included: "a,,b" is
// three fields and "" is one.
inline std::vector<std::string> Split(const std::string& line, char sep) {
  std::vector<std::string> out;
  size_t start = 0;
  for (;;) {
    const size_t at = line.find(sep, start);
    if (at == std::string::npos) {
      out.push_back(line.substr(start));
      return out;
    }
    out.push_back(line.substr(start, at - start));
    start = at + 1;
  }
}

// An 8-hex-digit id followed by `suffix`, whose letters match either case:
// the mod file name for one baked resource id.
inline bool ParseHexFileName(const std::string& fileName, const char* suffix, uint32_t& id) {
  const size_t suffixLength = std::strlen(suffix);
  if (fileName.size() != 8 + suffixLength) {
    return false;
  }
  for (size_t i = 0; i < suffixLength; ++i) {
    const char c = fileName[8 + i];
    if ((c >= 'A' && c <= 'Z' ? char(c | 0x20) : c) != suffix[i]) {
      return false;
    }
  }
  id = 0;
  for (size_t i = 0; i < 8; ++i) {
    const int digit = HexDigit(fileName[i]);
    if (digit < 0) {
      return false;
    }
    id = (id << 4) | uint32_t(digit);
  }
  return true;
}

namespace detail {
template <typename Bytes>
Bytes ReadAllAs(std::ifstream& in) {
  Bytes data;
  if (!in) {
    return data;
  }
  in.seekg(0, std::ios::end);
  const std::streamoff size = in.tellg();
  in.seekg(0, std::ios::beg);
  if (in && size > 0) {
    data.resize(size_t(size));
    in.read(reinterpret_cast<char*>(data.data()), std::streamsize(size));
    data.resize(size_t(in.gcount()));
  } else {
    in.clear();
    data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  in.clear();
  return data;
}
} // namespace detail

// The whole file, in one read when its size is known. The stream is left as
// a read through istreambuf_iterator leaves it: failed only when the file
// did not open.
inline std::vector<uint8_t> ReadAll(std::ifstream& in) { return detail::ReadAllAs<std::vector<uint8_t>>(in); }

// The same, as a string: for text, and for the formats kept in one.
inline std::string ReadAllText(std::ifstream& in) { return detail::ReadAllAs<std::string>(in); }

// Appends text as a JSON string literal, quotes included. This is the port's
// one JSON string escaper (JsonQuote and port::JsonWriter both come here),
// and it follows RFC 8259: '"' and '\' are escaped, control characters below
// 0x20 become \n, \r, \t or \u00XX, and every other byte passes through, so
// UTF-8 stays UTF-8 and '/' and DEL are left as they are. The text is not
// checked for valid UTF-8.
inline void AppendJsonQuoted(std::string& out, std::string_view text) {
  out.push_back('"');
  for (const char c : text) {
    switch (c) {
    case '"': out += "\\\""; break;
    case '\\': out += "\\\\"; break;
    case '\n': out += "\\n"; break;
    case '\r': out += "\\r"; break;
    case '\t': out += "\\t"; break;
    default:
      if (static_cast<unsigned char>(c) < 0x20) {
        char escape[8];
        std::snprintf(escape, sizeof(escape), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
        out += escape;
      } else {
        out.push_back(c);
      }
    }
  }
  out.push_back('"');
}

// text as a JSON string literal, quotes included (AppendJsonQuoted).
inline std::string JsonQuote(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 2);
  AppendJsonQuoted(out, text);
  return out;
}

}  // namespace port
