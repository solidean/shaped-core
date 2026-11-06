#include <clean-core/container/map.hh>
#include <clean-core/container/set.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics-language/check/impl/flattener.hh>

// PROTOTYPE of libs/graphics/shaped-graphics-language/docs/spec/incubator/autodiff.md.
// `fwd_diff` and `bwd_diff`, written as a transform of the flat tree.
//
// Both modes start alike.
// The function is inlined over locals that hold its arguments, and the body is LOWERED: every block that stands in an
// expression is lifted out into a statement block that assigns a local, so no statement's value holds a block.
// `derive` then writes the tangent of a value as linear operations over primal values.
//
// Forward mode writes the tangent of every statement beside it.
// Reverse mode unrolls every loop of constant bounds, hoists every declaration, turns every early leave into a flag and
// records every condition, so the body runs as structured code in which each statement runs at most once.
// Its backward sweep walks that code in reverse: it restores each value a statement overwrote, and TRANSPOSES the
// statement's tangent, which pushes the result's adjoint onto the adjoints of what the statement read.
//
// Every node a pass writes is attributed to the call of `fwd_diff` or `bwd_diff`.
// A node is placed in the tree at most once: a value read twice is `share`d, and each read is a `use` of it.

namespace sgl::check::impl
{
namespace
{
/// One step of a place below its root: a field of a struct or vector, or an element of an array.
struct place_step
{
    i32 member = -1;
    /// For an element; shared, so every copy of the place reads the same index.
    flat_expr_id index = flat_expr_id::none;
};

/// A value and its tangent, each to be placed once; a tangent of `none` is zero.
struct dual
{
    flat_expr_id primal = flat_expr_id::none;
    flat_expr_id tangent = flat_expr_id::none;
};

struct differentiator
{
    flattener& f;
    bool is_reverse = false;
    /// The call every written node is attributed to.
    ast::expr_id at = ast::expr_id::none;
    origin where;
    bool is_failed = false;

    /// Forward: the tangent local of each primal local the derivative follows.
    cc::map<i32, local_id> tangent_of;

    /// Reverse: the primal locals whose adjoints the backward sweep carries.
    cc::set<i32> tracked;
    /// Reverse: the stand-in a tangent expression names for a tracked local, never declared, and the local behind it.
    cc::map<i32, local_id> placeholder_of;
    cc::map<i32, local_id> primal_of_placeholder;
    /// Reverse: a tangent bound once and read several times, as a local that is never declared but transposed through.
    cc::map<i32, flat_expr_id> linear_definition;
    cc::vector<local_id> linear_lets;
    /// Reverse: placeholders and linear lets, which is what a linear expression is linear in.
    cc::set<i32> linear_locals;
    cc::map<i32, bool> linear_memo;
    /// Reverse: the adjoint of a tracked local or a linear let.
    cc::map<i32, local_id> adjoint_of;
    cc::vector<flat_stmt_id> adjoint_declarations;
    /// Reverse: the locals that were `let`s, which no statement overwrites.
    cc::set<i32> single_assigned;
    /// Reverse: the locals the pass adds to steer the sweeps (saves, flags, conditions), which have no adjoint.
    cc::set<i32> bookkeeping;
    /// Reverse: the save a statement that overwrites a local made of it first.
    cc::map<i32, flat_expr_id> save_of;
    /// Reverse: a loop of constant bounds kept as a loop, around the statement being straightened, innermost last.
    /// What a statement inside saves or records is a tape with one slot per iteration of all of them.
    struct kept_loop
    {
        local_id index = local_id::none;
        i32 first = 0;
        i32 count = 0;
    };
    cc::vector<kept_loop> kept_loops;
    /// Reverse: how many loops the declarations being hoisted stand in; a `let` in one is assigned once per iteration.
    i32 loop_depth = 0;
    /// Reverse: a local a clone reads in place of another: a loop's index, going backward, its reversed twin.
    cc::map<i32, local_id> substitution;
    /// The most slots a tape holds, which bounds what a kept loop keeps per value it overwrites.
    static constexpr i32 k_max_tape = 1024;
    /// Reverse: the flag a leave of a label sets.
    cc::map<i32, local_id> left_flag;
    /// Reverse: every declaration hoisted to the top of the body.
    cc::vector<flat_stmt_id> hoisted;

    /// The local a lifted block's value is assigned to, by the block's label.
    cc::map<i32, local_id> lifted;

    [[nodiscard]] checked_module const& m() const { return f.c.out; }
    [[nodiscard]] flat_expr const& node(flat_expr_id id) const { return f.entry.at(id); }
    [[nodiscard]] type_id type_of(flat_expr_id id) const { return f.entry.at(id).type; }

    // ---- failing --------------------------------------------------------------------------------------------------

    flat_expr_id failed(cc::string_view why)
    {
        if (!is_failed)
            f.failure_detail = cc::format("{}: {}", is_reverse ? "bwd_diff" : "fwd_diff", why);
        is_failed = true;
        f.mark_failed();
        return flat_expr_id::none;
    }

    // ---- types ----------------------------------------------------------------------------------------------------

    [[nodiscard]] builtins::type_record const* record_of(type_id t) const { return m().builtin_type_of(t); }

    [[nodiscard]] static bool is_float_kind(value_kind k)
    {
        return k == value_kind::scalar_float || k == value_kind::scalar_half;
    }

    [[nodiscard]] bool is_differentiable(type_id t) const { return f.c.is_differentiable_type(t); }
    /// What a change of a value of `t` is: a position's is a direction.
    [[nodiscard]] type_id tangent(type_id t) const { return f.c.tangent_type(t); }

    [[nodiscard]] i32 leaf_count(type_id t) const
    {
        auto const* const r = record_of(t);
        return r != nullptr ? r->leaf_count : -1;
    }
    [[nodiscard]] bool is_scalar(type_id t) const { return leaf_count(t) == 1; }
    [[nodiscard]] bool is_vector(type_id t) const { return leaf_count(t) > 1 && m().at(t).members.count > 0; }
    [[nodiscard]] bool is_matrix(type_id t) const { return leaf_count(t) > 1 && m().at(t).members.count == 0; }

    /// The type of a prelude struct named `name`, such as `float`.
    [[nodiscard]] type_id named_type(cc::string_view name) const
    {
        auto const* const found = f.c.prelude_names.get_ptr(cc::string(name));
        if (found == nullptr)
            return type_id::none;
        for (auto const s : *found)
            if (m().at(s).kind == symbol_kind::structure)
                return m().at(s).type;
        return type_id::none;
    }

    /// The scalar a value of `t` is made of: itself, a vector's field, and `float` for a matrix.
    [[nodiscard]] type_id scalar_of(type_id t) const
    {
        if (is_scalar(t))
            return t;
        if (is_vector(t))
            return f.field_type(t, 0);
        return named_type(builtins::k_float);
    }

    /// The scalars of a value of `t` that is `v` in every float and zero, the first case, or false everywhere else.
    bool leaves_of(type_id t, f32 v, cc::vector<scalar>& out) const
    {
        if (auto const* const r = record_of(t))
        {
            for (auto i = 0; i < r->leaf_count; ++i)
            {
                switch (r->leaf_kind)
                {
                case value_kind::scalar_float:
                    out.push_back(scalar::of(v));
                    break;
                case value_kind::scalar_half:
                    out.push_back(scalar::of_half(f64(v)));
                    break;
                case value_kind::scalar_uint:
                    out.push_back(scalar::of_uint(0));
                    break;
                case value_kind::boolean:
                    out.push_back(scalar::of(false));
                    break;
                case value_kind::none:
                    return false;
                default:
                    out.push_back({.kind = r->leaf_kind, .bits = 0});
                    break;
                }
            }
            return true;
        }
        auto const& info = m().at(t);
        if (info.kind == type_kind::enumeration)
        {
            auto const cases = m().at(info.cases);
            if (cases.empty())
                return false;
            out.push_back(scalar::of(cases[0].value));
            return true;
        }
        if (info.kind == type_kind::array)
        {
            for (auto i = 0; i < info.count; ++i)
                if (!leaves_of(info.element, v, out))
                    return false;
            return info.count > 0;
        }
        if (info.kind != type_kind::structure)
            return false;
        for (auto const& member : m().at(info.members))
            if (!leaves_of(member.type, v, out))
                return false;
        return true;
    }

    /// A value of `t` that is `v` in every float leaf; none for a type that holds a resource.
    flat_expr_id constant(type_id t, f32 v)
    {
        auto leaves = cc::vector<scalar>();
        if (!leaves_of(t, v, leaves))
            return failed(cc::format("a value of {} has no constant", m().name_of(t)));
        auto at_leaf = isize(0);
        return f.value_of(t, at, leaves, at_leaf);
    }
    flat_expr_id zero(type_id t) { return constant(t, 0.0f); }
    flat_expr_id one(type_id t) { return constant(t, 1.0f); }

    // ---- nodes ----------------------------------------------------------------------------------------------------

    template <class Node>
    flat_expr_id add(type_id type, Node n)
    {
        return f.add_expr(type, at, cc::move(n));
    }

    flat_expr_id ref(local_id local) { return f.local_ref(local, at); }
    flat_expr_id boolean(bool v) { return add(f.bool_type(), flat_bool_literal{.value = v}); }
    flat_expr_id integer(i32 v) { return add(f.int_type(), flat_int_literal{.value = v}); }

    template <class Node>
    void emit(Node n)
    {
        f.add_stmt(where, cc::move(n));
    }

    /// `id`'s statement again with `n` as its node, keeping where it came from.
    template <class Node>
    void restate(flat_stmt_id id, Node n)
    {
        auto const s = f.entry.at(id);
        f.entry.stmts.push_back({.from = s.from, .inlined_through = s.inlined_through, .node = cc::move(n)});
        f.block.push_back(flat_stmt_id(f.entry.stmts.size() - 1));
    }

    /// The statements `fill` writes, as a list of their own; the list being written is left as it was.
    template <class Fill>
    cc::vector<flat_stmt_id> collect(Fill&& fill)
    {
        auto outer = cc::move(f.block);
        f.block = {};
        fill();
        auto result = cc::move(f.block);
        f.block = cc::move(outer);
        return result;
    }

    [[nodiscard]] cc::vector<flat_stmt_id> copy_of(ast::range_of<flat_stmt_id> r) const
    {
        return cc::vector<flat_stmt_id>::create_copy_of(cc::span<flat_stmt_id const>(f.entry.at(r)));
    }
    [[nodiscard]] cc::vector<flat_expr_id> copy_of(ast::range_of<flat_expr_id> r) const
    {
        return cc::vector<flat_expr_id>::create_copy_of(cc::span<flat_expr_id const>(f.entry.at(r)));
    }

    /// `x` in a form every read of it may copy: itself where it is a literal or an immutable local, a `let` otherwise.
    /// Backward, a tangent is never bound by a `let`, which would place it in the tree: it becomes a linear let.
    flat_expr_id share(flat_expr_id x, cc::string_view name = "v")
    {
        if (!is_valid(x) || f.is_substitutable(x))
            return x;
        if (is_reverse && is_linear(x))
            return share_tangent(x);
        auto const local = f.add_local(local_kind::temporary, name, type_of(x));
        emit(flat_let{.local = local, .value = x});
        return ref(local);
    }
    flat_expr_id use(flat_expr_id shared) { return is_valid(shared) ? f.again(shared, at) : flat_expr_id::none; }

