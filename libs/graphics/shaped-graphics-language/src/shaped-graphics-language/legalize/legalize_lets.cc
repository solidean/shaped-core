#include <clean-core/common/hash.hh>
#include <clean-core/container/map.hh>
#include <clean-core/math/bit.hh>
#include <shaped-graphics-language/legalize/core.hh>
#include <shaped-graphics-language/legalize/impl/legalizer.hh>
#include <shaped-graphics-language/legalize/impl/walk.hh>

// Rules F1 (LEGAL-58) and F2 (LEGAL-59), then L3 (LEGAL-60), L1 (LEGAL-56), L2 (LEGAL-57) and L4 (LEGAL-61), after V1.
// What substitution made constant, a branch on it, a block's result, a let that repeats one in scope, a let nothing reads,
// and a struct read only field by field.

using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;

namespace
{
/// The local an assignment's place or a written argument ends in, through members and elements; none for memory.
local_id root_local_of(flat_entry_point const& e, flat_expr_id id)
{
    for (auto depth = 0; is_known(e, id) && depth < k_max_depth; ++depth)
    {
        auto const& node = e.at(id).node;
        if (auto const* const ref = node.try_as<flat_local_ref>())
            return ref->local;
        if (auto const* const member = node.try_as<flat_member>())
            id = member->object;
        else if (auto const* const element = node.try_as<flat_element>())
            id = element->object;
        else
            return local_id::none;
    }
    return local_id::none;
}

/// Rules F1 and F2, before L1 and L2: what substitution made constant is the literal it is, and a branch on one is
/// the side it takes.
struct constant_folder
{
    flat_builder& out;
    /// Per local: the literal an immutable `let` holds, which a call that folds whole reads through.
    cc::vector<flat_expr_id> literal_of;

    [[nodiscard]] checked_module const& m() const { return out.m; }
    [[nodiscard]] flat_entry_point const& e() const { return out.e; }

    /// The scalar a number or bool type holds one of; none for any other type.
    [[nodiscard]] check::value_kind scalar_kind_of(type_id type) const
    {
        auto const* const record = m().builtin_type_of(type);
        if (record == nullptr || record->leaf_count != 1)
            return check::value_kind::none;
        return record->leaf_kind;
    }

    /// The scalar `id` is where it is constant, reading through the immutable lets of a literal; none elsewhere.
    [[nodiscard]] cc::optional<check::scalar> value_of(flat_expr_id id, int depth) const
    {
        using check::scalar;
        using check::value_kind;
        if (!is_known(e(), id) || depth > k_max_depth)
            return cc::nullopt;
        auto const& x = e().at(id);
        if (auto const* const n = x.node.try_as<flat_int_literal>())
            return n->is_unsigned ? scalar::of_uint(u32(n->value)) : scalar::of(n->value);
        if (auto const* const b = x.node.try_as<flat_bool_literal>())
            return scalar::of(b->value);
        if (auto const* const f = x.node.try_as<flat_literal>();
            f != nullptr && scalar_kind_of(x.type) == value_kind::scalar_float)
            return scalar::of(f32(f->value));
        if (auto const* const v = x.node.try_as<flat_enum_value>(); v != nullptr && m().is_plain_enum(x.type))
        {
            auto const cases = m().at(m().at(x.type).cases);
            if (v->case_index < 0 || v->case_index >= cases.size())
                return cc::nullopt;
            return scalar::of(cases[v->case_index].value);
        }
        if (auto const* const ref = x.node.try_as<flat_local_ref>())
        {
            if (!is_known(e(), ref->local) || !is_valid(literal_of[index_of(ref->local)]))
                return cc::nullopt;
            return value_of(literal_of[index_of(ref->local)], depth + 1);
        }
        if (auto const* const n = x.node.try_as<flat_not>())
        {
            auto const operand = value_of(n->operand, depth + 1);
            if (!operand.has_value() || operand.value().kind != value_kind::boolean)
                return cc::nullopt;
            return scalar::of(!operand.value().as_bool());
        }
        if (x.node.is<flat_and>() || x.node.is<flat_or>())
        {
            auto const is_and = x.node.is<flat_and>();
            auto const lhs = value_of(is_and ? x.node.as<flat_and>().lhs : x.node.as<flat_or>().lhs, depth + 1);
            auto const rhs = value_of(is_and ? x.node.as<flat_and>().rhs : x.node.as<flat_or>().rhs, depth + 1);
            if (!lhs.has_value() || !rhs.has_value())
                return cc::nullopt;
            return scalar::of(is_and ? lhs.value().as_bool() && rhs.value().as_bool()
                                     : lhs.value().as_bool() || rhs.value().as_bool());
        }
        auto const* const call = x.node.try_as<flat_call>();
        if (call == nullptr || !call->is_pure || !is_known(e(), call->arguments))
            return cc::nullopt;
        auto const* const record = m().builtin_function(call->intrinsic);
        if (record == nullptr || record->evaluate == nullptr || record->uses_derivatives
            || record->is_subgroup_operation || record->is_quad_operation || record->is_barrier || record->is_atomic
            || record->differs_between_invocations || record->bytes_words > 0 || record->is_nonuniform_mark)
            return cc::nullopt;
        // only an integer or a bool result: a float would be rounded here where a target rounds on its own
        auto const result_kind = scalar_kind_of(x.type);
        if (result_kind != value_kind::scalar_int && result_kind != value_kind::scalar_uint
            && result_kind != value_kind::boolean)
            return cc::nullopt;
        auto arguments = cc::vector<scalar>();
        for (auto const a : e().at(call->arguments))
        {
            auto const value = value_of(a, depth + 1);
            if (!value.has_value())
                return cc::nullopt;
            arguments.push_back(value.value());
        }
        if (arguments.empty())
            return cc::nullopt;
        // a call with no value for these arguments stays, so a run still meets it (EVAL-84)
        if (record->undefined_when != nullptr && !record->undefined_when(arguments).empty())
            return cc::nullopt;
        auto result = cc::vector<scalar>();
        record->evaluate(arguments, result);
        if (result.size() != 1 || result[0].kind != result_kind)
            return cc::nullopt;
        return result[0];
    }

