#include <versioned-document-file/assets.hh>

namespace vdoc::file
{
blob_hash blob_hash::of(cc::span<byte const> bytes)
{
    return blob_hash(cc::hash256::create(bytes));
}

std::strong_ordering blob_hash::compare_bytes(blob_hash const& rhs) const
{
    // The canonical bytes, not the limbs: hash256's defaulted <=> orders limbs assembled little-endian, which is a
    // different order and would not agree with the hex digests a tool prints.
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
} // namespace vdoc::file
