#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/string.hh>
#include <nexus/source_root.hh>
#include <nexus/test.hh>
#include <shaped-graphics-language/driver/test_source.hh>

using namespace cc::primitive_defines;

// Module `display` carries its own tests — the sRGB reference points, the neutral curve's knee and its hue — which SGL's
// interpreter runs when the file is compiled on its own.

TEST("sr::display - the display module checks clean and passes its own tests")
{
    auto const path = nx::source_path(SR_SGL_MODULE_DIR) + "/display.sgl";
    auto adapter = cc::file_read_stream_adapter::open(path);
    REQUIRE(adapter.has_value());
    auto stream = adapter.value().stream();
    auto const bytes = stream.read_all();
    REQUIRE(bytes.has_value());
    auto const text
        = cc::string(cc::string_view(reinterpret_cast<char const*>(bytes.value().data()), bytes.value().size()));

    auto const t = sgl::test_source(text, "display.sgl");
    CHECK(t.is_clean()).dump("errors", t.errors);
    CHECK(t.tests_run > 0);
    CHECK(t.tests_passed == t.tests_run);
}
