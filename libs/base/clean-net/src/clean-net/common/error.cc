#include "error.hh"

#include <clean-core/string/format.hh>

namespace cnet
{
cc::string_view to_string(error_code code)
{
    switch (code)
    {
    case error_code::unknown:
        return "unknown";
    case error_code::unsupported:
        return "unsupported";
    case error_code::backend_missing:
        return "backend_missing";
    case error_code::invalid_argument:
        return "invalid_argument";
    case error_code::level_not_supported:
        return "level_not_supported";
    case error_code::timed_out:
        return "timed_out";
    case error_code::cancelled:
        return "cancelled";
    case error_code::name_not_resolved:
        return "name_not_resolved";
    case error_code::connection_refused:
        return "connection_refused";
    case error_code::host_unreachable:
        return "host_unreachable";
    case error_code::connection_reset:
        return "connection_reset";
    case error_code::connection_closed:
        return "connection_closed";
    case error_code::address_in_use:
        return "address_in_use";
    case error_code::permission_denied:
        return "permission_denied";
    case error_code::tls_handshake_failed:
        return "tls_handshake_failed";
    case error_code::certificate_rejected:
        return "certificate_rejected";
    case error_code::protocol_error:
        return "protocol_error";
    case error_code::body_too_large:
        return "body_too_large";
    case error_code::too_many_redirects:
        return "too_many_redirects";
    }
    return "unknown";
}

error unsupported_here(cc::string_view what)
{
    return {.code = error_code::unsupported,
            .native_code = 0,
            .message = cc::format("{} is not available on this platform", what)};
}

error backend_missing(cc::string_view what, cc::string_view how_to_get_it)
{
    return {.code = error_code::backend_missing,
            .native_code = 0,
            .message = cc::format("{} was not compiled into this build ({})", what, how_to_get_it)};
}
} // namespace cnet
