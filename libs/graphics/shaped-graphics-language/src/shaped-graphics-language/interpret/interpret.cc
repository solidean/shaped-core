#include "interpret.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/math/bit.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics-language/legalize/impl/walk.hh>

using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;

namespace
{
cc::span<member_info const> members_of(checked_module const& m, type_id type)
{
    if (!is_valid(type) || index_of(type) >= m.types.size())
        return {};
    auto const range = m.at(type).members;
    if (isize(range.first) + isize(range.count) > m.members.size())
        return {};
    return m.at(range);
}

void append_zero(checked_module const& m, type_id type, cc::vector<scalar>& leaves, int depth)
{
    if (depth > k_max_depth)
        return;
    // A builtin type says what it is made of, whether or not the prelude gives it fields.
    if (auto const* const record = m.builtin_type_of(type))
    {
        for (auto i = 0; i < record->leaf_count; ++i)
            leaves.push_back({.kind = record->leaf_kind, .bits = 0});
        return;
    }
    if (is_valid(type) && m.at(type).kind == type_kind::enumeration)
    {
        leaves.push_back({.kind = value_kind::scalar_int, .bits = 0});
        return;
    }
    if (is_valid(type) && m.at(type).kind == type_kind::atomic)
    {
        append_zero(m, m.at(type).element, leaves, depth + 1);
        return;
    }
    if (is_valid(type) && m.at(type).kind == type_kind::array)
    {
        for (auto i = 0; i < m.at(type).count; ++i)
            append_zero(m, m.at(type).element, leaves, depth + 1);
        return;
    }
    for (auto const& member : members_of(m, type))
        append_zero(m, member.type, leaves, depth + 1);
}

enum class flow_kind : u8
{
    normal,
    leave,
    continue_,
    break_,
    return_,
    failed,
};

/// What a statement or an expression ended in; an expression can exit too, since it may hold a block.
struct flow
{
    flow_kind kind = flow_kind::normal;
    label_id label = label_id::none;

    [[nodiscard]] bool is_normal() const { return kind == flow_kind::normal; }
};

struct machine
{
    checked_module const& m;
    flat_entry_point const& e;
    run_inputs const& inputs;
    run_limits const& limits;
    i64 fuel = 0;
    outcome out;

    cc::vector<value> locals;
    cc::vector<bool> is_set;

    /// One member of a `@workgroup` binding, which the run holds from its start: memory of one invocation's workgroup.
    struct workgroup_cell
    {
        symbol_id binding = symbol_id::none;
        i32 member = -1;
        value memory;
        /// Parallel to `memory.leaves`: nothing is defined before it is stored (CHK-292).
        cc::vector<bool> is_written;
    };
    cc::vector<workgroup_cell> workgroup;
    /// Parallel to `out.buffers`: whether the run stored to that buffer, which is what the outcome keeps.
    cc::vector<bool> is_stored;
    /// The value a `leave` or a `return` under way carries.
    value carried;
    int depth = 0;

    flow fail(run_status status, cc::string detail)
    {
        if (out.status == run_status::ok)
        {
            out.status = status;
            out.detail = cc::move(detail);
        }
        return {.kind = flow_kind::failed};
    }

    flow type_error(cc::string detail) { return fail(run_status::type_error, cc::move(detail)); }

    /// One step; false when the run is over.
    bool burn()
    {
        if (--fuel >= 0)
        {
            // a relaxed load every 4096 steps: a stop is seen within a fraction of a millisecond, and costs the run nothing measurable
            if (limits.stop != nullptr && (fuel & 4095) == 0 && limits.stop->load(cc::memory_order_relaxed))
            {
                fail(run_status::stopped, "");
                return false;
            }
            return true;
        }
        fail(run_status::out_of_fuel, "");
        return false;
    }

    // ---- expressions ------------------------------------------------------------------------------------------------

    flow eval_all(ast::range_of<flat_expr_id> range, cc::vector<value>& values)
    {
        if (!is_known(e, range))
            return type_error("an argument list that reaches outside the tree");
        for (auto const id : e.at(range))
        {
            auto v = value();
            if (auto const f = eval(id, v); !f.is_normal())
                return f;
            values.push_back(cc::move(v));
        }
        return {};
    }

    flow slice_member(value const& object, i32 index, value& result)
    {
        auto const members = members_of(m, object.type);
        if (index < 0 || index >= members.size())
            return type_error(cc::format("member {} of '{}'", index, m.name_of(object.type)));
        auto offset = isize(0);
        for (auto i = 0; i < index; ++i)
            offset += leaf_count_of(m, members[i].type);
        auto const count = leaf_count_of(m, members[index].type);
        if (offset + count > object.leaves.size())
            return type_error("a value with fewer scalars than its type");
        result.type = members[index].type;
        result.leaves.clear();
        result.leaves.push_back_range(cc::span<scalar const>(object.leaves).subspan({.offset = offset, .size = count}));
        return {};
    }

    /// Where element `index` of an array of `type` starts among its scalars; outside it is a program error (EVAL-90).
    flow element_offset(type_id type, value const& index, isize& offset, isize& count)
    {
        if (index.leaves.size() != 1 || index.leaves[0].kind != value_kind::scalar_int)
            return type_error("an array index that is no int");
        if (!is_valid(type) || m.at(type).kind != type_kind::array)
            return type_error("an element of what is no array");
        auto const& info = m.at(type);
        auto const at = isize(index.leaves[0].as_int());
        if (at < 0 || at >= info.count)
            return fail(run_status::program_error,
                        cc::format("the index {} is out of bounds of {}", at, m.name_of(type)));
        count = leaf_count_of(m, info.element);
        offset = at * count;
        return {};
    }

