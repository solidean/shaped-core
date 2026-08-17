#include "op.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/common/blake3.hh>
#include <clean-core/common/endian.hh>
#include <clean-core/string/string_view.hh>

using namespace cc::primitive_defines;

namespace
{
using vdoc::assignment;
using vdoc::assignment_encoding;
using vdoc::op_decode_error;
using vdoc::op_id;

// The hash preimage is a FORMAT CONSTANT, pinned here and specified in ../../docs/concepts/ops-and-content-addressing.md.
// Every integer in it is fixed-width little-endian, and the domain separator makes an op id unable to collide with
// any other digest this library computes.
constexpr cc::string_view op_domain = "vdoc::op/v1";

constexpr isize id_byte_size = op_id::byte_size;
constexpr isize length_prefix_size = 4;
constexpr isize encoding_tag_size = 1;

// Byte offsets inside one encoded value, mirroring value.cc — enough to bound-check a value's extent without decoding it.
// A length prefix counts the bytes that follow it, in all four length-prefixed kinds.
constexpr isize value_payload_offset = 1;

[[nodiscard]] cc::result<isize, op_decode_error> decode_failure(op_decode_error kind)
{
    return cc::error(kind);
}

/// Feeds `u64 length | bytes` into the hash, which is how every variable-length field enters the preimage.
void hash_length_prefixed(cc::blake3& hasher, cc::span<byte const> bytes)
{
    byte length_field[8] = {};
    cc::store_bytes_le<u64>(length_field, 0, u64(bytes.size()));
    hasher.update(length_field);
    hasher.update(bytes);
}

/// The total encoded size of the value starting at `offset`, bound-checked against untrusted bytes.
///
/// This only establishes the extent; try_decode is what proves the bytes are a canonical value.
/// Splitting the two is what lets an assignment blob find where one value ends without trusting it first.
[[nodiscard]] cc::result<isize, op_decode_error> try_value_extent(cc::span<byte const> b, isize offset)
{
    if (offset >= b.size())
        return decode_failure(op_decode_error::truncated);

    switch (vdoc::value_kind(u8(b[offset])))
    {
    case vdoc::value_kind::null:
        return isize(1);
    case vdoc::value_kind::boolean:
        return isize(2);
    case vdoc::value_kind::integer:
    case vdoc::value_kind::number:
        return isize(1 + 8);
    case vdoc::value_kind::string:
    case vdoc::value_kind::bytes:
    case vdoc::value_kind::array:
    case vdoc::value_kind::object:
    {
        if (offset + value_payload_offset + length_prefix_size > b.size())
            return decode_failure(op_decode_error::truncated);

        auto const prefix = isize(cc::load_bytes_le<u32>(b, offset + value_payload_offset));
        return 1 + length_prefix_size + prefix;
    }
    }

    // an unknown tag is not a value at all, so there is no extent to report
    return decode_failure(op_decode_error::invalid_value);
}

/// Reads a u32 length prefix and the bytes it introduces, advancing `cursor`.
[[nodiscard]] cc::result<cc::string_view, op_decode_error> try_read_id_bytes(cc::span<byte const> b, isize& cursor)
{
    if (cursor + length_prefix_size > b.size())
        return cc::error(op_decode_error::truncated);

    auto const size = isize(cc::load_bytes_le<u32>(b, cursor));
    cursor += length_prefix_size;

    if (size < 0 || cursor + size > b.size())
        return cc::error(op_decode_error::truncated);

    auto const chars = cc::string_view(reinterpret_cast<char const*>(b.data() + cursor), size);
    cursor += size;
    return chars;
}
} // namespace

std::strong_ordering vdoc::op_id::compare_bytes(op_id const& rhs) const
{
    // The canonical bytes, not the limbs: hash256's defaulted <=> orders limbs assembled little-endian, which is a
    // different order and would put a different parent sequence into the hash preimage.
    byte lhs_bytes[byte_size] = {};
    byte rhs_bytes[byte_size] = {};
    to_bytes(lhs_bytes);
    rhs.to_bytes(rhs_bytes);

    for (isize i = 0; i < byte_size; ++i)
    {
        auto const a = u8(lhs_bytes[i]);
        auto const b = u8(rhs_bytes[i]);
        if (a != b)
            return a < b ? std::strong_ordering::less : std::strong_ordering::greater;
    }
    return std::strong_ordering::equal;
}

vdoc::assignment_cursor vdoc::assignment_cursor::from_validated_bytes(cc::span<byte const> assignment_bytes)
{
    auto cursor = assignment_cursor();
    if (assignment_bytes.empty())
        return cursor;

    cursor._bytes = assignment_bytes;
    cursor._cursor = encoding_tag_size + length_prefix_size;
    cursor._remaining = isize(cc::load_bytes_le<u32>(assignment_bytes, encoding_tag_size));
    return cursor;
}

