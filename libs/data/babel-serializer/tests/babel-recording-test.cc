#include <babel-serializer/image/image.hh>
#include <clean-core/record/domain.hh>
#include <clean-core/record/event_view.hh>
#include <clean-core/record/listener.hh>
#include <clean-core/record/recording.hh>
#include <clean-core/record/system.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

// What babel records about its own work.
//
// A scope nobody can find is a scope that will be deleted by the next person who touches the function, so the ones
// that earn their keep are pinned here — as is the sub-domain each is attributed to, which is what makes
// "show me only the image decoder" possible at all.

namespace
{
/// Brings the recorder up for one test and takes it down again, whatever the body does.
///
/// Declared FIRST in a test so it is destroyed LAST: a recording holds CHUNK REFERENCES, and letting shutdown() free
/// the pool while one is still alive is a use-after-free into the heap rather than a diagnostic.
struct rec_fixture
{
    rec_fixture()
    {
        auto cfg = cc::rec::config{};
        cfg.threaded = false;
        cfg.overflow = cc::rec::overflow_policy::grow_unbounded;
        cc::rec::initialize(cfg);
    }
    ~rec_fixture() { cc::rec::shutdown(); }

    rec_fixture(rec_fixture const&) = delete;
    rec_fixture& operator=(rec_fixture const&) = delete;
};

/// Counts scope_begin events with `name`, and reports which domain they came from.
struct scope_probe
{
    isize count = 0;
    cc::string domain;
};

scope_probe probe_scopes(cc::rec::recording const& r, cc::string_view name)
{
    scope_probe out;
    for (auto const& b : r.blocks())
    {
        auto const v = b.view();
        for (auto it = v.begin(); it != v.end(); ++it)
            if (auto const e = *it; e.kind() == cc::rec::event_kind::scope_begin && cc::string_view(e.name()) == name)
            {
                ++out.count;
                out.domain = e.domain()->name();
            }
    }
    return out;
}
} // namespace

TEST("babel/recording - an image decode is scoped and counts its bytes", nx::config::exclusive(), nx::config::owns_recorder)
{
    // A 1x1 PNG, so the test needs no fixture on disk.
    // Bytes rather than a generated image: encoding one first would put an image.encode span in the recording too.
    constexpr u8 png_1x1[] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00,
        0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53, 0xDE, 0x00, 0x00, 0x00,
        0x0C, 0x49, 0x44, 0x41, 0x54, 0x08, 0xD7, 0x63, 0xF8, 0xCF, 0xC0, 0x00, 0x00, 0x03, 0x01, 0x01, 0x00, 0x18,
        0xDD, 0x8D, 0xB0, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82,
    };

    rec_fixture const fixture;

    cc::rec::recording_listener rl;
    auto const handle = cc::rec::register_listener(rl);

    auto const decoded
        = babel::image::read(cc::span<byte const>(reinterpret_cast<byte const*>(png_1x1), sizeof(png_1x1)));

    cc::rec::flush_blocking();
    cc::rec::unregister_listener(handle);

    auto const r = rl.take();

    CHECK(decoded.has_value());

    // The aggregator's span, and the codec's underneath it — which is what makes a trace read as "load an image".
    auto const image_scope = probe_scopes(r, "image.read");
    auto const png_scope = probe_scopes(r, "png.read");

    CHECK(image_scope.count == 1);
    CHECK(image_scope.domain == "babel.image");
    CHECK(png_scope.count == 1);
    CHECK(png_scope.domain == "babel.png");
}
