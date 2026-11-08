#include "llvm37.hh"

#include <babel-data/shader/llvm_bitstream.hh>
#include <clean-core/string/format.hh>

// The module block's records first, then its function blocks: a body's value ids start after every module value,
// so its blocks wait until the module's globals, functions and constants are numbered.

namespace babel::llvm37
{
namespace
{
namespace block_id
{
constexpr u32 module = 8;
constexpr u32 constants = 11;
constexpr u32 function = 12;
constexpr u32 value_symtab = 14;
constexpr u32 type = 17;
} // namespace block_id

constexpr u32 k_none = 0xffffffffu;

[[nodiscard]] i64 decode_signed(u64 v)
{
    return (v & 1) != 0 ? -i64(v >> 1) : i64(v >> 1);
}

class module_reader
{
public:
    module_reader(llvm::bitstream const& s, module& m) : _s(s), _m(m) {}

    cc::result<bool> run()
    {
        auto module_block = i64(-1);
        for (auto const& e : _s.blocks[0].entries)
            if (e.is_block && _s.blocks[e.index].id == block_id::module)
                module_block = e.index;
        if (module_block < 0)
            return cc::error("llvm37: the bitstream holds no module block");
        auto const& mb = _s.blocks[isize(module_block)];

        // the module's own records and blocks, in order, but its function bodies
        for (auto const& e : mb.entries)
        {
            if (!e.is_block)
            {
                if (auto r = module_record(_s.records[e.index]); r.has_error())
                    return r;
                continue;
            }
            auto const& b = _s.blocks[e.index];
            if (b.id == block_id::type)
                read_types(b);
            else if (b.id == block_id::constants)
                read_constants(b, _m.values);
            else if (b.id == block_id::value_symtab)
                read_names(b);
        }
        // then each defined function's body, in the order the functions were declared
        auto defined = cc::vector<u32>();
        for (auto f = isize(0); f < _m.functions.size(); ++f)
            if (!_m.functions[f].is_declaration)
                defined.push_back(u32(f));
        auto next = isize(0);
        for (auto const& e : mb.entries)
        {
            if (!e.is_block || _s.blocks[e.index].id != block_id::function)
                continue;
            if (next >= defined.size())
                return cc::error("llvm37: more function bodies than defined functions");
            if (auto r = read_body(_s.blocks[e.index], defined[next++]); r.has_error())
                return r;
        }
        return true;
    }

private:
    llvm::bitstream const& _s;
    module& _m;
    bool _relative = false;

    [[nodiscard]] cc::string string_of(cc::span<u64 const> chars) const
    {
        auto out = cc::string();
        for (auto const c : chars)
            out += char(c);
        return out;
    }

    // ---- types

    [[nodiscard]] u32 find_type(type_kind kind, u32 element, u64 count = 0, u32 width = 0) const
    {
        for (auto t = isize(0); t < _m.types.size(); ++t)
        {
            auto const& x = _m.types[t];
            if (x.kind == kind && (kind == type_kind::integer ? x.width == width : x.element == element)
                && (kind != type_kind::vector || x.count == count))
                return u32(t);
        }
        return k_none;
    }
    [[nodiscard]] u32 void_type() const
    {
        for (auto t = isize(0); t < _m.types.size(); ++t)
            if (_m.types[t].kind == type_kind::void_)
                return u32(t);
        return 0;
    }
    [[nodiscard]] type const* type_at(u32 t) const { return t < u32(_m.types.size()) ? &_m.types[t] : nullptr; }

