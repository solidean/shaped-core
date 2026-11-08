#include "ir-test-support.hh"

#include <clean-core/string/string.hh>
#include <shaped-graphics-language/ir/passes.hh>
#include <shaped-graphics-language/ir/verify.hh>

using namespace cc::primitive_defines;
using namespace sgl_test::impl;

namespace
{
namespace check = sgl::check;

void require_whole(program const& p, control_flow flow)
{
    auto const wrong = verify(p, flow);
    CHECK(!wrong.has_value()).dump("first wrong", wrong.value_or(cc::string()));
}

[[nodiscard]] isize count_of(program const& p, op code)
{
    auto count = isize(0);
    for (auto const& i : p.instructions)
        count += i.code == code ? 1 : 0;
    return count;
}

[[nodiscard]] instruction const& only(program const& p, op code)
{
    REQUIRE(count_of(p, code) == 1);
    for (auto const& i : p.instructions)
        if (i.code == code)
            return i;
    return p.instructions[0];
}

void mark(program_builder& b, value_ref v, u32 statement, check::float_level guarantee = check::float_level::fast)
{
    b.p.instructions[index_of(v)].statement = statement;
    b.p.instructions[index_of(v)].guarantee = guarantee;
}

/// A read a pass can prove nothing about, so nothing folds it.
[[nodiscard]] value_ref unknown(program_builder& b)
{
    return b.add(op::load, b.float_type, {u32(b.out)});
}

[[nodiscard]] value_ref read_only_buffer(program_builder& b)
{
    b.p.globals.push_back({.type = b.buffer_pointer, .kind = global_kind::buffer, .is_read_only = true});
    return global_at(u32(b.p.globals.size() - 1));
}
} // namespace

// ---- remove_unused

TEST("sgl ir remove_unused - what nothing reads goes, and what has an effect keeps what it reads")
{
    auto b = program_builder();
    b.blocks(1);
    b.open(0);
    auto const x = unknown(b);
    auto const kept = b.add(op::fadd, b.float_type, {u32(x), u32(b.one)});
    mark(b, kept, 7);
    (void)b.add(op::fmul, b.float_type, {u32(x), u32(b.two)});
    b.keep(kept);
    b.finish();
    remove_unused(b.p);
    require_whole(b.p, control_flow::structured);
    CHECK(count_of(b.p, op::fmul) == 0);
    CHECK(only(b.p, op::fadd).statement == 7);
}

TEST("sgl ir remove_unused - phis that only read each other go, though each is read")
{
    auto b = program_builder();
    b.blocks(4);
    b.open(0);
    b.branch(1);
    b.open(1);
    b.loop_merge(1, 3, 2);
    auto const phi = b.add(op::phi, b.float_type, {u32(b.one), 0, 0, 2});
    auto const go_on = b.add(op::flt, b.bool_type, {u32(unknown(b)), u32(b.two)});
    b.branch_if(go_on, 2, 3);
    b.open(2);
    auto const more = b.add(op::fadd, b.float_type, {u32(phi), u32(b.one)});
    b.branch(1);
    b.open(3);
    b.finish();
    b.p.operands[b.p.instructions[index_of(phi)].first + 2] = u32(more);
    require_whole(b.p, control_flow::structured);
    remove_unused(b.p);
    require_whole(b.p, control_flow::structured);
    CHECK(count_of(b.p, op::phi) == 0);
    CHECK(count_of(b.p, op::fadd) == 0);
}

// ---- share_common_values

TEST("sgl ir share_common_values - the second of two equal operations reads the first, whose statement stays")
{
    auto b = program_builder();
    b.blocks(1);
    b.open(0);
    auto const x = unknown(b);
    auto const first = b.add(op::fmul, b.float_type, {u32(x), u32(b.two)});
    mark(b, first, 1);
    auto const second = b.add(op::fmul, b.float_type, {u32(x), u32(b.two)});
    mark(b, second, 2);
    b.keep(first);
    b.keep(second);
    b.finish();
    share_common_values(b.p);
    remove_unused(b.p);
    require_whole(b.p, control_flow::structured);
    CHECK(only(b.p, op::fmul).statement == 1);
}