    /// F1 at `id` and below it: an operation that is constant is its literal, written in place, since that is what
    /// it means wherever it stands.
    /// A literal and a read of a local stay as they are.
    void fold(flat_expr_id id, int depth)
    {
        if (!is_known(e(), id) || depth > k_max_depth)
            return;
        auto const& x = e().at(id);
        if (x.node.is<flat_call>() || x.node.is<flat_not>() || x.node.is<flat_and>() || x.node.is<flat_or>())
            if (auto const value = value_of(id, 0); value.has_value())
            {
                auto& folded = out.e.exprs[index_of(id)];
                if (value.value().kind == check::value_kind::boolean)
                    folded.node = flat_bool_literal{.value = value.value().as_bool()};
                else
                    folded.node = flat_int_literal{.value = value.value().as_int(),
                                                   .is_unsigned = value.value().kind == check::value_kind::scalar_uint};
                return;
            }
        auto operands = cc::vector<flat_expr_id>();
        for_each_operand(e(), x, [&](flat_expr_id operand) { operands.push_back(operand); });
        for (auto const operand : operands)
            fold(operand, depth + 1);
    }

    void fold_statements(ast::range_of<flat_stmt_id> range, int depth)
    {
        if (!is_known(e(), range) || depth > k_max_depth)
            return;
        auto list = stmt_list();
        list.push_back_range(e().at(range));
        for (auto const id : list)
        {
            if (!is_known(e(), id))
                continue;
            auto const s = e().at(id);
            for_each_expr_of(s, [&](flat_expr_id x) { fold(x, 0); });
            for_each_pattern_of(e(), s,
                                [&](ast::range_of<flat_expr_id> patterns)
                                {
                                    if (is_known(e(), patterns))
                                        for (auto const p : e().at(patterns))
                                            fold(p, 0);
                                });
            if (auto const* const let = s.node.try_as<flat_let>();
                let != nullptr && is_known(e(), let->local) && !e().at(let->local).is_mut)
            {
                auto const& value = e().at(let->value).node;
                if (value.is<flat_int_literal>() || value.is<flat_bool_literal>() || value.is<flat_enum_value>()
                    || value.is<flat_literal>())
                    literal_of[index_of(let->local)] = let->value;
            }
            for_each_body_of(e(), s, [&](ast::range_of<flat_stmt_id> inner) { fold_statements(inner, depth + 1); });
        }
    }

    /// Whether `id` is a bool literal, and which.
    [[nodiscard]] cc::optional<bool> constant_condition(flat_expr_id id) const
    {
        if (!is_known(e(), id))
            return cc::nullopt;
        if (auto const* const b = e().at(id).node.try_as<flat_bool_literal>())
            return b->value;
        return cc::nullopt;
    }

    ast::range_of<flat_stmt_id> pruned_body(ast::range_of<flat_stmt_id> range, int depth)
    {
        auto list = stmt_list();
        if (is_known(e(), range))
            list.push_back_range(e().at(range));
        return out.stmt_list(pruned(list, depth + 1));
    }

    /// F2: an `if` on a constant is the side it takes, in its place, and a `while` on false is nothing.
    stmt_list pruned(stmt_list const& list, int depth)
    {
        auto result = stmt_list();
        for (auto const id : list)
        {
            if (!is_known(e(), id) || depth > k_max_depth)
            {
                result.push_back(id);
                continue;
            }
            auto copy = e().at(id);
            if (auto const* const i = copy.node.try_as<flat_if>())
                if (auto const taken = constant_condition(i->condition); taken.has_value())
                {
                    auto const body = taken.value() ? i->then_body : i->else_body;
                    auto inner = stmt_list();
                    if (is_known(e(), body))
                        inner.push_back_range(e().at(body));
                    result.push_back_range(pruned(inner, depth + 1));
                    continue;
                }
            if (auto const* const w = copy.node.try_as<flat_while>())
                if (auto const taken = constant_condition(w->condition); taken.has_value() && !taken.value())
                    continue;
            auto has_body = true;
            if (auto* const i = copy.node.try_as<flat_if>())
            {
                i->then_body = pruned_body(i->then_body, depth);
                i->else_body = pruned_body(i->else_body, depth);
            }
            else if (auto* const l = copy.node.try_as<flat_loop>())
                l->body = pruned_body(l->body, depth);
            else if (auto* const w = copy.node.try_as<flat_while>())
                w->body = pruned_body(w->body, depth);
            else if (auto* const f = copy.node.try_as<flat_for>())
                f->body = pruned_body(f->body, depth);
            else if (auto* const o = copy.node.try_as<flat_once>())
                o->body = pruned_body(o->body, depth);
            else if (auto* const sw = copy.node.try_as<flat_switch>())
            {
                auto arms = cc::vector<flat_arm>();
                if (is_known(e(), sw->arms))
                    arms.push_back_range(e().at(sw->arms));
                for (auto& arm : arms)
                    arm.body = pruned_body(arm.body, depth);
                sw->arms = out.arm_list(arms);
                sw->default_body = pruned_body(sw->default_body, depth);
            }
            else
                has_body = false;
            if (!has_body)
            {
                result.push_back(id);
                continue;
            }
            out.e.stmts.push_back(cc::move(copy));
            result.push_back(flat_stmt_id(out.e.stmts.size() - 1));
        }
        return result;
    }
};

struct let_simplifier
{
    flat_builder& out;
    /// Per local: whether anything but its declaration may give it a value.
    cc::vector<bool> written;
    /// Per local: the local L1 reads in its place; `none` for one that stays.
    cc::vector<local_id> replaced;
    /// Per statement of the tree as it was given: whether L1 or L2 takes it out.
    cc::vector<bool> dropped;
    bool drops_any = false;
    /// Per statement of the tree as it was given: what L4 writes in its place, empty where it stays.
    cc::vector<stmt_list> expanded;
    /// Per local: the lets L4 split it into, one per field; empty where it stays whole.
    cc::vector<cc::vector<local_id>> split_fields;
    /// Per local: whether a call writes through it or hands it on.
    cc::vector<bool> written_by_call;

    struct available
    {
        local_id local = local_id::none;
        flat_expr_id value = flat_expr_id::none;
        u64 hash = 0;
    };
    /// The lets in scope whose value L1 may reuse, innermost last, and where each hash's lets stand among them.
    cc::vector<available> in_scope;
    cc::map<u64, cc::vector<isize>> by_hash;

