#include "ir-test-support.hh"

#include <clean-core/string/string.hh>
#include <shaped-graphics-language/ir/verify.hh>

using namespace cc::primitive_defines;
using namespace sgl_test::impl;

// Each refusal is one mistake a pass could make, in a program otherwise whole: a verifier that passed everything would
// look exactly like a compiler with no bugs.

namespace
{
[[nodiscard]] cc::string refusal_of(program const& p, control_flow flow = control_flow::structured)
{
    auto const wrong = verify(p, flow);
    return wrong.has_value() ? wrong.value() : cc::string();
}

/// b0 adds, and returns.
[[nodiscard]] program_builder one_block()
{
    auto b = program_builder();
    b.blocks(1);
    b.open(0);
    return b;
}
} // namespace

// EMIT-246: SPIR-V forbids moving a hit object, so a pass that moves one has to stop here rather than at a driver.
TEST("sgl ir verify - a hit object stays the value of its trace")
{
    auto const hit_of = [](program_builder& b) { return b.add_type({.kind = type_kind::hit_object}); };

    auto read = one_block();
    auto const read_type = hit_of(read);
    auto const held = read.add(op::hit_object_trace, read_type, {});
    read.add(op::hit_object_read, read.float_type, {u32(held)}, u8(hit_object_value::t));
    read.finish();
    CHECK(refusal_of(read.p) == "");

    auto selected = one_block();
    auto const selected_type = hit_of(selected);
    auto const a = selected.add(op::hit_object_trace, selected_type, {});
    auto const b = selected.add(op::hit_object_trace, selected_type, {});
    selected.add(op::select, selected_type, {u32(selected.yes), u32(a), u32(b)});
    selected.finish();
    CHECK(refusal_of(selected.p).contains("moves a hit object"));

    auto merged = program_builder();
    auto const merged_type = hit_of(merged);
    merged.blocks(4);
    merged.open(0);
    merged.select_merge(0, 3);
    merged.branch_if(merged.yes, 1, 2);
    merged.open(1);
    auto const left = merged.add(op::hit_object_trace, merged_type, {});
    merged.branch(3);
    merged.open(2);
    auto const right = merged.add(op::hit_object_trace, merged_type, {});
    merged.branch(3);
    merged.open(3);
    merged.add(op::phi, merged_type, {u32(left), 1, u32(right), 2});
    merged.finish();
    CHECK(refusal_of(merged.p).contains("moves a hit object"));

    auto stored = one_block();
    auto const stored_type = hit_of(stored);
    auto const traced = stored.add(op::hit_object_trace, stored_type, {});
    stored.add(op::store, stored.void_type, {u32(stored.out), u32(traced)});
    stored.finish();
    CHECK(refusal_of(stored.p).contains("moves a hit object"));

    auto local = one_block();
    auto const local_type = hit_of(local);
    local.p.locals.push_back({.type = local.add_type({.kind = type_kind::pointer, .element = local_type})});
    local.finish();
    CHECK(refusal_of(local.p).contains("a local that holds a hit object"));

    auto member = one_block();
    auto const member_type = hit_of(member);
    member.p.members.push_back({.type = member_type});
    member.finish();
    CHECK(refusal_of(member.p).contains("a member that holds a hit object"));
}

TEST("sgl ir verify - a whole program passes in either form")
{
    auto b = program_builder();
    b.diamond(b.yes, b.one, b.two);
    CHECK(refusal_of(b.p, control_flow::structured) == "");
    CHECK(refusal_of(b.p, control_flow::unstructured) == "");
}

TEST("sgl ir verify - a program with no block is refused")
{
    CHECK(refusal_of(program()) != "");
}

TEST("sgl ir verify - a block must end in its one terminator")
{
    auto missing = one_block();
    missing.keep(missing.one);
    CHECK(refusal_of(missing.p).contains("no terminator"));

    auto early = one_block();
    early.finish();
    early.keep(early.one);
    early.finish();
    CHECK(refusal_of(early.p).contains("a terminator before"));
}

TEST("sgl ir verify - a value out of its table is refused, whichever table")
{
    auto instruction = one_block();
    instruction.keep(value_ref(99));
    instruction.finish();
    CHECK(refusal_of(instruction.p).contains("past its table"));

    auto constant = one_block();
    constant.keep(constant_at(99));
    constant.finish();
    CHECK(refusal_of(constant.p).contains("past its table"));

    auto none = one_block();
    none.keep(value_ref::none);
    none.finish();
    CHECK(refusal_of(none.p).contains("no value"));
}

