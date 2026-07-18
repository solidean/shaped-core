#include "options.hh"

#include <clean-core/string/format.hh>
#include <clean-core/string/string_view.hh>

namespace itrace
{
namespace
{
/// Parse a non-negative decimal. Fails on empty/garbage/overflow so a typo'd --skip is loud.
cc::result<u64> parse_u64(cc::string_view text, cc::string_view flag)
{
    if (text.empty())
        return cc::error(cc::format("{} expects a number", flag));

    u64 value = 0;
    for (char const c : text)
    {
        if (c < '0' || c > '9')
            return cc::error(cc::format("{} expects a number, got '{}'", flag, text));

        u64 const digit = u64(c - '0');
        if (value > (u64(-1) - digit) / 10)
            return cc::error(cc::format("{} value '{}' is too large", flag, text));

        value = value * 10 + digit;
    }
    return value;
}

/// Apply `fn` to each comma-separated, whitespace-free token of `value`. Empty tokens are skipped,
/// so "a,,b" and a trailing comma are tolerated. Stops and forwards the first error `fn` returns.
template <class F>
cc::result<cc::unit> for_each_token(cc::string_view value, F&& fn)
{
    isize start = 0;
    while (start <= value.size())
    {
        auto const comma = value.find(',', start);
        auto const end = comma < 0 ? value.size() : comma;
        if (end > start)
            CC_RETURN_IF_ERROR(fn(value.subview({.start = start, .end = end})));

        if (comma < 0)
            break;
        start = comma + 1;
    }
    return cc::unit{};
}

cc::result<cc::unit> parse_sections(cc::string_view value, output_sections& out)
{
    return for_each_token(value,
                          [&](cc::string_view token) -> cc::result<cc::unit>
                          {
                              if (token == "trace")
                                  out.trace = true;
                              else if (token == "stats")
                                  out.stats = true;
                              else if (token == "memory")
                                  out.memory = true;
                              else if (token == "cachelines")
                                  out.cachelines = true;
                              else if (token == "memory-stats")
                                  out.memory_stats = true;
                              else
                                  return cc::error(cc::format("unknown section '{}' (valid: trace, stats, memory, "
                                                              "cachelines, memory-stats)",
                                                              token));
                              return cc::unit{};
                          });
}

cc::result<cc::unit> parse_memory_regions(cc::string_view value, memory_regions& out)
{
    // An explicit list replaces the default, so start from nothing selected.
    out = {.heap = false, .frame = false, .stack = false, .instructions = false};
    return for_each_token(value,
                          [&](cc::string_view token) -> cc::result<cc::unit>
                          {
                              if (token == "heap")
                                  out.heap = true;
                              else if (token == "frame")
                                  out.frame = true;
                              else if (token == "stack")
                                  out.stack = true;
                              else if (token == "instructions")
                                  out.instructions = true;
                              else
                                  return cc::error(cc::format(
                                      "unknown memory region '{}' (valid: heap, frame, stack, instructions)", token));
                              return cc::unit{};
                          });
}

/// Match `--flag` / `--no-flag`, writing the sense into `out`. Returns false if `arg` is neither.
bool match_bool(cc::string_view arg, cc::string_view name, bool& out)
{
    if (arg.size() > 2 && arg.subview({.start = 0, .end = 2}) == "--")
    {
        auto const body = arg.subview({.start = 2, .end = arg.size()});
        if (body == name)
        {
            out = true;
            return true;
        }
        if (body.starts_with("no-") && body.subview({.start = 3, .end = body.size()}) == name)
        {
            out = false;
            return true;
        }
    }
    return false;
}
} // namespace

cc::string_view usage_text()
{
    return R"(instruction-tracer — record what optimized x64 code actually executed (Windows only)

usage:
  instruction-tracer --exe <path> (--symbol <name> | --address <hex> | --target <spec>)
                     [options] [-- <args passed to the debuggee>]

target (exactly one):
  --symbol <name>        break on a symbol; a unique substring is enough
  --address <hex>        break on an absolute runtime address, e.g. 0x7ff611203410
  --target <spec>        one of: foo::bar | 0x7ff6... | mod.exe!foo::bar | mod.exe+0x3410

collection:
  --skip <n>             ignore the first n entry hits            (default 0)
  --traces <n>           record n invocations                     (default 1)
  --instructions <n>     max retired instructions per trace       (default 100)
  --until-return         stop once the entry frame returns        (default on)
  --stop-at-syscall      stop before executing a syscall          (default on)

output sections (combine freely; all come from one capture):
  --sections <list>      comma-separated subset of:                (default: trace)
                           trace         the retired-instruction trace
                           stats         per-symbol instruction table
                           memory        raw chronological memory accesses
                           cachelines    memory accesses bucketed by cacheline
                           memory-stats  per-symbol memory table
                         any non-trace section raises the --instructions default to
                         100000, since a truncated trace corrupts the aggregates
  --stats                shortcut for --sections stats             (default off)

trace section:
  --stack                print the stack at entry                 (default on)
  --source               annotate with source file/line and text  (default on)
  --register-diffs       show registers changed by each instruction (default off)

memory sections:
  --memory-regions <list>  comma-separated subset of heap,frame,stack,instructions
                           to include                             (default: heap,stack)
                           frame = the current function's own stack; stack = another
                           function's stack (a stack array reached through a span);
                           instructions = code fetches (an I-cache footprint)
  --memory-instruction-addresses  annotate accesses with the accessing rip (default off)

process:
  --terminate-after-traces  kill the debuggee once done           (default on)

  Every boolean flag has a --no-<flag> form, e.g. --no-source.
  --colored / --plain    force color on / off (default: auto-detect the terminal)
  -h / --help            print this and exit
)";
}