    /// EVAL-93: the atomic's place, then the other arguments, then the update, in one step.
    flow atomic_call(flat_expr const& x, flat_call const& c, builtins::function_record const& record, value& result)
    {
        auto const arguments = e.at(c.arguments);
        if (arguments.empty() || !is_known(e, arguments[0]))
            return type_error("an atomic call without its atomic");

        // where the atomic is: an element of a buffer's scalars, or of a cell of workgroup memory
        auto buffer = isize(-1);
        auto where = place_ref();
        auto offset = isize(0);
        auto const atomic = arguments[0];
        if (auto const* const element = e.at(atomic).node.try_as<flat_buffer_element>())
        {
            if (auto const f = locate(*element, e.at(atomic).type, buffer, offset); !f.is_normal())
                return f;
        }
        else if (is_in_workgroup(atomic))
        {
            auto type = type_id::none;
            if (auto const f = locate_place(atomic, where, offset, type); !f.is_normal())
                return f;
        }
        else
            return type_error("an atomic that is no buffer element and no workgroup memory");

        auto in = cc::vector<scalar>();
        in.push_back({});
        for (auto k = isize(1); k < arguments.size(); ++k)
        {
            auto v = value();
            if (auto const f = eval(arguments[k], v); !f.is_normal())
                return f;
            in.push_back_range(v.leaves);
        }

        auto const is_store = !is_valid(record.result);
        if (where.cell >= 0)
        {
            auto const& cell = workgroup[where.cell];
            if (!is_store && !cell.is_written[offset])
                return fail(
                    run_status::program_error,
                    cc::format("an atomic {} of {}.{} where nothing was stored", record.name, m.at(cell.binding).name,
                               m.at(m.bindings[m.at(cell.binding).info].members)[cell.member].name));
            in[0] = cell.memory.leaves[offset];
        }
        else
            in[0] = out.buffers[buffer].leaves[offset];

        auto after = cc::vector<scalar>();
        record.evaluate(in, after);
        if (after.size() != 1)
            return type_error(cc::format("an atomic '{}' whose evaluator gave no one value", record.name));
        if (where.cell >= 0)
        {
            workgroup[where.cell].memory.leaves[offset] = after[0];
            workgroup[where.cell].is_written[offset] = true;
        }
        else
        {
            out.buffers[buffer].leaves[offset] = after[0];
            is_stored[buffer] = true;
        }

        result.type = x.type;
        result.leaves.clear();
        if (!is_store)
            result.leaves.push_back(in[0]);
        out.trace.push_back(result);
        return {};
    }

    flow call(flat_expr const& x, flat_call const& c, value& result)
    {
        if (auto const* const record = m.builtin_function(c.intrinsic); record != nullptr && record->is_atomic)
            return atomic_call(x, c, *record, result);
        auto args = cc::vector<value>();
        if (auto const f = eval_all(c.arguments, args); !f.is_normal())
            return f;

        result.type = x.type;
        result.leaves.clear();
        auto const* const record = m.builtin_function(c.intrinsic);
        if (record == nullptr || record->evaluate == nullptr)
            return type_error("a call of something that is no builtin function");
        // EVAL-95: the emulated trace reads the pool and the roots of the run's inputs, which no evaluator holds
        if (record->takes_acceleration_index || record->name == "acceleration_pool_load")
            return acceleration_read(*record, args, result);

        // Every argument is the scalars its parameter type says, which is all an evaluator relies on.
        auto in = cc::vector<scalar>();
        auto is_typed = args.size() == record->parameters.size();
        for (auto k = isize(0); is_typed && k < args.size(); ++k)
        {
            // A resource parameter names no builtin type, and a resource is a value of no scalars.
            auto const type = m.builtins->find_type(record->parameters[k]);
            auto const leaf_count = is_valid(type) ? m.builtins->at(type).leaf_count : 0;
            is_typed = args[k].leaves.size() == leaf_count;
            for (auto const& leaf : args[k].leaves)
                is_typed = is_typed && leaf.kind == m.builtins->at(type).leaf_kind;
            in.push_back_range(args[k].leaves);
        }
        if (is_typed && record->undefined_when != nullptr)
            if (auto const why = record->undefined_when(in); !why.empty())
                return fail(run_status::program_error, cc::format("{}, in a call of '{}'", why, record->name));
        if (is_typed)
            record->evaluate(in, result.leaves);

        auto is_result_typed = is_valid(record->result) || result.leaves.empty();
        if (is_valid(record->result))
        {
            auto const& returned = m.builtins->at(record->result);
            is_result_typed = result.leaves.size() == returned.leaf_count;
            for (auto const& leaf : result.leaves)
                is_result_typed = is_result_typed && leaf.kind == returned.leaf_kind;
        }
        if (!is_typed || !is_result_typed || result.leaves.size() != leaf_count_of(m, x.type))
            return type_error(cc::format("a call of '{}' with arguments or a result of the wrong type", record->name));
        // The one way to see WHEN a call with an effect ran: its value joins the trace where the call happened.
        if (!c.is_pure)
            out.trace.push_back(result);
        return {};
    }

    /// `acceleration_root(world, k)`, whose `k` flatten appended, or `acceleration_pool_load(unit)`.
    flow acceleration_read(builtins::function_record const& record, cc::span<value const> args, value& result)
    {
        auto const is_root = record.takes_acceleration_index;
        auto const* const last = args.empty() ? nullptr : &args[args.size() - 1];
        auto const kind = is_root ? value_kind::scalar_int : value_kind::scalar_uint;
        if (last == nullptr || last->leaves.size() != 1 || last->leaves[0].kind != kind)
            return type_error(cc::format("a call of '{}' with arguments of the wrong type", record.name));
        if (is_root)
        {
            auto const k = isize(last->leaves[0].as_int());
            result.leaves.push_back(
                scalar::of_uint(k >= 0 && k < inputs.acceleration_roots.size() ? inputs.acceleration_roots[k] : 0));
            return {};
        }

        auto const unit = isize(last->leaves[0].as_uint());
        auto const& pool = inputs.acceleration_pool;
        if (!pool.empty() && (unit + 1) * 16 > pool.size())
            return fail(
                run_status::program_error,
                cc::format("the unit {} is past the end of an acceleration pool of {} units", unit, pool.size() / 16));
        for (auto i = 0; i < 4; ++i)
        {
            auto word = u32(0);
            if (!pool.empty())
                for (auto b = 0; b < 4; ++b)
                    word |= u32(pool[unit * 16 + i * 4 + b]) << (8 * b);
            result.leaves.push_back(scalar::of_uint(word));
        }
        return {};
    }

