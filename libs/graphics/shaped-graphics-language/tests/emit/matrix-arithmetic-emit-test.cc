#include "emit-test-support.hh"

using namespace sgl_test;
using sgl::emit::target;

// A matrix's arithmetic (EVAL-109): each target's own operators, but `-m`, which WGSL has none of.

namespace
{
constexpr cc::string_view k_negated = "binding work:\n"
                                      "    values: mut buffer[float2]\n"
                                      "\n"
                                      "@compute(1) fun run(){work}:\n"
                                      "    let v = work.values[0]\n"
                                      "    let a = mat2(v, v)\n"
                                      "    let b = -a + a * 2.0\n"
                                      "    work.values[0] = b * v\n";
} // namespace

TEST("sgl emit - a negated matrix is a scale where the target has no `-` of one")
{
    auto const hlsl = emit_source(k_negated, 0, target::hlsl_dx12);
    CHECK(sgl::emit::dump_errors(hlsl) == "");
    CHECK(hlsl.text.contains("-a"));

    for (auto const t : {target::wgsl, target::msl})
    {
        auto const e = emit_source(k_negated, 0, t);
        CHECK(sgl::emit::dump_errors(e) == "").dump("target", sgl::emit::to_string(t));
        CHECK(e.text.contains("-1.0f * a")).dump("target", sgl::emit::to_string(t));
    }
}
