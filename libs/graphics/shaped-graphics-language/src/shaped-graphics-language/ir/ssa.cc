#include "ssa.hh"

#include <shaped-graphics-language/ir/ir.hh>

using namespace sgl;
using namespace sgl::ir;

// Braun, Buchwald, Hack, Leißa, Mallon, Zwinkau: "Simple and Efficient Construction of Static Single Assignment Form".
// A definition is looked up per block as a read needs it, so a phi stands only where a read reaches two definitions.
// Blocks are visited in the order they opened, a dominance order, so every predecessor but a back edge's is done.

namespace
{
constexpr u32 k_not_slot = 0xffffffffu;
/// A value that is a phi of this pass, which has no place in `instructions` until the blocks are laid out.
constexpr u32 k_phi_bit = 0x10000000u;
/// More scalars than this in one local are a table, which a variable holds better than one value per scalar does.
constexpr u32 k_most_slots = 4096;

[[nodiscard]] constexpr bool is_phi(value_ref v)
{
    return is_valid(v) && (u32(v) & (k_constant_bit | k_global_bit | k_local_bit | k_phi_bit)) == k_phi_bit;
}

class builder
{
public:
    explicit builder(program& p) : _p(p) {}

    void run()
    {
        find_promoted();
        if (_slot_types.empty())
            return;
        find_predecessors();
        _result.resize_to_filled(_p.instructions.size(), value_ref::none);
        _chain_slots.resize_to_filled(_p.instructions.size(), k_not_slot);
        _chain_types.resize_to_filled(_p.instructions.size(), type_ref::none);
        _new_first.resize_to_filled(_p.blocks.size(), 0u);
        _new_count.resize_to_filled(_p.blocks.size(), 0u);
        _filled.resize_to_filled(_p.blocks.size(), false);
        _sealed.resize_to_filled(_p.blocks.size(), false);
        _phis_of.resize_to_filled(_p.blocks.size(), 0u);
        _definitions.resize_to_filled(1024, {});

        auto const order = blocks_in_order(_p);
        for (auto const b : order)
            maybe_seal(b);
        for (auto const b : order)
        {
            _open = b;
            _new_first[isize(b)] = u32(_body.size());
            auto const& block = _p.blocks[isize(b)];
            for (auto i = block.first; i < block.first + block.count; ++i)
                visit(i);
            _new_count[isize(b)] = u32(_body.size()) - _new_first[isize(b)];
            _filled[isize(b)] = true;
            for (auto const to : successors_of(_p, _p.instructions[block.first + block.count - 1], _scratch))
                maybe_seal(block_ref(to));
        }
        for (auto const b : order)
            maybe_seal(b);
        drop_trivial_phis();
        lay_out(order);
    }

private:
    [[nodiscard]] u32 leaves_of(type_ref t)
    {
        if (isize(t) >= _leaves.size())
            _leaves.resize_to_filled(_p.types.size(), 0u);
        if (auto const known = _leaves[isize(t)]; known != 0)
            return known;
        auto const& info = _p.at(t);
        auto count = u32(1);
        switch (info.kind)
        {
        case type_kind::vector:
        case type_kind::matrix:
        case type_kind::array:
        {
            auto const each = leaves_of(info.element);
            count = each >= k_most_slots ? k_most_slots : each * info.count;
            break;
        }
        case type_kind::structure:
            count = 0;
            for (auto i = u32(0); i < info.member_count; ++i)
                count += leaves_of(_p.members[info.first_member + i].type);
            break;
        default:
            break;
        }
        _leaves[isize(t)] = count;
        return count;
    }

    /// Whether a value of `t` is made of scalars alone, which every local a program holds is.
    [[nodiscard]] bool is_splittable(type_ref t)
    {
        auto const& info = _p.at(t);
        switch (info.kind)
        {
        case type_kind::bool_:
        case type_kind::int_:
        case type_kind::float_:
            return true;
        case type_kind::vector:
        case type_kind::matrix:
        case type_kind::array:
            return is_splittable(info.element);
        case type_kind::structure:
            for (auto i = u32(0); i < info.member_count; ++i)
                if (!is_splittable(_p.members[info.first_member + i].type))
                    return false;
            return true;
        default:
            return false;
        }
    }

