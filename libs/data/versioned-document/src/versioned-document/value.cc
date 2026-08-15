#include "value.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/common/endian.hh>
#include <clean-core/common/hash.hh>
#include <clean-core/common/utility.hh>

using namespace cc::primitive_defines;

namespace
{
using vdoc::value_decode_error;
using vdoc::value_decode_error_kind;
using vdoc::value_kind;

// Byte offsets inside one encoded value, measured from its tag byte.
constexpr isize payload_offset = 1;
constexpr isize container_count_offset = 5;
constexpr isize container_entries_offset = 9;

// A length prefix always counts THE BYTES THAT FOLLOW IT.
// For string and bytes that is the data; for array and object it is the u32 count field plus the entries.
// One meaning across all four length-prefixed kinds is what makes skipping uniform — 5 + prefix, whatever the kind.
constexpr isize length_prefix_size = 4;

using vdoc::impl::compare_key_bytes;

/// The bytes at [offset, offset + size) as characters, for a key or a string payload.
[[nodiscard]] cc::string_view chars_at(cc::span<byte const> b, isize offset, isize size)
{
    return cc::string_view(reinterpret_cast<char const*>(b.data() + offset), size);
}

/// The total encoded size of the value at `offset`, on bytes that already passed try_decode.
[[nodiscard]] isize encoded_size_at(cc::span<byte const> b, isize offset)
{
    switch (value_kind(u8(b[offset])))
    {
    case value_kind::null:
        return 1;
    case value_kind::boolean:
        return 2;
    case value_kind::integer:
    case value_kind::number:
        return 1 + 8;
    case value_kind::string:
    case value_kind::bytes:
    case value_kind::array:
    case value_kind::object:
        return 1 + length_prefix_size + isize(cc::load_bytes_le<u32>(b, offset + payload_offset));
    }
    CC_UNREACHABLE("tag out of range on bytes that passed try_decode");
}

[[nodiscard]] cc::result<isize, value_decode_error> decode_failure(value_decode_error_kind kind, isize offset)
{
    return cc::error(value_decode_error{kind, offset});
}

/// Validates the one value starting at `offset`, refusing to look at or past `limit`.
/// Returns the offset just past it.
///
/// `depth` counts the value itself as level 1, so a scalar handed to try_decode is at depth 1.
[[nodiscard]] cc::result<isize, value_decode_error> validate(cc::span<byte const> b, isize offset, isize limit, isize depth)
{
    if (depth > vdoc::value_view::max_depth)
        return decode_failure(value_decode_error_kind::depth_exceeded, offset);

    if (offset >= limit)
        return decode_failure(value_decode_error_kind::truncated, offset);

    auto const tag = u8(b[offset]);
    if (tag > u8(value_kind::object))
        return decode_failure(value_decode_error_kind::unknown_tag, offset);

    auto const kind = value_kind(tag);
    switch (kind)
    {
    case value_kind::null:
        return offset + 1;

    case value_kind::boolean:
    {
        if (offset + 2 > limit)
            return decode_failure(value_decode_error_kind::truncated, offset + payload_offset);

        if (u8(b[offset + payload_offset]) > 1)
            return decode_failure(value_decode_error_kind::invalid_boolean, offset + payload_offset);

        return offset + 2;
    }

    case value_kind::integer:
    case value_kind::number:
    {
        if (offset + 1 + 8 > limit)
            return decode_failure(value_decode_error_kind::truncated, offset + payload_offset);

        return offset + 1 + 8;
    }

    case value_kind::string:
    case value_kind::bytes:
    {
        if (offset + 1 + length_prefix_size > limit)
            return decode_failure(value_decode_error_kind::truncated, offset + payload_offset);

        auto const size = isize(cc::load_bytes_le<u32>(b, offset + payload_offset));
        auto const end = offset + 1 + length_prefix_size + size;
        if (end > limit)
            return decode_failure(value_decode_error_kind::length_mismatch, offset + payload_offset);

        return end;
    }

    case value_kind::array:
    case value_kind::object:
    {
        if (offset + 1 + length_prefix_size > limit)
            return decode_failure(value_decode_error_kind::truncated, offset + payload_offset);

        auto const payload_size = isize(cc::load_bytes_le<u32>(b, offset + payload_offset));

        // the payload always begins with the u32 count, so anything shorter cannot describe a container at all
        if (payload_size < length_prefix_size)
            return decode_failure(value_decode_error_kind::length_mismatch, offset + payload_offset);

        auto const end = offset + 1 + length_prefix_size + payload_size;
        if (end > limit)
            return decode_failure(value_decode_error_kind::length_mismatch, offset + payload_offset);

        auto const count = isize(cc::load_bytes_le<u32>(b, offset + container_count_offset));
        auto cursor = offset + container_entries_offset;

        auto previous_key = cc::string_view();
        auto has_previous_key = false;

        for (isize i = 0; i < count; ++i)
        {
            if (cursor >= end)
                return decode_failure(value_decode_error_kind::count_mismatch, cursor);

            if (kind == value_kind::object)
            {
                if (cursor + length_prefix_size > end)
                    return decode_failure(value_decode_error_kind::length_mismatch, cursor);

                auto const key_size = isize(cc::load_bytes_le<u32>(b, cursor));
                if (cursor + length_prefix_size + key_size > end)
                    return decode_failure(value_decode_error_kind::length_mismatch, cursor);

                auto const key = chars_at(b, cursor + length_prefix_size, key_size);
                if (has_previous_key)
                {
                    auto const order = compare_key_bytes(previous_key, key);
                    if (order == 0)
                        return decode_failure(value_decode_error_kind::duplicate_key, cursor);
                    if (order > 0)
                        return decode_failure(value_decode_error_kind::unsorted_keys, cursor);
                }

                previous_key = key;
                has_previous_key = true;
                cursor += length_prefix_size + key_size;
            }

            auto const child = validate(b, cursor, end, depth + 1);
            if (child.has_error())
                return cc::error(child.error());

            cursor = child.value();
        }

        // the declared count and the declared length have to describe the same region, or one of them is a lie
        if (cursor != end)
            return decode_failure(value_decode_error_kind::count_mismatch, cursor);

        return end;
    }
    }

    CC_UNREACHABLE("tag out of range after the range check above");
}
} // namespace

