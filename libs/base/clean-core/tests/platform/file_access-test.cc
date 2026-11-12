#include <clean-core/platform/file_access.hh>
#include <clean-core/platform/file_path.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

namespace
{
/// Recording is process-wide and a test runner may have it on for the whole run: switch it on, never off again unless this turned it on.
struct recording_scope
{
    bool was = cc::file_access::is_recording();
    recording_scope() { cc::file_access::start_recording(); }
    ~recording_scope()
    {
        if (!was)
            cc::file_access::stop_recording();
    }
};

/// The record of `path` under the context a note made here would carry.
cc::file_access::record find(cc::string_view path)
{
    auto out = cc::file_access::record{};
    auto const context = cc::file_access::current_context();
    for (auto const& r : cc::file_access::snapshot())
        if (r.context == context && cc::string_view(r.path) == path)
            out = r;
    return out;
}
} // namespace

// Recording is process-wide, so the tests switching it share one exclusion tag.
TEST("file_access - streams note what they read and write, with their bytes", exclusive("cc-file-access"))
{
    auto const path = cc::temp_file_path("cc-file-access", ".bin");
    recording_scope const recording;
    {
        auto w = cc::file_write_stream_adapter::create(path);
        REQUIRE(w.has_value());
        auto s = w.value().stream();
        REQUIRE(s.write(cc::as_bytes(cc::string_view("hello"))).has_value());
        REQUIRE(s.flush().has_value());
    }
    {
        auto r = cc::file_read_stream_adapter::open(path);
        REQUIRE(r.has_value());
        auto bytes = cc::read_stream(r.value().stream()).read_all();
        REQUIRE(bytes.has_value());
        CHECK(bytes.value().size() == 5);
    }
    auto const rec = find(path);
    CHECK(rec.read);
    CHECK(rec.written);
    CHECK(!rec.listed);
    CHECK(rec.bytes_read == 5);
    CHECK(rec.bytes_written == 5);
    CHECK(cc::remove_file(path));
}

TEST("file_access - a missing file is noted as read, since finding it missing is an input too",
     exclusive("cc-file-access"))
{
    auto const path = cc::temp_file_path("cc-file-access-missing", ".bin");
    recording_scope const recording;
    CHECK(cc::file_read_stream_adapter::open(path).has_error());
    auto const rec = find(path);
    CHECK(rec.read);
    CHECK(rec.bytes_read == 0);
}

TEST("file_access - code past clean-core declares what it touches", exclusive("cc-file-access"))
{
    // under the temp directory: a runner records these notes as the test's own accesses, so a path in the source tree
    // would read as a write there and as an input the test depends on
    auto const directory = cc::temp_file_path("cc-file-access-declared", "");
    auto const read = cc::format("{}/read.txt", directory);
    auto const written = cc::format("{}/out.txt", directory);
    recording_scope const recording;
    cc::file_access::note_read(read);
    cc::file_access::note_write(written);
    cc::file_access::note_list(directory);
    CHECK(find(read).read);
    CHECK(find(written).written);
    CHECK(find(directory).listed);
}

TEST("file_access - nothing is noted while recording is off", exclusive("cc-file-access"))
{
    if (cc::file_access::is_recording())
        SKIP("a runner records this whole run, so the off state cannot be observed here");
    cc::file_access::note_read("cc-file-access-off/never.txt");
    auto noted = false;
    for (auto const& r : cc::file_access::snapshot())
        noted = noted || cc::string_view(r.path) == "cc-file-access-off/never.txt";
    CHECK(!noted); // a CHECK that runs even when nothing was ever recorded, as in a run where this test comes first
}