    void find_promoted()
    {
        auto const count = _p.locals.size();
        auto is_promoted = cc::vector<bool>::create_filled(count, true);
        // the local a chain reaches into, by the chain's position
        auto chains = cc::vector<u32>::create_filled(_p.instructions.size(), k_not_slot);

        for (auto l = isize(0); l < count; ++l)
        {
            auto const value = _p.at(_p.locals[l].type).element;
            if (!is_splittable(value) || leaves_of(value) >= k_most_slots)
                is_promoted[l] = false;
        }
        auto const reject = [&](value_ref v)
        {
            if (is_local(v))
                is_promoted[index_of(v)] = false;
            else if (is_valid(v) && !is_constant(v) && !is_global(v) && chains[index_of(v)] != k_not_slot)
                is_promoted[chains[index_of(v)]] = false;
        };
        for (auto i = isize(0); i < _p.instructions.size(); ++i)
        {
            auto const& inst = _p.instructions[i];
            auto const operands = _p.operands_of(inst);
            if (inst.code == op::access_chain && is_local(value_ref(operands[0])))
            {
                chains[i] = index_of(value_ref(operands[0]));
                for (auto k = isize(1); k < operands.size(); ++k)
                    if (!is_constant(value_ref(operands[k])))
                        is_promoted[chains[i]] = false;
                continue;
            }
            for (auto k = isize(0); k < operands.size(); ++k)
            {
                if (operand_role(inst.code, k) != operand::value)
                    continue;
                auto const is_address = k == 0 && (inst.code == op::load || inst.code == op::store);
                if (!is_address)
                    reject(value_ref(operands[k]));
            }
        }

        _local_slots.resize_to_filled(count, k_not_slot);
        for (auto l = isize(0); l < count; ++l)
            if (is_promoted[l])
            {
                _local_slots[l] = u32(_slot_types.size());
                add_slots(_p.at(_p.locals[l].type).element);
            }
    }

    void add_slots(type_ref t)
    {
        auto const& info = _p.at(t);
        switch (info.kind)
        {
        case type_kind::vector:
        case type_kind::matrix:
        case type_kind::array:
            for (auto i = u32(0); i < info.count; ++i)
                add_slots(info.element);
            break;
        case type_kind::structure:
            for (auto i = u32(0); i < info.member_count; ++i)
                add_slots(_p.members[info.first_member + i].type);
            break;
        default:
            _slot_types.push_back(t);
            break;
        }
    }

    void find_predecessors()
    {
        _pred_first.resize_to_filled(_p.blocks.size() + 1, 0u);
        // a pair per edge, then each block's predecessors as one run
        struct edge
        {
            u32 to;
            u32 from;
        };
        auto edges = cc::vector<edge>();
        for (auto b = isize(0); b < _p.blocks.size(); ++b)
        {
            auto const& block = _p.blocks[b];
            if (block.count == 0)
                continue;
            for (auto const to : successors_of(_p, _p.instructions[block.first + block.count - 1], _scratch))
                edges.push_back({.to = to, .from = u32(b)});
        }
        auto counts = cc::vector<u32>::create_filled(_p.blocks.size() + 1, 0u);
        for (auto const& e : edges)
            ++counts[e.to];
        auto at = u32(0);
        for (auto b = isize(0); b < _p.blocks.size(); ++b)
        {
            _pred_first[b] = at;
            at += counts[b];
        }
        _pred_first[_p.blocks.size()] = at;
        _preds.resize_to_filled(at, 0u);
        auto cursor = cc::vector<u32>::create_copy_of(cc::span<u32 const>(_pred_first));
        for (auto const& e : edges)
        {
            // an edge taken twice, as a branch with both arms alike, is one predecessor
            auto is_known = false;
            for (auto k = _pred_first[e.to]; k < cursor[e.to]; ++k)
                is_known = is_known || _preds[k] == e.from;
            if (!is_known)
                _preds[cursor[e.to]++] = e.from;
        }
        _pred_count = cc::vector<u32>::create_filled(_p.blocks.size(), 0u);
        for (auto b = isize(0); b < _p.blocks.size(); ++b)
            _pred_count[b] = cursor[b] - _pred_first[b];
    }

