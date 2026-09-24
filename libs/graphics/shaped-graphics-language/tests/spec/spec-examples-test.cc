#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>
#include <nexus/test.hh>
#include <shaped-graphics-language/debug/dump.hh>
#include <shaped-graphics-language/driver/compile_to_text.hh>
#include <shaped-graphics-language/driver/describe.hh>
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
    "pipelines.md",
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
    "semantics/_index.md",
    "semantics/checking.md",
    "semantics/why/checking.md",
    "semantics/emitting.md",
    "semantics/why/emitting.md",
    "semantics/evaluation.md",
    "semantics/why/evaluation.md",
    "semantics/legalization.md",
    "semantics/why/legalization.md",
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

namespace
{
/// The names after `fun` on every line that declares an entry point, which is what `compile_to_text` is asked for.
/// A scan rather than a parse: a source that does not check still has entry points to ask for, and each must fail cleanly.
cc::vector<cc::string> entry_points_of(cc::string_view source)
{
    auto result = cc::vector<cc::string>();
    auto at = isize(0);
    while (true)
    {
        auto const found = source.find(cc::string_view("fun "), at);
        if (found < 0)
            break;
        auto const line_start = source.subview({.start = 0, .end = found}).rfind('\n') + 1;
        auto const head = source.subview({.start = line_start, .end = found});
        auto name_end = found + 4;
        while (name_end < source.size()
               && (source[name_end] == '_' || (source[name_end] >= 'a' && source[name_end] <= 'z')
                   || (source[name_end] >= 'A' && source[name_end] <= 'Z')
                   || (source[name_end] >= '0' && source[name_end] <= '9')))
            ++name_end;
        if (head.contains('@') && name_end > found + 4)
            result.push_back(cc::string(source.subview({.start = found + 4, .end = name_end})));
        at = found + 4;
    }
    return result;
}

/// Every declared entry point of `source` through every target, and `describe` besides.
/// Each call must give text or an error that says something; the assert a crash would be fails the test by itself.
void require_total(cc::string_view source, cc::string_view where, cc::string& failures)
{
    auto const described = sgl::describe({.source = source, .source_name = where});
    if (described.has_error() && described.error().empty())
        failures.appendf("{}: describe failed without saying why\n", where);
    for (auto const& name : entry_points_of(source))
        for (auto const t : sgl::emit::all_targets())
        {
            auto const text
                = sgl::compile_to_text({.source = source, .source_name = where, .entry_point = name, .target = t});
            if (text.has_error() ? text.error().empty() : text.value().text.empty())
                failures.appendf("{}: '{}' for {} gave neither text nor a reason\n", where, name,
                                 sgl::emit::to_string(t));
        }
}
} // namespace

TEST("sgl spec - every example of the spec and every sample compiles for every target or says why, and nothing asserts")
{
    auto failures = cc::string();
    auto sources = 0;
    for (auto const file : spec_files)
    {
        auto const path = cc::string(SGL_SPEC_DIR) + "/" + file;
        for (auto const& e : examples_of(read_text(path)))
        {
            ++sources;
            require_total(e.source, cc::format("{}:{}", file, e.line), failures);
        }
    }
    for (auto const sample : {"basic-raster.sgl", "control-flow.sgl", "cube.sgl", "helpers.sgl", "matrices.sgl",
                              "members-and-bindings.sgl", "pipeline.sgl"})
    {
        ++sources;
        require_total(read_text(cc::string(SGL_SAMPLES_DIR) + "/" + sample), sample, failures);
    }
    CHECK(failures == "");
    CHECK(sources > 50);
}