TEST("sgl ir share_common_values - two guarantees are two values, so a narrower one is not lost to a wider")
{
    auto b = program_builder();
    b.blocks(1);
    b.open(0);
    auto const x = unknown(b);
    auto const fast = b.add(op::fmul, b.float_type, {u32(x), u32(b.two)});
    auto const kept = b.add(op::fmul, b.float_type, {u32(x), u32(b.two)});
    mark(b, kept, 2, check::float_level::precise);
    b.keep(fast);
    b.keep(kept);
    b.finish();
    share_common_values(b.p);
    remove_unused(b.p);
    require_whole(b.p, control_flow::structured);
    CHECK(count_of(b.p, op::fmul) == 2);
}

TEST("sgl ir share_common_values - what one arm computed is not there for the other")
{
    auto b = program_builder();
    b.blocks(4);
    b.open(0);
    b.select_merge(0, 3);
    auto const x = unknown(b);
    b.branch_if(b.add(op::flt, b.bool_type, {u32(x), u32(b.two)}), 1, 2);
    b.open(1);
    b.keep(b.add(op::fmul, b.float_type, {u32(x), u32(b.two)}));
    b.branch(3);
    b.open(2);
    b.keep(b.add(op::fmul, b.float_type, {u32(x), u32(b.two)}));
    b.branch(3);
    b.open(3);
    b.finish();
    share_common_values(b.p);
    require_whole(b.p, control_flow::structured);
    remove_unused(b.p);
    CHECK(count_of(b.p, op::fmul) == 2);
}

TEST("sgl ir share_common_values - a read shares only where the shader cannot write")
{
    auto b = program_builder();
    auto const held = read_only_buffer(b);
    b.blocks(1);
    b.open(0);
    b.keep(b.add(op::load, b.float_type, {u32(b.out)}));
    b.keep(b.add(op::load, b.float_type, {u32(b.out)}));
    b.keep(b.add(op::load, b.float_type, {u32(held)}));
    b.keep(b.add(op::load, b.float_type, {u32(held)}));
    b.finish();
    share_common_values(b.p);
    remove_unused(b.p);
    require_whole(b.p, control_flow::structured);
    CHECK(count_of(b.p, op::load) == 3);
}

namespace
{
/// A function local of four floats; locals made after the first share its types, as a program's do.
struct local_array
{
    type_ref type;
    type_ref pointer;
    value_ref local;
};
[[nodiscard]] local_array add_array(program_builder& b, local_array const* like = nullptr)
{
    auto const type
        = like != nullptr ? like->type : b.add_type({.kind = type_kind::array, .count = 4, .element = b.float_type});
    auto const pointer = like != nullptr
                           ? like->pointer
                           : b.add_type({.kind = type_kind::pointer, .space = storage::function, .element = type});
    b.p.locals.push_back({.type = pointer});
    return {.type = type, .pointer = pointer, .local = local_at(u32(b.p.locals.size() - 1))};
}
[[nodiscard]] value_ref element_of(program_builder& b, local_array const& a, value_ref at)
{
    return b.add(op::access_chain, b.float_pointer, {u32(a.local), u32(at)});
}
} // namespace

TEST("sgl ir share_common_values - two reads of a local's element after its last store are one, and with one between "
     "two")
{
    auto b = program_builder();
    auto const xs = add_array(b);
    b.blocks(1);
    b.open(0);
    auto const at = b.add(op::f_to_s, b.uint_type, {u32(unknown(b))});
    b.add(op::store, b.void_type, {u32(element_of(b, xs, at)), u32(b.one)});
    auto const before = b.add(op::load, b.float_type, {u32(element_of(b, xs, at))});
    b.add(op::store, b.void_type, {u32(element_of(b, xs, at)), u32(b.two)});
    auto const after = b.add(op::load, b.float_type, {u32(element_of(b, xs, at))});
    auto const again = b.add(op::load, b.float_type, {u32(element_of(b, xs, at))});
    b.keep(b.add(op::fadd, b.float_type, {u32(b.add(op::fadd, b.float_type, {u32(before), u32(after)})), u32(again)}));
    b.finish();
    share_common_values(b.p);
    remove_unused(b.p);
    require_whole(b.p, control_flow::structured);
    // `unknown`'s read, and the element's before and after the second store
    CHECK(count_of(b.p, op::load) == 3);
}

