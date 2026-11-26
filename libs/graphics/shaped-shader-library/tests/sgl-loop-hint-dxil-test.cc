#include <clean-core/string/format.hh>
#include <nexus/test.hh>
#include <shaped-graphics-language/driver/compile_to_text.hh>
#include <shaped-shader-compiler-dxc/compiler.hh>

using namespace cc::primitive_defines;

// EMIT-291: a loop written `@unroll(false)` reaches SGL's own DXIL as DXC's compile of `[loop]` writes it.
// Each back edge of the loop carries `!llvm.loop`, a distinct node of its own naming `llvm.loop.unroll.disable`.
// The validator takes it, and DXC's compile of SGL's text marks as many loops.

namespace
{
/// One entry point per shape a hinted loop may take, alone and nested, with the exits a loop may take.
constexpr cc::string_view k_hinted = "require subgroups\n"
                                     "\n"
                                     "binding io:\n"
                                     "    source: buffer[float]\n"
                                     "    target: mut buffer[float]\n"
                                     "    n: int\n"
                                     "    limit: float\n"
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
                                     "@compute(64) fun both(@thread_id id: int3){io}:\n"
                                     "    let mut total = 0.0\n"
                                     "    @unroll(false)\n"
                                     "    for a in 0 ..< id.y:\n"
                                     "        @unroll(false)\n"
                                     "        for b in 0 ..< id.z:\n"
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
                                     "@compute(64) fun waved(@thread_id id: int3){io}:\n"
                                     "    let mut total = 0.0\n"
                                     "    @unroll(false)\n"
                                     "    for i in 0 ..< io.n:\n"
                                     "        total += subgroup_add(io.source[i])\n"
                                     "        if total > io.limit => break\n"
                                     "        if io.source[i] < 0.0 => continue\n"
                                     "        total += 1.0\n"
                                     "    io.target[id.x] = total\n"
                                     "\n"
                                     "@compute(64) fun unrolled(@thread_id id: int3){io}:\n"
                                     "    let mut total = 0.0\n"
                                     "    @unroll\n"
                                     "    for i in 0 ..< 4:\n"
                                     "        total += io.source[id.x + i]\n"
                                     "    io.target[id.x] = total\n";

/// Each entry point of `k_hinted`, and how many of its loops are written `@unroll(false)`.
struct hinted_entry
{
    cc::string_view name;
    isize rolled = 0;
};
constexpr hinted_entry k_hinted_entries[] = {
    {"kept", 1}, {"inclusive", 1}, {"outer", 1}, {"inner", 1}, {"both", 2}, {"cased", 1}, {"waved", 1}, {"unrolled", 0},
};

[[nodiscard]] isize occurrences(cc::string_view text, cc::string_view part)
{
    auto count = isize(0);
    for (auto at = text.find(part); at >= 0; at = text.find(part, at + part.size()))
        ++count;
    return count;
}

/// The lines of `disassembly` that hold `part`.
[[nodiscard]] cc::vector<cc::string_view> lines_with(cc::string_view disassembly, cc::string_view part)
{
    auto found = cc::vector<cc::string_view>();
    auto start = isize(0);
    while (start < disassembly.size())
    {
        auto end = disassembly.find('\n', start);
        if (end < 0)
            end = disassembly.size();
        auto const line = disassembly.subview(cc::offset_size{.offset = start, .size = end - start});
        if (line.contains(part))
            found.push_back(line);
        start = end + 1;
    }
    return found;
}

/// The distinct `llvm.loop` nodes a disassembly states that name `llvm.loop.unroll.disable`, by their numbers.
[[nodiscard]] isize rolled_loops(cc::string_view disassembly)
{
    auto disable = cc::string();
    for (auto const line : lines_with(disassembly, "!{!\"llvm.loop.unroll.disable\"}"))
        if (line.starts_with("!"))
            disable = cc::string(line.subview(cc::offset_size{.offset = 0, .size = line.find(' ')}));
    if (disable.empty())
        return 0;
    auto count = isize(0);
    for (auto const line : lines_with(disassembly, "= distinct !{"))
        count += line.ends_with(cc::format(", {}}}", disable)) ? 1 : 0;
    return count;
}
} // namespace

TEST("slib - SGL's DXIL marks each back edge of an @unroll(false) loop as DXC's [loop] does, and the validator takes "
     "it "
     "(EMIT-291)")
{
    auto compiler = ssc::dxc::compiler::create();
    if (compiler.has_error())
        SKIP("no DXC");
    for (auto const& entry : k_hinted_entries)
    {
        auto const native = sgl::compile_to_text({.source = k_hinted,
                                                  .entry_point = entry.name,
                                                  .target = sgl::emit::target::hlsl_dx12,
                                                  .native = sgl::emit::native_route::require});
        REQUIRE(native.has_value()).dump("entry", entry.name).dump("error", native.has_error() ? native.error() : "");
        REQUIRE(!native.value().dxil.empty());
        auto const refused = compiler.value().validate(native.value().dxil);
        CHECK(!refused.has_value()).dump("entry", entry.name).dump("validator", refused.value_or(cc::string()));
        auto const ours = compiler.value().disassemble(native.value().dxil);
        REQUIRE(ours.has_value());

        // a node per loop, and every branch that names one goes back to that loop's header
        CHECK(rolled_loops(ours.value()) == entry.rolled).dump("entry", entry.name).dump("ours", ours.value());
        auto const marked = lines_with(ours.value(), ", !llvm.loop !");
        CHECK(marked.size() >= entry.rolled).dump("entry", entry.name).dump("ours", ours.value());
        for (auto const line : marked)
            CHECK((line.starts_with("  br ") || line.starts_with("  switch "))).dump("entry", entry.name).dump("line", line);
        CHECK(occurrences(ours.value(), "llvm.loop.unroll.full") == 0).dump("entry", entry.name);
        CHECK(occurrences(ours.value(), "llvm.loop.unroll.count") == 0).dump("entry", entry.name);

        // DXC's compile of SGL's text, `[loop]` on the same loops, marks as many
        auto const text = sgl::compile_to_text({.source = k_hinted,
                                                .entry_point = entry.name,
                                                .target = sgl::emit::target::hlsl_dx12,
                                                .native = sgl::emit::native_route::text});
        REQUIRE(text.has_value());
        CHECK(occurrences(text.value().text, "[loop]") == entry.rolled).dump("entry", entry.name);
        auto const built = compiler.value().compile({.source = text.value().text,
                                                     .entry_point = text.value().entry_point,
                                                     .stage = sg::shader_stage::compute,
                                                     .model = ssc::dxc::shader_model::sm_6_6});
        REQUIRE(built.has_value())
            .dump("entry", entry.name)
            .dump("dxc", built.has_error() ? built.error().to_string() : cc::string());
        auto const theirs = compiler.value().disassemble(built.value().bytecode);
        REQUIRE(theirs.has_value());
        CHECK(rolled_loops(theirs.value()) == entry.rolled)
            .dump("entry", entry.name)
            .dump("ours", ours.value())
            .dump("theirs", theirs.value());
    }
}