    /// The element `b` names: evaluates the index, then finds the buffer in `out.buffers` and the element's first scalar.
    /// A position rather than a pointer, since whatever runs next may be the value of a store.
    flow locate(flat_buffer_element const& b, type_id element_type, isize& buffer, isize& offset)
    {
        auto index = value();
        if (auto const f = eval(b.index, index); !f.is_normal())
            return f;
        if (index.leaves.size() != 1 || index.leaves[0].kind != value_kind::scalar_int)
            return type_error("a buffer index that is no int");
        if (!is_known(e, b.buffer))
            return type_error("an expression id that names nothing");
        auto const* const named = e.at(b.buffer).node.try_as<flat_binding_member>();
        if (named == nullptr)
            return type_error("a buffer that no binding member names");

        buffer = -1;
        for (auto i = isize(0); i < out.buffers.size(); ++i)
            if (out.buffers[i].binding == named->binding && out.buffers[i].member == named->member)
                buffer = i;
        if (buffer < 0)
            return type_error("a buffer the inputs do not hold");

        auto const count = leaf_count_of(m, element_type);
        if (count <= 0 || out.buffers[buffer].leaves.size() % count != 0)
            return type_error(cc::format("a buffer of {} scalars, which holds no whole number of its elements",
                                         out.buffers[buffer].leaves.size()));
        // EVAL-90: no target agrees on an index past the end, so a correct program never has one
        auto const at = isize(index.leaves[0].as_int());
        auto const length = out.buffers[buffer].leaves.size() / count;
        if (at < 0 || at >= length)
            return fail(run_status::program_error,
                        cc::format("the index {} is out of bounds of a buffer of {} elements", at, length));
        offset = at * count;
        return {};
    }

    flow eval_bool(flat_expr_id id, bool& result)
    {
        auto v = value();
        if (auto const f = eval(id, v); !f.is_normal())
            return f;
        if (v.leaves.size() != 1 || v.leaves[0].kind != value_kind::boolean)
            return type_error("a condition or a logical operand that is no bool");
        result = v.leaves[0].as_bool();
        return {};
    }

    flow eval(flat_expr_id id, value& result)
    {
        if (!is_known(e, id))
            return type_error("an expression id that names nothing");
        if (depth > k_max_depth)
            return type_error("an expression nested beyond any program");
        if (!burn())
            return {.kind = flow_kind::failed};
        ++depth;
        auto const f = is_in_workgroup(id) ? read_workgroup(id, result) : eval_node(e.at(id), result);
        --depth;
        return f;
    }

    flow eval_node(flat_expr const& x, value& result)
    {
        result.type = x.type;
        result.leaves.clear();

        if (x.node.is<flat_invalid>())
            return type_error("an unfilled expression");
        if (auto const* const l = x.node.try_as<flat_literal>())
        {
            result.leaves.push_back(scalar::of(f32(l->value)));
            return {};
        }
        if (auto const* const l = x.node.try_as<flat_int_literal>())
        {
            result.leaves.push_back(l->is_unsigned ? scalar::of_uint(u32(l->value)) : scalar::of(l->value));
            return {};
        }
        if (auto const* const l = x.node.try_as<flat_bool_literal>())
        {
            result.leaves.push_back(scalar::of(l->value));
            return {};
        }
        if (auto const* const v = x.node.try_as<flat_enum_value>())
        {
            auto const cases = m.at(m.at(x.type).cases);
            if (v->case_index < 0 || v->case_index >= cases.size())
                return type_error("an enum value whose case its type does not have");
            result.leaves.push_back(scalar::of(cases[v->case_index].value));
            return {};
        }
        if (auto const* const ref = x.node.try_as<flat_local_ref>())
        {
            if (!is_known(e, ref->local))
                return type_error("a local id that names nothing");
            // an undefined() is handed on like any value, and what reads it reads zeroes
            if (!is_set[index_of(ref->local)] && !e.at(ref->local).is_undefined)
                return fail(run_status::uninitialized_read, e.at(ref->local).name);
            result = locals[index_of(ref->local)];
            return {};
        }
        if (auto const* const b = x.node.try_as<flat_binding_member>())
        {
            for (auto k = isize(0); k < e.bindings.size() && k < inputs.bindings.size(); ++k)
            {
                if (e.bindings[k] != b->binding)
                    continue;
                auto const& info = m.bindings[m.at(b->binding).info];
                auto offset = isize(0);
                auto const members = m.at(info.members);
                if (b->member < 0 || b->member >= members.size())
                    break;
                for (auto i = 0; i < b->member; ++i)
                    offset += leaf_count_of(m, members[i].type);
                auto const count = leaf_count_of(m, members[b->member].type);
                if (offset + count > inputs.bindings[k].leaves.size())
                    break;
                result.leaves.push_back_range(
                    cc::span<scalar const>(inputs.bindings[k].leaves).subspan({.offset = offset, .size = count}));
                return {};
            }
            return type_error("a binding member the inputs do not hold");
        }
        if (auto const* const element = x.node.try_as<flat_buffer_element>())
        {
            auto buffer = isize(-1);
            auto offset = isize(0);
            if (auto const f = locate(*element, x.type, buffer, offset); !f.is_normal())
                return f;
            result.type = x.type;
            result.leaves.clear();
            result.leaves.push_back_range(cc::span<scalar const>(out.buffers[buffer].leaves)
                                              .subspan({.offset = offset, .size = leaf_count_of(m, x.type)}));
            return {};
        }
        if (auto const* const element = x.node.try_as<flat_element>())
        {
            auto object = value();
            if (auto const f = eval(element->object, object); !f.is_normal())
                return f;
            auto index = value();
            if (auto const f = eval(element->index, index); !f.is_normal())
                return f;
            auto offset = isize(0);
            auto count = isize(0);
            if (auto const f = element_offset(e.at(element->object).type, index, offset, count); !f.is_normal())
                return f;
            if (offset + count > object.leaves.size())
                return type_error("a value with fewer scalars than its type");
            result.type = x.type;
            result.leaves.clear();
            result.leaves.push_back_range(
                cc::span<scalar const>(object.leaves).subspan({.offset = offset, .size = count}));
            return {};
        }
        if (auto const* const member = x.node.try_as<flat_member>())
        {
            auto object = value();
            if (auto const f = eval(member->object, object); !f.is_normal())
                return f;
            auto const type = x.type;
            if (auto const f = slice_member(object, member->member, result); !f.is_normal())
                return f;
            result.type = type;
            return {};
        }
        if (auto const* const construct = x.node.try_as<flat_construct>())
        {
            auto args = cc::vector<value>();
            if (auto const f = eval_all(construct->arguments, args); !f.is_normal())
                return f;
            for (auto const& a : args)
                result.leaves.push_back_range(a.leaves);
            if (result.leaves.size() != leaf_count_of(m, x.type))
                return type_error(
                    cc::format("a construction of '{}' from the wrong number of scalars", m.name_of(x.type)));
            return {};
        }
        if (auto const* const c = x.node.try_as<flat_call>())
            return call(x, *c, result);
        // the interpreter has no device, so it runs what a device without the construct would: the emulated form
        if (auto const* const both = x.node.try_as<flat_by_target>())
            return eval(both->emulated, result);
        if (auto const* const n = x.node.try_as<flat_not>())
        {
            auto operand = false;
            if (auto const f = eval_bool(n->operand, operand); !f.is_normal())
                return f;
            result.leaves.push_back(scalar::of(!operand));
            return {};
        }
        auto const* const a = x.node.try_as<flat_and>();
        auto const* const o = x.node.try_as<flat_or>();
        if (a != nullptr || o != nullptr)
        {
            auto lhs = false;
            if (auto const f = eval_bool(a != nullptr ? a->lhs : o->lhs, lhs); !f.is_normal())
                return f;
            auto rhs = lhs;
            // the right operand runs only when the left one has not decided the value
            if (lhs == (a != nullptr))
                if (auto const f = eval_bool(a != nullptr ? a->rhs : o->rhs, rhs); !f.is_normal())
                    return f;
            result.leaves.push_back(scalar::of(rhs));
            return {};
        }
        if (auto const* const block = x.node.try_as<flat_block>())
        {
            auto const f = run_body(block->body);
            // A void block has its one value however it ends, which is no scalars at all.
            auto const is_void = leaf_count_of(m, x.type) == 0;
            if (f.kind == flow_kind::leave && f.label == block->label)
            {
                if (carried.leaves.empty() && !is_void)
                    return type_error("a block expression left without a value");
                result.leaves = cc::move(carried.leaves);
                carried = {};
                return {};
            }
            if (f.is_normal())
                return is_void ? flow{} : fail(run_status::fell_off_the_end, "a block expression");
            return f;
        }
        return type_error("an expression of a kind the machine does not know");
    }