TEST("sgl ir share_common_values - a whole copy of a local nothing writes after is read as that local, and goes")
{
    // what inlining a function that takes an array by value leaves
    auto b = program_builder();
    auto const xs = add_array(b);
    auto const copy = add_array(b, &xs);
    b.blocks(1);
    b.open(0);
    auto const at = b.add(op::f_to_s, b.uint_type, {u32(unknown(b))});
    b.add(op::store, b.void_type, {u32(element_of(b, xs, at)), u32(b.one)});
    b.add(op::store, b.void_type, {u32(copy.local), u32(b.add(op::load, xs.type, {u32(xs.local)}))});
    auto const first = b.add(op::load, b.float_type, {u32(element_of(b, copy, at))});
    auto const second = b.add(op::load, b.float_type, {u32(element_of(b, xs, at))});
    b.keep(b.add(op::fadd, b.float_type, {u32(first), u32(second)}));
    b.finish();
    share_common_values(b.p);
    remove_unused(b.p);
    require_whole(b.p, control_flow::structured);
    // the element's store and the kept sum's, and the copy with neither its read nor its write
    CHECK(count_of(b.p, op::store) == 2);
    CHECK(count_of(b.p, op::load) == 2);
}

TEST("sgl ir share_common_values - a copy of a local written after it stays a copy")
{
    auto b = program_builder();
    auto const xs = add_array(b);
    auto const copy = add_array(b, &xs);
    b.blocks(1);
    b.open(0);
    auto const at = b.add(op::f_to_s, b.uint_type, {u32(unknown(b))});
    b.add(op::store, b.void_type, {u32(copy.local), u32(b.add(op::load, xs.type, {u32(xs.local)}))});
    b.add(op::store, b.void_type, {u32(element_of(b, xs, at)), u32(b.one)});
    b.keep(b.add(op::load, b.float_type, {u32(element_of(b, copy, at))}));
    b.finish();
    share_common_values(b.p);
    remove_unused(b.p);
    require_whole(b.p, control_flow::structured);
    CHECK(count_of(b.p, op::store) == 3);
}

// ---- hoist_common_values

TEST("sgl ir hoist_common_values - a chain both arms take alike is one, in the block that branches")
{
    // an if-else over samplers reads one texture's element in each arm, which DXIL makes a handle of where the chain is
    auto b = program_builder();
    auto const array_type = b.add_type({.kind = type_kind::array, .count = 4, .element = b.float_type});
    auto const array_pointer
        = b.add_type({.kind = type_kind::pointer, .space = storage::function, .element = array_type});
    b.p.locals.push_back({.type = array_pointer});
    auto const xs = local_at(0);
    b.blocks(4);
    b.open(0);
    b.select_merge(0, 3);
    auto const x = unknown(b);
    auto const at = b.add(op::f_to_s, b.uint_type, {u32(x)});
    b.branch_if(b.add(op::flt, b.bool_type, {u32(x), u32(b.two)}), 1, 2);
    b.open(1);
    auto const first = b.add(op::load, b.float_type, {u32(b.add(op::access_chain, b.float_pointer, {u32(xs), u32(at)}))});
    b.branch(3);
    b.open(2);
    auto const second
        = b.add(op::fadd, b.float_type,
                {u32(b.add(op::load, b.float_type, {u32(b.add(op::access_chain, b.float_pointer, {u32(xs), u32(at)}))})),
                 u32(b.one)});
    b.branch(3);
    b.open(3);
    b.keep(b.add(op::phi, b.float_type, {u32(first), 1, u32(second), 2}));
    b.finish();
    share_common_values(b.p);
    hoist_common_values(b.p);
    require_whole(b.p, control_flow::structured);
    REQUIRE(count_of(b.p, op::access_chain) == 1);
    // it stands in the block that branches, ahead of its terminator, where both arms see it
    auto const& entry = b.p.blocks[0];
    auto in_entry = false;
    for (auto i = entry.first; i < entry.first + entry.count; ++i)
        in_entry = in_entry || b.p.instructions[i].code == op::access_chain;
    CHECK(in_entry);
}

