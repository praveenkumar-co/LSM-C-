// Phase 4 document tests: the JSON parser and serializer (P4.A), paths (P4.B) and update ops
// (P4.C1-C5, C7) checked against expected documents. No disk writes.
#include "test_common.hpp"

#include "doc/Json.hpp"
#include "doc/Update.hpp"

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

// ------------------------------------------------------------------------------------------
// Paths and ops. Every case applies ops to a copy of `before` and compares the result with the
// expected document; error cases name the exception type.
// ------------------------------------------------------------------------------------------
using doc::UpdateOp;
using Ops = std::vector<UpdateOp>;

static std::string applied(const std::string& before, const Ops& ops) {
    Value v = doc::parse(before);
    doc::applyOps(v, ops);
    return doc::serialize(v);
}

static void expectApply(const std::string& before, const Ops& ops, const std::string& after) {
    const std::string got = applied(before, ops);
    const std::string want = doc::serialize(doc::parse(after));
    if (got != want) {
        std::cerr << "apply on " << before << " gave " << got << ", expected " << want << "\n";
        CHECK(false);
    }
}

template <typename E> static void expectThrows(const std::string& before, const Ops& ops) {
    try {
        (void)applied(before, ops);
    } catch (const E&) {
        return;
    } catch (const std::exception& e) {
        std::cerr << "apply on " << before << " threw the wrong error: " << e.what() << "\n";
        CHECK(false);
    }
    std::cerr << "apply on " << before << " did not throw\n";
    CHECK(false);
}

static void testB1ValidPaths() {
    banner("P4.B1 Valid paths");
    CHECK(doc::splitPath("a") == std::vector<std::string>{"a"});
    CHECK((doc::splitPath("a.b.c") == std::vector<std::string>{"a", "b", "c"}));
    CHECK((doc::splitPath("items.0") == std::vector<std::string>{"items", "0"}));
    CHECK((doc::splitPath("\xD0\xBA.\xE6\x97\xA5") ==
           std::vector<std::string>{"\xD0\xBA", "\xE6\x97\xA5"}));
    expectApply("{\"items\":[1,2]}", {doc::set("items.0", Value::integer(9))}, "{\"items\":[9,2]}");
    expectApply("{}", {doc::set("\xD0\xBA.\xE6\x97\xA5", Value::boolean(true))},
                "{\"\xD0\xBA\":{\"\xE6\x97\xA5\":true}}");
    // A numeric segment on an object is a key.
    expectApply("{\"m\":{}}", {doc::set("m.0", Value::integer(1)), doc::set("m.01", Value())},
                "{\"m\":{\"0\":1,\"01\":null}}");
    pass("P4.B1 valid paths: \"a\", \"a.b.c\", \"items.0\", Unicode keys");
}

static void testB2InvalidPaths() {
    banner("P4.B2 Invalid paths");
    int n = 0;
    for (const char* bad : {"", ".", "a.", ".a", "a..b", ".."}) {
        expectThrows<doc::PathError>("{\"a\":1}", {doc::set(bad, Value())});
        ++n;
    }
    for (const char* bad :
         {"items.-1", "items.01", "items.1e2", "items.x", "items.+1", "items. 1", "items.-1.a"}) {
        expectThrows<doc::PathError>("{\"items\":[1,2,3]}", {doc::set(bad, Value())});
        expectThrows<doc::PathError>("{\"items\":[1,2,3]}", {doc::unset(bad)});
        ++n;
    }
    expectThrows<doc::PathError>("{}", {doc::set(std::string(1025, 'k'), Value())});
    expectThrows<doc::PathError>("{}", {doc::set("a.\xFF", Value())});
    pass("P4.B2 invalid paths rejected (\"\", \".\", \"a.\", \".a\", \"a..b\", \"items.-1\", "
         "\"items.01\", \"items.1e2\")",
         std::to_string(n + 2) + " cases");
}

static void testB3OutOfRange() {
    banner("P4.B3 Array index out of range");
    const std::string d = "{\"items\":[{\"k\":1},2]}";
    expectThrows<doc::RangeError>(d, {doc::set("items.2", Value())});
    expectThrows<doc::RangeError>(d, {doc::set("items.5.k", Value())});
    expectThrows<doc::RangeError>(d, {doc::inc("items.2", 1)});
    expectThrows<doc::RangeError>(d, {doc::unset("items.9")});
    expectThrows<doc::RangeError>(d, {doc::set("items.99999999999999999", Value())});
    expectThrows<doc::RangeError>("{\"e\":[]}", {doc::set("e.0", Value())});
    expectApply(d, {doc::set("items.1", Value::integer(3)), doc::inc("items.0.k", 1)},
                "{\"items\":[{\"k\":2},3]}");
    pass("P4.B3 array index out of range -> RangeError");
}