    [[nodiscard]] checked_module const& m() const { return out.m; }
    [[nodiscard]] flat_entry_point const& e() const { return out.e; }

    [[nodiscard]] local_id canonical(local_id id) const
    {
        while (is_known(e(), id) && is_valid(replaced[index_of(id)]))
            id = replaced[index_of(id)];
        return id;
    }

    void mark_written(local_id id)
    {
        if (is_known(e(), id))
            written[index_of(id)] = true;
    }

    [[nodiscard]] bool is_dropped(flat_stmt_id id) const
    {
        return is_valid(id) && index_of(id) < dropped.size() && dropped[index_of(id)];
    }

    void drop(flat_stmt_id id)
    {
        dropped[index_of(id)] = true;
        drops_any = true;
    }

    /// Every local something other than its declaration may change: an assignment's, a `var`'s, and one a call writes
    /// through or hands on; every node of the tree counts, reachable or not.
    void find_written()
    {
        written = cc::vector<bool>::create_filled(e().locals.size(), false);
        replaced = cc::vector<local_id>::create_filled(e().locals.size(), local_id::none);
        dropped = cc::vector<bool>::create_filled(e().stmts.size(), false);
        for (auto const& s : e().stmts)
        {
            if (auto const* const assign = s.node.try_as<flat_assign>())
                mark_written(root_local_of(e(), assign->place));
            else if (auto const* const var = s.node.try_as<flat_var>())
                mark_written(var->local);
            else if (auto const* const let = s.node.try_as<flat_let>(); let != nullptr && is_known(e(), let->value))
                if (auto const* const call = e().at(let->value).node.try_as<flat_call>())
                    if (auto const* const record = m().builtin_function(call->intrinsic);
                        record == nullptr || record->declares_only)
                        mark_written(let->local);
        }
        for (auto const& x : e().exprs)
        {
            auto const* const call = x.node.try_as<flat_call>();
            if (call == nullptr || !is_known(e(), call->arguments))
                continue;
            auto const* const record = m().builtin_function(call->intrinsic);
            auto const writes = !call->is_pure || record == nullptr || record->takes_element || record->is_atomic
                             || record->writes_through != builtins::written_argument::none;
            if (writes)
                for (auto const a : e().at(call->arguments))
                    mark_written(root_local_of(e(), a));
        }
    }

    [[nodiscard]] bool is_fixed(local_id id) const
    {
        if (!is_known(e(), id) || written[index_of(id)])
            return false;
        auto const& local = e().at(id);
        return local.kind != local_kind::var && !local.is_mut && !local.is_undefined;
    }

    /// A resource a call may read where its value stays the same: nothing the invocation may write.
    [[nodiscard]] bool is_read_only(type_id id) const
    {
        if (!is_valid(id) || index_of(id) >= m().types.size())
            return false;
        auto const& t = m().at(id);
        switch (t.kind)
        {
        case type_kind::buffer:
        case type_kind::bytes:
            return !t.is_mut;
        case type_kind::image:
            return t.access == access_mode::read;
        case type_kind::texture:
        case type_kind::sampler:
        case type_kind::acceleration_structure:
            return true;
        case type_kind::array:
            return is_read_only(t.element);
        case type_kind::atomic:
        case type_kind::stream:
        case type_kind::function:
            return false;
        default:
            return true;
        }
    }

    /// A call whose value depends on its arguments alone: no effect, no derivative, nothing that depends on which
    /// invocations run beside it, and no state of the invocation it reads without an argument naming it.
    [[nodiscard]] bool is_plain_call(flat_call const& call) const
    {
        auto const* const record = m().builtin_function(call.intrinsic);
        return call.is_pure && record != nullptr && !record->uses_derivatives && !record->is_barrier
            && !record->is_atomic && !record->is_image_store && !record->is_bytes_store
            && !record->is_subgroup_operation && !record->is_quad_operation && !record->is_uniform_load
            && !record->differs_between_invocations && !record->takes_element && !record->declares_only
            && record->writes_through == builtins::written_argument::none && !call.arguments.empty();
    }

    /// The same wherever the invocation evaluates it, as long as the locals it reads are in scope.
    [[nodiscard]] bool is_stable(flat_expr_id id, int depth) const
    {
        if (!is_known(e(), id) || depth > k_max_depth)
            return false;
        auto const& x = e().at(id);
        auto const operands_stable = [&]
        {
            auto result = true;
            for_each_operand(e(), x, [&](flat_expr_id operand) { result = result && is_stable(operand, depth + 1); });
            return result;
        };
        if (x.node.is<flat_literal>() || x.node.is<flat_int_literal>() || x.node.is<flat_bool_literal>()
            || x.node.is<flat_enum_value>() || x.node.is<flat_file_sampler>())
            return true;
        if (auto const* const ref = x.node.try_as<flat_local_ref>())
            return is_fixed(canonical(ref->local));
        if (auto const* const b = x.node.try_as<flat_binding_member>())
            return !b->is_workgroup;
        if (auto const* const element = x.node.try_as<flat_buffer_element>())
            return is_known(e(), element->buffer) && is_read_only(e().at(element->buffer).type) && operands_stable();
        if (auto const* const call = x.node.try_as<flat_call>())
        {
            if (!is_plain_call(*call) || !is_known(e(), call->arguments))
                return false;
            for (auto const a : e().at(call->arguments))
                if (!is_known(e(), a) || !is_read_only(e().at(a).type))
                    return false;
            return operands_stable();
        }
        if (x.node.is<flat_member>() || x.node.is<flat_element>() || x.node.is<flat_construct>()
            || x.node.is<flat_not>() || x.node.is<flat_and>() || x.node.is<flat_or>())
            return operands_stable();
        return false;
    }

    /// Whether `id` reads a local, a binding or a sampler; a value of literals alone is never reused, since the names
    /// of two lets that merely agree on a number say different things.
    [[nodiscard]] bool reads_something(flat_expr_id id, int depth) const
    {
        if (!is_known(e(), id) || depth > k_max_depth)
            return false;
        auto const& x = e().at(id);
        if (x.node.is<flat_local_ref>() || x.node.is<flat_binding_member>() || x.node.is<flat_file_sampler>())
            return true;
        auto result = false;
        for_each_operand(e(), x, [&](flat_expr_id operand) { result = result || reads_something(operand, depth + 1); });
        return result;
    }