TEST("sgl ir hoist_common_values - a chain whose index an arm computes stays in that arm")
{
    auto b = program_builder();
    auto const array_type = b.add_type({.kind = type_kind::array, .count = 4, .element = b.float_type});
    auto const array_pointer
        = b.add_type({.kind = type_kind::pointer, .space = storage::function, .element = array_type});
    b.p.locals.push_back({.type = array_pointer});
    auto const xs = local_at(0);
    b.blocks(4);
    b.open(0);
    b.select_merge(0, 3);
    auto const x = unknown(b);
    b.branch_if(b.add(op::flt, b.bool_type, {u32(x), u32(b.two)}), 1, 2);
    b.open(1);
    auto const at = b.add(op::f_to_s, b.uint_type, {u32(x)});
    auto const first = b.add(op::load, b.float_type, {u32(b.add(op::access_chain, b.float_pointer, {u32(xs), u32(at)}))});
    b.branch(3);
    b.open(2);
    b.branch(3);
    b.open(3);
    b.keep(b.add(op::phi, b.float_type, {u32(first), 1, u32(b.one), 2}));
    b.finish();
    hoist_common_values(b.p);
    require_whole(b.p, control_flow::structured);
    CHECK(count_of(b.p, op::access_chain) == 1);
}

TEST("sgl ir hoist_common_values - a test of a constant both arms make alike is one, made before the branch")
{
    // an unrolled loop over texels tests one constant in each arm of each iteration, which DXC hoists out of the loop
    auto b = program_builder();
    auto const constants_pointer
        = b.add_type({.kind = type_kind::pointer, .space = storage::push_constant, .element = b.float_type});
    b.p.globals.push_back({.type = constants_pointer, .kind = global_kind::inline_constants});
    auto const constants = global_at(u32(b.p.globals.size() - 1));
    b.blocks(4);
    b.open(0);
    b.select_merge(0, 3);
    auto const x = unknown(b);
    b.branch_if(b.add(op::flt, b.bool_type, {u32(x), u32(b.two)}), 1, 2);
    auto const tested = [&]
    { return b.add(op::flt, b.bool_type, {u32(b.add(op::load, b.float_type, {u32(constants)})), u32(b.one)}); };
    b.open(1);
    auto const first = b.add(op::select, b.float_type, {u32(tested()), u32(x), u32(b.one)});
    b.branch(3);
    b.open(2);
    auto const second = b.add(op::select, b.float_type, {u32(tested()), u32(b.two), u32(x)});
    b.branch(3);
    b.open(3);
    b.keep(b.add(op::phi, b.float_type, {u32(first), 1, u32(second), 2}));
    b.finish();
    hoist_common_values(b.p);
    remove_unused(b.p);
    require_whole(b.p, control_flow::structured);
    // the read of `x` and the constant's
    CHECK(count_of(b.p, op::load) == 2);
    CHECK(count_of(b.p, op::flt) == 2);
    CHECK(count_of(b.p, op::select) == 2);
}

TEST("sgl ir hoist_common_values - a value a thread's own read feeds stays in each arm that makes it")
{
    // moving it would lengthen a varying value's life across both arms for one computation an arm saves
    auto b = program_builder();
    b.blocks(4);
    b.open(0);
    b.select_merge(0, 3);
    auto const x = unknown(b);
    b.branch_if(b.add(op::flt, b.bool_type, {u32(x), u32(b.two)}), 1, 2);
    b.open(1);
    auto const first = b.add(op::fadd, b.float_type, {u32(x), u32(b.one)});
    b.branch(3);
    b.open(2);
    auto const second = b.add(op::fmul, b.float_type, {u32(b.add(op::fadd, b.float_type, {u32(x), u32(b.one)})), u32(x)});
    b.branch(3);
    b.open(3);
    b.keep(b.add(op::phi, b.float_type, {u32(first), 1, u32(second), 2}));
    b.finish();
    hoist_common_values(b.p);
    require_whole(b.p, control_flow::structured);
    CHECK(count_of(b.p, op::fadd) == 2);
}

