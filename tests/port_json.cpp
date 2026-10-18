#include "port_json.h"
#include "port_json_writer.h"
#include "port_strings.h"
#include "port_test.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <utility>

namespace {

bool Parse(const std::string& text, PortJson::Value& value, size_t* offset = nullptr,
           const char** reason = nullptr) {
  size_t localOffset = 999999;
  const char* localReason = "unchanged";
  const bool parsed = PortJson::Parse(text, value, localOffset, &localReason);
  if (offset != nullptr)
    *offset = localOffset;
  if (reason != nullptr)
    *reason = localReason;
  return parsed;
}

// port::JsonWriter: the commas, the escaping and the number rules every file
// and packet the port writes depends on.
void TestWriter() {
  using port::JsonWriter;
  using PortJson::Value;

  // Structure and commas.
  {
    JsonWriter json;
    json.BeginObject()
        .Key("a").Number(1)
        .Key("b").BeginArray().Number(1).String("x").Null().Bool(true).Bool(false).EndArray()
        .Key("c").BeginObject().EndObject()
        .Key("d").BeginArray().EndArray()
        .Key("e").BeginArray().BeginObject().Key("f").BeginArray().BeginArray().EndArray().EndArray().EndObject()
        .BeginObject().EndObject().EndArray()
        .EndObject();
    CHECK_EQ(json.Text(), std::string(R"({"a":1,"b":[1,"x",null,true,false],"c":{},"d":[],"e":[{"f":[[]]},{}]})"));
    const std::string taken = json.Take();
    CHECK_MSG(json.Text().empty(), "Take leaves the writer empty");
    json.BeginArray().EndArray();
    CHECK_EQ(json.Take(), std::string("[]"));
    json.String("top");
    CHECK_EQ(json.Take(), std::string("\"top\""));
    json.Number(7);
    CHECK_EQ(json.Take(), std::string("7"));
    Value parsed;
    CHECK_MSG(Parse(taken, parsed) && parsed.IsObject() && parsed.Size() == 5, "the writer's output parses");
  }

  // Strings and keys: RFC 8259 escaping, UTF-8 and '/' pass through.
  {
    JsonWriter json;
    const std::string nasty = std::string("q\"b\\s/n\nr\rt\tb\bf\f") + '\x01' + '\x1f' + '\x7f' + "\xc3\xa9" +
                              std::string(1, '\0') + "end";
    json.BeginObject().Key(nasty).String(nasty).EndObject();
    const std::string quoted = R"("q\"b\\s/n\nr\rt\tb\u0008f\u000c\u0001\u001f)" "\x7f\xc3\xa9" R"(\u0000end")";
    CHECK_EQ(json.Text(), "{" + quoted + ":" + quoted + "}");
    CHECK_EQ(port::JsonQuote(nasty), quoted);
    Value parsed;
    CHECK_MSG(Parse(json.Text(), parsed) && parsed.AsObject().size() == 1 && parsed.AsObject()[0].first == nasty &&
                  parsed.AsObject()[0].second.AsString() == nasty,
              "escaped strings and keys read back unchanged");
    CHECK_EQ(port::JsonQuote(""), std::string("\"\""));
  }

  // Integers print exactly, whatever their type.
  {
    JsonWriter json;
    json.BeginArray()
        .Number(0)
        .Number(-1)
        .Number(std::numeric_limits< int64_t >::min())
        .Number(std::numeric_limits< int64_t >::max())
        .Number(std::numeric_limits< uint64_t >::max())
        .Number(static_cast< size_t >(42))
        .Number(static_cast< short >(-3))
        .Number(static_cast< unsigned char >(200))
        .Int(-5)
        .UInt(5)
        .EndArray();
    CHECK_EQ(json.Text(), std::string("[0,-1,-9223372036854775808,9223372036854775807,18446744073709551615,42,-3,"
                                      "200,-5,5]"));
  }