    /// A tangent read several times: a `let` going forward, and a linear let the backward sweep transposes through.
    flat_expr_id share_tangent(flat_expr_id t)
    {
        if (!is_valid(t) || !is_reverse || f.is_substitutable(t))
            return share(t, "dt");
        auto const local = f.add_local(local_kind::temporary, "linear", type_of(t));
        linear_definition[i32(local)] = t;
        linear_lets.push_back(local);
        linear_locals.insert(i32(local));
        return ref(local);
    }

    /// A deep copy of the block-free expression `id`, which reads the locals it reads.
    flat_expr_id clone(flat_expr_id id)
    {
        if (!is_valid(id))
            return id;
        auto x = node(id);
        if (auto const* const r = x.node.try_as<flat_local_ref>())
            if (auto const* const twin = substitution.get_ptr(i32(r->local)))
                return ref(*twin);
        if (x.node.is<flat_block>() || x.node.is<flat_by_target>())
            return failed("a value that still holds a block");
        if (auto* const n = x.node.try_as<flat_member>())
            n->object = clone(n->object);
        else if (auto* const n = x.node.try_as<flat_element>())
        {
            n->object = clone(n->object);
            n->index = clone(n->index);
        }
        else if (auto* const n = x.node.try_as<flat_buffer_element>())
        {
            n->buffer = clone(n->buffer);
            n->index = clone(n->index);
        }
        else if (auto* const n = x.node.try_as<flat_buffer_length>())
            n->buffer = clone(n->buffer);
        else if (auto* const n = x.node.try_as<flat_construct>())
        {
            auto arguments = copy_of(n->arguments);
            for (auto& a : arguments)
                a = clone(a);
            n->arguments = f.add_list(arguments);
        }
        else if (auto* const n = x.node.try_as<flat_call>())
        {
            auto arguments = copy_of(n->arguments);
            for (auto& a : arguments)
                a = clone(a);
            n->arguments = f.add_list(arguments);
        }
        else if (auto* const n = x.node.try_as<flat_not>())
            n->operand = clone(n->operand);
        else if (auto* const n = x.node.try_as<flat_and>())
        {
            n->lhs = clone(n->lhs);
            n->rhs = clone(n->rhs);
        }
        else if (auto* const n = x.node.try_as<flat_or>())
        {
            n->lhs = clone(n->lhs);
            n->rhs = clone(n->rhs);
        }
        f.entry.exprs.push_back(cc::move(x));
        return flat_expr_id(f.entry.exprs.size() - 1);
    }

    // ---- calls ----------------------------------------------------------------------------------------------------

    [[nodiscard]] static bool is_operator(cc::string_view name)
    {
        return !name.empty()
            && !(name[0] == '_' || (name[0] >= 'a' && name[0] <= 'z') || (name[0] >= 'A' && name[0] <= 'Z'));
    }

    /// The builtin `name` that takes exactly `types`.
    [[nodiscard]] symbol_id find(cc::string_view name, cc::span<type_id const> types) const
    {
        if (is_operator(name))
            return f.c.find_operator(f.file(), name, types);
        auto const* const found = f.c.prelude_names.get_ptr(cc::string(name));
        if (found == nullptr)
            return symbol_id::none;
        for (auto const candidate : *found)
        {
            auto const& s = m().at(candidate);
            if (s.kind != symbol_kind::function || s.info < 0 || !is_valid(s.intrinsic))
                continue;
            auto const parameters = m().at(m().functions[s.info].parameters);
            auto is_match = parameters.size() == types.size();
            for (auto i = isize(0); is_match && i < parameters.size(); ++i)
                is_match = parameters[i].type == types[i];
            if (is_match)
                return candidate;
        }
        return symbol_id::none;
    }

    flat_expr_id call(cc::string_view name, cc::span<flat_expr_id const> arguments)
    {
        auto types = cc::vector<type_id>();
        for (auto const a : arguments)
        {
            if (!is_valid(a))
                return failed(cc::format("a derivative of {} met a value it could not write", name));
            types.push_back(type_of(a));
        }
        auto const callee = find(name, types);
        if (!is_valid(callee))
        {
            auto spelled = cc::string();
            for (auto const t : types)
                spelled += cc::format("{}{}", spelled.empty() ? "" : ", ", m().name_of(t));
            return failed(cc::format("its derivative needs {}({}), which no builtin is", name, spelled));
        }
        return f.builtin_call(at, callee, arguments);
    }

    flat_expr_id call(cc::string_view name, flat_expr_id a)
    {
        flat_expr_id const arguments[] = {a};
        return call(name, arguments);
    }
    flat_expr_id call(cc::string_view name, flat_expr_id a, flat_expr_id b)
    {
        flat_expr_id const arguments[] = {a, b};
        return call(name, arguments);
    }
    flat_expr_id call(cc::string_view name, flat_expr_id a, flat_expr_id b, flat_expr_id c)
    {
        flat_expr_id const arguments[] = {a, b, c};
        return call(name, arguments);
    }

    /// `a * b`, or `b * a` where one side is a scalar and only that order exists.
    flat_expr_id mul(flat_expr_id a, flat_expr_id b)
    {
        if (!is_valid(a) || !is_valid(b))
            return flat_expr_id::none;
        type_id const ab[] = {type_of(a), type_of(b)};
        type_id const ba[] = {type_of(b), type_of(a)};
        if (!is_valid(find("*", ab)) && (is_scalar(ab[0]) || is_scalar(ab[1])) && is_valid(find("*", ba)))
            return call("*", b, a);
        return call("*", a, b);
    }
    /// The sum of two terms of one type, either of which may be zero.
    flat_expr_id sum(flat_expr_id a, flat_expr_id b)
    {
        if (!is_valid(a))
            return b;
        if (!is_valid(b))
            return a;
        return call("+", a, b);
    }
    flat_expr_id negate(flat_expr_id a) { return is_valid(a) ? call("-", a) : a; }
    flat_expr_id difference(flat_expr_id a, flat_expr_id b)
    {
        if (!is_valid(b))
            return a;
        if (!is_valid(a))
            return negate(b);
        return call("-", a, b);
    }
    /// `1 - x`, of `x`'s type.
    flat_expr_id one_minus(flat_expr_id x) { return call("-", one(type_of(x)), x); }

    // ---- lowering -------------------------------------------------------------------------------------------------

    void lower_list(cc::span<flat_stmt_id const> statements)
    {
        for (auto const s : cc::vector<flat_stmt_id>::create_copy_of(statements))
            lower_stmt(s);
    }
    cc::vector<flat_stmt_id> lowered(ast::range_of<flat_stmt_id> body)
    {
        auto const statements = copy_of(body);
        return collect([&] { lower_list(statements); });
    }

    void lower_stmt(flat_stmt_id id)
    {
        auto const s = f.entry.at(id);
        if (auto const* const n = s.node.try_as<flat_let>())
            return restate(id, flat_let{.local = n->local, .value = lift(n->value)});
        if (auto const* const n = s.node.try_as<flat_var>())
            return restate(id, flat_var{.local = n->local, .value = lift(n->value)});
        if (auto const* const n = s.node.try_as<flat_assign>())
            return restate(id, flat_assign{.place = n->place, .value = lift(n->value)});
        if (auto const* const n = s.node.try_as<flat_eval>())
        {
            auto const value = lift(n->value);
            auto const* const r = is_valid(value) ? node(value).node.try_as<flat_local_ref>() : nullptr;
            if (is_valid(value) && (r == nullptr || !is_lifted_result(r->local)))
                restate(id, flat_eval{.value = value});
            return;
        }
        if (auto const* const n = s.node.try_as<flat_leave>())
        {
            if (auto const* const r = lifted.get_ptr(i32(n->target)); r != nullptr && is_valid(n->value))
            {
                auto const result = *r;
                auto const value = lift(n->value);
                restate(id, flat_assign{.place = ref(result), .value = value});
                return restate(id, flat_leave{.target = n->target});
            }
            return restate(id, flat_leave{.target = n->target, .value = lift(n->value)});
        }
        if (auto const* const n = s.node.try_as<flat_if>())
        {
            auto const condition = lift(n->condition);
            auto const then_body = f.add_list(lowered(n->then_body));
            auto const else_body = f.add_list(lowered(n->else_body));
            return restate(id, flat_if{.condition = condition, .then_body = then_body, .else_body = else_body});
        }
        if (auto const* const n = s.node.try_as<flat_block>())
            return restate(id, flat_block{.label = n->label, .body = f.add_list(lowered(n->body))});
        if (auto const* const n = s.node.try_as<flat_loop>())
            return restate(id, flat_loop{.label = n->label, .body = f.add_list(lowered(n->body))});
        if (auto const* const n = s.node.try_as<flat_while>())
            return restate(
                id, flat_while{.label = n->label, .condition = n->condition, .body = f.add_list(lowered(n->body))});
        if (auto const* const n = s.node.try_as<flat_for>())
            return restate(id, flat_for{.label = n->label,
                                        .index = n->index,
                                        .first = n->first,
                                        .end = n->end,
                                        .body = f.add_list(lowered(n->body))});
        if (auto const* const n = s.node.try_as<flat_case>())
        {
            auto result = *n;
            result.scrutinee = lift(n->scrutinee);
            auto arms = cc::vector<flat_arm>::create_copy_of(cc::span<flat_arm const>(f.entry.at(n->arms)));
            for (auto& arm : arms)
                arm.body = f.add_list(lowered(arm.body));
            result.arms = f.add_arms(arms);
            result.default_body = f.add_list(lowered(n->default_body));
            return restate(id, result);
        }
        f.block.push_back(id);
    }

    [[nodiscard]] bool is_lifted_result(local_id local) const
    {
        for (auto const& [label, result] : lifted)
            if (result == local)
                return true;
        return false;
    }

    /// `id` with every block it evaluates eagerly lifted into a statement block ahead of it.
    /// The right side of `and` and `or` stays where it is, since it may not run.
    flat_expr_id lift(flat_expr_id id)
    {
        if (!is_valid(id))
            return id;
        auto x = node(id);
        if (auto const* const b = x.node.try_as<flat_block>())
        {
            auto const block_where = origin{.file = x.from.file, .expr = x.from.expr};
            auto const label = b->label;
            auto const body = b->body;
            if (x.type == checked_module::void_type)
            {
                auto const statements = lowered(body);
                f.add_stmt(block_where, flat_block{.label = label, .body = f.add_list(statements)});
                return flat_expr_id::none;
            }
            auto const result = f.add_local(local_kind::var, f.entry.at(label).name, x.type);
            auto leaves = cc::vector<scalar>();
            auto const initial = leaves_of(x.type, 0.0f, leaves) ? zero(x.type) : flat_expr_id::none;
            f.add_stmt(block_where, flat_var{.local = result, .value = initial});
            lifted[i32(label)] = result;
            auto const statements = lowered(body);
            f.add_stmt(block_where, flat_block{.label = label, .body = f.add_list(statements)});
            return ref(result);
        }
        if (x.node.is<flat_by_target>())
            return failed("a trace, whose two forms it cannot follow yet");
        if (auto* const n = x.node.try_as<flat_member>())
            n->object = lift(n->object);
        else if (auto* const n = x.node.try_as<flat_element>())
        {
            n->object = lift(n->object);
            n->index = lift(n->index);
        }
        else if (auto* const n = x.node.try_as<flat_buffer_element>())
            n->index = lift(n->index);
        else if (auto* const n = x.node.try_as<flat_construct>())
        {
            auto arguments = copy_of(n->arguments);
            for (auto& a : arguments)
                a = lift(a);
            n->arguments = f.add_list(arguments);
        }
        else if (auto* const n = x.node.try_as<flat_call>())
        {
            auto arguments = copy_of(n->arguments);
            for (auto& a : arguments)
                a = lift(a);
            n->arguments = f.add_list(arguments);
        }
        else if (auto* const n = x.node.try_as<flat_not>())
            n->operand = lift(n->operand);
        else if (auto* const n = x.node.try_as<flat_and>())
            n->lhs = lift(n->lhs);
        else if (auto* const n = x.node.try_as<flat_or>())
            n->lhs = lift(n->lhs);
        f.entry.exprs[index_of(id)] = cc::move(x);
        return id;
    }

