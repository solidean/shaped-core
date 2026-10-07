#include "emit-test-support.hh"

using namespace sgl_test;
using sgl::emit::target;

namespace
{
/// A compute stage that unrolls a loop with a `continue`, and keeps a second loop as a loop.
constexpr cc::string_view k_unrolled = "binding io:\n"
                                       "    source: buffer[float]\n"
                                       "    target: mut buffer[float]\n"
                                       "\n"
                                       "@compute(64) fun blur(@thread_id id: int3){io}:\n"
                                       "    let weights = [0.25, 0.5, 0.25]\n"
                                       "    let mut total = 0.0\n"
                                       "    @unroll\n"
                                       "    for i in 0 ..< 3:\n"
                                       "        if i == 1 => continue\n"
                                       "        total += weights[i] * io.source[id.x + i]\n"
                                       "    for j in 0 ..< 3:\n"
                                       "        total += io.source[j]\n"
                                       "    io.target[id.x] = total\n";

cc::string text_of(cc::string_view source, target t)
{
    auto const e = emit_source(source, 0, t);
    CHECK(sgl::emit::dump_errors(e) == "");
    return e.text;
}
} // namespace

TEST("sgl emit - an unrolled for is written as straight-line code with no hint, the same on every target")
{
    // the copy whose index the `continue` skips is a branch on a constant, so nothing of it is left (CHK-356)
    auto const hlsl = text_of(k_unrolled, target::hlsl_dx12);
    CHECK(hlsl.contains("    total = total + weights[0] * io_source[id.x + 0];\n"
                        "    total = total + weights[2] * io_source[id.x + 2];\n"));
    CHECK(!hlsl.contains("weights[1] *"));
    CHECK(!hlsl.contains("[unroll]"));
    // the loop without the attribute stays the one loop of the text
    CHECK(hlsl.contains("for ("));

    for (auto const t : sgl::emit::all_targets())
    {
        auto const text = text_of(k_unrolled, t);
        CHECK(text.contains("weights[2] *"));
        CHECK(!text.contains("weights[i]"));
    }
}
