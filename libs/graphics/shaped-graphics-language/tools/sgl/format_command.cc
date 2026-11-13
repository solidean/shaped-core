#include "files.hh"

#include <clean-core/string/print.hh>
#include <nexus/args/ambient.hh>
#include <nexus/args/args.hh>
#include <nexus/test.hh>
#include <shaped-graphics-language/driver/format.hh>

namespace
{
// 0 = every file is formatted (or, with --check, already was), 1 = bad usage or an IO error, 2 = with --check, a file
// would change.
// The blocks around a syntax error are left as written, which is a warning and no failure.
constexpr int exit_ok = 0;
constexpr int exit_usage = 1;
constexpr int exit_changes = 2;
} // namespace

// `sgl format`: writes SGL's spacing into each file in place, or with --check says which files it would change.
COMMAND("format")
{
    auto paths = cc::vector<cc::string>();
    auto is_check = false;
    auto args
        = nx::args({.name = "sgl format",
                    .description = "Formats SGL files in place: the spacing within each line, trailing whitespace "
                                   "and blank lines at the end. Indentation and line breaks stay as written."});
    args.positional("FILE", paths, {.desc = "the SGL sources", .min_count = 1});
    args.arg({"check"}, is_check, {.desc = "change nothing, and fail where a file is not formatted"});
    if (auto const r = args.parse(nx::test_args()); r.should_exit())
        return r.exit_code();

    auto code = exit_ok;
    for (auto const& path : paths)
    {
        auto const source = sgl_tool::read_file(path);
        if (source.has_error())
        {
            cc::eprintln("sgl format: cannot read {}: {}", path, source.error());
            return exit_usage;
        }
        auto const formatted = sgl::format_source(source.value());
        for (auto const& s : formatted.skipped)
            cc::eprintln("{}:{}-{}: left as written, around the syntax error at line {}: {}", path, s.first_line + 1,
                         s.last_line + 1, s.error_line + 1, s.error);
        for (auto const l : formatted.unformatted_lines)
            cc::eprintln("{}:{}: left as written: formatting it changed what the file means, a bug of the formatter",
                         path, l + 1);
        if (formatted.text == source.value())
            continue;
        if (is_check)
        {
            cc::println("{}: would change", path);
            code = exit_changes;
            continue;
        }
        if (auto const w = sgl_tool::write_file(path, formatted.text); w.has_error())
        {
            cc::eprintln("sgl format: cannot write {}: {}", path, w.error());
            return exit_usage;
        }
        cc::println("{}: formatted", path);
    }
    return code;
}