    [[nodiscard]] cc::span<u32 const> preds_of(block_ref b) const
    {
        return cc::span<u32 const>(_preds.data() + _pred_first[isize(b)], isize(_pred_count[isize(b)]));
    }

    // ---- definitions, an open table of (slot, block) to a value

    struct definition
    {
        u64 key = ~u64(0);
        value_ref value = value_ref::none;
    };

    [[nodiscard]] static constexpr u64 key_of(u32 slot, block_ref b) { return (u64(slot) << 32) | u64(u32(b)); }

    [[nodiscard]] isize find(u64 key) const
    {
        auto const mask = _definitions.size() - 1;
        auto at = isize(((key >> 32) * 0x9e3779b1u) ^ (key * 0x85ebca6bu)) & mask;
        while (_definitions[at].key != key && _definitions[at].key != ~u64(0))
            at = (at + 1) & mask;
        return at;
    }

    void write(u32 slot, block_ref b, value_ref v)
    {
        auto const key = key_of(slot, b);
        auto at = find(key);
        if (_definitions[at].key == ~u64(0))
        {
            if ((_definition_count + 1) * 2 > _definitions.size())
            {
                auto old = cc::move(_definitions);
                _definitions.resize_to_filled(old.size() * 2, {});
                for (auto const& d : old)
                    if (d.key != ~u64(0))
                        _definitions[find(d.key)] = d;
                at = find(key);
            }
            ++_definition_count;
        }
        _definitions[at] = {.key = key, .value = v};
    }

    [[nodiscard]] value_ref undefined(type_ref t)
    {
        for (auto const& known : _undefined)
            if (known.type == t)
                return known.value;
        _p.constants.push_back({.type = t, .is_undefined = true});
        auto const value = constant_at(u32(_p.constants.size() - 1));
        _undefined.push_back({.type = t, .value = value});
        return value;
    }

    [[nodiscard]] value_ref new_phi(u32 slot, block_ref b)
    {
        auto const count = _pred_count[isize(b)];
        _phis.push_back({.block = b,
                         .type = _slot_types[slot],
                         .first = u32(_phi_operands.size()),
                         .count = count,
                         .replaced_by = value_ref::none});
        for (auto i = u32(0); i < count; ++i)
            _phi_operands.push_back(u32(value_ref::none));
        ++_phis_of[isize(b)];
        return value_ref(u32(_phis.size() - 1) | k_phi_bit);
    }

    void fill_phi(u32 slot, block_ref b, value_ref phi)
    {
        auto const first = _phis[index_of_phi(phi)].first;
        auto const preds = preds_of(b);
        for (auto i = isize(0); i < preds.size(); ++i)
        {
            auto const incoming = read(slot, block_ref(preds[i]));
            _phi_operands[first + u32(i)] = u32(incoming);
        }
    }

    [[nodiscard]] static constexpr u32 index_of_phi(value_ref v) { return u32(v) & ~k_phi_bit; }

    [[nodiscard]] value_ref read(u32 slot, block_ref b)
    {
        if (auto const at = find(key_of(slot, b)); _definitions[at].key != ~u64(0))
            return _definitions[at].value;

        auto result = value_ref::none;
        if (!_sealed[isize(b)])
        {
            result = new_phi(slot, b);
            _incomplete.push_back({.block = b, .slot = slot, .phi = result});
            write(slot, b, result);
            return result;
        }
        auto const preds = preds_of(b);
        if (preds.empty())
            result = undefined(_slot_types[slot]);
        else if (preds.size() == 1)
            result = read(slot, block_ref(preds[0]));
        else
        {
            // written first, so a cycle through this block ends here
            result = new_phi(slot, b);
            write(slot, b, result);
            fill_phi(slot, b, result);
            return result;
        }
        write(slot, b, result);
        return result;
    }

    void maybe_seal(block_ref b)
    {
        if (_sealed[isize(b)])
            return;
        for (auto const from : preds_of(b))
            if (!_filled[from])
                return;
        _sealed[isize(b)] = true;
        // a read below may add to the list, for another block
        for (auto i = isize(0); i < _incomplete.size(); ++i)
            if (_incomplete[i].block == b)
            {
                auto const held = _incomplete[i];
                _incomplete[i] = _incomplete.back();
                _incomplete.pop_back();
                --i;
                fill_phi(held.slot, b, held.phi);
            }
    }