vdoc::assignment vdoc::assignment_cursor::get() const
{
    CC_ASSERT(!at_end(), "no assignment at the cursor");

    auto cursor = _cursor;
    auto const read_id = [&]
    {
        auto const size = isize(cc::load_bytes_le<u32>(_bytes, cursor));
        cursor += length_prefix_size;
        auto const chars = cc::string_view(reinterpret_cast<char const*>(_bytes.data() + cursor), size);
        cursor += size;
        return chars;
    };

    auto const entity = read_id();
    auto const component = read_id();
    auto const property = read_id();

    auto const value_bytes = cc::span<byte const>(_bytes.data() + cursor, _bytes.size() - cursor);
    return assignment{.path = {.entity = entity_id::of(entity),
                               .component = component_type_id::of(component),
                               .property = property_id::of(property)},
                      .value = value_view::from_validated_bytes(
                          cc::span<byte const>(value_bytes.data(), vdoc::encoded_size(value_bytes)))};
}

void vdoc::assignment_cursor::advance()
{
    CC_ASSERT(!at_end(), "no assignment at the cursor");

    for (isize i = 0; i < 3; ++i)
    {
        auto const size = isize(cc::load_bytes_le<u32>(_bytes, _cursor));
        _cursor += length_prefix_size + size;
    }

    auto const value_bytes = cc::span<byte const>(_bytes.data() + _cursor, _bytes.size() - _cursor);
    _cursor += vdoc::encoded_size(value_bytes);
    --_remaining;
}

vdoc::value_view vdoc::op::metadata() const
{
    if (!payload.has_value())
        return {};

    return value_view::from_validated_bytes(payload.value().metadata_bytes);
}

vdoc::assignment_cursor vdoc::op::assignments() const
{
    if (!payload.has_value())
        return {};

    return assignment_cursor::from_validated_bytes(payload.value().assignment_bytes);
}

cc::result<cc::vector<vdoc::assignment>, vdoc::op_decode_error> vdoc::op::try_decode_assignments() const
{
    auto out = cc::vector<assignment>();
    if (!payload.has_value())
        return out;

    auto const& bytes = payload.value().assignment_bytes;
    if (assignment_encoding(u8(bytes[0])) != assignment_encoding::sorted_v1)
        return cc::error(op_decode_error::unknown_assignment_encoding);

    auto cursor = assignments();
    out.reserve(cursor.remaining());
    for (auto const a : cursor)
        out.push_back(a);

    return out;
}

vdoc::op_id vdoc::compute_op_id(cc::span<op_id const> parents,
                                cc::span<byte const> metadata_bytes,
                                cc::span<byte const> assignment_bytes)
{
    auto hasher = cc::blake3();

    // u64 length | "vdoc::op/v1"
    hash_length_prefixed(hasher, cc::span<byte const>(reinterpret_cast<byte const*>(op_domain.data()), op_domain.size()));

    // u32 parent count | each parent's 32 bytes, verbatim in the op's own order
    byte count_field[4] = {};
    cc::store_bytes_le<u32>(count_field, 0, u32(parents.size()));
    hasher.update(count_field);
    for (auto const& parent : parents)
    {
        byte id_bytes[id_byte_size] = {};
        parent.to_bytes(id_bytes);
        hasher.update(id_bytes);
    }

    // u64 length | metadata bytes, then u64 length | assignment bytes
    hash_length_prefixed(hasher, metadata_bytes);
    hash_length_prefixed(hasher, assignment_bytes);

    return op_id(hasher.finalize());
}

vdoc::op_verification vdoc::verify_op(op const& o)
{
    // A skeleton has no bytes to hash, so it is unverifiable by construction.
    // Reporting it as a mismatch would be a false alarm about the one thing content addressing exists to detect.
    if (o.is_skeleton())
        return op_verification::unverifiable;

    auto const& p = o.payload.value();
    auto const recomputed = compute_op_id(o.parents, p.metadata_bytes, p.assignment_bytes);
    return recomputed == o.id ? op_verification::verified : op_verification::mismatch;
}

cc::result<vdoc::op, vdoc::op_decode_error> vdoc::try_decode_skeleton_op(op_id const& id, cc::span<op_id const> parents)
{
    for (isize i = 1; i < parents.size(); ++i)
    {
        auto const order = parents[i - 1].compare_bytes(parents[i]);
        if (order == 0)
            return cc::error(op_decode_error::duplicate_parent);
        if (order > 0)
            return cc::error(op_decode_error::unsorted_parents);
    }

    return op{.id = id, .parents = cc::vector<op_id>::create_copy_of(parents), .payload = {}};
}

