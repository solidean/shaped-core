#include "compile_to_text.hh"

#include <clean-core/string/format.hh>
#include <shaped-graphics-language/ast/build.hh>
#include <shaped-graphics-language/check/check.hh>
#include <shaped-graphics-language/driver/prelude.hh>
#include <shaped-graphics-language/emit/impl/dialect.hh>
#include <shaped-graphics-language/source/format_diagnostic.hh>
#include <shaped-graphics-language/syntax/parsed_file.hh>

using namespace sgl;

namespace
{
/// The report of every phase, one diagnostic per line, and whether any of them is an error.
struct report
{
    cc::string text;
    bool has_error = false;

    void add(cc::string_view file_name, cc::string_view source, diagnostic const& d, cc::string_view detail = {})
    {
        // A warning is left out: the result is either the text or the reasons there is none.
        if (d.level == severity::warning)
            return;
        has_error = true;
        text += format_diagnostic(file_name, source, d, detail);
        text += "\n";
    }

    void add_all(cc::string_view file_name, parsed_file const& file, ast::file_ast const& ast)
    {
        for (auto const& d : file.diagnostics)
            this->add(file_name, file.source, d);
        for (auto const& d : ast.diagnostics)
            this->add(file_name, file.source, d);
    }
};
} // namespace

cc::result<cc::string, cc::string> sgl::compile_to_text(text_request const& request)
{
    auto const prelude = parse(prelude_source());
    auto const prelude_ast = ast::build(prelude);
    auto const user = parse(request.source);
    auto const user_ast = ast::build(user);

    auto found = report();
    found.add_all(prelude_name(), prelude, prelude_ast);
    found.add_all(request.source_name, user, user_ast);

    auto const m = check::check({.file = prelude, .ast = prelude_ast}, {.file = user, .ast = user_ast});
    for (auto const& d : m.diagnostics)
        found.add(d.file == 0 ? prelude_name() : request.source_name, d.file == 0 ? prelude.source : user.source,
                  d.what, d.detail);

    if (found.has_error)
        return cc::error(cc::move(found.text));

    auto index = isize(-1);
    for (auto i = isize(0); i < m.entry_points.size(); ++i)
        if (m.entry_points[i].name == request.entry_point)
            index = i;

    if (index < 0)
    {
        auto held = cc::string();
        for (auto const& e : m.entry_points)
            held.appendf("{}{} '{}'", held.empty() ? "" : ", ", emit::impl::stage_name(e.entry_stage), e.name);
        return cc::error(cc::format("{}: error: no entry point named '{}' (the source holds: {})\n", request.source_name,
                                    request.entry_point, held.empty() ? cc::string_view("none") : cc::string_view(held)));
    }

    auto const& e = m.entry_points[index];
    if (request.stage != check::stage::none && e.entry_stage != request.stage)
        return cc::error(cc::format("{}: error: entry point '{}' is a {} entry point, and a {} one was asked for\n",
                                    request.source_name, e.name, emit::impl::stage_name(e.entry_stage),
                                    emit::impl::stage_name(request.stage)));

    auto emitted = emit::emit(m, index, request.target);
    if (!emitted.has_text())
    {
        auto text = cc::string();
        for (auto const& error : emitted.errors)
            text.appendf("{}: error: {}: {}\n", request.source_name, emit::to_string(error.kind), error.detail);
        return cc::error(cc::move(text));
    }
    return cc::move(emitted.text);
}