    // ---- rewriting one operation at a time

    [[nodiscard]] value_ref emit(op code, type_ref type, cc::span<u32 const> operands)
    {
        auto const first = u32(_operands.size());
        for (auto const o : operands)
            _operands.push_back(o);
        auto copy = *_current;
        copy.code = code;
        copy.sub = 0;
        copy.aux = 0;
        copy.type = type;
        copy.first = first;
        copy.count = u32(operands.size());
        _body.push_back(copy);
        return value_ref(u32(_body.size() - 1));
    }

    /// The value of `t` held in the slots from `slot` on, which the caller's cursor moves past.
    [[nodiscard]] value_ref read_whole(type_ref t, u32& slot)
    {
        auto const info = _p.at(t);
        auto parts = cc::vector<u32>();
        switch (info.kind)
        {
        case type_kind::vector:
        case type_kind::matrix:
        case type_kind::array:
            for (auto i = u32(0); i < info.count; ++i)
                parts.push_back(u32(read_whole(info.element, slot)));
            break;
        case type_kind::structure:
            for (auto i = u32(0); i < info.member_count; ++i)
                parts.push_back(u32(read_whole(_p.members[info.first_member + i].type, slot)));
            break;
        default:
            return read(slot++, _open);
        }
        return emit(op::construct, t, parts);
    }

    void write_whole(type_ref t, value_ref value, u32& slot)
    {
        auto const info = _p.at(t);
        auto const part = [&](type_ref held, u32 index)
        {
            u32 const operands[] = {u32(value), index};
            write_whole(held, emit(op::extract, held, operands), slot);
        };
        switch (info.kind)
        {
        case type_kind::vector:
        case type_kind::matrix:
        case type_kind::array:
            for (auto i = u32(0); i < info.count; ++i)
                part(info.element, i);
            break;
        case type_kind::structure:
            for (auto i = u32(0); i < info.member_count; ++i)
                part(_p.members[info.first_member + i].type, i);
            break;
        default:
            write(slot++, _open, value);
            break;
        }
    }

    /// Where `chain` lands in a local's slots, and the type it lands on.
    void walk_chain(cc::span<u32 const> chain, u32& slot, type_ref& type)
    {
        for (auto const raw : chain)
        {
            auto const index = _p.constants[index_of(value_ref(raw))].low;
            auto const& info = _p.at(type);
            if (info.kind == type_kind::structure)
            {
                for (auto i = u32(0); i < index; ++i)
                    slot += leaves_of(_p.members[info.first_member + i].type);
                type = _p.members[info.first_member + index].type;
            }
            else
            {
                slot += index * leaves_of(info.element);
                type = info.element;
            }
        }
    }

    [[nodiscard]] value_ref translated(u32 raw) const
    {
        auto const v = value_ref(raw);
        if (!is_valid(v) || is_constant(v) || is_global(v))
            return v;
        return is_local(v) ? v : _result[index_of(v)];
    }

    void visit(u32 index)
    {
        auto const& inst = _p.instructions[index];
        _current = &inst;
        auto const operands = _p.operands_of(inst);

        if (inst.code == op::access_chain && is_local(value_ref(operands[0]))
            && _local_slots[index_of(value_ref(operands[0]))] != k_not_slot)
        {
            auto slot = _local_slots[index_of(value_ref(operands[0]))];
            auto type = _p.at(_p.locals[index_of(value_ref(operands[0]))].type).element;
            walk_chain(operands.subspan(1), slot, type);
            _chain_slots[index] = slot;
            _chain_types[index] = type;
            return;
        }
        if (inst.code == op::load || inst.code == op::store)
        {
            auto const pointer = value_ref(operands[0]);
            auto slot = k_not_slot;
            auto type = type_ref::none;
            if (is_local(pointer) && _local_slots[index_of(pointer)] != k_not_slot)
            {
                slot = _local_slots[index_of(pointer)];
                type = _p.at(_p.locals[index_of(pointer)].type).element;
            }
            else if (is_valid(pointer) && !is_constant(pointer) && !is_global(pointer) && !is_local(pointer)
                     && _chain_slots[index_of(pointer)] != k_not_slot)
            {
                slot = _chain_slots[index_of(pointer)];
                type = _chain_types[index_of(pointer)];
            }
            if (slot != k_not_slot)
            {
                if (inst.code == op::load)
                    _result[index] = read_whole(type, slot);
                else
                    write_whole(type, translated(operands[1]), slot);
                return;
            }
        }

        // a phi of the program comes after the definitions it joins are known, a back edge's among them
        auto const is_program_phi = inst.code == op::phi;
        auto const first = u32(_operands.size());
        for (auto k = isize(0); k < operands.size(); ++k)
        {
            auto const is_value = operand_role(inst.code, k) == operand::value;
            _operands.push_back(is_value && !is_program_phi ? u32(translated(operands[k])) : operands[k]);
        }
        auto copy = inst;
        copy.first = first;
        _body.push_back(copy);
        _result[index] = value_ref(u32(_body.size() - 1));
    }

