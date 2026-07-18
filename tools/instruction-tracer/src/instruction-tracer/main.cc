#include <clean-core/container/span.hh>
#include <clean-core/string/print.hh>
#include <instruction-tracer/cli/options.hh>
#include <instruction-tracer/debug/debug_session.hh>
#include <instruction-tracer/debug/trace_enrich.hh>
#include <instruction-tracer/decode/instruction_decoder.hh>
#include <instruction-tracer/report/console.hh>
#include <instruction-tracer/report/memory_formatter.hh>
#include <instruction-tracer/report/memory_stats.hh>
#include <instruction-tracer/report/source_cache.hh>
#include <instruction-tracer/report/trace_formatter.hh>
#include <instruction-tracer/report/trace_stats.hh>

namespace
{
// 0 = traced something, 1 = bad usage / launch failure, 2 = the target never resolved.
constexpr int exit_ok = 0;
constexpr int exit_usage = 1;
constexpr int exit_unresolved = 2;

int run(itrace::options const& opts)
{
    itrace::debug_config config;
    config.exe = opts.exe;
    config.args = opts.target_args;
    config.target = opts.target;
    config.skip = opts.skip;
    config.traces = opts.traces;
    config.terminate_after_traces = opts.terminate_after_traces;
    config.trace.max_instructions = opts.instructions;
    config.trace.until_return = opts.until_return;
    config.trace.stop_at_syscall = opts.stop_at_syscall;
    config.trace.capture_stack = opts.stack;

    auto const& sections = opts.sections;

    // Source lines only feed the trace section, and each is a PDB lookup per instruction; the owner
    // feeds the tables and the memory attribution; the memory pass needs the register snapshots, so
    // it forces register capture even without --register-diffs.
    bool const want_source = sections.trace && opts.source;
    bool const want_owner = sections.stats || sections.memory_stats;
    bool const want_memory = sections.any_memory();
    config.trace.capture_registers = opts.register_diffs || want_memory;

    itrace::debug_session session(cc::move(config));

    // Enrichment needs the debuggee's symbols, so it happens inside run()'s lifetime via the
    // callback below rather than after the session tears down.
    auto traces = session.run(
        [&](itrace::trace& t, itrace::symbol_session const& symbols)
        {
            itrace::instruction_decoder const decoder;
            itrace::enrich_trace(t, symbols, decoder, want_source, want_owner, want_memory);
        });

    if (traces.has_error())
    {
        cc::eprintln("{}", itrace::format_symbol_error(traces.error()));
        return exit_unresolved;
    }

    if (traces.value().empty())
    {
        cc::eprintln("no traces recorded: '{}' was never entered (--skip {} may exceed the hit count)",
                     opts.target.to_string(), opts.skip);
        return exit_unresolved;
    }

    itrace::memory_view_options mem;
    mem.heap = opts.regions.heap;
    mem.frame = opts.regions.frame;
    mem.stack = opts.regions.stack;
    mem.instructions = opts.regions.instructions;
    mem.instruction_addresses = opts.memory_instruction_addresses;

    // Print each selected section in a fixed order, all from this one capture. A blank line
    // separates them.
    bool need_separator = false;
    auto const separate = [&]
    {
        if (need_separator)
            cc::println();
        need_separator = true;
    };

    if (sections.trace)
    {
        separate();
        itrace::format_options fmt;
        fmt.stack = opts.stack;
        fmt.source = opts.source;
        fmt.register_diffs = opts.register_diffs;

        itrace::source_cache sources;
        auto const total = cc::u32(traces.value().size());
        for (auto const& t : traces.value())
        {
            cc::print(itrace::format_trace(t, total, fmt, sources));
            cc::println();
        }
    }

    if (sections.stats)
    {
        separate();
        cc::print(itrace::format_stats(itrace::collect_stats(traces.value())));
    }

    if (sections.memory)
    {
        separate();
        cc::print(itrace::format_memory_raw(traces.value(), mem));
    }

    if (sections.cachelines)
    {
        separate();
        cc::print(itrace::format_memory_cachelines(traces.value(), mem));
    }

    if (sections.memory_stats)
    {
        separate();
        cc::print(itrace::format_memory_stats(itrace::collect_memory_stats(traces.value(), mem)));
    }

    return exit_ok;
}
} // namespace

int main(int argc, char const* const* argv)
{
    auto opts = itrace::parse_options(cc::span<char const* const>(argv, cc::isize(argc)));

    // Resolve color before the first byte of output, including the usage error below. A parse
    // failure has no options to read, so that path auto-detects.
    itrace::configure_console(opts.has_value() ? opts.value().color : itrace::color_mode::automatic);

    if (opts.has_error())
    {
        cc::eprintln("error: {}\n", opts.error().to_string());
        cc::eprint(itrace::usage_text());
        return exit_usage;
    }

    if (opts.value().help)
    {
        cc::print(itrace::usage_text());
        return exit_ok;
    }

    return run(opts.value());
}
