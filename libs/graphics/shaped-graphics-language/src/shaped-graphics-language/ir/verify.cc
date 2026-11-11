#include "verify.hh"

#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics-language/ir/dominance.hh>
#include <shaped-graphics-language/ir/ir.hh>

#include <initializer_list>
#include <type_traits>

using namespace sgl;
using namespace sgl::ir;

namespace
{
constexpr u32 k_nowhere = 0xffffffffu;

/// What an operation's operands and result must look like; a row of `k_shapes` is what adds a case.
enum class shape : u8
{
    /// Checked by no family: the operation needs a result and nothing more is said here.
    any_result,
    real,
    integer,
    shift,
    /// Integers of the result's lanes, whose width may differ.
    bit_query,
    real_compare,
    real_test,
    integer_compare,
    logical,
    reduce,
    select,
    float_to_int,
    int_to_float,
    float_resize,
    int_resize,
    bitcast,
    dot,
    vector_times_scalar,
    matrix_times_scalar,
    matrix_times_vector,
    matrix_times_matrix,
    transpose,
    construct,
    extract,
    insert,
    shuffle,
    load,
    store,
    access_chain,
    texel_pointer,
    array_length,
    stage_load,
    stage_store,
    atomic,
    atomic_store,
    no_result,
    combine_sampler,
    image_access,
    image_write,
    phi,
    branch,
    branch_conditional,
    switch_,
    leaves,
};

/// Parallel to `op`, as `k_names` of ir.cc is.
constexpr shape k_shapes[] = {
    shape::real,                // fadd
    shape::real,                // fsub
    shape::real,                // fmul
    shape::real,                // fdiv
    shape::real,                // frem
    shape::real,                // fneg
    shape::integer,             // iadd
    shape::integer,             // isub
    shape::integer,             // imul
    shape::integer,             // sdiv
    shape::integer,             // udiv
    shape::integer,             // srem
    shape::integer,             // urem
    shape::integer,             // ineg
    shape::integer,             // bit_and
    shape::integer,             // bit_or
    shape::integer,             // bit_xor
    shape::integer,             // bit_not
    shape::integer,             // bit_reverse
    shape::bit_query,           // bit_count
    shape::shift,               // shift_left
    shape::shift,               // shift_right_logical
    shape::shift,               // shift_right_arithmetic
    shape::real_compare,        // feq
    shape::real_compare,        // fne
    shape::real_compare,        // flt
    shape::real_compare,        // fle
    shape::real_compare,        // fgt
    shape::real_compare,        // fge
    shape::real_test,           // is_nan
    shape::real_test,           // is_inf
    shape::integer_compare,     // ieq
    shape::integer_compare,     // ine
    shape::integer_compare,     // slt
    shape::integer_compare,     // sle
    shape::integer_compare,     // sgt
    shape::integer_compare,     // sge
    shape::integer_compare,     // ult
    shape::integer_compare,     // ule
    shape::integer_compare,     // ugt
    shape::integer_compare,     // uge
    shape::logical,             // logical_and
    shape::logical,             // logical_or
    shape::logical,             // logical_not
    shape::logical,             // logical_eq
    shape::logical,             // logical_ne
    shape::reduce,              // any
    shape::reduce,              // all
    shape::select,              // select
    shape::float_to_int,        // f_to_s
    shape::float_to_int,        // f_to_u
    shape::int_to_float,        // s_to_f
    shape::int_to_float,        // u_to_f
    shape::float_resize,        // f_resize
    shape::int_resize,          // s_resize
    shape::int_resize,          // u_resize
    shape::bitcast,             // bitcast
    shape::any_result,          // copy_logical
    shape::dot,                 // dot
    shape::vector_times_scalar, // vector_times_scalar
    shape::matrix_times_scalar, // matrix_times_scalar
    shape::matrix_times_vector, // matrix_times_vector
    shape::matrix_times_matrix, // matrix_times_matrix
    shape::transpose,           // transpose
    shape::any_result,          // math
    shape::real,                // derivative
    shape::construct,           // construct
    shape::extract,             // extract
    shape::insert,              // insert
    shape::shuffle,             // shuffle
    shape::load,                // load
    shape::store,               // store
    shape::access_chain,        // access_chain
    shape::texel_pointer,       // texel_pointer
    shape::array_length,        // array_length
    shape::stage_load,          // stage_load
    shape::stage_store,         // stage_store
    shape::atomic,              // atomic_load
    shape::atomic_store,        // atomic_store
    shape::atomic,              // atomic_exchange
    shape::atomic,              // atomic_compare_exchange
    shape::atomic,              // atomic_iadd
    shape::atomic,              // atomic_isub
    shape::atomic,              // atomic_smin
    shape::atomic,              // atomic_umin
    shape::atomic,              // atomic_smax
    shape::atomic,              // atomic_umax
    shape::atomic,              // atomic_and
    shape::atomic,              // atomic_or
    shape::atomic,              // atomic_xor
    shape::atomic,              // atomic_fadd
    shape::no_result,           // control_barrier
    shape::no_result,           // discard
    shape::no_result,           // emit_vertex
    shape::no_result,           // end_strip
    shape::no_result,           // set_mesh_counts
    shape::no_result,           // write_primitive
    shape::no_result,           // trace_ray
    shape::no_result,           // execute_callable
    shape::any_result,          // report_hit
    shape::any_result,          // hit_object_trace
    shape::any_result,          // trace_reorder_execute
    shape::no_result,           // hit_object_reorder
    shape::no_result,           // reorder_thread
    shape::no_result,           // hit_object_invoke
    shape::any_result,          // acceleration_from_address
    shape::any_result,          // ray_query_begin
    shape::no_result,           // ray_query_trace
    shape::any_result,          // ray_query_proceed
    shape::no_result,           // ray_query_commit_triangle
    shape::no_result,           // ray_query_commit_procedural
    shape::no_result,           // ray_query_abort
    shape::any_result,          // ray_query_read
    shape::any_result,          // hit_object_read
    shape::combine_sampler,     // combine_sampler
    shape::image_access,        // sample_lod
    shape::image_access,        // sample_implicit
    shape::image_access,        // sample_bias
    shape::image_access,        // sample_grad
    shape::image_access,        // sample_compare
    shape::image_access,        // sample_compare_zero
    shape::image_access,        // image_fetch
    shape::image_access,        // image_gather
    shape::image_access,        // image_gather_compare
    shape::image_access,        // image_read
    shape::image_write,         // image_write
    shape::image_access,        // image_size
    shape::image_access,        // image_levels
    shape::image_access,        // image_samples
    shape::any_result,          // subgroup
    shape::phi,                 // phi
    shape::branch,              // branch
    shape::branch_conditional,  // branch_conditional
    shape::switch_,             // switch_
    shape::leaves,              // dispatch_mesh
    shape::leaves,              // ignore_hit
    shape::leaves,              // accept_and_end_search
    shape::leaves,              // return_
    shape::leaves,              // unreachable
};
static_assert(sizeof(k_shapes) == sizeof(char[isize(op::unreachable) + 1]));

[[nodiscard]] constexpr bool is_terminator(op code)
{
    return code >= op::branch;
}

class verifier
{
public:
    verifier(program const& p, control_flow flow) : _p(p), _flow(flow) {}