    // ---- what is left, in place

    [[nodiscard]] value_ref resolved(value_ref v) const
    {
        while (is_phi(v) && is_valid(_phis[index_of_phi(v)].replaced_by))
            v = _phis[index_of_phi(v)].replaced_by;
        return v;
    }

    void drop_trivial_phis()
    {
        auto changed = true;
        while (changed)
        {
            changed = false;
            for (auto p = isize(0); p < _phis.size(); ++p)
            {
                auto& phi = _phis[p];
                if (is_valid(phi.replaced_by))
                    continue;
                auto const self = value_ref(u32(p) | k_phi_bit);
                auto unique = value_ref::none;
                auto is_trivial = true;
                for (auto i = u32(0); i < phi.count && is_trivial; ++i)
                {
                    auto const r = resolved(value_ref(_phi_operands[phi.first + i]));
                    if (r == self || r == unique)
                        continue;
                    if (is_valid(unique))
                        is_trivial = false;
                    unique = r;
                }
                if (!is_trivial)
                    continue;
                phi.replaced_by = is_valid(unique) ? unique : undefined(phi.type);
                changed = true;
            }
        }
    }

    void lay_out(cc::span<block_ref const> order)
    {
        // where each kept phi and each operation lands
        auto phi_place = cc::vector<u32>::create_filled(_phis.size(), 0u);
        auto body_place = cc::vector<u32>::create_filled(_body.size(), 0u);
        auto phis_by_block = cc::vector<u32>::create_filled(_p.blocks.size() + 1, 0u);
        for (auto const& phi : _phis)
            if (!is_valid(phi.replaced_by))
                ++phis_by_block[isize(phi.block)];
        auto first_phi = cc::vector<u32>::create_filled(_p.blocks.size() + 1, 0u);
        auto total = u32(0);
        for (auto b = isize(0); b < _p.blocks.size(); ++b)
        {
            first_phi[b] = total;
            total += phis_by_block[b];
        }
        auto phi_order = cc::vector<u32>::create_filled(total, 0u);
        auto cursor = cc::vector<u32>::create_copy_of(cc::span<u32 const>(first_phi));
        for (auto i = isize(0); i < _phis.size(); ++i)
            if (!is_valid(_phis[i].replaced_by))
                phi_order[cursor[isize(_phis[i].block)]++] = u32(i);

        auto at = u32(0);
        for (auto const b : order)
        {
            for (auto k = first_phi[isize(b)]; k < first_phi[isize(b)] + phis_by_block[isize(b)]; ++k)
                phi_place[phi_order[k]] = at++;
            for (auto i = _new_first[isize(b)]; i < _new_first[isize(b)] + _new_count[isize(b)]; ++i)
                body_place[i] = at++;
        }

        auto const final_of = [&](value_ref v) -> u32
        {
            v = resolved(v);
            if (!is_valid(v) || is_constant(v) || is_global(v) || is_local(v))
                return u32(v);
            return is_phi(v) ? phi_place[index_of_phi(v)] : body_place[index_of(v)];
        };

        auto instructions = cc::vector<instruction>::create_with_capacity(isize(at));
        auto operands = cc::vector<u32>::create_with_capacity(_operands.size() + _phi_operands.size() * 2);
        auto blocks = cc::vector<ir::block>::create_copy_of(cc::span<ir::block const>(_p.blocks));
        auto local_place = cc::vector<u32>::create_filled(_p.locals.size(), 0u);
        auto locals = cc::vector<ir::local>();
        for (auto l = isize(0); l < _p.locals.size(); ++l)
            if (_local_slots[l] == k_not_slot)
            {
                local_place[l] = u32(locals.size());
                locals.push_back(_p.locals[l]);
            }
        auto const moved_local = [&](u32 raw) -> u32
        {
            auto const v = value_ref(raw);
            return is_local(v) ? u32(local_at(local_place[index_of(v)])) : raw;
        };

        for (auto const b : order)
        {
            auto const start = u32(instructions.size());
            for (auto k = first_phi[isize(b)]; k < first_phi[isize(b)] + phis_by_block[isize(b)]; ++k)
            {
                auto const& phi = _phis[phi_order[k]];
                auto const preds = preds_of(b);
                auto const first = u32(operands.size());
                for (auto i = u32(0); i < phi.count; ++i)
                {
                    operands.push_back(moved_local(final_of(value_ref(_phi_operands[phi.first + i]))));
                    operands.push_back(preds[i]);
                }
                instructions.push_back({.code = op::phi, .type = phi.type, .first = first, .count = phi.count * 2});
            }
            for (auto i = _new_first[isize(b)]; i < _new_first[isize(b)] + _new_count[isize(b)]; ++i)
            {
                auto copy = _body[i];
                auto const first = u32(operands.size());
                auto const held = cc::span<u32 const>(_operands.data() + copy.first, isize(copy.count));
                for (auto k = isize(0); k < held.size(); ++k)
                {
                    if (operand_role(copy.code, k) != operand::value)
                    {
                        operands.push_back(held[k]);
                        continue;
                    }
                    // a phi of the program still speaks of the old positions
                    auto const v = copy.code == op::phi ? translated(held[k]) : value_ref(held[k]);
                    operands.push_back(moved_local(final_of(v)));
                }
                copy.first = first;
                instructions.push_back(copy);
            }
            blocks[isize(b)].first = start;
            blocks[isize(b)].count = u32(instructions.size()) - start;
        }
        _p.instructions = cc::move(instructions);
        _p.operands = cc::move(operands);
        _p.blocks = cc::move(blocks);
        _p.locals = cc::move(locals);
    }