    // ---- statements -------------------------------------------------------------------------------------------------

    flow store(local_id local, value v)
    {
        if (!is_known(e, local))
            return type_error("a local id that names nothing");
        v.type = e.at(local).type;
        locals[index_of(local)] = cc::move(v);
        is_set[index_of(local)] = true;
        return {};
    }

    /// Where a place lies in its local: evaluates its array indices, the one nearest the local first (EVAL-14).
    /// The cell of workgroup memory `b` names, which starts as the run's first touch of it.
    isize cell_of(flat_binding_member const& b)
    {
        for (auto i = isize(0); i < workgroup.size(); ++i)
            if (workgroup[i].binding == b.binding && workgroup[i].member == b.member)
                return i;
        auto const type = m.at(m.bindings[m.at(b.binding).info].members)[b.member].type;
        auto cell = workgroup_cell{.binding = b.binding, .member = b.member, .memory = zero_value(m, type)};
        cell.is_written.resize_to_filled(cell.memory.leaves.size(), false);
        workgroup.push_back(cc::move(cell));
        return workgroup.size() - 1;
    }

    /// True for a member or an element of workgroup memory, at any depth.
    [[nodiscard]] bool is_in_workgroup(flat_expr_id id) const
    {
        for (auto i = 0; i < k_max_depth && is_known(e, id); ++i)
        {
            auto const& node = e.at(id).node;
            if (auto const* const b = node.try_as<flat_binding_member>())
                return b->is_workgroup;
            if (auto const* const element = node.try_as<flat_element>())
                id = element->object;
            else if (auto const* const member = node.try_as<flat_member>())
                id = member->object;
            else
                return false;
        }
        return false;
    }

    /// A read of workgroup memory, of what the run stored there and nothing else (EVAL-92).
    flow read_workgroup(flat_expr_id id, value& result)
    {
        auto where = place_ref();
        auto offset = isize(0);
        auto type = type_id::none;
        if (auto const f = locate_place(id, where, offset, type); !f.is_normal())
            return f;
        auto const& cell = workgroup[where.cell];
        auto const count = leaf_count_of(m, type);
        for (auto i = isize(0); i < count; ++i)
            if (!cell.is_written[offset + i])
                return fail(run_status::program_error,
                            cc::format("a read of {}.{} where nothing was stored", m.at(cell.binding).name,
                                       m.at(m.bindings[m.at(cell.binding).info].members)[cell.member].name));
        result.type = type;
        result.leaves.clear();
        result.leaves.push_back_range(
            cc::span<scalar const>(cell.memory.leaves).subspan({.offset = offset, .size = count}));
        return {};
    }

    /// What a place is part of: a mutable local, or a cell of workgroup memory.
    struct place_ref
    {
        local_id local = local_id::none;
        isize cell = -1;
    };

