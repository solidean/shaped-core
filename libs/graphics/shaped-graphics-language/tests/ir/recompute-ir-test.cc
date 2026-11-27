#include "ir-test-support.hh"

#include <shaped-graphics-language/ir/passes.hh>

using namespace sgl_test::impl;

// EMIT-292's pass on programs written by hand: where a copy goes, which way of a phi reads it, and what it leaves.

namespace
{
/// A program builder with a constant block of one float, `@g`, and a callable's parameter, `@c`.
struct call_program
{
    program_builder b;
    value_ref block = value_ref::none;
    value_ref parameter = value_ref::none;
    value_ref zero = value_ref::none;
    type_ref field_pointer = type_ref::none;

    call_program()
    {
        auto& p = b.p;
        p.members.push_back({.type = b.float_type, .offset = 0});
        auto const structure = b.add_type({.kind = type_kind::structure,
                                           .is_block = true,
                                           .space = storage::uniform,
                                           .first_member = 0,
                                           .member_count = 1});
        auto const block_pointer
            = b.add_type({.kind = type_kind::pointer, .space = storage::uniform, .element = structure});
        field_pointer = b.add_type({.kind = type_kind::pointer, .space = storage::uniform, .element = b.float_type});
        auto const parameter_pointer
            = b.add_type({.kind = type_kind::pointer, .space = storage::callable_output, .element = b.float_type});
        block = global_at(u32(p.globals.size()));
        p.globals.push_back({.type = block_pointer, .kind = global_kind::constants});
        parameter = global_at(u32(p.globals.size()));
        p.globals.push_back({.type = parameter_pointer, .kind = global_kind::plain});
        zero = b.add_constant({.type = b.uint_type, .low = 0});
    }

    /// The constant block's field doubled: an access chain, a load and a product.
    value_ref doubled()
    {
        auto const chain = b.add(op::access_chain, field_pointer, {u32(block), u32(zero)});
        auto const read = b.add(op::load, b.float_type, {u32(chain)});
        return b.add(op::fmul, b.float_type, {u32(read), u32(b.two)});
    }
    void call() { b.add(op::execute_callable, b.void_type, {u32(zero), u32(parameter)}); }

    /// How many operations of `code` block `at` holds.
    [[nodiscard]] isize count_in(isize at, op code) const
    {
        auto const& block_of = b.p.blocks[at];
        auto n = isize(0);
        for (auto i = block_of.first; i < block_of.first + block_of.count; ++i)
            n += b.p.instructions[i].code == code ? 1 : 0;
        return n;
    }
    [[nodiscard]] isize count(op code) const
    {
        auto n = isize(0);
        for (auto const& i : b.p.instructions)
            n += i.code == code ? 1 : 0;
        return n;
    }
    /// The block that holds value `v`.
    [[nodiscard]] isize block_holding(value_ref v) const
    {
        for (auto at = isize(0); at < b.p.blocks.size(); ++at)
            if (index_of(v) >= b.p.blocks[at].first && index_of(v) < b.p.blocks[at].first + b.p.blocks[at].count)
                return at;
        return -1;
    }
};
} // namespace

TEST("sgl ir recompute_after_calls - a use after a call reads a copy of the whole chain, made right after the call")
{
    auto c = call_program();
    c.b.blocks(1);
    c.b.open(0);
    auto const value = c.doubled();
    c.b.keep(value);
    c.call();
    c.b.keep(value);
    c.b.keep(value);
    c.b.finish();
    recompute_after_calls(c.b.p);
    remove_unused(c.b.p);

    // the use ahead of the call keeps the original; both after it share one copy, which stands after the call
    CHECK(c.count(op::load) == 2);
    CHECK(c.count(op::access_chain) == 2);
    CHECK(c.count(op::fmul) == 2);
    auto const& p = c.b.p;
    auto call_at = isize(-1);
    auto copy_at = isize(-1);
    for (auto i = isize(0); i < p.instructions.size(); ++i)
    {
        if (p.instructions[i].code == op::execute_callable)
            call_at = i;
        if (p.instructions[i].code == op::load && call_at >= 0)
            copy_at = i;
    }
    CHECK(copy_at == call_at + 2);
}

