#include "ir-test-support.hh"

#include <clean-core/string/string.hh>
#include <shaped-graphics-language/ir/dominance.hh>
#include <shaped-graphics-language/ir/simplify.hh>
#include <shaped-graphics-language/ir/ssa.hh>
#include <shaped-graphics-language/ir/verify.hh>

using namespace cc::primitive_defines;
using namespace sgl_test::impl;

namespace
{
[[nodiscard]] cc::string refusal_of(program const& p, control_flow flow)
{
    auto const wrong = verify(p, flow);
    return wrong.has_value() ? wrong.value() : cc::string();
}

[[nodiscard]] isize count_of(program const& p, op code)
{
    auto count = isize(0);
    for (auto const& i : p.instructions)
        count += i.code == code ? 1 : 0;
    return count;
}

/// A local written in both arms of a diamond and read at the join.
[[nodiscard]] program_builder written_in_both_arms(value_ref(program_builder::* condition))
{
    auto b = program_builder();
    auto const x = b.add_local();
    b.blocks(4);
    b.open(0);
    b.select_merge(0, 3);
    b.branch_if(b.*condition, 1, 2);
    b.open(1);
    b.add(op::store, b.void_type, {u32(x), u32(b.one)});
    b.branch(3);
    b.open(2);
    b.add(op::store, b.void_type, {u32(x), u32(b.two)});
    b.branch(3);
    b.open(3);
    b.keep(b.add(op::load, b.float_type, {u32(x)}));
    b.finish();
    return b;
}
} // namespace

// ---- dominance

TEST("sgl ir dominance - a diamond's arms dominate nothing past themselves")
{
    // 0 -> 1, 2 -> 3
    u32 const first[] = {0, 0, 1, 2, 4};
    u32 const preds[] = {0, 0, 1, 2};
    auto const tree = dominance(first, preds);
    CHECK(tree.dominates(0, 3));
    CHECK(tree.dominates(1, 1));
    CHECK(!tree.dominates(1, 3));
    CHECK(!tree.dominates(2, 3));
    CHECK(!tree.dominates(3, 0));
    CHECK(tree.immediate(3) == 0);
    CHECK(tree.immediate(1) == 0);
    CHECK(tree.immediate(0) == 0);
}

TEST("sgl ir dominance - a back edge changes nothing, and a header dominates its loop")
{
    // 0 -> 1 -> 2 -> 1, 1 -> 3
    u32 const first[] = {0, 0, 2, 3, 4};
    u32 const preds[] = {0, 2, 1, 1};
    auto const tree = dominance(first, preds);
    CHECK(tree.dominates(1, 2));
    CHECK(tree.dominates(1, 3));
    CHECK(!tree.dominates(2, 1));
    CHECK(!tree.dominates(2, 3));
    CHECK(tree.immediate(2) == 1);
    CHECK(tree.immediate(3) == 1);
}

TEST("sgl ir dominance - a node no walk reaches dominates nothing")
{
    u32 const first[] = {0, 0, 0, 1};
    u32 const preds[] = {0};
    auto const tree = dominance(first, preds);
    CHECK(tree.dominates(0, 2));
    CHECK(!tree.dominates(1, 2));
}

// ---- construct_ssa

TEST("sgl ir construct_ssa - a local written in two arms becomes a phi")
{
    auto b = written_in_both_arms(&program_builder::yes);
    REQUIRE(refusal_of(b.p, control_flow::structured) == "");
    construct_ssa(b.p);
    CHECK(refusal_of(b.p, control_flow::structured) == "");
    CHECK(b.p.locals.empty());
    CHECK(count_of(b.p, op::phi) == 1);
    CHECK(count_of(b.p, op::load) == 0);
    // only the store to the buffer stands
    CHECK(count_of(b.p, op::store) == 1);
}

TEST("sgl ir construct_ssa - one definition reaching a read leaves no phi")
{
    auto b = program_builder();
    auto const x = b.add_local();
    b.blocks(1);
    b.open(0);
    b.add(op::store, b.void_type, {u32(x), u32(b.one)});
    b.keep(b.add(op::load, b.float_type, {u32(x)}));
    b.finish();
    construct_ssa(b.p);
    CHECK(refusal_of(b.p, control_flow::structured) == "");
    CHECK(count_of(b.p, op::phi) == 0);
    CHECK(dump(b.p).contains("store @ 0x3f800000"));
}

