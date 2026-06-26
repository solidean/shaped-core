#include <clean-core/string/print.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/test.hh>

// print/eprint write to the real stdout/stderr, which a unit test cannot portably capture here.
// Formatting correctness is covered by format-test.cc; this test guards the print-specific invariant:
// every overload compiles and resolves unambiguously (raw string_view vs. cc::format string), and runs.
// Content is kept tiny to avoid polluting the test console.

TEST("print - overloads resolve and run")
{
    // raw string_view form (no format interpretation, even with braces)
    cc::print("");
    cc::print("a {literal} brace stays literal\n");
    cc::println();   // just a newline
    cc::println(""); // empty + newline
    cc::eprint("");
    cc::eprintln();
    cc::eprintln("");

    // cc::format form (>= 1 argument disambiguates it from the raw overload)
    cc::print("{}", 0);
    cc::println("{}+{}={}", 1, 2, 3);
    cc::eprint("{:#x}", 255);
    cc::eprintln("{:.1f}", 1.5);

    // a non-literal string_view selects the raw overload
    cc::string_view const sv = "x";
    cc::print(sv);
    cc::println(sv);
    cc::string const s = "y";
    cc::eprintln(s);

    // flushing
    cc::flush();
    cc::eflush();

    SUCCEED("all print/println/eprint/eprintln overloads compiled, resolved, and executed");
}
