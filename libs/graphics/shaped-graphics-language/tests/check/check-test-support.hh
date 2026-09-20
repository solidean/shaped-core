#pragma once

#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>
#include <nexus/test.hh>
#include <shaped-graphics-language/ast/build.hh>
#include <shaped-graphics-language/check/check.hh>
#include <shaped-graphics-language/check/dump.hh>
#include <shaped-graphics-language/syntax/parsed_file.hh>

namespace sgl_test
{
using namespace cc::primitive_defines;

struct checked_sources;

inline cc::string read_text(cc::string_view path)
{
    auto adapter = cc::file_read_stream_adapter::open(path);
    REQUIRE(adapter.has_value());
    auto stream = adapter.value().stream();
    auto const bytes = stream.read_all();
    REQUIRE(bytes.has_value());
    return cc::string(reinterpret_cast<char const*>(bytes.value().data()), bytes.value().size());
}

/// The library's own prelude, which every test here checks against unless it brings one.
inline cc::string read_prelude()
{
    return read_text(cc::string(SGL_PRELUDE_DIR) + "/prelude.sgl");
}

} // namespace sgl_test

/// Both files of a module and what the check pass made of them, kept together so a test can read spans back.
struct sgl_test::checked_sources
{
    sgl::parsed_file prelude;
    sgl::parsed_file user;
    sgl::ast::file_ast prelude_ast;
    sgl::ast::file_ast user_ast;
    sgl::check::checked_module module;
};

namespace sgl_test
{
inline checked_sources check_sources(cc::string_view prelude, cc::string_view user)
{
    auto result = checked_sources{.prelude = sgl::parse(prelude), .user = sgl::parse(user)};
    result.prelude_ast = sgl::ast::build(result.prelude);
    result.user_ast = sgl::ast::build(result.user);
    result.module = sgl::check::check({.file = result.prelude, .ast = result.prelude_ast},
                                      {.file = result.user, .ast = result.user_ast});
    return result;
}

/// The diagnostics of the check pass, one per line, with the source they point at in place of an offset:
/// `unknown-name 1:[foo] foo` is kind, file, the first line of the span, and the detail.
inline cc::string reports_of(checked_sources const& s)
{
    auto out = cc::string();
    for (auto const& d : s.module.diagnostics)
    {
        auto text = (d.file == 0 ? s.prelude : s.user).text_of(d.what.where);
        if (auto const end = text.find('\n'); end >= 0)
            text = text.subview({.offset = 0, .size = end});
        out.appendf("{} {}:[{}]", sgl::to_string(d.what.kind), d.file, text);
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
