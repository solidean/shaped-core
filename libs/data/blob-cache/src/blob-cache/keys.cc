#include <blob-cache/keys.hh>
#include <clean-core/common/utility.hh>

using namespace cc::primitive_defines;

bcache::logical_key bcache::logical_key::create_from_string(cc::string_view s)
{
    return create_from_bytes(s.as_bytes());
}

bcache::logical_key bcache::logical_key::create_from_bytes(cc::span<byte const> b)
{
    return {cc::vector<byte>::create_copy_of(b)};
}

bcache::logical_key bcache::logical_key::create_from_hash(cc::hash256 const& h)
{
    auto bytes = cc::vector<byte>::create_uninitialized(32);
    h.to_bytes(bytes);
    return {cc::move(bytes)};
}
