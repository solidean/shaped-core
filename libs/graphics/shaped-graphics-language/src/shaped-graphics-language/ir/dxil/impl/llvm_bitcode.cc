#include "llvm.hh"

#include <clean-core/common/assert.hh>

using namespace sgl;
using namespace sgl::ir::dxil::impl;

// The module as LLVM 3.7's bitcode, every record unabbreviated: a reader takes that, and an abbreviation only saves bytes.
// The record layouts are what DXC's own output shows, read with llvm-bcanalyzer's eyes; nothing here links LLVM.

namespace
{
/// The bitstream LLVM's reader takes: little-endian 32-bit words, filled from their low bits.
class bit_writer
{
public:
    void fixed(u64 value, u32 width)
    {
        _current |= value << _used;
        _used += width;
        while (_used >= 32)
        {
            _words.push_back(u32(_current));
            _current >>= 32;
            _used -= 32;
        }
    }

    void vbr(u64 value, u32 width)
    {
        auto const high = u64(1) << (width - 1);
        while (value >= high)
        {
            fixed((value & (high - 1)) | high, width);
            value >>= width - 1;
        }
        fixed(value, width);
    }

    void align()
    {
        if (_used > 0)
            fixed(0, 32 - _used);
    }

    void enter_block(u32 id, u32 width)
    {
        fixed(1, _width);
        vbr(id, 8);
        vbr(width, 4);
        align();
        _stack.push_back({.width = _width, .size_at = u32(_words.size())});
        _words.push_back(0);
        _width = width;
    }

    void exit_block()
    {
        fixed(0, _width);
        align();
        auto const open = _stack.back();
        _stack.pop_back();
        _words[open.size_at] = u32(_words.size()) - open.size_at - 1;
        _width = open.width;
    }

    /// A record with no abbreviation, every operand in VBR6.
    void record(u32 code, cc::span<u64 const> operands)
    {
        fixed(3, _width);
        vbr(code, 6);
        vbr(u64(operands.size()), 6);
        for (auto const o : operands)
            vbr(o, 6);
    }

    void record(u32 code, std::initializer_list<u64> operands)
    {
        record(code, cc::span<u64 const>(operands.begin(), isize(operands.size())));
    }

    void string_record(u32 code, cc::string_view text)
    {
        auto operands = cc::vector<u64>::create_with_capacity(text.size());
        for (auto const c : text)
            operands.push_back(u8(c));
        record(code, operands);
    }

