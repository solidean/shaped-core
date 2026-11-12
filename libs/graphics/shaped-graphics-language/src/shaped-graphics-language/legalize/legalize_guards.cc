#include <clean-core/string/format.hh>
#include <shaped-graphics-language/builtins/registry.hh>
#include <shaped-graphics-language/legalize/impl/legalizer.hh>
#include <shaped-graphics-language/legalize/impl/walk.hh>

// Rule G1 (LEGAL-65): an operation that may have no value tests its operands first, and a failed test ends the
// invocation, or reports and then ends it.
// A guard is a block expression in the operation's place, so the expression rules that follow keep the order of
// evaluation and the short circuits as they keep an inlined call's.

using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;

namespace
{
/// One numeric shape: a scalar, or a vector of `width` scalars of `element`.
struct shape
{
    i32 width = 1;
    /// The scalar type's name: `int`, `uint`, `short`, `ushort`.
    cc::string_view element;
    /// The vector's lanes as positions among its members; empty for a scalar.
    cc::vector<i32> lanes;
};

struct guarder
{
    flat_builder& out;
    builtins::registry const& registry = builtins::default_registry();
    /// Guards the operations that may have no value; false where the pass runs for the asserts alone.
    bool guards = true;
    /// Prints the operation and its operands before the invocation ends.
    bool reports = false;
    /// Bounds a buffer index by the bound buffer's length, which the target can read.
    bool guards_buffers = true;
    /// LEGAL-68: a failed test replaces the operand with its safe value and goes on, ending nothing.
    bool defines = false;
    /// LEGAL-64: an `assert` before the last barrier is lowered here, where the flag is, rather than ending at once.
    bool lowers_asserts = false;
    /// The flag a guard sets where a barrier, a derivative or a subgroup operation may still follow, which the
    /// invocation must reach with every other one; tested after the last statement that holds one.
    local_id aborted = local_id::none;
    /// Whether the statement being guarded comes before or holds that last statement.
    bool defers = false;
    bool sets_flag = false;
    /// The second walk, once a guard set the flag: a loop that holds no barrier, derivative or subgroup operation is left
    /// at the top of each iteration once the flag is set, so a safe operand that bounds it cannot keep the
    /// invocation running long; a loop that holds one has a trip count every invocation shares, and runs on.
    bool breaks_loops = false;

    // ---- what a statement holds ---------------------------------------------------------------------------------------

    /// True for a call whose every invocation must reach it together (CHK-282, CHK-377), or one whose target text
    /// says it is one (CHK-431).
    [[nodiscard]] bool constrains_uniformity(flat_call const& call) const
    {
        if (call.text >= 0 || !is_valid(call.intrinsic))
        {
            auto const& symbol = out.m.at(call.callee);
            if (symbol.info < 0 || symbol.info >= out.m.functions.size())
                return call.text >= 0;
            auto const& f = out.m.functions[symbol.info];
            return f.is_barrier || f.is_subgroup_operation || f.uses_derivatives;
        }
        if (index_of(call.intrinsic) >= registry.functions.size())
            return false;
        auto const& r = registry.at(call.intrinsic);
        return r.uses_derivatives || r.is_barrier || r.is_subgroup_operation || r.is_uniform_load;
    }

    [[nodiscard]] bool expr_constrains(flat_expr_id id, int depth) const
    {
        if (!is_known(out.e, id) || depth > k_max_depth)
            return false;
        auto const& x = out.e.at(id);
        if (auto const* const call = x.node.try_as<flat_call>(); call != nullptr && constrains_uniformity(*call))
            return true;
        auto found = false;
        if (auto const* const b = x.node.try_as<flat_block>())
            found = body_constrains(b->body, depth + 1);
        for_each_operand(out.e, x, [&](flat_expr_id operand) { found = found || expr_constrains(operand, depth + 1); });
        return found;
    }

    [[nodiscard]] bool stmt_constrains(flat_stmt_id id, int depth) const
    {
        if (!is_known(out.e, id) || depth > k_max_depth)
            return false;
        auto const& s = out.e.at(id);
        auto found = false;
        for_each_expr_of(out.e, s, [&](flat_expr_id x) { found = found || expr_constrains(x, depth + 1); });
        for_each_pattern_of(out.e, s,
                            [&](ast::range_of<flat_expr_id> patterns)
                            {
                                if (is_known(out.e, patterns))
                                    for (auto const p : out.e.at(patterns))
                                        found = found || expr_constrains(p, depth + 1);
                            });
        for_each_body_of(
            out.e, s, [&](ast::range_of<flat_stmt_id> inner) { found = found || body_constrains(inner, depth + 1); });
        return found;
    }

    [[nodiscard]] bool body_constrains(ast::range_of<flat_stmt_id> range, int depth) const
    {
        if (!is_known(out.e, range))
            return false;
        for (auto const id : out.e.at(range))
            if (stmt_constrains(id, depth))
                return true;
        return false;
    }

    // ---- building a guard -----------------------------------------------------------------------------------------------

    /// The shape of `type`, where it is an integer scalar or a vector of them; none otherwise.
    [[nodiscard]] cc::optional<shape> integer_shape_of(type_id type) const
    {
        if (!is_valid(type))
            return {};
        auto const is_integer = [](cc::string_view name)
        {
            return name == "int" || name == "uint" || name == "short" || name == "ushort" || name == "long"
                || name == "ulong";
        };
        auto const name = out.m.name_of(type);
        if (is_integer(name))
            return shape{.width = 1, .element = name};
        auto const& t = out.m.at(type);
        if (t.kind != type_kind::structure || !t.has_swizzles)
            return {};
        auto const members = out.m.at(t.members);
        if (members.size() < 2 || members.size() > 4)
            return {};
        auto result = shape{.width = i32(members.size()), .element = out.m.name_of(members[0].type)};
        if (!is_integer(result.element))
            return {};
        for (auto i = isize(0); i < members.size(); ++i)
        {
            if (out.m.name_of(members[i].type) != result.element)
                return {};
            result.lanes.push_back(i32(i));
        }
        return result;
    }

    /// Lane `i` of the value `local` holds, widened to 32 bits; a 64-bit lane stays as it is.
    [[nodiscard]] flat_expr_id lane_of(local_id local, shape const& s, i32 i)
    {
        auto value = s.lanes.empty() ? out.local(local) : out.member(out.local(local), s.lanes[i]);
        if (s.element == "short")
            value = out.call(builtins::k_int, cc::span<flat_expr_id const>(&value, 1));
        else if (s.element == "ushort")
            value = out.call(builtins::k_uint, cc::span<flat_expr_id const>(&value, 1));
        return value;
    }

    [[nodiscard]] bool is_signed(shape const& s) const
    {
        return s.element == "int" || s.element == "short" || s.element == "long";
    }

    [[nodiscard]] static bool is_64_bit(shape const& s) { return s.element == "long" || s.element == "ulong"; }

    flat_expr_id uint_literal(i32 value)
    {
        return out.add_expr(out.type_named(builtins::k_uint), flat_int_literal{.value = value, .is_unsigned = true});
    }

    /// `int` or `uint` `value`, as the lanes `lane_of` gives compare to it.
    flat_expr_id literal_for(shape const& s, i64 value)
    {
        if (is_64_bit(s))
            return out.add_expr(out.type_named(s.element), flat_wide_literal{.bits = u64(value)});
        return is_signed(s) ? out.int_literal(i32(value)) : uint_literal(i32(value));
    }

    flat_expr_id compare(cc::string_view op, shape const& s, flat_expr_id a, flat_expr_id b)
    {
        flat_expr_id const args[] = {a, b};
        auto const family = is_64_bit(s) ? s.element : is_signed(s) ? builtins::k_int : builtins::k_uint;
        return out.call(cc::format("{}_{}", op, family), args);
    }

    /// A value of `type`, of shape `s`, whose every lane is 1.
    flat_expr_id ones(type_id type, shape const& s)
    {
        auto const one = [&]
        {
            auto lane = literal_for(s, 1);
            if (s.element == "short" || s.element == "ushort")
                lane = out.call(s.element, cc::span<flat_expr_id const>(&lane, 1));
            return lane;
        };
        if (s.lanes.empty())
            return one();
        auto lanes = cc::vector<flat_expr_id>();
        for (auto i = 0; i < s.width; ++i)
            lanes.push_back(one());
        return out.construct(type, lanes);
    }

    /// Whether a divisor is a literal that can never fail: no zero, and no -1 under an int.
    [[nodiscard]] bool is_safe_divisor(flat_expr_id divisor, shape const& dividend) const
    {
        if (auto const* const wide = out.e.at(divisor).node.try_as<flat_wide_literal>())
            return wide->bits != 0 && (wide->bits != ~u64(0) || dividend.element != "long");
        auto const* const literal = out.e.at(divisor).node.try_as<flat_int_literal>();
        if (literal == nullptr || literal->value == 0)
            return false;
        return literal->is_unsigned || literal->value != -1 || dividend.element != "int";
    }

    /// A safe value a deferring guard assigns to one of the operands it bound.
    struct replacement
    {
        local_id place = local_id::none;
        flat_expr_id value = flat_expr_id::none;
    };

    /// What fails: a print of `message` with `values` where the guard reports, then the end of the invocation, or the
    /// flag and each safe value assigned to its operand where the end must wait.
    stmt_list failed(cc::vector<cc::string> pieces, cc::span<flat_expr_id const> values, cc::span<replacement const> safe)
    {
        auto result = stmt_list();
        if (defines)
        {
            for (auto const& r : safe)
                result.push_back(out.assign(out.local(r.place), r.value));
            return result;
        }
        if (reports)
        {
            out.e.messages.push_back({.pieces = cc::move(pieces), .is_assert = true});
            result.push_back(out.print(i32(out.e.messages.size() - 1), values));
        }
        if (defers)
        {
            sets_flag = true;
            result.push_back(out.assign(out.local(aborted), out.bool_literal(true)));
            for (auto const& r : safe)
                result.push_back(out.assign(out.local(r.place), r.value));
        }
        else
            end_invocation(out, result);
        return result;
    }

    stmt_list failed(cc::vector<cc::string> pieces, cc::span<flat_expr_id const> values, local_id place, flat_expr_id fallback)
    {
        replacement const safe[] = {{.place = place, .value = fallback}};
        return failed(cc::move(pieces), values, safe);
    }

