#include "trace_enrich.hh"

#include <clean-core/string/format.hh>

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
} // namespace

void enrich_trace(trace& t, symbol_session const& symbols, instruction_decoder const& decoder, bool want_source, bool want_owner)
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

        // Only the stats table charges instructions to their containing function; the trace itself
        // never prints it, and a lookup per instruction is not free.
        if (want_owner)
            insn.owner_symbol = owners.owner_of(insn.rip);

        // Only symbolize where control actually went somewhere — a fallthrough target is the next
        // line of output anyway, and a lookup per instruction is not free.
        if (diverged(insn))
            insn.target_symbol = symbols.describe(insn.next_rip);
    }
}
} // namespace itrace
