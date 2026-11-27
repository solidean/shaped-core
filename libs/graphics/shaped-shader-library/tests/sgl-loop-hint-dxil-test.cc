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

namespace
{
/// More shapes a hinted loop may take: nested in the inclusive range's own loop, walked, bounded, and left at once.
constexpr cc::string_view k_shapes = "binding io:\n"
                                     "    source: buffer[float]\n"
                                     "    target: mut buffer[float]\n"
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
                                     "@compute(64) fun rolled_around_rolled(@thread_id id: int3){io}:\n"
                                     "    let mut total = 0.0\n"
                                     "    @unroll(false)\n"
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
                                     "@compute(64) fun left_then_plain(@thread_id id: int3){io}:\n"
                                     "    let mut total = 0.0\n"
                                     "    @unroll(false)\n"
                                     "    for i in 0 ..< id.x:\n"
                                     "        total += io.source[i]\n"
                                     "        break\n"
                                     "    for k in 0 ..= id.y:\n"
                                     "        total += io.source[k] * 2.0\n"
                                     "    io.target[id.x] = total\n"
                                     "\n"
                                     "@compute(64) fun left_then_rolled(@thread_id id: int3){io}:\n"
                                     "    let mut total = 0.0\n"
                                     "    @unroll(false)\n"
                                     "    for i in 0 ..< id.x:\n"
                                     "        total += io.source[i]\n"
                                     "        break\n"
                                     "    @unroll(false)\n"
                                     "    for k in 0 ..= id.y:\n"
                                     "        total += io.source[k] * 2.0\n"
                                     "    io.target[id.x] = total\n"
                                     "\n"
                                     "@compute(64) fun plain_around_left(@thread_id id: int3){io}:\n"
                                     "    let mut total = 0.0\n"
                                     "    for k in 0 ..= id.y:\n"
                                     "        @unroll(false)\n"
                                     "        for i in 0 ..< id.x:\n"
                                     "            total += io.source[i]\n"
                                     "            break\n"
                                     "        total += io.source[k] * 2.0\n"
                                     "    io.target[id.x] = total\n";

/// Each entry point of `k_shapes`, and how many loops of its DXIL keep the hint; a loop left at once is no loop.
constexpr hinted_entry k_shape_entries[] = {
    {"rolled_around_plain", 1}, {"plain_around_rolled", 1}, {"rolled_around_rolled", 2}, {"walked", 1}, {"bounded", 1},
    {"left_then_plain", 0},     {"left_then_rolled", 1},    {"plain_around_left", 0},
};

/// The number a disassembly writes from `at` on, as the `12` of `%12`.
[[nodiscard]] isize number_at(cc::string_view text, isize at)
{
    auto n = isize(0);
    while (at < text.size() && text[at] >= '0' && text[at] <= '9')
        n = n * 10 + (text[at++] - '0');
    return n;
}

/// A terminator of SGL's disassembled function: the block it ends, the blocks it goes to, and its `!llvm.loop` node.
struct terminator_line
{
    isize block = 0;
    cc::vector<isize> targets;
    isize loop = -1;
};

/// The `br` and `switch` terminators of the one function in `disassembly`, and its blocks in the order it lays them out.
/// SGL lays its blocks out each after its dominators, so an edge to a block at or before its own is a back edge, and
/// the block it goes to the loop's header.
[[nodiscard]] cc::vector<terminator_line> terminators_of(cc::string_view disassembly, cc::vector<isize>& laid_out)
{
    auto found = cc::vector<terminator_line>();
    auto block = isize(0);
    auto in_function = false;
    auto pending = cc::optional<terminator_line>();
    auto const note = [&](cc::string_view line, terminator_line& t)
    {
        for (auto at = line.find("label %"); at >= 0; at = line.find("label %", at + 1))
            t.targets.push_back(number_at(line, at + 7));
        if (auto const at = line.find("!llvm.loop !"); at >= 0)
            t.loop = number_at(line, at + 12);
    };
    for (auto const line : lines_with(disassembly, ""))
    {
        if (line.starts_with("define "))
        {
            in_function = true;
            laid_out.push_back(0);
            continue;
        }
        if (!in_function)
            continue;
        if (line.starts_with("}"))
            break;
        if (line.starts_with("; <label>:"))
        {
            block = number_at(line, 10);
            laid_out.push_back(block);
            continue;
        }
        // a switch's cases stand on lines of their own, its node after the closing bracket
        if (pending.has_value())
        {
            note(line, pending.value());
            if (line.contains("]"))
            {
                found.push_back(pending.value());
                pending = {};
            }
            continue;
        }
        if (line.starts_with("  br "))
        {
            auto t = terminator_line{.block = block};
            note(line, t);
            found.push_back(cc::move(t));
        }
        else if (line.starts_with("  switch "))
        {
            pending = terminator_line{.block = block};
            note(line, pending.value());
        }
    }
    return found;
}

[[nodiscard]] isize place_in(cc::span<isize const> laid_out, isize block)
{
    for (auto k = isize(0); k < laid_out.size(); ++k)
        if (laid_out[k] == block)
            return k;
    return -1;
}

/// What LLVM's loop analysis reads of a loop's hint: every latch of the loop names the loop's one node, or none names
/// any; a branch that goes back to no header names none; and `rolled` loops name one, each its own.
void check_back_edges(cc::string_view name, cc::string_view disassembly, isize rolled)
{
    auto laid_out = cc::vector<isize>();
    auto const lines = terminators_of(disassembly, laid_out);
    auto headers = cc::vector<isize>();
    auto nodes = cc::vector<isize>();
    for (auto const& t : lines)
    {
        auto is_back = false;
        for (auto const to : t.targets)
        {
            if (place_in(laid_out, to) > place_in(laid_out, t.block))
                continue;
            is_back = true;
            auto known = isize(-1);
            for (auto k = isize(0); k < headers.size(); ++k)
                if (headers[k] == to)
                    known = k;
            if (known < 0)
            {
                headers.push_back(to);
                nodes.push_back(t.loop);
                continue;
            }
            // a latch that names another node than the loop's other latches, or none where they name one, drops it
            CHECK(nodes[known] == t.loop).dump("entry", name).dump("header", to).dump("ours", disassembly);
        }
        if (!is_back)
            CHECK(t.loop == -1).dump("entry", name).dump("block", t.block).dump("ours", disassembly);
    }
    auto marked = isize(0);
    for (auto k = isize(0); k < nodes.size(); ++k)
    {
        marked += nodes[k] >= 0 ? 1 : 0;
        for (auto j = k + 1; j < nodes.size(); ++j)
            CHECK((nodes[k] < 0 || nodes[k] != nodes[j])).dump("entry", name).dump("ours", disassembly);
    }
    CHECK(marked == rolled).dump("entry", name).dump("ours", disassembly);
}
} // namespace