    [[nodiscard]] cc::optional<cc::string> run()
    {
        if (_p.blocks.empty())
            return cc::string("a program with no block");
        for (auto const step : {&verifier::check_tables, &verifier::check_blocks, &verifier::check_edges,
                                &verifier::check_operations, &verifier::check_order, &verifier::check_structure})
        {
            (this->*step)();
            if (!_failure.empty())
                return _failure;
        }
        return {};
    }

private:
    template <class... Args>
    void fail(cc::format_string<std::type_identity_t<Args>...> what, Args&&... args)
    {
        if (_failure.empty())
            _failure = cc::format(what, cc::forward<Args>(args)...);
    }
    /// `what`, prefixed with where the walk stands.
    template <class... Args>
    void wrong(cc::format_string<std::type_identity_t<Args>...> what, Args&&... args)
    {
        if (!_failure.empty())
            return;
        auto const& i = _p.instructions[_at];
        _failure = cc::format("b{} %{} {}: ", _block, _at, op_name(i.code));
        _failure.appendf(what, cc::forward<Args>(args)...);
    }

    // ---- the tables

    [[nodiscard]] bool is_type(type_ref t) const { return is_valid(t) && isize(t) < _p.types.size(); }

    void check_tables()
    {
        for (auto const& t : _p.types)
        {
            auto const holds_element = t.kind == type_kind::vector || t.kind == type_kind::matrix
                                    || t.kind == type_kind::array || t.kind == type_kind::runtime_array
                                    || t.kind == type_kind::pointer || t.kind == type_kind::sampled_image;
            if (holds_element && !is_type(t.element))
                return fail("a {} with no element", int(t.kind));
            if (t.kind == type_kind::structure && isize(t.first_member) + isize(t.member_count) > _p.members.size())
                return fail("a structure past the members");
        }
        for (auto const& c : _p.constants)
            if (!is_type(c.type))
                return fail("a constant of no type");
        for (auto const& g : _p.globals)
            if (!is_type(g.type) || _p.at(g.type).kind != type_kind::pointer)
                return fail("a global that is no pointer");
        for (auto const& l : _p.locals)
            if (!is_type(l.type) || _p.at(l.type).kind != type_kind::pointer)
                return fail("a local that is no pointer");
        for (auto const& m : _p.members)
            if (is_type(m.type) && holds_hit_object(m.type))
                return fail("a member that holds a hit object, which nothing may move");
        for (auto const& l : _p.locals)
            if (is_type(l.type) && holds_hit_object(_p.at(l.type).element))
                return fail("a local that holds a hit object, which nothing may move");
        for (auto const& g : _p.globals)
            if (is_type(g.type) && holds_hit_object(_p.at(g.type).element))
                return fail("a global that holds a hit object, which nothing may move");
        for (auto const& e : _p.stage_elements)
        {
            if (!is_type(e.type))
                return fail("a stage element of no type");
            auto const kind = _p.at(e.type).kind;
            // a hit's transform is a matrix the target hands over whole, and nothing packs it
            if (e.system == built_in::object_to_world || e.system == built_in::world_to_object)
                continue;
            if (kind == type_kind::matrix || kind == type_kind::array || kind == type_kind::structure
                || kind == type_kind::pointer)
                return fail("a stage element of {}, which crosses an edge only as scalars and vectors",
                            type_name(_p, e.type));
        }
    }