// ---- flatten_branches

TEST("sgl ir flatten_branches - a branch around little is a select, carrying the phi's statement and guarantee")
{
    auto b = program_builder();
    b.blocks(4);
    b.open(0);
    b.select_merge(0, 3);
    auto const x = unknown(b);
    b.branch_if(b.add(op::flt, b.bool_type, {u32(x), u32(b.two)}), 1, 2);
    b.open(1);
    auto const then_value = b.add(op::fadd, b.float_type, {u32(x), u32(b.one)});
    mark(b, then_value, 4);
    b.branch(3);
    b.open(2);
    b.branch(3);
    b.open(3);
    auto const joined = b.add(op::phi, b.float_type, {u32(then_value), 1, u32(b.two), 2});
    mark(b, joined, 9, check::float_level::precise);
    b.keep(joined);
    b.finish();
    flatten_branches(b.p);
    require_whole(b.p, control_flow::unstructured);
    CHECK(b.p.blocks.size() == 1);
    CHECK(b.p.blocks[0].merges == merge_kind::none);
    CHECK(count_of(b.p, op::phi) == 0);
    auto const& chosen = only(b.p, op::select);
    CHECK(chosen.statement == 9);
    CHECK(chosen.guarantee == check::float_level::precise);
    // what ran in the arm keeps its own
    CHECK(only(b.p, op::fadd).statement == 4);
}

TEST("sgl ir flatten_branches - neither an effect nor what can trap runs on a path that did not ask")
{
    for (auto const has_effect_in_arm : {true, false})
    {
        auto b = program_builder();
        b.blocks(3);
        b.open(0);
        b.select_merge(0, 2);
        auto const x = unknown(b);
        auto const as_int = b.add(op::f_to_u, b.uint_type, {u32(x)});
        b.branch_if(b.add(op::flt, b.bool_type, {u32(x), u32(b.two)}), 1, 2);
        b.open(1);
        auto const quotient
            = has_effect_in_arm ? value_ref::none : b.add(op::udiv, b.uint_type, {u32(b.three), u32(as_int)});
        if (has_effect_in_arm)
            b.keep(x);
        b.branch(2);
        b.open(2);
        if (!has_effect_in_arm)
            b.keep(b.add(op::u_to_f, b.float_type,
                         {u32(b.add(op::phi, b.uint_type, {u32(quotient), 1, u32(b.four), 0}))}));
        b.finish();
        flatten_branches(b.p);
        require_whole(b.p, control_flow::unstructured);
        CHECK(b.p.blocks.size() == 3);
        CHECK(count_of(b.p, op::select) == 0);
    }
}

TEST("sgl ir flatten_branches - a discard stays behind its branch, and goes on to what follows it")
{
    auto b = program_builder();
    b.blocks(3);
    b.open(0);
    b.select_merge(0, 2);
    auto const x = unknown(b);
    b.branch_if(b.add(op::flt, b.bool_type, {u32(x), u32(b.two)}), 1, 2);
    b.open(1);
    b.add(op::discard, type_ref::none, {});
    b.branch(2);
    b.open(2);
    b.keep(b.add(op::derivative, b.float_type, {u32(x)}, u8(derivative::x)));
    b.finish();
    remove_unused(b.p);
    flatten_branches(b.p);
    require_whole(b.p, control_flow::unstructured);
    CHECK(b.p.blocks.size() == 3);
    CHECK(count_of(b.p, op::discard) == 1);
    CHECK(count_of(b.p, op::derivative) == 1);
}

TEST("sgl ir flatten_branches - a block with one way in joins it, and what no walk reaches goes with its edges")
{
    auto b = program_builder();
    b.blocks(4);
    b.open(0);
    auto const x = unknown(b);
    b.branch(1);
    b.open(1);
    b.keep(x);
    b.branch(3);
    b.open(2);
    b.branch(3);
    b.open(3);
    b.keep(b.add(op::phi, b.float_type, {u32(b.one), 1, u32(b.two), 2}));
    b.finish();
    flatten_branches(b.p);
    require_whole(b.p, control_flow::unstructured);
    CHECK(b.p.blocks.size() == 1);
    CHECK(count_of(b.p, op::phi) == 0);
    CHECK(dump(b.p).contains("store @ 0x3f800000"));
}