    void read_types(llvm::block const& b)
    {
        auto pending_name = cc::string();
        for (auto const& e : b.entries)
        {
            if (e.is_block)
                continue;
            auto const& r = _s.records[e.index];
            auto const ops = _s.operands_of(r);
            auto t = type();
            switch (r.code)
            {
            case 1: // NUMENTRY
                continue;
            case 2:
                t.kind = type_kind::void_;
                break;
            case 3:
                t.kind = type_kind::float_;
                break;
            case 4:
                t.kind = type_kind::double_;
                break;
            case 5:
                t.kind = type_kind::label;
                break;
            case 6:
                t.kind = type_kind::opaque;
                t.name = cc::move(pending_name);
                pending_name = {};
                break;
            case 7:
                t.kind = type_kind::integer;
                t.width = ops.size() > 0 ? u32(ops[0]) : 0;
                break;
            case 8:
                t.kind = type_kind::pointer;
                t.element = ops.size() > 0 ? u32(ops[0]) : 0;
                t.address_space = ops.size() > 1 ? u32(ops[1]) : 0;
                break;
            case 10:
                t.kind = type_kind::half;
                break;
            case 11:
            case 12:
                t.kind = r.code == 11 ? type_kind::array : type_kind::vector;
                t.count = ops.size() > 0 ? ops[0] : 0;
                t.element = ops.size() > 1 ? u32(ops[1]) : 0;
                break;
            case 16:
                t.kind = type_kind::metadata;
                break;
            case 18: // STRUCT_ANON: [packed, members...]
            case 20: // STRUCT_NAMED
                t.kind = type_kind::structure;
                t.first_member = u32(_m.members.size());
                for (auto k = isize(1); k < ops.size(); ++k)
                    _m.members.push_back(u32(ops[k]));
                t.member_count = u32(_m.members.size()) - t.first_member;
                if (r.code == 20)
                {
                    t.name = cc::move(pending_name);
                    pending_name = {};
                }
                break;
            case 19: // STRUCT_NAME: the next named struct's or opaque's
                pending_name = string_of(ops);
                continue;
            case 21: // FUNCTION: [vararg, return, params...]
                t.kind = type_kind::function;
                t.element = ops.size() > 1 ? u32(ops[1]) : 0;
                t.first_member = u32(_m.members.size());
                for (auto k = isize(2); k < ops.size(); ++k)
                    _m.members.push_back(u32(ops[k]));
                t.member_count = u32(_m.members.size()) - t.first_member;
                break;
            default:
                t.kind = type_kind::other;
                break;
            }
            _m.types.push_back(cc::move(t));
        }
    }

    // ---- the module's records

    cc::result<bool> module_record(llvm::record const& r)
    {
        auto const ops = _s.operands_of(r);
        switch (r.code)
        {
        case 1: // VERSION: 1 on means relative value ids
            _relative = ops.size() > 0 && ops[0] >= 1;
            return true;
        case 2:
            _m.triple = string_of(ops);
            return true;
        case 3:
            _m.data_layout = string_of(ops);
            return true;
        case 7: // GLOBALVAR: [type, constant | explicit type << 1 | address space << 2, initializer + 1, ...]
        {
            if (ops.size() < 3)
                return cc::error("llvm37: a GLOBALVAR record short of its type and initializer");
            auto g = global_variable();
            auto const explicit_type = (ops[1] & 2) != 0;
            g.is_constant = (ops[1] & 1) != 0;
            if (explicit_type)
            {
                g.type = u32(ops[0]);
                g.address_space = u32(ops[1] >> 2);
            }
            else if (auto const* const p = type_at(u32(ops[0])); p != nullptr && p->kind == type_kind::pointer)
            {
                g.type = p->element;
                g.address_space = p->address_space;
            }
            g.initializer = ops[2] == 0 ? k_none : u32(ops[2] - 1);
            g.value_id = u32(_m.values.size());
            auto const pointer = find_type(type_kind::pointer, g.type);
            _m.values.push_back({.kind = value_kind::global_variable,
                                 .type = pointer == k_none ? g.type : pointer,
                                 .index = u32(_m.globals.size())});
            _m.globals.push_back(cc::move(g));
            return true;
        }
        case 8: // FUNCTION: [type, calling convention, is declaration, ...]
        {
            if (ops.size() < 3)
                return cc::error("llvm37: a FUNCTION record short of its type");
            auto f = function();
            f.type = u32(ops[0]);
            if (auto const* const p = type_at(f.type); p != nullptr && p->kind == type_kind::pointer)
                f.type = p->element;
            f.is_declaration = ops[2] != 0;
            f.value_id = u32(_m.values.size());
            auto const pointer = find_type(type_kind::pointer, f.type);
            _m.values.push_back({.kind = value_kind::function,
                                 .type = pointer == k_none ? f.type : pointer,
                                 .index = u32(_m.functions.size())});
            _m.functions.push_back(cc::move(f));
            return true;
        }
        default:
            return true;
        }
    }