    void check_blocks()
    {
        _block_of.resize_to_filled(_p.instructions.size(), k_nowhere);
        for (auto b = isize(0); b < _p.blocks.size(); ++b)
        {
            auto const& block = _p.blocks[b];
            if (isize(block.first) + isize(block.count) > _p.instructions.size())
                return fail("b{} runs past the instructions", b);
            for (auto i = block.first; i < block.first + block.count; ++i)
            {
                if (_block_of[i] != k_nowhere)
                    return fail("%{} stands in both b{} and b{}", i, _block_of[i], b);
                _block_of[i] = u32(b);
                _block = u32(b);
                _at = i;
                auto const code = _p.instructions[i].code;
                if (isize(code) > isize(op::unreachable))
                    return fail("%{} holds no operation", i);
                auto const is_last = i + 1 == block.first + block.count;
                if (is_last && !is_terminator(code))
                    return wrong("the last operation of a block is no terminator");
                if (!is_last && is_terminator(code))
                    return wrong("a terminator before a block's end");
                auto const& inst = _p.instructions[i];
                if (isize(inst.first) + isize(inst.count) > _p.operands.size())
                    return wrong("its operands run past the table");
            }
        }
    }

    [[nodiscard]] bool is_block(u32 raw) const { return isize(raw) < _p.blocks.size(); }

    /// Every edge, the reachable set and a numbering in reverse postorder, from nothing but the terminators.
    void check_edges()
    {
        auto const count = _p.blocks.size();
        _pred_first.resize_to_filled(count + 1, 0u);
        auto const each_successor = [&](isize b, auto&& visit)
        {
            auto const& block = _p.blocks[b];
            if (block.count == 0)
                return;
            auto const& last = _p.instructions[block.first + block.count - 1];
            for (auto const to : successors_of(_p, last, _scratch))
                visit(to);
        };
        for (auto b = isize(0); b < count && _failure.empty(); ++b)
        {
            _block = u32(b);
            if (_p.blocks[b].count != 0)
                _at = _p.blocks[b].first + _p.blocks[b].count - 1;
            each_successor(b,
                           [&](u32 to)
                           {
                               if (!is_block(to))
                                   return wrong("branches to b{}, past the blocks", to);
                               if (_p.blocks[to].count == 0)
                                   return wrong("branches to b{}, which holds nothing", to);
                               ++_pred_first[to + 1];
                           });
        }
        if (!_failure.empty())
            return;
        if (_p.blocks[0].count == 0)
            return fail("an entry block that holds nothing");
        if (_pred_first[1] != 0)
            return fail("a branch back to the entry block");
        for (auto b = isize(0); b < count; ++b)
            _pred_first[b + 1] += _pred_first[b];
        auto cursor = cc::vector<u32>::create_copy_of(cc::span<u32 const>(_pred_first));
        _preds.resize_to_filled(isize(_pred_first[count]), 0u);
        for (auto b = isize(0); b < count; ++b)
            each_successor(b, [&](u32 to) { _preds[cursor[to]++] = u32(b); });

        // one walk, for reverse postorder
        _number.resize_to_filled(count, k_nowhere);
        auto postorder = cc::vector<u32>();
        struct frame
        {
            u32 block;
            cc::vector<u32> to;
            isize next = 0;
        };
        auto stack = cc::vector<frame>();
        auto seen = cc::vector<bool>::create_filled(count, false);
        auto const enter = [&](u32 b)
        {
            seen[b] = true;
            auto to = cc::vector<u32>();
            each_successor(isize(b), [&](u32 next) { to.push_back(next); });
            stack.push_back({.block = b, .to = cc::move(to)});
        };
        enter(0);
        while (!stack.empty())
        {
            auto& top = stack.back();
            if (top.next == top.to.size())
            {
                postorder.push_back(top.block);
                stack.pop_back();
                continue;
            }
            auto const next = top.to[top.next++];
            if (!seen[next])
                enter(next);
        }
        _reachable = postorder.size();
        for (auto k = isize(0); k < postorder.size(); ++k)
            _number[postorder[postorder.size() - 1 - k]] = u32(k);

        // the dominator tree of what a walk reaches, over those numbers
        auto first = cc::vector<u32>::create_filled(_reachable + 1, 0u);
        auto preds = cc::vector<u32>();
        for (auto k = isize(0); k < _reachable; ++k)
        {
            auto const b = postorder[postorder.size() - 1 - k];
            for (auto e = _pred_first[b]; e < _pred_first[b + 1]; ++e)
                if (_number[_preds[e]] != k_nowhere)
                    preds.push_back(_number[_preds[e]]);
            first[k + 1] = u32(preds.size());
        }
        _tree = cc::make_unique<dominance>(cc::span<u32 const>(first), cc::span<u32 const>(preds));

        // a back edge goes to something that dominates it, which is what reducible means
        for (auto b = isize(0); b < count; ++b)
        {
            if (_number[b] == k_nowhere)
                continue;
            for (auto e = _pred_first[b]; e < _pred_first[b + 1]; ++e)
            {
                auto const from = _number[_preds[e]];
                if (from != k_nowhere && from >= _number[b] && !_tree->dominates(_number[b], from))
                    return fail("b{} to b{} is a back edge whose target does not dominate it", _preds[e], b);
            }
        }
    }

    [[nodiscard]] bool dominates_block(u32 a, u32 b) const
    {
        return _number[a] != k_nowhere && _tree->dominates(_number[a], _number[b]);
    }

    // ---- operations