TEST("sgl ir flatten_branches - a loop stays a loop, and its phi still names its two ways in")
{
    auto b = program_builder();
    b.blocks(4);
    b.open(0);
    b.branch(1);
    b.open(1);
    b.loop_merge(1, 3, 2);
    auto const phi = b.add(op::phi, b.float_type, {u32(b.one), 0, 0, 2});
    b.branch_if(b.add(op::flt, b.bool_type, {u32(phi), u32(b.two)}), 2, 3);
    b.open(2);
    auto const more = b.add(op::fadd, b.float_type, {u32(phi), u32(b.one)});
    b.branch(1);
    b.open(3);
    b.keep(phi);
    b.finish();
    b.p.operands[b.p.instructions[index_of(phi)].first + 2] = u32(more);
    flatten_branches(b.p);
    require_whole(b.p, control_flow::unstructured);
    CHECK(count_of(b.p, op::phi) == 1);
    CHECK(count_of(b.p, op::branch_conditional) == 1);
}

TEST("sgl ir flatten_branches structured - nested selections fold from the inside out into one block")
{
    auto b = program_builder();
    b.blocks(6);
    b.open(0);
    b.select_merge(0, 5);
    auto const x = unknown(b);
    b.branch_if(b.add(op::flt, b.bool_type, {u32(x), u32(b.two)}), 1, 5);
    b.open(1);
    b.select_merge(1, 4);
    b.branch_if(b.add(op::flt, b.bool_type, {u32(x), u32(b.one)}), 2, 3);
    b.open(2);
    auto const inner = b.add(op::fadd, b.float_type, {u32(x), u32(b.one)});
    b.branch(4);
    b.open(3);
    b.branch(4);
    b.open(4);
    auto const joined = b.add(op::phi, b.float_type, {u32(inner), 2, u32(b.two), 3});
    b.branch(5);
    b.open(5);
    b.keep(b.add(op::phi, b.float_type, {u32(joined), 4, u32(x), 0}));
    b.finish();
    require_whole(b.p, control_flow::structured);
    flatten_branches(b.p, control_flow::structured);
    require_whole(b.p, control_flow::structured);
    CHECK(b.p.blocks.size() == 1);
    CHECK(b.p.blocks[0].merges == merge_kind::none);
    CHECK(count_of(b.p, op::phi) == 0);
    CHECK(count_of(b.p, op::select) == 2);
}

TEST("sgl ir flatten_branches structured - a selection in a loop folds, and the loop keeps its header, merge and "
     "continue target")
{
    auto b = program_builder();
    b.blocks(7);
    b.open(0);
    b.branch(1);
    b.open(1);
    b.loop_merge(1, 6, 5);
    auto const counter = b.add(op::phi, b.float_type, {u32(b.one), 0, 0, 5});
    b.branch_if(b.add(op::flt, b.bool_type, {u32(counter), u32(b.two)}), 2, 6);
    b.open(2);
    b.select_merge(2, 5);
    b.branch_if(b.add(op::flt, b.bool_type, {u32(counter), u32(b.one)}), 3, 4);
    b.open(3);
    auto const step = b.add(op::fadd, b.float_type, {u32(counter), u32(b.one)});
    b.branch(5);
    b.open(4);
    b.branch(5);
    b.open(5);
    // the continue target, which is the selection's merge too
    auto const taken = b.add(op::phi, b.float_type, {u32(step), 3, u32(b.two), 4});
    auto const more = b.add(op::fadd, b.float_type, {u32(counter), u32(taken)});
    b.branch(1);
    b.open(6);
    b.keep(counter);
    b.finish();
    b.p.operands[b.p.instructions[index_of(counter)].first + 2] = u32(more);
    require_whole(b.p, control_flow::structured);
    flatten_branches(b.p, control_flow::structured);
    require_whole(b.p, control_flow::structured);
    CHECK(count_of(b.p, op::select) == 1);
    CHECK(count_of(b.p, op::phi) == 1);
    auto loops = 0;
    for (auto const& block : b.p.blocks)
    {
        CHECK(block.merges != merge_kind::selection);
        loops += block.merges == merge_kind::loop ? 1 : 0;
    }
    CHECK(loops == 1);
}

