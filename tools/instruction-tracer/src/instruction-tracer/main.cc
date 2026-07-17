#include <clean-core/container/span.hh>
#include <clean-core/string/print.hh>
#include <instruction-tracer/cli/options.hh>
#include <instruction-tracer/debug/debug_session.hh>
#include <instruction-tracer/debug/trace_enrich.hh>
#include <instruction-tracer/decode/instruction_decoder.hh>
#include <instruction-tracer/report/console.hh>
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
    config.trace.capture_registers = opts.register_diffs;
    config.trace.capture_stack = opts.stack;

    itrace::debug_session session(cc::move(config));

    // The stats table prints no source lines, so resolving them would be a PDB lookup per instruction
    // for output nobody sees — and --stats raises the instruction cap a thousandfold.
    bool const want_source = opts.source && !opts.stats;

    // Enrichment needs the debuggee's symbols, so it happens inside run()'s lifetime via the
    // callback below rather than after the session tears down.
    auto traces = session.run(
        [&](itrace::trace& t, itrace::symbol_session const& symbols)
        {
            itrace::instruction_decoder const decoder;
            itrace::enrich_trace(t, symbols, decoder, want_source, opts.stats);
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

    if (opts.stats)
    {
        cc::print(itrace::format_stats(itrace::collect_stats(traces.value())));
        return exit_ok;
    }

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
