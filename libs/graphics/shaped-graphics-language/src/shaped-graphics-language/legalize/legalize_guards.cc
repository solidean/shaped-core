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
/// A guard nests its operands a level deeper; one that would pass the limit every later walk stops at is not written.
constexpr int k_guard_depth_reserve = 4;

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

    /// The index of `element`, an array's, as a block that ends the invocation where it is outside the array (EVAL-90);
    /// `none` where it needs no guard.
    flat_expr_id guarded_index(flat_expr const& x, flat_element const& element)
    {
        if (!is_known(out.e, element.object) || !is_known(out.e, element.index))
            return flat_expr_id::none;
        auto const& array = out.m.at(out.e.at(element.object).type);
        if (array.kind != type_kind::array || array.count <= 0 || is_resource(out.m.at(array.element).kind))
            return flat_expr_id::none;
        if (out.e.at(element.index).node.is<flat_int_literal>())
            return flat_expr_id::none;
        // a type, not a reference: the guard grows the tree it would point into
        auto const index_type = out.e.at(element.index).type;
        auto const index_known = integer_shape_of(index_type);
        if (!index_known.has_value() || !index_known.value().lanes.empty()
            || (index_known.value().element != "int" && index_known.value().element != "uint"))
            return flat_expr_id::none;
        auto const index_shape = index_known.value();

        out.from = x.from;
        out.inlined_through = x.inlined_through;
        auto const label = out.add_label("guard");
        auto stmts = stmt_list();
        auto const i = pinned("index", element.index, stmts);
        auto bad = out.not_(compare("less", index_shape, out.local(i), literal_for(index_shape, array.count)));
        if (index_shape.element == "int")
            bad = out.or_(compare("less", index_shape, out.local(i), out.int_literal(0)), bad);
        auto const value = out.local(i);
        auto const on_failure = failed({cc::string("index "), cc::format(" is outside 0 ..< {}", array.count)},
                                       cc::span<flat_expr_id const>(&value, 1), i, literal_for(index_shape, 0));
        stmts.push_back(out.if_(bad, on_failure));
        stmts.push_back(out.leave(label, out.local(i)));
        return out.block_expr(label, index_type, stmts);
    }

    /// The index of `element`, a buffer's, as a block that ends the invocation where it is past the bound buffer's end
    /// (EVAL-90, LEGAL-66); `none` where it needs no guard or the buffer is no binding member.
    flat_expr_id guarded_buffer_index(flat_expr const& x, flat_buffer_element const& element)
    {
        if (!guards_buffers || !is_known(out.e, element.buffer) || !is_known(out.e, element.index))
            return flat_expr_id::none;
        if (!out.e.at(element.buffer).node.is<flat_binding_member>())
            return flat_expr_id::none;
        auto const index_type = out.e.at(element.index).type;
        auto const index_known = integer_shape_of(index_type);
        if (!index_known.has_value() || !index_known.value().lanes.empty()
            || (index_known.value().element != "int" && index_known.value().element != "uint"))
            return flat_expr_id::none;
        auto const index_shape = index_known.value();
        auto const uint_shape = shape{.element = builtins::k_uint};

        out.from = x.from;
        out.inlined_through = x.inlined_through;
        auto const label = out.add_label("guard");
        auto stmts = stmt_list();
        auto const i = pinned("index", element.index, stmts);
        auto const length = pinned(
            "length", out.add_expr(out.type_named(builtins::k_uint), flat_buffer_length{.buffer = element.buffer}),
            stmts);
        auto unsigned_index = out.local(i);
        if (index_shape.element == "int")
        {
            flat_expr_id const argument[] = {out.local(i)};
            unsigned_index = out.call("convert_int_to_uint", argument);
        }
        auto bad = out.not_(compare("less", uint_shape, unsigned_index, out.local(length)));
        if (index_shape.element == "int")
            bad = out.or_(compare("less", index_shape, out.local(i), out.int_literal(0)), bad);
        flat_expr_id const values[] = {out.local(i), out.local(length)};
        auto const on_failure
            = failed({cc::string("index "), cc::string(" is outside a buffer of "), cc::string(" elements")}, values, i,
                     literal_for(index_shape, 0));
        stmts.push_back(out.if_(bad, on_failure));
        stmts.push_back(out.leave(label, out.local(i)));
        return out.block_expr(label, index_type, stmts);
    }

    /// How many words a load or a store of `bytes` reaches, or 0 for a call that is none (EVAL-97).
    [[nodiscard]] i32 bytes_words_of(flat_call const& call) const
    {
        if (!guards_buffers || !is_valid(call.intrinsic) || index_of(call.intrinsic) >= registry.functions.size()
            || !is_known(out.e, call.arguments) || call.arguments.count < 2)
            return 0;
        auto const arguments = out.e.at(call.arguments);
        if (out.m.at(out.e.at(arguments[0]).type).kind != type_kind::bytes
            || !out.e.at(arguments[0]).node.is<flat_binding_member>())
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
    flat_expr_id guard_bytes(flat_expr_id id, i32 words)
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
        // the memory is a resource and no local holds one; the offset and a stored value keep their order
        auto const offset = pinned("offset", operands[1], stmts);
        auto stored = local_id::none;
        if (operands.size() == 3)
            stored = pinned("stored", operands[2], stmts);
        auto const length = pinned(
            "length", out.add_expr(out.type_named(builtins::k_uint), flat_buffer_length{.buffer = operands[0]}), stmts);

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
        auto arguments = cc::vector<flat_expr_id>{operands[0], out.local(offset)};
        if (operands.size() == 3)
            arguments.push_back(out.local(stored));
        guarded.arguments = out.expr_list(arguments);
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
        auto const type = out.e.at(operands[0]).type;
        auto const known = number_shape_of(type);
        if (!known.has_value())
            return id;
        auto const s = known.value();
        for (auto const a : operands)
            if (out.e.at(a).type != type)
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
            auto const lane = [&](isize k) { return number_lane(locals[k], s, s.lanes.empty() ? 0 : i); };
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
        if (name == "pow")
            safe.push_back({.place = locals[0], .value = filled(type, s, 1.0)});
        else if (name == "asin" || name == "acos")
            safe.push_back({.place = locals[0], .value = filled(type, s, 0.0)});
        else if (name == "atan2")
            safe.push_back({.place = locals[1], .value = filled(type, s, 1.0)});
        else if (name == "smoothstep")
        {
            safe.push_back({.place = locals[0], .value = filled(type, s, 0.0)});
            safe.push_back({.place = locals[1], .value = filled(type, s, 1.0)});
        }
        else if (name == "clamp")
            safe.push_back({.place = locals[2], .value = out.local(locals[1])});

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

    // ---- the walk -------------------------------------------------------------------------------------------------------

    flat_expr_id expr(flat_expr_id id, int depth)
    {
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
        if (auto* const member = copy.node.try_as<flat_member>())
            operand(member->object);
        else if (auto* const buffer = copy.node.try_as<flat_buffer_element>())
        {
            operand(buffer->buffer);
            operand(buffer->index);
        }
        else if (auto* const element = copy.node.try_as<flat_element>())
        {
            operand(element->object);
            operand(element->index);
        }
        else if (auto* const construct = copy.node.try_as<flat_construct>())
            construct->arguments = exprs(construct->arguments, depth, changed);
        else if (auto* const call = copy.node.try_as<flat_call>())
            call->arguments = exprs(call->arguments, depth, changed);
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
        if (auto const* const call = out.e.at(result).node.try_as<flat_call>();
            call != nullptr && may_guard && is_integer_division(*call))
            return guard_division(result);
        if (auto const* const call = out.e.at(result).node.try_as<flat_call>(); call != nullptr && may_guard)
            if (auto const name = guarded_math_of(*call); !name.empty())
                return guard_math(result, name);
        if (auto const* const call = out.e.at(result).node.try_as<flat_call>(); call != nullptr && may_guard)
            if (auto const words = bytes_words_of(*call); words > 0)
                return guard_bytes(result, words);
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
            value(n->place);
            value(n->value);
        }
        else if (auto* const n = copy.node.try_as<flat_print>())
            n->values = exprs(n->values, 0, changed);
        else if (auto* const n = copy.node.try_as<flat_eval>())
            value(n->value);
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