    /// A local holding `value`: mutable where a deferring guard replaces it.
    local_id pinned(cc::string_view name, flat_expr_id value, stmt_list& into)
    {
        auto const type = out.e.at(value).type;
        if (defers || defines)
        {
            auto const local = add_temporary_var(out, name, type);
            into.push_back(out.var(local, value));
            return local;
        }
        auto const local = out.add_local(local_kind::temporary, name, type);
        into.push_back(out.let(local, value));
        return local;
    }

    /// `call`, an integer `/` or `%`, as a block that ends the invocation where its divisor is zero, or where it divides
    /// the most negative int by -1 (EVAL-84); `id` itself where it can never fail.
    flat_expr_id guard_division(flat_expr_id id)
    {
        auto const x = out.e.at(id);
        auto const& call = *x.node.try_as<flat_call>();
        if (!is_known(out.e, call.arguments) || call.arguments.count != 2)
            return id;
        auto const dividend = out.e.at(call.arguments)[0];
        auto const divisor = out.e.at(call.arguments)[1];
        auto const a_known = integer_shape_of(out.e.at(dividend).type);
        auto const b_known = integer_shape_of(out.e.at(divisor).type);
        if (!a_known.has_value() || !b_known.has_value() || a_known.value().element != b_known.value().element)
            return id;
        auto const a_shape = a_known.value();
        auto const b_shape = b_known.value();
        if (b_shape.lanes.empty() && is_safe_divisor(divisor, a_shape))
            return id;

        out.from = x.from;
        out.inlined_through = x.inlined_through;
        auto const label = out.add_label("guard");
        auto stmts = stmt_list();
        auto const a = out.add_local(local_kind::temporary, "dividend", out.e.at(dividend).type);
        stmts.push_back(out.let(a, dividend));
        auto const b = pinned("divisor", divisor, stmts);

        // a lane fails where its divisor is zero, or where an int's is -1 and its dividend the most negative int
        auto const width = a_shape.width > b_shape.width ? a_shape.width : b_shape.width;
        auto bad = flat_expr_id::none;
        for (auto i = 0; i < width; ++i)
        {
            auto const b_lane = lane_of(b, b_shape, b_shape.lanes.empty() ? 0 : i);
            auto lane_bad = compare("equal", b_shape, b_lane, literal_for(b_shape, 0));
            if (a_shape.element == "int" || a_shape.element == "long")
            {
                auto const a_lane = lane_of(a, a_shape, a_shape.lanes.empty() ? 0 : i);
                auto const lowest_but_one = a_shape.element == "int" ? i64(-2147483647) : -i64((u64(1) << 63) - 1);
                auto const overflows
                    = out.and_(compare("equal", b_shape, lane_of(b, b_shape, b_shape.lanes.empty() ? 0 : i),
                                       literal_for(b_shape, -1)),
                               compare("less", a_shape, a_lane, literal_for(a_shape, lowest_but_one)));
                lane_bad = out.or_(lane_bad, overflows);
            }
            bad = is_valid(bad) ? out.or_(bad, lane_bad) : lane_bad;
        }

        auto const op = cc::string_view(out.m.at(call.callee).operator_spelling);
        // a report writes 32-bit words alone, so a 16-bit or a 64-bit operand is left out of it
        auto const is_16_bit = a_shape.element == "short" || a_shape.element == "ushort" || is_64_bit(a_shape);
        flat_expr_id const values[] = {out.local(a), out.local(b)};
        auto pieces = is_16_bit
                        ? cc::vector<cc::string>{cc::format("an integer `{}` has no value", op)}
                        : cc::vector<cc::string>{cc::string("`"), cc::format(" {} ", op), cc::string("` has no value")};
        auto const on_failure
            = failed(cc::move(pieces), is_16_bit ? cc::span<flat_expr_id const>() : cc::span<flat_expr_id const>(values),
                     b, ones(out.e.at(divisor).type, b_shape));
        stmts.push_back(out.if_(bad, on_failure));

        auto guarded = call;
        flat_expr_id const operands[] = {out.local(a), out.local(b)};
        guarded.arguments = out.expr_list(operands);
        stmts.push_back(out.leave(label, out.add_expr(x.type, guarded)));
        return out.block_expr(label, x.type, stmts);
    }

    /// The call of `nonuniform i` or `uniform i`, where `index` is one; a mark is read only where it indexes (CHK-300).
    [[nodiscard]] flat_call const* mark_of(flat_expr_id index) const
    {
        auto const* const call = out.e.at(index).node.try_as<flat_call>();
        auto const* const record = call != nullptr ? out.m.builtin_function(call->intrinsic) : nullptr;
        return record != nullptr && record->mark != builtins::index_mark::none ? call : nullptr;
    }

    /// What `index` marks, or `index` itself.
    [[nodiscard]] flat_expr_id unmarked(flat_expr_id index) const
    {
        auto const* const call = mark_of(index);
        return call != nullptr ? out.e.at(call->arguments)[0] : index;
    }

    /// `value` under the mark `index` carries, or `value` itself.
    flat_expr_id remarked(flat_expr_id index, flat_expr_id value)
    {
        auto const* const mark = mark_of(index);
        if (mark == nullptr)
            return value;
        auto const call = *mark;
        auto const type = out.e.at(index).type;
        flat_expr_id const arguments[] = {value};
        return out.add_expr(type, flat_call{.callee = call.callee,
                                            .intrinsic = call.intrinsic,
                                            .is_pure = call.is_pure,
                                            .arguments = out.expr_list(arguments),
                                            .text = call.text});
    }

    /// CHK-491's length of the unbounded binding array `object` names, as a `uint`; `none` where it names none.
    flat_expr_id unbounded_length_of(flat_expr_id object)
    {
        auto const* const b = out.e.at(object).node.try_as<flat_binding_member>();
        if (b == nullptr)
            return flat_expr_id::none;
        auto const named = *b;
        flat_expr_id const held[]
            = {out.add_expr(out.type_named(builtins::k_int),
                            flat_buffer_length{.buffer = out.add_expr(out.e.at(object).type, flat_binding_member{
                                                                                                 .binding = named.binding,
                                                                                                 .member = named.member,
                                                                                             })})};
        return out.call("convert_int_to_uint", held);
    }

    /// The test that the pinned `i` of shape `s` is outside `0 ..< length`, a pinned `uint`.
    flat_expr_id outside_length(local_id i, shape const& s, local_id length)
    {
        auto unsigned_index = out.local(i);
        if (s.element == "int")
        {
            flat_expr_id const argument[] = {out.local(i)};
            unsigned_index = out.call("convert_int_to_uint", argument);
        }
        auto bad = out.not_(compare("less", shape{.element = builtins::k_uint}, unsigned_index, out.local(length)));
        if (s.element == "int")
            bad = out.or_(compare("less", s, out.local(i), out.int_literal(0)), bad);
        return bad;
    }

    /// The index of `element`, an array's or a binding array's, as a block that ends the invocation where it is outside
    /// the array (EVAL-90); `none` where it needs no guard, or where no length of it is known.
    flat_expr_id guarded_index(flat_expr const& x, flat_element const& element)
    {
        if (!is_known(out.e, element.object) || !is_known(out.e, element.index))
            return flat_expr_id::none;
        auto const& array = out.m.at(out.e.at(element.object).type);
        if (array.kind != type_kind::array)
            return flat_expr_id::none;
        auto const count = array.count;
        auto const is_unbounded = count <= 0;
        auto const value = unmarked(element.index);
        if (out.e.at(value).node.is<flat_int_literal>() && !is_unbounded)
            return flat_expr_id::none;
        // a type, not a reference: the guard grows the tree it would point into
        auto const index_type = out.e.at(value).type;
        auto const index_known = integer_shape_of(index_type);
        if (!index_known.has_value() || !index_known.value().lanes.empty()
            || (index_known.value().element != "int" && index_known.value().element != "uint"))
            return flat_expr_id::none;
        auto const index_shape = index_known.value();

        out.from = x.from;
        out.inlined_through = x.inlined_through;
        auto const label = out.add_label("guard");
        auto stmts = stmt_list();
        auto const length = is_unbounded ? unbounded_length_of(element.object) : flat_expr_id::none;
        if (is_unbounded && !is_valid(length))
            return flat_expr_id::none;
        auto const i = pinned("index", value, stmts);
        if (is_unbounded)
        {
            auto const n = pinned("length", length, stmts);
            flat_expr_id const values[] = {out.local(i), out.local(n)};
            auto const on_failure
                = failed({cc::string("index "), cc::string(" is outside a binding array of "), cc::string(" elements")},
                         values, i, literal_for(index_shape, 0));
            stmts.push_back(out.if_(outside_length(i, index_shape, n), on_failure));
        }
        else
        {
            auto bad = out.not_(compare("less", index_shape, out.local(i), literal_for(index_shape, count)));
            if (index_shape.element == "int")
                bad = out.or_(compare("less", index_shape, out.local(i), out.int_literal(0)), bad);
            auto const shown = out.local(i);
            auto const on_failure = failed({cc::string("index "), cc::format(" is outside 0 ..< {}", count)},
                                           cc::span<flat_expr_id const>(&shown, 1), i, literal_for(index_shape, 0));
            stmts.push_back(out.if_(bad, on_failure));
        }
        stmts.push_back(out.leave(label, out.local(i)));
        return remarked(element.index, out.block_expr(label, index_type, stmts));
    }

    /// The pinned index and length a test of `index` against the memory `buffer` names leaves behind.
    struct bounded_index
    {
        local_id index = local_id::none;
        local_id length = local_id::none;
    };

    /// `index` of `buffer` pinned into `into`, tested against the bound length, failing as a guard does.
    cc::optional<bounded_index> bound_buffer_index(flat_expr_id index, flat_expr_id buffer, stmt_list& into)
    {
        auto const index_known = integer_shape_of(out.e.at(index).type);
        if (!index_known.has_value() || !index_known.value().lanes.empty()
            || (index_known.value().element != "int" && index_known.value().element != "uint"))
            return {};
        auto const index_shape = index_known.value();
        auto const i = pinned("index", index, into);
        auto const length = pinned(
            "length", out.add_expr(out.type_named(builtins::k_uint), flat_buffer_length{.buffer = buffer}), into);
        flat_expr_id const values[] = {out.local(i), out.local(length)};
        auto const on_failure
            = failed({cc::string("index "), cc::string(" is outside a buffer of "), cc::string(" elements")}, values, i,
                     literal_for(index_shape, 0));
        into.push_back(out.if_(outside_length(i, index_shape, length), on_failure));
        return bounded_index{.index = i, .length = length};
    }

