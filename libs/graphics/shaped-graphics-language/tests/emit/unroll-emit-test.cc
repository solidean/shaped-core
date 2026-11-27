#include "emit-test-support.hh"

#include <clean-core/common/macros.hh>
#include <clean-core/platform/file_path.hh>
#include <clean-core/streams/file_stream.hh>
#include <shaped-graphics-language/driver/compile_to_text.hh>

#include <cstdlib>

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

TEST("sgl emit - an unrolled for whose bound reads the index of an unrolled for written as a loop is a loop too")
{
    constexpr cc::string_view source = "binding io:\n"
                                       "    source: buffer[float]\n"
                                       "    target: mut buffer[float]\n"
                                       "\n"
                                       "@compute(64) fun triangle(@thread_id id: int3){io}:\n"
                                       "    let mut total = 0.0\n"
                                       "    @unroll\n"
                                       "    for row in 0 ..< 300:\n"
                                       "        @unroll\n"
                                       "        for column in 0 ..< row + 1:\n"
                                       "            total += io.source[row * 300 + column]\n"
                                       "    io.target[id.x] = total\n";
    // the two warnings, and no error
    auto const checked = check_sources(read_prelude(), source);
    CHECK(reports_of(checked)
          == "unroll-too-long user:[0 ..< 300] an unrolled for writes its body at most 256 times, and this range holds "
             "300, so it is written as a loop\n"
             "unroll-too-long user:[0 ..< row + 1] an unrolled for runs over a constant range, and this one is no "
             "constant inside the unrolled for over `0 ..< 300`, which is written as a loop, so this one is written as "
             "a loop too\n");
    for (auto const t : sgl::emit::all_targets())
    {
        auto const looped = sgl::emit::emit(checked.module, 0, t);
        CHECK(sgl::emit::dump_errors(looped) == "");
        CHECK(occurrences(looped.text, "for (") == 2).dump("text", looped.text);
    }
}

TEST("sgl emit - an unrolled for of an inlined function names the call it was reached through")
{
    constexpr cc::string_view callee = "fun sum_to(n: int) -> float:\n"
                                       "    let mut total = 0.0\n"
                                       "    @unroll\n"
                                       "    for i in 0 ..< n:\n"
                                       "        total += i as float\n"
                                       "    return total\n"
                                       "\n"
                                       "binding io:\n"
                                       "    target: mut buffer[float]\n"
                                       "\n";
    // a bound its argument leaves no constant
    auto const runtime = check_sources(read_prelude(), cc::format("{}@compute(64) fun cs(@thread_id id: int3){{io}}:\n"
                                                                  "    io.target[id.x] = sum_to(id.x)\n",
                                                                  callee));
    CHECK(reports_of(runtime)
          == "invalid-constant-argument user:[0 ..< n] an unrolled for runs over a constant range: both bounds are int "
             "constants once the function is inlined, such as `0 ..< taps` with `taps` a const\n"
             "  note user:[sum_to(id.x)] reached through this call, whose arguments stand in for its parameters\n");
    // and one its argument makes too long
    auto const wide = check_sources(read_prelude(), cc::format("{}@compute(64) fun cs(@thread_id id: int3){{io}}:\n"
                                                               "    io.target[id.x] = sum_to(300)\n",
                                                               callee));
    CHECK(reports_of(wide)
          == "unroll-too-long user:[0 ..< n] an unrolled for writes its body at most 256 times, and this range holds "
             "300, so it is written as a loop\n"
             "  note user:[sum_to(300)] reached through this call, whose arguments stand in for its parameters\n");
}

TEST("sgl emit - an unrolled index's arithmetic shifted in HLSL is written as an int, which DXC otherwise refuses")
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
    // the copy's `i + 1` is a constant, which F1 folds to the literal it is (LEGAL-58)
    CHECK(hlsl.contains("m = m | (int(1) << n);\n")).dump("hlsl", hlsl);
    CHECK(hlsl.contains("m = m | (int(3) << n);\n")).dump("hlsl", hlsl);
    CHECK(!hlsl.contains("(1 << n)")).dump("hlsl", hlsl);
}