TEST("sgl ir verify - an operand must give a value")
{
    auto b = one_block();
    b.keep(b.one);
    b.keep(value_ref(0));
    b.finish();
    CHECK(refusal_of(b.p).contains("gives no value"));
}

TEST("sgl ir verify - a branch must reach a block that holds something, and never the entry")
{
    auto past = one_block();
    past.branch(7);
    CHECK(refusal_of(past.p).contains("past the blocks"));

    auto empty = program_builder();
    empty.blocks(2);
    empty.open(0);
    empty.branch(1);
    CHECK(refusal_of(empty.p).contains("holds nothing"));

    auto to_entry = one_block();
    to_entry.branch(0);
    CHECK(refusal_of(to_entry.p).contains("back to the entry"));
}

TEST("sgl ir verify - a use must come after its definition in a block")
{
    auto b = one_block();
    auto const later = value_ref(1);
    b.keep(later);
    b.add(op::fadd, b.float_type, {u32(b.one), u32(b.two)});
    b.finish();
    CHECK(refusal_of(b.p).contains("does not dominate"));
}

TEST("sgl ir verify - a definition in one arm does not reach the join")
{
    auto b = program_builder();
    b.blocks(4);
    b.open(0);
    b.select_merge(0, 3);
    b.branch_if(b.yes, 1, 2);
    b.open(1);
    auto const only_here = b.add(op::fadd, b.float_type, {u32(b.one), u32(b.two)});
    b.branch(3);
    b.open(2);
    b.branch(3);
    b.open(3);
    b.keep(only_here);
    b.finish();
    CHECK(refusal_of(b.p).contains("does not dominate"));
}

TEST("sgl ir verify - what no walk reaches is not asked to dominate")
{
    auto b = program_builder();
    b.blocks(2);
    b.open(0);
    b.finish();
    b.open(1);
    b.keep(value_ref(2));
    b.add(op::fadd, b.float_type, {u32(b.one), u32(b.two)});
    b.finish();
    CHECK(refusal_of(b.p) == "");
}

TEST("sgl ir verify - a phi answers for each predecessor once")
{
    auto missing = program_builder();
    missing.diamond(missing.yes, missing.one, missing.two);
    missing.p.instructions[missing.p.blocks[3].first].count = 2;
    CHECK(refusal_of(missing.p).contains("predecessors"));

    auto stranger = program_builder();
    stranger.diamond(stranger.yes, stranger.one, stranger.two);
    stranger.p.operands[stranger.p.instructions[stranger.p.blocks[3].first].first + 3] = 0;
    CHECK(refusal_of(stranger.p).contains("does not branch here"));

    auto twice = program_builder();
    twice.diamond(twice.yes, twice.one, twice.two);
    twice.p.operands[twice.p.instructions[twice.p.blocks[3].first].first + 3] = 1;
    CHECK(refusal_of(twice.p).contains("twice"));

    auto typed = program_builder();
    typed.diamond(typed.yes, typed.one, typed.three);
    CHECK(refusal_of(typed.p).contains("takes"));
}

TEST("sgl ir verify - a phi stands first in its block")
{
    auto b = program_builder();
    b.blocks(4);
    b.open(0);
    b.select_merge(0, 3);
    b.branch_if(b.yes, 1, 2);
    b.open(1);
    b.branch(3);
    b.open(2);
    b.branch(3);
    b.open(3);
    b.keep(b.one);
    b.add(op::phi, b.float_type, {u32(b.one), 1, u32(b.two), 2});
    b.finish();
    CHECK(refusal_of(b.p).contains("after a block's first"));
}

TEST("sgl ir verify - a phi's operand is used at the end of its edge")
{
    // a value made in b1 reaches a phi of b3 along b1's edge, and not along b2's
    auto b = program_builder();
    b.blocks(4);
    b.open(0);
    b.select_merge(0, 3);
    b.branch_if(b.yes, 1, 2);
    b.open(1);
    auto const in_one = b.add(op::fadd, b.float_type, {u32(b.one), u32(b.two)});
    b.branch(3);
    b.open(2);
    b.branch(3);
    b.open(3);
    b.keep(b.add(op::phi, b.float_type, {u32(in_one), 1, u32(in_one), 2}));
    b.finish();
    CHECK(refusal_of(b.p).contains("the edge from b2"));
}

