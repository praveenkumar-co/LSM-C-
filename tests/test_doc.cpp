// Phase 4 document tests: the JSON parser and serializer (P4.A). No disk writes.
#include "test_common.hpp"

#include "doc/Json.hpp"

#include <cmath>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <vector>

using namespace testutil;
using doc::Type;
using doc::Value;

static bool parseFails(const std::string& text) {
    try {
        (void)doc::parse(text);
    } catch (const doc::ParseError&) {
        return true;
    }
    return false;
}

static void expectRejected(const std::string& text) {
    if (!parseFails(text)) {
        std::cerr << "accepted invalid JSON: " << text.substr(0, 80) << "\n";
        CHECK(false);
    }
}

// parse -> serialize gives `canonical`, and parsing that again gives the same tree.
static void expectRoundTrip(const std::string& text, const std::string& canonical) {
    Value v = doc::parse(text);
    const std::string out = doc::serialize(v);
    if (out != canonical) {
        std::cerr << "serialize(parse(" << text << ")) = " << out << ", expected " << canonical
                  << "\n";
        CHECK(false);
    }
    CHECK(doc::parse(out) == v);
}

static void testA1RoundTrip() {
    banner("P4.A1 Round trip keeps the tree and the key order");
    const std::string compact =
        "{\"z\":1,\"a\":{\"y\":[1,2,{\"b\":null}],\"x\":\"s\"},\"m\":true,\"d\":2.5}";
    expectRoundTrip(compact, compact);
    expectRoundTrip(" {\n \"z\" : 1 ,\t\"a\":{ \"y\" : [ 1 , 2 , { \"b\" : null } ] , \"x\" : "
                    "\"s\" } , \"m\" : true , \"d\" : 2.5 }\r\n",
                    compact);
    Value v = doc::parse(compact);
    CHECK(v.size() == 4 && v.keyAt(0) == "z" && v.keyAt(1) == "a" && v.keyAt(2) == "m" &&
          v.keyAt(3) == "d");
    CHECK(v.find("a")->keyAt(0) == "y" && v.find("a")->keyAt(1) == "x");
    // Setting an existing key keeps its position; a new key goes last.
    v.set("a", Value::integer(7));
    v.set("new", Value::boolean(false));
    CHECK(doc::serialize(v) == "{\"z\":1,\"a\":7,\"m\":true,\"d\":2.5,\"new\":false}");
    CHECK(v.erase("z") && !v.erase("z"));
    CHECK(doc::serialize(v) == "{\"a\":7,\"m\":true,\"d\":2.5,\"new\":false}");
    pass("P4.A1 round trip keeps tree and key order");
}

static void testA2Types() {
    banner("P4.A2 All types, empty {} and []");
    Value v = doc::parse("{\"n\":null,\"t\":true,\"f\":false,\"i\":-42,\"d\":0.5,\"s\":\"x\","
                         "\"a\":[1,\"two\",3.0],\"o\":{\"k\":\"v\"},\"eo\":{},\"ea\":[]}");
    CHECK(v.find("n")->isNull());
    CHECK(v.find("t")->asBool() && !v.find("f")->asBool());
    CHECK(v.find("i")->type() == Type::Int && v.find("i")->asInt() == -42);
    CHECK(v.find("d")->type() == Type::Double && v.find("d")->asDouble() == 0.5);
    CHECK(v.find("s")->asString() == "x");
    CHECK(v.find("a")->size() == 3 && v.find("a")->at(2).type() == Type::Double);
    CHECK(v.find("o")->find("k")->asString() == "v");
    CHECK(v.find("eo")->isObject() && v.find("eo")->size() == 0);
    CHECK(v.find("ea")->isArray() && v.find("ea")->size() == 0);
    for (const char* top : {"{}", "[]", "null", "true", "false", "0", "-1", "1.5", "\"s\"", "[[]]",
                            "{\"a\":{}}", "[{},[],{}]"})
        expectRoundTrip(top, top);
    expectRoundTrip("{ }", "{}");
    expectRoundTrip("[ ]", "[]");
    CHECK(doc::parse("1") != doc::parse("1.0")); // Int and Double are distinct
    pass("P4.A2 all types, empty {} and []");
}