    /// Whether a guard of `element` must hold the whole access rather than its index.
    /// An element of a binding array names its memory twice, for the length and for the access, so its index is pinned
    /// in front of both; under `define` an access of memory that holds nothing is not to run at all (LEGAL-68).
    [[nodiscard]] bool guards_whole_access(flat_buffer_element const& element) const
    {
        return guards_buffers && is_known(out.e, element.buffer)
            && (defines || out.e.at(element.buffer).node.is<flat_element>());
    }

    /// The buffer element at the root of the place `id`, or nothing where it is rooted elsewhere.
    [[nodiscard]] flat_buffer_element const* buffer_root_of(flat_expr_id id) const
    {
        for (auto steps = 0; steps < k_max_depth && is_known(out.e, id); ++steps)
        {
            auto const& x = out.e.at(id);
            if (auto const* const member = x.node.try_as<flat_member>())
                id = member->object;
            else if (auto const* const element = x.node.try_as<flat_element>())
                id = element->object;
            else
                return x.node.try_as<flat_buffer_element>();
        }
        return nullptr;
    }

    [[nodiscard]] bool is_whole_buffer_access(flat_expr_id id) const
    {
        auto const* const root = buffer_root_of(id);
        return root != nullptr && guards_whole_access(*root);
    }

    /// `resource`, a binding member or an element of a binding array of them, naming the same thing however often it is
    /// read: an element's index is pinned into `pins`.
    flat_expr_id pinned_resource(flat_expr_id resource, stmt_list& pins)
    {
        auto x = out.e.at(resource);
        auto* const element = x.node.try_as<flat_element>();
        if (element == nullptr || out.e.at(element->index).node.is<flat_int_literal>())
            return resource;
        auto const value = unmarked(element->index);
        auto const index = out.add_local(local_kind::temporary, "element", out.e.at(value).type);
        pins.push_back(out.let(index, value));
        element->index = remarked(element->index, out.local(index));
        out.e.exprs.push_back(x);
        return flat_expr_id(out.e.exprs.size() - 1);
    }

    /// A buffer place with its indices pinned, and the length its root was tested against.
    struct buffer_access
    {
        flat_expr_id place = flat_expr_id::none;
        local_id length = local_id::none;
    };

    /// `id`, a place rooted at a buffer element, with each index pinned into `pins` in the order the place evaluates them.
    cc::optional<buffer_access> pinned_buffer_place(flat_expr_id id, stmt_list& pins)
    {
        auto x = out.e.at(id);
        auto result = buffer_access();
        if (auto* const root = x.node.try_as<flat_buffer_element>())
        {
            auto const buffer = pinned_resource(root->buffer, pins);
            auto const bounded = bound_buffer_index(root->index, buffer, pins);
            if (!bounded.has_value())
                return {};
            root->buffer = buffer;
            root->index = out.local(bounded.value().index);
            result.length = bounded.value().length;
        }
        else if (auto* const member = x.node.try_as<flat_member>())
        {
            auto const inner = pinned_buffer_place(member->object, pins);
            if (!inner.has_value())
                return {};
            member->object = inner.value().place;
            result.length = inner.value().length;
        }
        else if (auto* const element = x.node.try_as<flat_element>())
        {
            auto const inner = pinned_buffer_place(element->object, pins);
            if (!inner.has_value())
                return {};
            element->object = inner.value().place;
            if (!out.e.at(element->index).node.is<flat_int_literal>())
            {
                auto const index = out.add_local(local_kind::temporary, "index", out.e.at(element->index).type);
                pins.push_back(out.let(index, element->index));
                element->index = out.local(index);
            }
            result.length = inner.value().length;
        }
        else
            return {};
        out.e.exprs.push_back(x);
        result.place = flat_expr_id(out.e.exprs.size() - 1);
        return result;
    }

    /// LEGAL-68: leaves `label` with a zero of `type` where `is_short`, memory too short for the access to run at all.
    void left_with_zero(flat_expr_id is_short, label_id label, type_id type, stmt_list& into)
    {
        auto const zero = add_temporary_var(out, "zero", type);
        out.e.locals[index_of(zero)].is_zeroed = true;
        into.push_back(out.var(zero));
        flat_stmt_id const leave[] = {out.leave(label, out.local(zero))};
        into.push_back(out.if_(is_short, leave));
    }

    [[nodiscard]] flat_expr_id holds_nothing(local_id length)
    {
        return compare("equal", shape{.element = builtins::k_uint}, out.local(length), uint_literal(0));
    }

    /// `id`, a read of a whole-access buffer place, as a block; under `define` it is zero where the memory holds nothing.
    flat_expr_id guard_buffer_read(flat_expr_id id)
    {
        auto const x = out.e.at(id);
        out.from = x.from;
        out.inlined_through = x.inlined_through;
        auto stmts = stmt_list();
        auto const access = pinned_buffer_place(id, stmts);
        if (!access.has_value())
            return id;
        auto const label = out.add_label("guard");
        if (defines)
            left_with_zero(holds_nothing(access.value().length), label, x.type, stmts);
        stmts.push_back(out.leave(label, access.value().place));
        return out.block_expr(label, x.type, stmts);
    }

    /// `s`, a store to a whole-access buffer place, as a block that evaluates its place, then its value, then stores;
    /// under `define` memory that holds nothing drops the store.
    flat_stmt_id guard_buffer_store(flat_stmt const& s, flat_assign const& n)
    {
        out.from = s.from;
        out.inlined_through = s.inlined_through;
        auto stmts = stmt_list();
        auto const access = pinned_buffer_place(n.place, stmts);
        if (!access.has_value())
            return flat_stmt_id::none;
        auto const stored = out.add_local(local_kind::temporary, "stored", out.e.at(n.value).type);
        stmts.push_back(out.let(stored, n.value));
        auto const store = out.assign(access.value().place, out.local(stored));
        if (defines)
        {
            flat_stmt_id const then[] = {store};
            stmts.push_back(out.if_(out.not_(holds_nothing(access.value().length)), then));
        }
        else
            stmts.push_back(store);
        return out.block(out.add_label("stored"), stmts);
    }

    /// Whether `call` is an atomic of a whole-access buffer place.
    [[nodiscard]] bool is_buffer_call(flat_call const& call) const
    {
        if (!guards_buffers || !is_valid(call.intrinsic) || index_of(call.intrinsic) >= registry.functions.size()
            || !is_known(out.e, call.arguments) || call.arguments.count < 1)
            return false;
        return registry.at(call.intrinsic).is_atomic && is_whole_buffer_access(out.e.at(call.arguments)[0]);
    }

    /// `id`, an atomic of a whole-access buffer place, with its place and its operands pinned, in their order.
    /// Under `define`, memory that holds nothing runs no atomic, and gives zero.
    /// A call that gives nothing is a statement, which `into` receives instead of a block.
    flat_expr_id guard_buffer_call(flat_expr_id id, stmt_list* into)
    {
        auto const x = out.e.at(id);
        auto call = *x.node.try_as<flat_call>();
        out.from = x.from;
        out.inlined_through = x.inlined_through;
        auto stmts = stmt_list();
        auto arguments = cc::vector<flat_expr_id>();
        for (auto const a : out.e.at(call.arguments))
            arguments.push_back(a);
        auto const access = pinned_buffer_place(arguments[0], stmts);
        if (!access.has_value())
            return id;
        arguments[0] = access.value().place;
        for (auto i = isize(1); i < arguments.size(); ++i)
        {
            auto const operand = out.add_local(local_kind::temporary, "operand", out.e.at(arguments[i]).type);
            stmts.push_back(out.let(operand, arguments[i]));
            arguments[i] = out.local(operand);
        }
        call.arguments = out.expr_list(arguments);
        auto const guarded = out.add_expr(x.type, call);
        if (into != nullptr)
        {
            auto const run = out.eval(guarded);
            if (defines)
            {
                flat_stmt_id const then[] = {run};
                stmts.push_back(out.if_(out.not_(holds_nothing(access.value().length)), then));
            }
            else
                stmts.push_back(run);
            for (auto const s : stmts)
                into->push_back(s);
            return id;
        }
        auto const label = out.add_label("guard");
        if (defines)
            left_with_zero(holds_nothing(access.value().length), label, x.type, stmts);
        auto const result = out.add_local(local_kind::temporary, "result", x.type);
        stmts.push_back(out.let(result, guarded));
        stmts.push_back(out.leave(label, out.local(result)));
        return out.block_expr(label, x.type, stmts);
    }

    /// The index of `element`, a buffer's, as a block that ends the invocation where it is past the bound buffer's end
    /// (EVAL-90, LEGAL-66); `none` where it needs no guard, the buffer is no binding member, or the whole access is
    /// guarded instead.
    flat_expr_id guarded_buffer_index(flat_expr const& x, flat_buffer_element const& element)
    {
        if (!guards_buffers || !is_known(out.e, element.buffer) || !is_known(out.e, element.index)
            || guards_whole_access(element))
            return flat_expr_id::none;
        if (!out.e.at(element.buffer).node.is<flat_binding_member>())
            return flat_expr_id::none;
        auto const index_type = out.e.at(element.index).type;

        out.from = x.from;
        out.inlined_through = x.inlined_through;
        auto const label = out.add_label("guard");
        auto stmts = stmt_list();
        auto const bounded = bound_buffer_index(element.index, element.buffer, stmts);
        if (!bounded.has_value())
            return flat_expr_id::none;
        stmts.push_back(out.leave(label, out.local(bounded.value().index)));
        return out.block_expr(label, index_type, stmts);
    }

    /// How many words a load or a store of `bytes` reaches, or 0 for a call that is none (EVAL-97).
    [[nodiscard]] i32 bytes_words_of(flat_call const& call) const
    {
        if (!guards_buffers || !is_valid(call.intrinsic) || index_of(call.intrinsic) >= registry.functions.size()
            || !is_known(out.e, call.arguments) || call.arguments.count < 2)
            return 0;
        auto const arguments = out.e.at(call.arguments);
        if (out.m.at(out.e.at(arguments[0]).type).kind != type_kind::bytes)
            return 0;
        auto const& r = registry.at(call.intrinsic);
        if (r.name == "load")
            return 1;
        if (r.name == "load2" || r.name == "load3" || r.name == "load4")
            return r.name[4] - '0';
        if (r.name == "store" && arguments.size() == 3)
            if (auto const s = integer_shape_of(out.e.at(arguments[2]).type); s.has_value())
                return s.value().width;
        return 0;
    }