TEST("sgl ir verify - an operation takes what its family takes")
{
    auto const refused = [](auto&& write)
    {
        auto b = one_block();
        write(b);
        b.finish();
        return refusal_of(b.p);
    };
    CHECK(refused([](program_builder& b) { b.add(op::fadd, b.float_type, {u32(b.one), u32(b.three)}); }) != "");
    CHECK(refused([](program_builder& b) { b.add(op::fadd, b.uint_type, {u32(b.three), u32(b.four)}); }) != "");
    CHECK(refused([](program_builder& b) { b.add(op::iadd, b.uint_type, {u32(b.one), u32(b.two)}); }) != "");
    CHECK(refused([](program_builder& b) { b.add(op::flt, b.float_type, {u32(b.one), u32(b.two)}); }) != "");
    CHECK(refused([](program_builder& b) { b.add(op::flt, b.bool_type, {u32(b.one)}); }) != "");
    CHECK(refused([](program_builder& b) { b.add(op::ieq, b.bool_type, {u32(b.one), u32(b.two)}); }) != "");
    CHECK(refused([](program_builder& b) { b.add(op::logical_not, b.bool_type, {u32(b.one)}); }) != "");
    CHECK(refused([](program_builder& b) { b.add(op::select, b.float_type, {u32(b.one), u32(b.one), u32(b.two)}); })
          != "");
    CHECK(refused([](program_builder& b) { b.add(op::f_to_s, b.uint_type, {u32(b.three)}); }) != "");
    CHECK(refused([](program_builder& b) { b.add(op::construct, b.float2_type, {u32(b.one)}); }) != "");
    CHECK(refused([](program_builder& b) { b.add(op::construct, b.float2_type, {u32(b.one), u32(b.three)}); }) != "");
    CHECK(refused([](program_builder& b) { b.add(op::load, b.float_type, {u32(b.one)}); }) != "");
    CHECK(refused([](program_builder& b) { b.add(op::load, b.uint_type, {u32(b.out)}); }) != "");
    CHECK(refused([](program_builder& b) { b.add(op::store, b.void_type, {u32(b.out), u32(b.three)}); }) != "");
    CHECK(refused([](program_builder& b) { b.add(op::store, b.float_type, {u32(b.out), u32(b.one)}); }) != "");
    CHECK(refused([](program_builder& b) { b.add(op::fadd, b.void_type, {u32(b.one), u32(b.two)}); }) != "");
    CHECK(refused([](program_builder& b) { b.add(op::access_chain, b.float_pointer, {u32(b.out)}); }) != "");
    CHECK(refused([](program_builder& b) { b.add(op::access_chain, b.buffer_pointer, {u32(b.out), u32(b.three)}); })
          != "");

    auto const v = [](program_builder& b) { return b.add(op::construct, b.float2_type, {u32(b.one), u32(b.two)}); };
    CHECK(refused([&](program_builder& b) { b.add(op::extract, b.uint_type, {u32(v(b)), 0}); }) != "");
    CHECK(refused([&](program_builder& b) { b.add(op::extract, b.float_type, {u32(v(b)), 2}); }) != "");
    CHECK(refused([&](program_builder& b) { b.add(op::insert, b.float2_type, {u32(b.three), u32(v(b)), 0}); }) != "");
    CHECK(refused(
              [&](program_builder& b)
              {
                  auto const x = v(b);
                  b.add(op::shuffle, b.float2_type, {u32(x), u32(x), 0, 4});
              })
          != "");
    CHECK(refused(
              [&](program_builder& b)
              {
                  auto const x = v(b);
                  b.add(op::shuffle, b.float2_type, {u32(x), u32(x), 0});
              })
          != "");
}

TEST("sgl ir verify - a well-typed operation of each family passes")
{
    auto b = one_block();
    auto const sum = b.add(op::fadd, b.float_type, {u32(b.one), u32(b.two)});
    auto const less = b.add(op::flt, b.bool_type, {u32(sum), u32(b.two)});
    auto const picked = b.add(op::select, b.float_type, {u32(less), u32(sum), u32(b.one)});
    auto const v = b.add(op::construct, b.float2_type, {u32(picked), u32(b.two)});
    auto const swapped = b.add(op::shuffle, b.float2_type, {u32(v), u32(v), 1, 2});
    auto const part = b.add(op::extract, b.float_type, {u32(swapped), 0});
    auto const inserted = b.add(op::insert, b.float2_type, {u32(part), u32(v), 1});
    auto const back = b.add(op::dot, b.float_type, {u32(inserted), u32(v)});
    b.keep(back);
    auto const read = b.add(op::load, b.float_type, {u32(b.out)});
    b.keep(read);
    b.finish();
    CHECK(refusal_of(b.p) == "");
}