    [[nodiscard]] u64 hash_of(flat_expr_id id, int depth) const
    {
        if (!is_known(e(), id) || depth > k_max_depth)
            return 0;
        auto const& x = e().at(id);
        auto h = cc::make_hash(u64(index_of(x.type)), u64(x.node.index()));
        x.node.visit([&](flat_literal const& n) { h = cc::make_hash(h, cc::bit_cast<u64>(n.value)); },
                     [&](flat_int_literal const& n) { h = cc::make_hash(h, u64(u32(n.value)), n.is_unsigned); },
                     [&](flat_bool_literal const& n) { h = cc::make_hash(h, n.value); },
                     [&](flat_enum_value const& n) { h = cc::make_hash(h, n.case_index); },
                     [&](flat_local_ref const& n) { h = cc::make_hash(h, u64(index_of(canonical(n.local)))); },
                     [&](flat_binding_member const& n) { h = cc::make_hash(h, u64(index_of(n.binding)), n.member); },
                     [&](flat_file_sampler const& n) { h = cc::make_hash(h, u64(index_of(n.sampler))); },
                     [&](flat_member const& n) { h = cc::make_hash(h, n.member, n.letters.count); },
                     [&](flat_call const& n) { h = cc::make_hash(h, u64(index_of(n.intrinsic))); }, [&](auto const&) {});
        for_each_operand(e(), x, [&](flat_expr_id operand) { h = cc::make_hash(h, hash_of(operand, depth + 1)); });
        return h;
    }

    [[nodiscard]] bool same_operands(flat_expr const& a, flat_expr const& b, int depth) const
    {
        auto lhs = cc::vector<flat_expr_id>();
        auto rhs = cc::vector<flat_expr_id>();
        for_each_operand(e(), a, [&](flat_expr_id operand) { lhs.push_back(operand); });
        for_each_operand(e(), b, [&](flat_expr_id operand) { rhs.push_back(operand); });
        if (lhs.size() != rhs.size())
            return false;
        for (auto i = isize(0); i < lhs.size(); ++i)
            if (!is_same(lhs[i], rhs[i], depth + 1))
                return false;
        return true;
    }

    /// Whether two stable expressions give the same value: the same tree, read through what L1 replaced.
    [[nodiscard]] bool is_same(flat_expr_id a, flat_expr_id b, int depth) const
    {
        if (a == b)
            return true;
        if (!is_known(e(), a) || !is_known(e(), b) || depth > k_max_depth)
            return false;
        auto const& x = e().at(a);
        auto const& y = e().at(b);
        if (x.type != y.type || x.node.index() != y.node.index())
            return false;
        auto equal = false;
        x.node.visit(
            [&](flat_literal const& n)
            { equal = cc::bit_cast<u64>(n.value) == cc::bit_cast<u64>(y.node.as<flat_literal>().value); },
            [&](flat_int_literal const& n) { equal = n == y.node.as<flat_int_literal>(); },
            [&](flat_bool_literal const& n) { equal = n == y.node.as<flat_bool_literal>(); },
            [&](flat_enum_value const& n) { equal = n == y.node.as<flat_enum_value>(); }, [&](flat_local_ref const& n)
            { equal = canonical(n.local) == canonical(y.node.as<flat_local_ref>().local); },
            [&](flat_binding_member const& n) { equal = n == y.node.as<flat_binding_member>(); },
            [&](flat_file_sampler const& n) { equal = n == y.node.as<flat_file_sampler>(); },
            [&](flat_member const& n)
            {
                auto const& other = y.node.as<flat_member>();
                equal = n.member == other.member && n.letters == other.letters && same_operands(x, y, depth);
            },
            [&](flat_call const& n)
            { equal = n.intrinsic == y.node.as<flat_call>().intrinsic && same_operands(x, y, depth); },
            [&](flat_element const&) { equal = same_operands(x, y, depth); }, [&](flat_buffer_element const&)
            { equal = same_operands(x, y, depth); }, [&](flat_construct const&) { equal = same_operands(x, y, depth); },
            [&](flat_not const&) { equal = same_operands(x, y, depth); },
            [&](flat_and const&) { equal = same_operands(x, y, depth); },
            [&](flat_or const&) { equal = same_operands(x, y, depth); }, [&](auto const&) { equal = false; });
        return equal;
    }

    /// L1 for one let: true when it repeats a let in scope, which its local then reads instead.
    bool reuse(flat_let const& let)
    {
        if (!is_fixed(let.local) || !is_stable(let.value, 0) || !reads_something(let.value, 0))
            return false;
        auto const hash = hash_of(let.value, 0);
        auto& candidates = by_hash[hash];
        for (auto const at : candidates)
        {
            auto const& earlier = in_scope[at];
            if (e().at(earlier.local).type == e().at(let.local).type && is_same(earlier.value, let.value, 0))
            {
                replaced[index_of(let.local)] = earlier.local;
                return true;
            }
        }
        candidates.push_back(in_scope.size());
        in_scope.push_back({.local = let.local, .value = let.value, .hash = hash});
        return false;
    }

    /// Leaves the scope of a body: the lets it declared are no longer available.
    void close_scope(isize mark)
    {
        while (in_scope.size() > mark)
        {
            by_hash[in_scope.back().hash].remove_back();
            in_scope.remove_back();
        }
    }

    void scan_body(ast::range_of<flat_stmt_id> range, int depth)
    {
        if (!is_known(e(), range) || depth > k_max_depth)
            return;
        auto const mark = in_scope.size();
        auto list = stmt_list();
        for (auto const id : e().at(range))
            list.push_back(id);
        for (auto const id : list)
        {
            if (!is_known(e(), id))
                continue;
            auto const& s = e().at(id);
            if (auto const* const let = s.node.try_as<flat_let>(); let != nullptr && reuse(*let))
            {
                drop(id);
                continue;
            }
            auto bodies = cc::vector<ast::range_of<flat_stmt_id>>();
            for_each_body_of(e(), s, [&](ast::range_of<flat_stmt_id> inner) { bodies.push_back(inner); });
            for (auto const inner : bodies)
                scan_body(inner, depth + 1);
        }
        close_scope(mark);
    }

