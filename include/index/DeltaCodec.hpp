#pragma once
// DeltaCodec.hpp - converts update ops to and from the payload of a vLog Delta entry, using the
// existing on-disk structs (vlog::DeltaPrefix, vlog::DeltaOpHeader):
//
//   payload = DeltaPrefix | op_count x (DeltaOpHeader | path | value | zero pad to 8)
//
//   $set   -> opcode Set,    encoding JsonText, value = canonical JSON
//   $unset -> opcode Unset,  encoding None,     no value
//   $inc   -> opcode IncI64, encoding I64,      8 bytes little-endian
//          -> opcode IncF64, encoding F64,      8 bytes IEEE-754 little-endian
//
// VLogWriter::append writes the DeltaPrefix itself, so encodeOps returns only the ops part.

#include "doc/Json.hpp"
#include "doc/Update.hpp"
#include "vlog/VLogFormat.hpp"

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace bpt {

// Malformed Delta payload bytes.
class DeltaFormatError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct DecodedDelta {
    vlog::DeltaPrefix prefix;
    std::vector<doc::UpdateOp> ops;
};

inline std::string encodeOps(const std::vector<doc::UpdateOp>& ops) {
    std::string out;
    for (const doc::UpdateOp& op : ops) {
        vlog::DeltaOpHeader h{};
        std::string value;
        switch (op.kind) {
        case doc::UpdateOp::Kind::Set:
            h.opcode = static_cast<uint8_t>(vlog::DeltaOpCode::Set);
            h.encoding = static_cast<uint8_t>(vlog::ValueEncoding::JsonText);
            value = doc::serialize(op.value);
            break;
        case doc::UpdateOp::Kind::Unset:
            h.opcode = static_cast<uint8_t>(vlog::DeltaOpCode::Unset);
            h.encoding = static_cast<uint8_t>(vlog::ValueEncoding::None);
            break;
        case doc::UpdateOp::Kind::IncI64: {
            h.opcode = static_cast<uint8_t>(vlog::DeltaOpCode::IncI64);
            h.encoding = static_cast<uint8_t>(vlog::ValueEncoding::I64);
            const int64_t v = op.value.asInt();
            value.assign(reinterpret_cast<const char*>(&v), sizeof(v));
            break;
        }
        case doc::UpdateOp::Kind::IncF64: {
            h.opcode = static_cast<uint8_t>(vlog::DeltaOpCode::IncF64);
            h.encoding = static_cast<uint8_t>(vlog::ValueEncoding::F64);
            const double v = op.value.asDouble();
            value.assign(reinterpret_cast<const char*>(&v), sizeof(v));
            break;
        }
        }
        if (op.path.size() > UINT16_MAX) throw doc::PathError("path too long to encode");
        h.path_len = static_cast<uint16_t>(op.path.size());
        h.value_len = static_cast<uint32_t>(value.size());
        const size_t start = out.size();
        out.append(reinterpret_cast<const char*>(&h), sizeof(h));
        out += op.path;
        out += value;
        out.resize(start + vlog::delta_op_size(h.path_len, h.value_len), '\0');
    }
    return out;
}

// Reads only the DeltaPrefix. False if the payload is too short to hold one.
inline bool readDeltaPrefix(const std::vector<uint8_t>& payload, vlog::DeltaPrefix* out) {
    if (payload.size() < sizeof(vlog::DeltaPrefix)) return false;
    std::memcpy(out, payload.data(), sizeof(*out));
    return true;
}

// Throws DeltaFormatError unless the payload is exactly a prefix plus op_count well-formed ops.
inline DecodedDelta decodeDelta(const std::vector<uint8_t>& payload) {
    DecodedDelta d;
    if (!readDeltaPrefix(payload, &d.prefix)) throw DeltaFormatError("payload shorter than prefix");
    size_t off = sizeof(vlog::DeltaPrefix);
    const uint16_t op_count = d.prefix.op_count;
    if (op_count == 0) throw DeltaFormatError("delta with no ops");
    d.ops.reserve(op_count);
    for (uint16_t i = 0; i < op_count; ++i) {
        vlog::DeltaOpHeader h;
        if (payload.size() - off < sizeof(h)) throw DeltaFormatError("truncated op header");
        std::memcpy(&h, payload.data() + off, sizeof(h));
        const uint64_t need = uint64_t(sizeof(h)) + h.path_len + h.value_len;
        if (payload.size() - off < need) throw DeltaFormatError("op runs past the payload");
        const char* p = reinterpret_cast<const char*>(payload.data() + off + sizeof(h));
        std::string path(p, h.path_len);
        std::string_view value(p + h.path_len, h.value_len);
        const auto opcode = static_cast<vlog::DeltaOpCode>(h.opcode);
        const auto enc = static_cast<vlog::ValueEncoding>(h.encoding);
        try {
            if (opcode == vlog::DeltaOpCode::Set && enc == vlog::ValueEncoding::JsonText) {
                d.ops.push_back(doc::set(std::move(path), doc::parse(value)));
            } else if (opcode == vlog::DeltaOpCode::Unset && enc == vlog::ValueEncoding::None &&
                       value.empty()) {
                d.ops.push_back(doc::unset(std::move(path)));
            } else if (opcode == vlog::DeltaOpCode::IncI64 && enc == vlog::ValueEncoding::I64 &&
                       value.size() == 8) {
                int64_t v;
                std::memcpy(&v, value.data(), 8);
                d.ops.push_back(doc::inc(std::move(path), v));
            } else if (opcode == vlog::DeltaOpCode::IncF64 && enc == vlog::ValueEncoding::F64 &&
                       value.size() == 8) {
                double v;
                std::memcpy(&v, value.data(), 8);
                d.ops.push_back(doc::inc(std::move(path), v));
            } else {
                throw DeltaFormatError("unknown opcode/encoding pair");
            }
        } catch (const doc::ParseError& e) {
            throw DeltaFormatError(std::string("bad $set value: ") + e.what());
        } catch (const doc::UpdateError& e) {
            throw DeltaFormatError(std::string("bad op: ") + e.what());
        }
        const size_t size = vlog::delta_op_size(h.path_len, h.value_len);
        if (payload.size() - off < size) throw DeltaFormatError("op padding runs past the payload");
        off += size;
    }
    if (off != payload.size()) throw DeltaFormatError("trailing bytes after the last op");
    return d;
}

} // namespace bpt