TEST("sgl ir verify - only a reducible graph passes, in either form")
{
    // a cycle of b1 and b2, entered at both
    auto b = program_builder();
    b.blocks(3);
    b.open(0);
    b.branch_if(b.yes, 1, 2);
    b.open(1);
    b.branch(2);
    b.open(2);
    b.branch(1);
    CHECK(refusal_of(b.p, control_flow::unstructured).contains("does not dominate"));
    CHECK(refusal_of(b.p, control_flow::structured).contains("does not dominate"));
}

TEST("sgl ir verify - a back edge needs a declared loop only while the form is structured")
{
    auto b = program_builder();
    b.blocks(3);
    b.open(0);
    b.branch(1);
    b.open(1);
    b.branch_if(b.yes, 1, 2);
    b.open(2);
    b.finish();
    CHECK(refusal_of(b.p, control_flow::structured).contains("heads no loop"));
    CHECK(refusal_of(b.p, control_flow::unstructured) == "");
}

TEST("sgl ir verify - what a header states must stand while the form is structured")
{
    auto selects_nothing = program_builder();
    selects_nothing.blocks(2);
    selects_nothing.open(0);
    selects_nothing.select_merge(0, 1);
    selects_nothing.branch(1);
    selects_nothing.open(1);
    selects_nothing.finish();
    CHECK(refusal_of(selects_nothing.p, control_flow::structured).contains("does not select"));
    CHECK(refusal_of(selects_nothing.p, control_flow::unstructured) == "");

    auto dropped = program_builder();
    dropped.diamond(dropped.yes, dropped.one, dropped.two);
    dropped.p.blocks[0].merge = block_ref::none;
    CHECK(refusal_of(dropped.p, control_flow::structured).contains("merge block"));
    CHECK(refusal_of(dropped.p, control_flow::unstructured) == "");

    auto shared = program_builder();
    shared.blocks(6);
    shared.open(0);
    shared.select_merge(0, 5);
    shared.branch_if(shared.yes, 1, 2);
    shared.open(1);
    shared.select_merge(1, 5);
    shared.branch_if(shared.yes, 3, 4);
    shared.open(2);
    shared.branch(5);
    shared.open(3);
    shared.branch(5);
    shared.open(4);
    shared.branch(5);
    shared.open(5);
    shared.finish();
    CHECK(refusal_of(shared.p, control_flow::structured).contains("merges both"));
}

TEST("sgl ir verify - a merge the header does not dominate is refused")
{
    auto b = program_builder();
    b.blocks(4);
    b.open(0);
    b.branch_if(b.yes, 1, 2);
    b.open(1);
    b.select_merge(1, 3);
    b.branch_if(b.yes, 3, 3);
    b.open(2);
    b.branch(3);
    b.open(3);
    b.finish();
    CHECK(refusal_of(b.p, control_flow::structured).contains("does not dominate its merge"));
}

TEST("sgl ir verify - blocks must open after what dominates them")
{
    auto b = program_builder();
    b.blocks(3);
    b.open(0);
    b.branch(2);
    b.open(1);
    b.finish();
    b.open(2);
    b.branch(1);
    CHECK(refusal_of(b.p).contains("which dominates it"));
}

TEST("sgl ir verify - a stage operation names an element and carries its type")
{
    auto whole = one_block();
    whole.p.stage_elements.push_back({.part = stage_part::input, .type = whole.float_type});
    whole.p.stage_elements.push_back({.part = stage_part::output, .type = whole.float_type});
    auto const read = whole.add(op::stage_load, whole.float_type, {0});
    whole.add(op::stage_store, whole.void_type, {1, u32(read)});
    whole.finish();
    CHECK(refusal_of(whole.p) == "");

    auto past = one_block();
    past.add(op::stage_load, past.float_type, {0});
    past.finish();
    CHECK(refusal_of(past.p).contains("no stage element"));

    auto retyped = one_block();
    retyped.p.stage_elements.push_back({.type = retyped.float_type});
    retyped.add(op::stage_load, retyped.uint_type, {0});
    retyped.finish();
    CHECK(refusal_of(retyped.p).contains("of an element of"));

    auto unindexed = one_block();
    unindexed.p.stage_elements.push_back({.type = unindexed.float_type, .array_size = 3});
    unindexed.add(op::stage_load, unindexed.float_type, {0});
    unindexed.finish();
    CHECK(refusal_of(unindexed.p) != "");

    auto arrayed = one_block();
    arrayed.p.types.push_back({.kind = type_kind::array, .count = 2, .element = arrayed.float_type});
    arrayed.p.stage_elements.push_back({.type = type_ref(arrayed.p.types.size() - 1)});
    arrayed.finish();
    CHECK(refusal_of(arrayed.p).contains("only as scalars and vectors"));
}
