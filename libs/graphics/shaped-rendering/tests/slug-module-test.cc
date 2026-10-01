#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/string.hh>
#include <nexus/test.hh>
#include <shaped-graphics-language/driver/test_source.hh>

using namespace cc::primitive_defines;

// Module `slug` carries its own tests — the root code, both root solves, the band wrap and the fill rules — which SGL's
// interpreter runs when the file is compiled on its own; the texture loop is held to the CPU reference elsewhere.

TEST("sr::slug - the slug module checks clean and passes its own tests")
{
    auto const path = cc::string(SR_SGL_MODULE_DIR) + "/slug.sgl";
    auto adapter = cc::file_read_stream_adapter::open(path);
    REQUIRE(adapter.has_value());
    auto stream = adapter.value().stream();
    auto const bytes = stream.read_all();
    REQUIRE(bytes.has_value());
    auto const text
        = cc::string(cc::string_view(reinterpret_cast<char const*>(bytes.value().data()), bytes.value().size()));

    auto const t = sgl::test_source(text, "slug.sgl");
    CHECK(t.is_clean()).dump("errors", t.errors);
    CHECK(t.tests_run > 0);
    CHECK(t.tests_passed == t.tests_run);
}
