#include "port_json.h"
#include "port_json_writer.h"
#include "port_strings.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <utility>

namespace PortJson {

Value Value::MakeBool(bool value) {
  Value result;
  result.mType = Type::Bool;
  result.mBool = value;
  return result;
}

Value Value::MakeNumber(double value) {
  Value result;
  result.mType = Type::Number;
  result.mNumber = value;
  return result;
}

Value Value::MakeString(std::string value) {
  Value result;
  result.mType = Type::String;
  result.mString = std::move(value);
  return result;
}

Value Value::MakeArray(Elements elements) {
  Value result;
  result.mType = Type::Array;
  result.mArray = std::move(elements);
  return result;
}

Value Value::MakeObject(Members members) {
  Value result;
  result.mType = Type::Object;
  result.mObject = std::move(members);
  return result;
}

bool Value::AsBool(bool fallback) const { return IsBool() ? mBool : fallback; }

double Value::AsNumber(double fallback) const { return IsNumber() ? mNumber : fallback; }

int64_t Value::AsInt(int64_t fallback) const {
  if (!IsNumber() || !std::isfinite(mNumber))
    return fallback;
  // 2^63 is exactly representable as a double; INT64_MAX is not. Use a
  // half-open range so converting the rounded representation of INT64_MAX
  // (2^63) never invokes an out-of-range floating-to-integer conversion.
  const double limit = std::ldexp(1.0, 63);
  if (mNumber < -limit || mNumber >= limit)
    return fallback;
  return static_cast<int64_t>(mNumber);
}

const std::string& Value::AsString() const {
  static const std::string empty;
  return IsString() ? mString : empty;
}

const Value::Elements& Value::AsArray() const {
  static const Elements empty;
  return IsArray() ? mArray : empty;
}

const Value::Members& Value::AsObject() const {
  static const Members empty;
  return IsObject() ? mObject : empty;
}

const Value* Value::Find(const char* key) const {
  if (!IsObject() || key == nullptr)
    return nullptr;
  for (const auto& member : mObject) {
    if (member.first == key)
      return &member.second;
  }
  return nullptr;
}

size_t Value::Size() const {
  switch (mType) {
  case Type::Array:
    return mArray.size();
  case Type::Object:
    return mObject.size();
  case Type::String:
    return mString.size();
  default:
    return 0;
  }
}

std::string Value::StringOr(const char* key, const char* fallback) const {
  const Value* member = Find(key);
  if (member != nullptr && member->IsString())
    return member->AsString();
  return fallback == nullptr ? std::string() : std::string(fallback);
}

namespace {

class Parser {
public:
  explicit Parser(const std::string& text) : mText(text) {}

  bool ParseDocument(Value& out) {
    SkipWhitespace();
    if (mPosition == mText.size())
      return Fail(mPosition, "expected a JSON value");
    if (!ParseValue(out, 0))
      return false;
    SkipWhitespace();
    if (mPosition != mText.size())
      return Fail(mPosition, "trailing data");
    return true;
  }

  size_t ErrorOffset() const { return mErrorOffset; }
  const char* ErrorReason() const { return mErrorReason; }

private:
  bool Fail(size_t offset, const char* reason) {
    if (mErrorReason == nullptr) {
      mErrorOffset = offset;
      mErrorReason = reason;
    }
    return false;
  }

  void SkipWhitespace() {
    while (mPosition < mText.size()) {
      const char c = mText[mPosition];
      if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
        break;
      ++mPosition;
    }
  }

  bool ParseValue(Value& out, size_t depth) {
    if (mPosition >= mText.size())
      return Fail(mPosition, "unexpected end of input");
    const char c = mText[mPosition];
    switch (c) {
    case 'n':
      return ParseLiteral("null", Value(), out);
    case 't':
      return ParseLiteral("true", Value::MakeBool(true), out);
    case 'f':
      return ParseLiteral("false", Value::MakeBool(false), out);
    case '"': {
      std::string value;
      if (!ParseString(value))
        return false;
      out = Value::MakeString(std::move(value));
      return true;
    }
    case '[':
    case '{':
      if (depth >= 128)
        return Fail(mPosition, "nesting too deep");
      return c == '[' ? ParseArray(out, depth) : ParseObject(out, depth);
    default:
      if (c == '-' || (c >= '0' && c <= '9'))
        return ParseNumber(out);
      return Fail(mPosition, "expected a JSON value");
    }
  }

