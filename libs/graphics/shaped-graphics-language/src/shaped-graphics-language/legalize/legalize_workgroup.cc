#include <clean-core/function/function_ref.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics-language/builtins/registry.hh>
#include <shaped-graphics-language/legalize/impl/legalizer.hh>
#include <shaped-graphics-language/legalize/impl/walk.hh>

// LEGAL-71: the shadow of workgroup memory and the prologue every target owes it, as flat statements of a finished
// core tree, so no route knows what a shadow word or a prologue is.
//
// Each invocation clears the words of the shadow its index strides to, or under `define` zeroes workgroup memory,
// then the workgroup waits, so no mark lands before a clear:
//     let index = uint(local_index)
//     for k in 0 ..< ceil(words / group):
//         let w = index + k * group
//         if w < words:
//             sgl_written[w].store(0)
//     workgroup_barrier()

using namespace sgl;
using namespace sgl::check;
using sgl::check::impl::stmt_list;

namespace
{
struct prologue_writer
{
    flat_builder& out;
    type_id uint_type = type_id::none;
    type_id atomic_uint = type_id::none;
    local_id index = local_id::none;
    u32 group = 1;

    [[nodiscard]] flat_expr_id uint_literal(u32 value)
    {
        return out.add_expr(uint_type, flat_int_literal{.value = i32(value), .is_unsigned = true});
    }
    [[nodiscard]] flat_expr_id uint_call(cc::string_view name, flat_expr_id a, flat_expr_id b)
    {
        flat_expr_id const args[] = {a, b};
        return out.call(name, args);
    }
    [[nodiscard]] flat_expr_id word(flat_expr_id at)
    {
        return out.add_expr(atomic_uint, flat_written_word{.word = at});
    }

    [[nodiscard]] bool holds_atomic(type_id type) const
    {
        if (out.m.builtin_type_of(type) != nullptr)
            return false;
        auto const& t = out.m.at(type);
        if (t.kind == type_kind::atomic)
            return true;
        if (t.kind == type_kind::array)
            return holds_atomic(t.element);
        if (t.kind == type_kind::structure)
            for (auto const& m : out.m.at(t.members))
                if (holds_atomic(m.type))
                    return true;
        return false;
    }

    /// A value of `type` whose every scalar is zero, which holds no atomic.
    [[nodiscard]] flat_expr_id zero_value(type_id type)
    {
        auto const& t = out.m.at(type);
        if (t.kind == type_kind::array)
        {
            auto elements = cc::vector<flat_expr_id>();
            for (auto i = 0; i < t.count; ++i)
                elements.push_back(zero_value(t.element));
            return out.construct(type, elements);
        }
        if (t.kind == type_kind::structure && !out.m.at(t.members).empty())
        {
            auto fields = cc::vector<flat_expr_id>();
            for (auto const& m : out.m.at(t.members))
                fields.push_back(zero_value(m.type));
            return out.construct(type, fields);
        }
        auto const name = out.m.name_of(type);
        // a matrix declares no fields, and is its columns
        if (name == "mat2" || name == "mat3" || name == "mat4")
        {
            auto const column = out.type_named(cc::format("float{}", name.back()));
            auto columns = cc::vector<flat_expr_id>();
            for (auto c = 0; c < name.back() - '0'; ++c)
                columns.push_back(zero_value(column));
            return out.call(name, columns);
        }
        if (name == builtins::k_bool)
            return out.bool_literal(false);
        if (name == "long" || name == "ulong")
            return out.add_expr(type, flat_wide_literal{.bits = 0});
        if (name == "float" || name == "half" || name == "double")
            return out.add_expr(type, flat_literal{.value = 0.0});
        return out.add_expr(type, flat_int_literal{.value = 0, .is_unsigned = name.starts_with("u")});
    }

    /// `place()` set to zero: an atomic by its store, since no assignment names one, and a whole value otherwise.
    void zero(cc::function_ref<flat_expr_id()> place, type_id type, stmt_list& into)
    {
        auto const& t = out.m.at(type);
        if (!holds_atomic(type))
        {
            into.push_back(out.assign(place(), zero_value(type)));
            return;
        }
        if (t.kind == type_kind::atomic)
        {
            flat_expr_id const args[] = {place(), zero_value(t.element)};
            into.push_back(out.eval(out.call_with_effect("store", args)));
            return;
        }
        if (t.kind == type_kind::array)
        {
            auto const label = out.add_label("zeroed");
            auto const i = out.add_local(local_kind::index, "zeroed", out.type_named(builtins::k_int));
            auto body = stmt_list();
            zero([&] { return out.add_expr(t.element, flat_element{.object = place(), .index = out.local(i)}); },
                 t.element, body);
            into.push_back(out.for_(label, i, out.int_literal(0), out.int_literal(t.count), body));
            return;
        }
        auto const members = out.m.at(t.members);
        for (auto m = isize(0); m < members.size(); ++m)
            zero([&] { return out.member(place(), i32(m)); }, members[m].type, into);
    }