    flow locate_place(flat_expr_id place, place_ref& where, isize& offset, type_id& type)
    {
        // the steps from the place down to its local, innermost first: a member, or an element's index expression
        struct step
        {
            i32 member = -1;
            flat_expr_id index = flat_expr_id::none;
        };
        auto path = cc::vector<step>();
        auto id = place;
        where = {};
        for (auto i = 0; i < k_max_depth && is_known(e, id); ++i)
        {
            auto const& x = e.at(id);
            if (auto const* const ref = x.node.try_as<flat_local_ref>())
            {
                where.local = ref->local;
                break;
            }
            if (auto const* const b = x.node.try_as<flat_binding_member>(); b != nullptr && b->is_workgroup)
            {
                where.cell = cell_of(*b);
                break;
            }
            if (auto const* const element = x.node.try_as<flat_element>())
            {
                path.push_back({.index = element->index});
                id = element->object;
                continue;
            }
            auto const* const member = x.node.try_as<flat_member>();
            if (member == nullptr)
                break;
            path.push_back({.member = member->member});
            id = member->object;
        }
        if (where.cell < 0 && (!is_known(e, where.local) || !e.at(where.local).is_mut))
            return type_error("an assignment to what is no mutable local");

        type = where.cell >= 0 ? workgroup[where.cell].memory.type : e.at(where.local).type;
        offset = 0;
        for (auto k = path.size() - 1; k >= 0; --k)
        {
            if (is_valid(path[k].index))
            {
                auto index = value();
                if (auto const f = eval(path[k].index, index); !f.is_normal())
                    return f;
                auto at = isize(0);
                auto count = isize(0);
                if (auto const f = element_offset(type, index, at, count); !f.is_normal())
                    return f;
                offset += at;
                type = m.at(type).element;
                continue;
            }
            auto const members = members_of(m, type);
            if (path[k].member < 0 || path[k].member >= members.size())
                return type_error("an assignment to a member its type does not have");
            for (auto i = 0; i < path[k].member; ++i)
                offset += leaf_count_of(m, members[i].type);
            type = members[path[k].member].type;
        }
        return {};
    }

    flow assign(flat_expr_id place, value const& v)
    {
        auto where = place_ref();
        auto offset = isize(0);
        auto type = type_id::none;
        if (auto const f = locate_place(place, where, offset, type); !f.is_normal())
            return f;
        return write(where, offset, type, v);
    }

    flow write(place_ref const& where, isize offset, type_id type, value const& v)
    {
        auto& target = where.cell >= 0 ? workgroup[where.cell].memory : locals[index_of(where.local)];
        auto const count = leaf_count_of(m, type);
        if (v.leaves.size() != count || offset + count > target.leaves.size())
            return type_error("an assignment of a value of the wrong size");
        for (auto i = isize(0); i < count; ++i)
            target.leaves[offset + i] = v.leaves[i];
        if (where.cell >= 0)
            for (auto i = isize(0); i < count; ++i)
                workgroup[where.cell].is_written[offset + i] = true;
        else
            is_set[index_of(where.local)] = true;
        return {};
    }

    /// EVAL-14: the index first, then the value, then the store.
    flow store_element(flat_buffer_element const& element, type_id element_type, flat_expr_id stored)
    {
        auto buffer = isize(-1);
        auto offset = isize(0);
        if (auto const f = locate(element, element_type, buffer, offset); !f.is_normal())
            return f;
        auto v = value();
        if (auto const f = eval(stored, v); !f.is_normal())
            return f;
        auto const count = leaf_count_of(m, element_type);
        if (v.leaves.size() != count)
            return type_error("a store of a value of the wrong size");
        for (auto i = isize(0); i < count; ++i)
            out.buffers[buffer].leaves[offset + i] = v.leaves[i];
        is_stored[buffer] = true;
        return {};
    }

    flow run_body(ast::range_of<flat_stmt_id> range)
    {
        if (!is_known(e, range))
            return type_error("a statement list that reaches outside the tree");
        if (depth > k_max_depth)
            return type_error("a statement nested beyond any program");
        ++depth;
        auto result = flow();
        for (auto const id : e.at(range))
        {
            result = run(id);
            if (!result.is_normal())
                break;
        }
        --depth;
        return result;
    }

    /// EVAL-65 to EVAL-70: the scrutinee once, then the arms in order, and the patterns only where they are reached.
    /// `captures_break` is the `switch`, which a `break` directly inside ends the way one inside a `once` ends that.
    flow run_arms(flat_expr_id scrutinee,
                  ast::range_of<flat_arm> arms,
                  ast::range_of<flat_stmt_id> default_body,
                  bool captures_break)
    {
        auto chosen = value();
        if (auto const f = eval(scrutinee, chosen); !f.is_normal())
            return f;
        if (!is_known(e, arms))
            return type_error("a case whose arms reach outside the tree");

        auto selected = default_body;
        auto is_selected = false;
        for (auto const& arm : e.at(arms))
        {
            if (is_selected)
                break;
            if (!is_known(e, arm.patterns))
                return type_error("a case arm whose patterns reach outside the tree");
            for (auto const id : e.at(arm.patterns))
            {
                auto pattern = value();
                if (auto const f = eval(id, pattern); !f.is_normal())
                    return f;
                if (pattern.leaves.size() != chosen.leaves.size())
                    return type_error("a case pattern of another type than its scrutinee");
                auto is_equal = true;
                for (auto i = isize(0); i < pattern.leaves.size(); ++i)
                    is_equal = is_equal && pattern.leaves[i] == chosen.leaves[i];
                if (is_equal)
                {
                    selected = arm.body;
                    is_selected = true;
                    break;
                }
            }
        }

        auto const f = run_body(selected);
        return captures_break && f.kind == flow_kind::break_ ? flow() : f;
    }

    /// What one iteration's end means for its loop: go on, stop, or hand the exit further out.
    enum class after : u8
    {
        next,
        stop,
        propagate,
    };

    after after_iteration(flow const& f, label_id label) const
    {
        if (f.is_normal() || (f.kind == flow_kind::continue_ && f.label == label))
            return after::next;
        if (f.kind == flow_kind::break_ || (f.kind == flow_kind::leave && f.label == label))
            return after::stop;
        return after::propagate;
    }

