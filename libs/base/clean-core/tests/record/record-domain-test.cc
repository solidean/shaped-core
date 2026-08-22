#include "record-test-types.hh"

#include <clean-core/common/log.hh>
#include <clean-core/record/domain.hh>
#include <clean-core/record/event_view.hh>
#include <clean-core/record/recording.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;
using namespace cc_rec_test;

// Which domain a site lands in is decided by ORDINARY UNQUALIFIED NAME LOOKUP at the macro, so it is a property of
// the namespace a file's code sits in rather than of anything the site says.
// That makes it exactly the kind of thing a refactor breaks silently: move a function into another namespace, or add
// a `cc_rec_domain()` to an enclosing one, and every site inside it is re-attributed with no diagnostic anywhere.

namespace
{
/// A namespace with no domain of its own, so lookup has to walk out to the global fallback.
namespace unattributed
{
cc::rec::domain const* domain_here()
{
    return cc_rec_domain();
}
} // namespace unattributed
} // namespace

namespace cc
{
namespace rec_domain_test
{
/// Nested inside `cc`, which is what a clean-core header does — so this must find clean-core's domain.
cc::rec::domain const* domain_here()
{
    return cc_rec_domain();
}
} // namespace rec_domain_test
} // namespace cc

namespace cc::rec
{
namespace domain_test
{
/// Nested inside `cc::rec`, which shadows `cc` — the recorder's own bookkeeping is a separate question.
cc::rec::domain const* domain_here()
{
    return cc_rec_domain();
}
} // namespace domain_test
} // namespace cc::rec

TEST("record/domain - every library's domain registers under its own name")
{
    // clean-core and the recorder are separate domains, and both are findable by name.
    // A library that failed to define the domain its fwd.hh declared would not link at all, so presence here is
    // really about the NAME being the one a configuration file or a console line would use.
    auto const* const cc_domain = cc::rec::find_domain("cc");
    auto const* const rec_domain = cc::rec::find_domain("cc.record");

    CHECK(cc_domain != nullptr);
    CHECK(rec_domain != nullptr);
    CHECK(cc_domain != rec_domain);

    CHECK(cc::rec::find_domain("default") != nullptr);
    CHECK(cc::rec::find_domain("nexus") != nullptr); // this binary links nexus, so its domain is registered too

    CHECK(cc::rec::find_domain("no-library-is-called-this") == nullptr);
}

TEST("record/domain - a site is attributed by the namespace it sits in")
{
    CHECK(cc::rec_domain_test::domain_here() == cc::rec::find_domain("cc"));
    CHECK(cc::rec::domain_test::domain_here() == cc::rec::find_domain("cc.record"));

    // Outside any namespace declaring one, the global fallback still answers.
    CHECK(unattributed::domain_here() == cc::rec::find_domain("default"));
}

REC_TEST("record/domain - a message records the domain of the code that logged it")
{
    rec_fixture const fixture(deterministic_config());

    cc::rec::recording_listener rl;
    {
        scoped_listener const reg(rl);
        CC_LOG_WARNING("logged from a test, which is not inside cc");
        cc::rec::flush_blocking();
    }

    auto const r = rl.take();

    auto found = 0;
    for (auto const& b : r.blocks())
    {
        auto const v = b.view();
        for (auto it = v.begin(); it != v.end(); ++it)
            if (auto const e = *it; e.kind() == cc::rec::event_kind::log)
            {
                ++found;
                CHECK(cc::string_view(e.domain()->name()) == "default");
            }
    }
    CHECK(found == 1);
}
