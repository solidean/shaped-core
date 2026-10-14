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

/// A compute stage summing `rows` by `columns` values, both loops unrolled.
cc::string grid_source(int rows, int columns)
{
    return cc::format("binding io:\n"
                      "    source: buffer[float]\n"
                      "    target: mut buffer[float]\n"
                      "\n"
                      "@compute(64) fun grid(@thread_id id: int3){{io}}:\n"
                      "    let mut total = 0.0\n"
                      "    @unroll\n"
                      "    for row in 0 ..< {}:\n"
                      "        @unroll\n"
                      "        for column in 0 ..< {}:\n"
                      "            total += io.source[row * 64 + column]\n"
                      "    io.target[id.x] = total\n",
                      rows, columns);
}

/// A compute stage summing `count` values in one unrolled loop.
cc::string row_source(int count)
{
    return cc::format("binding io:\n"
                      "    source: buffer[float]\n"
                      "    target: mut buffer[float]\n"
                      "\n"
                      "@compute(64) fun row(@thread_id id: int3){{io}}:\n"
                      "    let mut total = 0.0\n"
                      "    @unroll\n"
                      "    for i in 0 ..< {}:\n"
                      "        total += io.source[i]\n"
                      "    io.target[id.x] = total\n",
                      count);
}

isize occurrences(cc::string_view text, cc::string_view part)
{
    auto count = isize(0);
    for (auto at = text.find(part); at >= 0; at = text.find(part, at + part.size()))
        ++count;
    return count;
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
        // HLSL's hint and MSL's, which WGSL has none of
        CHECK(!text.contains("[unroll]"));
        CHECK(!text.contains("#pragma unroll"));
    }
}

TEST("sgl emit - an unrolled for past 256 copies is a loop, counting a nest's copies together")
{
    for (auto const t : sgl::emit::all_targets())
    {
        // 256 values, and 16 inside 16, are every copy the limit allows
        CHECK(!text_of(row_source(256), t).contains("for ("));
        CHECK(!text_of(grid_source(16, 16), t).contains("for ("));

        // 257 values are one loop, which the warning names
        auto const row = check_sources(read_prelude(), row_source(257));
        CHECK(reports_of(row)
              == "unroll-too-long user:[0 ..< 257] an unrolled for writes its body at most 256 times, and "
                 "this range holds 257, so it is written as a loop\n");
        auto const looped = sgl::emit::emit(row.module, 0, t);
        CHECK(sgl::emit::dump_errors(looped) == "");
        CHECK(occurrences(looped.text, "for (") == 1).dump("text", looped.text);

        // 16 inside 17 would be 272, so the rows unroll and each of the 17 holds the columns as a loop
        auto const grid = check_sources(read_prelude(), grid_source(17, 16));
        CHECK(reports_of(grid)
              == "unroll-too-long user:[0 ..< 16] an unrolled for writes its body at most 256 times, "
                 "counted once per copy of the unrolled loops around it, and this range of 16 inside "
                 "17 copies would write 272, so it is written as a loop\n");
        auto const rows = sgl::emit::emit(grid.module, 0, t);
        CHECK(sgl::emit::dump_errors(rows) == "");
        CHECK(occurrences(rows.text, "for (") == 17).dump("text", rows.text);
    }
}

TEST("sgl emit - an unrolled index's arithmetic shifted in HLSL is written as the int it is, which DXC otherwise refuses")
{
    constexpr cc::string_view source = "binding io:\n"
                                       "    target: mut buffer[int]\n"
                                       "\n"
                                       "@compute(64) fun masks(@thread_id id: int3){io}:\n"
                                       "    let n = id.x & 7\n"
                                       "    let mut m = 0\n"
                                       "    @unroll\n"
                                       "    for i in 0 ..< 3:\n"
                                       "        m |= (i + 1) << n\n"
                                       "    io.target[id.x] = m\n";
    auto const hlsl = text_of(source, target::hlsl_dx12);
    CHECK(hlsl.contains("m = m | (int((0 + 1)) << n);\n")).dump("hlsl", hlsl);
    CHECK(hlsl.contains("m = m | (int((2 + 1)) << n);\n")).dump("hlsl", hlsl);
    CHECK(!hlsl.contains("((0 + 1) << n)")).dump("hlsl", hlsl);
}