    /// Whether `id` writes `local` anywhere in it, as an assignment's place or a call that writes through it.
    [[nodiscard]] bool writes_local(flat_stmt_id id, local_id local, int depth) const
    {
        if (!is_known(e(), id) || depth > k_max_depth)
            return true;
        auto const& s = e().at(id);
        if (auto const* const assign = s.node.try_as<flat_assign>();
            assign != nullptr && root_local_of(e(), assign->place) == local)
            return true;
        auto writes = false;
        for_each_body_of(e(), s,
                         [&](ast::range_of<flat_stmt_id> inner)
                         {
                             if (is_known(e(), inner))
                                 for (auto const n : e().at(inner))
                                     writes = writes || writes_local(n, local, depth + 1);
                         });
        return writes;
    }

    /// How often `local` is read, a place of an assignment not among them, in what `range` holds.
    void count_value_reads(flat_expr_id id, local_id local, isize& reads, int depth) const
    {
        if (!is_known(e(), id) || depth > k_max_depth)
            return;
        auto const& x = e().at(id);
        if (auto const* const ref = x.node.try_as<flat_local_ref>(); ref != nullptr && ref->local == local)
            ++reads;
        for_each_operand(e(), x, [&](flat_expr_id operand) { count_value_reads(operand, local, reads, depth + 1); });
    }

    void count_value_reads(ast::range_of<flat_stmt_id> range, local_id local, isize& reads, int depth) const
    {
        if (!is_known(e(), range) || depth > k_max_depth)
            return;
        for (auto const id : e().at(range))
        {
            if (!is_known(e(), id))
                continue;
            auto const& s = e().at(id);
            if (auto const* const assign = s.node.try_as<flat_assign>())
            {
                // a place is written, and only its indices are read
                auto at = assign->place;
                for (auto step = 0; is_known(e(), at) && step < k_max_depth; ++step)
                {
                    auto const& node = e().at(at).node;
                    if (auto const* const element = node.try_as<flat_element>())
                    {
                        count_value_reads(element->index, local, reads, depth + 1);
                        at = element->object;
                    }
                    else if (auto const* const member = node.try_as<flat_member>())
                        at = member->object;
                    else
                    {
                        if (!node.is<flat_local_ref>())
                            count_value_reads(at, local, reads, depth + 1);
                        break;
                    }
                }
                count_value_reads(assign->value, local, reads, depth + 1);
            }
            else
                for_each_expr_of(s, [&](flat_expr_id x) { count_value_reads(x, local, reads, depth + 1); });
            for_each_pattern_of(e(), s,
                                [&](ast::range_of<flat_expr_id> patterns)
                                {
                                    if (is_known(e(), patterns))
                                        for (auto const p : e().at(patterns))
                                            count_value_reads(p, local, reads, depth + 1);
                                });
            for_each_body_of(
                e(), s, [&](ast::range_of<flat_stmt_id> inner) { count_value_reads(inner, local, reads, depth + 1); });
        }
    }

    /// Whether the `end` of a `for` in `range` reads `local`, which may then not become a `var` (LEGAL-7).
    [[nodiscard]] bool bounds_a_loop(ast::range_of<flat_stmt_id> range, local_id local, int depth) const
    {
        if (!is_known(e(), range) || depth > k_max_depth)
            return true;
        auto bounds = false;
        for (auto const id : e().at(range))
        {
            if (!is_known(e(), id) || bounds)
                continue;
            auto const& s = e().at(id);
            if (auto const* const f = s.node.try_as<flat_for>())
            {
                auto reads = isize(0);
                count_value_reads(f->end, local, reads, 0);
                bounds = reads > 0;
            }
            for_each_body_of(e(), s, [&](ast::range_of<flat_stmt_id> inner)
                             { bounds = bounds || bounds_a_loop(inner, local, depth + 1); });
        }
        return bounds;
    }

    /// L3: a `let` whose value is a block's result, the temporary `var` E1 declared in the same list, is that `var`
    /// under the let's name, where nothing else reads the `var` and nothing writes it after the let.
    void merge_results(ast::range_of<flat_stmt_id> range, ast::range_of<flat_stmt_id> whole, int depth)
    {
        if (!is_known(e(), range) || depth > k_max_depth)
            return;
        auto list = stmt_list();
        list.push_back_range(e().at(range));
        auto declared = cc::vector<local_id>();
        for (auto i = isize(0); i < list.size(); ++i)
        {
            if (!is_known(e(), list[i]))
                continue;
            auto const& s = e().at(list[i]);
            // a block's result, or a var a block fills, such as `T[N].filled(v)`'s
            if (auto const* const var = s.node.try_as<flat_var>();
                var != nullptr && !is_valid(var->value) && is_known(e(), var->local)
                && (e().at(var->local).kind == local_kind::temporary || e().at(var->local).kind == local_kind::var))
                declared.push_back(var->local);
            for_each_body_of(e(), s, [&](ast::range_of<flat_stmt_id> inner) { merge_results(inner, whole, depth + 1); });

            // A `let`, or a `var` given that value, which then fills the block's storage in place: `T[N].filled(v)`.
            auto local = local_id::none;
            auto value = flat_expr_id::none;
            if (auto const* const let = s.node.try_as<flat_let>();
                let != nullptr && is_known(e(), let->local) && e().at(let->local).kind == local_kind::let)
            {
                local = let->local;
                value = let->value;
            }
            else if (auto const* const v = s.node.try_as<flat_var>(); v != nullptr && is_known(e(), v->local)
                                                                      && is_valid(v->value)
                                                                      && e().at(v->local).kind == local_kind::var)
            {
                local = v->local;
                value = v->value;
            }
            if (!is_valid(local) || !is_known(e(), value))
                continue;
            auto const* const ref = e().at(value).node.try_as<flat_local_ref>();
            auto is_declared_here = false;
            for (auto const d : declared)
                is_declared_here = is_declared_here || (ref != nullptr && d == ref->local);
            if (!is_declared_here || e().at(ref->local).type != e().at(local).type)
                continue;
            auto reads = isize(0);
            count_value_reads(whole, ref->local, reads, 0);
            auto is_written_after = false;
            for (auto k = i + 1; k < list.size() && !is_written_after; ++k)
                is_written_after = writes_local(list[k], ref->local, 0);
            if (reads != 1 || is_written_after || bounds_a_loop(whole, local, 0))
                continue;
            replaced[index_of(local)] = ref->local;
            out.e.locals[index_of(ref->local)].name = e().at(local).name;
            drop(list[i]);
        }
    }

