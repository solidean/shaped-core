#pragma once

#include <clean-core/container/map.hh>
#include <clean-core/container/set.hh>
#include <shaped-graphics-language/check/impl/flattener.hh>

// PROTOTYPE of libs/graphics/shaped-graphics-language/docs/spec/incubator/autodiff.md.
// The differentiator writes `fwd_diff` and `bwd_diff` as more flat tree; flatten_diff.cc says how.

namespace sgl::check::impl
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
        /// The slots it takes of a tape: its iterations, or a segment's where it is checkpointed.
        i32 count = 0;
        /// A checkpointed loop's iterations per segment, whose tapes hold one segment; 0 for a loop that stores all.
        i32 segment = 0;
    };
    cc::vector<kept_loop> kept_loops;
    /// Reverse: a kept loop too long to store every iteration of, walked back a segment at a time (DIFF-21).
    /// Going forward it saves its state at each segment's start; going backward each segment, last first, starts from
    /// its save, runs again to record its tapes, and is walked back.
    struct checkpointed_loop
    {
        i32 first = 0;
        i32 count = 0;
        i32 segment = 0;
        i32 segments = 0;
        /// What the body assigns, but its tapes, and the array of each one's value at each segment's start.
        cc::vector<local_id> state;
        cc::vector<local_id> saved;
        /// The straightened body, which runs again and is walked back.
        cc::vector<flat_stmt_id> body;
    };
    /// By the loop's label.
    cc::map<i32, checkpointed_loop> checkpointed;
    /// Reverse: the tapes, which a checkpointed loop writes again rather than saves.
    cc::set<i32> tapes;
    /// Reverse: how many loops the declarations being hoisted stand in; a `let` in one is assigned once per iteration.
    i32 loop_depth = 0;
    /// Reverse: a local a clone reads in place of another: a loop's index, going backward, its reversed twin.
    cc::map<i32, local_id> substitution;
    /// The most bytes a tape holds, which bounds what a kept loop keeps per value it overwrites, and what a
    /// checkpointed loop keeps of each value at its segments' starts: 1024 floats.
    static constexpr i32 k_max_tape_bytes = 4096;
    /// Reverse: the flag a leave of a label sets.
    cc::map<i32, local_id> left_flag;
    /// Reverse: every declaration hoisted to the top of the body.
    cc::vector<flat_stmt_id> hoisted;

    /// The local a lifted block's value is assigned to, by the block's label.
    cc::map<i32, local_id> lifted;

    /// A buffer a `@gradient` or `@tangent` names: a binding's member.
    struct paired_buffer
    {
        symbol_id binding = symbol_id::none;
        i32 member = -1;
        type_id type = type_id::none;
    };
    /// Reverse: a read of a buffer with a `@gradient`, in the statement being transposed.
    /// Its placeholder's adjoint is added to the gradient's element at the read's index, shared.
    struct gradient_read
    {
        local_id placeholder = local_id::none;
        paired_buffer gradients;
        flat_expr_id index = flat_expr_id::none;
        /// The floats an element holds, each an atomic of the gradient: element `i`'s `k`-th at `i * stride + k`.
        i32 stride = 1;
    };
    cc::vector<gradient_read> gradient_reads;
    /// Reverse: a read of a texture with a `@gradient`, in the statement being transposed; its taps are read again
    /// where its adjoint is added, at their weights.
    struct texture_read
    {
        local_id placeholder = local_id::none;
        paired_buffer gradients;
        /// The texture member, which a read names again, and the coordinate or the texel, shared.
        flat_expr_id texture = flat_expr_id::none;
        flat_expr_id at = flat_expr_id::none;
        bool is_load = false;
        bool is_linear = false;
        i32 mode_u = 0;
        i32 mode_v = 0;
        /// A gather's: the channel it reads of each of its four texels, whose adjoints are the placeholder's four; -1
        /// for a load and a sample.
        i32 gather_channel = -1;
        i32 channels = 0;
    };
    cc::vector<texture_read> texture_reads;

    [[nodiscard]] checked_module const& m() const { return f.c.out; }
    [[nodiscard]] flat_expr const& node(flat_expr_id id) const { return f.entry.at(id); }
    [[nodiscard]] type_id type_of(flat_expr_id id) const { return f.entry.at(id).type; }

    // ---- failing --------------------------------------------------------------------------------------------------

    flat_expr_id failed(cc::string_view why);

    // ---- types ----------------------------------------------------------------------------------------------------

    [[nodiscard]] builtins::type_record const* record_of(type_id t) const { return m().builtin_type_of(t); }

    [[nodiscard]] bool is_differentiable(type_id t) const { return f.c.is_differentiable_type(t); }
    /// What a change of a value of `t` is: a position's is a direction.
    // PROTOTYPE: the flattener is always made from a checker it may change, and a tangent struct is minted there
    [[nodiscard]] type_id tangent(type_id t) const { return const_cast<checker&>(f.c).tangent_type(t); }
    /// The field of `t`'s tangent that its field `member` changes, -1 where none does; a vector's is its own.
    [[nodiscard]] i32 tangent_member(type_id t, i32 member) const
    {
        if (record_of(t) != nullptr || m().at(t).kind != type_kind::structure)
            return member;
        return const_cast<checker&>(f.c).tangent_member(t, member);
    }
    /// Whether part `i` of a value of `t` has a part of `t`'s tangent: an element or a vector's field always does.
    [[nodiscard]] bool is_part_kept(type_id t, isize i) const { return tangent_member(t, i32(i)) >= 0; }

    [[nodiscard]] i32 leaf_count(type_id t) const;
    [[nodiscard]] bool is_scalar(type_id t) const { return leaf_count(t) == 1; }
    [[nodiscard]] bool is_vector(type_id t) const { return leaf_count(t) > 1 && m().at(t).members.count > 0; }
    [[nodiscard]] bool is_matrix(type_id t) const { return leaf_count(t) > 1 && m().at(t).members.count == 0; }

    /// The type of a prelude struct named `name`, such as `float`.
    [[nodiscard]] type_id named_type(cc::string_view name) const;

    /// The scalar a value of `t` is made of: itself, a vector's field, and `float` for a matrix.
    [[nodiscard]] type_id scalar_of(type_id t) const;

    /// The scalars of a value of `t` that is `v` in every float and zero, the first case, or false everywhere else.
    bool leaves_of(type_id t, f32 v, cc::vector<scalar>& out) const;

    /// A value of `t` that is `v` in every float leaf; none for a type that holds a resource.
    flat_expr_id constant(type_id t, f32 v);
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

    [[nodiscard]] cc::vector<flat_stmt_id> copy_of(ast::range_of<flat_stmt_id> r) const;
    [[nodiscard]] cc::vector<flat_expr_id> copy_of(ast::range_of<flat_expr_id> r) const;

    /// `x` in a form every read of it may copy: itself where it is a literal or an immutable local, a `let` otherwise.
    /// Backward, a tangent is never bound by a `let`, which would place it in the tree: it becomes a linear let.
    flat_expr_id share(flat_expr_id x, cc::string_view name = "v");
    flat_expr_id use(flat_expr_id shared) { return is_valid(shared) ? f.again(shared, at) : flat_expr_id::none; }

    /// A tangent read several times: a `let` going forward, and a linear let the backward sweep transposes through.
    flat_expr_id share_tangent(flat_expr_id t);

    /// A deep copy of the block-free expression `id`, which reads the locals it reads.
    flat_expr_id clone(flat_expr_id id);

    // ---- calls ----------------------------------------------------------------------------------------------------

    [[nodiscard]] static bool is_operator(cc::string_view name);

    /// The builtin `name` that takes exactly `types`.
    [[nodiscard]] symbol_id find(cc::string_view name, cc::span<type_id const> types) const;

    flat_expr_id call(cc::string_view name, cc::span<flat_expr_id const> arguments);

    flat_expr_id call(cc::string_view name, flat_expr_id a);
    flat_expr_id call(cc::string_view name, flat_expr_id a, flat_expr_id b);
    flat_expr_id call(cc::string_view name, flat_expr_id a, flat_expr_id b, flat_expr_id c);

    /// `a * b`, or `b * a` where one side is a scalar and only that order exists.
    flat_expr_id mul(flat_expr_id a, flat_expr_id b);
    /// The sum of two terms of one type, either of which may be zero.
    flat_expr_id sum(flat_expr_id a, flat_expr_id b);
    flat_expr_id negate(flat_expr_id a) { return is_valid(a) ? call("-", a) : a; }
    flat_expr_id difference(flat_expr_id a, flat_expr_id b);
    /// `1 - x`, of `x`'s type.
    flat_expr_id one_minus(flat_expr_id x) { return call("-", one(type_of(x)), x); }

    // ---- lowering -------------------------------------------------------------------------------------------------

    void lower_list(cc::span<flat_stmt_id const> statements);
    cc::vector<flat_stmt_id> lowered(ast::range_of<flat_stmt_id> body);

    void lower_stmt(flat_stmt_id id);

    [[nodiscard]] bool is_lifted_result(local_id local) const;

    /// `id` with every block it evaluates eagerly lifted into a statement block ahead of it.
    /// The right side of `and` and `or` stays where it is, since it may not run.
    flat_expr_id lift(flat_expr_id id);

    // ---- tangents -------------------------------------------------------------------------------------------------

    /// The tangent a read of `local` stands for: its tangent local going forward, its placeholder going backward.
    flat_expr_id tangent_leaf(local_id local);

    /// `id`, consumed, as its value and its tangent, each to be placed once.
    dual derive(flat_expr_id id);

    /// The buffer `text`, `binding.member`, that the `@<which>` of a buffer of `element`s names, checked for its role.
    cc::optional<paired_buffer> resolve_paired(cc::string_view text, cc::string_view which, type_id element);
    /// A read of a buffer element: its tangent from the buffer's `@tangent` going forward, and a placeholder whose
    /// adjoint goes to its `@gradient` going backward; refused without either.
    dual derive_buffer_read(flat_expr_id id, type_id type, flat_buffer_element const& n);
    /// Reverse: each gradient read of the statement just transposed, added to its gradient at its index.
    void add_gradient_reads();
    /// The float leaves of a value of `t`, in the order a gradient buffer holds them; -1 where one is no `float`.
    [[nodiscard]] i32 float_leaves(type_id t) const;
    /// Each float leaf of the shared value `g`, in that order.
    void leaves_into(flat_expr_id g, cc::vector<flat_expr_id>& out);
    /// A texture_2d member's `load` at a level or its `sample` at level 0 through a static sampler: going forward the
    /// texels' change from the `@tangent` texture and the coordinate's through the taps' slopes, and going backward a
    /// placeholder whose adjoint goes to the `@gradient` at the taps' weights.
    flat_expr_id texture_tangent(flat_call const& n,
                                 cc::span<flat_expr_id const> primals,
                                 cc::span<flat_expr_id const> tangents,
                                 type_id type);
    /// The state of a static sampler, a member's or the file's; null for one bound at run time.
    [[nodiscard]] sampler_state const* static_sampler_of(flat_expr_id smp) const;
    /// A texture member's `gather` through a static sampler: its texels' changes, and no coordinate's.
    flat_expr_id gather_tangent(flat_call const& n,
                                cc::span<flat_expr_id const> primals,
                                member_info const& info,
                                type_id texture_type);
    /// A comparison sample at level 0 through a static sampler: its coordinate's change under a linear filter.
    flat_expr_id compare_tangent(cc::span<flat_expr_id const> primals, cc::span<flat_expr_id const> tangents);
    /// `call(name, arguments)` over the invocations that reach it (`flat_call::is_active_lanes`).
    flat_expr_id active_call(cc::string_view name, cc::span<flat_expr_id const> arguments);
    /// The builtin `name` that takes `texture` first and `arity` arguments: one of its exact type, or of its shape.
    [[nodiscard]] symbol_id texture_builtin(cc::string_view name, type_id texture, isize arity) const;
    /// A call of the prelude's SGL function `name`, inlined where it stands.
    flat_expr_id prelude_call(cc::string_view name, cc::span<flat_expr_id const> arguments);
    /// Reverse: each texture read of the statement just transposed, its adjoint added to its gradient at each tap.
    void add_texture_reads();
    /// The bytes a value of `t` takes on a tape, 4 a scalar.
    [[nodiscard]] i32 bytes_of(type_id t) const;
    /// The straightened `statements` once more, every read of a substituted local reading its twin; refused where one
    /// has an effect, which running again would repeat.
    cc::vector<flat_stmt_id> run_again(cc::span<flat_stmt_id const> statements);
    /// Reverse: a checkpointed loop, walked back a segment at a time.
    void backward_checkpointed(flat_for const& n, checkpointed_loop const& loop);
    /// Whether a gradient's atomics are shared through the subgroup first: where the entry point declares `subgroups`,
    /// outside a test, whose run has no device.
    [[nodiscard]] bool reduces_gradients() const;
    /// The tangent of an affine3's product with a position (`w` 1) or a direction or a float4 (`w` 0), one dot of a
    /// row per component.
    flat_expr_id affine_tangent(cc::span<flat_expr_id const> primals,
                                cc::span<flat_expr_id const> tangents,
                                type_id type,
                                f64 w);
    /// A `case` as the chain of `if` it means, its scrutinee held in a `let` (LEGAL-46).
    cc::vector<flat_stmt_id> case_as_ifs(flat_case const& n);
    /// `call(name, a)` over the invocations that reach it (`flat_call::is_active_lanes`).
    flat_expr_id active_call(cc::string_view name, flat_expr_id a);
    /// The prelude's `add` of an atomic of `element`s; none where there is none.
    [[nodiscard]] symbol_id atomic_add_of(type_id element) const;

    /// The name a call's rule is chosen by: an operator's spelling, a function's name.
    [[nodiscard]] cc::string_view rule_name(symbol_id callee) const;

    /// What the registry states of a builtin's derivative; `unstated` for anything else.
    [[nodiscard]] builtins::derivative_kind derivative_of(symbol_id callee) const;

    /// The factor each argument's tangent is multiplied by, for a builtin that acts on each component on its own.
    /// `a` holds the arguments and `r` the result, each shared; none where a rule has no factor for that argument.
    bool componentwise_factors(cc::string_view name,
                               cc::span<flat_expr_id const> a,
                               flat_expr_id r,
                               cc::vector<flat_expr_id>& factors);

    /// The tangent of `m * p`, which is `dm * (p, 1) + m * (dp, 0)` as a `float4`: the change of `p` takes no translation.
    flat_expr_id transform_position_tangent(cc::span<flat_expr_id const> primals,
                                            cc::span<flat_expr_id const> tangents,
                                            type_id type);

    dual derive_call(type_id type, flat_call const& n);

    /// The tangent of the block-free value `value`, read where it stands; the value itself is left in place.
    flat_expr_id tangent_of_value(flat_expr_id value) { return derive(clone(value)).tangent; }

    // ---- places ---------------------------------------------------------------------------------------------------

    /// The local `place` is a part of, and the steps from it down to the place; none for a buffer's element.
    local_id root_of(flat_expr_id place, cc::vector<place_step>* steps);

    /// A place in a buffer, which a derivative leaves alone: what a shader stores is no value it follows.
    [[nodiscard]] bool is_buffer_place(flat_expr_id place) const;

    /// What indexing a value of `t` reads: an array's element, or a vector's component.
    [[nodiscard]] type_id element_type(type_id t) const;

    /// `steps` below a fresh read of `root`.
    flat_expr_id place_of(flat_expr_id root, cc::span<place_step const> steps);

    /// `steps` of a primal place below a fresh read of `root`, its tangent, whose struct fields `tangent_member` maps;
    /// none where a step reaches a field no tangent holds.
    flat_expr_id tangent_place_of(flat_expr_id root, cc::span<place_step const> steps);

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
        // an affine3 has no `+`, and is added row by row
        if (place_type == type_of(g) && m().name_of(place_type) == "affine3")
        {
            flat_expr_id rows[3] = {};
            for (auto i = 0; i < 3; ++i)
                rows[i] = call("+", call("affine3_row", place_of(root(), steps), integer(i)),
                               call("affine3_row", use(g), integer(i)));
            return emit(flat_assign{.place = place_of(root(), steps),
                                    .value = call("affine3_of_rows", rows[0], rows[1], rows[2])});
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

    cc::vector<flat_stmt_id> forward(cc::span<flat_stmt_id const> statements);
    cc::vector<flat_stmt_id> forward(ast::range_of<flat_stmt_id> body) { return forward(copy_of(body)); }

    void forward_stmt(flat_stmt_id id);

    // ---- reverse: straight structured code ------------------------------------------------------------------------

    /// A local the reverse pass declares at the top of the body.
    local_id hoist(cc::string_view name, type_id type, flat_expr_id value);

    /// Every `let` and `var` of `statements` declared at the top instead, and assigned where it stood.
    cc::vector<flat_stmt_id> hoist_declarations(cc::span<flat_stmt_id const> statements);

    /// `statements` with every `continue` of the loop `loop` a leave of `iteration`, the block around its body.
    cc::vector<flat_stmt_id> continues_as_leaves(cc::span<flat_stmt_id const> statements,
                                                 label_id loop,
                                                 label_id iteration);

    /// The labels a run of `id` may leave that it does not itself declare.
    void escapes(flat_stmt_id id, cc::set<i32>& out) const;

    local_id flag_of(label_id label);

    /// The constant bounds of `n`, as its first index and how many it runs; none for a loop of other bounds.
    struct loop_bounds
    {
        i32 first = 0;
        i32 count = 0;
    };
    [[nodiscard]] cc::optional<loop_bounds> constant_bounds(flat_for const& n) const;
    /// How many iterations of the outermost kept loop `n` a segment holds: 0 where every iteration fits a tape, and -1
    /// where no segment does.
    [[nodiscard]] i32 segment_of(flat_for const& n, loop_bounds const& bounds) const;

    /// A value the forward sweep keeps for the backward one: one slot, or one per iteration of the kept loops around.
    local_id tape(cc::string_view name, type_id type, flat_expr_id initial);

    /// Where the current iteration's value of `tape` stands.
    flat_expr_id slot(local_id tape);

    /// `condition`, recorded where it is evaluated, for the backward sweep to branch on alike.
    flat_expr_id record(flat_expr_id condition);

    /// `statements` with every leave a flag, what follows a leave guarded by it, every condition recorded, and every
    /// overwritten value saved first.
    cc::vector<flat_stmt_id> straighten(cc::span<flat_stmt_id const> statements);

    void straighten_into(cc::span<flat_stmt_id const> statements, isize from);

    // ---- reverse: the backward sweep ------------------------------------------------------------------------------

    local_id adjoint_of_local(local_id local, flat_expr_id initial = flat_expr_id::none);

    [[nodiscard]] bool is_linear(flat_expr_id id);

    /// The local a linear leaf stands for: a placeholder's primal, or the linear let itself.
    [[nodiscard]] local_id adjoint_target(local_id leaf) const;

    /// The scalar sum of the components of `g`, shared.
    flat_expr_id sum_of_components(flat_expr_id g);

    /// The sum of the products of two shared matrices' elements, column by column.
    flat_expr_id inner_product(flat_expr_id a, flat_expr_id b);

    /// `g`, shared, brought to the type of an operand that a builtin broadcast: a scalar from a vector is its sum.
    flat_expr_id reduce(flat_expr_id g, type_id to);

    /// Component `k` of `g`, shared: a field of a vector, an entry of a matrix in column order, or `g` itself.
    flat_expr_id component(flat_expr_id g, i32 k);

    /// A value of `t` made of `parts`: a scalar, a vector of its components, or a matrix of columns of them.
    flat_expr_id assemble(type_id t, cc::span<flat_expr_id const> parts);

    /// A value of `t` that is zero but for the field or the swizzle `member` reads, which is `g`, shared.
    flat_expr_id one_hot(type_id t, flat_member const& member, flat_expr_id g);

    /// Pushes `g`, shared, the adjoint of the linear expression `t`, onto the adjoints of what `t` is linear in.
    void transpose(flat_expr_id t, flat_expr_id g);

    /// The backward sweep of `statements`, last first, written into the list being written.
    void backward(cc::span<flat_stmt_id const> statements);

    void backward_stmt(flat_stmt_id id);

    // ---- derivatives written by hand ------------------------------------------------------------------------------

    /// The parameters of the function a custom call calls.
    [[nodiscard]] cc::span<parameter const> parameters_of(symbol_id function) const;

    /// True where `derivative`'s parameters are the custom call's own, each followed by one of its tangent's type, a
    /// `mut` one going backward, and backward the result's adjoint last.
    [[nodiscard]] bool fits(flattener::custom_call const& custom, symbol_id derivative, type_id result) const;

    /// The function `derivative`, inlined over `arguments`, as the block it becomes.
    flat_expr_id inline_derivative(flattener::custom_call const& custom,
                                   symbol_id derivative,
                                   cc::span<flat_expr_id const> arguments);

    /// The call as it is, then its result's tangent from `@forward`.
    void forward_custom(flat_stmt_id id, flattener::custom_call const& custom, label_id label);

    /// `@backward`, handed the call's arguments, their adjoints as places, and its result's adjoint.
    void backward_custom(flattener::custom_call const& custom, label_id label);
};
} // namespace sgl::check::impl