  // Doubles: whole numbers below 9e15 as integers, others with %.17g, and
  // non-finite values as null.
  {
    const std::pair< double, const char* > cases[] = {
        {0.0, "0"},
        {-0.0, "0"},
        {5031100.0, "5031100"},
        {-42.0, "-42"},
        {1.5, "1.5"},
        {0.1, "0.10000000000000001"},
        {-1e-7, "-9.9999999999999995e-08"},
        {8999999999999999.0, "8999999999999999"},
        {9e15, "9000000000000000"},
        {12345678901234567.0, "12345678901234568"},
        {1e300, "1.0000000000000001e+300"},
        {std::numeric_limits< double >::infinity(), "null"},
        {-std::numeric_limits< double >::infinity(), "null"},
        {std::numeric_limits< double >::quiet_NaN(), "null"},
    };
    for (const auto& [number, text] : cases) {
      JsonWriter json;
      json.Number(number);
      CHECK_EQ(json.Text(), std::string(text));
      CHECK_EQ(JsonWriter::NumberText(number), std::string(text));
      Value parsed;
      CHECK_MSG(Parse(json.Text(), parsed), "a written double parses");
      if (std::isfinite(number))
        CHECK_MSG(parsed.AsNumber() == number, "a finite double reads back to the same value");
    }
    JsonWriter json;
    json.Number(2.5f); // a float without Float() takes the double rule
    CHECK_EQ(json.Take(), std::string("2.5"));
  }

  // Floats: %.9g, null when not finite.
  {
    const std::pair< float, const char* > cases[] = {
        {1.0f, "1"},
        {0.1f, "0.100000001"},
        {-48.1011543f, "-48.1011543"},
        {std::numeric_limits< float >::max(), "3.40282347e+38"},
        {std::numeric_limits< float >::infinity(), "null"},
        {std::numeric_limits< float >::quiet_NaN(), "null"},
    };
    for (const auto& [number, text] : cases) {
      JsonWriter json;
      json.Float(number);
      CHECK_EQ(json.Text(), std::string(text));
      CHECK_EQ(JsonWriter::FloatText(number), std::string(text));
    }
  }

  // Raw text and parsed values.
  {
    JsonWriter json;
    json.BeginObject().Key("time").Raw("1790000000.125").Key("slot_data").Raw("{\"a\":1}").EndObject();
    CHECK_EQ(json.Take(), std::string(R"({"time":1790000000.125,"slot_data":{"a":1}})"));
    json.BeginArray().Raw("1").Raw("2").EndArray();
    CHECK_EQ(json.Take(), std::string("[1,2]"));

    Value parsed;
    REQUIRE(Parse(R"( { "z" : [ 1 , 2.5 , -0.0 , 1e2 , "sé\/" , true , null , { } , [ ] ] , "a" : { "b" : false } } )",
                  parsed));
    json.Value(parsed);
    CHECK_EQ(json.Take(), std::string("{\"z\":[1,2.5,0,100,\"s\xc3\xa9/\",true,null,{},[]],\"a\":{\"b\":false}}"));
    json.BeginArray().Value(parsed.AsObject()[1].second).Value(Value()).EndArray();
    CHECK_EQ(json.Take(), std::string(R"([{"b":false},null])"));
    // An out-of-range literal parses to infinity; it is written as null rather
    // than as text no reader accepts.
    REQUIRE(Parse("[1e400]", parsed));
    json.Value(parsed);
    CHECK_EQ(json.Take(), std::string("[null]"));
  }
}

} // namespace