cc::result<options> parse_options(cc::span<char const* const> args)
{
    options opts;
    bool has_target = false;
    bool explicit_instructions = false;

    // argv[0] is the program itself.
    for (isize i = 1; i < args.size(); ++i)
    {
        cc::string_view const arg = args[i];

        // Everything after `--` belongs to the debuggee.
        if (arg == "--")
        {
            for (isize j = i + 1; j < args.size(); ++j)
                opts.target_args.push_back(cc::string(args[j]));
            break;
        }

        if (arg == "-h" || arg == "--help")
        {
            opts.help = true;
            return opts;
        }

        // Not --no- style: these are a pair overriding the same auto-detected setting.
        if (arg == "--colored")
        {
            opts.color = color_mode::always;
            continue;
        }
        if (arg == "--plain")
        {
            opts.color = color_mode::never;
            continue;
        }

        // Flags taking a value. `need_value` keeps the "missing value" error in one place.
        auto const need_value = [&](cc::string_view& out) -> cc::result<cc::unit>
        {
            if (i + 1 >= args.size())
                return cc::error(cc::format("{} expects a value", arg));
            out = args[++i];
            return cc::unit{};
        };

        cc::string_view value;

        if (arg == "--exe")
        {
            CC_RETURN_IF_ERROR(need_value(value));
            opts.exe = value;
            continue;
        }

        if (arg == "--symbol" || arg == "--address" || arg == "--target")
        {
            CC_RETURN_IF_ERROR(need_value(value));

            if (has_target)
                return cc::error("--symbol / --address / --target are mutually exclusive");
            has_target = true;

            if (arg == "--symbol")
            {
                // An explicit --symbol is always a symbol, even if it looks like an address.
                opts.target.form = target_spec::kind::symbol;
                opts.target.symbol = value;
            }
            else if (arg == "--address")
            {
                auto address = parse_address(value);
                CC_RETURN_IF_ERROR(address);
                opts.target.form = target_spec::kind::address;
                opts.target.address = address.value();
            }
            else
            {
                auto spec = parse_target_spec(value);
                CC_RETURN_IF_ERROR(spec);
                opts.target = cc::move(spec.value());
            }
            continue;
        }

        if (arg == "--skip")
        {
            CC_RETURN_IF_ERROR(need_value(value));
            auto n = parse_u64(value, arg);
            CC_RETURN_IF_ERROR(n);
            opts.skip = n.value();
            continue;
        }

        if (arg == "--traces" || arg == "--instructions")
        {
            CC_RETURN_IF_ERROR(need_value(value));
            auto n = parse_u64(value, arg);
            CC_RETURN_IF_ERROR(n);
            if (n.value() == 0)
                return cc::error(cc::format("{} must be at least 1", arg));

            if (arg == "--traces")
            {
                opts.traces = u32(cc::min<u64>(n.value(), u32(-1)));
            }
            else
            {
                opts.instructions = u32(cc::min<u64>(n.value(), u32(-1)));
                explicit_instructions = true;
            }
            continue;
        }

        if (arg == "--sections")
        {
            CC_RETURN_IF_ERROR(need_value(value));
            CC_RETURN_IF_ERROR(parse_sections(value, opts.sections));
            continue;
        }

        if (arg == "--memory-regions")
        {
            CC_RETURN_IF_ERROR(need_value(value));
            CC_RETURN_IF_ERROR(parse_memory_regions(value, opts.regions));
            continue;
        }

        // Back-compat shortcut for `--sections stats`; `--no-stats` clears just that section.
        {
            bool stats = opts.sections.stats;
            if (match_bool(arg, "stats", stats))
            {
                opts.sections.stats = stats;
                continue;
            }
        }

        if (match_bool(arg, "until-return", opts.until_return))
            continue;
        if (match_bool(arg, "stop-at-syscall", opts.stop_at_syscall))
            continue;
        if (match_bool(arg, "stack", opts.stack))
            continue;
        if (match_bool(arg, "source", opts.source))
            continue;
        if (match_bool(arg, "terminate-after-traces", opts.terminate_after_traces))
            continue;
        if (match_bool(arg, "register-diffs", opts.register_diffs))
            continue;
        if (match_bool(arg, "memory-instruction-addresses", opts.memory_instruction_addresses))
            continue;

        return cc::error(cc::format("unknown argument '{}' (see --help)", arg));
    }

    if (opts.exe.empty())
        return cc::error("--exe is required (see --help)");
    if (!has_target)
        return cc::error("one of --symbol / --address / --target is required (see --help)");

    // No section selected means the trace alone — today's default.
    if (opts.sections.none())
        opts.sections.trace = true;

    // Order-independent: a table/memory section only raises the cap where the user set none,
    // whichever came first on the command line. A short trace silently corrupts every aggregate.
    if (opts.sections.any_non_trace() && !explicit_instructions)
        opts.instructions = stats_instruction_default;

    return opts;
}
} // namespace itrace