static void testB4SegmentLimit() {
    banner("P4.B4 64 path segments accepted, 65 rejected");
    std::string p64 = "s";
    for (int i = 1; i < 64; ++i) p64 += ".s";
    CHECK(doc::splitPath(p64).size() == 64);
    Value v = Value::object();
    doc::applyOps(v, {doc::set(p64, Value::integer(1))});
    CHECK(doc::depth(v) == 64);
    expectThrows<doc::PathError>("{}", {doc::set(p64 + ".s", Value())});
    // 64 segments with an object value would make the document 65 levels deep.
    expectThrows<doc::RangeError>("{}", {doc::set(p64, Value::object())});
    pass("P4.B4 64 path segments ok, 65 rejected");
}

static void testC1Set() {
    banner("P4.C1 $set");
    expectApply("{\"a\":1}", {doc::set("b", Value::string("x"))}, "{\"a\":1,\"b\":\"x\"}");
    expectApply("{\"a\":1,\"b\":2}", {doc::set("a", Value::integer(5))}, "{\"a\":5,\"b\":2}");
    expectApply("{\"a\":1}", {doc::set("a", doc::parse("{\"x\":[1]}"))}, "{\"a\":{\"x\":[1]}}");
    expectApply("{\"a\":{\"x\":1}}", {doc::set("a", Value::string("s"))}, "{\"a\":\"s\"}");
    expectApply("{\"a\":1}", {doc::set("a", Value::number(1.0))}, "{\"a\":1.0}");
    expectApply("{}", {doc::set("a.b.c", Value::integer(1))}, "{\"a\":{\"b\":{\"c\":1}}}");
    expectApply("{\"a\":{\"z\":0}}", {doc::set("a.b.c", Value())},
                "{\"a\":{\"z\":0,\"b\":{\"c\":null}}}");
    expectApply("{\"l\":[{\"n\":1},{\"n\":2}]}", {doc::set("l.1.name", Value::string("b"))},
                "{\"l\":[{\"n\":1},{\"n\":2,\"name\":\"b\"}]}");
    expectApply("{\"a\":1}", {doc::set("a", Value::integer(1))}, "{\"a\":1}");
    pass("P4.C1 $set: new, overwrite, type change, nested auto-create");
}

static void testC2SubfieldOnPrimitive() {
    banner("P4.C2 Subfield on a primitive");
    for (const char* d :
         {"{\"a\":5}", "{\"a\":\"s\"}", "{\"a\":null}", "{\"a\":true}", "{\"a\":1.5}"}) {
        expectThrows<doc::TypeError>(d, {doc::set("a.b", Value::integer(1))});
        expectThrows<doc::TypeError>(d, {doc::set("a.b.c", Value::integer(1))});
        expectThrows<doc::TypeError>(d, {doc::inc("a.b", 1)});
        expectApply(d, {doc::unset("a.b")}, d); // nothing to unset below a primitive
    }
    expectThrows<doc::TypeError>("{\"a\":[1,2]}", {doc::set("a.0.x", Value())});
    pass("P4.C2 subfield on a primitive -> TypeError ($unset: no-op)");
}

static void testC3Unset() {
    banner("P4.C3 $unset");
    expectApply("{\"a\":1,\"b\":2,\"c\":3}", {doc::unset("b")}, "{\"a\":1,\"c\":3}");
    expectApply("{\"a\":1}", {doc::unset("zz")}, "{\"a\":1}");
    expectApply("{\"a\":1}", {doc::unset("x.y.z")}, "{\"a\":1}");
    expectApply("{\"a\":{\"b\":1,\"c\":2}}", {doc::unset("a.b")}, "{\"a\":{\"c\":2}}");
    expectApply("{\"l\":[{\"k\":1,\"j\":2}]}", {doc::unset("l.0.k")}, "{\"l\":[{\"j\":2}]}");
    expectThrows<doc::TypeError>("{\"l\":[1,2]}", {doc::unset("l.0")});
    pass("P4.C3 $unset: existing, missing (no change), nested; array element -> TypeError");
}