    /// `call`, a load or a store of `bytes`, as a block that ends the invocation where its offset is no multiple of 4
    /// or a word it reaches is past the bound memory's end (EVAL-97, LEGAL-66).
    /// Under `define`, memory too short for those words runs no load or store: a load is zero, and a store is dropped.
    /// A store gives nothing, so under `define` it is a statement, which `into` receives instead of a block.
    flat_expr_id guard_bytes(flat_expr_id id, i32 words, stmt_list* into)
    {
        auto const x = out.e.at(id);
        auto const& call = *x.node.try_as<flat_call>();
        auto operands = cc::vector<flat_expr_id>();
        for (auto const a : out.e.at(call.arguments))
            operands.push_back(a);
        auto const uint_shape = shape{.element = builtins::k_uint};

        out.from = x.from;
        out.inlined_through = x.inlined_through;
        auto const label = out.add_label("guard");
        auto stmts = stmt_list();
        // the memory is a resource and no local holds one; its index, the offset and a stored value keep their order
        auto const memory = pinned_resource(operands[0], stmts);
        auto const offset = pinned("offset", operands[1], stmts);
        auto stored = local_id::none;
        if (operands.size() == 3)
            stored = pinned("stored", operands[2], stmts);
        auto const length = pinned(
            "length", out.add_expr(out.type_named(builtins::k_uint), flat_buffer_length{.buffer = memory}), stmts);

        flat_expr_id const remainder_args[] = {out.local(offset), uint_literal(4)};
        auto const misaligned
            = compare("not_equal", uint_shape, out.call("remainder_uint", remainder_args), uint_literal(0));
        auto const reach = uint_literal(4 * words);
        flat_expr_id const room_args[] = {out.local(length), reach};
        auto const past_the_end
            = out.or_(compare("less", uint_shape, out.local(length), uint_literal(4 * words)),
                      compare("less", uint_shape, out.call("subtract_uint", room_args), out.local(offset)));
        flat_expr_id const values[] = {out.local(offset), out.local(length)};
        auto const on_failure
            = failed({cc::string("the offset "), cc::format(" of {} words is unaligned or outside memory of ", words),
                      cc::string(" bytes")},
                     values, offset, uint_literal(0));
        stmts.push_back(out.if_(out.or_(misaligned, past_the_end), on_failure));

        auto guarded = call;
        auto arguments = cc::vector<flat_expr_id>{memory, out.local(offset)};
        if (operands.size() == 3)
            arguments.push_back(out.local(stored));
        guarded.arguments = out.expr_list(arguments);
        auto const access = out.add_expr(x.type, guarded);
        if (into != nullptr)
        {
            flat_stmt_id const then[] = {out.eval(access)};
            stmts.push_back(
                out.if_(out.not_(compare("less", uint_shape, out.local(length), uint_literal(4 * words))), then));
            for (auto const s : stmts)
                into->push_back(s);
            return id;
        }
        if (defines)
            left_with_zero(compare("less", uint_shape, out.local(length), uint_literal(4 * words)), label, x.type, stmts);
        stmts.push_back(out.leave(label, access));
        return out.block_expr(label, x.type, stmts);
    }

    /// The size of the matrix whose row or column `call` reads (EVAL-87), or 0 for a call that reads none.
    [[nodiscard]] i32 matrix_part_size_of(flat_call const& call) const
    {
        if (!is_valid(call.intrinsic) || index_of(call.intrinsic) >= registry.functions.size()
            || !is_known(out.e, call.arguments) || call.arguments.count != 2)
            return 0;
        auto const& r = registry.at(call.intrinsic);
        if (r.name != "matrix_row" && r.name != "matrix_column")
            return 0;
        auto const matrix = out.m.name_of(out.e.at(out.e.at(call.arguments)[0]).type);
        return matrix == "mat4" ? 4 : matrix == "mat3" ? 3 : matrix == "mat2" ? 2 : 0;
    }

    /// `call`, a matrix's row or column, as a block that ends the invocation where its index is outside the matrix.
    flat_expr_id guard_matrix_part(flat_expr_id id, i32 size)
    {
        auto const x = out.e.at(id);
        auto const& call = *x.node.try_as<flat_call>();
        auto const arguments = out.e.at(call.arguments);
        auto const matrix_value = arguments[0];
        auto const index_value = arguments[1];
        if (out.e.at(index_value).node.is<flat_int_literal>())
            return id;
        auto const int_shape = shape{.element = builtins::k_int};

        out.from = x.from;
        out.inlined_through = x.inlined_through;
        auto const label = out.add_label("guard");
        auto stmts = stmt_list();
        auto const matrix = pinned("matrix", matrix_value, stmts);
        auto const i = pinned("index", index_value, stmts);
        auto const bad = out.or_(compare("less", int_shape, out.local(i), out.int_literal(0)),
                                 out.not_(compare("less", int_shape, out.local(i), out.int_literal(size))));
        auto const value = out.local(i);
        stmts.push_back(out.if_(bad, failed({cc::string("matrix index "), cc::format(" is outside 0 ..< {}", size)},
                                            cc::span<flat_expr_id const>(&value, 1), i, out.int_literal(0))));
        auto guarded = call;
        flat_expr_id const operands[] = {out.local(matrix), out.local(i)};
        guarded.arguments = out.expr_list(operands);
        stmts.push_back(out.leave(label, out.add_expr(x.type, guarded)));
        return out.block_expr(label, x.type, stmts);
    }

    // ---- the maths builtins of EVAL-87 --------------------------------------------------------------------------------

    /// The shape of `type`, where it is a scalar of a number type, `float` and `half` included, or a vector of one.
    [[nodiscard]] cc::optional<shape> number_shape_of(type_id type) const
    {
        if (!is_valid(type))
            return {};
        auto const is_number = [](cc::string_view name)
        {
            return name == "int" || name == "uint" || name == "short" || name == "ushort" || name == "float"
                || name == "half";
        };
        auto const name = out.m.name_of(type);
        if (is_number(name))
            return shape{.width = 1, .element = name};
        auto const& t = out.m.at(type);
        if (t.kind != type_kind::structure || !t.has_swizzles)
            return {};
        auto const members = out.m.at(t.members);
        if (members.size() < 2 || members.size() > 4)
            return {};
        auto result = shape{.width = i32(members.size()), .element = out.m.name_of(members[0].type)};
        if (!is_number(result.element))
            return {};
        for (auto i = isize(0); i < members.size(); ++i)
        {
            if (out.m.name_of(members[i].type) != result.element)
                return {};
            result.lanes.push_back(i32(i));
        }
        return result;
    }

    [[nodiscard]] static bool is_float(shape const& s) { return s.element == "float" || s.element == "half"; }

    /// Lane `i` of `local` in the type its comparisons are written in: `int`, `uint`, or `float`, a half widened.
    [[nodiscard]] flat_expr_id number_lane(local_id local, shape const& s, i32 i)
    {
        if (!is_float(s))
            return lane_of(local, s, i);
        auto value = s.lanes.empty() ? out.local(local) : out.member(out.local(local), s.lanes[i]);
        if (s.element == "half")
            value = out.call("float", cc::span<flat_expr_id const>(&value, 1));
        return value;
    }

    /// `value` as a literal of the type `number_lane` compares in.
    flat_expr_id number_literal(shape const& s, f64 value)
    {
        return is_float(s) ? out.literal(value) : literal_for(s, i32(value));
    }

    /// `a op b` for lanes of shape `s`, where `op` is `less`, `less_equal` or `equal`.
    flat_expr_id compare_numbers(cc::string_view op, shape const& s, flat_expr_id a, flat_expr_id b)
    {
        if (!is_float(s))
            return compare(op, s, a, b);
        flat_expr_id const args[] = {a, b};
        return out.call(op, args);
    }

    /// A value of `type`, of shape `s`, whose every lane is `value` in the lane's own type.
    flat_expr_id filled(type_id type, shape const& s, f64 value)
    {
        auto const one = [&]
        {
            auto lane = number_literal(s, value);
            if (s.element == "half" || s.element == "short" || s.element == "ushort")
                lane = out.call(s.element, cc::span<flat_expr_id const>(&lane, 1));
            return lane;
        };
        if (s.lanes.empty())
            return one();
        auto lanes = cc::vector<flat_expr_id>();
        for (auto i = 0; i < s.width; ++i)
            lanes.push_back(one());
        return out.construct(type, lanes);
    }

    /// The name of `call`'s builtin where it is one of EVAL-87's this pass guards; empty for every other call.
    [[nodiscard]] cc::string_view guarded_math_of(flat_call const& call) const
    {
        if (!is_valid(call.intrinsic) || index_of(call.intrinsic) >= registry.functions.size())
            return {};
        auto const& r = registry.at(call.intrinsic);
        if (r.undefined_when == nullptr)
            return {};
        for (auto const name : {"pow", "asin", "acos", "atan2", "smoothstep", "clamp"})
            if (r.name == name)
                return name;
        return {};
    }

