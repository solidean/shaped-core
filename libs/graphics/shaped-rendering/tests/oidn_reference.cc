#include "oidn_reference.hh"

#include <OpenImageDenoise/oidn.hpp>
#include <clean-core/common/time.hh>
#include <clean-core/record/log.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>

// OIDN, confined to this one TU; the header names none of its types.

namespace sr_test
{
using namespace cc::primitive_defines;

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
                           cc::span<tg::vec3f> out,
                           sr::oidn_network_size size)
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
    filter.set("quality", size == sr::oidn_network_size::small ? OIDN_QUALITY_FAST : OIDN_QUALITY_BALANCED);
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

f64 oidn_time_cpu_filter(tg::vec2i extent, sr::oidn_network_size size, int runs)
{
    auto device = oidn::newDevice(oidn::DeviceType::CPU);
    if (!device)
        return -1.0;
    device.commit();

    auto const width = size_t(extent[0]);
    auto const height = size_t(extent[1]);
    auto const bytes = width * height * sizeof(tg::vec3f);

    // The device's own buffers, filled once, so no copy in or out is on the clock.
    auto color = device.newBuffer(bytes);
    auto albedo = device.newBuffer(bytes);
    auto normal = device.newBuffer(bytes);
    auto output = device.newBuffer(bytes);
    auto* const c = static_cast<tg::vec3f*>(color.getData());
    auto* const a = static_cast<tg::vec3f*>(albedo.getData());
    auto* const n = static_cast<tg::vec3f*>(normal.getData());
    for (auto i = size_t(0); i < width * height; ++i)
    {
        auto const speckle = 0.35f + f32(i * 7 % 11) / 11.0f;
        a[i] = tg::vec3f(0.8f, 0.6f, 0.3f);
        c[i] = a[i] * speckle;
        n[i] = tg::vec3f(0, 0, 1);
    }

    auto filter = device.newFilter("RT");
    filter.setImage("color", color, oidn::Format::Float3, width, height);
    filter.setImage("albedo", albedo, oidn::Format::Float3, width, height);
    filter.setImage("normal", normal, oidn::Format::Float3, width, height);
    filter.setImage("output", output, oidn::Format::Float3, width, height);
    filter.set("hdr", true);
    filter.set("inputScale", 1.0f);
    filter.set("quality", size == sr::oidn_network_size::small ? OIDN_QUALITY_FAST : OIDN_QUALITY_BALANCED);
    filter.commit();

    // Warm-up, which also pays for whatever the first execution allocates.
    filter.execute();

    auto best = -1.0;
    for (auto r = 0; r < runs; ++r)
    {
        auto const start = cc::current_time_steady_secs();
        filter.execute();
        device.sync();
        auto const took = cc::current_time_steady_secs() - start;
        if (best < 0.0 || took < best)
            best = took;
    }

    char const* message = nullptr;
    if (device.getError(message) != oidn::Error::None)
    {
        CC_LOG_WARNING("oidn: the timed filter failed: {}", message != nullptr ? message : "(no message)");
        return -1.0;
    }
    return best;
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
} // namespace sr_test