    // ---- tangents -------------------------------------------------------------------------------------------------

    /// The tangent a read of `local` stands for: its tangent local going forward, its placeholder going backward.
    flat_expr_id tangent_leaf(local_id local)
    {
        if (!is_reverse)
        {
            auto const* const t = tangent_of.get_ptr(i32(local));
            return t != nullptr ? ref(*t) : flat_expr_id::none;
        }
        if (!tracked.contains(i32(local)))
            return flat_expr_id::none;
        if (auto const* const p = placeholder_of.get_ptr(i32(local)))
            return ref(*p);
        auto const p = f.add_local(local_kind::temporary, cc::format("t_{}", f.entry.at(local).name),
                                   tangent(f.entry.at(local).type));
        placeholder_of[i32(local)] = p;
        primal_of_placeholder[i32(p)] = local;
        linear_locals.insert(i32(p));
        return ref(p);
    }

    /// `id`, consumed, as its value and its tangent, each to be placed once.
    dual derive(flat_expr_id id)
    {
        if (!is_valid(id) || is_failed)
            return {id, flat_expr_id::none};
        auto const x = node(id);
        if (!is_differentiable(x.type))
            return {id, flat_expr_id::none};
        if (auto const* const r = x.node.try_as<flat_local_ref>())
            return {id, tangent_leaf(r->local)};
        if (x.node.is<flat_literal>())
            return {id, flat_expr_id::none};
        // a constant block's member is a constant: a camera, a setting, what the host hands every invocation alike
        if (auto const* const member = x.node.try_as<flat_binding_member>(); member != nullptr && !member->is_workgroup)
            return {id, flat_expr_id::none};
        // a buffer holds what a gradient would have to be written back into, which a derivative never does silently
        if (x.node.is<flat_buffer_element>() || x.node.is<flat_binding_member>())
        {
            failed("a read of a buffer, whose gradient it cannot write back; read it as `no_diff(…)` to hold it "
                   "constant, or give the function that reads it a @backward that writes the gradient");
            return {id, flat_expr_id::none};
        }
        if (auto const* const n = x.node.try_as<flat_member>())
        {
            auto const object = derive(n->object);
            auto const primal
                = add(x.type, flat_member{.object = object.primal, .member = n->member, .letters = n->letters});
            if (!is_valid(object.tangent))
                return {primal, flat_expr_id::none};
            return {primal, add(tangent(x.type),
                                flat_member{.object = object.tangent, .member = n->member, .letters = n->letters})};
        }
        if (auto const* const n = x.node.try_as<flat_element>())
        {
            auto const object = derive(n->object);
            auto const index = share(n->index, "index");
            auto const primal = add(x.type, flat_element{.object = object.primal, .index = use(index)});
            if (!is_valid(object.tangent))
                return {primal, flat_expr_id::none};
            return {primal, add(tangent(x.type), flat_element{.object = object.tangent, .index = use(index)})};
        }
        if (auto const* const n = x.node.try_as<flat_construct>())
        {
            auto const arguments = copy_of(n->arguments);
            auto primals = cc::vector<flat_expr_id>();
            auto tangents = cc::vector<flat_expr_id>();
            auto any = false;
            for (auto const a : arguments)
            {
                auto const d = derive(a);
                primals.push_back(d.primal);
                tangents.push_back(d.tangent);
                any = any || is_valid(d.tangent);
            }
            auto const primal = add(x.type, flat_construct{.arguments = f.add_list(primals)});
            if (!any)
                return {primal, flat_expr_id::none};
            for (auto i = isize(0); i < tangents.size(); ++i)
                if (!is_valid(tangents[i]))
                    tangents[i] = zero(tangent(type_of(primals[i])));
            return {primal, add(tangent(x.type), flat_construct{.arguments = f.add_list(tangents)})};
        }
        if (auto const* const n = x.node.try_as<flat_call>())
            return derive_call(x.type, *n);
        failed("a value it cannot follow yet");
        return {id, flat_expr_id::none};
    }

    /// The name a call's rule is chosen by: an operator's spelling, a function's name.
    [[nodiscard]] cc::string_view rule_name(symbol_id callee) const
    {
        auto const& s = m().at(callee);
        return s.operator_spelling.empty() ? cc::string_view(s.name) : cc::string_view(s.operator_spelling);
    }

    [[nodiscard]] static bool is_linear_function(cc::string_view name, isize arity)
    {
        if ((name == "+" || name == "-") && arity <= 2)
            return true;
        cc::string_view const linear[]
            = {"transpose", "matrix_column", "matrix_row", "select", "float", "half", "float2", "float3", "float4",
               "half2",     "half3",         "half4",      "vec3",   "pos3",  "mat2", "mat3",   "mat4"};
        for (auto const l : linear)
            if (name == l)
                return true;
        return false;
    }

    /// The factor each argument's tangent is multiplied by, for a builtin that acts on each component on its own.
    /// `a` holds the arguments and `r` the result, each shared; none where a rule has no factor for that argument.
    bool componentwise_factors(cc::string_view name,
                               cc::span<flat_expr_id const> a,
                               flat_expr_id r,
                               cc::vector<flat_expr_id>& factors)
    {
        auto const x = [&] { return use(a[0]); };
        auto const t = type_of(r);
        auto const s = scalar_of(t);
        auto const number = [&](f64 v) { return add(s, flat_literal{.value = v}); };
        factors.resize_to_filled(a.size(), flat_expr_id::none);
        if (name == "sin")
            factors[0] = call("cos", x());
        else if (name == "cos")
            factors[0] = negate(call("sin", x()));
        else if (name == "tan")
            factors[0] = call("+", one(t), call("*", use(r), use(r)));
        else if (name == "exp")
            factors[0] = use(r);
        else if (name == "exp2")
            factors[0] = mul(use(r), number(0.69314718055994530942));
        else if (name == "log")
            factors[0] = call("/", one(t), x());
        else if (name == "log2")
            factors[0] = call("/", one(t), mul(x(), number(0.69314718055994530942)));
        else if (name == "sqrt")
            factors[0] = call("/", constant(t, 0.5f), use(r));
        else if (name == "inverse_sqrt")
            factors[0] = mul(call("*", call("*", use(r), use(r)), use(r)), number(-0.5));
        else if (name == "abs")
            factors[0] = call("sign", x());
        else if (name == "fract")
            factors[0] = one(t);
        else if (name == "sinh")
            factors[0] = call("cosh", x());
        else if (name == "cosh")
            factors[0] = call("sinh", x());
        else if (name == "tanh")
            factors[0] = one_minus(call("*", use(r), use(r)));
        else if (name == "asin")
            factors[0] = call("/", one(t), call("sqrt", one_minus(call("*", x(), x()))));
        else if (name == "acos")
            factors[0] = negate(call("/", one(t), call("sqrt", one_minus(call("*", x(), x())))));
        else if (name == "atan")
            factors[0] = call("/", one(t), call("+", one(t), call("*", x(), x())));
        else if (name == "saturate")
            factors[0] = call("*", call("step", zero(t), x()), call("step", x(), one(t)));
        else if (name == "floor" || name == "ceil" || name == "round" || name == "trunc" || name == "sign"
                 || name == "step")
            ;
        else if (name == "pow")
        {
            factors[0] = call("*", use(a[1]), call("pow", x(), call("-", use(a[1]), one(t))));
            factors[1] = call("*", use(r), call("log", x()));
        }
        else if (name == "atan2")
        {
            // atan2(y, x)
            auto const denominator = share(call("+", call("*", use(a[1]), use(a[1])), call("*", use(a[0]), use(a[0]))));
            factors[0] = call("/", use(a[1]), use(denominator));
            factors[1] = negate(call("/", use(a[0]), use(denominator)));
        }
        else if (name == "min" || name == "max")
        {
            // the side that is taken takes the tangent; at a tie the first does
            auto const first
                = share(name == "min" ? call("step", use(a[0]), use(a[1])) : call("step", use(a[1]), use(a[0])));
            factors[0] = use(first);
            factors[1] = one_minus(use(first));
        }
        else if (name == "clamp")
        {
            auto const above_low = share(call("step", use(a[1]), use(a[0])));
            auto const below_high = share(call("step", use(a[0]), use(a[2])));
            factors[0] = call("*", use(above_low), use(below_high));
            factors[1] = one_minus(use(above_low));
            factors[2] = one_minus(use(below_high));
        }
        else if (name == "mix")
        {
            factors[0] = one_minus(use(a[2]));
            factors[1] = use(a[2]);
            factors[2] = call("-", use(a[1]), use(a[0]));
        }
        else
            return false;
        return true;
    }

    /// The tangent of `m * p`, which is `dm * (p, 1) + m * (dp, 0)` as a `float4`: the change of `p` takes no translation.
    flat_expr_id transform_position_tangent(cc::span<flat_expr_id const> primals,
                                            cc::span<flat_expr_id const> tangents,
                                            type_id type)
    {
        auto const float4 = tangent(type);
        auto const scalar = scalar_of(float4);
        auto const extended = [&](flat_expr_id v, f64 w)
        {
            auto parts = cc::vector<flat_expr_id>();
            for (auto i = 0; i < 3; ++i)
                parts.push_back(add(f.field_type(type_of(v), i), flat_member{.object = use(v), .member = i}));
            parts.push_back(add(scalar, flat_literal{.value = w}));
            return add(float4, flat_construct{.arguments = f.add_list(parts)});
        };
        auto result = flat_expr_id::none;
        if (is_valid(tangents[0]))
            result = call("*", share_tangent(tangents[0]), extended(primals[1], 1.0));
        if (is_valid(tangents[1]))
            result = sum(result, call("*", use(primals[0]), extended(share_tangent(tangents[1]), 0.0)));
        return result;
    }

