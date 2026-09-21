#pragma once

#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>
#include <nexus/test.hh>
#include <shaped-graphics-language/ast/build.hh>
#include <shaped-graphics-language/check/check.hh>
#include <shaped-graphics-language/check/dump.hh>
#include <shaped-graphics-language/driver/prelude.hh>
#include <shaped-graphics-language/syntax/parsed_file.hh>

namespace sgl_test
{
using namespace cc::primitive_defines;

struct checked_sources;
struct library_prelude;

inline cc::string read_text(cc::string_view path)
{
    auto adapter = cc::file_read_stream_adapter::open(path);
    REQUIRE(adapter.has_value());
    auto stream = adapter.value().stream();
    auto const bytes = stream.read_all();
    REQUIRE(bytes.has_value());
    return cc::string(reinterpret_cast<char const*>(bytes.value().data()), bytes.value().size());
}

/// The generated half of the library's prelude as one text, for a test that cuts it up.
inline cc::string builtins_text()
{
    return cc::string(sgl::prelude_files()[0].source);
}

} // namespace sgl_test

/// Stands for the library's own prelude, both files of it, which every test here checks against unless it brings one.
struct sgl_test::library_prelude
{
};

/// Every file of a module and what the check pass made of them, kept together so a test can read spans back.
/// The prelude's files stand in front, and the user file is the last one.
struct sgl_test::checked_sources
{
    /// One entry per file of the module, in the module's order.
    cc::vector<sgl::parsed_file> files;
    cc::vector<sgl::ast::file_ast> asts;
    /// Copies of the last entry of each, which is what nearly every test reads.
    sgl::parsed_file user;
    sgl::ast::file_ast user_ast;
    sgl::check::checked_module module;

    /// The position of the user file, which a diagnostic and an origin name it by: 1 behind one prelude file, 2 behind the library's.
    [[nodiscard]] sgl::i32 user_file() const { return sgl::i32(files.size() - 1); }
    /// The side tables of the user file.
    [[nodiscard]] sgl::check::file_tables const& tables() const { return module.files[user_file()]; }
};

namespace sgl_test
{
inline library_prelude read_prelude()
{
    return {};
}

/// `sources` are the files of the module in order, the user file last.
inline checked_sources check_files(cc::span<cc::string_view const> sources)
{
    auto result = checked_sources();
    for (auto const source : sources)
        result.files.push_back(sgl::parse(source));
    for (auto const& file : result.files)
        result.asts.push_back(sgl::ast::build(file));

    // only now: a `module_file` is two references, and both vectors are complete
    auto prelude = cc::vector<sgl::check::module_file>();
    for (auto i = isize(0); i + 1 < result.files.size(); ++i)
        prelude.push_back({.file = result.files[i], .ast = result.asts[i]});
    result.module = sgl::check::check(prelude, {.file = result.files.back(), .ast = result.asts.back()});

    result.user = result.files.back();
    result.user_ast = result.asts.back();
    return result;
}

/// `user` behind ONE prelude file of the test's own, so the user file is file 1.
inline checked_sources check_sources(cc::string_view prelude, cc::string_view user)
{
    cc::string_view const sources[] = {prelude, user};
    return check_files(sources);
}

/// `user` behind the library's prelude, so the user file is file 2.
inline checked_sources check_sources(library_prelude, cc::string_view user)
{
    auto sources = cc::vector<cc::string_view>();
    for (auto const& p : sgl::prelude_files())
        sources.push_back(p.source);
    sources.push_back(user);
    return check_files(sources);
}

/// The diagnostics of the check pass, one per line, with the source they point at in place of an offset:
/// `unknown-name user:[foo] foo` is kind, file, the first line of the span, and the detail.
/// A file is named by its role, since its position moves whenever the prelude gains a file: the last one is `user`,
/// and a prelude file is `prelude`, or `prelude.<i>` where there are several.
inline cc::string reports_of(checked_sources const& s)
{
    auto out = cc::string();
    for (auto const& d : s.module.diagnostics)
    {
        auto text = s.files[d.file].text_of(d.what.where);
        if (auto const end = text.find('\n'); end >= 0)
            text = text.subview({.offset = 0, .size = end});
        auto const prelude_count = s.files.size() - 1;
        auto role = cc::string("user");
        if (d.file < prelude_count)
            role = prelude_count == 1 ? cc::string("prelude") : cc::format("prelude.{}", d.file);
        out.appendf("{} {}:[{}]", sgl::to_string(d.what.kind), role, text);
        if (!d.detail.empty())
            out.appendf(" {}", d.detail);
        out += "\n";
    }
    return out;
}

/// `user` checked against the library's prelude, as `reports_of` writes it.
inline cc::string reports_for(cc::string_view user)
{
    return reports_of(check_sources(read_prelude(), user));
}
} // namespace sgl_test