    /// `call`, a maths builtin of EVAL-87, as a block that ends the invocation where its operands have no value, each
    /// test the one the interpreter asks; `id` itself where its operands are not of one number shape.
    flat_expr_id guard_math(flat_expr_id id, cc::string_view name)
    {
        auto const x = out.e.at(id);
        auto const& call = *x.node.try_as<flat_call>();
        if (!is_known(out.e, call.arguments) || call.arguments.count == 0)
            return id;
        auto operands = cc::vector<flat_expr_id>();
        for (auto const a : out.e.at(call.arguments))
            operands.push_back(a);
        // the widest operand's shape, and each operand's own: a scalar beside a vector reads as every lane of it
        auto shapes = cc::vector<shape>();
        auto types = cc::vector<type_id>();
        for (auto const a : operands)
        {
            auto const known = number_shape_of(out.e.at(a).type);
            if (!known.has_value())
                return id;
            shapes.push_back(known.value());
            types.push_back(out.e.at(a).type);
        }
        auto widest = isize(0);
        for (auto k = isize(1); k < shapes.size(); ++k)
            if (shapes[k].width > shapes[widest].width)
                widest = k;
        auto const s = shapes[widest];
        for (auto const& o : shapes)
            if (o.element != s.element || (o.width != s.width && o.width != 1))
                return id;

        out.from = x.from;
        out.inlined_through = x.inlined_through;
        auto const label = out.add_label("guard");
        auto stmts = stmt_list();
        auto locals = cc::vector<local_id>();
        for (auto const a : operands)
            locals.push_back(pinned("operand", a, stmts));

        auto bad = flat_expr_id::none;
        for (auto i = 0; i < s.width; ++i)
        {
            auto const lane = [&](isize k) { return number_lane(locals[k], shapes[k], shapes[k].lanes.empty() ? 0 : i); };
            auto const literal = [&](f64 v) { return number_literal(s, v); };
            auto lane_bad = flat_expr_id::none;
            if (name == "pow")
                lane_bad = out.or_(compare_numbers("less", s, lane(0), literal(0.0)),
                                   out.and_(compare_numbers("equal", s, lane(0), literal(0.0)),
                                            compare_numbers("less_equal", s, lane(1), literal(0.0))));
            else if (name == "asin" || name == "acos")
                lane_bad = out.or_(compare_numbers("less", s, lane(0), literal(-1.0)),
                                   compare_numbers("less", s, literal(1.0), lane(0)));
            else if (name == "atan2")
                lane_bad = out.and_(compare_numbers("equal", s, lane(0), literal(0.0)),
                                    compare_numbers("equal", s, lane(1), literal(0.0)));
            else if (name == "smoothstep")
                lane_bad = out.not_(compare_numbers("less", s, lane(0), lane(1)));
            else if (name == "clamp")
                lane_bad = compare_numbers("less", s, lane(2), lane(1));
            if (!is_valid(lane_bad))
                return id;
            bad = is_valid(bad) ? out.or_(bad, lane_bad) : lane_bad;
        }

        // what the operation computes once a deferring guard tripped: any value, so one that has one
        auto safe = cc::vector<replacement>();
        auto const fill = [&](isize k, f64 v) { return filled(types[k], shapes[k], v); };
        if (name == "pow")
            safe.push_back({.place = locals[0], .value = fill(0, 1.0)});
        else if (name == "asin" || name == "acos")
            safe.push_back({.place = locals[0], .value = fill(0, 0.0)});
        else if (name == "atan2")
            safe.push_back({.place = locals[1], .value = fill(1, 1.0)});
        else if (name == "smoothstep")
        {
            safe.push_back({.place = locals[0], .value = fill(0, 0.0)});
            safe.push_back({.place = locals[1], .value = fill(1, 1.0)});
        }
        else if (name == "clamp" && types[1] == types[2])
            safe.push_back({.place = locals[2], .value = out.local(locals[1])});
        else if (name == "clamp")
            return id;

        auto const is_16_bit_int = s.element == "short" || s.element == "ushort";
        auto pieces = cc::vector<cc::string>();
        auto values = cc::vector<flat_expr_id>();
        if (is_16_bit_int)
            pieces.push_back(cc::format("`{}` has no value for these operands", name));
        else
        {
            pieces.push_back(cc::format("`{}(", name));
            for (auto k = isize(1); k < locals.size(); ++k)
                pieces.push_back(cc::string(", "));
            pieces.push_back(cc::string(")` has no value"));
            for (auto const local : locals)
                values.push_back(out.local(local));
        }
        stmts.push_back(out.if_(bad, failed(cc::move(pieces), values, safe)));

        auto guarded = call;
        auto arguments = cc::vector<flat_expr_id>();
        for (auto const local : locals)
            arguments.push_back(out.local(local));
        guarded.arguments = out.expr_list(arguments);
        stmts.push_back(out.leave(label, out.add_expr(x.type, guarded)));
        return out.block_expr(label, x.type, stmts);
    }

    [[nodiscard]] bool is_integer_division(flat_call const& call) const
    {
        if (!is_valid(call.intrinsic) || index_of(call.intrinsic) >= registry.functions.size())
            return false;
        auto const& r = registry.at(call.intrinsic);
        return r.undefined_when != nullptr && (r.name.starts_with("divide") || r.name.starts_with("remainder"));
    }

    // ---- LEGAL-69: workgroup memory read before anything stored it ---------------------------------------------------

    /// The first scalar of one `@workgroup` binding member in the shadow, which counts every scalar of the entry
    /// point's workgroup memory once, in the order its members are listed.
    struct shadow_member
    {
        symbol_id binding = symbol_id::none;
        i32 member = -1;
        u32 first = 0;
    };
    /// The members the shadow covers; empty where no read is tested.
    cc::vector<shadow_member> shadow;
    /// Places whose parent tests or marks them itself, as an assignment's, an atomic's and a uniform load's.
    cc::vector<flat_expr_id> own_places;
    /// Set for the walk into the object of a member or an element, which is part of a place rather than a read.
    bool next_is_chain_object = false;

    /// A place of workgroup memory as the shadow sees it: rebuilt over pinned indices, so the shadow's test and the
    /// access itself read each index once.
    struct shadow_place
    {
        flat_expr_id place = flat_expr_id::none;
        /// The first scalar it reaches, a `uint`.
        flat_expr_id first = flat_expr_id::none;
        u32 count = 0;
        /// A swizzle reaches its fields one at a time: each field's offset from `first`, one scalar each.
        cc::vector<u32> fields;
        /// The place as the report names it, around the indices in `values`.
        cc::vector<cc::string> pieces;
        cc::vector<flat_expr_id> values;
    };

    [[nodiscard]] u32 slots_of(type_id type) const
    {
        if (auto const* const record = out.m.builtin_type_of(type))
            return u32(record->leaf_count > 0 ? record->leaf_count : 1);
        auto const& t = out.m.at(type);
        if (t.kind == type_kind::array)
            return u32(t.count > 0 ? t.count : 0) * slots_of(t.element);
        if (t.kind == type_kind::structure)
        {
            auto total = u32(0);
            for (auto const& m : out.m.at(t.members))
                total += slots_of(m.type);
            return total;
        }
        // an atomic and an enum are one scalar
        return 1;
    }

    [[nodiscard]] u32 offset_of_member(type_id type, i32 member) const
    {
        auto offset = u32(0);
        auto const members = out.m.at(out.m.at(type).members);
        for (auto i = 0; i < member && i < members.size(); ++i)
            offset += slots_of(members[i].type);
        return offset;
    }

    [[nodiscard]] bool is_workgroup_place(flat_expr_id id) const
    {
        for (auto steps = 0; steps < k_max_depth && is_known(out.e, id); ++steps)
        {
            auto const& x = out.e.at(id);
            if (auto const* const member = x.node.try_as<flat_member>())
                id = member->object;
            else if (auto const* const element = x.node.try_as<flat_element>())
                id = element->object;
            else if (auto const* const b = x.node.try_as<flat_binding_member>())
                return b->is_workgroup;
            else
                return false;
        }
        return false;
    }

    /// `id`, a place of workgroup memory, with each index pinned into `pins` in the order the place evaluates them.
    cc::optional<shadow_place> pinned_place(flat_expr_id id, stmt_list& pins)
    {
        auto x = out.e.at(id);
        if (auto const* const b = x.node.try_as<flat_binding_member>())
        {
            for (auto const& s : shadow)
                if (s.binding == b->binding && s.member == b->member)
                {
                    auto const& binding = out.m.bindings[out.m.at(b->binding).info];
                    auto const name
                        = cc::format("{}.{}", out.m.at(b->binding).name, out.m.at(binding.members)[b->member].name);
                    return shadow_place{.place = id,
                                        .first = uint_literal(i32(s.first)),
                                        .count = slots_of(x.type),
                                        .pieces = {cc::format("workgroup memory `{}", name)}};
                }
            return {};
        }
        if (auto* const member = x.node.try_as<flat_member>())
        {
            auto inner = pinned_place(member->object, pins);
            if (!inner.has_value())
                return {};
            auto result = cc::move(inner.value());
            auto const object_type = out.e.at(member->object).type;
            auto const members = out.m.at(out.m.at(object_type).members);
            member->object = result.place;
            out.e.exprs.push_back(x);
            result.place = flat_expr_id(out.e.exprs.size() - 1);
            auto field_names = cc::string();
            if (member->is_swizzle())
            {
                for (auto i = 0; i < member->letters.count; ++i)
                {
                    auto const field = member->letters.fields[i];
                    result.fields.push_back(offset_of_member(object_type, field));
                    field_names += members[field].name;
                }
            }
            else
            {
                if (auto const offset = offset_of_member(object_type, member->member); offset > 0)
                {
                    flat_expr_id const args[] = {result.first, uint_literal(i32(offset))};
                    result.first = out.call("add_uint", args);
                }
                result.count = slots_of(x.type);
                field_names = members[member->member].name;
            }
            result.pieces.back() += cc::format(".{}", field_names);
            return result;
        }
        if (auto* const element = x.node.try_as<flat_element>())
        {
            auto inner = pinned_place(element->object, pins);
            if (!inner.has_value())
                return {};
            auto result = cc::move(inner.value());
            auto const index_type = out.e.at(element->index).type;
            auto const index_known = integer_shape_of(index_type);
            if (!index_known.has_value() || !index_known.value().lanes.empty()
                || (index_known.value().element != "int" && index_known.value().element != "uint"))
                return {};
            auto const index = out.add_local(local_kind::temporary, "index", index_type);
            pins.push_back(out.let(index, element->index));
            auto unsigned_index = out.local(index);
            if (index_known.value().element == "int")
            {
                flat_expr_id const argument[] = {out.local(index)};
                unsigned_index = out.call("convert_int_to_uint", argument);
            }
            flat_expr_id const scaled_args[] = {unsigned_index, uint_literal(i32(slots_of(x.type)))};
            flat_expr_id const first_args[] = {result.first, out.call("multiply_uint", scaled_args)};
            result.first = out.call("add_uint", first_args);
            result.count = slots_of(x.type);
            element->object = result.place;
            element->index = out.local(index);
            out.e.exprs.push_back(x);
            result.place = flat_expr_id(out.e.exprs.size() - 1);
            result.pieces.back() += "[";
            result.values.push_back(out.local(index));
            result.pieces.push_back(cc::string("]"));
            return result;
        }
        return {};
    }

    flat_expr_id uint_call(cc::string_view name, flat_expr_id a, flat_expr_id b)
    {
        flat_expr_id const args[] = {a, b};
        return out.call(name, args);
    }

