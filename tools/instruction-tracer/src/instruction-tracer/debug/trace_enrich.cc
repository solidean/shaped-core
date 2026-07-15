#include "trace_enrich.hh"

namespace itrace
{
namespace
{
/// True when control did not simply fall through to the next instruction.
bool diverged(recorded_instruction const& insn)
{
    if (insn.next_rip == 0) // the last record: it never retired a successor we saw
        return false;
    if (insn.length == 0) // undecoded: no fallthrough to compare against
        return false;

    return insn.next_rip != insn.rip + insn.length;
}
} // namespace

void enrich_trace(trace& t, symbol_session const& symbols, instruction_decoder const& decoder, bool want_source)
{
    decoder.decode(t.instructions);

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

        // Only symbolize where control actually went somewhere — a fallthrough target is the next
        // line of output anyway, and a lookup per instruction is not free.
        if (diverged(insn))
            insn.target_symbol = symbols.describe(insn.next_rip);
    }
}
} // namespace itrace