TEST("sgl ir construct_ssa - a value that changes around a loop gets its phi at the header")
{
    auto b = program_builder();
    auto const x = b.add_local();
    b.blocks(4);
    b.open(0);
    b.add(op::store, b.void_type, {u32(x), u32(b.one)});
    b.branch(1);
    b.open(1);
    b.loop_merge(1, 3, 2);
    auto const read = b.add(op::load, b.float_type, {u32(x)});
    auto const go_on = b.add(op::flt, b.bool_type, {u32(read), u32(b.two)});
    b.branch_if(go_on, 2, 3);
    b.open(2);
    auto const held = b.add(op::load, b.float_type, {u32(x)});
    auto const more = b.add(op::fadd, b.float_type, {u32(held), u32(b.one)});
    b.add(op::store, b.void_type, {u32(x), u32(more)});
    b.branch(1);
    b.open(3);
    b.keep(b.add(op::load, b.float_type, {u32(x)}));
    b.finish();
    REQUIRE(refusal_of(b.p, control_flow::structured) == "");
    construct_ssa(b.p);
    CHECK(refusal_of(b.p, control_flow::structured) == "");
    CHECK(b.p.locals.empty());
    CHECK(count_of(b.p, op::phi) == 1);
    CHECK(count_of(b.p, op::load) == 0);
    CHECK(b.p.instructions[b.p.blocks[1].first].code == op::phi);
}

TEST("sgl ir construct_ssa - a local read before any write gives something undefined, not a guess")
{
    auto b = program_builder();
    auto const x = b.add_local();
    b.blocks(1);
    b.open(0);
    b.keep(b.add(op::load, b.float_type, {u32(x)}));
    b.finish();
    construct_ssa(b.p);
    CHECK(refusal_of(b.p, control_flow::structured) == "");
    CHECK(dump(b.p).contains("undef"));
}

TEST("sgl ir construct_ssa - a local an operation other than a load or a store touches stays a variable")
{
    auto b = program_builder();
    auto const array_type = b.add_type({.kind = type_kind::array, .count = 4, .element = b.float_type});
    auto const array_pointer
        = b.add_type({.kind = type_kind::pointer, .space = storage::function, .element = array_type});
    b.p.locals.push_back({.type = array_pointer});
    auto const xs = local_at(0);
    auto const index = b.add_local();
    b.blocks(1);
    b.open(0);
    // the index is known to no one before run time
    b.add(op::store, b.void_type, {u32(index), u32(b.one)});
    auto const known = b.add(op::load, b.float_type, {u32(index)});
    auto const at = b.add(op::f_to_s, b.uint_type, {u32(known)});
    auto const element = b.add(op::access_chain, b.float_pointer, {u32(xs), u32(at)});
    b.add(op::store, b.void_type, {u32(element), u32(b.two)});
    b.keep(b.add(op::load, b.float_type, {u32(element)}));
    b.finish();
    REQUIRE(refusal_of(b.p, control_flow::structured) == "");
    construct_ssa(b.p);
    CHECK(refusal_of(b.p, control_flow::structured) == "");
    CHECK(b.p.locals.size() == 1);
    CHECK(count_of(b.p, op::access_chain) == 1);
}

// ---- simplify

TEST("sgl ir simplify - a known condition drops the arm it rules out, and the phi with it")
{
    auto b = program_builder();
    b.diamond(b.yes, b.one, b.two);
    simplify(b.p);
    CHECK(refusal_of(b.p, control_flow::unstructured) == "");
    CHECK(count_of(b.p, op::phi) == 0);
    CHECK(count_of(b.p, op::branch_conditional) == 0);
    CHECK(dump(b.p).contains("store @ 0x3f800000"));
    CHECK(!dump(b.p).contains("0x40000000"));
}

TEST("sgl ir simplify - a branch on the negation of a known condition goes the other way")
{
    // what an unrolled `continue` past a test of a constant leaves, once the test folds
    auto b = program_builder();
    b.blocks(4);
    b.open(0);
    b.select_merge(0, 3);
    auto const twice = b.add(op::logical_not, b.bool_type, {u32(b.add(op::logical_not, b.bool_type, {u32(b.no)}))});
    b.branch_if(b.add(op::logical_not, b.bool_type, {u32(twice)}), 1, 2);
    b.open(1);
    b.branch(3);
    b.open(2);
    b.branch(3);
    b.open(3);
    b.keep(b.add(op::phi, b.float_type, {u32(b.one), 1, u32(b.two), 2}));
    b.finish();
    simplify(b.p);
    CHECK(refusal_of(b.p, control_flow::unstructured) == "");
    CHECK(count_of(b.p, op::branch_conditional) == 0);
    CHECK(count_of(b.p, op::logical_not) == 0);
    CHECK(dump(b.p).contains("store @ 0x3f800000"));
}

TEST("sgl ir simplify - the other condition keeps the other value")
{
    auto b = program_builder();
    b.diamond(b.no, b.one, b.two);
    simplify(b.p);
    CHECK(refusal_of(b.p, control_flow::unstructured) == "");
    CHECK(dump(b.p).contains("store @ 0x40000000"));
}

