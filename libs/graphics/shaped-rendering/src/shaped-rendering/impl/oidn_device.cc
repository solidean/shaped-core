#include <OpenImageDenoise/oidn.hpp>
#include <clean-core/record/log.hh>
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

bool oidn_filter_reference(cc::span<tg::vec3f const> color,
                           cc::span<tg::vec3f const> albedo,
                           cc::span<tg::vec3f const> normal,
                           tg::vec2i extent,
                           cc::span<tg::vec3f> out)
{
    auto const count = isize(extent[0]) * isize(extent[1]);
    if (color.size() != count || albedo.size() != count || normal.size() != count || out.size() != count)
    {
        CC_LOG_WARNING("oidn: the reference filter was given buffers that are not {}x{}", extent[0], extent[1]);
        return false;
    }

    auto device = oidn::newDevice(oidn::DeviceType::CPU);
    if (!device)
        return false;
    device.commit();

    auto filter = device.newFilter("RT");

    // Const-cast because OIDN takes non-const pointers even for what it only reads.
    auto const width = size_t(extent[0]);
    auto const height = size_t(extent[1]);
    auto const stride = sizeof(tg::vec3f);
    auto const row = stride * width;

    filter.setImage("color", const_cast<tg::vec3f*>(color.data()), oidn::Format::Float3, width, height, 0, stride, row);
    filter.setImage("albedo", const_cast<tg::vec3f*>(albedo.data()), oidn::Format::Float3, width, height, 0, stride, row);
    filter.setImage("normal", const_cast<tg::vec3f*>(normal.data()), oidn::Format::Float3, width, height, 0, stride, row);
    filter.setImage("output", out.data(), oidn::Format::Float3, width, height, 0, stride, row);

    // What the member does, spelled out rather than left to a default that could move between versions.
    //
    // `inputScale` above all: without it OIDN MEASURES the image and derives its own, and then the two are denoising
    // different pictures — which looks like a wrong network rather than a different exposure.
    filter.set("hdr", true);
    filter.set("inputScale", 1.0f);
    filter.set("quality", OIDN_QUALITY_BALANCED);
    filter.commit();

    filter.execute();

    char const* message = nullptr;
    if (device.getError(message) != oidn::Error::None)
    {
        CC_LOG_WARNING("oidn: the reference filter failed: {}", message != nullptr ? message : "(no message)");
        return false;
    }
    return true;
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