int main() {
  using PortJson::Value;
  Value value;
  CHECK_MSG(Parse("null", value) && value.IsNull(), "null literal");
  CHECK_MSG(Parse("true", value) && value.IsBool() && value.AsBool(), "true literal");
  CHECK_MSG(Parse("false", value) && value.IsBool() && !value.AsBool(true), "false literal");
  CHECK_MSG(Parse("  \t\r\n[true,false,null,-7,0,1.25,3e2,\"s\",[],{}] \n", value),
            "array, nested values, and JSON whitespace");
  CHECK_MSG(value.IsArray() && value.Size() == 10, "array type and size");
  CHECK_MSG(value.AsArray()[3].AsInt() == -7 && value.AsArray()[5].AsNumber() == 1.25 &&
                value.AsArray()[6].AsNumber() == 300.0,
            "negative, fractional, and exponent numbers");
  std::string atDepthLimit(128, '[');
  atDepthLimit.push_back('0');
  atDepthLimit.append(128, ']');
  CHECK_MSG(Parse(atDepthLimit, value), "nesting at the 128-level limit is accepted");

  const std::string escaped = R"("\"\\\/\b\f\n\r\t\u0041\uD83D\uDE00")";
  CHECK_MSG(Parse(escaped, value) && value.IsString(), "all string escapes and Unicode escapes");
  const std::string expectedEscapes = std::string("\"\\/\b\f\n\r\tA") + "\xF0\x9F\x98\x80";
  CHECK_MSG(value.AsString() == expectedEscapes, "escaped string decodes to expected UTF-8 bytes");
  CHECK_MSG(value.Size() == expectedEscapes.size(), "string size counts UTF-8 bytes");

  CHECK_MSG(Parse("{\"second\":2,\"first\":1,\"str\":\"yes\",\"n\":null}", value),
            "object syntax");
  CHECK_MSG(value.IsObject() && value.Size() == 4 && value.AsObject()[0].first == "second" &&
                value.AsObject()[1].first == "first",
            "object insertion order and member count");
  CHECK_MSG(value.Find("first") != nullptr && value.Find("first")->AsInt() == 1 &&
                value.Find("missing") == nullptr && value.Find(nullptr) == nullptr,
            "Find lookup and missing members");
  CHECK_MSG(value.StringOr("str", "fallback") == "yes" &&
                value.StringOr("n", "fallback") == "fallback" &&
                value.StringOr("missing", "fallback") == "fallback" &&
                value.StringOr("str", nullptr) == "yes",
            "StringOr string-only lookup and fallback");
  CHECK_MSG(value.AsBool(true) && value.AsNumber(42.0) == 42.0 && value.AsArray().empty() &&
                value.AsString().empty() && Value::MakeBool(true).AsObject().empty(),
            "wrong-type accessors return fallbacks or empty values");

  CHECK_MSG(Value::MakeNumber(12.9).AsInt(-5) == 12 &&
                Value::MakeNumber(-12.9).AsInt(-5) == -12 &&
                Value::MakeNumber(std::numeric_limits<double>::infinity()).AsInt(-5) == -5 &&
                Value::MakeNumber(std::ldexp(1.0, 63)).AsInt(-5) == -5 &&
                Value::MakeNumber(-std::ldexp(1.0, 63)).AsInt(-5) == std::numeric_limits<int64_t>::min(),
            "AsInt truncation and int64 range handling");
  CHECK_MSG(PortJson::TypeName(Value::Type::Null) == std::string("null") &&
                PortJson::TypeName(Value::Type::Bool) == std::string("bool") &&
                PortJson::TypeName(Value::Type::Number) == std::string("number") &&
                PortJson::TypeName(Value::Type::String) == std::string("string") &&
                PortJson::TypeName(Value::Type::Array) == std::string("array") &&
                PortJson::TypeName(Value::Type::Object) == std::string("object"),
            "TypeName values");
  CHECK_MSG(Value().Size() == 0 && Value::MakeBool(true).Size() == 0 &&
                Value::MakeNumber(1).Size() == 0,
            "non-container sizes are zero");

  const std::string invalid[] = {
      "", "NaN", "Infinity", "+1", ".5", "1.", "0x10", "01", "//comment\nnull",
      "/*comment*/null",
      "[1,]", "{\"x\":1,}", "[1 2]", "{\"x\" 1}", "true false", "\"bad\nstring\"",
      R"("\q")", "\"unterminated", R"("\uD800")", R"("\uDC00")",
      R"("\uD800\u0041")"};
  for (const std::string& text : invalid) {
    size_t offset = 0;
    const char* reason = nullptr;
    const bool parsed = Parse(text, value, &offset, &reason);
    CHECK_MSG(!parsed, "invalid JSON form must be rejected");
    CHECK_MSG(offset <= text.size(), "failure offset should identify a byte in/at the input");
    CHECK_MSG(reason != nullptr && std::string(reason) != "unchanged", "failure reason is set");
  }
  size_t offset = 0;
  CHECK_MSG(!Parse("", value, &offset) && offset == 0, "empty input reports offset zero");
  CHECK_MSG(!Parse("01", value, &offset) && offset == 1, "leading-zero error points at the extra digit");
  CHECK_MSG(!Parse("true false", value, &offset) && offset == 5,
            "trailing-data error points at the first extra byte");
  CHECK_MSG(!Parse(R"("\q")", value, &offset) && offset == 2,
        "bad escape error points at the escape character");

  std::string tooDeep(129, '[');
  tooDeep.append(129, ']');
  size_t deepOffset = 0;
  const char* deepReason = nullptr;
  CHECK_MSG(!Parse(tooDeep, value, &deepOffset, &deepReason) && deepReason != nullptr &&
                std::string(deepReason) == "nesting too deep" && deepOffset == 128,
            "nesting limit has the specified error");

  size_t untouchedOffset = 321;
  const char* untouchedReason = "unchanged";
  CHECK_MSG(PortJson::Parse("0", value, untouchedOffset, &untouchedReason) &&
                untouchedOffset == 321 && untouchedReason == std::string("unchanged"),
            "success leaves error outputs untouched");

  TestWriter();

  if (port_test::Failures() == 0) {
    std::puts("[json-tests] passed");
  }
  return PORT_TEST_RESULT();
}