TEST("sgl ir recompute_after_calls - a phi reads a copy on the way a call stands on, and the original on the other")
{
    auto c = call_program();
    c.b.blocks(4);
    c.b.open(0);
    auto const value = c.doubled();
    c.b.branch_if(c.b.yes, 1, 2);
    c.b.open(1);
    c.call();
    c.b.branch(3);
    c.b.open(2);
    c.b.branch(3);
    c.b.open(3);
    auto const joined = c.b.add(op::phi, c.b.float_type, {u32(value), 1, u32(value), 2});
    c.b.keep(joined);
    c.b.keep(value);
    c.b.finish();
    recompute_after_calls(c.b.p);
    remove_unused(c.b.p);

    auto const& p = c.b.p;
    // a copy at the end of the way with the call, one in the joining block after its phi, and the original
    CHECK(c.count(op::load) == 3);
    CHECK(c.count_in(0, op::load) == 1);
    CHECK(c.count_in(1, op::load) == 1);
    CHECK(c.count_in(2, op::load) == 0);
    CHECK(c.count_in(3, op::load) == 1);
    CHECK(p.instructions[p.blocks[3].first].code == op::phi);
    auto const& phi = p.instructions[p.blocks[3].first];
    auto const operands = p.operands_of(phi);
    REQUIRE(operands.size() == 4);
    CHECK(c.block_holding(value_ref(operands[0])) == 1);
    CHECK(c.block_holding(value_ref(operands[2])) == 0);
}

TEST("sgl ir recompute_after_calls - a value is copied into a loop whose call can run before it")
{
    auto c = call_program();
    c.b.blocks(4);
    c.b.open(0);
    auto const value = c.doubled();
    c.b.branch(1);
    c.b.open(1);
    c.b.branch_if(c.b.yes, 2, 3);
    c.b.open(2);
    // read ahead of this body's own call, which the way around the loop runs before it
    c.b.keep(value);
    c.call();
    c.b.branch(1);
    c.b.open(3);
    c.b.finish();
    recompute_after_calls(c.b.p);
    remove_unused(c.b.p);

    CHECK(c.count(op::load) == 1);
    CHECK(c.count_in(2, op::load) == 1);
}

TEST("sgl ir recompute_after_calls - no call point, or none between, leaves the program as it was")
{
    auto c = call_program();
    c.b.blocks(2);
    c.b.open(0);
    auto const value = c.doubled();
    c.b.branch(1);
    c.b.open(1);
    c.b.keep(value);
    c.call();
    c.b.finish();
    auto const before = dump(c.b.p);
    recompute_after_calls(c.b.p);
    CHECK(dump(c.b.p) == before);

    auto plain = call_program();
    plain.b.blocks(1);
    plain.b.open(0);
    auto const read = plain.doubled();
    plain.b.keep(read);
    plain.b.keep(read);
    plain.b.finish();
    auto const was = dump(plain.b.p);
    recompute_after_calls(plain.b.p);
    CHECK(dump(plain.b.p) == was);
}

TEST("sgl ir recompute_after_calls - a read of writable memory is kept across the call, and what it feeds with it")
{
    auto c = call_program();
    auto& b = c.b;
    b.blocks(1);
    b.open(0);
    // a buffer's element, which the shader or a called shader can write
    auto const element = b.add(op::load, b.float_type, {u32(b.out)});
    auto const scaled = b.add(op::fmul, b.float_type, {u32(element), u32(c.doubled())});
    c.call();
    b.keep(scaled);
    b.finish();
    recompute_after_calls(b.p);
    remove_unused(b.p);

    // the product reads the buffer, so it stays where it was, and the constant it read with it is kept too
    CHECK(c.count(op::load) == 2);
    CHECK(c.count(op::fmul) == 2);
}
