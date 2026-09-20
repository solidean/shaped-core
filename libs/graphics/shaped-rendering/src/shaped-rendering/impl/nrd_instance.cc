#include <NRD.h>
#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>
#include <shaped-rendering/impl/nrd_instance.hh>

// NRD, confined to this one TU.
//
// The seam above names no NRD type, so nothing of the library reaches a public header of ours or a consumer's
// translation unit — the same confinement `impl/dlss_ngx.cc` gives NGX.

namespace sr::impl
{
bool nrd_is_compiled_in()
{
    return true;
}

cc::string nrd_version()
{
    auto const* const desc = nrd::GetLibraryDesc();
    if (desc == nullptr)
        return {};
    return cc::format("{}.{}.{}", desc->versionMajor, desc->versionMinor, desc->versionBuild);
}
} // namespace sr::impl