TEST("sgl emit - @unroll(false) keeps a loop and writes HLSL's [loop], a hint no other target spells")
{
    // CHK-414, on an exclusive range and on an inclusive one whose last bound is no constant
    auto const source = cc::string("binding io:\n"
                                   "    source: buffer[float]\n"
                                   "    target: mut buffer[float]\n"
                                   "\n"
                                   "@compute(64) fun sum(@thread_id id: int3){io}:\n"
                                   "    let mut total = 0.0\n"
                                   "    @unroll(false)\n"
                                   "    for i in 0 ..< 8:\n"
                                   "        total += io.source[i]\n"
                                   "    @unroll(false)\n"
                                   "    for j in 0 ..= id.x:\n"
                                   "        total += io.source[j]\n"
                                   "    for k in 0 ..< 2:\n"
                                   "        total += io.source[k]\n"
                                   "    io.target[id.x] = total\n");
    auto const hlsl = text_of(source, target::hlsl_dx12);
    auto hints = cc::string(hlsl);
    CHECK(hints.replace_all("[loop]", "") == 2).dump("hlsl", hlsl);
    CHECK(hlsl.contains("[loop]\n    for (int i = 0; i < 8; ++i)")).dump("hlsl", hlsl);
    for (auto const t : {target::wgsl, target::msl})
    {
        auto const text = text_of(source, t);
        CHECK(!text.contains("[loop]")).dump("text", text);
        CHECK(text.contains("for")).dump("text", text);
    }
}