    dual derive_call(type_id type, flat_call const& n)
    {
        auto const arguments = copy_of(n.arguments);
        auto primals = cc::vector<flat_expr_id>();
        auto tangents = cc::vector<flat_expr_id>();
        auto any = false;
        for (auto const a : arguments)
        {
            auto const d = derive(a);
            primals.push_back(share(d.primal, "a"));
            tangents.push_back(d.tangent);
            any = any || is_valid(d.tangent);
        }
        auto const write_primal = [&]
        {
            auto copies = cc::vector<flat_expr_id>();
            for (auto const p : primals)
                copies.push_back(use(p));
            return f.builtin_call(at, n.callee, copies);
        };
        if (!any)
            return {write_primal(), flat_expr_id::none};
        auto const name = rule_name(n.callee);
        auto const result = share(write_primal(), "r");
        // `m * p` takes the translation, and its tangent does not: a position's change moves through `m` as a direction
        if (m().at(n.callee).name == "transform_position")
            return {use(result), transform_position_tangent(primals, tangents, type)};
        if (m().at(n.callee).name == "transform_position_affine")
        {
            failed("an affine3 times a position, which has no rule yet");
            return {use(result), flat_expr_id::none};
        }

        if (is_linear_function(name, arguments.size()))
        {
            // `x + dx` alone where the other side is zero and of the result's type
            if (name == "+" && arguments.size() == 2 && (!is_valid(tangents[0]) || !is_valid(tangents[1])))
            {
                auto const only = is_valid(tangents[0]) ? tangents[0] : tangents[1];
                if (type_of(only) == type)
                    return {use(result), only};
            }
            if (name == "-" && arguments.size() == 2 && !is_valid(tangents[1]) && type_of(tangents[0]) == type)
                return {use(result), tangents[0]};
            auto written = cc::vector<flat_expr_id>();
            for (auto i = isize(0); i < arguments.size(); ++i)
            {
                if (!is_differentiable(type_of(primals[i])))
                    written.push_back(use(primals[i]));
                else
                    written.push_back(is_valid(tangents[i]) ? tangents[i] : zero(tangent(type_of(primals[i]))));
            }
            // a construction of a position builds its tangent as the direction it is: `pos3(x)`'s is `vec3(dx)`
            if (tangent(type) != type && name == m().name_of(type))
                return {use(result), call(m().name_of(tangent(type)), written)};
            return {use(result), f.builtin_call(at, n.callee, written)};
        }

        for (auto& t : tangents)
            t = share_tangent(t);
        auto const tangent_of_argument = [&](isize i) { return use(tangents[i]); };
        auto tangent = flat_expr_id::none;

        if (name == "*")
            tangent = sum(is_valid(tangents[0]) ? mul(tangent_of_argument(0), use(primals[1])) : flat_expr_id::none,
                          is_valid(tangents[1]) ? mul(use(primals[0]), tangent_of_argument(1)) : flat_expr_id::none);
        else if (name == "/")
        {
            // (da - r * db) / b
            auto const r_db = is_valid(tangents[1]) ? mul(use(result), tangent_of_argument(1)) : flat_expr_id::none;
            auto const da = is_valid(tangents[0]) ? tangent_of_argument(0) : flat_expr_id::none;
            tangent = call("/", difference(da, r_db), use(primals[1]));
        }
        else if (name == "dot" || name == "cross")
            tangent
                = sum(is_valid(tangents[0]) ? call(name, tangent_of_argument(0), use(primals[1])) : flat_expr_id::none,
                      is_valid(tangents[1]) ? call(name, use(primals[0]), tangent_of_argument(1)) : flat_expr_id::none);
        else if (name == "length")
            tangent = call("/", call("dot", use(primals[0]), tangent_of_argument(0)), use(result));
        else if (name == "distance")
        {
            auto const d = share(call("-", use(primals[0]), use(primals[1])));
            auto const dd = difference(is_valid(tangents[0]) ? tangent_of_argument(0) : flat_expr_id::none,
                                       is_valid(tangents[1]) ? tangent_of_argument(1) : flat_expr_id::none);
            tangent = call("/", call("dot", use(d), dd), use(result));
        }
        else if (name == "normalize")
        {
            // (dv - n * dot(n, dv)) / length(v)
            auto const along = mul(use(result), call("dot", use(result), tangent_of_argument(0)));
            tangent = call("/", call("-", tangent_of_argument(0), along), call("length", use(primals[0])));
        }
        else
        {
            auto factors = cc::vector<flat_expr_id>();
            if (!componentwise_factors(name, primals, result, factors))
            {
                failed(cc::format("no derivative of {} yet", name));
                return {use(result), flat_expr_id::none};
            }
            for (auto i = isize(0); i < tangents.size(); ++i)
                if (is_valid(tangents[i]) && is_valid(factors[i]))
                    tangent = sum(tangent, mul(tangent_of_argument(i), factors[i]));
        }
        return {use(result), tangent};
    }

    /// The tangent of the block-free value `value`, read where it stands; the value itself is left in place.
    flat_expr_id tangent_of_value(flat_expr_id value) { return derive(clone(value)).tangent; }

    // ---- places ---------------------------------------------------------------------------------------------------

    /// The local `place` is a part of, and the steps from it down to the place; none for a buffer's element.
    local_id root_of(flat_expr_id place, cc::vector<place_step>* steps)
    {
        auto const x = node(place);
        if (auto const* const r = x.node.try_as<flat_local_ref>())
            return r->local;
        if (auto const* const n = x.node.try_as<flat_member>())
        {
            if (n->is_swizzle())
                return local_id::none;
            auto const root = root_of(n->object, steps);
            if (steps != nullptr)
                steps->push_back({.member = n->member});
            return root;
        }
        if (auto const* const n = x.node.try_as<flat_element>())
        {
            if (!f.is_substitutable(n->index))
                return local_id::none;
            auto const root = root_of(n->object, steps);
            if (steps != nullptr)
                steps->push_back({.index = n->index});
            return root;
        }
        return local_id::none;
    }

    /// A place in a buffer, which a derivative leaves alone: what a shader stores is no value it follows.
    [[nodiscard]] bool is_buffer_place(flat_expr_id place) const
    {
        auto const& x = node(place);
        if (x.node.is<flat_buffer_element>())
            return true;
        if (auto const* const n = x.node.try_as<flat_member>())
            return is_buffer_place(n->object);
        if (auto const* const n = x.node.try_as<flat_element>())
            return is_buffer_place(n->object);
        return false;
    }

    /// What indexing a value of `t` reads: an array's element, or a vector's component.
    [[nodiscard]] type_id element_type(type_id t) const
    {
        return m().at(t).kind == type_kind::array ? m().at(t).element : f.field_type(t, 0);
    }

    /// `steps` below a fresh read of `root`.
    flat_expr_id place_of(flat_expr_id root, cc::span<place_step const> steps)
    {
        auto x = root;
        for (auto const& s : steps)
        {
            if (is_valid(s.index))
                x = add(element_type(type_of(x)), flat_element{.object = x, .index = use(s.index)});
            else
                x = add(f.field_type(type_of(x), s.member), flat_member{.object = x, .member = s.member});
        }
        return x;
    }

    /// `place += g`, field by field for a struct or an array, which has no `+`.
    template <class Root>
    void accumulate(Root&& root, cc::vector<place_step> steps, flat_expr_id g)
    {
        auto const place_type = type_of(place_of(root(), steps));
        type_id const types[] = {place_type, type_of(g)};
        if (is_valid(find("+", types)))
        {
            auto const read = place_of(root(), steps);
            return emit(flat_assign{.place = place_of(root(), steps), .value = call("+", read, use(g))});
        }
        auto const& info = m().at(place_type);
        auto const has_parts = info.kind == type_kind::array ? info.count > 0 : info.members.count > 0;
        if (place_type != type_of(g) || (info.kind != type_kind::structure && info.kind != type_kind::array) || !has_parts)
        {
            failed(cc::format("an adjoint of {} that it cannot add {} to", m().name_of(place_type),
                              m().name_of(type_of(g))));
            return;
        }
        auto const count = info.kind == type_kind::array ? info.count : i32(info.members.count);
        for (auto i = 0; i < count; ++i)
        {
            auto const is_array = info.kind == type_kind::array;
            auto const step = is_array ? place_step{.index = integer(i)} : place_step{.member = i};
            auto const part_type = is_array ? info.element : f.field_type(place_type, i);
            if (!is_differentiable(part_type))
                continue;
            auto const part = share(is_array ? add(part_type, flat_element{.object = use(g), .index = use(step.index)})
                                             : add(part_type, flat_member{.object = use(g), .member = i}));
            auto deeper = steps;
            deeper.push_back(step);
            accumulate(root, cc::move(deeper), part);
        }
    }

    // ---- forward --------------------------------------------------------------------------------------------------

    cc::vector<flat_stmt_id> forward(cc::span<flat_stmt_id const> statements)
    {
        auto const copy = cc::vector<flat_stmt_id>::create_copy_of(statements);
        return collect(
            [&]
            {
                for (auto const s : copy)
                    forward_stmt(s);
            });
    }
    cc::vector<flat_stmt_id> forward(ast::range_of<flat_stmt_id> body) { return forward(copy_of(body)); }

    void forward_stmt(flat_stmt_id id)
    {
        auto const s = f.entry.at(id);
        auto const declare = [&](local_id local, flat_expr_id value, bool is_mut)
        {
            auto const type = f.entry.at(local).type;
            if (!is_differentiable(type))
            {
                f.block.push_back(id);
                return;
            }
            auto const tangent = is_valid(value) ? tangent_of_value(value) : flat_expr_id::none;
            f.block.push_back(id);
            auto const t = f.add_local(is_mut ? local_kind::var : local_kind::let,
                                       cc::format("d_{}", f.entry.at(local).name), this->tangent(type));
            auto const initial = is_valid(tangent) ? tangent : zero(this->tangent(type));
            if (is_mut)
                emit(flat_var{.local = t, .value = initial});
            else
                emit(flat_let{.local = t, .value = initial});
            tangent_of[i32(local)] = t;
        };
        if (auto const* const n = s.node.try_as<flat_let>())
            return declare(n->local, n->value, false);
        if (auto const* const n = s.node.try_as<flat_var>())
            return declare(n->local, n->value, true);
        if (auto const* const n = s.node.try_as<flat_assign>())
        {
            auto steps = cc::vector<place_step>();
            auto const root = root_of(n->place, &steps);
            if (!is_valid(root) && !is_buffer_place(n->place))
                return (void)failed("an assignment to a part of a local it cannot follow yet");
            auto const* const t = is_valid(root) ? tangent_of.get_ptr(i32(root)) : nullptr;
            if (t == nullptr || !is_differentiable(type_of(n->place)))
            {
                f.block.push_back(id);
                return;
            }
            auto const tangent_local = *t;
            // computed before the assignment, which may change what it reads
            auto const tangent = tangent_of_value(n->value);
            auto const value = share(is_valid(tangent) ? tangent : zero(this->tangent(type_of(n->place))), "dt");
            f.block.push_back(id);
            return emit(flat_assign{.place = place_of(ref(tangent_local), steps), .value = use(value)});
        }
        if (auto const* const n = s.node.try_as<flat_if>())
        {
            auto const then_body = f.add_list(forward(n->then_body));
            auto const else_body = f.add_list(forward(n->else_body));
            return restate(id, flat_if{.condition = n->condition, .then_body = then_body, .else_body = else_body});
        }
        if (auto const* const n = s.node.try_as<flat_block>())
        {
            if (auto const* const custom = f.custom_calls.get_ptr(i32(n->label)))
                return forward_custom(id, *custom, n->label);
            return restate(id, flat_block{.label = n->label, .body = f.add_list(forward(n->body))});
        }
        if (auto const* const n = s.node.try_as<flat_loop>())
            return restate(id, flat_loop{.label = n->label, .body = f.add_list(forward(n->body))});
        if (auto const* const n = s.node.try_as<flat_while>())
            return restate(
                id, flat_while{.label = n->label, .condition = n->condition, .body = f.add_list(forward(n->body))});
        if (auto const* const n = s.node.try_as<flat_for>())
            return restate(id, flat_for{.label = n->label,
                                        .index = n->index,
                                        .first = n->first,
                                        .end = n->end,
                                        .body = f.add_list(forward(n->body))});
        if (auto const* const n = s.node.try_as<flat_case>())
        {
            auto result = *n;
            auto arms = cc::vector<flat_arm>::create_copy_of(cc::span<flat_arm const>(f.entry.at(n->arms)));
            for (auto& arm : arms)
                arm.body = f.add_list(forward(arm.body));
            result.arms = f.add_arms(arms);
            result.default_body = f.add_list(forward(n->default_body));
            return restate(id, result);
        }
        f.block.push_back(id);
    }

