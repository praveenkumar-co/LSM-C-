#pragma once
// Update.hpp - update operators on JSON documents: $set, $unset and $inc on dotted paths
// ("profile.score", "items.0"). The same code applies ops when an update is written and when a
// Delta entry is replayed at read time, so the two always produce the same document.
//
// Path rules: segments are split on '.', so object keys containing '.' cannot be addressed. On an
// object a segment is a key (also "0"); on an array it must be a canonical index ("0", "12").
// $set and $inc create missing intermediate objects; $unset of a missing path is a no-op.

#include "doc/Json.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace doc {

constexpr size_t kMaxPathBytes = 1024;
constexpr size_t kMaxPathSegments = 64;

class UpdateError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
class NotFoundError : public UpdateError { // update() on a missing or deleted key
public:
    using UpdateError::UpdateError;
};
class TypeError : public UpdateError { // wrong type at a path, or the document is not an object
public:
    using UpdateError::UpdateError;
};
class ConflictError : public UpdateError { // two ops in one update touch overlapping paths
public:
    using UpdateError::UpdateError;
};
class RangeError : public UpdateError { // index out of range, int64 overflow, depth or size
public:
    using UpdateError::UpdateError;
};
class PathError : public UpdateError { // malformed path or array index
public:
    using UpdateError::UpdateError;
};

struct UpdateOp {
    enum class Kind : uint8_t { Set, Unset, IncI64, IncF64 };
    Kind kind;
    std::string path;
    Value value; // Set: the new value; IncI64: Int; IncF64: Double; Unset: unused
};