cc::result<vdoc::op, vdoc::op_decode_error> vdoc::try_decode_op(op_id const& expected_id,
                                                                cc::span<op_id const> parents,
                                                                cc::span<byte const> metadata_bytes,
                                                                cc::span<byte const> assignment_bytes)
{
    for (isize i = 1; i < parents.size(); ++i)
    {
        auto const order = parents[i - 1].compare_bytes(parents[i]);
        if (order == 0)
            return cc::error(op_decode_error::duplicate_parent);
        if (order > 0)
            return cc::error(op_decode_error::unsorted_parents);
    }

    // The metadata is any canonical value.
    // Nothing interprets it, so constraining its kind would reject a future producer for no benefit — and a decoder
    // that rejects more than it must is a forward-compatibility break.
    if (vdoc::try_decode(metadata_bytes).has_error())
        return cc::error(op_decode_error::invalid_value);

    if (assignment_bytes.size() < encoding_tag_size + length_prefix_size)
        return cc::error(op_decode_error::truncated);

    if (assignment_encoding(u8(assignment_bytes[0])) != assignment_encoding::sorted_v1)
        return cc::error(op_decode_error::unknown_assignment_encoding);

    auto const count = isize(cc::load_bytes_le<u32>(assignment_bytes, encoding_tag_size));
    auto cursor = encoding_tag_size + length_prefix_size;

    auto previous = property_path();
    for (isize i = 0; i < count; ++i)
    {
        auto const entity = try_read_id_bytes(assignment_bytes, cursor);
        if (entity.has_error())
            return cc::error(entity.error());
        auto const component = try_read_id_bytes(assignment_bytes, cursor);
        if (component.has_error())
            return cc::error(component.error());
        auto const property = try_read_id_bytes(assignment_bytes, cursor);
        if (property.has_error())
            return cc::error(property.error());

        auto const extent = try_value_extent(assignment_bytes, cursor);
        if (extent.has_error())
            return cc::error(extent.error());
        if (cursor + extent.value() > assignment_bytes.size())
            return cc::error(op_decode_error::truncated);

        // the extent only says where the value ends; this is what proves it is canonical
        auto const value_bytes = cc::span<byte const>(assignment_bytes.data() + cursor, extent.value());
        if (vdoc::try_decode(value_bytes).has_error())
            return cc::error(op_decode_error::invalid_value);
        cursor += extent.value();

        auto const path = property_path{.entity = entity_id::of(entity.value()),
                                        .component = component_type_id::of(component.value()),
                                        .property = property_id::of(property.value())};

        if (i > 0)
        {
            auto const order = previous.compare_bytes(path);
            if (order == 0)
                return cc::error(op_decode_error::duplicate_assignment);
            if (order > 0)
                return cc::error(op_decode_error::unsorted_assignments);
        }
        previous = path;
    }

    if (cursor != assignment_bytes.size())
        return cc::error(op_decode_error::trailing_bytes);

    // Re-HASH what was read.
    // Nothing above re-serialized anything, which is why a formatter change can never make a good stored op look
    // like tampering.
    auto const recomputed = compute_op_id(parents, metadata_bytes, assignment_bytes);
    if (recomputed != expected_id)
        return cc::error(op_decode_error::hash_mismatch);

    return op{.id = expected_id,
              .parents = cc::vector<op_id>::create_copy_of(parents),
              .payload = op_payload{.metadata_bytes = cc::vector<byte>::create_copy_of(metadata_bytes),
                                    .assignment_bytes = cc::vector<byte>::create_copy_of(assignment_bytes)}};
}

cc::vector<byte> vdoc::encode_assignments(cc::span<assignment const> sorted_assignments)
{
    auto out = cc::vector<byte>();
    out.resize_to_uninitialized(encoding_tag_size + length_prefix_size);
    out[0] = byte(u8(assignment_encoding::sorted_v1));
    cc::store_bytes_le<u32>(out, encoding_tag_size, u32(sorted_assignments.size()));

    auto const append_id = [&](cc::string_view chars)
    {
        auto const at = out.size();
        out.resize_to_uninitialized(at + length_prefix_size + chars.size());
        cc::store_bytes_le<u32>(out, at, u32(chars.size()));
        for (isize i = 0; i < chars.size(); ++i)
            out[at + length_prefix_size + i] = byte(chars[i]);
    };

    for (isize i = 0; i < sorted_assignments.size(); ++i)
    {
        auto const& a = sorted_assignments[i];
        CC_ASSERT(i == 0 || sorted_assignments[i - 1].path.compare_bytes(a.path) < 0,
                  "assignments must be sorted by path and free of duplicates before encoding");

        append_id(a.path.entity.as_string_view());
        append_id(a.path.component.as_string_view());
        append_id(a.path.property.as_string_view());

        auto const value_bytes = a.value.bytes();
        auto const at = out.size();
        out.resize_to_uninitialized(at + value_bytes.size());
        for (isize j = 0; j < value_bytes.size(); ++j)
            out[at + j] = value_bytes[j];
    }

    return out;
}