namespace
{
/// One entry point per form of `@unroll` a loop may carry, alone and nested, with the exits a loop may take.
constexpr cc::string_view k_hinted = "binding io:\n"
                                     "    source: buffer[float]\n"
                                     "    target: mut buffer[float]\n"
                                     "\n"
                                     "@compute(64) fun kept(@thread_id id: int3){io}:\n"
                                     "    let mut total = 0.0\n"
                                     "    @unroll(false)\n"
                                     "    for i in 0 ..< id.x:\n"
                                     "        total += io.source[i]\n"
                                     "    for k in 0 ..< id.y:\n"
                                     "        total += io.source[k] * 2.0\n"
                                     "    io.target[id.x] = total\n"
                                     "\n"
                                     "@compute(64) fun inclusive(@thread_id id: int3){io}:\n"
                                     "    let mut total = 0.0\n"
                                     "    @unroll(false)\n"
                                     "    for i in 0 ..= id.x:\n"
                                     "        total += io.source[i]\n"
                                     "    io.target[id.x] = total\n"
                                     "\n"
                                     "@compute(64) fun outer(@thread_id id: int3){io}:\n"
                                     "    let mut total = 0.0\n"
                                     "    @unroll(false)\n"
                                     "    for i in 0 ..< id.x:\n"
                                     "        for j in 0 ..< id.y:\n"
                                     "            if io.source[j] > 4.0 => break\n"
                                     "            if io.source[j] < 0.0 => continue\n"
                                     "            total += io.source[i * 8 + j]\n"
                                     "        if total > 100.0 => break\n"
                                     "    io.target[id.x] = total\n"
                                     "\n"
                                     "@compute(64) fun inner(@thread_id id: int3){io}:\n"
                                     "    let mut total = 0.0\n"
                                     "    for a in 0 ..< id.y:\n"
                                     "        @unroll(false)\n"
                                     "        for b in 0 ..< id.z:\n"
                                     "            if io.source[b] < 0.0 => continue\n"
                                     "            if io.source[b] > 9.0 => break\n"
                                     "            total += io.source[a + b]\n"
                                     "    io.target[id.x] = total\n"
                                     "\n"
                                     "@compute(64) fun cased(@thread_id id: int3){io}:\n"
                                     "    let mut total = 0.0\n"
                                     "    @unroll(false)\n"
                                     "    for i in 0 ..< id.x:\n"
                                     "        case i & 3:\n"
                                     "            0 => continue\n"
                                     "            1 => break\n"
                                     "            _ => total += io.source[i]\n"
                                     "    io.target[id.x] = total\n"
                                     "\n"
                                     "@compute(64) fun unrolled(@thread_id id: int3){io}:\n"
                                     "    let mut total = 0.0\n"
                                     "    @unroll\n"
                                     "    for i in 0 ..< 4:\n"
                                     "        total += io.source[id.x + i]\n"
                                     "    io.target[id.x] = total\n";

/// Each entry point of `k_hinted`, and the loop control of each `OpLoopMerge` SGL's own SPIR-V writes for it, in order.
struct hinted_entry
{
    cc::string_view name;
    cc::span<u32 const> controls;
};
constexpr u32 k_none = 0;
constexpr u32 k_dont_unroll = 2;
constexpr u32 k_kept_controls[] = {k_dont_unroll, k_none};
constexpr u32 k_inclusive_controls[] = {k_dont_unroll};
constexpr u32 k_outer_controls[] = {k_dont_unroll, k_none};
constexpr u32 k_inner_controls[] = {k_none, k_dont_unroll};
constexpr u32 k_cased_controls[] = {k_dont_unroll};
constexpr hinted_entry k_hinted_entries[] = {
    {"kept", k_kept_controls},   {"inclusive", k_inclusive_controls}, {"outer", k_outer_controls},
    {"inner", k_inner_controls}, {"cased", k_cased_controls},         {"unrolled", {}},
};

[[nodiscard]] cc::result<sgl::emitted_source, cc::string> native_of(cc::string_view source,
                                                                    cc::string_view entry,
                                                                    sgl::emit::target t)
{
    return sgl::compile_to_text(
        {.source = source, .entry_point = entry, .target = t, .native = sgl::emit::native_route::require});
}

/// The loop control of every `OpLoopMerge` in `words`, in the order the module holds them.
[[nodiscard]] cc::vector<u32> loop_controls(cc::span<u32 const> words)
{
    auto controls = cc::vector<u32>();
    for (auto at = isize(5); at < words.size(); at += isize(words[at] >> 16))
        if ((words[at] & 0xffffu) == 246) // OpLoopMerge
            controls.push_back(words[at + 3]);
    return controls;
}

[[nodiscard]] bool has_spirv_val()
{
#if defined(CC_OS_EMSCRIPTEN) || defined(CC_OS_IOS) || defined(CC_OS_TVOS)
    return false;
#elif defined(CC_OS_WINDOWS)
    return std::system("spirv-val --version > NUL 2>&1") == 0;
#else
    return std::system("spirv-val --version > /dev/null 2>&1") == 0;
#endif
}

/// Whether spirv-val takes `words`, written to a temporary file of their own; only where `has_spirv_val`.
[[nodiscard]] bool spirv_val_takes(cc::span<u32 const> words, cc::string_view name)
{
#if defined(CC_OS_EMSCRIPTEN) || defined(CC_OS_IOS) || defined(CC_OS_TVOS)
    (void)words;
    (void)name;
    return true;
#else
    auto const path = cc::temp_file_path(cc::format("sgl-unroll-{}", name), ".spv");
    {
        auto adapter = cc::file_write_stream_adapter::create(path);
        REQUIRE(adapter.has_value());
        auto stream = adapter.value().stream();
        REQUIRE(stream.write(words.as_bytes()).has_value());
        REQUIRE(stream.flush().has_value());
    }
    auto command = cc::format("spirv-val --target-env vulkan1.3 --scalar-block-layout \"{}\"", path);
    auto const takes = std::system(command.c_str_materialize()) == 0;
    (void)cc::remove_file(path);
    return takes;
#endif
}
} // namespace