    /// EVAL-75: the body leaves every node's value in its `var`, and a false condition is recorded with all of them.
    flow check(flat_check const& k)
    {
        if (!limits.run_checks)
            return {};
        if (k.site < 0 || k.site >= e.check_sites.size())
            return type_error("a check whose site the tree does not have");
        if (auto const f = run_body(k.body); !f.is_normal())
            return f;

        auto const& site = e.check_sites[k.site];
        auto const nodes = e.at(site.nodes);
        if (nodes.empty() || !is_known(e, nodes[0].value) || !is_set[index_of(nodes[0].value)])
            return type_error("a check whose condition has no value");
        auto const& condition = locals[index_of(nodes[0].value)];
        if (condition.leaves.size() != 1 || condition.leaves[0].kind != value_kind::boolean)
            return type_error("a check whose condition is no bool");
        ++out.checks_run;
        if (site.stops)
            ++out.asserts_run;
        auto& tally = out.sites[k.site];
        if (condition.leaves[0].as_bool())
        {
            ++tally.passed;
            return {};
        }
        ++tally.failed;

        if (out.failures.size() < limits.max_failures)
        {
            auto failure = check_failure{.site = k.site};
            for (auto const& node : nodes)
            {
                auto const known = is_known(e, node.value) && is_set[index_of(node.value)];
                failure.values.push_back(known ? locals[index_of(node.value)] : value{.type = e.at(node.value).type});
                failure.is_evaluated.push_back(known);
            }
            if (is_known(e, site.loop_variables))
                for (auto const id : e.at(site.loop_variables))
                {
                    auto v = value();
                    if (auto const f = eval(id, v); !f.is_normal())
                        return f;
                    failure.loop_values.push_back(cc::move(v));
                }
            out.failures.push_back(cc::move(failure));
        }
        else
            ++out.failures_dropped;
        return site.stops ? fail(run_status::assertion_failed, "an assert was false") : flow();
    }

    flow run(flat_stmt_id id)
    {
        if (!is_known(e, id))
            return type_error("a statement id that names nothing");
        if (!burn())
            return {.kind = flow_kind::failed};
        auto const& s = e.at(id);

        if (auto const* const let = s.node.try_as<flat_let>())
        {
            auto v = value();
            if (auto const f = eval(let->value, v); !f.is_normal())
                return f;
            return store(let->local, cc::move(v));
        }
        if (auto const* const var = s.node.try_as<flat_var>())
        {
            if (!is_known(e, var->local))
                return type_error("a local id that names nothing");
            if (!is_valid(var->value))
            {
                locals[index_of(var->local)] = zero_value(m, e.at(var->local).type);
                is_set[index_of(var->local)] = false;
                return {};
            }
            auto v = value();
            if (auto const f = eval(var->value, v); !f.is_normal())
                return f;
            return store(var->local, cc::move(v));
        }
        if (auto const* const a = s.node.try_as<flat_assign>())
        {
            auto const* const element
                = is_known(e, a->place) ? e.at(a->place).node.try_as<flat_buffer_element>() : nullptr;
            if (element != nullptr)
                return store_element(*element, e.at(a->place).type, a->value);
            // EVAL-14: the place's indices, then the value
            auto where = place_ref();
            auto offset = isize(0);
            auto type = type_id::none;
            if (auto const f = locate_place(a->place, where, offset, type); !f.is_normal())
                return f;
            auto v = value();
            if (auto const f = eval(a->value, v); !f.is_normal())
                return f;
            return write(where, offset, type, v);
        }
        if (auto const* const p = s.node.try_as<flat_print>())
        {
            auto v = value();
            if (auto const f = eval(p->value, v); !f.is_normal())
                return f;
            out.trace.push_back(cc::move(v));
            return {};
        }
        if (auto const* const dropped = s.node.try_as<flat_eval>())
        {
            auto v = value();
            return eval(dropped->value, v);
        }
        if (auto const* const branch = s.node.try_as<flat_if>())
        {
            auto condition = false;
            if (auto const f = eval_bool(branch->condition, condition); !f.is_normal())
                return f;
            return run_body(condition ? branch->then_body : branch->else_body);
        }
        if (auto const* const block = s.node.try_as<flat_block>())
        {
            auto const f = run_body(block->body);
            if (f.kind == flow_kind::leave && f.label == block->label)
            {
                carried = {};
                return {};
            }
            return f;
        }
        if (auto const* const leave = s.node.try_as<flat_leave>())
        {
            auto v = value();
            if (is_valid(leave->value))
                if (auto const f = eval(leave->value, v); !f.is_normal())
                    return f;
            carried = cc::move(v);
            return {.kind = flow_kind::leave, .label = leave->target};
        }
        if (auto const* const loop = s.node.try_as<flat_loop>())
        {
            while (burn())
            {
                auto const f = run_body(loop->body);
                auto const next = after_iteration(f, loop->label);
                if (next == after::stop)
                    return {};
                if (next == after::propagate)
                    return f;
            }
            return {.kind = flow_kind::failed};
        }
        if (auto const* const w = s.node.try_as<flat_while>())
        {
            while (burn())
            {
                auto condition = false;
                if (auto const f = eval_bool(w->condition, condition); !f.is_normal())
                    return f;
                if (!condition)
                    return {};
                auto const f = run_body(w->body);
                auto const next = after_iteration(f, w->label);
                if (next == after::stop)
                    return {};
                if (next == after::propagate)
                    return f;
            }
            return {.kind = flow_kind::failed};
        }
        if (auto const* const loop = s.node.try_as<flat_for>())
        {
            auto first = value();
            auto end = value();
            if (auto const f = eval(loop->first, first); !f.is_normal())
                return f;
            if (auto const f = eval(loop->end, end); !f.is_normal())
                return f;
            auto const is_int
                = [](value const& v) { return v.leaves.size() == 1 && v.leaves[0].kind == value_kind::scalar_int; };
            if (!is_int(first) || !is_int(end))
                return type_error("a `for` whose bounds are no int");
            for (auto index = first.leaves[0].as_int(); index < end.leaves[0].as_int(); ++index)
            {
                if (!burn())
                    return {.kind = flow_kind::failed};
                auto v = value();
                v.leaves.push_back(scalar::of(index));
                if (auto const f = store(loop->index, cc::move(v)); !f.is_normal())
                    return f;
                auto const f = run_body(loop->body);
                auto const next = after_iteration(f, loop->label);
                if (next == after::stop)
                    return {};
                if (next == after::propagate)
                    return f;
            }
            return {};
        }
        if (auto const* const c = s.node.try_as<flat_continue>())
            return {.kind = flow_kind::continue_, .label = c->target};
        if (s.node.is<flat_discard>())
            return fail(run_status::discarded, "");
        if (auto const* const once = s.node.try_as<flat_once>())
        {
            auto const f = run_body(once->body);
            return f.kind == flow_kind::break_ ? flow() : f;
        }
        if (s.node.is<flat_break>())
            return {.kind = flow_kind::break_};
        if (auto const* const c = s.node.try_as<flat_case>())
            return run_arms(c->scrutinee, c->arms, c->default_body, false);
        if (auto const* const sw = s.node.try_as<flat_switch>())
            return run_arms(sw->scrutinee, sw->arms, sw->default_body, true);
        if (auto const* const r = s.node.try_as<flat_return>())
        {
            auto v = value();
            if (is_valid(r->value))
                if (auto const f = eval(r->value, v); !f.is_normal())
                    return f;
            carried = cc::move(v);
            return {.kind = flow_kind::return_};
        }
        if (auto const* const k = s.node.try_as<flat_check>())
            return check(*k);
        return type_error("a statement of a kind the machine does not know");
    }
};
} // namespace