    /// The bits of shadow word `word` that the scalars `first ..< first + count` take, 0 where it holds none, as a
    /// `uint` local; a shift by 32 has no value on any target, so a whole word is its own case.
    local_id bits_of(local_id word, local_id first, u32 count, stmt_list& into)
    {
        auto const uint_type = out.type_named(builtins::k_uint);
        auto const pinned = [&](cc::string_view name, flat_expr_id value)
        {
            auto const local = out.add_local(local_kind::temporary, name, uint_type);
            into.push_back(out.let(local, value));
            return local;
        };
        auto const base = pinned("base", uint_call("shift_left_uint", out.local(word), uint_literal(5)));
        auto const lo = pinned("lo", uint_call("max", out.local(first), out.local(base)));
        auto const hi = pinned("hi", uint_call("min", uint_call("add_uint", out.local(first), uint_literal(i32(count))),
                                               uint_call("add_uint", out.local(base), uint_literal(32))));
        auto const bits = add_temporary_var(out, "bits", uint_type);
        into.push_back(out.var(bits, uint_literal(0)));
        auto const n = out.add_local(local_kind::temporary, "n", uint_type);
        auto const all = out.add_expr(uint_type, flat_int_literal{.value = -1, .is_unsigned = true, .is_hex = true});
        auto const low_bits
            = uint_call("subtract_uint", uint_call("shift_left_uint", uint_literal(1), out.local(n)), uint_literal(1));
        flat_stmt_id const whole[] = {out.assign(out.local(bits), all)};
        flat_stmt_id const part[]
            = {out.assign(out.local(bits), uint_call("shift_left_uint", low_bits,
                                                     uint_call("subtract_uint", out.local(lo), out.local(base))))};
        flat_stmt_id const held[]
            = {out.let(n, uint_call("subtract_uint", out.local(hi), out.local(lo))),
               out.if_(compare("greater_equal", shape{.element = builtins::k_uint}, out.local(n), uint_literal(32)),
                       whole, part)};
        into.push_back(
            out.if_(compare("greater", shape{.element = builtins::k_uint}, out.local(hi), out.local(lo)), held));
        return bits;
    }

    /// Calls `each(word, bits)` for every word of the shadow that the scalars `first ..< first + count` take, with the
    /// bits they take there; `first` is a pinned `uint`.
    template <class Each>
    void for_each_word(local_id first, u32 count, stmt_list& into, Each const& each)
    {
        // the most words `count` scalars can touch, starting anywhere in one
        auto const words = i32((count + 62) / 32);
        auto const at = [&](flat_expr_id k, stmt_list& body)
        {
            auto const word = out.add_local(local_kind::temporary, "word", out.type_named(builtins::k_uint));
            body.push_back(out.let(
                word, uint_call("add_uint", uint_call("shift_right_uint", out.local(first), uint_literal(5)), k)));
            each(word, bits_of(word, first, count, body), body);
        };
        if (words <= 4)
        {
            for (auto k = 0; k < words; ++k)
                at(uint_literal(k), into);
            return;
        }
        auto const label = out.add_label("words");
        auto const k = out.add_local(local_kind::index, "k", out.type_named(builtins::k_int));
        auto body = stmt_list();
        flat_expr_id const k_args[] = {out.local(k)};
        at(out.call("convert_int_to_uint", k_args), body);
        into.push_back(out.for_(label, k, out.int_literal(0), out.int_literal(words), body));
    }

    /// Whether every scalar `first ..< first + count` was stored by some invocation, as a pinned `bool`.
    local_id stored(local_id first, u32 count, stmt_list& into)
    {
        auto const ok = add_temporary_var(out, "stored", out.type_named(builtins::k_bool));
        into.push_back(out.var(ok, out.bool_literal(true)));
        for_each_word(first, count, into,
                      [&](local_id word, local_id bits, stmt_list& body)
                      {
                          flat_expr_id const word_args[] = {out.local(word)};
                          auto const held = uint_call("bit_and_uint", out.call("workgroup_written_word", word_args),
                                                      out.local(bits));
                          auto const missing = out.and_(
                              compare("not_equal", shape{.element = builtins::k_uint}, out.local(bits), uint_literal(0)),
                              compare("not_equal", shape{.element = builtins::k_uint}, held, out.local(bits)));
                          flat_stmt_id const unset[] = {out.assign(out.local(ok), out.bool_literal(false))};
                          body.push_back(out.if_(missing, unset));
                      });
        return ok;
    }

    void mark_stored(local_id first, u32 count, stmt_list& into)
    {
        for_each_word(
            first, count, into,
            [&](local_id word, local_id bits, stmt_list& body)
            {
                flat_expr_id const mark_args[] = {out.local(word), out.local(bits)};
                flat_stmt_id const mark[] = {out.eval(out.call("workgroup_mark_written", mark_args))};
                body.push_back(out.if_(
                    compare("not_equal", shape{.element = builtins::k_uint}, out.local(bits), uint_literal(0)), mark));
            });
    }

    /// The scalars `place` reaches, each range as a pinned first scalar and its size.
    cc::vector<cc::pair<local_id, u32>> ranges_of(shadow_place const& place, stmt_list& into)
    {
        auto const first = out.add_local(local_kind::temporary, "first", out.type_named(builtins::k_uint));
        into.push_back(out.let(first, place.first));
        auto result = cc::vector<cc::pair<local_id, u32>>();
        if (place.fields.empty())
        {
            result.push_back({first, place.count});
            return result;
        }
        for (auto const offset : place.fields)
        {
            auto const field = out.add_local(local_kind::temporary, "first", out.type_named(builtins::k_uint));
            into.push_back(out.let(field, uint_call("add_uint", out.local(first), uint_literal(i32(offset)))));
            result.push_back({field, 1});
        }
        return result;
    }

    /// A test of every range of `place`, failing as a guard does where one was never stored.
    void test_stored(shadow_place const& place, cc::span<cc::pair<local_id, u32> const> ranges, stmt_list& into)
    {
        for (auto const& [first, count] : ranges)
        {
            auto const ok = stored(first, count, into);
            auto pieces = place.pieces;
            pieces.back() += "` is read before any invocation stored it";
            into.push_back(out.if_(out.not_(out.local(ok)), failed(cc::move(pieces), place.values, {})));
        }
    }

    /// `id`, a read of workgroup memory, as a block that ends the invocation where a scalar it reads was never stored.
    flat_expr_id guard_workgroup_read(flat_expr_id id)
    {
        auto const x = out.e.at(id);
        out.from = x.from;
        out.inlined_through = x.inlined_through;
        auto stmts = stmt_list();
        auto const place = pinned_place(id, stmts);
        if (!place.has_value())
            return id;
        auto const label = out.add_label("guard");
        auto const ranges = ranges_of(place.value(), stmts);
        test_stored(place.value(), ranges, stmts);
        stmts.push_back(out.leave(label, place.value().place));
        return out.block_expr(label, x.type, stmts);
    }

    /// Whether `call` is an atomic or a uniform load whose first argument is workgroup memory the shadow covers.
    [[nodiscard]] bool is_workgroup_call(flat_call const& call) const
    {
        if (shadow.empty() || !is_valid(call.intrinsic) || index_of(call.intrinsic) >= registry.functions.size()
            || !is_known(out.e, call.arguments) || call.arguments.count < 1)
            return false;
        auto const& r = registry.at(call.intrinsic);
        return (r.is_atomic || r.is_uniform_load) && is_workgroup_place(out.e.at(call.arguments)[0]);
    }

    /// `id`, an atomic or a uniform load of workgroup memory, with its place pinned and the mark of what it stores after.
    /// What it reads is tested before it, or after a uniform load, whose barrier makes another invocation's store visible.
    /// A call that gives nothing is a statement, which `into` receives instead of a block.
    flat_expr_id guard_workgroup_call(flat_expr_id id, stmt_list* into)
    {
        auto const x = out.e.at(id);
        auto call = *x.node.try_as<flat_call>();
        auto const& r = registry.at(call.intrinsic);
        out.from = x.from;
        out.inlined_through = x.inlined_through;
        auto stmts = stmt_list();
        auto arguments = cc::vector<flat_expr_id>();
        for (auto const a : out.e.at(call.arguments))
            arguments.push_back(a);
        auto const place = pinned_place(arguments[0], stmts);
        if (!place.has_value())
            return id;
        auto const ranges = ranges_of(place.value(), stmts);
        if (!r.is_uniform_load && r.name != "store")
            test_stored(place.value(), ranges, stmts);
        arguments[0] = place.value().place;
        call.arguments = out.expr_list(arguments);
        auto const guarded = out.add_expr(x.type, call);
        auto const label = out.add_label("guard");
        auto result = local_id::none;
        if (into != nullptr)
            stmts.push_back(out.eval(guarded));
        else
        {
            result = out.add_local(local_kind::temporary, "result", x.type);
            stmts.push_back(out.let(result, guarded));
        }
        if (r.is_uniform_load)
            test_stored(place.value(), ranges, stmts);
        else if (r.name != "load")
            for (auto const& [first, count] : ranges)
                mark_stored(first, count, stmts);
        if (into != nullptr)
        {
            for (auto const s : stmts)
                into->push_back(s);
            return id;
        }
        stmts.push_back(out.leave(label, out.local(result)));
        return out.block_expr(label, x.type, stmts);
    }

    /// `s`, a store to workgroup memory, as a block that pins its place, stores, then marks what it stored.
    flat_stmt_id guard_workgroup_store(flat_stmt const& s, flat_assign const& n)
    {
        out.from = s.from;
        out.inlined_through = s.inlined_through;
        auto stmts = stmt_list();
        auto const place = pinned_place(n.place, stmts);
        if (!place.has_value())
            return flat_stmt_id::none;
        auto const ranges = ranges_of(place.value(), stmts);
        stmts.push_back(out.assign(place.value().place, n.value));
        for (auto const& [first, count] : ranges)
            mark_stored(first, count, stmts);
        return out.block(out.add_label("stored"), stmts);
    }

    // ---- the walk -------------------------------------------------------------------------------------------------------

