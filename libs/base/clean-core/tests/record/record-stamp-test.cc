#include "record-test-types.hh"

#include <clean-core/record/event_view.hh>
#include <clean-core/record/listener.hh>
#include <clean-core/record/recording.hh>
#include <clean-core/record/stamp.hh>
#include <clean-core/record/system.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

namespace
{
/// A contributor whose bytes are static, which is the shape a real one has: computed once, handed out by reference.
cc::span<byte const> test_provider(cc::rec::stamp_moment moment)
{
    static constexpr char k_open[] = "test.moment=open\n";
    static constexpr char k_close[] = "test.moment=close\n";

    auto const text = moment == cc::rec::stamp_moment::open ? cc::string_view(k_open) : cc::string_view(k_close);
    return cc::span<byte const>(reinterpret_cast<byte const*>(text.data()), text.size());
}

cc::span<byte const> silent_provider(cc::rec::stamp_moment)
{
    // A contributor that cannot answer leaves its section out rather than failing the recording.
    return {};
}

/// The stamp sections in a recording, by name.
cc::vector<cc::string> stamp_names(cc::rec::recording const& r)
{
    auto out = cc::vector<cc::string>();
    r.for_each_event(
        [&out](cc::rec::chunk_view const&, cc::rec::event_view const& e)
        {
            if (e.kind() == cc::rec::event_kind::stamp)
                out.push_back(cc::string(e.name()));
        });
    return out;
}

bool contains(cc::vector<cc::string> const& names, cc::string_view wanted)
{
    for (auto const& n : names)
        if (cc::string_view(n) == wanted)
            return true;
    return false;
}
} // namespace

TEST("rec/stamp - a recording is stamped at open and at close", nx::config::exclusive(), nx::config::owns_recorder)
{
    cc_rec_test::rec_fixture const fixture(cc_rec_test::deterministic_config());

    auto listener = cc::rec::recording_listener();
    {
        cc_rec_test::scoped_listener const registration(listener);
        cc::rec::emit_stamp(cc::rec::stamp_moment::open);
        cc::rec::emit_stamp(cc::rec::stamp_moment::close);
        cc::rec::flush_blocking();
    }

    auto const names = stamp_names(listener.result());

    // The machine description once, since it cannot have changed; the levels at both ends, because the difference is
    // the point of taking a pair.
    auto system_count = 0;
    auto resource_count = 0;
    for (auto const& n : names)
    {
        if (cc::string_view(n) == "cc.system")
            ++system_count;
        if (cc::string_view(n) == "cc.resources")
            ++resource_count;
    }

    CHECK(system_count == 1);
    CHECK(resource_count == 2);
}

TEST("rec/stamp - the machine section carries readable key=value lines", nx::config::exclusive(), nx::config::owns_recorder)
{
    cc_rec_test::rec_fixture const fixture(cc_rec_test::deterministic_config());

    auto listener = cc::rec::recording_listener();
    {
        cc_rec_test::scoped_listener const registration(listener);
        cc::rec::emit_stamp(cc::rec::stamp_moment::open);
        cc::rec::emit_stamp(cc::rec::stamp_moment::close);
        cc::rec::flush_blocking();
    }

    auto payload = cc::string();
    listener.result().for_each_event(
        [&payload](cc::rec::chunk_view const&, cc::rec::event_view const& e)
        {
            if (e.kind() == cc::rec::event_kind::stamp && cc::string_view(e.name()) == "cc.system")
                payload = cc::string(e.payload_as_text());
        });

    // No codec and no schema: a reader gets the machine out of a hex dump if it has to.
    CHECK(!payload.empty());
    CHECK(cc::string_view(payload).contains("cpu.logical_cores="));
    CHECK(cc::string_view(payload).contains("os.name="));
}

TEST("rec/stamp - a contributor is called at both moments, and an empty one is skipped",
     nx::config::exclusive(),
     nx::config::owns_recorder)
{
    CHECK(cc::rec::register_stamp_contributor("test.section", test_provider));
    CHECK(cc::rec::register_stamp_contributor("test.silent", silent_provider));

    // Registering the same name again replaces rather than duplicating.
    CHECK(cc::rec::register_stamp_contributor("test.section", test_provider));

    cc_rec_test::rec_fixture const fixture(cc_rec_test::deterministic_config());

    auto listener = cc::rec::recording_listener();
    {
        cc_rec_test::scoped_listener const registration(listener);
        cc::rec::emit_stamp(cc::rec::stamp_moment::open);
        cc::rec::emit_stamp(cc::rec::stamp_moment::close);
        cc::rec::flush_blocking();
    }

    auto sections = cc::vector<cc::string>();
    listener.result().for_each_event(
        [&sections](cc::rec::chunk_view const&, cc::rec::event_view const& e)
        {
            if (e.kind() == cc::rec::event_kind::stamp && cc::string_view(e.name()) == "test.section")
                sections.push_back(cc::string(e.payload_as_text()));
        });

    CHECK(sections.size() == 2);
    if (sections.size() == 2)
    {
        CHECK(cc::string_view(sections[0]).contains("open"));
        CHECK(cc::string_view(sections[1]).contains("close"));
    }

    // The contributor that returned nothing left no section behind.
    CHECK(!contains(stamp_names(listener.result()), "test.silent"));
}