TEST("slib - every back edge of an @unroll(false) loop names its loop's node, and no other branch names one (EMIT-291)")
{
    auto compiler = ssc::dxc::compiler::create();
    if (compiler.has_error())
        SKIP("no DXC");
    // with the debug info too, whose module DXC disassembles in the program's place and numbers its metadata anew
    auto const run = [&](cc::string_view source, cc::span<hinted_entry const> entries)
    {
        for (auto const level : {sgl::emit::debug_info::none, sgl::emit::debug_info::full})
            for (auto const& entry : entries)
            {
                auto const native = sgl::compile_to_text({.source = source,
                                                          .entry_point = entry.name,
                                                          .target = sgl::emit::target::hlsl_dx12,
                                                          .native = sgl::emit::native_route::require,
                                                          .native_debug_info = level});
                REQUIRE(native.has_value()).dump("entry", entry.name).dump("error", native.has_error() ? native.error() : "");
                auto const refused = compiler.value().validate(native.value().dxil);
                CHECK(!refused.has_value()).dump("entry", entry.name).dump("validator", refused.value_or(cc::string()));
                auto const ours = compiler.value().disassemble(native.value().dxil);
                REQUIRE(ours.has_value());
                CHECK(rolled_loops(ours.value()) == entry.rolled).dump("entry", entry.name).dump("ours", ours.value());
                check_back_edges(entry.name, ours.value(), entry.rolled);
            }
    };
    run(k_hinted, k_hinted_entries);
    run(k_shapes, k_shape_entries);
}