inline UpdateOp set(std::string path, Value v) {
    return UpdateOp{UpdateOp::Kind::Set, std::move(path), std::move(v)};
}
inline UpdateOp unset(std::string path) {
    return UpdateOp{UpdateOp::Kind::Unset, std::move(path), Value()};
}
template <typename I, std::enable_if_t<std::is_integral_v<I> && !std::is_same_v<I, bool>, int> = 0>
inline UpdateOp inc(std::string path, I by) {
    if constexpr (std::is_unsigned_v<I>) {
        if (static_cast<uint64_t>(by) > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
            throw RangeError("$inc amount does not fit in int64");
    }
    return UpdateOp{UpdateOp::Kind::IncI64, std::move(path),
                    Value::integer(static_cast<int64_t>(by))};
}
inline UpdateOp inc(std::string path, double by) {
    if (!std::isfinite(by)) throw RangeError("$inc amount must be finite");
    return UpdateOp{UpdateOp::Kind::IncF64, std::move(path), Value::number(by)};
}

// Throws PathError for "", empty segments ("a..b", ".a", "a."), more than 64 segments, more
// than 1024 bytes, or invalid UTF-8.
inline std::vector<std::string> splitPath(std::string_view path) {
    if (path.empty()) throw PathError("empty path");
    if (path.size() > kMaxPathBytes)
        throw PathError("path longer than " + std::to_string(kMaxPathBytes) + " bytes");
    if (!validUtf8(path)) throw PathError("path is not valid UTF-8");
    std::vector<std::string> segs;
    size_t start = 0;
    while (true) {
        const size_t dot = path.find('.', start);
        const size_t end = dot == std::string_view::npos ? path.size() : dot;
        if (end == start) throw PathError("empty segment in path \"" + std::string(path) + "\"");
        segs.emplace_back(path.substr(start, end - start));
        if (segs.size() > kMaxPathSegments)
            throw PathError("path has more than " + std::to_string(kMaxPathSegments) + " segments");
        if (dot == std::string_view::npos) break;
        start = dot + 1;
    }
    return segs;
}

namespace detail {

struct ParsedOp {
    UpdateOp::Kind kind;
    std::vector<std::string> segs;
    const UpdateOp* op;
};

inline const char* typeName(const Value& v) {
    switch (v.type()) {
    case Type::Null:
        return "null";
    case Type::Bool:
        return "bool";
    case Type::Int:
        return "int";
    case Type::Double:
        return "double";
    case Type::String:
        return "string";
    case Type::Array:
        return "array";
    case Type::Object:
        return "object";
    }
    return "?";
}

// A segment used on an array must be a canonical decimal index ("0", "7", "12"; not "-1", "01",
// "1e2"). Throws PathError if it is not, RangeError if it is past the end.
inline size_t arrayIndex(const std::string& seg, size_t size, const std::string& path) {
    const bool canonical =
        !seg.empty() && seg.size() <= 19 &&
        std::all_of(seg.begin(), seg.end(), [](char c) { return c >= '0' && c <= '9'; }) &&
        (seg.size() == 1 || seg[0] != '0');
    if (!canonical)
        throw PathError("\"" + seg + "\" is not an array index (path \"" + path + "\")");
    const uint64_t idx = std::stoull(seg);
    if (idx >= size)
        throw RangeError("array index " + seg + " out of range (size " + std::to_string(size) +
                         ", path \"" + path + "\")");
    return static_cast<size_t>(idx);
}

inline Value incResult(const Value* cur, const Value& by, const std::string& path) {
    if (cur == nullptr) { // missing field: 0 + by
        return by.type() == Type::Int ? Value::integer(by.asInt())
                                      : Value::number(0.0 + by.asDouble());
    }
    if (!cur->isNumber())
        throw TypeError(std::string("$inc on a ") + typeName(*cur) + " at \"" + path + "\"");
    if (cur->type() == Type::Int && by.type() == Type::Int) {
        int64_t r;
        if (__builtin_add_overflow(cur->asInt(), by.asInt(), &r))
            throw RangeError("$inc overflows int64 at \"" + path + "\"");
        return Value::integer(r);
    }
    const double a = cur->type() == Type::Int ? static_cast<double>(cur->asInt()) : cur->asDouble();
    const double b = by.type() == Type::Int ? static_cast<double>(by.asInt()) : by.asDouble();
    const double r = a + b;
    if (!std::isfinite(r)) throw RangeError("$inc overflows double at \"" + path + "\"");
    return Value::number(r);
}

inline void applyOne(Value& root, const ParsedOp& p) {
    const std::string& path = p.op->path;
    const bool creates = p.kind != UpdateOp::Kind::Unset;
    Value* cur = &root;
    for (size_t i = 0; i + 1 < p.segs.size(); ++i) {
        const std::string& seg = p.segs[i];
        if (cur->isObject()) {
            Value* child = cur->find(seg);
            if (child == nullptr) {
                if (!creates) return; // $unset of a missing path
                child = &cur->set(seg, Value::object());
            }
            cur = child;
        } else if (cur->isArray()) {
            cur = &cur->at(arrayIndex(seg, cur->size(), path));
        } else {
            if (!creates) return; // nothing to unset below a primitive
            throw TypeError(std::string("cannot address a field inside a ") + typeName(*cur) +
                            " (path \"" + path + "\")");
        }
    }
    const std::string& last = p.segs.back();
    if (cur->isObject()) {
        switch (p.kind) {
        case UpdateOp::Kind::Set:
            cur->set(last, p.op->value);
            return;
        case UpdateOp::Kind::Unset:
            cur->erase(last);
            return;
        case UpdateOp::Kind::IncI64:
        case UpdateOp::Kind::IncF64: {
            Value r = incResult(cur->find(last), p.op->value, path);
            cur->set(last, std::move(r));
            return;
        }
        }
    }
    if (cur->isArray()) {
        Value& target = cur->at(arrayIndex(last, cur->size(), path));
        switch (p.kind) {
        case UpdateOp::Kind::Set:
            target = p.op->value;
            return;
        case UpdateOp::Kind::Unset:
            throw TypeError("$unset cannot remove an array element (path \"" + path + "\")");
        case UpdateOp::Kind::IncI64:
        case UpdateOp::Kind::IncF64:
            target = incResult(&target, p.op->value, path);
            return;
        }
    }
    if (!creates) return;
    throw TypeError(std::string("cannot address a field inside a ") + typeName(*cur) + " (path \"" +
                    path + "\")");
}

} // namespace detail

// Checks paths, op values and overlaps. Throws PathError, TypeError (an Inc op whose value is not
// Int/Double as its kind says), RangeError (a $set value deeper than 64) or ConflictError (two
// paths equal, or one a segment-wise prefix of the other: "a" and "a.b"; "a.b" and "a.bc" are
// fine).
inline std::vector<detail::ParsedOp> validateOps(const std::vector<UpdateOp>& ops) {
    std::vector<detail::ParsedOp> parsed;
    parsed.reserve(ops.size());
    for (const UpdateOp& op : ops) {
        switch (op.kind) {
        case UpdateOp::Kind::Set:
            if (depth(op.value) > kMaxDepth) throw RangeError("$set value deeper than 64 levels");
            break;
        case UpdateOp::Kind::Unset:
            break;
        case UpdateOp::Kind::IncI64:
            if (op.value.type() != Type::Int) throw TypeError("IncI64 op without an int value");
            break;
        case UpdateOp::Kind::IncF64:
            if (op.value.type() != Type::Double)
                throw TypeError("IncF64 op without a double value");
            break;
        default:
            throw TypeError("unknown update op");
        }
        parsed.push_back(detail::ParsedOp{op.kind, splitPath(op.path), &op});
    }
    // After sorting by segments, a path that overlaps another sits right before one it overlaps.
    std::vector<const detail::ParsedOp*> sorted;
    for (const auto& p : parsed) sorted.push_back(&p);
    std::sort(
        sorted.begin(), sorted.end(),
        [](const detail::ParsedOp* a, const detail::ParsedOp* b) { return a->segs < b->segs; });
    for (size_t i = 1; i < sorted.size(); ++i) {
        const auto& a = sorted[i - 1]->segs;
        const auto& b = sorted[i]->segs;
        if (a.size() <= b.size() && std::equal(a.begin(), a.end(), b.begin()))
            throw ConflictError("paths \"" + sorted[i - 1]->op->path + "\" and \"" +
                                sorted[i]->op->path + "\" overlap in one update");
    }
    return parsed;
}

// Applies ops in order to `document`, which must be an object (else TypeError). On any error
// `document` may be partly modified: apply to a copy when the original must survive.
inline void applyOps(Value& document, const std::vector<UpdateOp>& ops) {
    if (!document.isObject())
        throw TypeError(std::string("document is a ") + detail::typeName(document) +
                        ", not an object");
    std::vector<detail::ParsedOp> parsed = validateOps(ops);
    for (const detail::ParsedOp& p : parsed) detail::applyOne(document, p);
    if (depth(document) > kMaxDepth) throw RangeError("document deeper than 64 levels");
}

} // namespace doc