  bool ParseLiteral(const char* literal, Value value, Value& out) {
    size_t i = 0;
    while (literal[i] != '\0') {
      if (mPosition + i >= mText.size() || mText[mPosition + i] != literal[i])
        return Fail(mPosition + i, "invalid literal");
      ++i;
    }
    mPosition += i;
    out = std::move(value);
    return true;
  }

  static int HexValue(char c) {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
    return -1;
  }

  bool ParseHexQuad(uint16_t& value) {
    value = 0;
    for (int i = 0; i < 4; ++i) {
      if (mPosition >= mText.size())
        return Fail(mPosition, "incomplete unicode escape");
      const int digit = HexValue(mText[mPosition]);
      if (digit < 0)
        return Fail(mPosition, "invalid unicode escape");
      value = static_cast<uint16_t>((value << 4) | digit);
      ++mPosition;
    }
    return true;
  }

  static void AppendUtf8(std::string& out, uint32_t codepoint) {
    if (codepoint <= 0x7f) {
      out.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ff) {
      out.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
      out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else if (codepoint <= 0xffff) {
      out.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
      out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
      out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else {
      out.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
      out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
      out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
      out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    }
  }

  bool ParseString(std::string& out) {
    ++mPosition; // opening quote
    while (mPosition < mText.size()) {
      const unsigned char c = static_cast<unsigned char>(mText[mPosition++]);
      if (c == '"')
        return true;
      if (c < 0x20)
        return Fail(mPosition - 1, "unescaped control character");
      if (c != '\\') {
        out.push_back(static_cast<char>(c));
        continue;
      }
      if (mPosition == mText.size())
        return Fail(mPosition - 1, "incomplete escape");
      const char escape = mText[mPosition++];
      switch (escape) {
      case '"':
        out.push_back('"');
        break;
      case '\\':
        out.push_back('\\');
        break;
      case '/':
        out.push_back('/');
        break;
      case 'b':
        out.push_back('\b');
        break;
      case 'f':
        out.push_back('\f');
        break;
      case 'n':
        out.push_back('\n');
        break;
      case 'r':
        out.push_back('\r');
        break;
      case 't':
        out.push_back('\t');
        break;
      case 'u': {
        uint16_t first = 0;
        if (!ParseHexQuad(first))
          return false;
        uint32_t codepoint = first;
        if (first >= 0xd800 && first <= 0xdbff) {
          if (mPosition + 2 > mText.size() || mText[mPosition] != '\\' ||
              mText[mPosition + 1] != 'u')
            return Fail(mPosition, "lone high surrogate");
          mPosition += 2;
          uint16_t second = 0;
          if (!ParseHexQuad(second))
            return false;
          if (second < 0xdc00 || second > 0xdfff)
            return Fail(mPosition - 4, "invalid surrogate pair");
          codepoint = 0x10000u + ((static_cast<uint32_t>(first) - 0xd800u) << 10) +
                      (static_cast<uint32_t>(second) - 0xdc00u);
        } else if (first >= 0xdc00 && first <= 0xdfff) {
          return Fail(mPosition - 4, "lone low surrogate");
        }
        AppendUtf8(out, codepoint);
        break;
      }
      default:
        return Fail(mPosition - 1, "invalid escape");
      }
    }
    return Fail(mPosition, "unterminated string");
  }

  bool ParseNumber(Value& out) {
    const size_t start = mPosition;
    if (mText[mPosition] == '-') {
      ++mPosition;
      if (mPosition == mText.size() || mText[mPosition] < '0' || mText[mPosition] > '9')
        return Fail(mPosition, "invalid number");
    }
    if (mText[mPosition] == '0') {
      ++mPosition;
      if (mPosition < mText.size() && mText[mPosition] >= '0' && mText[mPosition] <= '9')
        return Fail(mPosition, "leading zero in number");
    } else {
      if (mText[mPosition] < '1' || mText[mPosition] > '9')
        return Fail(mPosition, "invalid number");
      while (mPosition < mText.size() && mText[mPosition] >= '0' && mText[mPosition] <= '9')
        ++mPosition;
    }
    if (mPosition < mText.size() && mText[mPosition] == '.') {
      ++mPosition;
      const size_t fractionStart = mPosition;
      while (mPosition < mText.size() && mText[mPosition] >= '0' && mText[mPosition] <= '9')
        ++mPosition;
      if (fractionStart == mPosition)
        return Fail(mPosition, "fraction requires a digit");
    }
    if (mPosition < mText.size() && (mText[mPosition] == 'e' || mText[mPosition] == 'E')) {
      ++mPosition;
      if (mPosition < mText.size() && (mText[mPosition] == '+' || mText[mPosition] == '-'))
        ++mPosition;
      const size_t exponentStart = mPosition;
      while (mPosition < mText.size() && mText[mPosition] >= '0' && mText[mPosition] <= '9')
        ++mPosition;
      if (exponentStart == mPosition)
        return Fail(mPosition, "exponent requires a digit");
    }
    const std::string span = mText.substr(start, mPosition - start);
    char* end = nullptr;
    const double number = std::strtod(span.c_str(), &end);
    if (end != span.c_str() + span.size())
      return Fail(start, "invalid number");
    out = Value::MakeNumber(number);
    return true;
  }

  bool ParseArray(Value& out, size_t depth) {
    ++mPosition; // '['
    SkipWhitespace();
    Value::Elements elements;
    if (mPosition < mText.size() && mText[mPosition] == ']') {
      ++mPosition;
      out = Value::MakeArray(std::move(elements));
      return true;
    }
    for (;;) {
      Value element;
      if (!ParseValue(element, depth + 1))
        return false;
      elements.push_back(std::move(element));
      SkipWhitespace();
      if (mPosition >= mText.size())
        return Fail(mPosition, "unterminated array");
      if (mText[mPosition] == ']') {
        ++mPosition;
        out = Value::MakeArray(std::move(elements));
        return true;
      }
      if (mText[mPosition] != ',')
        return Fail(mPosition, "expected ',' or ']'");
      ++mPosition;
      SkipWhitespace();
    }
  }

  bool ParseObject(Value& out, size_t depth) {
    ++mPosition; // '{'
    SkipWhitespace();
    Value::Members members;
    if (mPosition < mText.size() && mText[mPosition] == '}') {
      ++mPosition;
      out = Value::MakeObject(std::move(members));
      return true;
    }
    for (;;) {
      if (mPosition >= mText.size() || mText[mPosition] != '"')
        return Fail(mPosition, "expected object key");
      std::string key;
      if (!ParseString(key))
        return false;
      SkipWhitespace();
      if (mPosition >= mText.size() || mText[mPosition] != ':')
        return Fail(mPosition, "expected ':'");
      ++mPosition;
      SkipWhitespace();
      Value value;
      if (!ParseValue(value, depth + 1))
        return false;
      members.emplace_back(std::move(key), std::move(value));
      SkipWhitespace();
      if (mPosition >= mText.size())
        return Fail(mPosition, "unterminated object");
      if (mText[mPosition] == '}') {
        ++mPosition;
        out = Value::MakeObject(std::move(members));
        return true;
      }
      if (mText[mPosition] != ',')
        return Fail(mPosition, "expected ',' or '}'");
      ++mPosition;
      SkipWhitespace();
    }
  }

  const std::string& mText;
  size_t mPosition = 0;
  size_t mErrorOffset = 0;
  const char* mErrorReason = nullptr;
};

} // namespace

bool Parse(const std::string& text, Value& out, size_t& errorOffset, const char** errorReason) {
  try {
    Parser parser(text);
    if (parser.ParseDocument(out))
      return true;
    errorOffset = parser.ErrorOffset();
    if (errorReason != nullptr)
      *errorReason = parser.ErrorReason();
    return false;
  } catch (...) {
    errorOffset = 0;
    if (errorReason != nullptr)
      *errorReason = "allocation or parser failure";
    return false;
  }
}

const char* TypeName(Value::Type type) {
  switch (type) {
  case Value::Type::Null:
    return "null";
  case Value::Type::Bool:
    return "bool";
  case Value::Type::Number:
    return "number";
  case Value::Type::String:
    return "string";
  case Value::Type::Array:
    return "array";
  case Value::Type::Object:
    return "object";
  }
  return "null";
}

} // namespace PortJson

namespace port {

void JsonWriter::BeforeValue() {
  if (mAfterKey) {
    mAfterKey = false;
    return;
  }
  assert((mLevels.empty() || !mLevels.back().object) && "a value in an object needs a Key first");
  if (!mLevels.empty()) {
    if (!mLevels.back().empty)
      mOut.push_back(',');
    mLevels.back().empty = false;
  }
}

void JsonWriter::Open(char bracket, bool object) {
  BeforeValue();
  mOut.push_back(bracket);
  mLevels.push_back({object, true});
}

void JsonWriter::Close(char bracket, bool object) {
  assert(!mLevels.empty() && mLevels.back().object == object && !mAfterKey && "unbalanced JSON End");
  (void)object;
  mLevels.pop_back();
  mOut.push_back(bracket);
}

JsonWriter& JsonWriter::BeginObject() {
  Open('{', true);
  return *this;
}

JsonWriter& JsonWriter::EndObject() {
  Close('}', true);
  return *this;
}

JsonWriter& JsonWriter::BeginArray() {
  Open('[', false);
  return *this;
}

JsonWriter& JsonWriter::EndArray() {
  Close(']', false);
  return *this;
}

JsonWriter& JsonWriter::Key(std::string_view key) {
  assert(!mLevels.empty() && mLevels.back().object && !mAfterKey && "a Key belongs in an object");
  if (!mLevels.back().empty)
    mOut.push_back(',');
  mLevels.back().empty = false;
  AppendJsonQuoted(mOut, key);
  mOut.push_back(':');
  mAfterKey = true;
  return *this;
}

JsonWriter& JsonWriter::String(std::string_view text) {
  BeforeValue();
  AppendJsonQuoted(mOut, text);
  return *this;
}

JsonWriter& JsonWriter::Bool(bool value) {
  BeforeValue();
  mOut += value ? "true" : "false";
  return *this;
}

JsonWriter& JsonWriter::Null() {
  BeforeValue();
  mOut += "null";
  return *this;
}

JsonWriter& JsonWriter::Int(int64_t value) {
  BeforeValue();
  mOut += std::to_string(value);
  return *this;
}

JsonWriter& JsonWriter::UInt(uint64_t value) {
  BeforeValue();
  mOut += std::to_string(value);
  return *this;
}

std::string JsonWriter::NumberText(double value) {
  if (!std::isfinite(value))
    return "null";
  char text[40];
  if (std::fabs(value) < 9e15 && std::trunc(value) == value)
    std::snprintf(text, sizeof(text), "%lld", static_cast< long long >(value));
  else
    std::snprintf(text, sizeof(text), "%.17g", value);
  return text;
}

std::string JsonWriter::FloatText(float value) {
  if (!std::isfinite(value))
    return "null";
  char text[32];
  std::snprintf(text, sizeof(text), "%.9g", static_cast< double >(value));
  return text;
}

JsonWriter& JsonWriter::Number(double value) {
  BeforeValue();
  mOut += NumberText(value);
  return *this;
}

JsonWriter& JsonWriter::Float(float value) {
  BeforeValue();
  mOut += FloatText(value);
  return *this;
}

JsonWriter& JsonWriter::Raw(std::string_view json) {
  BeforeValue();
  mOut += json;
  return *this;
}

JsonWriter& JsonWriter::Value(const PortJson::Value& value) {
  switch (value.GetType()) {
  case PortJson::Value::Type::Null: return Null();
  case PortJson::Value::Type::Bool: return Bool(value.AsBool());
  case PortJson::Value::Type::Number: return Number(value.AsNumber());
  case PortJson::Value::Type::String: return String(value.AsString());
  case PortJson::Value::Type::Array:
    BeginArray();
    for (const PortJson::Value& element : value.AsArray())
      Value(element);
    return EndArray();
  case PortJson::Value::Type::Object:
    BeginObject();
    for (const auto& [key, member] : value.AsObject())
      Key(key).Value(member);
    return EndObject();
  }
  return Null();
}

std::string JsonWriter::Take() {
  assert(mLevels.empty() && !mAfterKey && "JSON taken before every Begin had its End");
  std::string out = std::move(mOut);
  mOut.clear();
  mLevels.clear();
  mAfterKey = false;
  return out;
}

} // namespace port