TEST("sgl emit - @unroll(false) is DontUnroll on SGL's own SPIR-V, on its loop alone, past every exit (EMIT-291)")
{
    auto const validates = has_spirv_val();
    for (auto const& entry : k_hinted_entries)
    {
        auto const e = native_of(k_hinted, entry.name, sgl::emit::target::hlsl_vulkan);
        REQUIRE(e.has_value()).dump("entry", entry.name).dump("error", e.has_error() ? e.error() : cc::string());
        auto const controls = loop_controls(e.value().spirv);
        REQUIRE(controls.size() == entry.controls.size()).dump("entry", entry.name);
        for (auto k = isize(0); k < controls.size(); ++k)
            CHECK(controls[k] == entry.controls[k]).dump("entry", entry.name).dump("loop", k);
        if (validates)
            CHECK(spirv_val_takes(e.value().spirv, entry.name)).dump("entry", entry.name);
    }
    if (!validates)
        SKIP("no spirv-val on the path");
}

TEST("sgl emit - @unroll(false) heads its loop through SGL's DXIL passes, which drop the loop's structure (EMIT-291)")
{
    // the program a reader of the native route sees is the one the writer wrote from, its headers' hints among it;
    // slib's tests read the `llvm.loop` metadata of the DXIL itself, through DXC
    for (auto const& entry : k_hinted_entries)
    {
        auto const e = native_of(k_hinted, entry.name, sgl::emit::target::hlsl_dx12);
        REQUIRE(e.has_value()).dump("entry", entry.name).dump("error", e.has_error() ? e.error() : cc::string());
        REQUIRE(!e.value().dxil.empty());
        auto rolled = isize(0);
        for (auto const c : entry.controls)
            rolled += c == k_dont_unroll ? 1 : 0;
        CHECK(occurrences(e.value().text, " rolled\n") == rolled).dump("entry", entry.name).dump("program", e.value().text);
    }
}

TEST("sgl emit - an unrolled for past 256 copies is a loop that asks nothing of the target's unrolling")
{
    // CHK-389 writes it as an ordinary loop, so no route spells a hint: HLSL no [unroll], SPIR-V `None`
    auto const source = row_source(257);
    auto const spirv = native_of(source, "row", sgl::emit::target::hlsl_vulkan);
    REQUIRE(spirv.has_value()).dump("error", spirv.has_error() ? spirv.error() : cc::string());
    auto const controls = loop_controls(spirv.value().spirv);
    REQUIRE(controls.size() == 1);
    CHECK(controls[0] == k_none);
    auto const dxil = native_of(source, "row", sgl::emit::target::hlsl_dx12);
    REQUIRE(dxil.has_value()).dump("error", dxil.has_error() ? dxil.error() : cc::string());
    CHECK(!dxil.value().text.contains(" rolled\n"));
}