TEST("sgl ir simplify - a phi of two constants of the same bits is that constant, a null zero among them")
{
    // a struct returned from two places leaves a field at 0.0 in one and at a folded zero in the other
    for (auto const flow : {control_flow::structured, control_flow::unstructured})
    {
        auto b = program_builder();
        auto const zero = b.add_constant({.type = b.float_type, .low = 0});
        auto const null = b.add_constant({.type = b.float_type, .is_null = true});
        b.blocks(4);
        b.open(0);
        b.select_merge(0, 3);
        auto const read = b.add(op::load, b.float_type, {u32(b.out)});
        b.branch_if(b.add(op::flt, b.bool_type, {u32(read), u32(b.two)}), 1, 2);
        b.open(1);
        b.branch(3);
        b.open(2);
        b.branch(3);
        b.open(3);
        b.keep(b.add(op::phi, b.float_type, {u32(zero), 1, u32(null), 2}));
        b.finish();
        simplify(b.p, flow);
        CHECK(refusal_of(b.p, flow) == "");
        CHECK(count_of(b.p, op::phi) == 0);
    }
}

TEST("sgl ir simplify - a branch on what a dominating branch decided goes that branch's way")
{
    // openpbr's `else if pick < p.fuzz + p.coat`, with no coat: the condition its `if` already found false
    auto b = program_builder();
    b.blocks(5);
    b.open(0);
    auto const read = b.add(op::load, b.float_type, {u32(b.out)});
    b.branch_if(b.add(op::flt, b.bool_type, {u32(read), u32(b.two)}), 1, 2);
    b.open(1);
    b.keep(b.one);
    b.branch(4);
    b.open(2);
    b.branch_if(b.add(op::flt, b.bool_type, {u32(read), u32(b.two)}), 3, 4);
    b.open(3);
    b.keep(b.two);
    b.branch(4);
    b.open(4);
    b.finish();
    simplify(b.p);
    CHECK(refusal_of(b.p, control_flow::unstructured) == "");
    CHECK(count_of(b.p, op::branch_conditional) == 1);
    CHECK(!dump(b.p).contains("store @ 0x40000000"));
}

TEST("sgl ir simplify - any of a vector comparison known in every lane is a constant, and so is a select of two zeros")
{
    auto b = program_builder();
    auto const null = b.add_constant({.type = b.float_type, .is_null = true});
    auto const zero = b.add_constant({.type = b.float_type, .low = 0});
    b.blocks(1);
    b.open(0);
    auto const ones = b.add(op::construct, b.float2_type, {u32(b.one), u32(b.one)});
    auto const mixed = b.add(op::construct, b.float2_type, {u32(b.one), u32(b.two)});
    auto const bool2 = b.add_type({.kind = type_kind::vector, .count = 2, .element = b.bool_type});
    auto const differs = b.add(op::fne, bool2, {u32(ones), u32(mixed)});
    auto const any = b.add(op::any, b.bool_type, {u32(differs)});
    auto const read = b.add(op::load, b.float_type, {u32(b.out)});
    auto const picked = b.add(op::select, b.float_type,
                              {u32(b.add(op::flt, b.bool_type, {u32(read), u32(b.two)})), u32(null), u32(zero)});
    b.keep(b.add(op::select, b.float_type, {u32(any), u32(picked), u32(b.two)}));
    b.finish();
    simplify(b.p);
    CHECK(refusal_of(b.p, control_flow::unstructured) == "");
    CHECK(count_of(b.p, op::any) == 0);
    CHECK(count_of(b.p, op::select) == 0);
}

TEST("sgl ir simplify structured - a dead arm goes whole, and only a merge a live header names stays a stub")
{
    for (auto const both_arms_return : {false, true})
    {
        // a known selection around a nested one, or a live selection whose merge no way reaches
        auto b = program_builder();
        b.blocks(6);
        b.open(0);
        b.select_merge(0, 5);
        auto const read = b.add(op::load, b.float_type, {u32(b.out)});
        auto const less = b.add(op::flt, b.bool_type, {u32(read), u32(b.two)});
        b.branch_if(both_arms_return ? less : b.no, 1, 5);
        b.open(1);
        b.select_merge(1, 4);
        b.branch_if(b.add(op::fgt, b.bool_type, {u32(read), u32(b.one)}), 2, 3);
        b.open(2);
        b.add(op::return_, b.void_type, {});
        b.open(3);
        b.add(op::return_, b.void_type, {});
        b.open(4);
        b.branch(5);
        b.open(5);
        b.finish();
        simplify(b.p, control_flow::structured);
        CHECK(refusal_of(b.p, control_flow::structured) == "");
        auto standing = 0;
        for (auto const& block : b.p.blocks)
            standing += block.count > 0 ? 1 : 0;
        // the inner merge no walk reaches stays as `unreachable` only where its header runs
        CHECK(standing == (both_arms_return ? 6 : 2)).dump("program", dump(b.p));
        CHECK(count_of(b.p, op::unreachable) == (both_arms_return ? 1 : 0));
    }
}