    [[nodiscard]] cc::vector<byte> take_bytes()
    {
        CC_ASSERT(_used == 0, "a stream ends on a word");
        return cc::vector<byte>::create_copy_of(cc::span<u32 const>(_words).as_bytes());
    }

private:
    struct open_block
    {
        u32 width = 0;
        u32 size_at = 0;
    };
    cc::vector<u32> _words;
    cc::vector<open_block> _stack;
    u64 _current = 0;
    u32 _used = 0;
    u32 _width = 2;
};

// block ids and record codes, as LLVM's bitcode reader names them
constexpr u32 k_block_module = 8;
constexpr u32 k_block_paramattr = 9;
constexpr u32 k_cst_cast = 11;
constexpr u32 k_cast_bitcast = 11;
constexpr u32 k_block_paramattr_group = 10;
constexpr u32 k_block_constants = 11;
constexpr u32 k_block_function = 12;
constexpr u32 k_block_value_symtab = 14;
constexpr u32 k_block_metadata = 15;
constexpr u32 k_block_type = 17;
constexpr u32 k_block_metadata_kind = 15;

constexpr u32 k_type_numentry = 1;
constexpr u32 k_type_void = 2;
constexpr u32 k_type_float = 3;
constexpr u32 k_type_double = 4;
constexpr u32 k_type_label = 5;
constexpr u32 k_type_integer = 7;
constexpr u32 k_type_pointer = 8;
constexpr u32 k_type_half = 10;
constexpr u32 k_type_array = 11;
constexpr u32 k_type_metadata = 16;
constexpr u32 k_type_struct_name = 19;
constexpr u32 k_type_struct_named = 20;
constexpr u32 k_type_function = 21;

constexpr u32 k_cst_settype = 1;
constexpr u32 k_cst_null = 2;
constexpr u32 k_cst_undef = 3;
constexpr u32 k_cst_integer = 4;
constexpr u32 k_cst_float = 6;
constexpr u32 k_cst_aggregate = 7;

constexpr u32 k_inst_declareblocks = 1;
constexpr u32 k_inst_binop = 2;
constexpr u32 k_inst_cast = 3;
constexpr u32 k_inst_ret = 10;
constexpr u32 k_inst_br = 11;
constexpr u32 k_inst_switch = 12;
constexpr u32 k_inst_unreachable = 15;
constexpr u32 k_inst_phi = 16;
constexpr u32 k_inst_alloca = 19;
constexpr u32 k_inst_load = 20;
constexpr u32 k_inst_extractval = 26;
constexpr u32 k_inst_cmp2 = 28;
constexpr u32 k_inst_vselect = 29;
constexpr u32 k_inst_call = 34;
constexpr u32 k_inst_atomicrmw = 38;
constexpr u32 k_inst_gep = 43;
constexpr u32 k_inst_store = 44;
constexpr u32 k_inst_cmpxchg = 46;

constexpr u32 k_md_string = 1;
constexpr u32 k_md_value = 2;
constexpr u32 k_md_node = 3;
constexpr u32 k_md_name = 4;
constexpr u32 k_md_kind = 6;
constexpr u32 k_md_named_node = 10;
constexpr u32 k_inst_debug_loc_again = 33;
constexpr u32 k_inst_debug_loc = 35;

constexpr u32 k_order_seq_cst = 6;
constexpr u32 k_scope_cross_thread = 1;

/// The metadata kinds an LLVM 3.7 context fixes, then the ones DXC's context adds, in the order its reader expects.
constexpr cc::string_view k_metadata_kinds[] = {
    "dbg",
    "tbaa",
    "prof",
    "fpmath",
    "range",
    "tbaa.struct",
    "invariant.load",
    "alias.scope",
    "noalias",
    "nontemporal",
    "llvm.mem.parallel_loop_access",
    "nonnull",
    "dereferenceable",
    "dereferenceable_or_null",
    "dx.temp",
    "dx.dbg.varlayout",
    "dx.break.br",
    "dx.nonuniform",
};
/// The number of "dx.nonuniform" in `k_metadata_kinds`.
constexpr u32 k_kind_nonuniform = 17;
constexpr u32 k_block_metadata_attachment = 16;
constexpr u32 k_md_attachment = 11;

/// A signed value as LLVM's VBR states it: its magnitude shifted up, the sign in the low bit.
[[nodiscard]] constexpr u64 signed_vbr(i64 value)
{
    return value >= 0 ? u64(value) << 1 : (u64(-value) << 1) | 1u;
}
} // namespace

