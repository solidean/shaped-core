#include "metal_common.hh"

namespace sg::backend::metal
{
NS::String* ns_string(cc::string_view text)
{
    // NSString's UTF-8 initializer wants a NUL-terminated pointer, and a string_view carries no terminator of its own.
    // A cc::string materializes one; the bytes are copied into the NSString before it goes out of scope.
    auto owned = cc::string(text);
    return NS::String::string(owned.c_str_materialize(), NS::UTF8StringEncoding);
}

cc::string to_string(NS::String const* string)
{
    if (string == nullptr)
        return {};

    auto const* const utf8 = const_cast<NS::String*>(string)->utf8String();
    return utf8 == nullptr ? cc::string() : cc::string(utf8);
}

cc::string describe_error(NS::Error const* error, cc::string_view what)
{
    if (error == nullptr)
        return cc::format("{} (no NSError reported)", what);

    auto* const mutable_error = const_cast<NS::Error*>(error);
    return cc::format("{} ({}, code {})", what, to_string(mutable_error->localizedDescription()),
                      i64(mutable_error->code()));
}
} // namespace sg::backend::metal
