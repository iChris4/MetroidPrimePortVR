#ifndef METROID_PRIME_PORT_PORT_JSON_H
#define METROID_PRIME_PORT_PORT_JSON_H
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Minimal strict JSON for the port's data files (randomizer seeds, Archipelago
// configuration) and for Archipelago protocol packets. Parses into a value
// tree; objects keep insertion order so files read in the order they were
// written and packet fields stay inspectable. The writer is port::JsonWriter
// (port_json_writer.h); it is built from port_json.cpp too.
namespace PortJson {

class Value {
public:
  enum class Type { Null, Bool, Number, String, Array, Object };
  using Element = Value;
  using Elements = std::vector<Value>;
  using Members = std::vector<std::pair<std::string, Value>>;

  Value() = default;
  Value(const Value&) = default;
  Value(Value&&) = default;
  Value& operator=(const Value&) = default;
  Value& operator=(Value&&) = default;
  ~Value() = default;

  static Value MakeBool(bool value);
  static Value MakeNumber(double value);
  static Value MakeString(std::string value);
  static Value MakeArray(Elements elements);
  static Value MakeObject(Members members);

  Type GetType() const { return mType; }
  bool IsNull() const { return mType == Type::Null; }
  bool IsBool() const { return mType == Type::Bool; }
  bool IsNumber() const { return mType == Type::Number; }
  bool IsString() const { return mType == Type::String; }
  bool IsArray() const { return mType == Type::Array; }
  bool IsObject() const { return mType == Type::Object; }

  // Accessors return a fallback/empty value rather than throwing or asserting
  // when the value has a different type, so packet handling stays defensive.
  bool AsBool(bool fallback = false) const;
  double AsNumber(double fallback = 0.0) const;
  // Rounds toward zero. Numbers outside int64's range clamp to the fallback.
  int64_t AsInt(int64_t fallback = 0) const;
  const std::string& AsString() const;
  const Elements& AsArray() const;
  const Members& AsObject() const;
  // Object member lookup; null when absent or when this is not an object.
  const Value* Find(const char* key) const;
  // Array/object/string size, 0 otherwise.
  size_t Size() const;
  // Convenience for the packets: member lookup that must be a string.
  std::string StringOr(const char* key, const char* fallback = "") const;

private:
  Type mType = Type::Null;
  bool mBool = false;
  double mNumber = 0.0;
  std::string mString;
  Elements mArray;
  Members mObject;
};

// Parses `text`. Returns false and fills errorOffset/errorReason on failure;
// `out` is then unspecified. The parser is strict: no comments, no trailing
// commas, no NaN/Infinity, control characters must be escaped, and nesting is
// limited. Trailing whitespace is allowed, trailing data is not.
bool Parse(const std::string& text, Value& out, size_t& errorOffset, const char** errorReason);

// "null"/"bool"/"number"/"string"/"array"/"object", for diagnostics.
const char* TypeName(Value::Type type);

} // namespace PortJson

#endif // METROID_PRIME_PORT_PORT_JSON_H