scalar sgl::check::scalar::of(f32 v)
{
    return {.kind = value_kind::scalar_float, .bits = cc::bit_cast<u32>(v)};
}

scalar sgl::check::scalar::of(i32 v)
{
    return {.kind = value_kind::scalar_int, .bits = u32(v)};
}

scalar sgl::check::scalar::of(bool v)
{
    return {.kind = value_kind::boolean, .bits = v ? 1u : 0u};
}

sgl::f32 sgl::check::scalar::as_float() const
{
    return cc::bit_cast<f32>(bits);
}

cc::string_view sgl::check::to_string(run_status s)
{
    switch (s)
    {
    case run_status::ok:
        return "ok";
    case run_status::out_of_fuel:
        return "out-of-fuel";
    case run_status::fell_off_the_end:
        return "fell-off-the-end";
    case run_status::type_error:
        return "type-error";
    case run_status::uninitialized_read:
        return "uninitialized-read";
    case run_status::program_error:
        return "program-error";
    case run_status::discarded:
        return "discarded";
    case run_status::assertion_failed:
        return "assertion-failed";
    case run_status::stopped:
        return "stopped";
    }
    return "";
}

sgl::isize sgl::check::leaf_count_of(checked_module const& m, type_id type)
{
    auto leaves = cc::vector<scalar>();
    append_zero(m, type, leaves, 0);
    return leaves.size();
}

value sgl::check::zero_value(checked_module const& m, type_id type)
{
    auto result = value{.type = type};
    append_zero(m, type, result.leaves, 0);
    return result;
}

outcome sgl::check::interpret(checked_module const& m,
                              flat_entry_point const& e,
                              run_inputs const& inputs,
                              run_limits const& limits)
{
    auto run = machine{.m = m, .e = e, .inputs = inputs, .limits = limits, .fuel = limits.fuel};
    run.out.buffers = inputs.buffers;
    run.is_stored.resize_to_filled(inputs.buffers.size(), false);
    run.out.sites.resize_to_defaulted(e.check_sites.size());
    run.locals.resize_to_defaulted(e.locals.size());
    run.is_set.resize_to_filled(e.locals.size(), false);
    // A test has no parameter, and its first local is one of its own; an entry point may have stage inputs alone.
    if (is_valid(e.input) && !e.locals.empty() && e.locals[0].kind == local_kind::parameter)
    {
        run.locals[0] = inputs.parameter;
        run.is_set[0] = true;
    }
    // A stage input the caller did not state is zero, which is what one invocation of a draw or a dispatch would read.
    for (auto i = isize(0); i < e.stage_inputs.size(); ++i)
    {
        auto const local = index_of(e.stage_inputs[i].local);
        run.locals[local] = i < inputs.stage_inputs.size() ? inputs.stage_inputs[i] : zero_value(m, e.locals[local].type);
        run.is_set[local] = true;
    }

    auto const f = run.run_body(e.body);
    auto const is_returned
        = f.kind == flow_kind::return_ || (f.kind == flow_kind::leave && is_valid(e.root) && f.label == e.root);
    if (is_returned)
    {
        run.out.result = cc::move(run.carried);
        run.out.result.type = e.result;
    }
    else if (f.is_normal())
    {
        // A test and a compute entry point return void, whose one value a run has however it ends.
        if (e.result != checked_module::void_type)
            run.fail(run_status::fell_off_the_end, "the function");
    }
    else if (f.kind != flow_kind::failed)
        run.type_error("an exit that nothing encloses");

    auto stored = cc::vector<buffer_contents>();
    for (auto i = isize(0); i < run.out.buffers.size(); ++i)
        if (run.is_stored[i])
            stored.push_back(cc::move(run.out.buffers[i]));
    run.out.buffers = cc::move(stored);
    return cc::move(run.out);
}

namespace
{
/// Fills the scalars of `leaves`, whose kinds they keep, from `bytes` as `member_data` lays them out.
void read_leaves(cc::span<byte const> bytes, cc::span<scalar> leaves)
{
    for (auto i = isize(0); i < leaves.size(); ++i)
    {
        auto word = u32(0);
        for (auto b = 0; b < 4; ++b)
            word |= u32(bytes[i * 4 + b]) << (8 * b);
        leaves[i].bits = leaves[i].kind == value_kind::boolean ? u32(word != 0) : word;
    }
}

member_data const* find_member(driver_bindings const& bound, cc::string_view binding, cc::string_view member)
{
    for (auto const& g : bound.groups)
        if (g.name == binding)
            for (auto const& d : g.members)
                if (d.name == member)
                    return &d;
    return nullptr;
}
} // namespace

