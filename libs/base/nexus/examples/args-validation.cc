#include <clean-core/container/vector.hh>
#include <clean-core/string/print.hh>
#include <clean-core/string/string.hh>
#include <nexus/args/args.hh>
#include <nexus/test.hh>

// Validation, and the thing that makes it worth having: the object that REJECTS a value is the object that
// PRINTS the rule, so a help page cannot describe something the parser does not enforce.
//
//   uv run dev.py example nexus/args-validation
//   uv run dev.py example nexus/args-validation --test-args "--help"
//   uv run dev.py example nexus/args-validation --test-args "--port 70000"

namespace
{
/// Everything this example declares, in one place, since it parses several different lines below.
struct server_options
{
    int port = 8080;
    int workers = 4;
    cc::string mode = "fast";
    cc::string tls_cert;
    bool tls = false;
    bool quiet = false;
    bool verbose = false;
};

nx::args_builder build(server_options& o)
{
    auto args = nx::args({.name = "args-validation", .description = "a made-up server, to show the rules"});
    args.no_auto_print();

    // Each rule states itself, so none of these needed a description written by hand.
    args.arg({"p", "port"}, o.port, {.desc = "which port to listen on", .validate = nx::arg::in_range(1, 65535)});
    args.arg({"w", "workers"}, o.workers,
             {.desc = "how many workers", .validate = nx::arg::at_least(1) && nx::arg::at_most(64)});
    args.arg({"m", "mode"}, o.mode, {.desc = "how to serve", .validate = nx::arg::one_of({"fast", "safe", "debug"})});

    args.group("tls");
    args.arg({"tls"}, o.tls, "serve over TLS");
    args.arg({"tls-cert"}, o.tls_cert, {.desc = "the certificate to serve with", .metavar = "FILE"});

    args.group("output");
    args.arg({"q", "quiet"}, o.quiet, "say less");
    args.arg({"v", "verbose"}, o.verbose, "say more");

    // Cross-argument rules go on the builder, because they are about the command line rather than any one
    // argument.
    // Each appears in the CONSTRAINTS section, so a rule can never be enforced silently.
    args.requires_all("--tls", {"--tls-cert"});
    args.mutually_exclusive({"--quiet", "--verbose"});

    return args;
}

/// Parse one line and report what came of it, so the whole point reads as a table.
void show(cc::string_view label, cc::span<cc::string_view const> line)
{
    auto options = server_options();
    auto args = build(options);
    auto const result = args.parse(line);

    cc::println(cc::format("  {}", label));
    if (result.ok())
        cc::println(
            cc::format("    accepted: port {}, {} worker(s), mode {}", options.port, options.workers, options.mode));
    else
        for (auto const& diagnostic : result.diagnostics())
            cc::println(cc::format("    rejected: {}", diagnostic.message));
}
} // namespace

EXAMPLE("nexus/args-validation", nx::config::args("--port 9000 --workers 8"))
{
    auto options = server_options();
    auto args = build(options);

    auto const result = args.parse(nx::current_args());

    if (result.outcome() == nx::args_outcome::help_requested)
    {
        // The rules appear twice over: beside the argument they constrain, and in CONSTRAINTS for the
        // cross-argument ones.
        // Neither was written as prose.
        cc::println(args.help_text({.width = 96}));
        return;
    }

    if (result.ok())
        cc::println(
            cc::format("running on port {} with {} worker(s), mode {}", options.port, options.workers, options.mode));
    else
        cc::println(result.diagnostic_text());

    cc::println("");
    cc::println("what each rule catches:");

    show("--port 70000", {"--port", "70000"});
    show("--workers 0", {"--workers", "0"});
    show("--mode sideways", {"--mode", "sideways"});
    show("--tls, with no certificate", {"--tls"});
    show("--quiet --verbose", {"--quiet", "--verbose"});
    show("--port 443 --workers 8", {"--port", "443", "--workers", "8"});
}