    void check_operations()
    {
        for (auto b = isize(0); b < _p.blocks.size() && _failure.empty(); ++b)
        {
            auto const& block = _p.blocks[b];
            _block = u32(b);
            auto past_phis = false;
            for (auto i = block.first; i < block.first + block.count && _failure.empty(); ++i)
            {
                _at = i;
                auto const& inst = _p.instructions[i];
                if (inst.code != op::phi)
                    past_phis = true;
                else if (past_phis)
                    return wrong("a phi after a block's first other operation");
                check_operands(inst);
                if (_failure.empty())
                    check_hit_object(inst);
                if (_failure.empty())
                    check_shape(inst);
            }
        }
    }

    /// Whether `t` is a hit object, or holds one in a member or an element.
    [[nodiscard]] bool holds_hit_object(type_ref t) const
    {
        auto const& info = _p.at(t);
        switch (info.kind)
        {
        case type_kind::hit_object:
            return true;
        case type_kind::array:
        case type_kind::runtime_array:
            return holds_hit_object(info.element);
        case type_kind::structure:
            for (auto m = u32(0); m < info.member_count; ++m)
                if (holds_hit_object(_p.members[info.first_member + m].type))
                    return true;
            return false;
        default:
            return false;
        }
    }

    /// EMIT-246: a hit object stays where its trace made it, whatever a pass did around it.
    /// One value of one operation, read by the operations that take a hit object and by nothing else.
    void check_hit_object(instruction const& inst)
    {
        auto const makes = inst.code == op::hit_object_trace || inst.code == op::trace_reorder_execute;
        if (gives_value(inst) && holds_hit_object(inst.type) && !makes)
            return wrong("moves a hit object, which only its trace gives");
        auto const takes = inst.code == op::hit_object_reorder || inst.code == op::hit_object_invoke
                        || inst.code == op::hit_object_read;
        auto const operands = _p.operands_of(inst);
        for (auto k = isize(0); k < operands.size(); ++k)
        {
            if (operand_role(inst.code, k) != operand::value)
                continue;
            auto const type = _p.type_of(value_ref(operands[k]));
            if (is_type(type) && holds_hit_object(type) && (!takes || k != 0))
                return wrong("operand {} moves a hit object, which only its readers take", k);
        }
    }

    [[nodiscard]] bool gives_value(instruction const& i) const
    {
        return is_type(i.type) && _p.at(i.type).kind != type_kind::void_;
    }

    void check_operands(instruction const& inst)
    {
        auto const operands = _p.operands_of(inst);
        for (auto k = isize(0); k < operands.size() && _failure.empty(); ++k)
        {
            auto const role = operand_role(inst.code, k);
            if (role == operand::block && !is_block(operands[k]))
                return wrong("operand {} is no block", k);
            if (role != operand::value)
                continue;
            auto const v = value_ref(operands[k]);
            if (!is_valid(v))
                return wrong("operand {} is no value", k);
            auto const index = isize(index_of(v));
            if (is_constant(v) ? index >= _p.constants.size()
                : is_global(v) ? index >= _p.globals.size()
                : is_local(v)  ? index >= _p.locals.size()
                               : isize(u32(v)) >= _p.instructions.size())
                return wrong("operand {} is past its table", k);
            if (is_constant(v) || is_global(v) || is_local(v))
                continue;
            auto const& by = _p.instructions[u32(v)];
            if (!gives_value(by))
                return wrong("operand {} is %{} {}, which gives no value", k, u32(v), op_name(by.code));
            if (_block_of[u32(v)] == k_nowhere)
                return wrong("operand {} is %{}, which stands in no block", k, u32(v));
            if (inst.code == op::phi)
                check_phi_edge(u32(v), operands[k + 1], k);
            else
                check_dominated(u32(v));
        }
        if (inst.code == op::phi)
            check_phi_predecessors(inst);
    }

    void check_dominated(u32 definition)
    {
        if (_number[_block] == k_nowhere)
            return;
        auto const from = _block_of[definition];
        if (from == _block ? definition >= _at : !dominates_block(from, _block))
            wrong("uses %{}, which does not dominate it", definition);
    }

    /// A phi's operand is used at the end of the edge it arrives by.
    void check_phi_edge(u32 definition, u32 edge, isize at)
    {
        if (!is_block(edge) || _number[edge] == k_nowhere)
            return;
        auto const from = _block_of[definition];
        if (!dominates_block(from, edge))
            wrong("operand {} is %{}, which does not dominate the edge from b{}", at, definition, edge);
    }

    void check_phi_predecessors(instruction const& inst)
    {
        auto const operands = _p.operands_of(inst);
        if (operands.size() % 2 != 0)
            return wrong("a value with no block");
        auto const first = _pred_first[_block];
        auto const preds = _pred_first[_block + 1] - first;
        // one block may stand twice among them, for a branch whose two ways meet here
        auto distinct = u32(0);
        for (auto e = first; e < first + preds; ++e)
        {
            auto is_again = false;
            for (auto o = first; o < e; ++o)
                is_again = is_again || _preds[o] == _preds[e];
            distinct += is_again ? 0u : 1u;
        }
        for (auto k = isize(1); k < operands.size(); k += 2)
        {
            auto is_pred = false;
            for (auto e = first; e < first + preds; ++e)
                is_pred = is_pred || _preds[e] == operands[k];
            if (!is_pred)
                return wrong("names b{}, which does not branch here", operands[k]);
            for (auto o = isize(1); o < k; o += 2)
                if (operands[o] == operands[k])
                    return wrong("names b{} twice", operands[k]);
        }
        if (isize(operands.size() / 2) != isize(distinct))
            return wrong("has {} values for {} predecessors", operands.size() / 2, distinct);
    }

