#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>
#include <nexus/test.hh>
#include <shaped-graphics-language/debug/dump.hh>
#include <shaped-graphics-language/syntax/parsed_file.hh>

using namespace cc::primitive_defines;

namespace
{
/// Every file of the spec whose examples are checked, relative to `docs/spec/`.
/// Listed rather than discovered, so a file dropping out of the check is a visible edit.
constexpr cc::string_view spec_files[] = {
    "_index.md",
    "keywords.md",
    "notation.md",
    "syntax/_index.md",
    "syntax/line-tree.md",
    "syntax/tokens.md",
    "syntax/strings-and-comments.md",
    "syntax/groups.md",
    "syntax/forms.md",
    "syntax/numbers.md",
    "syntax/operators.md",
    "syntax/ast.md",
    "syntax/diagnostics.md",
    "syntax/why/line-tree.md",
    "syntax/why/tokens.md",
    "syntax/why/strings-and-comments.md",
    "syntax/why/groups.md",
    "syntax/why/forms.md",
    "syntax/why/numbers.md",
    "syntax/why/operators.md",
    "syntax/why/ast.md",
    "incubator/structural-types.md",
    "incubator/function-model.md",
    "incubator/shader-logging.md",
    "incubator/ranges-and-iteration.md",
    "incubator/user-operators.md",
    "incubator/string-family.md",
};

enum class fence_kind : u8
{
    /// `sgl`: parses without a single diagnostic.
    valid,
    /// `sgl error`: reports at least one.
    error,
    /// `sgl sketch`: planned syntax or a later phase, so nothing is claimed about it.
    sketch,
};

struct example
{
    fence_kind kind;
    int line;
    /// The last line of prose before the fence, which names the diagnostic an `sgl error` example is about.
    cc::string lead;
    cc::string source;
};

cc::string read_text(cc::string_view path)
{
    auto adapter = cc::file_read_stream_adapter::open(path);
    REQUIRE(adapter.has_value());
    auto stream = adapter.value().stream();
    auto const bytes = stream.read_all();
    REQUIRE(bytes.has_value());
    return cc::string(reinterpret_cast<char const*>(bytes.value().data()), bytes.value().size());
}

cc::vector<example> examples_of(cc::string_view text)
{
    auto result = cc::vector<example>();
    auto line_number = 0;
    auto is_inside = false;
    auto lead = cc::string_view();
    auto at = isize(0);
    while (at < text.size())
    {
        auto end = text.find('\n', at);
        if (end < 0)
            end = text.size();
        auto line = text.subview({.start = at, .end = end});
        if (line.ends_with('\r'))
            line.remove_suffix(1);
        at = end + 1;
        ++line_number;

        if (is_inside)
        {
            if (line == "```")
            {
                is_inside = false;
                lead = {};
            }
            else
            {
                result.back().source += line;
                result.back().source += "\n";
            }
            continue;
        }

        if (line == "```sgl")
            result.push_back({.kind = fence_kind::valid, .line = line_number});
        else if (line == "```sgl error")
            result.push_back({.kind = fence_kind::error, .line = line_number});
        else if (line == "```sgl sketch")
            result.push_back({.kind = fence_kind::sketch, .line = line_number});
        else
        {
            // A table row lists many kinds and is about none of them in particular.
            if (!line.empty())
                lead = line.starts_with('|') ? cc::string_view() : line;
            continue;
        }
        result.back().lead = lead;
        is_inside = true;
    }
    return result;
}
} // namespace

TEST("sgl spec - every checked example in the spec parses the way its fence says")
{
    auto failures = cc::string();
    auto checked = 0;

    for (auto const file : spec_files)
    {
        auto const path = cc::string(SGL_SPEC_DIR) + "/" + file;
        for (auto const& e : examples_of(read_text(path)))
        {
            if (e.kind == fence_kind::sketch)
                continue;

            ++checked;

            auto const parsed = sgl::parse(e.source);
            auto const diagnostics = sgl::dump_diagnostics(parsed);
            if (e.kind == fence_kind::valid && !diagnostics.empty())
                failures.appendf("{}:{}: an `sgl` example must parse cleanly, got:\n{}", file, e.line, diagnostics);
            if (e.kind == fence_kind::error && diagnostics.empty())
                failures.appendf("{}:{}: an `sgl error` example must report something\n", file, e.line);

            // Reporting *something* is not enough when the lead names the kind: the example is about that kind.
            auto names_a_kind = false;
            auto reports_it = false;
            for (auto k = 0; k <= int(sgl::diagnostic_kind::underscore_in_number); ++k)
            {
                auto const name = sgl::to_string(sgl::diagnostic_kind(k));
                if (!e.lead.contains(cc::format("`{}`", name)))
                    continue;
                names_a_kind = true;
                reports_it = reports_it || diagnostics.contains(name);
            }
            if (e.kind == fence_kind::error && names_a_kind && !reports_it)
                failures.appendf("{}:{}: the lead names a diagnostic the example does not report, got:\n{}", file,
                                 e.line, diagnostics);
        }
    }

    CHECK(failures == "");
    // A spec whose examples all quietly became sketches would pass the check above.
    CHECK(checked > 40);
}
