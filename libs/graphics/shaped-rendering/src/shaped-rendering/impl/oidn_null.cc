#include <clean-core/string/string.hh>
#include <shaped-rendering/impl/oidn_device.hh>

// The OIDN seam where OIDN was never fetched.
//
// Present rather than absent so the member and every symbol around it exist in every build: a caller naming
// `sr::denoise_method::oidn` compiles everywhere and is told `unsupported` here.

namespace sr::impl
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
} // namespace sr::impl