    /// Every local read through what L1 replaced.
    void apply_replacements()
    {
        for (auto& x : out.e.exprs)
            if (auto* const ref = x.node.try_as<flat_local_ref>())
                ref->local = canonical(ref->local);
    }

    /// Whether evaluating `id` may have no value at all (EVAL-84, EVAL-90), which a let L2 keeps must still reach.
    [[nodiscard]] bool may_be_undefined(flat_expr_id id, int depth) const
    {
        if (!is_known(e(), id) || depth > k_max_depth)
            return true;
        auto const& x = e().at(id);
        if (x.node.is<flat_element>() || x.node.is<flat_buffer_element>())
            return true;
        if (auto const* const call = x.node.try_as<flat_call>())
        {
            auto const* const record = m().builtin_function(call->intrinsic);
            if (record == nullptr || record->undefined_when != nullptr || record->bytes_words > 0)
                return true;
        }
        auto result = false;
        for_each_operand(e(), x, [&](flat_expr_id operand) { result = result || may_be_undefined(operand, depth + 1); });
        return result;
    }

    /// Adds `step` to the reads of every local `id` reads.
    void count_reads_of(flat_expr_id id, cc::vector<i32>& reads, i32 step, int depth) const
    {
        if (!is_known(e(), id) || depth > k_max_depth)
            return;
        auto const& x = e().at(id);
        if (auto const* const ref = x.node.try_as<flat_local_ref>(); ref != nullptr && is_known(e(), ref->local))
            reads[index_of(ref->local)] += step;
        for_each_operand(e(), x, [&](flat_expr_id operand) { count_reads_of(operand, reads, step, depth + 1); });
    }

    /// The lets reachable from `range` that L1 kept, and how often each local is read there.
    void count_reads(ast::range_of<flat_stmt_id> range, cc::vector<i32>& reads, cc::vector<flat_stmt_id>& lets, int depth) const
    {
        if (!is_known(e(), range) || depth > k_max_depth)
            return;
        for (auto const id : e().at(range))
        {
            if (!is_known(e(), id) || is_dropped(id))
                continue;
            auto const& s = e().at(id);
            if (s.node.is<flat_let>())
                lets.push_back(id);
            for_each_expr_of(s, [&](flat_expr_id x) { count_reads_of(x, reads, 1, 0); });
            for_each_pattern_of(e(), s,
                                [&](ast::range_of<flat_expr_id> patterns)
                                {
                                    if (is_known(e(), patterns))
                                        for (auto const p : e().at(patterns))
                                            count_reads_of(p, reads, 1, 0);
                                });
            for_each_body_of(e(), s,
                             [&](ast::range_of<flat_stmt_id> inner) { count_reads(inner, reads, lets, depth + 1); });
        }
    }

    /// Calls `visit` on `id` and every operand below it.
    template <class Visit>
    void visit_expr(flat_expr_id id, Visit& visit, int depth) const
    {
        if (!is_known(e(), id) || depth > k_max_depth)
            return;
        visit(id);
        for_each_operand(e(), e().at(id), [&](flat_expr_id operand) { visit_expr(operand, visit, depth + 1); });
    }

    /// Calls `visit` on every expression the statements of `range` that stay hold, operands included.
    template <class Visit>
    void visit_live(ast::range_of<flat_stmt_id> range, Visit&& visit, int depth) const
    {
        if (!is_known(e(), range) || depth > k_max_depth)
            return;
        for (auto const id : e().at(range))
        {
            if (!is_known(e(), id) || is_dropped(id))
                continue;
            auto const& s = e().at(id);
            for_each_expr_of(s, [&](flat_expr_id x) { visit_expr(x, visit, 0); });
            for_each_pattern_of(e(), s,
                                [&](ast::range_of<flat_expr_id> patterns)
                                {
                                    if (is_known(e(), patterns))
                                        for (auto const p : e().at(patterns))
                                            visit_expr(p, visit, 0);
                                });
            for_each_body_of(e(), s, [&](ast::range_of<flat_stmt_id> inner) { visit_live(inner, visit, depth + 1); });
        }
    }

    /// Per local of a live tree: how often it is named, how often as an assignment's whole place, how often as the
    /// object of a field read, and how often it is assigned.
    struct uses
    {
        cc::vector<i32> refs;
        cc::vector<i32> places;
        cc::vector<i32> field_reads;
        cc::vector<i32> assigns;

        [[nodiscard]] static uses create(isize locals)
        {
            return {.refs = cc::vector<i32>::create_filled(locals, 0),
                    .places = cc::vector<i32>::create_filled(locals, 0),
                    .field_reads = cc::vector<i32>::create_filled(locals, 0),
                    .assigns = cc::vector<i32>::create_filled(locals, 0)};
        }
    };

    void count_assigns(ast::range_of<flat_stmt_id> range, uses& u, int depth) const
    {
        if (!is_known(e(), range) || depth > k_max_depth)
            return;
        for (auto const id : e().at(range))
        {
            if (!is_known(e(), id) || is_dropped(id))
                continue;
            auto const& s = e().at(id);
            if (auto const* const assign = s.node.try_as<flat_assign>())
            {
                if (auto const root = root_local_of(e(), assign->place); is_known(e(), root))
                    ++u.assigns[index_of(root)];
                if (auto const* const ref = e().at(assign->place).node.try_as<flat_local_ref>();
                    ref != nullptr && is_known(e(), ref->local))
                    ++u.places[index_of(ref->local)];
            }
            for_each_body_of(e(), s, [&](ast::range_of<flat_stmt_id> inner) { count_assigns(inner, u, depth + 1); });
        }
    }

    void count_uses(ast::range_of<flat_stmt_id> range, uses& u) const
    {
        count_assigns(range, u, 0);
        visit_live(
            range,
            [&](flat_expr_id id)
            {
                auto const& node = e().at(id).node;
                if (auto const* const ref = node.try_as<flat_local_ref>(); ref != nullptr && is_known(e(), ref->local))
                    ++u.refs[index_of(ref->local)];
                else if (auto const* const member = node.try_as<flat_member>();
                         member != nullptr && !member->is_swizzle() && is_known(e(), member->object))
                    if (auto const* const object = e().at(member->object).node.try_as<flat_local_ref>();
                        object != nullptr && is_known(e(), object->local))
                        ++u.field_reads[index_of(object->local)];
            },
            0);
    }

