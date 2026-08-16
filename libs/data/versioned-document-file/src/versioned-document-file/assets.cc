#include <clean-core/string/format.hh>
#include <versioned-document-file/assets.hh>
#include <versioned-document-file/impl/payload_codec.hh>

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

cc::result<asset_part const*, part_lookup_error> asset_record::main_part() const
{
    return try_find_part(main_part_name);
}

cc::result<asset_part const*, part_lookup_error> asset_record::try_find_part(cc::string_view name) const
{
    asset_part const* found = nullptr;
    for (auto const& part : parts)
    {
        if (part.name != name)
            continue;
        // A second match is reported rather than ignored: returning the first would hand back plausible bytes from a
        // part the caller never meant, which nothing downstream could detect.
        if (found != nullptr)
            return cc::error(part_lookup_error::ambiguous);
        found = &part;
    }

    if (found == nullptr)
        return cc::error(part_lookup_error::not_found);
    return found;
}

cc::optional<asset_part const*> asset_record::part_at(cc::string_view name, isize index) const
{
    if (index < 0)
        return {};

    auto seen = isize(0);
    for (auto const& part : parts)
        if (part.name == name && seen++ == index)
            return &part;
    return {};
}

part_range asset_record::parts_named(cc::string_view name) const
{
    auto matched = cc::vector<asset_part const*>();
    for (auto const& part : parts)
        if (part.name == name)
            matched.push_back(&part);
    return part_range(cc::move(matched));
}

cc::result<blob_upload> blob_upload::of(cc::span<byte const> decoded, cc::string_view format, cc::string_view encoding)
{
    auto const* codec = impl::find_payload_codec(encoding);
    if (codec == nullptr)
        return cc::error(cc::any_error(cc::format("this build has no codec for the blob encoding '{}'", encoding)));

    // The hash is over the DECODED bytes, so it is taken before the encode and names the content rather than how it
    // happens to be stored.
    auto stored = codec->encode(cc::vector<byte>::create_copy_of(decoded));
    CC_RETURN_IF_ERROR(stored);

    return blob_upload{.hash = blob_hash::of(decoded),
                       .format = cc::string(format),
                       .encoding = cc::string(encoding),
                       .decoded_size = decoded.size(),
                       .data = cc::move(stored.value())};
}
} // namespace vdoc::file