static void testA3Escapes() {
    banner("P4.A3 Escapes, surrogate pairs, bad escapes");
    Value v = doc::parse("\"\\\"\\\\\\/\\b\\f\\n\\r\\t\"");
    CHECK(v.asString() == std::string("\"\\/\b\f\n\r\t"));
    CHECK(doc::parse("\"\\u0041\\u00e9\\u20AC\"").asString() == "A\xC3\xA9\xE2\x82\xAC");
    CHECK(doc::parse("\"\\ud83d\\ude00\"").asString() == "\xF0\x9F\x98\x80"); // U+1F600
    CHECK(doc::parse("\"\\uDBFF\\uDFFF\"").asString() == "\xF4\x8F\xBF\xBF"); // U+10FFFF
    CHECK(doc::parse("\"\\u0000\"").asString() == std::string(1, '\0'));
    // Output escapes only what JSON requires; '/' and non-ASCII stay as they are.
    expectRoundTrip("\"\\u0001\\u001f\\/\\u00e9\"", "\"\\u0001\\u001f/\xC3\xA9\"");
    expectRoundTrip("\"\\b\\f\\n\\r\\t\\\"\\\\\"", "\"\\b\\f\\n\\r\\t\\\"\\\\\"");
    int rejected = 0;
    for (const char* bad : {"\"\\ud83d\"", "\"\\ude00\"", "\"\\ude00\\ud83d\"", "\"\\ud83dx\"",
                            "\"\\ud83d\\u0041\"", "\"\\ud83d\\\"", "\"\\x41\"", "\"\\u12\"",
                            "\"\\u12G4\"", "\"\\U0041\"", "\"\\a\"", "\"\\'\"", "\"\\", "\"abc"}) {
        expectRejected(bad);
        ++rejected;
    }
    pass("P4.A3 escapes incl. surrogate pairs; bad escapes and lone/reversed surrogates rejected",
         std::to_string(rejected) + " rejected");
}

static void testA4Rejects() {
    banner("P4.A4 Invalid documents are rejected");
    const std::vector<std::string> bad = {"{\"a\":1,\"a\":2}",
                                          "{\"a\":{\"b\":1,\"c\":2,\"b\":3}}",
                                          "NaN",
                                          "Infinity",
                                          "-Infinity",
                                          "[NaN]",
                                          "{\"a\":Infinity}",
                                          "[1,]",
                                          "{\"a\":1,}",
                                          "[,1]",
                                          "/* c */ 1",
                                          "[1] // c",
                                          "{\"a\":1 /* c */}",
                                          "01",
                                          "-01",
                                          "[00]",
                                          "{a:1}",
                                          "{'a':1}",
                                          "['a']",
                                          "{} x",
                                          "[1] [2]",
                                          "1 2",
                                          "\"a\x01\"",
                                          std::string("\"a\0b\"", 5),
                                          "\"a\nb\"",
                                          "\"\xC0\xAF\"",
                                          "\"\xED\xA0\x80\"",
                                          "\"\xF5\x80\x80\x80\"",
                                          "\"\xE2\x82\"",
                                          "\"\x80\"",
                                          "{\"\xFF\":1}",
                                          "",
                                          " ",
                                          "tru",
                                          "nul",
                                          "True",
                                          "+1",
                                          ".5",
                                          "1.",
                                          "1e",
                                          "1e+",
                                          "-",
                                          "0x10",
                                          "[1 2]",
                                          "{\"a\" 1}",
                                          "{\"a\":}",
                                          "{1:2}",
                                          "[",
                                          "{",
                                          "]"};
    for (const std::string& b : bad) expectRejected(b);
    pass("P4.A4 duplicate keys, NaN/Inf, trailing commas, comments, leading zeros, unquoted keys, "
         "trailing garbage, control chars, invalid UTF-8 rejected",
         std::to_string(bad.size()) + " cases");
}