    // ---- reverse: straight structured code ------------------------------------------------------------------------

    /// A local the reverse pass declares at the top of the body.
    local_id hoist(cc::string_view name, type_id type, flat_expr_id value)
    {
        auto const local = f.add_local(local_kind::var, name, type);
        hoisted.push_back(f.make_stmt(where, flat_var{.local = local, .value = value}));
        return local;
    }

    /// Every `let` and `var` of `statements` declared at the top instead, and assigned where it stood.
    cc::vector<flat_stmt_id> hoist_declarations(cc::span<flat_stmt_id const> statements)
    {
        auto const copy = cc::vector<flat_stmt_id>::create_copy_of(statements);
        return collect(
            [&]
            {
                for (auto const id : copy)
                {
                    auto const s = f.entry.at(id);
                    auto const declare = [&](local_id local, flat_expr_id value, bool is_let)
                    {
                        auto& l = f.entry.locals[index_of(local)];
                        l.kind = local_kind::var;
                        l.is_mut = true;
                        auto leaves = cc::vector<scalar>();
                        if (!leaves_of(l.type, 0.0f, leaves))
                            return (void)failed(cc::format("a local of {}", m().name_of(l.type)));
                        hoisted.push_back(f.make_stmt(where, flat_var{.local = local, .value = zero(l.type)}));
                        if (is_differentiable(l.type))
                            tracked.insert(i32(local));
                        if (is_let && loop_depth == 0)
                            single_assigned.insert(i32(local));
                        if (is_valid(value))
                            restate(id, flat_assign{.place = ref(local), .value = value});
                    };
                    if (auto const* const n = s.node.try_as<flat_let>())
                        declare(n->local, n->value, true);
                    else if (auto const* const n = s.node.try_as<flat_var>())
                        declare(n->local, n->value, false);
                    else if (auto const* const n = s.node.try_as<flat_if>())
                    {
                        auto const then_body = f.add_list(hoist_declarations(copy_of(n->then_body)));
                        auto const else_body = f.add_list(hoist_declarations(copy_of(n->else_body)));
                        restate(id, flat_if{.condition = n->condition, .then_body = then_body, .else_body = else_body});
                    }
                    // a call with a derivative written by hand runs forward as it is, loops and all
                    else if (auto const* const custom = s.node.try_as<flat_block>();
                             custom != nullptr && f.custom_calls.contains(i32(custom->label)))
                        f.block.push_back(id);
                    else if (auto const* const n = s.node.try_as<flat_block>())
                        restate(id,
                                flat_block{.label = n->label, .body = f.add_list(hoist_declarations(copy_of(n->body)))});
                    else if (auto const* const loop = s.node.try_as<flat_for>();
                             loop != nullptr && constant_bounds(*loop).has_value())
                    {
                        // a `continue` of this loop leaves the rest of its iteration: a block around the body
                        auto const iteration = f.add_label("iteration");
                        auto const wrapped
                            = f.make_stmt(where, flat_block{.label = iteration,
                                                            .body = f.add_list(continues_as_leaves(
                                                                copy_of(loop->body), loop->label, iteration))});
                        ++loop_depth;
                        auto const body = f.add_list(hoist_declarations(cc::span<flat_stmt_id const>(&wrapped, 1)));
                        --loop_depth;
                        restate(id, flat_for{.label = loop->label,
                                             .index = loop->index,
                                             .first = loop->first,
                                             .end = loop->end,
                                             .body = body});
                    }
                    else if (s.node.is<flat_loop>() || s.node.is<flat_while>() || s.node.is<flat_for>())
                        failed("a loop whose bounds are no constants, which only a @backward can walk back");
                    else if (s.node.is<flat_case>())
                        failed("a case");
                    else if (s.node.is<flat_continue>() || s.node.is<flat_break>() || s.node.is<flat_discard>())
                        failed("a jump it cannot follow yet");
                    else
                        f.block.push_back(id);
                }
            });
    }

    /// `statements` with every `continue` of the loop `loop` a leave of `iteration`, the block around its body.
    cc::vector<flat_stmt_id> continues_as_leaves(cc::span<flat_stmt_id const> statements, label_id loop, label_id iteration)
    {
        auto const copy = cc::vector<flat_stmt_id>::create_copy_of(statements);
        return collect(
            [&]
            {
                for (auto const id : copy)
                {
                    auto const s = f.entry.at(id);
                    auto const inside = [&](ast::range_of<flat_stmt_id> r)
                    { return f.add_list(continues_as_leaves(copy_of(r), loop, iteration)); };
                    if (auto const* const n = s.node.try_as<flat_continue>(); n != nullptr && n->target == loop)
                        restate(id, flat_leave{.target = iteration});
                    else if (auto const* const n = s.node.try_as<flat_if>())
                    {
                        auto const then_body = inside(n->then_body);
                        auto const else_body = inside(n->else_body);
                        restate(id, flat_if{.condition = n->condition, .then_body = then_body, .else_body = else_body});
                    }
                    else if (auto const* const block = s.node.try_as<flat_block>();
                             block != nullptr && !f.custom_calls.contains(i32(block->label)))
                        restate(id, flat_block{.label = block->label, .body = inside(block->body)});
                    else if (auto const* const n = s.node.try_as<flat_for>())
                        restate(id, flat_for{.label = n->label,
                                             .index = n->index,
                                             .first = n->first,
                                             .end = n->end,
                                             .body = inside(n->body)});
                    else
                        f.block.push_back(id);
                }
            });
    }

    /// The labels a run of `id` may leave that it does not itself declare.
    void escapes(flat_stmt_id id, cc::set<i32>& out) const
    {
        auto const& s = f.entry.at(id);
        auto inner = cc::set<i32>();
        auto const body = [&](ast::range_of<flat_stmt_id> r)
        {
            for (auto const x : f.entry.at(r))
                escapes(x, inner);
        };
        if (auto const* const n = s.node.try_as<flat_leave>())
            out.insert(i32(n->target));
        else if (auto const* const n = s.node.try_as<flat_if>())
        {
            body(n->then_body);
            body(n->else_body);
        }
        else if (auto const* const n = s.node.try_as<flat_block>())
        {
            body(n->body);
            inner.erase(i32(n->label));
        }
        else if (auto const* const n = s.node.try_as<flat_for>())
        {
            body(n->body);
            inner.erase(i32(n->label));
        }
        for (auto const l : inner)
            out.insert(l);
    }

    local_id flag_of(label_id label)
    {
        if (auto const* const flag = left_flag.get_ptr(i32(label)))
            return *flag;
        auto const flag = hoist(cc::format("left_{}", f.entry.at(label).name), f.bool_type(), boolean(false));
        bookkeeping.insert(i32(flag));
        left_flag[i32(label)] = flag;
        return flag;
    }

    /// The constant bounds of `n`, as its first index and how many it runs; none for a loop of other bounds.
    struct loop_bounds
    {
        i32 first = 0;
        i32 count = 0;
    };
    [[nodiscard]] cc::optional<loop_bounds> constant_bounds(flat_for const& n) const
    {
        auto const first = f.constant_int(n.first);
        auto const end = f.constant_int(n.end);
        if (!first.has_value() || !end.has_value())
            return {};
        return loop_bounds{.first = first.value(),
                           .count = end.value() > first.value() ? end.value() - first.value() : 0};
    }

    /// A value the forward sweep keeps for the backward one: one slot, or one per iteration of the kept loops around.
    local_id tape(cc::string_view name, type_id type, flat_expr_id initial)
    {
        if (kept_loops.empty())
            return hoist(name, type, initial);
        auto size = 1;
        for (auto const& l : kept_loops)
            size *= l.count > 0 ? l.count : 1;
        if (size > k_max_tape)
        {
            failed(cc::format("loops that run {} times between them, more than a tape holds ({})", size, k_max_tape));
            return hoist(name, type, initial);
        }
        // PROTOTYPE: the flattener is always made from a checker it may change, and an array type is interned there
        auto const array = const_cast<checker&>(f.c).array_type(type, size);
        return hoist(name, array, zero(array));
    }

    /// Where the current iteration's value of `tape` stands.
    flat_expr_id slot(local_id tape)
    {
        if (kept_loops.empty())
            return ref(tape);
        auto index = flat_expr_id::none;
        for (auto const& l : kept_loops)
        {
            auto const offset = l.first == 0 ? ref(l.index) : call("-", ref(l.index), integer(l.first));
            index = is_valid(index) ? call("+", call("*", index, integer(l.count)), offset) : offset;
        }
        auto const element = m().at(f.entry.at(tape).type).element;
        return add(element, flat_element{.object = ref(tape), .index = index});
    }

    /// `condition`, recorded where it is evaluated, for the backward sweep to branch on alike.
    flat_expr_id record(flat_expr_id condition)
    {
        auto const k = tape("taken", f.bool_type(), boolean(false));
        bookkeeping.insert(i32(k));
        emit(flat_assign{.place = slot(k), .value = condition});
        return slot(k);
    }

    /// `statements` with every leave a flag, what follows a leave guarded by it, every condition recorded, and every
    /// overwritten value saved first.
    cc::vector<flat_stmt_id> straighten(cc::span<flat_stmt_id const> statements)
    {
        auto const copy = cc::vector<flat_stmt_id>::create_copy_of(statements);
        return collect([&] { straighten_into(copy, 0); });
    }

