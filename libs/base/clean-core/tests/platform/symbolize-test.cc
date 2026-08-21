#include <clean-core/container/vector.hh>
#include <clean-core/platform/stack_capture.hh>
#include <clean-core/platform/symbolize.hh>
#include <clean-core/string/string.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

// Symbolization is the half of a stack capture that costs money, so what is asserted here is that it resolves what it
// should, admits what it cannot, and answers the same question twice the same way.
//
// Nothing here asserts an exact name: an optimized build inlines, and which frame carries which name is the
// optimizer's business, not a contract.

namespace
{
/// A function distinctive enough to look for by name, and kept out of line so it has a frame to be found in.
CC_DONT_INLINE isize capture_here_for_symbolize_test(cc::span<void*> out)
{
    auto const r = cc::capture_stack(out);
    return r.count;
}

/// Whether this BUILD has names at all.
///
/// Not a platform question: a release preset can ship without debug info, and then resolving nothing is the correct
/// answer rather than a failure.
/// Taking a known function's address answers it without needing a stack at all.
[[nodiscard]] bool build_has_symbols()
{
    if (!cc::symbolizer::is_available())
        return false;

    cc::symbolizer sym;
    return sym.resolve(reinterpret_cast<void const*>(&capture_here_for_symbolize_test)).has_function();
}

/// The same question for line info, which a build can lack while still having names.
[[nodiscard]] bool build_has_line_info()
{
    if (!cc::symbolizer::is_available())
        return false;

    cc::symbolizer sym;
    return sym.resolve(reinterpret_cast<void const*>(&capture_here_for_symbolize_test)).has_line();
}
} // namespace

TEST("symbolize - every address renders as something, resolved or not")
{
    if (!cc::stack_capture_available())
        SKIP("no stack walking on this platform");

    void* frames[32] = {};
    auto const count = capture_here_for_symbolize_test(cc::span<void*>(frames, 32));
    REQUIRE(count > 0);

    cc::symbolizer sym;

    // The contract that holds in every build, symbols or not: a frame renders as SOMETHING a reader can act on — a
    // name, a module and an offset, or an honest <unknown>.
    for (isize i = 0; i < count; ++i)
        CHECK(!sym.resolve(frames[i]).to_string().empty());
}

TEST("symbolize - a captured frame resolves to a function name")
{
    if (!build_has_symbols() || !cc::stack_capture_available())
        SKIP("this build has no symbols");

    void* frames[32] = {};
    auto const count = capture_here_for_symbolize_test(cc::span<void*>(frames, 32));
    REQUIRE(count > 0);

    cc::symbolizer sym;

    auto resolved = 0;
    for (isize i = 0; i < count; ++i)
        if (sym.resolve(frames[i]).has_function())
            ++resolved;

    CHECK(resolved > 0);
}

TEST("symbolize - a captured frame resolves to a source location")
{
    if (!build_has_line_info() || !cc::stack_capture_available())
        SKIP("this build has no line info");

    void* frames[32] = {};
    auto const count = capture_here_for_symbolize_test(cc::span<void*>(frames, 32));
    REQUIRE(count > 0);

    cc::symbolizer sym;

    auto with_line = 0;
    for (isize i = 0; i < count; ++i)
        if (sym.resolve(frames[i]).has_line())
            ++with_line;

    CHECK(with_line > 0);
}

TEST("symbolize - this test's own name is in its own stack")
{
    if (!build_has_symbols() || !cc::stack_capture_available())
        SKIP("this build has no symbols");

    void* frames[32] = {};
    auto const count = capture_here_for_symbolize_test(cc::span<void*>(frames, 32));
    REQUIRE(count > 0);

    cc::symbolizer sym;

    // The helper is CC_DONT_INLINE, so it has a frame, and its name is unusual enough that finding it proves the
    // resolution is real rather than plausible-looking.
    auto found = false;
    for (isize i = 0; i < count; ++i)
        if (sym.resolve(frames[i]).function.contains("capture_here_for_symbolize_test"))
            found = true;

    CHECK(found);
}

TEST("symbolize - an address in no module resolves to nothing, and says so")
{
    if (!cc::symbolizer::is_available())
        SKIP("no symbolization on this platform");

    cc::symbolizer sym;

    // Admitting ignorance is the whole contract: a confident wrong name is worse than a hex address.
    auto const& info = sym.resolve(reinterpret_cast<void*>(u64(0x1234)));
    CHECK(!info.has_function());
    CHECK(!info.has_line());
    CHECK(info.to_string() == "<unknown>");
}

TEST("symbolize - the same address answers the same way, from the cache")
{
    if (!cc::stack_capture_available())
        SKIP("no stack walking on this platform");

    void* frames[32] = {};
    auto const count = capture_here_for_symbolize_test(cc::span<void*>(frames, 32));
    REQUIRE(count > 0);

    cc::symbolizer sym;
    CHECK(sym.cached_count() == 0);

    auto const first = sym.resolve(frames[0]).to_string();
    CHECK(sym.cached_count() == 1);

    // The cache is the point rather than an optimization: a sampled profile is thousands of hits on a few addresses,
    // and each miss is a debug-info lookup.
    auto const second = sym.resolve(frames[0]).to_string();
    CHECK(sym.cached_count() == 1);
    CHECK(first == second);
}

TEST("symbolize - a rendering always says something")
{
    cc::symbol_info info;
    CHECK(info.to_string() == "<unknown>");

    info.module = "app.exe";
    info.module_offset = 0x1234;
    CHECK(info.to_string() == "app.exe+0x1234"); // no symbols, but still findable in a disassembly

    info.function = "render_frame";
    info.displacement = 0x20;
    CHECK(info.to_string() == "render_frame+0x20");

    info.file = "renderer.cc";
    info.line = 42;
    CHECK(info.to_string() == "render_frame at renderer.cc:42");
}
