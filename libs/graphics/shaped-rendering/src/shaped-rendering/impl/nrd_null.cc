#include <clean-core/string/string.hh>
#include <shaped-rendering/impl/nrd_instance.hh>

// The NRD seam where NRD was never fetched.
//
// Present rather than absent so the member and every symbol around it exist in every build: a caller naming
// `sr::denoise_method::nrd` compiles everywhere and is told `unsupported` here.

namespace sr::impl
{
bool nrd_is_compiled_in()
{
    return false;
}

cc::string nrd_version()
{
    return {};
}
} // namespace sr::impl