    // ---- what each family of operation takes

    [[nodiscard]] type_ref type_at(cc::span<u32 const> operands, isize k) const
    {
        return _p.type_of(value_ref(operands[k]));
    }
    [[nodiscard]] type_kind kind_of(type_ref t) const { return _p.at(t).kind; }
    [[nodiscard]] u32 lanes_of(type_ref t) const { return kind_of(t) == type_kind::vector ? _p.at(t).count : 1u; }
    [[nodiscard]] type_ref scalar_of(type_ref t) const
    {
        return kind_of(t) == type_kind::vector ? _p.at(t).element : t;
    }
    [[nodiscard]] bool is_real(type_ref t) const { return kind_of(scalar_of(t)) == type_kind::float_; }
    [[nodiscard]] bool is_integer(type_ref t) const { return kind_of(scalar_of(t)) == type_kind::int_; }
    [[nodiscard]] bool is_bool(type_ref t) const { return kind_of(scalar_of(t)) == type_kind::bool_; }
    [[nodiscard]] bool same_bits(type_ref a, type_ref b) const
    {
        return lanes_of(a) == lanes_of(b) && _p.at(scalar_of(a)).width == _p.at(scalar_of(b)).width;
    }
    void mismatch(type_ref got, cc::string_view expected)
    {
        wrong("takes {} where {} belongs", type_name(_p, got), expected);
    }
    void need_count(cc::span<u32 const> operands, isize count)
    {
        if (operands.size() != count)
            wrong("holds {} operands, not {}", operands.size(), count);
    }

    /// What `literals` reach inside `from`, or none.
    [[nodiscard]] type_ref walk(type_ref from, cc::span<u32 const> literals)
    {
        for (auto const literal : literals)
        {
            auto const& t = _p.at(from);
            if (t.kind == type_kind::structure)
            {
                if (literal >= t.member_count)
                    return type_ref::none;
                from = _p.members[t.first_member + literal].type;
            }
            else if (t.kind == type_kind::vector || t.kind == type_kind::matrix || t.kind == type_kind::array)
            {
                if (literal >= t.count)
                    return type_ref::none;
                from = t.element;
            }
            else
                return type_ref::none;
        }
        return from;
    }

    void check_construct(instruction const& i, cc::span<u32 const> operands)
    {
        auto const& t = _p.at(i.type);
        if (t.kind == type_kind::vector)
        {
            auto lanes = u32(0);
            for (auto k = isize(0); k < operands.size(); ++k)
            {
                auto const part = type_at(operands, k);
                if (scalar_of(part) != t.element)
                    return mismatch(part, "a part of the vector's scalar");
                lanes += lanes_of(part);
            }
            if (lanes != t.count)
                wrong("holds {} lanes for {}", lanes, t.count);
            return;
        }
        if (t.kind == type_kind::structure)
        {
            if (operands.size() != isize(t.member_count))
                return wrong("holds {} members for {}", operands.size(), t.member_count);
            for (auto k = isize(0); k < operands.size(); ++k)
                if (type_at(operands, k) != _p.members[t.first_member + u32(k)].type)
                    return mismatch(type_at(operands, k), "the member's type");
            return;
        }
        if (t.kind != type_kind::matrix && t.kind != type_kind::array)
            return wrong("builds {}, which is no composite", type_name(_p, i.type));
        if (operands.size() != isize(t.count))
            return wrong("holds {} elements for {}", operands.size(), t.count);
        for (auto k = isize(0); k < operands.size(); ++k)
            if (type_at(operands, k) != t.element)
                return mismatch(type_at(operands, k), "the element type");
    }

    void check_chain(instruction const& i, cc::span<u32 const> operands)
    {
        if (operands.empty())
            return wrong("names no base");
        auto const base = type_at(operands, 0);
        if (kind_of(base) != type_kind::pointer)
            return mismatch(base, "a pointer");
        if (kind_of(i.type) != type_kind::pointer || _p.at(i.type).space != _p.at(base).space)
            return wrong("gives {} in another space than its base", type_name(_p, i.type));
        auto at = _p.at(base).element;
        for (auto k = isize(1); k < operands.size(); ++k)
        {
            auto const index = value_ref(operands[k]);
            if (!is_integer(type_at(operands, k)))
                return mismatch(type_at(operands, k), "an integer index");
            auto const& t = _p.at(at);
            switch (t.kind)
            {
            case type_kind::structure:
            {
                if (!is_constant(index) || _p.constants[index_of(index)].low >= t.member_count)
                    return wrong("indexes a structure with no constant member");
                at = _p.members[t.first_member + _p.constants[index_of(index)].low].type;
                break;
            }
            case type_kind::vector:
            case type_kind::matrix:
            case type_kind::array:
            case type_kind::runtime_array:
                at = t.element;
                break;
            default:
                return wrong("indexes {}", type_name(_p, at));
            }
        }
        if (at != _p.at(i.type).element)
            wrong("reaches {} but gives a pointer to {}", type_name(_p, at), type_name(_p, _p.at(i.type).element));
    }