cc::vector<byte> module_builder::write(cc::string_view triple, cc::string_view layout, bool with_debug_info) const
{
    auto out = bit_writer();
    out.fixed('B', 8);
    out.fixed('C', 8);
    out.fixed(0xC0, 8);
    out.fixed(0xDE, 8);

    // The validator refuses an operation declared and never called, which a call the passes removed leaves behind.
    // So a declaration nothing calls is left out as the module is written, and a global's number is its place among
    // the ones kept: renumbering the module instead would leave stale every global value the writer holds.
    auto is_kept = cc::vector<bool>::create_filled(_globals.size(), false);
    for (auto const& inst : _instructions)
        if (inst.code == inst_code::call)
            is_kept[ll_index(ll_value(_args[inst.first]))] = true;
    auto global_numbers = cc::vector<u32>::create_filled(_globals.size(), 0u);
    auto global_count = u32(0);
    for (auto g = isize(0); g < _globals.size(); ++g)
    {
        is_kept[g] = is_kept[g] || !_globals[g].is_declaration;
        global_numbers[g] = global_count;
        global_count += is_kept[g] ? 1 : 0;
    }
    // the debug info's constants come last and no instruction names one, so a write without it can leave them out
    auto const constant_count
        = with_debug_info ? u32(_constants.size()) : cc::min(u32(_constants.size()), _debug_constants_from);
    auto const argument_count = u32(_arguments.size());
    auto const number_of = [&](u32 raw) -> u32
    {
        auto const v = ll_value(raw);
        if (ll_is_argument(v))
            return global_count + constant_count + ll_index(v);
        if (ll_is_constant(v))
            return global_count + ll_index(v);
        if (ll_is_global(v))
            return global_numbers[ll_index(v)];
        return global_count + constant_count + argument_count + raw;
    };

    out.enter_block(k_block_module, 3);
    out.record(1, {1u}); // version 1: relative value ids

    if (_attribute_lists.size() > 1)
    {
        out.enter_block(k_block_paramattr_group, 3);
        auto group = cc::vector<u64>();
        for (auto i = isize(1); i < _attribute_lists.size(); ++i)
        {
            group.clear();
            group.push_back(u64(i));
            group.push_back(0xffffffffu); // the function itself
            auto const& list = _attribute_lists[i];
            for (auto k = u32(0); k < list.count; ++k)
            {
                auto const& a = _attribute_items[list.first + k];
                if (a.kind != 0)
                {
                    group.push_back(0);
                    group.push_back(a.kind);
                    continue;
                }
                group.push_back(a.value.empty() ? 3u : 4u);
                for (auto const c : a.key)
                    group.push_back(u8(c));
                group.push_back(0);
                if (!a.value.empty())
                {
                    for (auto const c : a.value)
                        group.push_back(u8(c));
                    group.push_back(0);
                }
            }
            out.record(3, group);
        }
        out.exit_block();

        out.enter_block(k_block_paramattr, 3);
        for (auto i = isize(1); i < _attribute_lists.size(); ++i)
            out.record(2, {u64(i)});
        out.exit_block();
    }

    out.enter_block(k_block_type, 4);
    out.record(k_type_numentry, {u64(_types.size())});
    auto scratch = cc::vector<u64>();
    for (auto const& t : _types)
    {
        switch (t.code)
        {
        case type_code::void_:
            out.record(k_type_void, {});
            break;
        case type_code::integer:
            out.record(k_type_integer, {t.number});
            break;
        case type_code::half:
            out.record(k_type_half, {});
            break;
        case type_code::float_:
            out.record(k_type_float, {});
            break;
        case type_code::double_:
            out.record(k_type_double, {});
            break;
        case type_code::label:
            out.record(k_type_label, {});
            break;
        case type_code::metadata:
            out.record(k_type_metadata, {});
            break;
        case type_code::pointer:
            out.record(k_type_pointer, {u64(u32(t.element)), t.number});
            break;
        case type_code::array:
            out.record(k_type_array, {t.number, u64(u32(t.element))});
            break;
        case type_code::function:
            scratch.clear();
            scratch.push_back(0); // not variadic
            for (auto k = u32(0); k < t.count; ++k)
                scratch.push_back(_type_args[t.first + k]);
            out.record(k_type_function, scratch);
            break;
        case type_code::structure:
            if (t.name_size > 0)
                out.string_record(k_type_struct_name, name_at(t.name_first, t.name_size));
            scratch.clear();
            scratch.push_back(0); // not packed
            for (auto k = u32(0); k < t.count; ++k)
                scratch.push_back(_type_args[t.first + k]);
            out.record(t.name_size > 0 ? k_type_struct_named : 18u, scratch);
            break;
        }
    }
    out.exit_block();

    out.string_record(2, triple);
    out.string_record(3, layout);
    // a value's number is its record's position, so a variable and a function stand in the order they were added
    for (auto i = isize(0); i < _globals.size(); ++i)
    {
        if (!is_kept[i])
            continue;
        auto const& g = _globals[i];
        if (!g.is_function)
        {
            // an initializer is its value's number plus one, and makes the variable internal, linkage 3
            auto const has_value = g.initializer != ll_value::none;
            out.record(7, {u64(u32(g.type)), (g.is_constant ? 3u : 2u) | (u64(g.address_space) << 2),
                           has_value ? number_of(u32(g.initializer)) + 1 : 0u, has_value ? 3u : 0u,
                           log2_plus_one(g.alignment), 0u});
            continue;
        }
        u64 const fields[] = {
            u64(u32(g.type)), 0u, g.is_declaration ? 1u : 0u, 0u, g.attributes, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};
        out.record(8, cc::span<u64 const>(fields, isize(sizeof(fields) / sizeof(fields[0]))));
    }

    if (constant_count > 0)
    {
        out.enter_block(k_block_constants, 4);
        auto current = ll_type::none;
        for (auto const& c : cc::span<constant_entry const>(_constants.data(), constant_count))
        {
            if (c.type != current)
            {
                current = c.type;
                out.record(k_cst_settype, {u64(u32(current))});
            }
            switch (c.code)
            {
            case constant_code::integer:
            {
                // the bits sign-extended, as LLVM states an integer of any width
                auto const width = bits_of(c.type);
                auto value = i64(c.bits);
                if (width < 64)
                    value = i64(c.bits << (64 - width)) >> (64 - width);
                if (c.bits == 0 && width > 0)
                {
                    out.record(k_cst_null, {});
                    break;
                }
                // i1 true is 1, which sign extension would make -1
                out.record(k_cst_integer, {signed_vbr(width == 1 ? i64(c.bits) : value)});
                break;
            }
            case constant_code::floating:
                out.record(k_cst_float, {c.bits});
                break;
            case constant_code::null_:
                out.record(k_cst_null, {});
                break;
            case constant_code::undefined:
                out.record(k_cst_undef, {});
                break;
            case constant_code::aggregate:
                scratch.clear();
                for (auto k = u32(0); k < c.count; ++k)
                    scratch.push_back(number_of(_constant_args[c.first + k]));
                out.record(k_cst_aggregate, scratch);
                break;
            case constant_code::bitcast:
            {
                auto const source = ll_value(_constant_args[c.first]);
                out.record(k_cst_cast, {k_cast_bitcast, u64(u32(type_of(source))), u64(number_of(u32(source)))});
                break;
            }
            }
        }
        out.exit_block();
    }

    auto const metadata_end = with_debug_info ? _metadata.size() : cc::min(_metadata.size(), isize(_debug_metadata_from));
    auto const named_end = with_debug_info ? _named.size() : cc::min(_named.size(), isize(_debug_named_from));
    if (metadata_end > 0)
    {
        out.enter_block(k_block_metadata, 3);
        for (auto const& m : cc::span<md_entry const>(_metadata.data(), metadata_end))
            switch (m.code)
            {
            case md_code::string:
                out.string_record(k_md_string, name_at(m.first, m.count));
                break;
            case md_code::value:
                out.record(k_md_value, {u64(u32(m.type)), number_of(u32(m.value))});
                break;
            case md_code::node:
                scratch.clear();
                for (auto k = u32(0); k < m.count; ++k)
                    scratch.push_back(_md_args[m.first + k]);
                out.record(k_md_node, scratch);
                break;
            case md_code::record:
                scratch.clear();
                for (auto k = u32(0); k < m.count; ++k)
                    scratch.push_back(_md_fields[m.first + k]);
                out.record(m.record, scratch);
                break;
            }
        for (auto const& n : cc::span<named_md const>(_named.data(), named_end))
        {
            out.string_record(k_md_name, name_at(n.name_first, n.name_size));
            scratch.clear();
            for (auto k = u32(0); k < n.count; ++k)
                scratch.push_back(_md_args[n.first + k] - 1);
            out.record(k_md_named_node, scratch);
        }
        out.exit_block();
    }

    out.enter_block(k_block_metadata_kind, 3);
    auto kind_number = u64(0);
    for (auto const kind : k_metadata_kinds)
    {
        scratch.clear();
        scratch.push_back(kind_number++);
        for (auto const c : kind)
            scratch.push_back(u8(c));
        out.record(k_md_kind, scratch);
    }
    out.exit_block();

    out.enter_block(k_block_value_symtab, 4);
    for (auto i = isize(0); i < _globals.size(); ++i)
    {
        if (!is_kept[i])
            continue;
        scratch.clear();
        scratch.push_back(u64(global_numbers[i]));
        for (auto const c : name_at(_globals[i].name_first, _globals[i].name_size))
            scratch.push_back(u8(c));
        out.record(1, scratch);
    }
    out.exit_block();

    // ---- the functions: each numbers its own blocks and values from where the module's end
    auto first_block = u32(0);
    auto earlier_values = u32(0);
    for (auto body = isize(0); body <= _later_bodies.size(); ++body)
    {
        auto const end_block = body < _later_bodies.size() ? _later_bodies[body] : u32(_blocks.size());
        out.enter_block(k_block_function, 4);
        out.record(k_inst_declareblocks, {u64(end_block - first_block)});
        auto next = global_count + constant_count + argument_count;
        auto values_here = u32(0);
        auto const local_number = [&](u32 raw) -> u32
        {
            auto const v = ll_value(raw);
            return ll_is_constant(v) || ll_is_global(v) ? number_of(raw) : number_of(raw) - earlier_values;
        };
        auto const relative = [&](u32 raw) -> u64 { return u64(next - local_number(raw)); };
        auto const signed_relative = [&](u32 raw) -> u64 { return signed_vbr(i64(next) - i64(local_number(raw))); };
        auto const block_number = [&](u32 raw) -> u64 { return u64(raw - first_block); };
        auto marked = cc::vector<u32>();
        auto position = u32(0);
        auto last_location = u32(0);
        for (auto block_at = first_block; block_at < end_block; ++block_at)
        {
            auto const& b = _blocks[block_at];
            for (auto i = b.first; i < b.first + b.count; ++i, ++position)
            {
                auto const& inst = _instructions[i];
                if (inst.code == inst_code::element_pointer && inst.sub == k_ll_nonuniform)
                    marked.push_back(position);
                auto const args = cc::span<u32 const>(_args.data() + inst.first, isize(inst.count));
                switch (inst.code)
                {
                case inst_code::binary:
                    scratch.clear();
                    scratch.push_back(relative(args[0]));
                    scratch.push_back(relative(args[1]));
                    scratch.push_back(inst.sub);
                    if (inst.flags != 0)
                        scratch.push_back(inst.flags);
                    out.record(k_inst_binop, scratch);
                    break;
                case inst_code::cast:
                    out.record(k_inst_cast, {relative(args[0]), u64(u32(inst.type)), u64(inst.sub)});
                    break;
                case inst_code::compare:
                    scratch.clear();
                    scratch.push_back(relative(args[0]));
                    scratch.push_back(relative(args[1]));
                    scratch.push_back(inst.sub);
                    if (inst.flags != 0)
                        scratch.push_back(inst.flags);
                    out.record(k_inst_cmp2, scratch);
                    break;
                case inst_code::select:
                    out.record(k_inst_vselect, {relative(args[0]), relative(args[1]), relative(args[2])});
                    break;
                case inst_code::extract_value:
                    out.record(k_inst_extractval, {relative(args[0]), u64(args[1])});
                    break;
                case inst_code::call:
                {
                    scratch.clear();
                    scratch.push_back(0);
                    scratch.push_back(1u << 15); // the function's type is stated
                    scratch.push_back(u32(_globals[ll_index(ll_value(args[0]))].type));
                    scratch.push_back(relative(args[0]));
                    for (auto k = isize(1); k < args.size(); ++k)
                        scratch.push_back(relative(args[k]));
                    out.record(k_inst_call, scratch);
                    break;
                }
                case inst_code::alloca_:
                    out.record(k_inst_alloca,
                               {u64(args[0]), u64(u32(_i32)), number_of(args[1]), u64(inst.flags) | (1u << 6)});
                    break;
                case inst_code::load:
                    out.record(k_inst_load, {relative(args[0]), u64(u32(inst.type)), u64(inst.flags), 0u});
                    break;
                case inst_code::store:
                    out.record(k_inst_store, {relative(args[0]), relative(args[1]), u64(inst.flags), 0u});
                    break;
                case inst_code::element_pointer:
                    scratch.clear();
                    scratch.push_back(1); // inbounds
                    scratch.push_back(args[0]);
                    for (auto k = isize(1); k < args.size(); ++k)
                        scratch.push_back(relative(args[k]));
                    out.record(k_inst_gep, scratch);
                    break;
                case inst_code::atomic_rmw:
                    out.record(k_inst_atomicrmw, {relative(args[0]), relative(args[1]), u64(inst.sub), 0u,
                                                  k_order_seq_cst, k_scope_cross_thread});
                    break;
                case inst_code::atomic_exchange:
                    out.record(k_inst_cmpxchg, {relative(args[0]), relative(args[1]), relative(args[2]), 0u,
                                                k_order_seq_cst, k_scope_cross_thread, k_order_seq_cst, 0u});
                    break;
                case inst_code::phi:
                    scratch.clear();
                    scratch.push_back(u32(inst.type));
                    for (auto k = isize(0); k < args.size(); k += 2)
                    {
                        scratch.push_back(signed_relative(args[k]));
                        scratch.push_back(block_number(args[k + 1]));
                    }
                    out.record(k_inst_phi, scratch);
                    break;
                case inst_code::branch:
                    out.record(k_inst_br, {block_number(args[0])});
                    break;
                case inst_code::branch_if:
                    out.record(k_inst_br, {block_number(args[0]), block_number(args[1]), relative(args[2])});
                    break;
                case inst_code::switch_:
                    scratch.clear();
                    scratch.push_back(u32(type_of(ll_value(args[0]))));
                    scratch.push_back(relative(args[0]));
                    scratch.push_back(block_number(args[1]));
                    for (auto k = isize(2); k < args.size(); k += 2)
                    {
                        scratch.push_back(number_of(args[k]));
                        scratch.push_back(block_number(args[k + 1]));
                    }
                    out.record(k_inst_switch, scratch);
                    break;
                case inst_code::return_void:
                    out.record(k_inst_ret, {});
                    break;
                case inst_code::unreachable:
                    out.record(k_inst_unreachable, {});
                    break;
                }
                // EMIT-257: an instruction's location follows it, and one the same as the last is said again for free
                if (with_debug_info && inst.location != 0 && inst.location <= u32(_debug_locations.size()))
                {
                    if (inst.location == last_location)
                        out.record(k_inst_debug_loc_again, {});
                    else
                    {
                        auto const& at = _debug_locations[inst.location - 1];
                        out.record(k_inst_debug_loc, {u64(at.line), u64(at.column), u64(at.scope), u64(at.inlined_at)});
                    }
                    last_location = inst.location;
                }
                if (inst.type != _void)
                {
                    ++next;
                    ++values_here;
                }
            }
        }
        if (!marked.empty())
        {
            out.enter_block(k_block_metadata_attachment, 3);
            for (auto const at : marked)
                out.record(k_md_attachment, {u64(at), u64(k_kind_nonuniform), u64(_nonuniform_node - 1)});
            out.exit_block();
        }
        out.exit_block();
        first_block = end_block;
        earlier_values += values_here;
    }
    out.exit_block();
    return out.take_bytes();
}
