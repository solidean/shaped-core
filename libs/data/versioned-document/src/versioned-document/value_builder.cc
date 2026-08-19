#include "value_builder.hh"

#include <clean-core/algorithm/sort.hh>
#include <clean-core/common/assert.hh>
#include <clean-core/common/endian.hh>
#include <clean-core/common/macros.hh>
#include <clean-core/common/utility.hh>

using namespace cc::primitive_defines;

namespace
{
// Mirrors the layout constants in value.cc; a length prefix always counts the bytes that follow it.
constexpr isize length_prefix_size = 4;
constexpr isize container_entries_offset = 9;

// The largest value a u32 length prefix can describe.
constexpr isize max_prefixed_size = isize(0xFFFFFFFF);
} // namespace

vdoc::value_builder& vdoc::value_builder::push_encoded(cc::span<byte const> encoded)
{
    CC_ASSERT(_kind == value_kind::array, "push is for an array builder; use set on an object builder");
    _entries.push_back({cc::string(), cc::vector<byte>::create_copy_of(encoded)});
    return *this;
}

vdoc::value_builder& vdoc::value_builder::set_encoded(cc::string_view key, cc::span<byte const> encoded)
{
    CC_ASSERT(_kind == value_kind::object, "set is for an object builder; use push on an array builder");
    _entries.push_back({cc::string(key), cc::vector<byte>::create_copy_of(encoded)});
    return *this;
}

cc::result<vdoc::value, vdoc::value_build_error> vdoc::value_builder::try_build() const
{
    auto const count = _entries.size();
    if (count > max_prefixed_size)
        return cc::error(value_build_error::too_large);

    // Objects are emitted in key order whatever order the caller supplied, which is what makes an object literal
    // order-independent — two callers writing the same entries produce byte-identical values.
    auto order = cc::vector<isize>::create_uninitialized(count);
    for (isize i = 0; i < count; ++i)
        order[i] = i;

    if (_kind == value_kind::object)
    {
        // sort_indices breaks ties on the index, so equal keys keep caller order and the duplicate check below
        // sees them adjacent either way.
        cc::sort_indices(order, _entries,
                         [](entry const& a, entry const& b) { return impl::compare_key_bytes(a.key, b.key) < 0; });

        for (isize i = 1; i < count; ++i)
            if (impl::compare_key_bytes(_entries[order[i - 1]].key, _entries[order[i]].key) == 0)
                return cc::error(value_build_error::duplicate_key);
    }

    auto payload_size = length_prefix_size; // the u32 count field, which the prefix also covers
    for (auto const& e : _entries)
    {
        if (_kind == value_kind::object)
        {
            if (e.key.size() > max_prefixed_size)
                return cc::error(value_build_error::too_large);

            payload_size += length_prefix_size + e.key.size();
        }

        payload_size += e.encoded.size();
        if (payload_size > max_prefixed_size)
            return cc::error(value_build_error::too_large);
    }

    auto encoded = cc::vector<byte>::create_uninitialized(1 + length_prefix_size + payload_size);
    auto const out = cc::span<byte>(encoded.data(), encoded.size());

    out[0] = byte(u8(_kind));
    cc::store_bytes_le<u32>(out, 1, u32(payload_size));
    cc::store_bytes_le<u32>(out, 1 + length_prefix_size, u32(count));

    auto cursor = container_entries_offset;
    for (isize i = 0; i < count; ++i)
    {
        auto const& e = _entries[order[i]];

        if (_kind == value_kind::object)
        {
            cc::store_bytes_le<u32>(out, cursor, u32(e.key.size()));
            cursor += length_prefix_size;
            if (e.key.size() > 0)
                cc::memcpy(out.data() + cursor, e.key.data(), e.key.size());
            cursor += e.key.size();
        }

        cc::memcpy(out.data() + cursor, e.encoded.data(), e.encoded.size());
        cursor += e.encoded.size();
    }

    auto result = value::from_validated_bytes(out);

#if CC_ASSERT_ENABLED
    // Depth is the decoder's rule, and the only way a builder breaks it is by composing past value_view::max_depth.
    // Checking here rather than tracking a depth per value keeps the release path free of a walk it would almost never need.
    CC_ASSERT(try_decode(result.bytes()).has_value(), "value_builder produced bytes its own decoder rejects; nesting "
                                                      "past value_view::max_depth is the usual cause");
#endif

    return result;
}

vdoc::value vdoc::value_builder::build() const
{
    return try_build().value_assert("value_builder: a duplicate object key, or a payload too large for its u32 prefix");
}
