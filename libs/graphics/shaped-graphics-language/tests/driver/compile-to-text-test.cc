#include "../emit/emit-test-support.hh"

#include <clean-core/string/format.hh>
#include <shaped-graphics-language/driver/compile_to_text.hh>
#include <shaped-graphics-language/driver/prelude.hh>
#include <shaped-graphics-language/source/format_diagnostic.hh>

using namespace sgl_test;
using sgl::emit::target;

namespace
{
cc::string cube_source()
{
    return read_text(cc::string(SGL_SAMPLES_DIR) + "/cube.sgl");
}

cc::string error_of(sgl::text_request const& request)
{
    auto const r = sgl::compile_to_text(request);
    REQUIRE(r.has_error());
    return r.error();
}

/// A pixel shader whose one `let` is a left-deep sum of `terms` reads, which nests one level per term.
cc::string sum_source(int terms)
{
    auto sum = cc::string("p.v");
    for (auto i = 1; i < terms; ++i)
        sum += " + p.v";
    return cc::format("@pixel struct target:\n"
                      "    color: float4\n"
                      "\n"
                      "struct pixel_input:\n"
                      "    @position position: hpos4\n"
                      "    v: float\n"
                      "\n"
                      "@pixel fun main_ps(p: pixel_input) -> target:\n"
                      "    let x = {}\n"
                      "    return {{\n"
                      "        color = float4(x, x, x, 1.0)\n"
                      "    }}\n",
                      sum);
}
} // namespace

TEST("sgl driver - the prelude is the generated builtins and the hand-written files, and each matches its file")
{
    auto const files = sgl::prelude_files();
    REQUIRE(files.size() == 3);
    CHECK(files[0].name == "builtins.sgl");
    CHECK(files[1].name == "core.sgl");
    CHECK(files[2].name == "raytracing.sgl");

    // The committed builtins.sgl is what the registry generates, byte for byte: `uv run dev.py check sgl-prelude --fix` rewrites it.
    // That is also what makes a diagnostic's line and column right in the committed file, which the compiler never opens.
    CHECK(files[0].source == read_text(cc::string(SGL_PRELUDE_DIR) + "/builtins.sgl"));
    // core.sgl is embedded when CMake configures, and editing it re-runs the configure.
    CHECK(files[1].source == read_text(cc::string(SGL_PRELUDE_DIR) + "/core.sgl"));
    CHECK(files[2].source == read_text(cc::string(SGL_PRELUDE_DIR) + "/raytracing.sgl"));
}

TEST("sgl driver - a line and a column are 1-based, and end of file is a place")
{
    auto const source = cc::string_view("ab\ncd\r\nef");
    auto const at = [](i32 line, i32 column) { return sgl::line_column{.line = line, .column = column}; };
    CHECK(sgl::line_column_of(source, 0) == at(1, 1));
    CHECK(sgl::line_column_of(source, 2) == at(1, 3));
    CHECK(sgl::line_column_of(source, 3) == at(2, 1));
    CHECK(sgl::line_column_of(source, 7) == at(3, 1));
    CHECK(sgl::line_column_of(source, 9) == at(3, 3));
    CHECK(sgl::line_column_of(source, 100) == at(3, 3));

    // a bare `\r` ends a line, as it does in the line tree
    auto const old_mac = cc::string_view("ab\rcd\r\ref");
    CHECK(sgl::line_column_of(old_mac, 2) == at(1, 3));
    CHECK(sgl::line_column_of(old_mac, 3) == at(2, 1));
    CHECK(sgl::line_column_of(old_mac, 6) == at(3, 1));
    CHECK(sgl::line_column_of(old_mac, 7) == at(4, 1));
}

TEST("sgl driver - every diagnostic kind has a summary a reader understands without its name")
{
    // `unroll_too_long` is the last kind; a kind added after it moves this bound
    for (auto k = 0; k <= int(sgl::diagnostic_kind::unroll_too_long); ++k)
    {
        auto const kind = sgl::diagnostic_kind(k);
        CHECK(!sgl::summary_of(kind).empty());
        CHECK(sgl::summary_of(kind) != sgl::to_string(kind));
    }
}