namespace
{
/// More shapes a hinted loop may take: nested in the inclusive range's own loop, walked, bounded, and with a subgroup
/// operation, which keeps DXIL's passes structured.
constexpr cc::string_view k_shapes = "require subgroups\n"
                                     "\n"
                                     "binding io:\n"
                                     "    source: buffer[float]\n"
                                     "    target: mut buffer[float]\n"
                                     "    n: int\n"
                                     "    limit: float\n"
                                     "\n"
                                     "@compute(64) fun rolled_around_plain(@thread_id id: int3){io}:\n"
                                     "    let mut total = 0.0\n"
                                     "    @unroll(false)\n"
                                     "    for k in 0 ..= id.y:\n"
                                     "        for i in 0 ..= id.x:\n"
                                     "            total += io.source[i + k]\n"
                                     "    io.target[id.x] = total\n"
                                     "\n"
                                     "@compute(64) fun plain_around_rolled(@thread_id id: int3){io}:\n"
                                     "    let mut total = 0.0\n"
                                     "    for k in 0 ..= id.y:\n"
                                     "        @unroll(false)\n"
                                     "        for i in 0 ..= id.x:\n"
                                     "            total += io.source[i + k]\n"
                                     "    io.target[id.x] = total\n"
                                     "\n"
                                     "@compute(64) fun walked(@thread_id id: int3){io}:\n"
                                     "    let mut total = 0.0\n"
                                     "    @unroll(false)\n"
                                     "    for i in (0 ..< 64).by(3).reversed:\n"
                                     "        total += io.source[i + id.x]\n"
                                     "    io.target[id.x] = total\n"
                                     "\n"
                                     "@compute(64) fun bounded(@thread_id id: int3){io}:\n"
                                     "    let mut total = 0.0\n"
                                     "    @unroll(false)\n"
                                     "    @max_iterations(16)\n"
                                     "    for i in 0 ..< id.x:\n"
                                     "        total += io.source[i]\n"
                                     "    io.target[id.x] = total\n"
                                     "\n"
                                     "@compute(64) fun waved(@thread_id id: int3){io}:\n"
                                     "    let mut total = 0.0\n"
                                     "    for k in 0 ..< io.n:\n"
                                     "        @unroll(false)\n"
                                     "        for i in 0 ..< io.n:\n"
                                     "            total += subgroup_add(io.source[i + k])\n"
                                     "            if total > io.limit => break\n"
                                     "            if io.source[i] < 0.0 => continue\n"
                                     "            total += 1.0\n"
                                     "    io.target[id.x] = total\n";

constexpr u32 k_rolled_around_plain_controls[] = {k_dont_unroll, k_none};
constexpr u32 k_plain_around_rolled_controls[] = {k_none, k_dont_unroll};
constexpr u32 k_walked_controls[] = {k_dont_unroll};
constexpr u32 k_bounded_controls[] = {k_dont_unroll};
constexpr u32 k_waved_controls[] = {k_none, k_dont_unroll};
constexpr hinted_entry k_shape_entries[] = {
    {"rolled_around_plain", k_rolled_around_plain_controls},
    {"plain_around_rolled", k_plain_around_rolled_controls},
    {"walked", k_walked_controls},
    {"bounded", k_bounded_controls},
    {"waved", k_waved_controls},
};
} // namespace

TEST("sgl emit - @unroll(false) on an inclusive range's loop, a walk, a bounded loop and a subgroup's loop is "
     "DontUnroll "
     "on its loop alone (EMIT-291)")
{
    auto const validates = has_spirv_val();
    for (auto const& entry : k_shape_entries)
    {
        auto const e = native_of(k_shapes, entry.name, sgl::emit::target::hlsl_vulkan);
        REQUIRE(e.has_value()).dump("entry", entry.name).dump("error", e.has_error() ? e.error() : cc::string());
        auto const controls = loop_controls(e.value().spirv);
        REQUIRE(controls.size() == entry.controls.size()).dump("entry", entry.name);
        for (auto k = isize(0); k < controls.size(); ++k)
            CHECK(controls[k] == entry.controls[k]).dump("entry", entry.name).dump("loop", k);
        if (validates)
            CHECK(spirv_val_takes(e.value().spirv, entry.name)).dump("entry", entry.name);

        // DXIL's passes keep the hint on the header of the loop it was written on alone
        auto const d = native_of(k_shapes, entry.name, sgl::emit::target::hlsl_dx12);
        REQUIRE(d.has_value()).dump("entry", entry.name).dump("error", d.has_error() ? d.error() : cc::string());
        auto rolled = isize(0);
        for (auto const c : entry.controls)
            rolled += c == k_dont_unroll ? 1 : 0;
        CHECK(occurrences(d.value().text, " rolled\n") == rolled).dump("entry", entry.name).dump("program", d.value().text);
    }
    if (!validates)
        SKIP("no spirv-val on the path");
}