TEST("sgl ir flatten_branches structured - a phi of a pointer keeps its branch, which logical addressing cannot select")
{
    auto b = program_builder();
    auto const held = read_only_buffer(b);
    b.blocks(4);
    b.open(0);
    b.select_merge(0, 3);
    auto const x = unknown(b);
    b.branch_if(b.add(op::flt, b.bool_type, {u32(x), u32(b.two)}), 1, 2);
    b.open(1);
    b.branch(3);
    b.open(2);
    b.branch(3);
    b.open(3);
    auto const chosen = b.add(op::phi, b.buffer_pointer, {u32(b.out), 1, u32(held), 2});
    b.keep(b.add(op::load, b.float_type, {u32(chosen)}));
    b.finish();
    flatten_branches(b.p, control_flow::structured);
    require_whole(b.p, control_flow::structured);
    CHECK(count_of(b.p, op::select) == 0);
    CHECK(count_of(b.p, op::phi) == 1);
    CHECK(b.p.blocks[0].merges == merge_kind::selection);
}

// ---- regroup_lanes

TEST("sgl ir regroup_lanes - a vector crossing nested merges is one phi at each, the inner one found through the outer")
{
    // only the outer merge's phis reach a `construct`; the inner merge's are read by the outer's alone
    auto b = program_builder();
    b.blocks(6);
    b.open(0);
    b.select_merge(0, 5);
    auto const x = unknown(b);
    auto const whole = b.add(op::construct, b.float2_type, {u32(x), u32(unknown(b))});
    auto const lane0 = b.add(op::extract, b.float_type, {u32(whole), 0});
    auto const lane1 = b.add(op::extract, b.float_type, {u32(whole), 1});
    b.branch_if(b.add(op::flt, b.bool_type, {u32(x), u32(b.two)}), 1, 4);
    b.open(1);
    b.select_merge(1, 3);
    b.branch_if(b.add(op::flt, b.bool_type, {u32(x), u32(b.one)}), 2, 3);
    b.open(2);
    auto const moved = b.add(op::fadd, b.float_type, {u32(lane0), u32(b.one)});
    b.branch(3);
    b.open(3);
    auto const inner0 = b.add(op::phi, b.float_type, {u32(moved), 2, u32(lane0), 1});
    auto const inner1 = b.add(op::phi, b.float_type, {u32(lane1), 2, u32(lane1), 1});
    b.branch(5);
    b.open(4);
    b.branch(5);
    b.open(5);
    auto const outer0 = b.add(op::phi, b.float_type, {u32(inner0), 3, u32(lane0), 4});
    auto const outer1 = b.add(op::phi, b.float_type, {u32(inner1), 3, u32(lane1), 4});
    auto const rebuilt = b.add(op::construct, b.float2_type, {u32(outer0), u32(outer1)});
    b.keep(b.add(op::extract, b.float_type, {u32(rebuilt), 1}));
    b.finish();
    require_whole(b.p, control_flow::structured);
    regroup_lanes(b.p);
    remove_unused(b.p);
    require_whole(b.p, control_flow::structured);
    auto vector_phis = 0;
    auto lane_phis = 0;
    for (auto const& i : b.p.instructions)
        if (i.code == op::phi)
            (i.type == b.float2_type ? vector_phis : lane_phis) += 1;
    CHECK(vector_phis == 2);
    CHECK(lane_phis == 0);
}

// ---- chain_sums

