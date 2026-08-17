#include <clean-core/string/format.hh>
#include <versioned-document-file/impl/payload_codec.hh>

namespace vdoc::file::impl
{
namespace
{
cc::result<cc::vector<byte>> raw_encode(cc::vector<byte> decoded)
{
    return decoded;
}

cc::result<cc::vector<byte>> raw_decode(cc::vector<byte> stored, i64 decoded_size)
{
    // The identity codec still checks the promise, because a `raw` row whose stored size disagrees with its decoded
    // size is a corrupt row rather than a decode this build cannot do.
    if (stored.size() != decoded_size)
        return cc::error(cc::any_error(
            cc::format("a raw blob stores {} bytes but its row promises {}", stored.size(), decoded_size)));
    return stored;
}

constexpr payload_codec codecs[] = {
    {.name = "raw", .encode = raw_encode, .decode = raw_decode, .is_byte_addressable = true},
};
} // namespace

cc::span<payload_codec const> payload_codecs()
{
    return codecs;
}

payload_codec const* find_payload_codec(cc::string_view encoding)
{
    for (auto const& codec : codecs)
        if (codec.name == encoding)
            return &codec;
    return nullptr;
}
} // namespace vdoc::file::impl
