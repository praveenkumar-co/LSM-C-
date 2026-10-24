#pragma once
// Json.hpp - documents as JSON: an owned value tree, a strict RFC 8259 parser and a canonical
// serializer. Object keys keep insertion order, so the same tree always serializes to the same
// bytes (delta updates rely on this). Int (int64) and Double are distinct types.

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace doc {

constexpr int kMaxDepth = 64; // nested arrays/objects; a scalar has depth 0
// Largest vLog payload: 16 MiB entry - 32-byte entry header - 8 bytes for the smallest padded key.
// IndexedStore static_asserts that this matches the vLog's limits.
constexpr size_t kMaxDocBytes = 16u * 1024 * 1024 - 32 - 8;

class ParseError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

enum class Type : uint8_t { Null, Bool, Int, Double, String, Array, Object };

// True if s is well-formed UTF-8 (no overlong forms, no surrogates, nothing above U+10FFFF).
inline bool validUtf8(std::string_view s) {
    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    const auto* end = p + s.size();
    while (p < end) {
        const unsigned char c = *p;
        if (c < 0x80) {
            ++p;
            continue;
        }
        int len;
        unsigned char lo = 0x80, hi = 0xBF; // allowed range of the second byte
        if (c >= 0xC2 && c <= 0xDF) {
            len = 2;
        } else if (c >= 0xE0 && c <= 0xEF) {
            len = 3;
            if (c == 0xE0) lo = 0xA0; // overlong
            if (c == 0xED) hi = 0x9F; // UTF-16 surrogates
        } else if (c >= 0xF0 && c <= 0xF4) {
            len = 4;
            if (c == 0xF0) lo = 0x90; // overlong
            if (c == 0xF4) hi = 0x8F; // above U+10FFFF
        } else {
            return false;
        }
        if (end - p < len || p[1] < lo || p[1] > hi) return false;
        for (int i = 2; i < len; ++i)
            if (p[i] < 0x80 || p[i] > 0xBF) return false;
        p += len;
    }
    return true;
}

class Value {
public:
    Value() = default; // null

    static Value boolean(bool b) {
        Value v(Type::Bool);
        v.b_ = b;
        return v;
    }
    static Value integer(int64_t i) {
        Value v(Type::Int);
        v.i_ = i;
        return v;
    }
    // Throws std::invalid_argument for NaN or infinity (JSON cannot represent them).
    static Value number(double d) {
        if (!std::isfinite(d)) throw std::invalid_argument("JSON numbers must be finite");
        Value v(Type::Double);
        v.d_ = d;
        return v;
    }
    // Throws std::invalid_argument if s is not valid UTF-8.
    static Value string(std::string s) {
        if (!validUtf8(s)) throw std::invalid_argument("JSON strings must be valid UTF-8");
        return rawString(std::move(s));
    }
    static Value array() { return Value(Type::Array); }
    static Value object() { return Value(Type::Object); }

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isNumber() const { return type_ == Type::Int || type_ == Type::Double; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    bool asBool() const { return expect(Type::Bool).b_; }
    int64_t asInt() const { return expect(Type::Int).i_; }
    double asDouble() const { return expect(Type::Double).d_; }
    const std::string& asString() const { return expect(Type::String).s_; }

    // Arrays and objects.
    size_t size() const {
        if (type_ != Type::Array && type_ != Type::Object)
            throw std::logic_error("size() of a JSON scalar");
        return items_.size();
    }
    Value& at(size_t i) { return items_.at(i); }
    const Value& at(size_t i) const { return items_.at(i); }
    void push(Value v) {
        expect(Type::Array);
        items_.push_back(std::move(v));
    }

    // Objects.
    const std::string& keyAt(size_t i) const { return expect(Type::Object).keys_.at(i); }
    Value* find(std::string_view key) {
        expect(Type::Object);
        for (size_t i = 0; i < keys_.size(); ++i)
            if (keys_[i] == key) return &items_[i];
        return nullptr;
    }
    const Value* find(std::string_view key) const { return const_cast<Value*>(this)->find(key); }
    // Replaces the value of an existing key in place, or appends the key. Returns the stored value.
    // Throws std::invalid_argument if key is not valid UTF-8.
    Value& set(std::string_view key, Value v) {
        if (Value* cur = find(key)) {
            *cur = std::move(v);
            return *cur;
        }
        if (!validUtf8(key)) throw std::invalid_argument("JSON keys must be valid UTF-8");
        keys_.emplace_back(key);
        items_.push_back(std::move(v));
        return items_.back();
    }
    bool erase(std::string_view key) {
        expect(Type::Object);
        for (size_t i = 0; i < keys_.size(); ++i) {
            if (keys_[i] == key) {
                keys_.erase(keys_.begin() + static_cast<std::ptrdiff_t>(i));
                items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(i));
                return true;
            }
        }
        return false;
    }

    // Same type and content, keys in the same order; doubles compared by bit pattern, so equal
    // values serialize to equal bytes (0.0 != -0.0, 1 != 1.0).
    bool operator==(const Value& o) const {
        if (type_ != o.type_) return false;
        switch (type_) {
        case Type::Null:
            return true;
        case Type::Bool:
            return b_ == o.b_;
        case Type::Int:
            return i_ == o.i_;
        case Type::Double:
            return std::memcmp(&d_, &o.d_, sizeof(d_)) == 0;
        case Type::String:
            return s_ == o.s_;
        case Type::Array:
        case Type::Object:
            return keys_ == o.keys_ && items_ == o.items_;
        }
        return false;
    }
    bool operator!=(const Value& o) const { return !(*this == o); }

private:
    friend class Parser;
    explicit Value(Type t) : type_(t) {}
    static Value rawString(std::string s) {
        Value v(Type::String);
        v.s_ = std::move(s);
        return v;
    }
    const Value& expect(Type t) const {
        if (type_ != t) throw std::logic_error("JSON value has a different type");
        return *this;
    }

    Type type_ = Type::Null;
    bool b_ = false;
    int64_t i_ = 0;
    double d_ = 0;
    std::string s_;
    std::vector<std::string> keys_; // object keys, parallel to items_
    std::vector<Value> items_;      // array elements or object values
};

// Nesting depth: 0 for a scalar, 1 + the deepest child for an array or object.
inline int depth(const Value& v) {
    if (!v.isArray() && !v.isObject()) return 0;
    int d = 0;
    for (size_t i = 0; i < v.size(); ++i) d = std::max(d, depth(v.at(i)));
    return d + 1;
}

class Parser {
public:
    explicit Parser(std::string_view in) : begin_(in.data()), p_(in.data()), end_(p_ + in.size()) {}

    Value parseDocument() {
        skipWs();
        Value v = parseValue(0);
        skipWs();
        if (p_ != end_) fail("trailing characters after the JSON value");
        return v;
    }

private:
    [[noreturn]] void fail(const std::string& what) const {
        throw ParseError(what + " at byte " + std::to_string(p_ - begin_));
    }
    void skipWs() {
        while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')) ++p_;
    }
    void expectChar(char c) {
        if (p_ == end_ || *p_ != c) fail(std::string("expected '") + c + "'");
        ++p_;
    }
    void literal(const char* word) {
        const size_t n = std::strlen(word);
        if (static_cast<size_t>(end_ - p_) < n || std::memcmp(p_, word, n) != 0)
            fail("invalid literal");
        p_ += n;
    }

    Value parseValue(int depth) {
        if (p_ == end_) fail("unexpected end of input");
        switch (*p_) {
        case '{':
            return parseObject(depth + 1);
        case '[':
            return parseArray(depth + 1);
        case '"':
            return Value::rawString(parseString());
        case 't':
            literal("true");
            return Value::boolean(true);
        case 'f':
            literal("false");
            return Value::boolean(false);
        case 'n':
            literal("null");
            return Value();
        default:
            if (*p_ == '-' || (*p_ >= '0' && *p_ <= '9')) return parseNumber();
            fail("unexpected character");
        }
    }

    Value parseObject(int depth) {
        if (depth > kMaxDepth) fail("nesting deeper than 64 levels");
        ++p_;
        Value obj(Type::Object);
        skipWs();
        if (p_ < end_ && *p_ == '}') {
            ++p_;
            return obj;
        }
        while (true) {
            skipWs();
            if (p_ == end_ || *p_ != '"') fail("expected a string key");
            std::string key = parseString();
            skipWs();
            expectChar(':');
            skipWs();
            Value v = parseValue(depth);
            obj.keys_.push_back(std::move(key));
            obj.items_.push_back(std::move(v));
            skipWs();
            if (p_ == end_) fail("unterminated object");
            if (*p_ == ',') {
                ++p_;
                continue;
            }
            if (*p_ != '}') fail("expected ',' or '}'");
            ++p_;
            break;
        }
        checkNoDuplicateKeys(obj.keys_);
        return obj;
    }

    void checkNoDuplicateKeys(const std::vector<std::string>& keys) const {
        std::vector<const std::string*> sorted;
        sorted.reserve(keys.size());
        for (const std::string& k : keys) sorted.push_back(&k);
        std::sort(sorted.begin(), sorted.end(),
                  [](const std::string* a, const std::string* b) { return *a < *b; });
        for (size_t i = 1; i < sorted.size(); ++i)
            if (*sorted[i] == *sorted[i - 1]) fail("duplicate key \"" + *sorted[i] + "\"");
    }

    Value parseArray(int depth) {
        if (depth > kMaxDepth) fail("nesting deeper than 64 levels");
        ++p_;
        Value arr(Type::Array);
        skipWs();
        if (p_ < end_ && *p_ == ']') {
            ++p_;
            return arr;
        }
        while (true) {
            skipWs();
            arr.items_.push_back(parseValue(depth));
            skipWs();
            if (p_ == end_) fail("unterminated array");
            if (*p_ == ',') {
                ++p_;
                continue;
            }
            if (*p_ != ']') fail("expected ',' or ']'");
            ++p_;
            return arr;
        }
    }

    uint32_t hex4() {
        if (end_ - p_ < 4) fail("truncated \\u escape");
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = *p_++;
            v <<= 4;
            if (c >= '0' && c <= '9')
                v |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f')
                v |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                v |= static_cast<uint32_t>(c - 'A' + 10);
            else
                fail("invalid hex digit in \\u escape");
        }
        return v;
    }

    static void appendUtf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    std::string parseString() {
        ++p_; // opening quote
        std::string out;
        const char* run = p_; // start of unescaped bytes not yet copied
        while (true) {
            if (p_ == end_) fail("unterminated string");
            const unsigned char c = static_cast<unsigned char>(*p_);
            if (c == '"' || c == '\\') {
                out.append(run, static_cast<size_t>(p_ - run));
                if (c == '"') {
                    ++p_;
                    break;
                }
                ++p_;
                if (p_ == end_) fail("unterminated escape");
                const char e = *p_++;
                switch (e) {
                case '"':
                    out += '"';
                    break;
                case '\\':
                    out += '\\';
                    break;
                case '/':
                    out += '/';
                    break;
                case 'b':
                    out += '\b';
                    break;
                case 'f':
                    out += '\f';
                    break;
                case 'n':
                    out += '\n';
                    break;
                case 'r':
                    out += '\r';
                    break;
                case 't':
                    out += '\t';
                    break;
                case 'u': {
                    uint32_t cp = hex4();
                    if (cp >= 0xDC00 && cp <= 0xDFFF) fail("lone low surrogate");
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (end_ - p_ < 2 || p_[0] != '\\' || p_[1] != 'u')
                            fail("high surrogate without a low surrogate");
                        p_ += 2;
                        const uint32_t lo = hex4();
                        if (lo < 0xDC00 || lo > 0xDFFF)
                            fail("high surrogate without a low surrogate");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default:
                    fail("invalid escape");
                }
                run = p_;
                continue;
            }
            if (c < 0x20) fail("raw control character in string");
            ++p_;
        }
        if (!validUtf8(out)) fail("invalid UTF-8 in string");
        return out;
    }

    Value parseNumber() {
        const char* start = p_;
        if (*p_ == '-') ++p_;
        if (p_ == end_) fail("invalid number");
        if (*p_ == '0') {
            ++p_;
            if (p_ < end_ && *p_ >= '0' && *p_ <= '9') fail("leading zero in number");
        } else if (*p_ >= '1' && *p_ <= '9') {
            while (p_ < end_ && *p_ >= '0' && *p_ <= '9') ++p_;
        } else {
            fail("invalid number");
        }
        bool is_int = true;
        if (p_ < end_ && *p_ == '.') {
            ++p_;
            if (p_ == end_ || *p_ < '0' || *p_ > '9') fail("digit expected after '.'");
            while (p_ < end_ && *p_ >= '0' && *p_ <= '9') ++p_;
            is_int = false;
        }
        if (p_ < end_ && (*p_ == 'e' || *p_ == 'E')) {
            ++p_;
            if (p_ < end_ && (*p_ == '+' || *p_ == '-')) ++p_;
            if (p_ == end_ || *p_ < '0' || *p_ > '9') fail("digit expected in exponent");
            while (p_ < end_ && *p_ >= '0' && *p_ <= '9') ++p_;
            is_int = false;
        }
        if (is_int) {
            int64_t v = 0;
            auto r = std::from_chars(start, p_, v);
            if (r.ec == std::errc::result_out_of_range) fail("integer outside the int64 range");
            if (r.ec != std::errc() || r.ptr != p_) fail("invalid integer");
            return Value::integer(v);
        }
        double d = 0;
        auto r = std::from_chars(start, p_, d, std::chars_format::general);
        // out_of_range covers overflow to infinity and underflow below the smallest denormal.
        if (r.ec == std::errc::result_out_of_range) fail("number outside the double range");
        if (r.ec != std::errc() || r.ptr != p_ || !std::isfinite(d)) fail("invalid number");
        return Value::number(d);
    }

    const char* begin_;
    const char* p_;
    const char* end_;
};

// Throws ParseError for anything that is not strict RFC 8259 JSON, for nesting deeper than 64,
// and for input longer than max_bytes.
inline Value parse(std::string_view text, size_t max_bytes = kMaxDocBytes) {
    if (text.size() > max_bytes)
        throw ParseError("document of " + std::to_string(text.size()) + " bytes exceeds the " +
                         std::to_string(max_bytes) + "-byte limit");
    return Parser(text).parseDocument();
}

namespace detail {
inline void writeString(const std::string& s, std::string& out) {
    static const char kHex[] = "0123456789abcdef";
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20) {
                out += "\\u00";
                out += kHex[c >> 4];
                out += kHex[c & 0xF];
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    out += '"';
}

inline void writeValue(const Value& v, std::string& out) {
    char buf[40];
    switch (v.type()) {
    case Type::Null:
        out += "null";
        return;
    case Type::Bool:
        out += v.asBool() ? "true" : "false";
        return;
    case Type::Int: {
        auto r = std::to_chars(buf, buf + sizeof(buf), v.asInt());
        out.append(buf, r.ptr);
        return;
    }
    case Type::Double: {
        // Shortest form that parses back to the same bits. A '.' or 'e' is always present so the
        // value reads back as a Double, not an Int ("1.0", "-0.0", "1e+308").
        auto r = std::to_chars(buf, buf + sizeof(buf), v.asDouble());
        std::string_view sv(buf, static_cast<size_t>(r.ptr - buf));
        out += sv;
        if (sv.find_first_of(".e") == std::string_view::npos) out += ".0";
        return;
    }
    case Type::String:
        writeString(v.asString(), out);
        return;
    case Type::Array:
        out += '[';
        for (size_t i = 0; i < v.size(); ++i) {
            if (i) out += ',';
            writeValue(v.at(i), out);
        }
        out += ']';
        return;
    case Type::Object:
        out += '{';
        for (size_t i = 0; i < v.size(); ++i) {
            if (i) out += ',';
            writeString(v.keyAt(i), out);
            out += ':';
            writeValue(v.at(i), out);
        }
        out += '}';
        return;
    }
}
} // namespace detail

// Canonical form: no whitespace, keys in insertion order, minimal escaping, UTF-8 as is.
inline std::string serialize(const Value& v) {
    std::string out;
    detail::writeValue(v, out);
    return out;
}

} // namespace doc