    void read_constants(llvm::block const& b, cc::vector<value>& values)
    {
        auto current = u32(0);
        for (auto const& e : b.entries)
        {
            if (e.is_block)
                continue;
            auto const& r = _s.records[e.index];
            auto const ops = _s.operands_of(r);
            if (r.code == 1) // SETTYPE
            {
                current = ops.size() > 0 ? u32(ops[0]) : 0;
                continue;
            }
            auto c = constant{.type = current,
                              .code = u16(r.code),
                              .first = u32(_m.constant_operands.size()),
                              .count = u32(ops.size())};
            for (auto const o : ops)
                _m.constant_operands.push_back(o);
            values.push_back({.kind = value_kind::constant, .type = current, .index = u32(_m.constants.size())});
            _m.constants.push_back(c);
        }
    }

    void read_names(llvm::block const& b)
    {
        for (auto const& e : b.entries)
        {
            if (e.is_block)
                continue;
            auto const& r = _s.records[e.index];
            auto const ops = _s.operands_of(r);
            // ENTRY: [value id, name...]; FNENTRY: [value id, offset, name...]
            if ((r.code != 1 && r.code != 3) || ops.size() < 2)
                continue;
            auto const id = u32(ops[0]);
            auto const name = string_of(ops.subspan(r.code == 3 ? 2 : 1));
            if (id >= u32(_m.values.size()))
                continue;
            auto const& v = _m.values[id];
            if (v.kind == value_kind::function)
                _m.functions[v.index].name = name;
            else if (v.kind == value_kind::global_variable)
                _m.globals[v.index].name = name;
        }
    }

    // ---- function bodies

    struct body_state
    {
        body* b = nullptr;
        cc::span<u64 const> ops;
        isize slot = 0;
        u32 next = 0;
        bool failed = false;
    };

    [[nodiscard]] u32 type_of_value(body const& b, u32 id) const
    {
        if (id < u32(_m.values.size()))
            return _m.values[id].type;
        auto const local = id - b.first_value;
        return local < u32(b.values.size()) ? b.values[local].type : k_none;
    }

    /// The next operand as a value id; where it refers forward and `typed`, the type that follows it.
    u32 value_operand(body_state& st, bool typed, u32* forward_type = nullptr)
    {
        if (st.slot >= st.ops.size())
        {
            st.failed = true;
            return 0;
        }
        auto const raw = st.ops[st.slot++];
        auto const id = _relative ? u32(st.next - u32(raw)) : u32(raw);
        if (typed && id >= st.next)
        {
            if (st.slot >= st.ops.size())
            {
                st.failed = true;
                return id;
            }
            auto const t = u32(st.ops[st.slot++]);
            if (forward_type != nullptr)
                *forward_type = t;
        }
        return id;
    }
    void push(body_state& st, u32 v, operand_role role)
    {
        st.b->operands.push_back(v);
        st.b->roles.push_back(role);
    }
    [[nodiscard]] u64 literal(body_state& st)
    {
        if (st.slot >= st.ops.size())
        {
            st.failed = true;
            return 0;
        }
        return st.ops[st.slot++];
    }

