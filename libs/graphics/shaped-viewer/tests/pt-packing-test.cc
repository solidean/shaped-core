#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/string.hh>
#include <nexus/test.hh>
#include <shaped-graphics-language/driver/test_source.hh>

// pt_packing.sgl carries its own tests, which SGL's interpreter runs when the file is compiled on its own.
// What the packed ReSTIR records then do to an image is the wavefront tests' to measure.

TEST("sv::pt_packing - the packing helpers check clean and pass their own tests")
{
    auto const path = cc::string(SV_SGL_MODULE_DIR) + "/pt_packing.sgl";
    auto adapter = cc::file_read_stream_adapter::open(path);
    REQUIRE(adapter.has_value());
    auto stream = adapter.value().stream();
    auto const bytes = stream.read_all();
    REQUIRE(bytes.has_value());
    auto const text
        = cc::string(cc::string_view(reinterpret_cast<char const*>(bytes.value().data()), bytes.value().size()));

    auto const t = sgl::test_source(text, "pt_packing.sgl");
    CHECK(t.is_clean()).dump("errors", t.errors);
    CHECK(t.tests_run > 0);
    CHECK(t.tests_run == t.test_count);
    CHECK(t.tests_passed == t.tests_run);
}