cc::result<run_inputs> sgl::check::resolve_inputs(checked_module const& m,
                                                  flat_entry_point const& e,
                                                  driver_bindings const& bound)
{
    auto inputs = run_inputs{.acceleration_pool = bound.acceleration_pool};
    for (auto const binding : e.bindings)
    {
        auto const& name = m.at(binding).name;
        auto const members = m.at(m.bindings[m.at(binding).info].members);
        for (auto const& g : bound.groups)
            if (g.name == name)
                for (auto const& d : g.members)
                {
                    auto is_known = false;
                    for (auto const& member : members)
                        is_known = is_known || member.name == d.name;
                    if (!is_known)
                        return cc::error(cc::format("the binding {} has no member {}", name, d.name));
                }

        auto whole = value();
        for (auto i = isize(0); i < members.size(); ++i)
        {
            auto const& member = members[i];
            auto const* const d = find_member(bound, name, member.name);
            auto const bytes = d == nullptr              ? cc::span<byte const>()
                             : !d->mutable_bytes.empty() ? cc::span<byte const>(d->mutable_bytes.span())
                                                         : d->bytes.span();
            auto const& type = m.at(member.type);
            if (type.kind == type_kind::acceleration_structure)
            {
                inputs.acceleration_roots.push_back(d == nullptr ? 0 : d->acceleration_root);
                continue;
            }
            if (type.kind == type_kind::buffer)
            {
                auto const element = zero_value(m, type.element);
                auto const stride = element.leaves.size() * 4;
                if (stride == 0 || bytes.size() % stride != 0)
                    return cc::error(cc::format("{}.{} is {} bytes, which is no whole number of {}-byte elements", name,
                                                member.name, bytes.size(), stride));
                auto contents = buffer_contents{.binding = binding, .member = i32(i)};
                for (auto k = isize(0); k < bytes.size() / stride; ++k)
                    contents.leaves.push_back_range(element.leaves);
                read_leaves(bytes, contents.leaves);
                inputs.buffers.push_back(cc::move(contents));
                continue;
            }

            auto v = zero_value(m, member.type);
            if (d != nullptr && bytes.size() != v.leaves.size() * 4)
                return cc::error(cc::format("{}.{} is {} bytes, and its value is {}", name, member.name, bytes.size(),
                                            v.leaves.size() * 4));
            if (d != nullptr)
                read_leaves(bytes, v.leaves);
            whole.leaves.push_back_range(v.leaves);
        }
        inputs.bindings.push_back(cc::move(whole));
    }
    return inputs;
}

void sgl::check::write_back(checked_module const& m, outcome const& o, driver_bindings const& bound)
{
    for (auto const& b : o.buffers)
    {
        auto const members = m.at(m.bindings[m.at(b.binding).info].members);
        if (b.member < 0 || b.member >= members.size())
            continue;
        auto const* const d = find_member(bound, m.at(b.binding).name, members[b.member].name);
        if (d == nullptr || d->mutable_bytes.empty())
            continue;
        CC_ASSERT(d->mutable_bytes.size() == b.leaves.size() * 4, "a run never changes the size of a buffer");
        for (auto i = isize(0); i < b.leaves.size(); ++i)
            for (auto k = 0; k < 4; ++k)
                d->mutable_bytes[i * 4 + k] = byte(u8(b.leaves[i].bits >> (8 * k)));
    }
}

bool sgl::check::is_constant(checked_module const& m, flat_entry_point const& e, flat_expr_id id)
{
    if (!is_known(e, id))
        return false;
    auto const& x = e.at(id);
    auto const all_constant = [&](ast::range_of<flat_expr_id> range)
    {
        if (!is_known(e, range))
            return false;
        for (auto const argument : e.at(range))
            if (!is_constant(m, e, argument))
                return false;
        return true;
    };
    if (x.node.is<flat_literal>() || x.node.is<flat_int_literal>() || x.node.is<flat_bool_literal>()
        || x.node.is<flat_enum_value>())
        return true;
    if (auto const* const construct = x.node.try_as<flat_construct>())
        return all_constant(construct->arguments);
    if (auto const* const member = x.node.try_as<flat_member>())
        return is_constant(m, e, member->object);
    if (auto const* const n = x.node.try_as<flat_not>())
        return is_constant(m, e, n->operand);
    if (auto const* const a = x.node.try_as<flat_and>())
        return is_constant(m, e, a->lhs) && is_constant(m, e, a->rhs);
    if (auto const* const o = x.node.try_as<flat_or>())
        return is_constant(m, e, o->lhs) && is_constant(m, e, o->rhs);
    if (auto const* const c = x.node.try_as<flat_call>())
    {
        auto const* const record = m.builtin_function(c->intrinsic);
        return c->is_pure && record != nullptr && record->evaluate != nullptr && !record->is_atomic
            && all_constant(c->arguments);
    }
    return false;
}

outcome sgl::check::evaluate_constant(checked_module const& m, flat_entry_point const& e, flat_expr_id id)
{
    auto const inputs = run_inputs();
    auto const limits = run_limits();
    auto run = machine{.m = m, .e = e, .inputs = inputs, .limits = limits, .fuel = limits.fuel};
    if (!is_constant(m, e, id))
    {
        run.type_error("an expression that is no constant");
        return cc::move(run.out);
    }
    auto result = value();
    if (run.eval(id, result).is_normal())
        run.out.result = cc::move(result);
    return cc::move(run.out);
}

cc::string sgl::check::dump(outcome const& o)
{
    auto const write = [](cc::string& out, value const& v)
    {
        for (auto const& leaf : v.leaves)
        {
            if (leaf.kind == value_kind::scalar_float)
                out.appendf(" {}", leaf.as_float());
            else if (leaf.kind == value_kind::scalar_int)
                out.appendf(" {}", leaf.as_int());
            else if (leaf.kind == value_kind::scalar_uint)
                out.appendf(" {}u", leaf.as_uint());
            else
                out.appendf(" {}", leaf.as_bool() ? "true" : "false");
        }
    };
    auto out = cc::string(to_string(o.status));
    if (!o.detail.empty())
        out.appendf(" ({})", o.detail);
    write(out, o.result);
    for (auto const& v : o.trace)
    {
        out += " | print";
        write(out, v);
    }
    for (auto const& b : o.buffers)
    {
        out += " | buffer";
        write(out, value{.leaves = b.leaves});
    }
    return out;
}