static void testA5Numbers() {
    banner("P4.A5 Numbers");
    const int64_t mx = std::numeric_limits<int64_t>::max();
    const int64_t mn = std::numeric_limits<int64_t>::min();
    CHECK(doc::parse("9223372036854775807").asInt() == mx);
    CHECK(doc::parse("-9223372036854775808").asInt() == mn);
    expectRoundTrip("9223372036854775807", "9223372036854775807");
    expectRoundTrip("-9223372036854775808", "-9223372036854775808");
    expectRejected("9223372036854775808");
    expectRejected("-9223372036854775809");
    expectRejected("123456789012345678901234567890");
    CHECK(doc::parse("-0").type() == Type::Int && doc::parse("-0").asInt() == 0);
    // Doubles round-trip bit for bit and keep a '.' or 'e'.
    Value nz = doc::parse("-0.0");
    CHECK(nz.type() == Type::Double && std::signbit(nz.asDouble()) && nz.asDouble() == 0.0);
    expectRoundTrip("-0.0", "-0.0");
    expectRoundTrip("1e308", "1e+308");
    expectRoundTrip("5e-324", "5e-324");
    expectRoundTrip("1.0", "1.0");
    expectRoundTrip("100.0", "100.0");
    expectRoundTrip("1E2", "100.0");
    expectRoundTrip("2.5e-3", "0.0025");
    expectRoundTrip("0.1", "0.1");
    expectRoundTrip("1.7976931348623157e308", "1.7976931348623157e+308");
    CHECK(doc::parse("5e-324").asDouble() == std::numeric_limits<double>::denorm_min());
    CHECK(doc::parse("1e308").asDouble() == 1e308);
    expectRejected("1e309");
    expectRejected("-1e400");
    expectRejected("1e-400"); // underflows below the smallest denormal
    // Every double survives serialize -> parse with the same bits.
    std::mt19937_64 rng(5);
    for (int i = 0; i < 2000; ++i) {
        uint64_t bits = rng();
        double d;
        std::memcpy(&d, &bits, sizeof(d));
        if (!std::isfinite(d)) continue;
        Value back = doc::parse(doc::serialize(Value::number(d)));
        CHECK(back.type() == Type::Double && back == Value::number(d));
    }
    bool threw = false;
    try {
        (void)Value::number(std::nan(""));
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
    pass("P4.A5 numbers: int64 min/max ok, beyond rejected; -0.0, 1e308, 5e-324 exact");
}

static std::string nested(int depth, bool objects) {
    std::string s;
    for (int i = 0; i < depth; ++i) s += objects ? "{\"a\":" : "[";
    s += "1";
    for (int i = 0; i < depth; ++i) s += objects ? "}" : "]";
    return s;
}

static void testA6Depth() {
    banner("P4.A6 Depth 64 accepted, 65 rejected");
    for (bool objects : {false, true}) {
        Value v = doc::parse(nested(64, objects));
        CHECK(doc::depth(v) == 64);
        CHECK(doc::serialize(v) == nested(64, objects));
        expectRejected(nested(65, objects));
    }
    expectRejected("[" + nested(64, true) + "]"); // mixed nesting counts both kinds
    pass("P4.A6 depth 64 ok, 65 rejected (objects and arrays)");
}

static void testA7MaxSize() {
    banner("P4.A7 Largest document accepted, one byte more rejected");
    // {"k":"xxx...x"} with exactly kMaxDocBytes bytes.
    const size_t overhead = std::strlen("{\"k\":\"\"}");
    std::string text = "{\"k\":\"" + std::string(doc::kMaxDocBytes - overhead, 'x') + "\"}";
    CHECK(text.size() == doc::kMaxDocBytes);
    Value v = doc::parse(text);
    CHECK(v.find("k")->asString().size() == doc::kMaxDocBytes - overhead);
    text.insert(6, "x");
    CHECK(text.size() == doc::kMaxDocBytes + 1);
    expectRejected(text);
    CHECK(!parseFails(std::string(" 1 ")));
    pass("P4.A7 max size ok, +1 byte rejected", std::to_string(doc::kMaxDocBytes) + " bytes");
}

static void testA8Utf8() {
    banner("P4.A8 Multi-byte UTF-8 keys and values");
    const std::string text = "{\"\xD0\xBA\xD0\xBB\xD1\x8E\xD1\x87\":\"\xD0\xB7\xD0\xBD\xD0\xB0"
                             "\xD1\x87\",\"\xE6\x97\xA5\xE6\x9C\xAC\":\"\xE8\xAA\x9E\","
                             "\"e\xF0\x9F\x98\x80\":\"\xF0\x9F\x8E\x89\"}";
    expectRoundTrip(text, text);
    Value v = doc::parse(text);
    CHECK(v.find("\xE6\x97\xA5\xE6\x9C\xAC")->asString() == "\xE8\xAA\x9E");
    CHECK(v.keyAt(2) == "e\xF0\x9F\x98\x80");
    bool threw = false;
    try {
        (void)Value::string("\xC3"); // truncated sequence
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
    threw = false;
    try {
        Value o = Value::object();
        o.set("\xFF", Value());
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
    pass("P4.A8 multi-byte UTF-8 keys/values round-trip");
}

int main() {
    std::string dir = setupRun("doc_test");
    Cleaner cleaner{dir};
    testA1RoundTrip();
    testA2Types();
    testA3Escapes();
    testA4Rejects();
    testA5Numbers();
    testA6Depth();
    testA7MaxSize();
    testA8Utf8();
    printBudget();
    return 0;
}