    flat_expr_id expr(flat_expr_id id, int depth)
    {
        auto const is_chain_object = next_is_chain_object;
        next_is_chain_object = false;
        if (!is_known(out.e, id) || depth > k_max_depth)
            return id;
        auto copy = out.e.at(id);
        auto changed = false;
        auto const operand = [&](flat_expr_id& x)
        {
            auto const y = expr(x, depth + 1);
            changed = changed || y != x;
            x = y;
        };
        auto const tests_reads = guards && !shadow.empty() && depth + k_guard_depth_reserve <= k_max_depth;
        if (auto* const member = copy.node.try_as<flat_member>())
        {
            next_is_chain_object = true;
            operand(member->object);
        }
        else if (auto* const buffer = copy.node.try_as<flat_buffer_element>())
        {
            operand(buffer->buffer);
            operand(buffer->index);
        }
        else if (auto* const length = copy.node.try_as<flat_buffer_length>())
            operand(length->buffer);
        else if (auto* const element = copy.node.try_as<flat_element>())
        {
            next_is_chain_object = true;
            operand(element->object);
            operand(element->index);
        }
        else if (auto* const construct = copy.node.try_as<flat_construct>())
            construct->arguments = exprs(construct->arguments, depth, changed);
        else if (auto* const call = copy.node.try_as<flat_call>())
        {
            if (tests_reads && is_workgroup_call(*call))
                own_places.push_back(out.e.at(call->arguments)[0]);
            if (guards && is_buffer_call(*call))
                own_places.push_back(out.e.at(call->arguments)[0]);
            call->arguments = exprs(call->arguments, depth, changed);
        }
        else if (auto* const n = copy.node.try_as<flat_not>())
            operand(n->operand);
        else if (auto* const a = copy.node.try_as<flat_and>())
        {
            operand(a->lhs);
            operand(a->rhs);
        }
        else if (auto* const o = copy.node.try_as<flat_or>())
        {
            operand(o->lhs);
            operand(o->rhs);
        }
        else if (auto* const b = copy.node.try_as<flat_block>())
            b->body = body(b->body, depth, changed);

        auto const may_guard = guards && depth + k_guard_depth_reserve <= k_max_depth;
        if (auto* const element = copy.node.try_as<flat_element>(); element != nullptr && may_guard)
            if (auto const index = guarded_index(copy, *element); is_valid(index))
            {
                element->index = index;
                changed = true;
            }
        if (auto* const element = copy.node.try_as<flat_buffer_element>(); element != nullptr && may_guard)
            if (auto const index = guarded_buffer_index(copy, *element); is_valid(index))
            {
                element->index = index;
                changed = true;
            }
        auto result = id;
        if (changed)
        {
            out.e.exprs.push_back(cc::move(copy));
            result = flat_expr_id(out.e.exprs.size() - 1);
        }
        // LEGAL-69: a read of workgroup memory tests the shadow; a place inside a place is no read of its own
        if (tests_reads && !is_chain_object && !own_places.values().contains(id) && is_workgroup_place(result))
            return guard_workgroup_read(result);
        // LEGAL-66: a read of a binding array's buffer, and every read under `define`, holds its own test
        if (may_guard && !is_chain_object && !own_places.values().contains(id) && is_whole_buffer_access(result))
            return guard_buffer_read(result);
        if (auto const* const call = out.e.at(result).node.try_as<flat_call>();
            call != nullptr && tests_reads && out.e.at(result).type != checked_module::void_type
            && is_workgroup_call(*call))
            return guard_workgroup_call(result, nullptr);
        if (auto const* const call = out.e.at(result).node.try_as<flat_call>();
            call != nullptr && may_guard && out.e.at(result).type != checked_module::void_type && is_buffer_call(*call))
            return guard_buffer_call(result, nullptr);
        if (auto const* const call = out.e.at(result).node.try_as<flat_call>();
            call != nullptr && may_guard && is_integer_division(*call))
            return guard_division(result);
        if (auto const* const call = out.e.at(result).node.try_as<flat_call>(); call != nullptr && may_guard)
            if (auto const name = guarded_math_of(*call); !name.empty())
                return guard_math(result, name);
        if (auto const* const call = out.e.at(result).node.try_as<flat_call>(); call != nullptr && may_guard)
            if (auto const words = bytes_words_of(*call);
                words > 0 && !(defines && out.e.at(result).type == checked_module::void_type))
                return guard_bytes(result, words, nullptr);
        if (auto const* const call = out.e.at(result).node.try_as<flat_call>(); call != nullptr && may_guard)
            if (auto const size = matrix_part_size_of(*call); size > 0)
                return guard_matrix_part(result, size);
        return result;
    }

    ast::range_of<flat_expr_id> exprs(ast::range_of<flat_expr_id> range, int depth, bool& changed)
    {
        if (!is_known(out.e, range))
            return range;
        auto list = cc::vector<flat_expr_id>();
        for (auto const id : out.e.at(range))
            list.push_back(id);
        auto any = false;
        for (auto& id : list)
        {
            auto const y = expr(id, depth + 1);
            any = any || y != id;
            id = y;
        }
        if (!any)
            return range;
        changed = true;
        return out.expr_list(list);
    }

    ast::range_of<flat_stmt_id> body(ast::range_of<flat_stmt_id> range, int depth, bool& changed)
    {
        if (!is_known(out.e, range))
            return range;
        auto list = stmt_list();
        for (auto const id : out.e.at(range))
            list.push_back(id);
        auto any = false;
        for (auto& id : list)
        {
            auto const y = stmt(id, depth + 1);
            any = any || y != id;
            id = y;
        }
        if (!any)
            return range;
        changed = true;
        return out.stmt_list(list);
    }

    ast::range_of<flat_arm> arms(ast::range_of<flat_arm> range, int depth, bool& changed)
    {
        if (!is_known(out.e, range))
            return range;
        auto list = cc::vector<flat_arm>();
        for (auto const& arm : out.e.at(range))
            list.push_back(arm);
        auto any = false;
        for (auto& arm : list)
        {
            arm.patterns = exprs(arm.patterns, depth, any);
            arm.body = body(arm.body, depth, any);
        }
        if (!any)
            return range;
        changed = true;
        return out.arm_list(list);
    }

    /// LEGAL-64 for an `assert` a barrier may still follow: its body, then its message printed and the flag set where
    /// its condition is false, as a block of its own; the invocation ends with the guards' after that barrier.
    flat_stmt_id deferred_assert(flat_stmt const& s, flat_check const& k)
    {
        out.from = s.from;
        out.inlined_through = s.inlined_through;
        auto list = stmt_list();
        if (is_known(out.e, k.body))
            for (auto const id : out.e.at(k.body))
                list.push_back(id);
        auto const site = out.e.check_sites[k.site];
        if (site.nodes.count > 0 && isize(site.nodes.first) < out.e.check_nodes.size())
        {
            auto const condition = out.e.check_nodes[site.nodes.first].value;
            auto message = site.message;
            if (message < 0)
            {
                out.e.messages.push_back({.pieces = {cc::string()}});
                message = i32(out.e.messages.size() - 1);
            }
            out.e.messages[message].is_assert = true;
            auto values = cc::vector<flat_expr_id>();
            if (site.message_values.count > 0 && is_known(out.e, site.message_values))
                for (auto const v : out.e.at(site.message_values))
                    values.push_back(v);
            auto failed = stmt_list();
            failed.push_back(out.print(message, values));
            failed.push_back(out.assign(out.local(aborted), out.bool_literal(true)));
            sets_flag = true;
            list.push_back(out.if_(out.not_(out.local(condition)), failed));
        }
        return out.block(out.add_label("assert"), list);
    }

    /// A loop's `body` with `if aborted { leave label }` in front, where `breaks_loops` asks for it and the body is free
    /// of what every invocation must reach together.
    /// Whether `s` writes memory another invocation may read: a store to a buffer element or to workgroup memory, or
    /// a call with an effect that is no barrier, derivative or subgroup operation, which every invocation must reach.
    [[nodiscard]] bool writes_memory(flat_stmt const& s) const
    {
        if (auto const* const assign = s.node.try_as<flat_assign>())
        {
            auto place = assign->place;
            for (auto steps = 0; steps < k_max_depth && is_known(out.e, place); ++steps)
            {
                auto const& x = out.e.at(place);
                if (auto const* const member = x.node.try_as<flat_member>())
                    place = member->object;
                else if (auto const* const element = x.node.try_as<flat_element>())
                    place = element->object;
                else if (auto const* const b = x.node.try_as<flat_binding_member>())
                    return b->is_workgroup;
                else
                    return x.node.is<flat_buffer_element>();
            }
            return false;
        }
        if (auto const* const eval = s.node.try_as<flat_eval>(); eval != nullptr && is_known(out.e, eval->value))
            if (auto const* const call = out.e.at(eval->value).node.try_as<flat_call>())
                return !call->is_pure && !constrains_uniformity(*call);
        return false;
    }

    /// `s` as `let x = (0 where the flag is set, else the call)` where `s` binds the result of a call that writes memory,
    /// a number scalar's; `none` for any other statement.
    flat_stmt_id masked_binding(flat_stmt const& s)
    {
        auto const* const let = s.node.try_as<flat_let>();
        if (let == nullptr || !is_known(out.e, let->value))
            return flat_stmt_id::none;
        auto const* const call = out.e.at(let->value).node.try_as<flat_call>();
        if (call == nullptr || call->is_pure || constrains_uniformity(*call))
            return flat_stmt_id::none;
        auto const type = out.e.at(let->value).type;
        auto const name = out.m.name_of(type);
        auto const zero = name == builtins::k_int   ? out.int_literal(0)
                        : name == builtins::k_uint  ? uint_literal(0)
                        : name == builtins::k_float ? out.literal(0.0)
                                                    : flat_expr_id::none;
        if (!is_valid(zero))
            return flat_stmt_id::none;
        out.from = s.from;
        out.inlined_through = s.inlined_through;
        auto const label = out.add_label("masked");
        auto skipped = stmt_list();
        skipped.push_back(out.leave(label, zero));
        auto stmts = stmt_list();
        stmts.push_back(out.if_(out.local(aborted), skipped));
        stmts.push_back(out.leave(label, let->value));
        return out.let(let->local, out.block_expr(label, type, stmts));
    }

    ast::range_of<flat_stmt_id> left_once_aborted(label_id label, ast::range_of<flat_stmt_id> range, bool& changed)
    {
        if (!breaks_loops || !is_valid(label) || !is_known(out.e, range) || body_constrains(range, 0))
            return range;
        auto exit = stmt_list();
        exit.push_back(out.leave(label));
        auto list = stmt_list();
        list.push_back(out.if_(out.local(aborted), exit));
        for (auto const id : out.e.at(range))
            list.push_back(id);
        changed = true;
        return out.stmt_list(list);
    }