static void testC4Inc() {
    banner("P4.C4 $inc");
    const int64_t mx = std::numeric_limits<int64_t>::max();
    const int64_t mn = std::numeric_limits<int64_t>::min();
    expectApply("{}", {doc::inc("n", 5)}, "{\"n\":5}");
    expectApply("{}", {doc::inc("n", 2.5)}, "{\"n\":2.5}");
    expectApply("{}", {doc::inc("a.b", 1)}, "{\"a\":{\"b\":1}}");
    expectApply("{\"n\":40}", {doc::inc("n", 2)}, "{\"n\":42}");
    expectApply("{\"n\":40}", {doc::inc("n", -50)}, "{\"n\":-10}");
    expectApply("{\"n\":1}", {doc::inc("n", 0.5)}, "{\"n\":1.5}");
    expectApply("{\"n\":1}", {doc::inc("n", 1.0)}, "{\"n\":2.0}"); // any double -> double
    expectApply("{\"n\":1.5}", {doc::inc("n", 1)}, "{\"n\":2.5}");
    expectApply("{\"n\":0.25}", {doc::inc("n", 0.5)}, "{\"n\":0.75}");
    expectApply("{\"n\":" + std::to_string(mx - 1) + "}", {doc::inc("n", 1)},
                "{\"n\":" + std::to_string(mx) + "}");
    for (const char* d :
         {"{\"n\":\"5\"}", "{\"n\":null}", "{\"n\":true}", "{\"n\":[1]}", "{\"n\":{}}"}) {
        expectThrows<doc::TypeError>(d, {doc::inc("n", 1)});
        expectThrows<doc::TypeError>(d, {doc::inc("n", 1.0)});
    }
    expectThrows<doc::RangeError>("{\"n\":" + std::to_string(mx) + "}", {doc::inc("n", 1)});
    expectThrows<doc::RangeError>("{\"n\":" + std::to_string(mn) + "}", {doc::inc("n", -1)});
    expectThrows<doc::RangeError>("{\"n\":1e308}", {doc::inc("n", 1e308)});
    bool threw = false;
    try {
        (void)doc::inc("n", uint64_t(1) << 63);
    } catch (const doc::RangeError&) {
        threw = true;
    }
    CHECK(threw);
    pass("P4.C4 $inc: missing, int+int, int+double, double+double; non-number -> TypeError; "
         "INT64_MAX+1, INT64_MIN-1 -> RangeError");
}

static void testC5Conflicts() {
    banner("P4.C5 Overlapping paths in one update");
    const std::string d = "{\"a\":{\"b\":1,\"bc\":2}}";
    expectThrows<doc::ConflictError>(d, {doc::set("a", Value()), doc::set("a.b", Value())});
    expectThrows<doc::ConflictError>(d, {doc::set("a.b", Value()), doc::set("a", Value())});
    expectThrows<doc::ConflictError>(d, {doc::inc("a.b", 1), doc::inc("a.b", 1)});
    expectThrows<doc::ConflictError>(d, {doc::unset("a.b"), doc::set("a.b.c", Value())});
    expectThrows<doc::ConflictError>(
        d, {doc::set("x", Value()), doc::set("y.z", Value()), doc::unset("y")});
    expectApply(d, {doc::inc("a.b", 1), doc::inc("a.bc", 1)}, "{\"a\":{\"b\":2,\"bc\":3}}");
    expectApply(d, {doc::set("a.b", Value()), doc::set("a.c", Value())},
                "{\"a\":{\"b\":null,\"bc\":2,\"c\":null}}");
    pass("P4.C5 conflicts: (\"a\",\"a.b\"), (\"a.b\",\"a\"), same path twice; (\"a.b\",\"a.bc\") "
         "allowed");
}

static void testC7ThousandOps() {
    banner("P4.C7 One update with 1,000 ops");
    Ops ops;
    Value expect = doc::parse("{\"keep\":true,\"n\":{}}");
    for (int i = 0; i < 1000; ++i) {
        const std::string k = "f" + std::to_string(i);
        switch (i % 3) {
        case 0:
            ops.push_back(doc::set(k, Value::integer(i)));
            expect.set(k, Value::integer(i));
            break;
        case 1:
            ops.push_back(doc::inc("n." + k, i));
            expect.find("n")->set(k, Value::integer(i));
            break;
        default:
            ops.push_back(doc::set("o." + k, Value::string(k)));
            if (!expect.find("o")) expect.set("o", Value::object());
            expect.find("o")->set(k, Value::string(k));
        }
    }
    Value v = doc::parse("{\"keep\":true,\"n\":{}}");
    doc::applyOps(v, ops);
    CHECK(v == expect);
    CHECK(v.size() == 337); // keep, n, o, and 334 top-level f-keys
    pass("P4.C7 one update with 1,000 ops");
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
    testB1ValidPaths();
    testB2InvalidPaths();
    testB3OutOfRange();
    testB4SegmentLimit();
    testC1Set();
    testC2SubfieldOnPrimitive();
    testC3Unset();
    testC4Inc();
    testC5Conflicts();
    testC7ThousandOps();
    printBudget();
    return 0;
}
