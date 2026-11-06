#pragma once

#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <shaped-graphics-language/ir/ir.hh>

#include <initializer_list>

/// `ir::program`s written by hand, since a pass's contract is about programs no lowering gives.
namespace sgl_test::impl
{
using namespace cc::primitive_defines;
using namespace sgl::ir;

class program_builder
{
public:
    program_builder()
    {
        void_type = add_type({.kind = type_kind::void_});
        bool_type = add_type({.kind = type_kind::bool_});
        float_type = add_type({.kind = type_kind::float_, .width = 32});
        uint_type = add_type({.kind = type_kind::int_, .width = 32});
        float2_type = add_type({.kind = type_kind::vector, .count = 2, .element = float_type});
        float_pointer = add_type({.kind = type_kind::pointer, .space = storage::function, .element = float_type});
        buffer_pointer = add_type({.kind = type_kind::pointer, .space = storage::storage_buffer, .element = float_type});
        no = add_constant({.type = bool_type, .low = 0});
        yes = add_constant({.type = bool_type, .low = 1});
        one = add_constant({.type = float_type, .low = 0x3f800000u});
        two = add_constant({.type = float_type, .low = 0x40000000u});
        three = add_constant({.type = uint_type, .low = 3});
        four = add_constant({.type = uint_type, .low = 4});
        out = global_at(u32(p.globals.size()));
        p.globals.push_back({.type = buffer_pointer, .kind = global_kind::plain});
        p.entry_name = "main";
    }

    type_ref add_type(type t)
    {
        p.types.push_back(t);
        return type_ref(p.types.size() - 1);
    }
    value_ref add_constant(constant c)
    {
        p.constants.push_back(c);
        return constant_at(u32(p.constants.size() - 1));
    }
    value_ref add_local()
    {
        p.locals.push_back({.type = float_pointer});
        return local_at(u32(p.locals.size() - 1));
    }

    /// Reserves `count` blocks, each opened by `open`.
    void blocks(isize count)
    {
        for (auto i = isize(0); i < count; ++i)
            p.blocks.push_back({});
    }
    void open(isize b)
    {
        _open = b;
        p.blocks[b].first = u32(p.instructions.size());
    }
    value_ref add(op code, type_ref t, std::initializer_list<u32> operands, u8 sub = 0)
    {
        auto const first = u32(p.operands.size());
        for (auto const o : operands)
            p.operands.push_back(o);
        p.instructions.push_back({.code = code, .sub = sub, .type = t, .first = first, .count = u32(operands.size())});
        p.blocks[_open].count = u32(p.instructions.size()) - p.blocks[_open].first;
        return value_ref(p.instructions.size() - 1);
    }
    /// An effect nothing may drop, which keeps `v` used.
    void keep(value_ref v) { add(op::store, void_type, {u32(out), u32(v)}); }
    void branch(isize to) { add(op::branch, void_type, {u32(to)}); }
    void branch_if(value_ref condition, isize yes_to, isize no_to)
    {
        add(op::branch_conditional, void_type, {u32(condition), u32(yes_to), u32(no_to)});
    }
    void finish() { add(op::return_, void_type, {}); }
    void select_merge(isize header, isize merge)
    {
        p.blocks[header].merges = merge_kind::selection;
        p.blocks[header].merge = block_ref(merge);
    }
    void loop_merge(isize header, isize merge, isize next)
    {
        p.blocks[header].merges = merge_kind::loop;
        p.blocks[header].merge = block_ref(merge);
        p.blocks[header].continue_to = block_ref(next);
    }

    /// `if condition: then else: otherwise`, joined in b3 by a phi, as a lowering writes one.
    value_ref diamond(value_ref condition, value_ref then_value, value_ref else_value)
    {
        blocks(4);
        open(0);
        select_merge(0, 3);
        branch_if(condition, 1, 2);
        open(1);
        branch(3);
        open(2);
        branch(3);
        open(3);
        auto const joined = add(op::phi, float_type, {u32(then_value), 1, u32(else_value), 2});
        keep(joined);
        finish();
        return joined;
    }

    program p;
    type_ref void_type = type_ref::none;
    type_ref bool_type = type_ref::none;
    type_ref float_type = type_ref::none;
    type_ref uint_type = type_ref::none;
    type_ref float2_type = type_ref::none;
    type_ref float_pointer = type_ref::none;
    type_ref buffer_pointer = type_ref::none;
    value_ref no = value_ref::none;
    value_ref yes = value_ref::none;
    value_ref one = value_ref::none;
    value_ref two = value_ref::none;
    value_ref three = value_ref::none;
    value_ref four = value_ref::none;
    value_ref out = value_ref::none;

private:
    isize _open = 0;
};
} // namespace sgl_test::impl