    /// The fields of the struct `construct` builds, where `local` may be split into them: a struct whose every field
    /// has the type of the value building it.
    [[nodiscard]] cc::optional<ast::range_of<member_info>> fields_of(local_id local, flat_expr_id construct) const
    {
        auto const type = e().at(local).type;
        auto const* const c = e().at(construct).node.try_as<flat_construct>();
        if (c == nullptr || !is_valid(type) || index_of(type) >= m().types.size()
            || m().at(type).kind != type_kind::structure || !is_known(e(), c->arguments))
            return cc::nullopt;
        auto const members = m().at(type).members;
        auto const arguments = e().at(c->arguments);
        if (m().at(members).size() != arguments.size())
            return cc::nullopt;
        for (auto i = isize(0); i < arguments.size(); ++i)
            if (e().at(arguments[i]).type != m().at(members)[i].type)
                return cc::nullopt;
        return members;
    }

    /// Replaces the statement `at`, which builds `local` from `construct`, by one let per field.
    void split_at(flat_stmt_id at, local_id local, flat_expr_id construct, ast::range_of<member_info> members)
    {
        auto arguments = cc::vector<flat_expr_id>();
        arguments.push_back_range(e().at(e().at(construct).node.as<flat_construct>().arguments));
        auto fields = cc::vector<local_id>();
        auto lets = stmt_list();
        for (auto i = isize(0); i < arguments.size(); ++i)
        {
            // a field built from a local nothing writes is that local
            if (auto const* const ref = e().at(arguments[i]).node.try_as<flat_local_ref>();
                ref != nullptr && is_fixed(ref->local))
            {
                fields.push_back(ref->local);
                continue;
            }
            auto const name = cc::string(m().at(members)[i].name);
            auto const type = m().at(members)[i].type;
            auto const field = out.add_local(local_kind::let, name, type);
            fields.push_back(field);
            lets.push_back(out.let(field, arguments[i]));
        }
        split_fields[index_of(local)] = cc::move(fields);
        expanded[index_of(at)] = cc::move(lets);
        drops_any = true;
    }

    /// Splits what `range` holds that `u` allows, at any depth.
    void split_in(ast::range_of<flat_stmt_id> range, uses const& u, int depth)
    {
        if (!is_known(e(), range) || depth > k_max_depth)
            return;
        auto list = stmt_list();
        list.push_back_range(e().at(range));
        auto const only_fields_read = [&](local_id local)
        {
            auto const at = index_of(local);
            return !written_by_call[at] && u.field_reads[at] > 0 && u.refs[at] - u.places[at] == u.field_reads[at];
        };
        for (auto i = isize(0); i < list.size(); ++i)
        {
            if (!is_known(e(), list[i]) || is_dropped(list[i]))
                continue;
            // A copy: the recursion below adds statements, which can move the one a reference would point at.
            auto const s = e().at(list[i]);
            for_each_body_of(e(), s, [&](ast::range_of<flat_stmt_id> inner) { split_in(inner, u, depth + 1); });

            // a let of a construction
            if (auto const* const let = s.node.try_as<flat_let>(); let != nullptr)
            {
                if (is_known(e(), let->local) && is_known(e(), let->value) && !e().at(let->local).is_mut
                    && u.assigns[index_of(let->local)] == 0 && only_fields_read(let->local))
                    if (auto const members = fields_of(let->local, let->value); members.has_value())
                        split_at(list[i], let->local, let->value, members.value());
                continue;
            }

            // a var given its one value by a construction later in the same list, and named nowhere before that
            auto const* const var = s.node.try_as<flat_var>();
            if (var == nullptr || is_valid(var->value) || !is_known(e(), var->local)
                || u.assigns[index_of(var->local)] != 1 || !only_fields_read(var->local))
                continue;
            for (auto k = i + 1; k < list.size(); ++k)
            {
                if (!is_known(e(), list[k]))
                    break;
                auto const* const assign = e().at(list[k]).node.try_as<flat_assign>();
                auto const* const place
                    = assign != nullptr ? e().at(assign->place).node.try_as<flat_local_ref>() : nullptr;
                if (place != nullptr && place->local == var->local)
                {
                    if (auto const members = fields_of(var->local, assign->value); members.has_value())
                    {
                        split_at(list[k], var->local, assign->value, members.value());
                        drop(list[i]);
                    }
                    break;
                }
                auto between = uses::create(e().locals.size());
                count_uses(out.stmt_list(cc::span<flat_stmt_id const>(&list[k], 1)), between);
                if (between.refs[index_of(var->local)] > 0 || between.assigns[index_of(var->local)] > 0)
                    break;
            }
        }
    }

    /// L4: a struct a let or a block's result var holds, read only field by field, is one let per field where it was
    /// built, in the same order; each field read reads its field's let, and L2 then drops the fields nothing reads.
    /// Every value stays where it was computed, so nothing that changes between the construction and a read matters.
    void split_constructions(stmt_list const& body)
    {
        auto const range = out.stmt_list(body);
        auto const n = e().locals.size();
        auto u = uses::create(n);
        count_uses(range, u);
        written_by_call = cc::vector<bool>::create_filled(n, false);
        for (auto const& x : e().exprs)
        {
            auto const* const call = x.node.try_as<flat_call>();
            if (call == nullptr || !is_known(e(), call->arguments))
                continue;
            auto const* const record = m().builtin_function(call->intrinsic);
            if (!call->is_pure || record == nullptr || record->takes_element || record->is_atomic
                || record->writes_through != builtins::written_argument::none)
                for (auto const a : e().at(call->arguments))
                    if (auto const root = root_local_of(e(), a); is_known(e(), root))
                        written_by_call[index_of(root)] = true;
        }
        split_fields = cc::vector<cc::vector<local_id>>::create_defaulted(n);
        expanded = cc::vector<stmt_list>::create_defaulted(e().stmts.size());
        split_in(range, u, 0);

        auto reads = cc::vector<flat_expr_id>();
        visit_live(
            range,
            [&](flat_expr_id id)
            {
                auto const* const member = e().at(id).node.try_as<flat_member>();
                if (member == nullptr || member->is_swizzle() || !is_known(e(), member->object))
                    return;
                if (auto const* const object = e().at(member->object).node.try_as<flat_local_ref>();
                    object != nullptr && is_known(e(), object->local) && index_of(object->local) < split_fields.size()
                    && !split_fields[index_of(object->local)].empty())
                    reads.push_back(id);
            },
            0);
        for (auto const id : reads)
        {
            auto const& member = e().at(id).node.as<flat_member>();
            auto const local = e().at(member.object).node.as<flat_local_ref>().local;
            auto const field = split_fields[index_of(local)][member.member];
            out.e.exprs[index_of(id)].node = flat_local_ref{.local = field};
        }
    }

