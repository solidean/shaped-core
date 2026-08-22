#include "record-test-types.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/platform/file_path.hh>
#include <clean-core/record/event_view.hh>
#include <clean-core/record/recording.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;
using namespace cc_rec_test;

// clean-core recording its own work.
//
// The point of a stat is that somebody downstream can ask; these are that somebody, so a stat that stops being
// recorded fails here rather than going quietly missing from a graph nobody was watching closely.

namespace
{
/// Sums one accumulate stat across a recording.
f64 accumulated(cc::rec::recording const& r, cc::string_view name)
{
    auto total = 0.0;
    for (auto const& b : r.blocks())
    {
        auto const v = b.view();
        for (auto it = v.begin(); it != v.end(); ++it)
            if (auto const e = *it; e.kind() == cc::rec::event_kind::stat_accumulate && cc::string_view(e.name()) == name)
                total += e.field_as_double("value").value_or(0.0);
    }
    return total;
}
} // namespace

REC_TEST("record/adoption - a file stream reports the bytes it moved")
{
    // temp_file_path rather than a fixed name: it keys on the process and a counter, so a second copy of this binary
    // running beside the first does not overwrite its file out from under it.
    auto const path = cc::temp_file_path("cc-rec-adoption", ".bin");
    auto const payload = cc::string("the bytes a stat has to account for");

    rec_fixture const fixture(deterministic_config());

    cc::rec::recording_listener rl;
    {
        scoped_listener const reg(rl);

        {
            auto out = cc::file_write_stream_adapter::create(path);
            REQUIRE(out.has_value());
            auto stream = out.value().stream();
            REQUIRE(stream.write(cc::span<byte const>(reinterpret_cast<byte const*>(payload.data()), payload.size()))
                        .has_value());
            REQUIRE(stream.flush().has_value());
        }

        {
            auto in = cc::file_read_stream_adapter::open(path);
            REQUIRE(in.has_value());
            auto stream = in.value().stream();
            auto buffer = cc::vector<byte>::create_defaulted(payload.size());
            REQUIRE(stream.read_exact(buffer).has_value());
        }

        cc::rec::flush_blocking();
    }

    auto const r = rl.take();

    // At least the payload: the write may be issued in more than one go, and the read fills a whole buffer window.
    CHECK(accumulated(r, "cc.file.bytes_written") >= f64(payload.size()));
    CHECK(accumulated(r, "cc.file.bytes_read") >= f64(payload.size()));

    cc::remove_file(path);
}
