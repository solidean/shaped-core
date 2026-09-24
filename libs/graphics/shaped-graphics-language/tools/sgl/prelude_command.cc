#include "files.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/string/print.hh>
#include <nexus/args/ambient.hh>
#include <nexus/args/args.hh>
#include <nexus/test.hh>
#include <shaped-graphics-language/builtins/registry.hh>
#include <shaped-graphics-language/driver/pipeline_fields.hh>

using namespace cc::primitive_defines;

namespace
{
// 0 = in sync or written, 1 = bad usage or an IO error, 2 = the file differs from what generates it.
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
/// `tool` is the command, `source` what the text is generated from, and `edit` where a change belongs instead.
void explain_difference(cc::string_view tool,
                        cc::string_view source,
                        cc::string_view edit,
                        cc::string_view path,
                        cc::string_view committed,
                        cc::string_view generated)
{
    auto const old_lines = lines_of(committed);
    auto const new_lines = lines_of(generated);
    auto at = isize(0);
    while (at < old_lines.size() && at < new_lines.size() && old_lines[at] == new_lines[at])
        ++at;

    auto const shown = [&](cc::vector<cc::string_view> const& lines)
    { return at < lines.size() ? lines[at] : cc::string_view("<end of file>"); };

    cc::eprintln("{}: {} is not what {} generates", tool, path, source);
    cc::eprintln("  first difference at line {}", at + 1);
    cc::eprintln("    committed: {}", shown(old_lines));
    cc::eprintln("    generated: {}", shown(new_lines));
    cc::eprintln("  the committed file has {} lines, the generated text {}", old_lines.size(), new_lines.size());
    if (committed.contains('\r'))
        cc::eprintln("  the committed file has CR line endings; the generated text has LF alone");
    cc::eprintln("The file is generated: edit {}, then run", edit);
    cc::eprintln("    uv run dev.py check sgl-prelude --fix");
}

/// What `sgl prelude` and `sgl pipeline-fields` share: print a generated text, write it, or check a file against it.
struct generated_file
{
    cc::string_view tool;
    cc::string_view description;
    cc::string_view source;
    cc::string_view edit;
};

int run_generated(generated_file const& file, cc::string const& generated)
{
    auto write_path = cc::string();
    auto check_path = cc::string();
    auto args = nx::args({.name = file.tool, .description = file.description});
    args.arg({"write"}, write_path, {.desc = "write the generated text to PATH", .metavar = "PATH"});
    args.arg({"check"}, check_path, {.desc = "exit 2 when PATH differs from the generated text", .metavar = "PATH"});
    args.mutually_exclusive({"write", "check"});
    if (auto const r = args.parse(nx::test_args()); r.should_exit())
        return r.exit_code();

    if (!write_path.empty())
    {
        if (auto const written = sgl_tool::write_file(write_path, generated); written.has_error())
        {
            cc::eprintln("{}: cannot write {}: {}", file.tool, write_path, written.error());
            return exit_usage;
        }
        cc::println("{}: wrote {}", file.tool, write_path);
        return exit_ok;
    }

    if (!check_path.empty())
    {
        auto const committed = sgl_tool::read_file(check_path);
        if (committed.has_error())
        {
            cc::eprintln("{}: cannot read {}: {}", file.tool, check_path, committed.error());
            return exit_usage;
        }
        if (committed.value() == generated)
        {
            cc::println("{}: {} is in sync with {}", file.tool, check_path, file.source);
            return exit_ok;
        }
        explain_difference(file.tool, file.source, file.edit, check_path, committed.value(), generated);
        return exit_differs;
    }

    cc::print(generated);
    cc::flush();
    return exit_ok;
}
} // namespace

// `sgl prelude`: the builtin half of the prelude, as the C++ builtin registry generates it.
COMMAND("prelude")
{
    return run_generated({.tool = "sgl prelude",
                          .description = "Prints prelude/builtins.sgl as the builtin registry generates it, "
                                         "or writes it to a file, or checks a file against it.",
                          .source = "the builtin registry",
                          .edit = "the registry under src/shaped-graphics-language/builtins/"},
                         sgl::builtins::default_registry().prelude_text());
}

// `sgl pipeline-fields`: slib's setters and reload tables, as the prelude's mirror of sg's description generates them.
COMMAND("pipeline-fields")
{
    return run_generated({.tool = "sgl pipeline-fields",
                          .description = "Prints slib's impl/pipeline_fields.hh as the prelude's mirror of "
                                         "sg::raster_pipeline_description generates it, or writes it, or checks a file "
                                         "against it.",
                          .source = "the prelude's mirror of sg's description",
                          .edit = "prelude/core.sgl"},
                         sgl::pipeline_fields_text());
}