TEST("sgl driver - a diagnostic is one line with its place, its level and its kind")
{
    auto const d = sgl::diagnostic{.kind = sgl::diagnostic_kind::unknown_name,
                                   .level = sgl::severity::normal_error,
                                   .where = {.offset = 4, .length = 3}};
    CHECK(sgl::format_diagnostic("a.sgl", "ab\ncd", d, "foo") == "a.sgl:2:2: error: unknown-name: foo");
    CHECK(sgl::format_diagnostic("a.sgl", "ab\ncd", d)
          == "a.sgl:2:2: error: unknown-name: a name that nothing in scope declares");
    // without a detail, the kind's summary says what the name alone does not
    CHECK(sgl::summary_of(sgl::diagnostic_kind::unknown_name) == "a name that nothing in scope declares");
    CHECK(sgl::format_note("a.sgl", "ab\ncd", {.offset = 1, .length = 1}, "declared here")
          == "a.sgl:1:2: note: declared here");
}

TEST("sgl driver - an entry point is found by its name, and the text is the emitter's")
{
    auto const source = cube_source();
    auto const checked = check_sources(read_prelude(), source);
    for (auto const t : sgl::emit::all_targets())
    {
        auto const vs = sgl::compile_to_text({.source = source, .entry_point = "main_vs", .target = t});
        auto const ps = sgl::compile_to_text(
            {.source = source, .entry_point = "main_ps", .stage = sgl::check::stage::pixel, .target = t});
        REQUIRE(vs.has_value());
        REQUIRE(ps.has_value());
        CHECK(vs.value().text == sgl::emit::emit(checked.module, 0, t).text);
        CHECK(ps.value().text == sgl::emit::emit(checked.module, 1, t).text);
    }
}

TEST("sgl driver - a missing entry point names the ones the source holds")
{
    auto const source = cube_source();
    CHECK(error_of({.source = source, .source_name = "cube.sgl", .entry_point = "main_cs", .target = target::wgsl})
          == "cube.sgl: error: no entry point named 'main_cs' (the source holds: vertex 'main_vs', pixel 'main_ps')\n");
}

TEST("sgl driver - an entry point of another stage is an error")
{
    auto const source = cube_source();
    CHECK(error_of({.source = source,
                    .source_name = "cube.sgl",
                    .entry_point = "main_ps",
                    .stage = sgl::check::stage::vertex,
                    .target = target::wgsl})
          == "cube.sgl: error: entry point 'main_ps' is a pixel entry point, and a vertex one was asked for\n");
}

TEST("sgl driver - a broken source reports where and what, and gives no text")
{
    auto const source = cc::string_view("@pixel struct target:\n"
                                        "    color: float4\n"
                                        "\n"
                                        "struct pixel_input:\n"
                                        "    @position position: hpos4\n"
                                        "\n"
                                        "@pixel fun main_ps(p: pixel_input) -> target:\n"
                                        "    return {\n"
                                        "        color = float4(missing, 0.0, 0.0, 1.0)\n"
                                        "    }\n");
    CHECK(error_of({.source = source, .source_name = "broken.sgl", .entry_point = "main_ps", .target = target::hlsl_dx12})
          == "broken.sgl:9:24: error: unknown-name: missing\n");
}