    void straighten_into(cc::span<flat_stmt_id const> statements, isize from)
    {
        for (auto i = from; i < statements.size(); ++i)
        {
            auto const id = statements[i];
            auto const s = f.entry.at(id);
            if (auto const* const n = s.node.try_as<flat_assign>())
            {
                auto const root = root_of(n->place, nullptr);
                if (!is_valid(root) && !is_buffer_place(n->place))
                    return (void)failed("an assignment to a part of a local it cannot follow yet");
                if (is_valid(root) && !single_assigned.contains(i32(root)) && !bookkeeping.contains(i32(root)))
                {
                    auto const save = tape("saved", type_of(n->place), zero(type_of(n->place)));
                    bookkeeping.insert(i32(save));
                    emit(flat_assign{.place = slot(save), .value = clone(n->place)});
                    save_of[i32(id)] = slot(save);
                }
                f.block.push_back(id);
            }
            else if (auto const* const n = s.node.try_as<flat_leave>())
                emit(flat_assign{.place = ref(flag_of(n->target)), .value = boolean(true)});
            else if (auto const* const n = s.node.try_as<flat_if>())
            {
                auto const condition = record(n->condition);
                auto const then_body = f.add_list(straighten(copy_of(n->then_body)));
                auto const else_body = f.add_list(straighten(copy_of(n->else_body)));
                restate(id, flat_if{.condition = condition, .then_body = then_body, .else_body = else_body});
            }
            else if (auto const* const block = s.node.try_as<flat_block>();
                     block != nullptr && !f.custom_calls.contains(i32(block->label)))
            {
                auto body = straighten(copy_of(block->body));
                // a block in a loop runs again, and its leave of the last run must not guard this one
                if (auto const* const flag = left_flag.get_ptr(i32(block->label)))
                    body.insert_at(0, f.make_stmt(where, flat_assign{.place = ref(*flag), .value = boolean(false)}));
                restate(id, flat_block{.label = block->label, .body = f.add_list(body)});
            }
            else if (auto const* const n = s.node.try_as<flat_for>())
            {
                auto const bounds = constant_bounds(*n).value();
                auto left = cc::set<i32>();
                for (auto const x : f.entry.at(n->body))
                    escapes(x, left);
                // a break leaves to the loop, whose flag starts false each time the loop does
                if (left.contains(i32(n->label)))
                    emit(flat_assign{.place = ref(flag_of(n->label)), .value = boolean(false)});
                kept_loops.push_back({.index = n->index, .first = bounds.first, .count = bounds.count});
                auto body = straighten(copy_of(n->body));
                if (!left.empty())
                    body = collect(
                        [&]
                        {
                            auto still = flat_expr_id::none;
                            for (auto const l : left)
                            {
                                auto const stays = add(f.bool_type(), flat_not{.operand = ref(flag_of(label_id(l)))});
                                still = is_valid(still) ? add(f.bool_type(), flat_and{.lhs = still, .rhs = stays})
                                                        : stays;
                            }
                            auto const runs = record(still);
                            emit(flat_if{.condition = runs,
                                         .then_body = f.add_list(body),
                                         .else_body = f.add_list(cc::span<flat_stmt_id const>())});
                        });
                kept_loops.remove_back();
                restate(id, flat_for{.label = n->label,
                                     .index = n->index,
                                     .first = n->first,
                                     .end = n->end,
                                     .body = f.add_list(body)});
            }
            else
                f.block.push_back(id);

            auto escaped = cc::set<i32>();
            escapes(id, escaped);
            if (escaped.empty() || i + 1 == statements.size())
                continue;
            // what follows runs only where no leave of this statement was taken
            auto still = flat_expr_id::none;
            for (auto const l : escaped)
            {
                auto const stays = add(f.bool_type(), flat_not{.operand = ref(flag_of(label_id(l)))});
                still = is_valid(still) ? add(f.bool_type(), flat_and{.lhs = still, .rhs = stays}) : stays;
            }
            auto const condition = record(still);
            auto const rest = collect([&] { straighten_into(statements, i + 1); });
            emit(flat_if{.condition = condition,
                         .then_body = f.add_list(rest),
                         .else_body = f.add_list(cc::span<flat_stmt_id const>())});
            return;
        }
    }

    // ---- reverse: the backward sweep ------------------------------------------------------------------------------

    local_id adjoint_of_local(local_id local, flat_expr_id initial = flat_expr_id::none)
    {
        if (auto const* const a = adjoint_of.get_ptr(i32(local)))
            return *a;
        auto const type = linear_locals.contains(i32(local)) ? f.entry.at(local).type : tangent(f.entry.at(local).type);
        auto const a = f.add_local(local_kind::var, cc::format("a_{}", f.entry.at(local).name), type);
        adjoint_declarations.push_back(
            f.make_stmt(where, flat_var{.local = a, .value = is_valid(initial) ? initial : zero(type)}));
        adjoint_of[i32(local)] = a;
        return a;
    }

    [[nodiscard]] bool is_linear(flat_expr_id id)
    {
        if (!is_valid(id))
            return false;
        if (auto const* const known = linear_memo.get_ptr(i32(id)))
            return *known;
        auto const x = node(id);
        auto result = false;
        if (auto const* const r = x.node.try_as<flat_local_ref>())
            result = linear_locals.contains(i32(r->local));
        else if (auto const* const n = x.node.try_as<flat_member>())
            result = is_linear(n->object);
        else if (auto const* const n = x.node.try_as<flat_element>())
            result = is_linear(n->object);
        else if (auto const* const n = x.node.try_as<flat_construct>())
            for (auto const a : copy_of(n->arguments))
                result = result || is_linear(a);
        else if (auto const* const n = x.node.try_as<flat_call>())
            for (auto const a : copy_of(n->arguments))
                result = result || is_linear(a);
        linear_memo[i32(id)] = result;
        return result;
    }

    /// The local a linear leaf stands for: a placeholder's primal, or the linear let itself.
    [[nodiscard]] local_id adjoint_target(local_id leaf) const
    {
        auto const* const primal = primal_of_placeholder.get_ptr(i32(leaf));
        return primal != nullptr ? *primal : leaf;
    }

    /// The scalar sum of the components of `g`, shared.
    flat_expr_id sum_of_components(flat_expr_id g)
    {
        auto const t = type_of(g);
        auto result = flat_expr_id::none;
        for (auto i = 0; i < leaf_count(t); ++i)
            result = sum(result, add(f.field_type(t, i), flat_member{.object = use(g), .member = i}));
        return result;
    }

    /// The sum of the products of two shared matrices' elements, column by column.
    flat_expr_id inner_product(flat_expr_id a, flat_expr_id b)
    {
        auto const column_count = leaf_count(type_of(a)) / leaf_count(type_of(call("matrix_column", use(a), integer(0))));
        auto result = flat_expr_id::none;
        for (auto c = 0; c < column_count; ++c)
            result = sum(result, call("dot", call("matrix_column", use(a), integer(c)),
                                      call("matrix_column", use(b), integer(c))));
        return result;
    }

    /// `g`, shared, brought to the type of an operand that a builtin broadcast: a scalar from a vector is its sum.
    flat_expr_id reduce(flat_expr_id g, type_id to)
    {
        if (type_of(g) == to || !is_scalar(to) || !is_vector(type_of(g)))
            return use(g);
        return sum_of_components(g);
    }

    /// Component `k` of `g`, shared: a field of a vector, an entry of a matrix in column order, or `g` itself.
    flat_expr_id component(flat_expr_id g, i32 k)
    {
        auto const t = type_of(g);
        if (is_scalar(t))
            return use(g);
        if (is_vector(t))
            return add(f.field_type(t, k), flat_member{.object = use(g), .member = k});
        auto const column = call("matrix_column", use(g), integer(0));
        auto const rows = leaf_count(type_of(column));
        auto const c = call("matrix_column", use(g), integer(k / rows));
        return add(f.field_type(type_of(c), k % rows), flat_member{.object = c, .member = k % rows});
    }

    /// A value of `t` made of `parts`: a scalar, a vector of its components, or a matrix of columns of them.
    flat_expr_id assemble(type_id t, cc::span<flat_expr_id const> parts)
    {
        if (is_scalar(t))
            return parts[0];
        if (is_vector(t))
            return add(t, flat_construct{.arguments = f.add_list(parts)});
        auto const* const overloads = f.c.prelude_names.get_ptr(cc::string(m().name_of(t)));
        if (overloads == nullptr)
            return failed(cc::format("no way to build a {}", m().name_of(t)));
        for (auto const callee : *overloads)
        {
            auto const& s = m().at(callee);
            if (s.kind != symbol_kind::function || s.info < 0)
                continue;
            auto const& info = m().functions[s.info];
            auto const parameters = m().at(info.parameters);
            if (info.result != t || isize(parameters.size() * parameters.size()) != parts.size())
                continue;
            auto columns = cc::vector<flat_expr_id>();
            auto const rows = parameters.size();
            for (auto c = isize(0); c < rows; ++c)
                columns.push_back(assemble(parameters[c].type, parts.subspan({.offset = c * rows, .size = rows})));
            return f.builtin_call(at, callee, columns);
        }
        return failed(cc::format("no way to build a {}", m().name_of(t)));
    }

    /// A value of `t` that is zero but for the field or the swizzle `member` reads, which is `g`, shared.
    flat_expr_id one_hot(type_id t, flat_member const& member, flat_expr_id g)
    {
        auto const& info = m().at(t);
        auto parts = cc::vector<flat_expr_id>();
        for (auto j = 0; j < i32(info.members.count); ++j)
        {
            auto part = flat_expr_id::none;
            if (!member.is_swizzle())
                part = j == member.member ? use(g) : flat_expr_id::none;
            else
                for (auto k = 0; k < member.letters.count; ++k)
                    if (member.letters.fields[k] == j)
                        part = sum(part, add(f.field_type(t, j), flat_member{.object = use(g), .member = k}));
            parts.push_back(is_valid(part) ? part : zero(f.field_type(t, j)));
        }
        return add(t, flat_construct{.arguments = f.add_list(parts)});
    }

