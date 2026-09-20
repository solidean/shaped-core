#include "files.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/string/print.hh>
#include <nexus/args/ambient.hh>
#include <nexus/args/args.hh>
#include <nexus/test.hh>
#include <shaped-graphics-language/builtins/registry.hh>

using namespace cc::primitive_defines;

namespace
{
// 0 = in sync or written, 1 = bad usage or an IO error, 2 = the file differs from the registry.
constexpr int exit_ok = 0;
constexpr int exit_usage = 1;
constexpr int exit_differs = 2;

cc::vector<cc::string_view> lines_of(cc::string_view text)
{
    auto result = cc::vector<cc::string_view>();
    while (!text.empty())
    {
        auto const end = text.find('\n');
        if (end < 0)
        {
            result.push_back(text);
            break;
        }
        result.push_back(text.subview({.offset = 0, .size = end}));
        text = text.subview({.offset = end + 1, .size = text.size() - end - 1});
    }
    return result;
}

/// Where the two texts part, for a reader: the first line that differs, as each side has it.
void explain_difference(cc::string_view path, cc::string_view committed, cc::string_view generated)
{
    auto const old_lines = lines_of(committed);
    auto const new_lines = lines_of(generated);
    auto at = isize(0);
    while (at < old_lines.size() && at < new_lines.size() && old_lines[at] == new_lines[at])
        ++at;

    auto const shown = [&](cc::vector<cc::string_view> const& lines)
    { return at < lines.size() ? lines[at] : cc::string_view("<end of file>"); };

    cc::eprintln("sgl prelude: {} is not what the builtin registry generates", path);
    cc::eprintln("  first difference at line {}", at + 1);
    cc::eprintln("    committed: {}", shown(old_lines));
    cc::eprintln("    generated: {}", shown(new_lines));
    cc::eprintln("  the committed file has {} lines, the generated text {}", old_lines.size(), new_lines.size());
    if (committed.contains('\r'))
        cc::eprintln("  the committed file has CR line endings; the generated text has LF alone");
    cc::eprintln("The file is generated: edit the registry under src/shaped-graphics-language/builtins/, then run");
    cc::eprintln("    uv run dev.py check sgl-prelude --fix");
}
} // namespace

// `sgl prelude`: the builtin half of the prelude, as the C++ builtin registry generates it.
COMMAND("prelude")
{
    auto write_path = cc::string();
    auto check_path = cc::string();
    auto args = nx::args({.name = "sgl prelude",
                          .description = "Prints prelude/builtins.sgl as the builtin registry generates it, "
                                         "or writes it to a file, or checks a file against it."});
    args.arg({"write"}, write_path, {.desc = "write the generated text to PATH", .metavar = "PATH"});
    args.arg({"check"}, check_path, {.desc = "exit 2 when PATH differs from the generated text", .metavar = "PATH"});
    args.mutually_exclusive({"write", "check"});
    if (auto const r = args.parse(nx::test_args()); r.should_exit())
        return r.exit_code();

    auto const generated = sgl::builtins::default_registry().prelude_text();

    if (!write_path.empty())
    {
        if (auto const written = sgl_tool::write_file(write_path, generated); written.has_error())
        {
            cc::eprintln("sgl prelude: cannot write {}: {}", write_path, written.error());
            return exit_usage;
        }
        cc::println("sgl prelude: wrote {}", write_path);
        return exit_ok;
    }

    if (!check_path.empty())
    {
        auto const committed = sgl_tool::read_file(check_path);
        if (committed.has_error())
        {
            cc::eprintln("sgl prelude: cannot read {}: {}", check_path, committed.error());
            return exit_usage;
        }
        if (committed.value() == generated)
        {
            cc::println("sgl prelude: {} is in sync with the builtin registry", check_path);
            return exit_ok;
        }
        explain_difference(check_path, committed.value(), generated);
        return exit_differs;
    }

    cc::print(generated);
    cc::flush();
    return exit_ok;
}