// ---- value_view --------------------------------------------------------------------------------

bool vdoc::value_view::as_bool() const
{
    CC_ASSERT(kind() == value_kind::boolean, "value is not a boolean");
    return u8(_bytes[payload_offset]) != 0;
}

i64 vdoc::value_view::as_i64() const
{
    CC_ASSERT(kind() == value_kind::integer, "value is not an integer");
    return cc::load_bytes_le<i64>(_bytes, payload_offset);
}

f64 vdoc::value_view::as_f64() const
{
    CC_ASSERT(kind() == value_kind::number, "value is not a number");
    return cc::load_bytes_le<f64>(_bytes, payload_offset);
}

cc::string_view vdoc::value_view::as_string() const
{
    CC_ASSERT(kind() == value_kind::string, "value is not a string");
    auto const size = isize(cc::load_bytes_le<u32>(_bytes, payload_offset));
    return chars_at(_bytes, 1 + length_prefix_size, size);
}

cc::span<byte const> vdoc::value_view::as_bytes() const
{
    CC_ASSERT(kind() == value_kind::bytes, "value is not a byte string");
    auto const size = isize(cc::load_bytes_le<u32>(_bytes, payload_offset));
    return cc::span<byte const>(_bytes.data() + 1 + length_prefix_size, size);
}

isize vdoc::value_view::size() const
{
    auto const k = kind();
    if (k != value_kind::array && k != value_kind::object)
        return 0;

    return isize(cc::load_bytes_le<u32>(_bytes, container_count_offset));
}

vdoc::value_view vdoc::value_view::element_at(isize i) const
{
    auto const k = kind();
    CC_ASSERT(k == value_kind::array || k == value_kind::object, "value is not a container");
    CC_ASSERT(0 <= i && i < size(), "element index out of bounds");

    auto cursor = container_entries_offset;
    for (isize e = 0; e < i; ++e)
    {
        if (k == value_kind::object)
            cursor += length_prefix_size + isize(cc::load_bytes_le<u32>(_bytes, cursor));
        cursor += encoded_size_at(_bytes, cursor);
    }

    if (k == value_kind::object)
        cursor += length_prefix_size + isize(cc::load_bytes_le<u32>(_bytes, cursor));

    return from_validated_bytes(cc::span<byte const>(_bytes.data() + cursor, encoded_size_at(_bytes, cursor)));
}

cc::string_view vdoc::value_view::key_at(isize i) const
{
    CC_ASSERT(kind() == value_kind::object, "value is not an object");
    CC_ASSERT(0 <= i && i < size(), "entry index out of bounds");

    auto cursor = container_entries_offset;
    for (isize e = 0; e < i; ++e)
    {
        cursor += length_prefix_size + isize(cc::load_bytes_le<u32>(_bytes, cursor));
        cursor += encoded_size_at(_bytes, cursor);
    }

    return chars_at(_bytes, cursor + length_prefix_size, isize(cc::load_bytes_le<u32>(_bytes, cursor)));
}