TEST("sgl ir simplify - an integer multiply by a power of two is a shift, either way round")
{
    for (auto const constant_first : {false, true})
    {
        auto b = program_builder();
        auto const eight = b.add_constant({.type = b.uint_type, .low = 8});
        b.blocks(1);
        b.open(0);
        auto const read = b.add(op::f_to_u, b.uint_type, {u32(b.add(op::load, b.float_type, {u32(b.out)}))});
        auto const product = constant_first ? b.add(op::imul, b.uint_type, {u32(eight), u32(read)})
                                            : b.add(op::imul, b.uint_type, {u32(read), u32(eight)});
        b.keep(b.add(op::u_to_f, b.float_type, {u32(product)}));
        b.finish();
        simplify(b.p);
        CHECK(refusal_of(b.p, control_flow::unstructured) == "");
        CHECK(count_of(b.p, op::imul) == 0);
        CHECK(count_of(b.p, op::shift_left) == 1);
    }
}

TEST("sgl ir simplify - a condition nobody knows keeps both arms")
{
    auto b = program_builder();
    b.blocks(4);
    b.open(0);
    b.select_merge(0, 3);
    auto const read = b.add(op::load, b.float_type, {u32(b.out)});
    auto const less = b.add(op::flt, b.bool_type, {u32(read), u32(b.two)});
    b.branch_if(less, 1, 2);
    b.open(1);
    b.branch(3);
    b.open(2);
    b.branch(3);
    b.open(3);
    b.keep(b.add(op::phi, b.float_type, {u32(b.one), 1, u32(b.two), 2}));
    b.finish();
    simplify(b.p);
    CHECK(refusal_of(b.p, control_flow::structured) == "");
    CHECK(count_of(b.p, op::phi) == 1);
    CHECK(count_of(b.p, op::branch_conditional) == 1);
}

TEST("sgl ir simplify - integers fold, and nothing with an effect goes")
{
    auto b = program_builder();
    b.blocks(1);
    b.open(0);
    auto const sum = b.add(op::iadd, b.uint_type, {u32(b.three), u32(b.four)});
    auto const as_float = b.add(op::u_to_f, b.float_type, {u32(sum)});
    b.keep(as_float);
    b.finish();
    simplify(b.p);
    CHECK(refusal_of(b.p, control_flow::unstructured) == "");
    CHECK(count_of(b.p, op::iadd) == 0);
    CHECK(count_of(b.p, op::store) == 1);
    // seven, as a float: the conversion folds too
    CHECK(dump(b.p).contains("store @ 0x40e00000"));
}

TEST("sgl ir simplify - a division by zero stays for the target to answer")
{
    auto b = program_builder();
    auto const zero = b.add_constant({.type = b.uint_type, .low = 0});
    b.blocks(1);
    b.open(0);
    auto const q = b.add(op::udiv, b.uint_type, {u32(b.three), u32(zero)});
    b.keep(b.add(op::u_to_f, b.float_type, {u32(q)}));
    b.finish();
    simplify(b.p);
    CHECK(refusal_of(b.p, control_flow::unstructured) == "");
    CHECK(count_of(b.p, op::udiv) == 1);
}

TEST("sgl ir simplify - a fold keeps what a precise operation promises")
{
    auto b = program_builder();
    b.blocks(1);
    b.open(0);
    auto const sum = b.add(op::fadd, b.float_type, {u32(b.one), u32(b.two)});
    b.p.instructions[0].guarantee = sgl::check::float_level::precise;
    b.keep(sum);
    b.finish();
    simplify(b.p);
    CHECK(refusal_of(b.p, control_flow::unstructured) == "");
    for (auto const& i : b.p.instructions)
        if (i.code == op::fadd)
            CHECK(i.guarantee == sgl::check::float_level::precise);
}

TEST("sgl ir simplify - a loop whose back edge folds away leaves an unstructured whole")
{
    auto b = program_builder();
    b.blocks(4);
    b.open(0);
    b.branch(1);
    b.open(1);
    b.loop_merge(1, 3, 2);
    b.keep(b.one);
    b.branch_if(b.no, 2, 3);
    b.open(2);
    b.branch(1);
    b.open(3);
    b.finish();
    REQUIRE(refusal_of(b.p, control_flow::structured) == "");
    simplify(b.p);
    CHECK(refusal_of(b.p, control_flow::unstructured) == "");
    CHECK(count_of(b.p, op::branch_conditional) == 0);
    CHECK(count_of(b.p, op::store) == 1);
}