TEST("sgl driver - a warning does not fail the compile, and comes back with the text, one entry each")
{
    // a statement that does nothing is the AST pass's warning, and 257 values staying a loop is the check pass's
    constexpr auto source = cc::string_view("binding work:\n"
                                            "    values: mut buffer[int]\n"
                                            "\n"
                                            "@compute(64) fun main(@thread_id id: int3){work}:\n"
                                            "    let mut t = 0\n"
                                            "    t + 1\n"
                                            "    @unroll\n"
                                            "    for i in 0 ..< 257:\n"
                                            "        t += i\n"
                                            "    work.values[id.x] = t\n");
    auto const expected_no_effect = cc::string_view("warned.sgl:6:5: warning: no-effect: ");
    auto const expected_unroll = cc::string_view("warned.sgl:8:14: warning: unroll-too-long: "
                                                 "an unrolled for writes its body at most 256 times, "
                                                 "and this range holds 257, so it is written as a loop");

    for (auto const t : sgl::emit::all_targets())
    {
        auto const r
            = sgl::compile_to_text({.source = source, .source_name = "warned.sgl", .entry_point = "main", .target = t});
        REQUIRE(r.has_value()).context(r.has_error() ? r.error() : cc::string());
        auto const& warnings = r.value().warnings;
        REQUIRE(warnings.size() == 2);
        CHECK(warnings[0].starts_with(expected_no_effect)).context(warnings[0]);
        CHECK(!warnings[0].contains("\n")).context(warnings[0]);
        CHECK(warnings[1] == expected_unroll).context(warnings[1]);
    }

    // every entry point's text carries the source's warnings alike
    auto const all
        = sgl::compile_all_to_text({.source = source, .source_name = "warned.sgl", .targets = sgl::emit::all_targets()});
    REQUIRE(all.has_value());
    for (auto const& e : all.value())
    {
        REQUIRE(e.text.has_value());
        REQUIRE(e.text.value().warnings.size() == 2);
        CHECK(e.text.value().warnings[1] == expected_unroll);
    }

    // a failure's error holds its errors alone
    auto const broken = cc::string(source) + "    work.values[id.x] = missing\n";
    auto const e = error_of({.source = broken, .source_name = "warned.sgl", .entry_point = "main"});
    CHECK(e == "warned.sgl:11:25: error: unknown-name: missing\n");
}

TEST("sgl driver - what is judged of one entry point's inlined body is no error of another one's compile")
{
    // CHK-402: the vertex stage never reaches the pixel stage's helper, so its compile says nothing of the helper
    constexpr auto source
        = cc::string_view("require binding_arrays\n"
                          "\n"
                          "binding textures:\n"
                          "    maps: texture_2d[float4][16]\n"
                          "    s: sampler\n"
                          "\n"
                          "binding draw:\n"
                          "    slot: int\n"
                          "\n"
                          "@vertex struct vertex_in:\n"
                          "    position: float3\n"
                          "\n"
                          "struct varyings:\n"
                          "    @position position: hpos4\n"
                          "    uv: float2\n"
                          "    @interpolate(.flat) picked: int\n"
                          "\n"
                          "@pixel struct target:\n"
                          "    color: float4\n"
                          "\n"
                          "@vertex fun main_vs(v: vertex_in) -> varyings:\n"
                          "    return {position = hpos4(v.position.x, v.position.y, v.position.z, "
                          "1.0), uv = float2(0.0, 0.0), picked = 0}\n"
                          "\n"
                          "fun sample_map(map: int, uv: float2){textures} -> float4 => "
                          "textures.maps[map].sample(uv, textures.s)\n"
                          "\n"
                          "@pixel fun main_ps(v: varyings){textures, draw} -> target:\n"
                          "    return {color = sample_map(draw.slot, v.uv) + sample_map(v.picked, v.uv)}\n");
    auto const vs = sgl::compile_to_text(
        {.source = source, .source_name = "split.sgl", .entry_point = "main_vs", .target = target::msl});
    REQUIRE(vs.has_value()).context(vs.has_error() ? vs.error() : cc::string());
    CHECK(vs.value().warnings.empty());
    auto const ps
        = error_of({.source = source, .source_name = "split.sgl", .entry_point = "main_ps", .target = target::msl});
    CHECK(ps.starts_with("split.sgl:24:75: error: mixed-index: "));

    // compiling every entry point at once fails the one the error is judged of alone
    auto const all = sgl::compile_all_to_text({.source = source, .source_name = "split.sgl", .targets = {target::msl}});
    REQUIRE(all.has_value());
    REQUIRE(all.value().size() == 2);
    CHECK(all.value()[0].text.has_value());
    REQUIRE(all.value()[1].text.has_error());
    CHECK(all.value()[1].text.error() == ps);

    // a needless mark is a warning of the entry points that reach it, and of no other
    auto needless = cc::string(source);
    needless.replace_all("textures.maps[map]", "textures.maps[nonuniform map]");
    needless.replace_all("sample_map(v.picked, v.uv)", "sample_map(draw.slot + 1, v.uv)");
    auto const quiet = sgl::compile_to_text(
        {.source = needless, .source_name = "split.sgl", .entry_point = "main_vs", .target = target::msl});
    REQUIRE(quiet.has_value());
    CHECK(quiet.value().warnings.empty());
    auto const warned = sgl::compile_to_text(
        {.source = needless, .source_name = "split.sgl", .entry_point = "main_ps", .target = target::msl});
    REQUIRE(warned.has_value());
    REQUIRE(warned.value().warnings.size() == 1);
    CHECK(warned.value().warnings[0].contains("warning: needless-nonuniform: "));
}

