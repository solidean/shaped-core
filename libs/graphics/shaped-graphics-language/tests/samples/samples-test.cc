#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/string.hh>
#include <nexus/test.hh>
#include <shaped-graphics-language/debug/dump.hh>
#include <shaped-graphics-language/syntax/parsed_file.hh>

using namespace cc::primitive_defines;

namespace
{
cc::string read_sample(cc::string_view name)
{
    auto adapter = cc::file_read_stream_adapter::open(cc::string(SGL_SAMPLES_DIR) + "/" + name);
    REQUIRE(adapter.has_value());
    auto stream = adapter.value().stream();
    auto const bytes = stream.read_all();
    REQUIRE(bytes.has_value());
    return cc::string(reinterpret_cast<char const*>(bytes.value().data()), bytes.value().size());
}
} // namespace

TEST("sgl samples - a whole raster shader parses, and says only what it should")
{
    auto const file = sgl::parse(read_sample("basic-raster.sgl"));

    // The one thing in it the syntax has not decided is the postfix `..` splat, and that is all it reports.
    CHECK(sgl::dump_diagnostics(file) == "reserved-operator @3563+2\n");
    CHECK(sgl::print_source(file) == file.source);

    auto const forms = sgl::dump_forms(file);
    // A binding list is a fused curly list after the parameters, so a signature is a call of a call.
    CHECK(forms.contains("(call (call id:make_mvp (round (run id:model op:: id:mat4))) (curly id:frame))"));
    // An anonymous return type spans element lines and closes on the line that carries the block colon.
    CHECK(forms.contains("op:-> (curly (run id:pos op:: id:hpos4){@position} (run id:normal op:: id:vec3)"));
    CHECK(forms.contains("(apply id:make_mvp (member model id:instance))"));
    // `sampler` is a keyword, so a static sampler is a keyword form like any other declaration.
    CHECK(forms.contains("(kw kw:sampler id:bilinear)"));
}
