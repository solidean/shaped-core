#include "compile_to_text.hh"

#include <clean-core/string/format.hh>
#include <shaped-graphics-language/driver/impl/front_end.hh>
#include <shaped-graphics-language/emit/impl/dialect.hh>
#include <shaped-graphics-language/legalize/legalize.hh>
#include <shaped-graphics-language/test/run_tests.hh>

using namespace sgl;

cc::result<sgl::emitted_source, cc::string> sgl::compile_to_text(text_request const& request)
{
    auto const front = driver::impl::run_front_end(request.source, request.source_name);
    if (!front.errors.empty())
        return cc::error(front.errors);
    auto const& m = front.module;

    if (request.run_tests)
    {
        auto failed = cc::string();
        for (auto const& r : test::run_tests(m, driver::impl::module_files_of(front), {.file = front.program_file()}))
            if (!r.is_passed())
                failed += driver::impl::format_located(front, test::diagnostic_of(m, r));
        if (!failed.empty())
            return cc::error(cc::move(failed));
    }

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

    // The check pass writes the structured form, and a target prints the core form.
    auto emitted = emit::emit_entry_point(m, check::legalize(m, e), request.target);
    if (!emitted.has_text())
    {
        auto text = cc::string();
        for (auto const& error : emitted.errors)
            text.appendf("{}: error: {}: {}\n", request.source_name, emit::to_string(error.kind), error.detail);
        return cc::error(cc::move(text));
    }
    return sgl::emitted_source{.text = cc::move(emitted.text),
                               .entry_point = cc::move(emitted.entry_point),
                               .bound_names = cc::move(emitted.bound_names),
                               .color_targets = emitted.color_targets,
                               .target_struct = cc::move(emitted.target_struct)};
}