    struct phi_node
    {
        block_ref block = block_ref::none;
        type_ref type = type_ref::none;
        /// A range of `_phi_operands`, parallel to the block's predecessors.
        u32 first = 0;
        u32 count = 0;
        value_ref replaced_by = value_ref::none;
    };
    struct incomplete_phi
    {
        block_ref block = block_ref::none;
        u32 slot = 0;
        value_ref phi = value_ref::none;
    };
    struct undefined_value
    {
        type_ref type = type_ref::none;
        value_ref value = value_ref::none;
    };

    program& _p;
    instruction const* _current = nullptr;
    block_ref _open = block_ref::none;

    cc::vector<u32> _leaves;
    /// Parallel to `_p.locals`: its first slot, or `k_not_slot` for a local that stays one.
    cc::vector<u32> _local_slots;
    cc::vector<type_ref> _slot_types;
    /// Parallel to `_p.instructions`: what each became, and where a chain into a promoted local landed.
    cc::vector<value_ref> _result;
    cc::vector<u32> _chain_slots;
    cc::vector<type_ref> _chain_types;

    cc::vector<u32> _pred_first;
    cc::vector<u32> _pred_count;
    cc::vector<u32> _preds;
    cc::vector<u32> _scratch;

    cc::vector<bool> _filled;
    cc::vector<bool> _sealed;
    cc::vector<u32> _phis_of;
    cc::vector<definition> _definitions;
    isize _definition_count = 0;
    cc::vector<undefined_value> _undefined;

    cc::vector<phi_node> _phis;
    cc::vector<u32> _phi_operands;
    cc::vector<incomplete_phi> _incomplete;

    cc::vector<instruction> _body;
    cc::vector<u32> _operands;
    cc::vector<u32> _new_first;
    cc::vector<u32> _new_count;
};
} // namespace

void sgl::ir::construct_ssa(program& p)
{
    builder(p).run();
}