cc::optional<vdoc::value_view> vdoc::value_view::try_find(cc::string_view key) const
{
    if (kind() != value_kind::object)
        return {};

    auto const count = size();
    auto cursor = container_entries_offset;

    for (isize i = 0; i < count; ++i)
    {
        auto const key_size = isize(cc::load_bytes_le<u32>(_bytes, cursor));
        auto const entry_key = chars_at(_bytes, cursor + length_prefix_size, key_size);
        cursor += length_prefix_size + key_size;

        auto const order = compare_key_bytes(entry_key, key);
        if (order == 0)
            return from_validated_bytes(cc::span<byte const>(_bytes.data() + cursor, encoded_size_at(_bytes, cursor)));

        // keys ascend, so once one sorts past the target nothing later can match
        if (order > 0)
            return {};

        cursor += encoded_size_at(_bytes, cursor);
    }

    return {};
}

bool vdoc::impl::bytes_equal(cc::span<byte const> a, cc::span<byte const> b)
{
    if (a.size() != b.size())
        return false;

    return cc::memcmp(a.data(), b.data(), a.size()) == 0;
}

u64 vdoc::impl::hash_bytes(cc::span<byte const> b)
{
    return cc::make_hash_of_bytes(b);
}

int vdoc::impl::compare_key_bytes(cc::string_view a, cc::string_view b)
{
    return a.compare(b);
}

// ---- value -------------------------------------------------------------------------------------

vdoc::value vdoc::value::make_uninitialized(isize size)
{
    auto v = value();
    v._storage.resize_to_uninitialized(size);
    return v;
}

vdoc::value vdoc::value::of_null()
{
    return value();
}

vdoc::value vdoc::value::of(bool v)
{
    auto r = make_uninitialized(2);
    auto const b = r.mutable_bytes();
    b[0] = byte(u8(value_kind::boolean));
    b[payload_offset] = byte(v ? 1 : 0);
    return r;
}

vdoc::value vdoc::value::of_i64(i64 v)
{
    auto r = make_uninitialized(1 + 8);
    auto const b = r.mutable_bytes();
    b[0] = byte(u8(value_kind::integer));
    cc::store_bytes_le<i64>(b, payload_offset, v);
    return r;
}

vdoc::value vdoc::value::of(f32 v)
{
    return of(f64(v));
}

vdoc::value vdoc::value::of(f64 v)
{
    auto r = make_uninitialized(1 + 8);
    auto const b = r.mutable_bytes();
    b[0] = byte(u8(value_kind::number));
    cc::store_bytes_le<f64>(b, payload_offset, v);
    return r;
}

vdoc::value vdoc::value::of(cc::string_view v)
{
    CC_ASSERT(v.size() <= isize(0xFFFFFFFF), "a string value's byte length must fit a u32");

    auto r = make_uninitialized(1 + length_prefix_size + v.size());
    auto const b = r.mutable_bytes();
    b[0] = byte(u8(value_kind::string));
    cc::store_bytes_le<u32>(b, payload_offset, u32(v.size()));
    if (v.size() > 0)
        cc::memcpy(b.data() + 1 + length_prefix_size, v.data(), size_t(v.size()));

    return r;
}

vdoc::value vdoc::value::of_bytes(cc::span<byte const> v)
{
    CC_ASSERT(v.size() <= isize(0xFFFFFFFF), "a bytes value's length must fit a u32");

    auto r = make_uninitialized(1 + length_prefix_size + v.size());
    auto const b = r.mutable_bytes();
    b[0] = byte(u8(value_kind::bytes));
    cc::store_bytes_le<u32>(b, payload_offset, u32(v.size()));
    if (v.size() > 0)
        cc::memcpy(b.data() + 1 + length_prefix_size, v.data(), size_t(v.size()));

    return r;
}

vdoc::value vdoc::value::from_validated_bytes(cc::span<byte const> bytes)
{
    CC_ASSERT(!bytes.empty(), "an encoded value is at least a tag byte");

    auto r = make_uninitialized(bytes.size());
    cc::memcpy(r.mutable_bytes().data(), bytes.data(), size_t(bytes.size()));
    return r;
}

// ---- decoding ----------------------------------------------------------------------------------

cc::result<vdoc::value_view, vdoc::value_decode_error> vdoc::try_decode(cc::span<byte const> bytes)
{
    auto const end = validate(bytes, 0, bytes.size(), 1);
    if (end.has_error())
        return cc::error(end.error());

    if (end.value() != bytes.size())
        return cc::error(value_decode_error{value_decode_error_kind::trailing_bytes, end.value()});

    return value_view::from_validated_bytes(bytes);
}

isize vdoc::encoded_size(cc::span<byte const> bytes)
{
    CC_ASSERT(!bytes.empty(), "encoded_size needs at least a tag byte");
    return encoded_size_at(bytes, 0);
}

cc::span<byte const> vdoc::skip_value(cc::span<byte const> bytes)
{
    auto const size = encoded_size(bytes);
    CC_ASSERT(size <= bytes.size(), "skip_value ran past the buffer, so these bytes never passed try_decode");

    return cc::span<byte const>(bytes.data() + size, bytes.size() - size);
}
