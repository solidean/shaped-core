#include "trace_enrich.hh"

#include <clean-core/string/format.hh>
#include <instruction-tracer/decode/memory_access.hh>

namespace itrace
{
namespace
{
/// Resolves the function containing an address, reusing the last hit. Traces run mostly straight
/// through one function, so a single-entry range cache turns a lookup per instruction into a lookup
/// per function. Symbols whose extent the PDB withholds (size 0) simply never cache.
class owner_cache
{
public:
    explicit owner_cache(symbol_session const& symbols) : _symbols(symbols) {}

    cc::string const& owner_of(u64 address)
    {
        if (_size != 0 && address >= _start && address < _start + _size)
            return _name;

        auto const sym = _symbols.symbol_at(address);
        if (!sym.has_value())
        {
            // No symbol: fall back to describe's module+offset / bare-address form. Never cached —
            // each such address is its own row, which is the honest answer.
            _size = 0;
            _name = _symbols.describe(address);
            return _name;
        }

        auto const& s = sym.value();
        _start = s.address;
        _size = s.size;
        _name = s.module.empty() ? s.name : cc::format("{}!{}", s.module, s.name);
        return _name;
    }

private:
    symbol_session const& _symbols;
    u64 _start = 0;
    u64 _size = 0;
    cc::string _name;
};

/// A data symbol whose *extent* covers `address` — a global, a static. Empty when the address is
/// inside no known symbol: unlike code, SymFromAddr's nearest-preceding match is meaningless for a
/// stack or heap address, so we demand a real containment before naming it.
cc::string data_symbol_at(symbol_session const& symbols, u64 address)
{
    auto const sym = symbols.symbol_at(address);
    if (!sym.has_value())
        return {};

    auto const& s = sym.value();
    if (s.size == 0 || address < s.address || address >= s.address + s.size)
        return {};

    return s.module.empty() ? s.name : cc::format("{}!{}", s.module, s.name);
}

/// The function containing `address`, as "module!name" (no offset). Falls back to describe's
/// module+offset / bare-address form when the PDB has no symbol there.
cc::string function_name_at(symbol_session const& symbols, u64 address)
{
    auto const sym = symbols.symbol_at(address);
    if (!sym.has_value())
        return symbols.describe(address);

    auto const& s = sym.value();
    return s.module.empty() ? s.name : cc::format("{}!{}", s.module, s.name);
}

/// Frames active at the current instruction, outermost first. Each entry is the return-address-slot
/// rsp of a frame (its stack memory grows down from there) and the function that owns it.
struct frame_stack
{
    cc::vector<u64> bases;
    cc::vector<cc::string> owners;

    void push(u64 base, cc::string owner)
    {
        bases.push_back(base);
        owners.push_back(cc::move(owner));
    }

    void pop()
    {
        // Never drop the outermost: the trace stops when it returns, and a stray ret in between
        // (e.g. a mismatched frame from a tail-call jmp we did not track) must not underflow.
        if (bases.size() > 1)
        {
            bases.remove_back();
            owners.remove_back();
        }
    }
};

/// Fill `insn.memory_accesses` for one instruction: every data operand plus the instruction fetch,
/// each classified into a region and symbolized. `regs` is the state before `insn` ran.
void fill_accesses(recorded_instruction& insn,
                   register_snapshot const& regs,
                   trace const& t,
                   frame_stack const& frames,
                   symbol_session const& symbols)
{
    for (auto const& op : decode_memory_operands(insn, regs))
    {
        memory_access acc;
        acc.address = op.address;
        acc.size = op.size;
        acc.is_read = op.is_read;
        acc.is_write = op.is_write;

        isize owner = -1;
        acc.region = classify_region(op.address, t.stack_low, t.stack_high, frames.bases, owner);

        if (acc.region == access_region::heap)
            acc.symbol = data_symbol_at(symbols, op.address);
        else if (owner >= 0)
            acc.symbol = frames.owners[owner];

        insn.memory_accesses.push_back(cc::move(acc));
    }

    // The instruction fetch itself — code memory, so an I-cache footprint once the `instructions`
    // region is opted in. Charged to the function it lives in.
    if (insn.length > 0)
    {
        memory_access fetch;
        fetch.address = insn.rip;
        fetch.size = insn.length;
        fetch.is_read = true;
        fetch.region = access_region::instructions;
        fetch.symbol = insn.owner_symbol;
        insn.memory_accesses.push_back(cc::move(fetch));
    }
}

void enrich_memory(trace& t, symbol_session const& symbols)
{
    // Needs the before-instruction register snapshots; without them no address can be resolved.
    if (t.registers.size() < t.instructions.size())
        return;

    frame_stack frames;
    if (!t.instructions.empty())
        frames.push(t.instructions.front().rsp, t.entry_symbol);

    for (isize i = 0; i < t.instructions.size(); ++i)
    {
        auto& insn = t.instructions[i];

        // Classify against the frames as they stand while this instruction runs — a `call` executes
        // in the caller's frame, so the callee is pushed only afterwards.
        fill_accesses(insn, t.registers[i], t, frames, symbols);

        if (insn.category == insn_category::call && i + 1 < t.instructions.size())
        {
            // The callee's frame base is the return-address slot: rsp at its first instruction.
            frames.push(t.instructions[i + 1].rsp, function_name_at(symbols, insn.next_rip));
        }
        else if (insn.category == insn_category::ret)
        {
            frames.pop();
        }
    }
}
} // namespace

void enrich_trace(trace& t,
                  symbol_session const& symbols,
                  instruction_decoder const& decoder,
                  bool want_source,
                  bool want_owner,
                  bool want_memory)
{
    decoder.decode(t.instructions);

    owner_cache owners(symbols);

    for (auto& insn : t.instructions)
    {
        if (want_source)
        {
            if (auto const line = symbols.line_at(insn.rip); line.has_value())
            {
                insn.file = line.value().file;
                insn.line = line.value().line;
            }
        }

        // The stats table and the memory-fetch attribution both charge instructions to their
        // containing function; the plain trace never prints it, and a lookup per instruction is not
        // free.
        if (want_owner || want_memory)
            insn.owner_symbol = owners.owner_of(insn.rip);

        // Only symbolize where control actually went somewhere — a fallthrough target is the next
        // line of output anyway, and a lookup per instruction is not free.
        if (diverged(insn))
            insn.target_symbol = symbols.describe(insn.next_rip);
    }

    if (want_memory)
        enrich_memory(t, symbols);
}
} // namespace itrace
