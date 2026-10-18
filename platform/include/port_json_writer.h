#ifndef METROID_PRIME_PORT_PORT_JSON_WRITER_H
#define METROID_PRIME_PORT_PORT_JSON_WRITER_H
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace PortJson {
class Value;
}

namespace port {

// Compact JSON, written front to back: the counterpart of PortJson::Parse for
// the port's seed and state files and its Archipelago and Discord packets.
// The writer puts in the commas and the key/value colons, so a caller only
// says what comes next:
//
//   port::JsonWriter json;
//   json.BeginObject().Key("cmd").String("Say").Key("text").String(text).EndObject();
//   send(json.Take());
//
// There is no whitespace and no pretty-printing: every file and packet the
// port writes is compact, and the files are compared byte for byte (a seed
// that is saved again must not look changed). Strings go through
// port::AppendJsonQuoted (port_strings.h).
//
// Numbers follow one rule per kind, so a value prints the same wherever it is
// written:
//   - integers (Number with any integral type) print exactly in decimal;
//   - Number(double) prints a whole number below 9e15 in magnitude as an
//     integer ("5031100", not "5031100.0"; AP ids are doubles in PortJson),
//     any other finite value with %.17g so it reads back to the same double,
//     and NaN or an infinity as null, which is the only JSON for them;
//   - Float(float) prints %.9g, the shortest text that is certain to read
//     back to the same float, and null when it is not finite.
// Fractions go through snprintf and so through LC_NUMERIC, which main.cpp
// pins to "C" so the decimal point is always '.'.
//
// Misuse (a value in an object without a key, an unbalanced End, a Key
// outside an object) is a programming error: it asserts in a debug build and
// is not checked otherwise.
class JsonWriter {
public:
  JsonWriter& BeginObject();
  JsonWriter& EndObject();
  JsonWriter& BeginArray();
  JsonWriter& EndArray();

  // The next member's key. The value written next belongs to it.
  JsonWriter& Key(std::string_view key);

  JsonWriter& String(std::string_view text);
  JsonWriter& Bool(bool value);
  JsonWriter& Null();
  JsonWriter& Int(int64_t value);
  JsonWriter& UInt(uint64_t value);
  // Any integer type, exactly. bool is not a number here: use Bool, or
  // Number(flag ? 1 : 0) where the format wants 0/1.
  template < typename T, std::enable_if_t< std::is_integral_v< T > && !std::is_same_v< T, bool >, int > = 0 >
  JsonWriter& Number(T value) {
    if constexpr (std::is_signed_v< T >)
      return Int(static_cast< int64_t >(value));
    else
      return UInt(static_cast< uint64_t >(value));
  }
  JsonWriter& Number(double value);
  JsonWriter& Float(float value);

  // A value that is already JSON text (a stored slot_data object, a number
  // formatted elsewhere), copied as it is. The caller vouches for it.
  JsonWriter& Raw(std::string_view json);
  // A parsed value, compactly, with its numbers through Number(double) and
  // its objects' members in their stored order.
  JsonWriter& Value(const PortJson::Value& value);

  // The text so far. Complete once every Begin has had its End.
  const std::string& Text() const { return mOut; }
  // Moves the text out and leaves the writer empty, ready for another value.
  std::string Take();

  // Number(double) and Float(float) as text, for code that needs the number
  // outside a writer.
  static std::string NumberText(double value);
  static std::string FloatText(float value);

private:
  void BeforeValue();
  void Open(char bracket, bool object);
  void Close(char bracket, bool object);

  struct Level {
    bool object = false;
    bool empty = true;
  };
  std::string mOut;
  std::vector< Level > mLevels;
  bool mAfterKey = false;
};

} // namespace port

#endif // METROID_PRIME_PORT_PORT_JSON_WRITER_H