    cc::result<bool> read_body(llvm::block const& fb, u32 function_index)
    {
        auto& f = _m.functions[function_index];
        f.body = i32(_m.bodies.size());
        _m.bodies.push_back({.function = function_index, .first_value = u32(_m.values.size())});
        auto& b = _m.bodies.back();
        if (auto const* const ft = type_at(f.type); ft != nullptr && ft->kind == type_kind::function)
            for (auto k = u32(0); k < ft->member_count; ++k)
                b.values.push_back({.kind = value_kind::argument, .type = _m.members[ft->first_member + k], .index = k});
        auto const void_t = void_type();
        auto current_block = u32(0);
        auto last = i64(-1);
        auto st = body_state{.b = &b};
        for (auto const& e : fb.entries)
        {
            if (e.is_block)
            {
                if (_s.blocks[e.index].id == block_id::constants)
                    read_constants(_s.blocks[e.index], b.values);
                continue;
            }
            auto const& r = _s.records[e.index];
            auto const ops = _s.operands_of(r);
            if (r.code == 1) // DECLAREBLOCKS
            {
                b.block_count = ops.size() > 0 ? u32(ops[0]) : 0;
                continue;
            }
            if (r.code == 35) // DEBUG_LOC: [line, column, scope, inlined at]
            {
                if (last >= 0 && ops.size() >= 3)
                {
                    auto& i = b.instructions[last];
                    i.line = u32(ops[0]);
                    i.column = u32(ops[1]);
                    i.scope = u32(ops[2]);
                }
                continue;
            }
            if (r.code == 33) // DEBUG_LOC_AGAIN
            {
                if (last > 0)
                {
                    auto& i = b.instructions[last];
                    auto const& before = b.instructions[last - 1];
                    i.line = before.line;
                    i.column = before.column;
                    i.scope = before.scope;
                }
                continue;
            }
            st.ops = ops;
            st.slot = 0;
            st.next = b.first_value + u32(b.values.size());
            auto inst = instruction{.code = inst_code(r.code),
                                    .type = void_t,
                                    .first = u32(b.operands.size()),
                                    .block = current_block};
            auto gives = true;
            auto is_terminator = false;
            auto forward = k_none;
            switch (inst.code)
            {
            case inst_code::binop:
            {
                auto const a = value_operand(st, true, &forward);
                push(st, a, operand_role::value);
                push(st, value_operand(st, false), operand_role::value);
                inst.sub = u32(literal(st));
                if (st.slot < ops.size())
                    inst.flags = u32(literal(st));
                inst.type = forward != k_none ? forward : type_of_value(b, a);
                break;
            }
            case inst_code::cast:
                push(st, value_operand(st, true), operand_role::value);
                inst.type = u32(literal(st));
                inst.sub = u32(literal(st));
                break;
            case inst_code::vselect: // [true, false, condition]
            {
                auto const a = value_operand(st, true, &forward);
                push(st, a, operand_role::value);
                push(st, value_operand(st, false), operand_role::value);
                push(st, value_operand(st, true), operand_role::value);
                inst.type = forward != k_none ? forward : type_of_value(b, a);
                break;
            }
            case inst_code::extract_element:
            {
                auto const v = value_operand(st, true, &forward);
                push(st, v, operand_role::value);
                push(st, value_operand(st, true), operand_role::value);
                auto const* const t = type_at(forward != k_none ? forward : type_of_value(b, v));
                inst.type = t != nullptr ? t->element : k_none;
                break;
            }
            case inst_code::insert_element:
            {
                auto const v = value_operand(st, true, &forward);
                push(st, v, operand_role::value);
                push(st, value_operand(st, false), operand_role::value);
                push(st, value_operand(st, true), operand_role::value);
                inst.type = forward != k_none ? forward : type_of_value(b, v);
                break;
            }
            case inst_code::shuffle_vector:
            {
                auto const v = value_operand(st, true, &forward);
                push(st, v, operand_role::value);
                push(st, value_operand(st, false), operand_role::value);
                auto mask_type = k_none;
                auto const mask = value_operand(st, true, &mask_type);
                push(st, mask, operand_role::value);
                auto const* const vt = type_at(forward != k_none ? forward : type_of_value(b, v));
                auto const* const mt = type_at(mask_type != k_none ? mask_type : type_of_value(b, mask));
                inst.type
                    = vt != nullptr && mt != nullptr ? find_type(type_kind::vector, vt->element, mt->count) : k_none;
                break;
            }
            case inst_code::cmp:
            case inst_code::cmp2:
            {
                auto const a = value_operand(st, true, &forward);
                push(st, a, operand_role::value);
                push(st, value_operand(st, false), operand_role::value);
                inst.sub = u32(literal(st));
                if (st.slot < ops.size())
                    inst.flags = u32(literal(st));
                auto const i1 = find_type(type_kind::integer, 0, 0, 1);
                auto const* const t = type_at(forward != k_none ? forward : type_of_value(b, a));
                inst.type
                    = t != nullptr && t->kind == type_kind::vector ? find_type(type_kind::vector, i1, t->count) : i1;
                break;
            }
            case inst_code::ret:
                if (st.slot < ops.size())
                    push(st, value_operand(st, true), operand_role::value);
                gives = false;
                is_terminator = true;
                break;
            case inst_code::br:
                push(st, u32(literal(st)), operand_role::block);
                if (ops.size() >= 3)
                {
                    push(st, u32(literal(st)), operand_role::block);
                    push(st, value_operand(st, false), operand_role::value);
                }
                gives = false;
                is_terminator = true;
                break;
            case inst_code::switch_: // [type, condition, default, (case value id, block)...]
            {
                (void)literal(st);
                push(st, value_operand(st, false), operand_role::value);
                push(st, u32(literal(st)), operand_role::block);
                while (st.slot + 1 < ops.size())
                {
                    push(st, u32(literal(st)), operand_role::literal);
                    push(st, u32(literal(st)), operand_role::block);
                }
                gives = false;
                is_terminator = true;
                break;
            }
            case inst_code::unreachable:
                gives = false;
                is_terminator = true;
                break;
            case inst_code::phi: // [type, (signed relative value, block)...]
            {
                inst.type = u32(literal(st));
                while (st.slot + 1 < ops.size())
                {
                    auto const raw = literal(st);
                    auto const id = _relative ? u32(i64(st.next) - decode_signed(raw)) : u32(raw);
                    push(st, id, operand_role::value);
                    push(st, u32(literal(st)), operand_role::block);
                }
                break;
            }
            case inst_code::alloca_: // [allocated type, size type, size, alignment]
            {
                auto const allocated = u32(literal(st));
                (void)literal(st);
                push(st, u32(literal(st)), operand_role::value); // the size, as an absolute value id
                (void)literal(st);
                auto const pointer = find_type(type_kind::pointer, allocated);
                inst.type = pointer == k_none ? allocated : pointer;
                break;
            }
            case inst_code::load: // [pointer, type, alignment, volatile]
            {
                auto const p = value_operand(st, true, &forward);
                push(st, p, operand_role::value);
                if (st.slot + 2 < ops.size())
                    inst.type = u32(literal(st));
                else if (auto const* const t = type_at(forward != k_none ? forward : type_of_value(b, p)))
                    inst.type = t->element;
                break;
            }
            case inst_code::store: // [pointer, value, alignment, volatile]
                push(st, value_operand(st, true), operand_role::value);
                push(st, value_operand(st, true), operand_role::value);
                gives = false;
                break;
            case inst_code::extract_value:
            case inst_code::insert_value:
            {
                auto const agg = value_operand(st, true, &forward);
                push(st, agg, operand_role::value);
                auto const agg_type = forward != k_none ? forward : type_of_value(b, agg);
                if (inst.code == inst_code::insert_value)
                    push(st, value_operand(st, true), operand_role::value);
                auto t = agg_type;
                while (st.slot < ops.size())
                {
                    auto const index = literal(st);
                    push(st, u32(index), operand_role::literal);
                    auto const* const x = type_at(t);
                    if (x == nullptr)
                        break;
                    t = x->kind == type_kind::structure
                          ? (index < x->member_count ? _m.members[x->first_member + index] : k_none)
                          : x->element;
                }
                inst.type = inst.code == inst_code::insert_value ? agg_type : t;
                break;
            }
            case inst_code::gep: // [inbounds, source type, pointer, indices...]
            {
                (void)literal(st);
                (void)literal(st);
                auto const p = value_operand(st, true, &forward);
                push(st, p, operand_role::value);
                while (st.slot < ops.size())
                    push(st, value_operand(st, true), operand_role::value);
                // the result's pointee is the indexed element; its pointer's address space is the base's
                inst.type = forward != k_none ? forward : type_of_value(b, p);
                break;
            }
            case inst_code::call: // [attributes, calling convention, (fast-math), function type, callee, args...]
            {
                (void)literal(st);
                auto const cc = literal(st);
                if ((cc & (u64(1) << 17)) != 0)
                    (void)literal(st);
                auto fn_type = k_none;
                if ((cc & (u64(1) << 15)) != 0)
                    fn_type = u32(literal(st));
                auto const callee = value_operand(st, true, &forward);
                push(st, callee, operand_role::value);
                if (fn_type == k_none)
                    if (auto const* const p = type_at(forward != k_none ? forward : type_of_value(b, callee)))
                        fn_type = p->element;
                auto const* const ft = type_at(fn_type);
                auto const params = ft != nullptr ? ft->member_count : 0;
                for (auto k = u32(0); k < params && st.slot < ops.size(); ++k)
                {
                    auto const* const pt = type_at(_m.members[ft->first_member + k]);
                    if (pt != nullptr && pt->kind == type_kind::label)
                        push(st, u32(literal(st)), operand_role::block);
                    else if (pt != nullptr && pt->kind == type_kind::metadata)
                        push(st, u32(literal(st)), operand_role::literal);
                    else
                        push(st, value_operand(st, false), operand_role::value);
                }
                while (st.slot < ops.size())
                    push(st, value_operand(st, true), operand_role::value);
                inst.type = ft != nullptr ? ft->element : void_t;
                gives = inst.type != void_t;
                break;
            }
            case inst_code::atomic_rmw: // [pointer, value, operation, volatile, ordering, scope]
            {
                push(st, value_operand(st, true), operand_role::value);
                auto const v = value_operand(st, false);
                push(st, v, operand_role::value);
                inst.sub = u32(literal(st));
                inst.type = type_of_value(b, v);
                break;
            }
            case inst_code::cmpxchg: // [pointer, compare, new, ...]: a {value, i1}
            {
                push(st, value_operand(st, true), operand_role::value);
                auto const compare = value_operand(st, true, &forward);
                push(st, compare, operand_role::value);
                push(st, value_operand(st, false), operand_role::value);
                inst.type = k_none;
                break;
            }
            case inst_code::fence:
                gives = false;
                break;
            default:
                return cc::error(cc::format("llvm37: function {} holds an instruction of record code {}, which this "
                                            "reader does not read",
                                            f.name.empty() ? cc::format("{}", function_index) : f.name, r.code));
            }
            if (st.failed)
                return cc::error(cc::format("llvm37: an instruction of record code {} is short of its operands", r.code));
            inst.count = u32(b.operands.size()) - inst.first;
            if (gives)
            {
                inst.result = st.next;
                b.values.push_back({.kind = value_kind::instruction,
                                    .type = inst.type == k_none ? void_t : inst.type,
                                    .index = u32(b.instructions.size())});
            }
            if (inst.type == k_none)
                inst.type = void_t;
            b.instructions.push_back(inst);
            last = i64(b.instructions.size()) - 1;
            if (is_terminator)
                ++current_block;
        }
        return true;
    }
};
} // namespace

value module::value_of(body const& b, u32 id) const
{
    if (id < u32(values.size()))
        return values[id];
    auto const local = id - b.first_value;
    return local < u32(b.values.size()) ? b.values[local]
                                        : value{.kind = value_kind::constant, .type = 0, .index = 0xffffffffu};
}

cc::string_view module::callee_name(body const& b, instruction const& call) const
{
    if (call.code != inst_code::call || call.count == 0)
        return {};
    auto const v = value_of(b, b.operands[call.first]);
    return v.kind == value_kind::function ? cc::string_view(functions[v.index].name) : cc::string_view();
}

bool module::integer_of(u32 constant_index, i64& out) const
{
    if (constant_index >= u32(constants.size()))
        return false;
    auto const& c = constants[constant_index];
    if (c.code == 2) // NULL
    {
        out = 0;
        return true;
    }
    if (c.code != 4 || c.count < 1) // INTEGER, signed VBR
        return false;
    out = decode_signed(constant_operands[c.first]);
    return true;
}

cc::result<module> read(llvm::bitstream const& stream)
{
    auto m = module();
    auto r = module_reader(stream, m);
    if (auto result = r.run(); result.has_error())
        return cc::error(result.error().to_string());
    return m;
}
} // namespace babel::llvm37