    /// `has_effect`, but for a call of a builtin that only reads, which an unread value lets go.
    [[nodiscard]] bool has_effect_here(flat_expr_id id, int depth) const
    {
        if (!is_known(e(), id) || depth > k_max_depth)
            return true;
        auto const& x = e().at(id);
        if (x.node.is<flat_block>() || x.node.is<flat_invalid>())
            return true;
        if (auto const* const call = x.node.try_as<flat_call>(); call != nullptr && !call->is_pure)
        {
            auto const* const record = m().builtin_function(call->intrinsic);
            if (record == nullptr || !record->reads_only)
                return true;
        }
        auto result = false;
        for_each_operand(e(), x, [&](flat_expr_id operand) { result = result || has_effect_here(operand, depth + 1); });
        return result;
    }

    /// L2: a let nothing reads goes, with what it computed, until every let left is read or has an effect.
    void drop_unread(stmt_list const& body)
    {
        auto reads = cc::vector<i32>::create_filled(e().locals.size(), 0);
        auto lets = cc::vector<flat_stmt_id>();
        auto const range = out.stmt_list(body);
        count_reads(range, reads, lets, 0);
        for (auto changed = true; changed;)
        {
            changed = false;
            for (auto const id : lets)
            {
                if (is_dropped(id))
                    continue;
                auto const& let = e().at(id).node.as<flat_let>();
                if (!is_known(e(), let.local) || reads[index_of(let.local)] > 0 || written[index_of(let.local)]
                    || has_effect_here(let.value, 0) || may_be_undefined(let.value, 0))
                    continue;
                drop(id);
                count_reads_of(let.value, reads, -1, 0);
                changed = true;
            }
        }
    }

    ast::range_of<flat_stmt_id> rebuilt_body(ast::range_of<flat_stmt_id> range, int depth)
    {
        auto list = stmt_list();
        if (is_known(e(), range))
            for (auto const id : e().at(range))
                list.push_back(id);
        return out.stmt_list(rebuilt(list, depth + 1));
    }

    ast::range_of<flat_arm> rebuilt_arms(ast::range_of<flat_arm> range, int depth)
    {
        auto list = cc::vector<flat_arm>();
        if (is_known(e(), range))
            for (auto const& arm : e().at(range))
                list.push_back(arm);
        for (auto& arm : list)
            arm.body = rebuilt_body(arm.body, depth);
        return out.arm_list(list);
    }

    /// `list` without what was dropped, at any depth.
    stmt_list rebuilt(stmt_list const& list, int depth)
    {
        auto result = stmt_list();
        for (auto const id : list)
        {
            if (is_dropped(id))
                continue;
            if (is_valid(id) && index_of(id) < expanded.size() && !expanded[index_of(id)].empty())
            {
                result.push_back_range(expanded[index_of(id)]);
                continue;
            }
            if (!is_known(e(), id) || depth > k_max_depth)
            {
                result.push_back(id);
                continue;
            }
            auto copy = e().at(id);
            auto has_body = true;
            if (auto* const i = copy.node.try_as<flat_if>())
            {
                i->then_body = rebuilt_body(i->then_body, depth);
                i->else_body = rebuilt_body(i->else_body, depth);
            }
            else if (auto* const l = copy.node.try_as<flat_loop>())
                l->body = rebuilt_body(l->body, depth);
            else if (auto* const w = copy.node.try_as<flat_while>())
                w->body = rebuilt_body(w->body, depth);
            else if (auto* const f = copy.node.try_as<flat_for>())
                f->body = rebuilt_body(f->body, depth);
            else if (auto* const o = copy.node.try_as<flat_once>())
                o->body = rebuilt_body(o->body, depth);
            else if (auto* const sw = copy.node.try_as<flat_switch>())
            {
                sw->arms = rebuilt_arms(sw->arms, depth);
                sw->default_body = rebuilt_body(sw->default_body, depth);
            }
            else
                has_body = false;
            if (!has_body)
            {
                result.push_back(id);
                continue;
            }
            out.e.stmts.push_back(cc::move(copy));
            result.push_back(flat_stmt_id(out.e.stmts.size() - 1));
        }
        return result;
    }
};
} // namespace

stmt_list sgl::check::impl::simplify_lets(flat_builder& out, stmt_list const& unfolded)
{
    auto folder = constant_folder{
        .out = out,
        .literal_of = cc::vector<flat_expr_id>::create_filled(out.e.locals.size(), flat_expr_id::none)};
    folder.fold_statements(out.stmt_list(unfolded), 0);
    auto const body = folder.pruned(unfolded, 0);

    auto s = let_simplifier{.out = out};
    s.find_written();
    auto const whole = out.stmt_list(body);
    s.merge_results(whole, whole, 0);
    s.scan_body(whole, 0);
    s.apply_replacements();
    s.drop_unread(body);
    auto const simplified = s.drops_any ? s.rebuilt(body, 0) : body;

    // L4 after L3, whose merge is what leaves a block's result a var, and L2 again over the fields it made.
    auto splitter = let_simplifier{.out = out};
    splitter.find_written();
    splitter.split_constructions(simplified);
    if (!splitter.drops_any)
        return simplified;
    auto const split = splitter.rebuilt(simplified, 0);
    auto again = let_simplifier{.out = out};
    again.find_written();
    again.drop_unread(split);
    return again.drops_any ? again.rebuilt(split, 0) : split;
}
