#include "trace_formatter.hh"

#include <clean-core/string/format.hh>
#include <instruction-tracer/report/console.hh>

namespace itrace
{
namespace
{
cc::string_view reason_text(step_reason reason)
{
    switch (reason)
    {
    case step_reason::instruction_budget:
        return "instruction budget reached (--instructions)";
    case step_reason::returned:
        return "original stack frame returned";
    case step_reason::syscall:
        return "syscall boundary (stopped before entering the kernel)";
    case step_reason::exception:
        return "the debuggee raised an exception";
    case step_reason::process_exited:
        return "the debuggee exited mid-trace";
    }
    return "unknown";
}

cc::string hex_bytes(recorded_instruction const& insn)
{
    // Only the bytes the decoder claimed; the record always holds a full 15-byte window.
    auto const n = insn.length > 0 ? insn.length : insn.byte_count;

    cc::string out;
    for (u8 i = 0; i < n; ++i)
        cc::format_append(out, "{}{:02x}", i == 0 ? "" : " ", insn.bytes[isize(i)]);
    return out;
}

/// Pad from `visible` columns out to `width`, or a single space if already past it. Takes the
/// visible width rather than measuring `out`, whose bytes may include invisible color codes.
void pad_to(cc::string& out, isize visible, isize width)
{
    for (isize i = 0, n = cc::max<isize>(width - visible, 1); i < n; ++i)
        out += ' ';
}

void append_branch_note(cc::string& out, recorded_instruction const& insn)
{
    if (insn.length == 0 || insn.next_rip == 0)
        return;

    bool const fell_through = insn.next_rip == insn.rip + insn.length;
    auto const target = insn.target_symbol.empty() ? cc::string() : cc::format(" -> {}", cyan(insn.target_symbol));

    switch (insn.category)
    {
    case insn_category::conditional_branch:
        // The one case where "not taken" is worth saying: the branch had a choice.
        if (fell_through)
            out += dim("  ; not taken");
        else
            out += yellow("  ; taken") + target;
        break;

    case insn_category::call:
    case insn_category::unconditional_branch:
    case insn_category::ret:
        // Where an indirect call/jmp/ret actually landed is the interesting part, and static
        // disassembly cannot know it.
        if (!insn.target_symbol.empty())
            out += dim("  ;") + target;
        break;

    default:
        break;
    }
}

void append_register_diff(cc::string& out, register_snapshot const& before, register_snapshot const& after)
{
    cc::string diff;
    for (int i = 0; i < gpr_count; ++i)
    {
        if (before.gpr[i] == after.gpr[i])
            continue;

        cc::format_append(diff, "{}{}={:#x}", diff.empty() ? "" : " ", gpr_names[i], after.gpr[i]);
    }

    if (!diff.empty())
        out += dim("  ; ") + green(diff);
}
} // namespace

cc::string format_address(u64 address)
{
    return cc::format("{:08x}`{:08x}", u32(address >> 32), u32(address));
}

cc::string format_instruction(recorded_instruction const& insn)
{
    cc::string out;
    out += dim(cc::format("  {}  ", format_address(insn.rip)));

    // No decode: the bytes are all we can honestly show.
    out += insn.text.empty() ? dim(cc::format("({})", hex_bytes(insn))) : insn.text;

    append_branch_note(out, insn);
    return out;
}

cc::string format_trace(trace const& t, u32 total_traces, format_options const& opts, source_cache& sources)
{
    cc::string out;

    out += bold(cc::format("=== trace {}/{}: {} ===", t.index, total_traces, t.entry_symbol)) + "\n";
    cc::format_append(out, "{} {}\n", dim("thread:"), t.thread_id);
    cc::format_append(out, "{}    {}\n", dim("hit:"), t.hit_index);
    cc::format_append(out, "{}  {}\n", dim("entry:"), cyan(t.entry_symbol));
    if (!t.return_symbol.empty())
        cc::format_append(out, "{} {}\n", dim("return:"), cyan(t.return_symbol));

    if (opts.stack && !t.entry_stack.empty())
    {
        out += "\n" + dim("stack:") + "\n";
        for (auto const& f : t.entry_stack)
        {
            auto const name = f.symbol.empty() ? format_address(f.rip) : f.symbol;
            out += "  " + cyan(name);

            if (!f.file.empty())
            {
                pad_to(out, 2 + name.size(), 42);
                out += dim(cc::format("{}:{}", f.file, f.line));
            }
            out += '\n';
        }
    }

    out += '\n';

    cc::string current_file;
    u32 current_line = 0;

    for (isize i = 0; i < t.instructions.size(); ++i)
    {
        auto const& insn = t.instructions[i];

        // Print a source heading only where the location changes, so straight-line code stays dense.
        if (opts.source && insn.line != 0 && (insn.line != current_line || insn.file != current_file))
        {
            current_file = insn.file;
            current_line = insn.line;

            if (i > 0)
                out += '\n';

            out += dim(cc::format("{}:{}", insn.file, insn.line)) + "\n";
            if (auto const text = sources.line(insn.file, insn.line); !text.empty())
                out += "    " + green(text) + "\n";
            out += '\n';
        }

        out += format_instruction(insn);

        if (opts.register_diffs && i + 1 < t.registers.size())
            append_register_diff(out, t.registers[i], t.registers[i + 1]);

        out += '\n';
    }

    out += "\n" + dim("trace ended: ") + reason_text(t.reason) + "\n";
    out += dim(cc::format("instructions: {}", t.instructions.size())) + "\n";

    return out;
}

cc::string format_symbol_error(symbol_error const& error)
{
    cc::string out;
    out += red(error.message);

    if (error.candidates.empty())
        return out;

    out += ":\n\n";
    for (auto const& c : error.candidates)
    {
        auto const name = c.module.empty() ? c.name : cc::format("{}!{}", c.module, c.name);
        out += dim("  " + format_address(c.address)) + "  " + cyan(name) + "\n";
    }
    out += "\n" + dim("narrow the spec, or use --target module!symbol / --address.");

    return out;
}
} // namespace itrace