    void check_shape(instruction const& i)
    {
        auto const operands = _p.operands_of(i);
        auto const s = k_shapes[isize(i.code)];
        auto const gives_nothing = s == shape::store || s == shape::stage_store || s == shape::atomic_store
                                || s == shape::no_result || s == shape::image_write || s == shape::branch
                                || s == shape::branch_conditional || s == shape::switch_ || s == shape::leaves;
        if (gives_nothing)
        {
            if (gives_value(i))
                wrong("gives {}, which it cannot", type_name(_p, i.type));
        }
        else if (!gives_value(i))
            wrong("gives no value");
        if (!_failure.empty())
            return;

        auto const all_are_result = [&]
        {
            for (auto k = isize(0); k < operands.size(); ++k)
                if (type_at(operands, k) != i.type)
                    return mismatch(type_at(operands, k), type_name(_p, i.type));
        };
        auto const same_as_first = [&](isize count)
        {
            need_count(operands, count);
            for (auto k = isize(1); _failure.empty() && k < count; ++k)
                if (type_at(operands, k) != type_at(operands, 0))
                    mismatch(type_at(operands, k), type_name(_p, type_at(operands, 0)));
        };
        auto const pointer_operand = [&]() -> type_ref
        {
            if (operands.empty())
                return wrong("names no pointer"), type_ref::none;
            if (kind_of(type_at(operands, 0)) != type_kind::pointer)
                return mismatch(type_at(operands, 0), "a pointer"), type_ref::none;
            return _p.at(type_at(operands, 0)).element;
        };

        switch (s)
        {
        case shape::any_result:
        case shape::no_result:
        case shape::leaves:
            return;
        case shape::real:
            if (!is_real(i.type))
                return mismatch(i.type, "a float or a vector of them");
            return all_are_result();
        case shape::integer:
            if (!is_integer(i.type))
                return mismatch(i.type, "an integer or a vector of them");
            for (auto k = isize(0); k < operands.size(); ++k)
                if (!is_integer(type_at(operands, k)) || !same_bits(type_at(operands, k), i.type))
                    return mismatch(type_at(operands, k), type_name(_p, i.type));
            return;
        case shape::shift:
        case shape::bit_query:
            for (auto k = isize(0); k < operands.size(); ++k)
                if (!is_integer(type_at(operands, k)) || lanes_of(type_at(operands, k)) != lanes_of(i.type))
                    return mismatch(type_at(operands, k), "integers of the result's lanes");
            return;
        case shape::real_compare:
        case shape::integer_compare:
            same_as_first(2);
            if (_failure.empty()
                && (s == shape::real_compare ? !is_real(type_at(operands, 0)) : !is_integer(type_at(operands, 0))))
                return mismatch(type_at(operands, 0), s == shape::real_compare ? "floats" : "integers");
            [[fallthrough]];
        case shape::real_test:
            if (_failure.empty() && (!is_bool(i.type) || lanes_of(i.type) != lanes_of(type_at(operands, 0))))
                wrong("gives {} of {}", type_name(_p, i.type), type_name(_p, type_at(operands, 0)));
            return;
        case shape::logical:
            if (!is_bool(i.type))
                return mismatch(i.type, "bools");
            return all_are_result();
        case shape::reduce:
            need_count(operands, 1);
            if (_failure.empty() && (kind_of(i.type) != type_kind::bool_ || !is_bool(type_at(operands, 0))))
                mismatch(type_at(operands, 0), "bools reduced to one");
            return;
        case shape::select:
        {
            need_count(operands, 3);
            if (!_failure.empty())
                return;
            auto const condition = type_at(operands, 0);
            if (!is_bool(condition) || (lanes_of(condition) != 1 && lanes_of(condition) != lanes_of(i.type)))
                return mismatch(condition, "a bool for each lane, or one");
            if (type_at(operands, 1) != i.type || type_at(operands, 2) != i.type)
                return mismatch(type_at(operands, 1), type_name(_p, i.type));
            return;
        }
        case shape::float_to_int:
            need_count(operands, 1);
            if (_failure.empty()
                && (!is_real(type_at(operands, 0)) || !is_integer(i.type)
                    || lanes_of(i.type) != lanes_of(type_at(operands, 0))))
                mismatch(type_at(operands, 0), "floats of the result's lanes");
            return;
        case shape::int_to_float:
            need_count(operands, 1);
            if (_failure.empty()
                && (!is_integer(type_at(operands, 0)) || !is_real(i.type)
                    || lanes_of(i.type) != lanes_of(type_at(operands, 0))))
                mismatch(type_at(operands, 0), "integers of the result's lanes");
            return;
        case shape::float_resize:
            need_count(operands, 1);
            if (_failure.empty()
                && (!is_real(type_at(operands, 0)) || !is_real(i.type)
                    || lanes_of(i.type) != lanes_of(type_at(operands, 0))))
                mismatch(type_at(operands, 0), "floats of the result's lanes");
            return;
        case shape::int_resize:
            need_count(operands, 1);
            if (_failure.empty()
                && (!is_integer(type_at(operands, 0)) || !is_integer(i.type)
                    || lanes_of(i.type) != lanes_of(type_at(operands, 0))))
                mismatch(type_at(operands, 0), "integers of the result's lanes");
            return;
        case shape::bitcast:
        {
            need_count(operands, 1);
            if (!_failure.empty())
                return;
            auto const from = type_at(operands, 0);
            if (_p.at(scalar_of(from)).width * lanes_of(from) != _p.at(scalar_of(i.type)).width * lanes_of(i.type))
                mismatch(from, "a value of the result's bits");
            return;
        }
        case shape::dot:
            same_as_first(2);
            if (_failure.empty()
                && (kind_of(type_at(operands, 0)) != type_kind::vector || scalar_of(type_at(operands, 0)) != i.type))
                mismatch(type_at(operands, 0), "vectors of the result");
            return;
        case shape::vector_times_scalar:
        case shape::matrix_times_scalar:
        {
            need_count(operands, 2);
            if (!_failure.empty())
                return;
            if (type_at(operands, 0) != i.type)
                return mismatch(type_at(operands, 0), type_name(_p, i.type));
            auto const lanes
                = s == shape::vector_times_scalar ? _p.at(i.type).element : _p.at(_p.at(i.type).element).element;
            if (type_at(operands, 1) != lanes)
                mismatch(type_at(operands, 1), "the scalar of the other operand");
            return;
        }
        case shape::matrix_times_vector:
        case shape::matrix_times_matrix:
        {
            need_count(operands, 2);
            if (!_failure.empty())
                return;
            auto const left = type_at(operands, 0);
            auto const right = type_at(operands, 1);
            if (kind_of(left) != type_kind::matrix)
                return mismatch(left, "a matrix");
            auto const column = s == shape::matrix_times_vector ? right : _p.at(right).element;
            if (lanes_of(column) != _p.at(left).count)
                mismatch(right, "as many rows as the left has columns");
            return;
        }
        case shape::transpose:
            need_count(operands, 1);
            if (_failure.empty()
                && (kind_of(type_at(operands, 0)) != type_kind::matrix || kind_of(i.type) != type_kind::matrix))
                mismatch(type_at(operands, 0), "a matrix");
            return;
        case shape::construct:
            return check_construct(i, operands);
        case shape::extract:
        {
            if (operands.size() < 2)
                return wrong("names no index");
            if (walk(type_at(operands, 0), operands.subspan(1)) != i.type)
                wrong("reaches no {}", type_name(_p, i.type));
            return;
        }
        case shape::insert:
        {
            if (operands.size() < 3)
                return wrong("names no index");
            if (type_at(operands, 1) != i.type)
                return mismatch(type_at(operands, 1), type_name(_p, i.type));
            if (walk(i.type, operands.subspan(2)) != type_at(operands, 0))
                wrong("reaches no {}", type_name(_p, type_at(operands, 0)));
            return;
        }
        case shape::shuffle:
        {
            if (operands.size() < 3 || kind_of(i.type) != type_kind::vector)
                return wrong("shuffles nothing into a vector");
            auto const held = lanes_of(type_at(operands, 0)) + lanes_of(type_at(operands, 1));
            if (isize(_p.at(i.type).count) != operands.size() - 2)
                return wrong("names {} lanes for {}", operands.size() - 2, _p.at(i.type).count);
            for (auto k = isize(2); k < operands.size(); ++k)
                if (operands[k] >= held && operands[k] != 0xffffffffu)
                    return wrong("names lane {} of {}", operands[k], held);
            return;
        }
        case shape::stage_load:
        case shape::stage_store:
        {
            auto const is_load = s == shape::stage_load;
            if (operands.empty() || operands[0] >= u32(_p.stage_elements.size()))
                return wrong("names no stage element");
            auto const& element = _p.stage_elements[operands[0]];
            auto const held = is_load ? 1 : 2;
            need_count(operands, element.array_size > 0 ? held + 1 : held);
            if (!_failure.empty())
                return;
            if (is_load && i.type != element.type)
                return wrong("gives {} of an element of {}", type_name(_p, i.type), type_name(_p, element.type));
            if (!is_load && type_at(operands, 1) != element.type)
                mismatch(type_at(operands, 1), type_name(_p, element.type));
            return;
        }
        case shape::load:
        {
            auto const held = pointer_operand();
            if (_failure.empty() && held != i.type)
                wrong("gives {} through a pointer to {}", type_name(_p, i.type), type_name(_p, held));
            return;
        }
        case shape::store:
        {
            need_count(operands, 2);
            auto const held = _failure.empty() ? pointer_operand() : type_ref::none;
            if (_failure.empty() && held != type_at(operands, 1))
                mismatch(type_at(operands, 1), type_name(_p, held));
            return;
        }
        case shape::access_chain:
            return check_chain(i, operands);
        case shape::texel_pointer:
        {
            need_count(operands, 3);
            auto const image = _failure.empty() ? pointer_operand() : type_ref::none;
            if (!_failure.empty())
                return;
            if (kind_of(image) != type_kind::image)
                return mismatch(type_at(operands, 0), "a pointer to an image");
            if (kind_of(i.type) != type_kind::pointer || _p.at(i.type).space != storage::image
                || _p.at(i.type).element != _p.at(image).element)
                return mismatch(i.type, "a pointer of image storage to the image's scalar");
            return;
        }
        case shape::array_length:
            (void)pointer_operand();
            return;
        case shape::atomic:
        case shape::atomic_store:
        {
            auto const held = pointer_operand();
            if (!_failure.empty())
                return;
            if (kind_of(held) != type_kind::int_ && kind_of(held) != type_kind::float_)
                return mismatch(held, "an integer or a float in memory");
            for (auto k = isize(1); k < operands.size(); ++k)
                if (!same_bits(type_at(operands, k), held))
                    return mismatch(type_at(operands, k), type_name(_p, held));
            return;
        }
        case shape::combine_sampler:
            need_count(operands, 2);
            if (_failure.empty()
                && (kind_of(i.type) != type_kind::sampled_image || kind_of(type_at(operands, 0)) != type_kind::image
                    || kind_of(type_at(operands, 1)) != type_kind::sampler))
                mismatch(type_at(operands, 0), "an image and a sampler");
            return;
        case shape::image_access:
        case shape::image_write:
            if (operands.empty()
                || (kind_of(type_at(operands, 0)) != type_kind::image
                    && kind_of(type_at(operands, 0)) != type_kind::sampled_image))
                wrong("names no image");
            return;
        case shape::phi:
            for (auto k = isize(0); k < operands.size(); k += 2)
                if (type_at(operands, k) != i.type)
                    return mismatch(type_at(operands, k), type_name(_p, i.type));
            return;
        case shape::branch:
            return need_count(operands, 1);
        case shape::branch_conditional:
            need_count(operands, 3);
            if (_failure.empty() && kind_of(type_at(operands, 0)) != type_kind::bool_)
                mismatch(type_at(operands, 0), "a bool");
            return;
        case shape::switch_:
            if (operands.size() < 2 || operands.size() % 2 != 0)
                return wrong("holds a selector, a default and pairs, not {} operands", operands.size());
            if (kind_of(type_at(operands, 0)) != type_kind::int_)
                mismatch(type_at(operands, 0), "an integer selector");
            return;
        }
    }

