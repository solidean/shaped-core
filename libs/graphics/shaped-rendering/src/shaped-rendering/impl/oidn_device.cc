#include <OpenImageDenoise/oidn.hpp>
#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>
#include <shaped-rendering/impl/oidn_device.hh>

// OIDN, confined to this one TU.
//
// The seam above names no OIDN type, so nothing of the library reaches a public header of ours or a consumer's
// translation unit — the same confinement `impl/nrd_instance.cc` gives NRD.

namespace sr::impl
{
bool oidn_is_compiled_in()
{
    return true;
}

cc::string oidn_version()
{
    // Reported by the header rather than queried, which is what makes it the version this TU was COMPILED against.
    // A facade that loaded an older core would still answer this, and the device creation below is what catches that.
    return cc::format("{}.{}.{}", OIDN_VERSION_MAJOR, OIDN_VERSION_MINOR, OIDN_VERSION_PATCH);
}

bool oidn_has_device()
{
    // Created and dropped, because there is no cheaper question to ask.
    // The facade dlopens its core and its CPU device module out of its own directory, so this is the first point at
    // which a build that linked OIDN but staged it incompletely says so.
    auto device = oidn::newDevice(oidn::DeviceType::CPU);
    if (!device)
        return false;

    device.commit();

    char const* message = nullptr;
    return device.getError(message) == oidn::Error::None;
}
} // namespace sr::impl