    /// Pushes `g`, shared, the adjoint of the linear expression `t`, onto the adjoints of what `t` is linear in.
    void transpose(flat_expr_id t, flat_expr_id g)
    {
        if (!is_valid(g) || !is_linear(t) || is_failed)
            return;
        auto const x = node(t);
        // a field or an element of a leaf: added where it stands
        {
            auto steps = cc::vector<place_step>();
            auto const root = root_of(t, &steps);
            if (is_valid(root) && linear_locals.contains(i32(root)))
            {
                auto const target = adjoint_of_local(adjoint_target(root));
                return accumulate([&] { return ref(target); }, cc::move(steps), g);
            }
        }
        if (auto const* const n = x.node.try_as<flat_member>())
            return transpose(n->object, share(one_hot(type_of(n->object), *n, g)));
        if (auto const* const n = x.node.try_as<flat_construct>())
        {
            auto const arguments = copy_of(n->arguments);
            auto const is_array = m().at(x.type).kind == type_kind::array;
            for (auto i = isize(0); i < arguments.size(); ++i)
            {
                if (!is_linear(arguments[i]))
                    continue;
                auto const part
                    = is_array ? add(type_of(arguments[i]), flat_element{.object = use(g), .index = integer(i32(i))})
                               : add(type_of(arguments[i]), flat_member{.object = use(g), .member = i32(i)});
                transpose(arguments[i], share(part));
            }
            return;
        }
        auto const* const n = x.node.try_as<flat_call>();
        if (n == nullptr)
            return (void)failed("a tangent it cannot transpose");
        auto const name = rule_name(n->callee);
        auto const arguments = copy_of(n->arguments);
        auto const linear_at = [&](isize i) { return i < arguments.size() && is_linear(arguments[i]); };
        auto const r = x.type;

        if ((name == "+" || name == "-") && arguments.size() == 1)
            return transpose(arguments[0], name == "+" ? g : share(negate(use(g))));
        if ((name == "+" || name == "-") && arguments.size() == 2)
        {
            if (linear_at(0))
                transpose(arguments[0], share(reduce(g, type_of(arguments[0]))));
            if (linear_at(1))
            {
                auto const negated = name == "-" ? share(negate(use(g))) : g;
                transpose(arguments[1], share(reduce(negated, type_of(arguments[1]))));
            }
            return;
        }
        if (name == "*" && arguments.size() == 2)
        {
            auto const k = linear_at(0) ? 0 : 1;
            auto const u = arguments[k];
            auto const w = share(arguments[1 - k], "w");
            auto const ut = type_of(u);
            auto const wt = type_of(w);
            auto gu = flat_expr_id::none;
            if (!is_matrix(ut) && !is_matrix(wt) && (ut == r || is_scalar(wt)))
                gu = mul(use(g), use(w));
            else if (is_scalar(ut) && is_vector(wt))
            {
                type_id const both[] = {r, wt};
                gu = is_valid(find("dot", both)) ? call("dot", use(g), use(w))
                                                 : sum_of_components(share(mul(use(g), use(w))));
            }
            else if (is_matrix(ut) && k == 0 && is_vector(wt))
            {
                // the outer product of the result's adjoint and the vector
                auto parts = cc::vector<flat_expr_id>();
                for (auto c = 0; c < leaf_count(wt); ++c)
                    for (auto row = 0; row < leaf_count(r); ++row)
                        parts.push_back(mul(component(g, row), component(w, c)));
                if (isize(leaf_count(ut)) != parts.size())
                    return (void)failed(cc::format("the adjoint of {} * {}", m().name_of(ut), m().name_of(wt)));
                gu = assemble(ut, parts);
            }
            else if (is_vector(ut) && k == 1 && is_matrix(wt))
                gu = call("*", call("transpose", use(w)), use(g));
            else if (is_matrix(ut) && is_scalar(wt))
                gu = mul(use(g), use(w));
            else if (is_scalar(ut) && is_matrix(wt))
                gu = inner_product(g, w);
            else if (is_matrix(ut) && is_matrix(wt))
                gu = k == 0 ? call("*", use(g), call("transpose", use(w))) : call("*", call("transpose", use(w)), use(g));
            else
                return (void)failed(cc::format("the adjoint of {} * {}", m().name_of(type_of(arguments[0])),
                                               m().name_of(type_of(arguments[1]))));
            return transpose(u, share(gu));
        }
        if (name == "/" && linear_at(0))
            return transpose(arguments[0], share(reduce(share(call("/", use(g), arguments[1])), type_of(arguments[0]))));
        if (name == "dot" && arguments.size() == 2)
        {
            auto const k = linear_at(0) ? 0 : 1;
            return transpose(arguments[k], share(mul(arguments[1 - k], use(g))));
        }
        if (name == "cross" && arguments.size() == 2)
        {
            auto const k = linear_at(0) ? 0 : 1;
            auto const w = arguments[1 - k];
            return transpose(arguments[k], share(k == 0 ? call("cross", w, use(g)) : call("cross", use(g), w)));
        }
        if (name == "transpose")
            return transpose(arguments[0], share(call("transpose", use(g))));
        if (name == "select" && arguments.size() == 3)
        {
            auto const condition = share(arguments[0]);
            if (linear_at(1))
                transpose(arguments[1], share(call("select", use(condition), use(g), zero(r))));
            if (linear_at(2))
                transpose(arguments[2], share(call("select", use(condition), zero(r), use(g))));
            return;
        }
        if (name == "matrix_column" && arguments.size() == 2)
        {
            // the matrix whose column `i` is `g`, and every other column zero
            auto const i = share(arguments[1]);
            auto const ut = type_of(arguments[0]);
            auto const columns = leaf_count(ut) / leaf_count(r);
            auto parts = cc::vector<flat_expr_id>();
            for (auto c = 0; c < columns; ++c)
            {
                auto const is_this = call("==", use(i), integer(c));
                auto const column = share(call("select", is_this, use(g), zero(r)));
                for (auto row = 0; row < leaf_count(r); ++row)
                    parts.push_back(component(column, row));
            }
            return transpose(arguments[0], share(assemble(ut, parts)));
        }
        if (is_linear_function(name, arguments.size()) && leaf_count(r) > 0)
        {
            // a construction or a conversion: each argument's adjoint is the components of `g` it filled
            auto total = 0;
            for (auto const a : arguments)
                total += leaf_count(type_of(a));
            auto offset = 0;
            for (auto const a : arguments)
            {
                auto const n_a = leaf_count(type_of(a));
                if (n_a < 0)
                    return (void)failed(cc::format("the adjoint of {}", name));
                if (is_linear(a))
                {
                    if (record_of(type_of(a))->leaf_kind != record_of(r)->leaf_kind)
                        return (void)failed(cc::format("the adjoint of {} between float widths", name));
                    auto parts = cc::vector<flat_expr_id>();
                    if (total == leaf_count(r))
                        for (auto k = 0; k < n_a; ++k)
                            parts.push_back(component(g, offset + k));
                    else if (arguments.size() == 1 && n_a == 1)
                        parts.push_back(sum_of_components(g));
                    else
                        return (void)failed(cc::format("the adjoint of {}", name));
                    transpose(a, share(assemble(type_of(a), parts)));
                }
                offset += n_a;
            }
            return;
        }
        failed(cc::format("no adjoint of {} yet", name));
    }

    /// The backward sweep of `statements`, last first, written into the list being written.
    void backward(cc::span<flat_stmt_id const> statements)
    {
        auto const copy = cc::vector<flat_stmt_id>::create_copy_of(statements);
        for (auto i = copy.size(); i-- > 0;)
            backward_stmt(copy[i]);
    }

    void backward_stmt(flat_stmt_id id)
    {
        auto const s = f.entry.at(id);
        if (auto const* const n = s.node.try_as<flat_assign>())
        {
            auto steps = cc::vector<place_step>();
            auto const root = root_of(clone(n->place), &steps);
            if (!is_valid(root) || bookkeeping.contains(i32(root)))
                return;
            if (auto const* const save = save_of.get_ptr(i32(id)))
                emit(flat_assign{.place = clone(n->place), .value = clone(*save)});
            if (!tracked.contains(i32(root)) || !is_differentiable(type_of(n->place)))
                return;
            auto const adjoint = adjoint_of_local(root);
            auto const g = share(place_of(ref(adjoint), steps), "g");
            emit(flat_assign{.place = place_of(ref(adjoint), steps), .value = zero(tangent(type_of(n->place)))});
            linear_lets.clear();
            auto const t = tangent_of_value(n->value);
            transpose(t, g);
            // a tangent read twice was bound once; its adjoint is complete only once every read is transposed
            for (auto k = linear_lets.size(); k-- > 0;)
            {
                auto const l = linear_lets[k];
                if (auto const* const a = adjoint_of.get_ptr(i32(l)))
                {
                    auto const adjoint = *a;
                    transpose(linear_definition.get(i32(l)), ref(adjoint));
                    // a statement in a kept loop runs again, and this adjoint is its alone
                    emit(flat_assign{.place = ref(adjoint), .value = zero(f.entry.at(adjoint).type)});
                }
            }
            return;
        }
        if (auto const* const n = s.node.try_as<flat_if>())
        {
            auto const then_body = collect([&] { backward(copy_of(n->then_body)); });
            auto const else_body = collect([&] { backward(copy_of(n->else_body)); });
            return emit(flat_if{.condition = clone(n->condition),
                                .then_body = f.add_list(then_body),
                                .else_body = f.add_list(else_body)});
        }
        // a kept loop is walked back from its last iteration, its index read reversed by every clone of its body
        if (auto const* const n = s.node.try_as<flat_for>())
        {
            auto const bounds = constant_bounds(*n).value();
            auto const back = f.add_local(local_kind::index, "back", f.int_type());
            auto const index = n->index;
            auto const body = copy_of(n->body);
            auto const reversed_body = collect(
                [&]
                {
                    auto const reversed = f.add_local(local_kind::let, f.entry.at(index).name, f.int_type());
                    emit(flat_let{.local = reversed,
                                  .value = call("-", integer(bounds.first + bounds.count - 1), ref(back))});
                    substitution[i32(index)] = reversed;
                    backward(body);
                    substitution.erase(i32(index));
                });
            return emit(flat_for{.label = f.add_label("back"),
                                 .index = back,
                                 .first = integer(0),
                                 .end = integer(bounds.count),
                                 .body = f.add_list(reversed_body)});
        }
        if (auto const* const n = s.node.try_as<flat_block>())
        {
            if (auto const* const custom = f.custom_calls.get_ptr(i32(n->label)))
                return backward_custom(*custom, n->label);
            auto const body = collect([&] { backward(copy_of(n->body)); });
            emit(flat_block{.label = f.add_label("back"), .body = f.add_list(body)});
        }
    }

    // ---- derivatives written by hand ------------------------------------------------------------------------------

    /// The parameters of the function a custom call calls.
    [[nodiscard]] cc::span<parameter const> parameters_of(symbol_id function) const
    {
        return m().at(m().functions[m().at(function).info].parameters);
    }

    /// True where `derivative`'s parameters are the custom call's own, each followed by one of its tangent's type, a
    /// `mut` one going backward, and backward the result's adjoint last.
    [[nodiscard]] bool fits(flattener::custom_call const& custom, symbol_id derivative, type_id result) const
    {
        auto const own = parameters_of(custom.function);
        auto const theirs = parameters_of(derivative);
        auto k = isize(0);
        for (auto i = isize(0); i < own.size(); ++i)
        {
            if (k >= theirs.size() || theirs[k++].type != own[i].type)
                return false;
            if (!f.c.takes_tangent(custom.function, i))
                continue;
            if (k >= theirs.size() || theirs[k].type != tangent(own[i].type) || theirs[k].is_mut != is_reverse)
                return false;
            ++k;
        }
        auto const& info = m().functions[m().at(derivative).info];
        if (is_reverse)
            return k + 1 == theirs.size() && theirs[k].type == tangent(result)
                && info.result == checked_module::void_type;
        return k == theirs.size() && info.result == tangent(result);
    }

    /// The function `derivative`, inlined over `arguments`, as the block it becomes.
    flat_expr_id inline_derivative(flattener::custom_call const& custom,
                                   symbol_id derivative,
                                   cc::span<flat_expr_id const> arguments)
    {
        auto const result = m().functions[m().at(derivative).info].result;
        auto const inlined = f.inline_call(custom.call, derivative, arguments);
        if (!is_valid(inlined.label))
            return failed(cc::format("{}, which did not inline", m().at(derivative).name));
        return add(result, flat_block{.label = inlined.label, .body = inlined.body});
    }

    /// The call as it is, then its result's tangent from `@forward`.
    void forward_custom(flat_stmt_id id, flattener::custom_call const& custom, label_id label)
    {
        f.block.push_back(id);
        // `no_diff(x)`: the value as it is, and its tangent zero
        if (custom.is_detached)
            return;
        auto const* const result = lifted.get_ptr(i32(label));
        auto const* const result_tangent = result != nullptr ? tangent_of.get_ptr(i32(*result)) : nullptr;
        if (result_tangent == nullptr)
            return;
        auto const tangent_local = *result_tangent;
        auto const result_type = f.entry.at(*result).type;
        if (!is_valid(custom.derivatives.forward))
            return (void)failed(cc::format("{} has a @backward and no @forward", m().at(custom.function).name));
        if (!fits(custom, custom.derivatives.forward, result_type))
            return (void)failed(cc::format("@forward({}) takes each parameter and its tangent, and gives the result",
                                           m().at(custom.function).name));
        auto arguments = cc::vector<flat_expr_id>();
        for (auto i = isize(0); i < custom.arguments.size(); ++i)
        {
            auto const a = custom.arguments[i];
            arguments.push_back(ref(a));
            if (!f.c.takes_tangent(custom.function, i))
                continue;
            auto const* const t = tangent_of.get_ptr(i32(a));
            arguments.push_back(t != nullptr ? ref(*t) : zero(tangent(f.entry.at(a).type)));
        }
        emit(flat_assign{.place = ref(tangent_local),
                         .value = inline_derivative(custom, custom.derivatives.forward, arguments)});
    }