    // ---- the order blocks stand in, and what headers promise

    /// `blocks_in_order`'s order must keep a block after its immediate dominator, as a writer and a pass walk it.
    void check_order()
    {
        auto const order = blocks_in_order(_p);
        auto place = cc::vector<u32>::create_filled(_p.blocks.size(), 0u);
        for (auto k = isize(0); k < order.size(); ++k)
            place[isize(order[k])] = u32(k);
        if (place[0] != 0)
            return fail("the entry block is not the first to open");
        auto by_number = cc::vector<u32>::create_filled(_reachable, 0u);
        for (auto b = isize(0); b < _p.blocks.size(); ++b)
            if (_number[b] != k_nowhere)
                by_number[_number[b]] = u32(b);
        for (auto k = isize(1); k < _reachable; ++k)
        {
            auto const up = by_number[_tree->immediate(u32(k))];
            if (place[up] >= place[by_number[k]])
                return fail("b{} opens before b{}, which dominates it", by_number[k], up);
        }
    }

    void check_structure()
    {
        if (_flow != control_flow::structured)
            return;
        auto merged_by = cc::vector<u32>::create_filled(_p.blocks.size(), k_nowhere);
        for (auto b = isize(0); b < _p.blocks.size(); ++b)
        {
            auto const& block = _p.blocks[b];
            if (block.merges == merge_kind::none || block.count == 0)
                continue;
            auto const merge = u32(block.merge);
            if (!is_valid(block.merge) || !is_block(merge) || _p.blocks[merge].count == 0)
                return fail("b{} names a merge block that does not stand", b);
            if (merged_by[merge] != k_nowhere)
                return fail("b{} merges both b{} and b{}", merge, merged_by[merge], b);
            merged_by[merge] = u32(b);
            auto const code = _p.instructions[block.first + block.count - 1].code;
            if (block.merges == merge_kind::selection && code != op::branch_conditional && code != op::switch_)
                return fail("b{} is a selection's header but does not select", b);
            if (block.merges == merge_kind::loop)
            {
                auto const next = u32(block.continue_to);
                if (!is_valid(block.continue_to) || !is_block(next) || _p.blocks[next].count == 0)
                    return fail("b{} names a continue target that does not stand", b);
                if (code != op::branch && code != op::branch_conditional)
                    return fail("b{} is a loop's header and ends in {}", b, op_name(code));
                if (_number[b] != k_nowhere && _number[next] != k_nowhere && !dominates_block(u32(b), next))
                    return fail("b{} does not dominate its continue target b{}", b, next);
            }
            if (_number[b] != k_nowhere && _number[merge] != k_nowhere && !dominates_block(u32(b), merge))
                return fail("b{} does not dominate its merge block b{}", b, merge);
        }
        // a back edge closes a loop this form declares
        for (auto b = isize(0); b < _p.blocks.size(); ++b)
        {
            if (_number[b] == k_nowhere)
                continue;
            for (auto e = _pred_first[b]; e < _pred_first[b + 1]; ++e)
            {
                auto const from = _number[_preds[e]];
                if (from != k_nowhere && from >= _number[b] && _p.blocks[b].merges != merge_kind::loop)
                    return fail("b{} branches back to b{}, which heads no loop", _preds[e], b);
            }
        }
    }

    program const& _p;
    control_flow _flow;
    cc::string _failure;
    /// Where the walk stands.
    u32 _block = 0;
    u32 _at = 0;
    isize _reachable = 0;
    /// Parallel to the instructions: the block holding each.
    cc::vector<u32> _block_of;
    /// Each block's predecessors as a run, one for each edge.
    cc::vector<u32> _pred_first;
    cc::vector<u32> _preds;
    /// Parallel to the blocks: a place in reverse postorder, `k_nowhere` for what no walk reaches.
    cc::vector<u32> _number;
    cc::vector<u32> _scratch;
    cc::unique_ptr<dominance> _tree;
};
} // namespace

cc::optional<cc::string> sgl::ir::verify(program const& p, control_flow flow)
{
    return verifier(p, flow).run();
}