    flat_stmt_id stmt(flat_stmt_id id, int depth)
    {
        if (!is_known(out.e, id) || depth > k_max_depth)
            return id;
        auto copy = out.e.at(id);
        auto changed = false;
        auto const value = [&](flat_expr_id& x)
        {
            auto const y = expr(x, 0);
            changed = changed || y != x;
            x = y;
        };
        if (auto* const n = copy.node.try_as<flat_let>())
            value(n->value);
        else if (auto* const n = copy.node.try_as<flat_var>())
            value(n->value);
        else if (auto* const n = copy.node.try_as<flat_assign>())
        {
            // LEGAL-69: a store to workgroup memory marks what it stored, and is no read of it
            auto const is_store = guards && !shadow.empty() && is_workgroup_place(n->place);
            auto const is_buffer_store = guards && is_whole_buffer_access(n->place);
            if (is_store || is_buffer_store)
                own_places.push_back(n->place);
            value(n->place);
            value(n->value);
            if (is_store)
                if (auto const block = guard_workgroup_store(copy, *n); is_valid(block))
                    return block;
            if (is_buffer_store)
                if (auto const block = guard_buffer_store(copy, *n); is_valid(block))
                    return block;
        }
        else if (auto* const n = copy.node.try_as<flat_print>())
            n->values = exprs(n->values, 0, changed);
        else if (auto* const n = copy.node.try_as<flat_eval>())
        {
            value(n->value);
            // an atomic store gives nothing, so its test and its mark stand as statements around it
            if (auto const* const call = out.e.at(n->value).node.try_as<flat_call>();
                call != nullptr && guards && out.e.at(n->value).type == checked_module::void_type
                && is_workgroup_call(*call))
            {
                auto stmts = stmt_list();
                guard_workgroup_call(n->value, &stmts);
                if (!stmts.empty())
                    return out.block(out.add_label("stored"), stmts);
            }
            if (auto const* const call = out.e.at(n->value).node.try_as<flat_call>();
                call != nullptr && guards && out.e.at(n->value).type == checked_module::void_type)
            {
                auto stmts = stmt_list();
                if (is_buffer_call(*call))
                    guard_buffer_call(n->value, &stmts);
                else if (auto const words = bytes_words_of(*call); defines && words > 0)
                    guard_bytes(n->value, words, &stmts);
                if (!stmts.empty())
                    return out.block(out.add_label("stored"), stmts);
            }
        }
        else if (auto* const n = copy.node.try_as<flat_if>())
        {
            value(n->condition);
            n->then_body = body(n->then_body, depth, changed);
            n->else_body = body(n->else_body, depth, changed);
        }
        else if (auto* const n = copy.node.try_as<flat_block>())
            n->body = body(n->body, depth, changed);
        else if (auto* const n = copy.node.try_as<flat_leave>())
            value(n->value);
        else if (auto* const n = copy.node.try_as<flat_loop>())
            n->body = left_once_aborted(n->label, body(n->body, depth, changed), changed);
        else if (auto* const n = copy.node.try_as<flat_while>())
        {
            value(n->condition);
            n->body = left_once_aborted(n->label, body(n->body, depth, changed), changed);
        }
        else if (auto* const n = copy.node.try_as<flat_for>())
        {
            value(n->first);
            value(n->end);
            n->body = left_once_aborted(n->label, body(n->body, depth, changed), changed);
        }
        else if (auto* const n = copy.node.try_as<flat_once>())
            n->body = body(n->body, depth, changed);
        else if (auto* const n = copy.node.try_as<flat_case>())
        {
            value(n->scrutinee);
            n->arms = arms(n->arms, depth, changed);
            n->default_body = body(n->default_body, depth, changed);
        }
        else if (auto* const n = copy.node.try_as<flat_switch>())
        {
            value(n->scrutinee);
            n->arms = arms(n->arms, depth, changed);
            n->default_body = body(n->default_body, depth, changed);
        }
        else if (auto* const n = copy.node.try_as<flat_return>())
            value(n->value);
        else if (auto* const n = copy.node.try_as<flat_check>())
        {
            n->body = body(n->body, depth, changed);
            if (defers && lowers_asserts && n->site >= 0 && n->site < out.e.check_sites.size()
                && out.e.check_sites[n->site].stops)
                return deferred_assert(copy, *n);
        }
        auto result = id;
        if (changed)
        {
            out.e.stmts.push_back(cc::move(copy));
            result = flat_stmt_id(out.e.stmts.size() - 1);
        }
        // LEGAL-67: an atomic whose result a let binds runs only where the flag is clear, and gives 0 where it is set
        if (breaks_loops)
            if (auto const masked = masked_binding(out.e.at(result)); is_valid(masked))
                return masked;
        // LEGAL-67: an invocation whose guard tripped computes on with a safe operand, and writes none of it
        if (breaks_loops && writes_memory(out.e.at(result)))
        {
            auto const& s = out.e.at(result);
            out.from = s.from;
            out.inlined_through = s.inlined_through;
            auto then = stmt_list();
            then.push_back(result);
            return out.if_(out.not_(out.local(aborted)), then);
        }
        return result;
    }
};
/// The bytes `type` takes in workgroup memory, laid out as WGSL lays out workgroup variables, as CHK-293 measures it.
i32 workgroup_bytes_of(checked_module const& m, type_id type)
{
    struct measured
    {
        i32 size = 0;
        i32 alignment = 1;
    };
    auto const round_up = [](i32 n, i32 to) { return (n + to - 1) / to * to; };
    auto const measure = [&](auto const& self, type_id t) -> measured
    {
        if (auto const* const record = m.builtin_type_of(t))
        {
            if (record->wgsl_layout.size == 0)
                return {.size = 4 * record->leaf_count, .alignment = 4};
            return {.size = record->wgsl_layout.size, .alignment = record->wgsl_layout.alignment};
        }
        auto const& info = m.at(t);
        if (info.kind == type_kind::atomic)
            return self(self, info.element);
        if (info.kind == type_kind::enumeration)
            return {.size = 4, .alignment = 4};
        if (info.kind == type_kind::array)
        {
            auto const element = self(self, info.element);
            return {.size = round_up(element.size, element.alignment) * info.count, .alignment = element.alignment};
        }
        auto result = measured();
        for (auto const& member : m.at(info.members))
        {
            auto const inner = self(self, member.type);
            result.size = round_up(result.size, inner.alignment) + inner.size;
            result.alignment = inner.alignment > result.alignment ? inner.alignment : result.alignment;
        }
        result.size = round_up(result.size, result.alignment);
        return result;
    };
    return measure(measure, type).size;
}

/// LEGAL-69: the shadow of a compute, task or mesh stage's workgroup memory where the profile tests reads, or its zeroing under
/// `define`, and the invocation's index in its workgroup that LEGAL-71's prologue strides by.
void prepare_workgroup(flat_builder& out, guarder& g)
{
    auto& e = out.e;
    auto members = cc::vector<guarder::shadow_member>();
    auto slots = u32(0);
    auto bytes = 0;
    for (auto const id : e.bindings)
    {
        auto const& b = out.m.bindings[out.m.at(id).info];
        if (!b.is_workgroup)
            continue;
        auto const list = out.m.at(b.members);
        for (auto i = isize(0); i < list.size(); ++i)
        {
            members.push_back({.binding = id, .member = i32(i), .first = slots});
            slots += g.slots_of(list[i].type);
            bytes += workgroup_bytes_of(out.m, list[i].type);
        }
    }
    auto const words = i32((slots + 31) / 32);
    // the shadow takes a word per 32 scalars, beside the memory it shadows, in the budget CHK-294 holds every stage to
    constexpr auto k_portable_workgroup_bytes = 16384;
    auto const shadows = !g.defines && bytes + 4 * words <= k_portable_workgroup_bytes;
    // a compile says so rather than testing nothing in silence
    if (!members.empty() && !g.defines && !shadows)
        e.unshadowed_bytes = bytes;
    if (members.empty() || (!g.defines && !shadows))
        return;

    for (auto const& input : e.stage_inputs)
        if (input.input == stage_input::local_thread_index)
            e.workgroup_index = input.local;
    if (!is_valid(e.workgroup_index))
    {
        e.workgroup_index = out.add_local(local_kind::parameter, "local_index", out.type_named(builtins::k_int));
        e.stage_inputs.push_back({.input = stage_input::local_thread_index, .local = e.workgroup_index});
    }
    if (g.defines)
    {
        e.zeroes_workgroup = true;
        return;
    }
    e.written_words = words;
    g.shadow = cc::move(members);
}
} // namespace

stmt_list sgl::check::impl::guard_undefined(flat_builder& out, stmt_list const& body, legalize_options const& options)
{
    if (options.guards_undefined == undefined_guard::none && !options.lowers_asserts)
        return body;
    auto g = guarder{.out = out,
                     .guards = options.guards_undefined != undefined_guard::none,
                     .reports = options.guards_undefined == undefined_guard::report && !options.erases_prints,
                     .guards_buffers = options.guards_buffers,
                     .defines = options.guards_undefined == undefined_guard::define,
                     .lowers_asserts = options.lowers_asserts};
    // the stages that have workgroup memory: a compute stage, and a task and a mesh stage, which are dispatched alike
    auto const has_workgroup
        = out.e.entry_stage == stage::compute || out.e.entry_stage == stage::task || out.e.entry_stage == stage::mesh;
    if (g.guards && has_workgroup)
        prepare_workgroup(out, g);

    // The last statement of the body that holds a barrier, a derivative or a subgroup operation, which an invocation
    // that ended earlier would leave the others waiting at or reading from; a guard before it sets a flag instead.
    auto last = isize(-1);
    for (auto i = isize(0); i < body.size(); ++i)
        if (g.stmt_constrains(body[i], 0))
            last = i;
    if (last >= 0)
        g.aborted = add_temporary_var(out, "aborted", out.type_named(builtins::k_bool));

    auto result = stmt_list();
    // past the test of the flag, a set flag has ended the invocation and no loop can see it
    auto flag_seen_up_to = isize(0);
    for (auto i = isize(0); i < body.size(); ++i)
    {
        g.defers = i <= last;
        result.push_back(g.stmt(body[i], 0));
        if (i == last && g.sets_flag)
        {
            out.from = out.e.at(body[i]).from;
            out.inlined_through = out.e.at(body[i]).inlined_through;
            auto ends = stmt_list();
            end_invocation(out, ends);
            result.push_back(out.if_(out.local(g.aborted), ends));
            flag_seen_up_to = result.size();
        }
    }
    if (!g.sets_flag)
        return result;

    // The loops the flag may still find running, those of the statements up to the last one that holds such an operation
    g.guards = false;
    g.lowers_asserts = false;
    g.breaks_loops = true;
    for (auto i = isize(0); i < flag_seen_up_to; ++i)
    {
        auto const& s = out.e.at(result[i]);
        out.from = s.from;
        out.inlined_through = s.inlined_through;
        result[i] = g.stmt(result[i], 0);
    }
    result.insert_at(0, out.var(g.aborted, out.bool_literal(false)));
    return result;
}