// `main_thread` for the stack: this is the one test that deliberately builds a tree AT the limit, and the check pass
// recurses over the AST before any flat-tree guard applies.
// A worker thread has 512 KiB where the main thread has 8 MiB, and an unoptimized sanitizer build spends enough per
// `check_expr` level to overflow the smaller one well before 40.
TEST("sgl driver - a tree past the depth limit is refused by name, never as a hole a later pass trips over", main_thread)
{
    // The `let` is a level of its own, so 39 terms reach exactly 40 levels and 40 terms reach 41.
    // Every target, since each legalizes and emits the tree the check pass handed over.
    for (auto const t : sgl::emit::all_targets())
    {
        auto const at_limit = sgl::compile_to_text({.source = sum_source(39), .entry_point = "main_ps", .target = t});
        REQUIRE(at_limit.has_value()).context(at_limit.has_error() ? at_limit.error() : cc::string());

        auto const past
            = error_of({.source = sum_source(40), .source_name = "sum.sgl", .entry_point = "main_ps", .target = t});
        CHECK(past.contains("error: nesting-too-deep: 'main_ps' nests deeper than 40 levels")).context(past);
        CHECK(!past.contains("not-core")).context(past);
    }
}

TEST("sgl driver - a request to run the tests makes one that fails an error, and an untested request ignores them")
{
    auto const source = cube_source() + "\ntest 1 < 2\n\n// deliberately false\ntest 2 < 1\n";
    CHECK(sgl::compile_to_text({.source = source, .entry_point = "main_ps"}).has_value());
    auto const e = error_of({.source = source, .source_name = "cube.sgl", .entry_point = "main_ps", .run_tests = true});
    CHECK(e.contains(": error: test-failed: 1 of 1 checks failed (deliberately false)\n"));
    CHECK(e.contains(": note: `2 < 1` is 2 < 1\n"));
}

TEST("sgl driver - a file-scope sampler the code reaches is an interface binding of no group, at its index")
{
    constexpr auto source
        = "sampler unused:\n"
          "    filter = .linear\n"
          "\n"
          "sampler edge:\n"
          "    filter = .nearest\n"
          "\n"
          "binding set:\n"
          "    src: texture_2d[float4]\n"
          "    dst: out image_2d[.rgba8_unorm]\n"
          "\n"
          "@compute(8, 8) fun cs(@thread_id id: int3){set}:\n"
          "    set.dst.store(int2(id.x, id.y), set.src.sample(float2(0.5, 0.5), edge, level = 0.0))\n";
    auto const r = sgl::compile_to_text({.source = source, .entry_point = "cs", .target = target::wgsl});
    REQUIRE(r.has_value());
    auto const& bindings = r.value().bindings;
    REQUIRE(bindings.size() == 3);
    auto const& smp = bindings[2];
    CHECK(smp.name == "edge");
    CHECK(smp.emitted == "edge");
    CHECK(smp.is_file_sampler);
    CHECK(smp.kind == sgl::described_member_kind::sampler);
    CHECK(smp.group == -1);
    CHECK(smp.slot == 1);
    CHECK(smp.is_used);
    CHECK(smp.sampler_type == "non_filtering");
    // a static sampler is the layout's, so no footprint names it: barriers track what a group binds
    for (auto const& slot : r.value().footprint)
        CHECK(slot.host_name != "edge");
}