TEST("sgl ir chain_sums - a total meets each product alone, and each addition keeps its statement")
{
    auto b = program_builder();
    b.blocks(1);
    b.open(0);
    auto const x = unknown(b);
    auto const p0 = b.add(op::fmul, b.float_type, {u32(x), u32(b.one)});
    auto const p1 = b.add(op::fmul, b.float_type, {u32(x), u32(b.two)});
    auto const p2 = b.add(op::fmul, b.float_type, {u32(p0), u32(b.two)});
    auto const inner = b.add(op::fadd, b.float_type, {u32(p1), u32(p2)});
    mark(b, inner, 1);
    auto const outer = b.add(op::fadd, b.float_type, {u32(p0), u32(inner)});
    mark(b, outer, 2);
    b.keep(outer);
    b.finish();
    chain_sums(b.p);
    require_whole(b.p, control_flow::structured);
    auto const sum_of = [&](value_ref v) { return b.p.operands_of(b.p.instructions[index_of(v)]); };
    // earliest leaf first: the inner sum adds the first two products, the outer one the last
    CHECK(sum_of(inner)[0] == u32(p0));
    CHECK(sum_of(inner)[1] == u32(p1));
    CHECK(sum_of(outer)[0] == u32(inner));
    CHECK(sum_of(outer)[1] == u32(p2));
    CHECK(b.p.instructions[index_of(inner)].statement == 1);
    CHECK(b.p.instructions[index_of(outer)].statement == 2);
}

TEST("sgl ir chain_sums - a guarantee above fast holds a sum where it stands")
{
    auto b = program_builder();
    b.blocks(1);
    b.open(0);
    auto const x = unknown(b);
    auto const y = unknown(b);
    auto const inner = b.add(op::fadd, b.float_type, {u32(x), u32(y)});
    auto const outer = b.add(op::fadd, b.float_type, {u32(b.one), u32(inner)});
    mark(b, inner, 1, check::float_level::precise);
    mark(b, outer, 2, check::float_level::precise);
    b.keep(outer);
    b.finish();
    auto const before = dump(b.p);
    chain_sums(b.p);
    CHECK(dump(b.p) == before);
}

TEST("sgl ir chain_sums - a fast sum does not reach through a precise one")
{
    auto b = program_builder();
    b.blocks(1);
    b.open(0);
    auto const x = unknown(b);
    auto const y = unknown(b);
    auto const inner = b.add(op::fadd, b.float_type, {u32(x), u32(y)});
    mark(b, inner, 1, check::float_level::precise);
    auto const outer = b.add(op::fadd, b.float_type, {u32(b.one), u32(inner)});
    b.keep(outer);
    b.finish();
    auto const before = dump(b.p);
    chain_sums(b.p);
    CHECK(dump(b.p) == before);
}

TEST("sgl ir chain_sums - a sum read twice stays a value of its own")
{
    auto b = program_builder();
    b.blocks(1);
    b.open(0);
    auto const x = unknown(b);
    auto const y = unknown(b);
    auto const inner = b.add(op::fadd, b.float_type, {u32(x), u32(y)});
    auto const outer = b.add(op::fadd, b.float_type, {u32(b.one), u32(inner)});
    b.keep(outer);
    b.keep(inner);
    b.finish();
    auto const before = dump(b.p);
    chain_sums(b.p);
    CHECK(dump(b.p) == before);
}

TEST("sgl ir passes - a stage load is a pure value, and a stage store runs whether or not anything reads")
{
    auto b = program_builder();
    b.p.stage_elements.push_back({.part = stage_part::input, .type = b.float_type});
    b.p.stage_elements.push_back({.part = stage_part::output, .type = b.float_type});
    b.blocks(1);
    b.open(0);
    b.add(op::stage_load, b.float_type, {0});
    auto const first = b.add(op::stage_load, b.float_type, {0});
    auto const second = b.add(op::stage_load, b.float_type, {0});
    b.add(op::stage_store, b.void_type, {1, u32(first)});
    b.add(op::stage_store, b.void_type, {1, u32(second)});
    b.finish();

    share_common_values(b.p);
    remove_unused(b.p);
    require_whole(b.p, control_flow::unstructured);
    CHECK(count_of(b.p, op::stage_load) == 1);
    CHECK(count_of(b.p, op::stage_store) == 2);
}