    /// `each(w)` for every `w ..< count` this invocation's index strides to, `w` a pinned `uint`.
    template <class Each>
    void strided(u32 count, stmt_list& into, Each const& each)
    {
        auto const label = out.add_label("strided");
        auto const k = out.add_local(local_kind::index, "k", out.type_named(builtins::k_int));
        auto body = stmt_list();
        flat_expr_id const k_args[] = {out.local(k)};
        auto const w = out.add_local(local_kind::temporary, "strided", uint_type);
        body.push_back(out.let(
            w, uint_call("add_uint", out.local(index),
                         uint_call("multiply_uint", out.call("convert_int_to_uint", k_args), uint_literal(group)))));
        auto inner = stmt_list();
        each(w, inner);
        body.push_back(out.if_(uint_call("less_uint", out.local(w), uint_literal(count)), inner));
        into.push_back(out.for_(label, k, out.int_literal(0), out.int_literal(i32((count + group - 1) / group)), body));
    }

    void write(stmt_list& into)
    {
        auto& e = out.e;
        flat_expr_id const raw[] = {out.local(e.workgroup_index)};
        index = out.add_local(local_kind::temporary, "local_index_uint", uint_type);
        into.push_back(out.let(index, out.call("convert_int_to_uint", raw)));
        group = u32(e.workgroup[0] * e.workgroup[1] * e.workgroup[2]);

        if (e.written_words > 0)
            strided(u32(e.written_words), into,
                    [&](local_id w, stmt_list& body)
                    {
                        flat_expr_id const args[] = {word(out.local(w)), uint_literal(0)};
                        body.push_back(out.eval(out.call_with_effect("store", args)));
                    });
        if (e.zeroes_workgroup)
            for (auto const id : cc::vector<symbol_id>::create_copy_of(cc::span<symbol_id const>(e.bindings)))
            {
                auto const& b = out.m.bindings[out.m.at(id).info];
                if (!b.is_workgroup)
                    continue;
                auto const members = out.m.at(b.members);
                for (auto m = isize(0); m < members.size(); ++m)
                {
                    auto const type = members[m].type;
                    auto const& t = out.m.at(type);
                    auto const member = [&] { return out.binding_member(id, i32(m)); };
                    if (out.m.builtin_type_of(type) == nullptr && t.kind == type_kind::array)
                    {
                        strided(u32(t.count), into,
                                [&](local_id w, stmt_list& body)
                                {
                                    flat_expr_id const w_args[] = {out.local(w)};
                                    zero(
                                        [&]
                                        {
                                            return out.add_expr(
                                                t.element,
                                                flat_element{.object = member(),
                                                             .index = out.call("convert_uint_to_int", w_args)});
                                        },
                                        t.element, body);
                                });
                        continue;
                    }
                    auto first = stmt_list();
                    zero(member, type, first);
                    into.push_back(out.if_(uint_call("equal_uint", out.local(index), uint_literal(0)), first));
                }
            }
        into.push_back(out.eval(out.call("workgroup_barrier", {})));
    }
};

/// `atomic[uint]`, which the prelude's atomics declare.
[[nodiscard]] type_id atomic_uint_of(checked_module const& m, type_id uint_type)
{
    for (auto i = isize(0); i < m.types.size(); ++i)
        if (m.types[i].kind == type_kind::atomic && m.types[i].element == uint_type)
            return type_id(i);
    return type_id::none;
}
} // namespace

bool check::impl::has_workgroup_shadow(flat_entry_point const& e)
{
    return e.written_words > 0 || e.zeroes_workgroup;
}

stmt_list check::impl::expand_workgroup(flat_builder& out, stmt_list const& body, legalize_options const& options)
{
    auto w = prologue_writer{.out = out, .uint_type = out.type_named(builtins::k_uint)};
    w.atomic_uint = atomic_uint_of(out.m, w.uint_type);

    // the guards wrote a read and a mark as calls, which no pass between had to know; each is now its atomic, on the
    // same operands, so every expression keeps its one parent
    auto const count = out.e.exprs.size();
    for (auto i = isize(0); i < count; ++i)
    {
        auto const* const call = out.e.exprs[i].node.try_as<flat_call>();
        auto const* const record = call != nullptr ? out.m.builtin_function(call->intrinsic) : nullptr;
        if (record == nullptr)
            continue;
        auto const is_read = record->name == "workgroup_written_word";
        if (!is_read && record->name != "workgroup_mark_written")
            continue;
        auto args = cc::vector<flat_expr_id>::create_copy_of(cc::span<flat_expr_id const>(out.e.at(call->arguments)));
        args[0] = w.word(args[0]);
        auto const atomic = is_read ? out.call("load", args) : out.call_with_effect("bit_or", args);
        out.e.exprs[i].type = out.e.exprs[index_of(atomic)].type;
        out.e.exprs[i].node = out.e.exprs[index_of(atomic)].node;
    }

    // WebGPU's workgroup memory starts at zero, the shadow's included, so its text owes no prologue
    if (options.workgroup_starts_zeroed || !is_valid(out.e.workgroup_index))
        return body;
    auto result = stmt_list();
    w.write(result);
    result.push_back_range(body);
    return result;
}