    /// `@backward`, handed the call's arguments, their adjoints as places, and its result's adjoint.
    void backward_custom(flattener::custom_call const& custom, label_id label)
    {
        auto const* const result = lifted.get_ptr(i32(label));
        if (custom.is_detached || result == nullptr || !tracked.contains(i32(*result)))
            return;
        auto const result_local = *result;
        auto const result_type = f.entry.at(result_local).type;
        if (!is_valid(custom.derivatives.backward))
            return (void)failed(cc::format("{} has a @forward and no @backward", m().at(custom.function).name));
        if (!fits(custom, custom.derivatives.backward, result_type))
            return (void)failed(cc::format("@backward({}) takes each parameter and its adjoint as `mut`, then the "
                                           "result's adjoint",
                                           m().at(custom.function).name));
        auto const adjoint = adjoint_of_local(result_local);
        auto const g = share(ref(adjoint), "g");
        emit(flat_assign{.place = ref(adjoint), .value = zero(tangent(result_type))});
        auto arguments = cc::vector<flat_expr_id>();
        for (auto i = isize(0); i < custom.arguments.size(); ++i)
        {
            auto const a = custom.arguments[i];
            arguments.push_back(ref(a));
            if (!f.c.takes_tangent(custom.function, i))
                continue;
            // what is not followed takes its adjoint into a local nobody reads
            auto const place = tracked.contains(i32(a))
                                 ? adjoint_of_local(a)
                                 : f.add_local(local_kind::var, "ignored", tangent(f.entry.at(a).type));
            if (!tracked.contains(i32(a)))
                emit(flat_var{.local = place, .value = zero(tangent(f.entry.at(a).type))});
            arguments.push_back(ref(place));
        }
        arguments.push_back(use(g));
        auto const call = inline_derivative(custom, custom.derivatives.backward, arguments);
        if (is_valid(call))
            emit(flat_block{.label = node(call).node.as<flat_block>().label,
                            .body = node(call).node.as<flat_block>().body});
    }
};
} // namespace
} // namespace sgl::check::impl

auto sgl::check::impl::flattener::custom_derivatives_of(symbol_id function) const -> custom_derivatives
{
    // PROTOTYPE: matched by name, since an attribute's argument is no expression the check pass resolves yet
    auto result = custom_derivatives();
    auto const& target = c.out.at(function);
    for (auto i = isize(0); i < c.out.symbols.size(); ++i)
    {
        auto const& s = c.out.symbols[i];
        if (s.kind != symbol_kind::function || !ast::is_valid(s.declaration))
            continue;
        auto const& declaration = c.ast_of(s.file).at(s.declaration);
        if (!declaration.node.is<ast::fun_decl>())
            continue;
        for (auto const which : {cc::string_view("forward"), cc::string_view("backward")})
        {
            auto const* const a = c.find_attribute(s.file, declaration.attributes, which);
            if (a == nullptr)
                continue;
            auto const arguments = c.ast_of(s.file).at(a->arguments);
            if (arguments.size() != 1 || !ast::is_valid(arguments[0].value)
                || c.text_of(s.file, c.span_of(s.file, arguments[0].value)) != target.name)
                continue;
            (which == "forward" ? result.forward : result.backward) = symbol_id(i);
        }
    }
    return result;
}

auto sgl::check::impl::flattener::flatten_custom_call(ast::expr_id id,
                                                      type_id type,
                                                      call_record const& record,
                                                      custom_derivatives d,
                                                      cc::span<flat_expr_id const> values) -> flat_expr_id
{
    auto const slots = c.out.at(record.slots);
    auto const parameters = c.out.at(c.out.functions[c.out.at(record.callee).info].parameters);
    auto arguments = cc::vector<local_id>::create_filled(parameters.size(), local_id::none);
    // each argument in a local of its own, evaluated in the order written, which the derivative then reads
    for (auto k = isize(0); k < values.size(); ++k)
    {
        if (slots[k] < 0 || slots[k] >= parameters.size() || !is_valid(values[k]) || parameters[slots[k]].is_mut)
            return fail();
        auto const local = add_local(local_kind::let, parameters[slots[k]].name, entry.at(values[k]).type);
        add_stmt({.file = file(), .expr = id}, flat_let{.local = local, .value = values[k]});
        arguments[slots[k]] = local;
    }
    auto references = cc::vector<flat_expr_id>();
    for (auto const a : arguments)
    {
        if (!is_valid(a))
            return fail();
        references.push_back(local_ref(a, id));
    }
    auto const inlined = inline_call(id, record.callee, references);
    if (!is_valid(inlined.label))
        return fail();
    custom_calls[i32(inlined.label)]
        = {.function = record.callee, .derivatives = d, .arguments = cc::move(arguments), .call = id};
    return add_expr(type, id, flat_block{.label = inlined.label, .body = inlined.body});
}

auto sgl::check::impl::flattener::is_detach_call(symbol_id callee) const -> bool
{
    auto const& s = c.out.at(callee);
    return s.name == "no_diff" && c.is_prelude_file(s.file);
}

auto sgl::check::impl::flattener::is_diff_call(symbol_id callee) const -> bool
{
    auto const& s = c.out.at(callee);
    return (s.name == "fwd_diff" || s.name == "bwd_diff") && c.is_prelude_file(s.file);
}

auto sgl::check::impl::flattener::flatten_diff(ast::expr_id id, type_id type, call_record const& record) -> flat_expr_id
{
    auto d = differentiator{.f = *this, .is_reverse = c.out.at(record.callee).name == "bwd_diff", .at = id};
    d.where = origin{.file = file(), .expr = id};

    auto const values = flatten_written(c.out.at(record.written));
    auto const handed = written_closures;
    auto const slots = c.out.at(record.slots);
    auto const* const signature = c.derivative_signatures.get_ptr(i32(record.callee));
    if (signature == nullptr)
        return d.failed("a lambda or a parameter handed on, where it can differentiate a function's name alone");
    auto const arity = signature->value_slots.size();
    auto function_slot = isize(-1);
    for (auto k = isize(0); k < slots.size(); ++k)
        if (slots[k] == 0)
            function_slot = k;
    if (function_slot < 0 || function_slot >= handed.size() || handed[function_slot] < 0)
        return d.failed("the function to differentiate is no function it can inline");

    auto const label = add_label(d.is_reverse ? "bwd_diff" : "fwd_diff");
    auto outer = cc::move(block);
    block = {};
    auto const give_up = [&](cc::string_view why)
    {
        block = cc::move(outer);
        return d.failed(why);
    };

    // every argument is evaluated once, in the order written, before anything else
    auto primals = cc::vector<local_id>();
    auto seconds = cc::vector<flat_expr_id>();
    primals.resize_to_filled(arity, local_id::none);
    seconds.resize_to_filled(arity, flat_expr_id::none);
    auto result_adjoint = flat_expr_id::none;
    for (auto k = isize(0); k < values.size(); ++k)
    {
        auto const p = slots[k];
        if (p <= 0 || !is_valid(values[k]))
            continue;
        if (p == signature->result_slot)
        {
            result_adjoint = d.share(values[k], "dr");
            continue;
        }
        for (auto i = isize(0); i < arity; ++i)
        {
            if (p == signature->value_slots[i])
            {
                auto const local = add_local(local_kind::let, "x", entry.at(values[k]).type);
                add_stmt(d.where, flat_let{.local = local, .value = values[k]});
                primals[i] = local;
            }
            else if (p == signature->tangent_slots[i])
                seconds[i] = d.is_reverse ? pin_place(values[k]) : d.share(values[k], "dx");
        }
    }
    for (auto const p : primals)
        if (!is_valid(p))
            return give_up("an argument it could not bind");

    // the function, inlined over the locals
    auto references = cc::vector<flat_expr_id>();
    auto identity = cc::vector<i32>();
    auto no_closures = cc::vector<i32>();
    for (auto i = isize(0); i < arity; ++i)
    {
        references.push_back(local_ref(primals[i], id));
        identity.push_back(i32(i));
        no_closures.push_back(-1);
    }
    auto const result_type = c.out.functions[c.out.at(signature->function).info].result;
    ++diff_depth;
    auto const inlined = call_closure(id, result_type, handed[function_slot], references, identity, no_closures);
    --diff_depth;
    if (!is_valid(inlined))
        return give_up("the function, which did not inline");
    auto const result = add_local(local_kind::let, "result", result_type);
    auto const unlowered = make_stmt(d.where, flat_let{.local = result, .value = inlined});
    auto const seeds = cc::move(block);
    block = {};
    auto const body = d.collect([&] { d.lower_list(cc::span<flat_stmt_id const>(&unlowered, 1)); });

    auto statements = seeds;
    if (!d.is_reverse)
    {
        for (auto i = isize(0); i < arity; ++i)
            if (d.is_differentiable(entry.at(primals[i]).type) && is_valid(seconds[i]))
            {
                auto const t = add_local(local_kind::let, "dx", entry.at(seconds[i]).type);
                statements.push_back(make_stmt(d.where, flat_let{.local = t, .value = d.use(seconds[i])}));
                d.tangent_of[i32(primals[i])] = t;
            }
        for (auto const s : d.forward(body))
            statements.push_back(s);
        auto const* const t = d.tangent_of.get_ptr(i32(result));
        auto const tangent = t != nullptr ? local_ref(*t, id) : d.zero(type);
        statements.push_back(make_stmt(d.where, flat_leave{.target = label, .value = tangent}));
    }
    else
    {
        for (auto i = isize(0); i < arity; ++i)
            if (d.is_differentiable(entry.at(primals[i]).type) && is_valid(seconds[i]))
                d.tracked.insert(i32(primals[i]));
        auto const flat = d.hoist_declarations(body);
        auto const straight = d.straighten(flat);
        d.adjoint_of_local(result, d.use(result_adjoint));
        auto const sweep = d.collect([&] { d.backward(straight); });
        auto const outputs = d.collect(
            [&]
            {
                for (auto i = isize(0); i < arity; ++i)
                {
                    auto const* const a = d.adjoint_of.get_ptr(i32(primals[i]));
                    if (a == nullptr || !is_valid(seconds[i]))
                        continue;
                    auto const adjoint = *a;
                    auto const place = seconds[i];
                    d.accumulate([&] { return again(place, id); }, {}, d.share(local_ref(adjoint, id)));
                }
            });
        cc::vector<flat_stmt_id> const* const parts[]
            = {&d.hoisted, &straight, &d.adjoint_declarations, &sweep, &outputs};
        for (auto const* const part : parts)
            for (auto const s : *part)
                statements.push_back(s);
    }
    block = cc::move(outer);
    if (d.is_failed)
        return flat_expr_id::none;
    return add_expr(type, id, flat_block{.label = label, .body = add_list(statements)});
}
