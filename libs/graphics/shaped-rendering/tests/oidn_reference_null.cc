#include "oidn_reference.hh"

#include <clean-core/string/string.hh>

// The oracle where OIDN was not fetched: every answer is "not here", so the tests that need it skip.

namespace sr_test
{
bool oidn_is_compiled_in()
{
    return false;
}

cc::string oidn_version()
{
    return {};
}

bool oidn_has_device()
{
    return false;
}

bool oidn_filter_reference(cc::span<tg::vec3f const>,
                           cc::span<tg::vec3f const>,
                           cc::span<tg::vec3f const>,
                           tg::vec2i,
                           cc::span<tg::vec3f>,
                           sr::oidn_network_size)
{
    return false;
}
} // namespace sr_test
