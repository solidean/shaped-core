#pragma once

#include "../check/check-test-support.hh"

#include <shaped-graphics-language/emit/emit.hh>

namespace sgl_test
{
/// `user` checked against the library's prelude, and its entry point `index` written for `t`.
/// The program must check without a diagnostic: a test of the emitters is no test of the check pass.
inline sgl::emit::emitted_text emit_source(cc::string_view user, isize index, sgl::emit::target t)
{
    auto const checked = check_sources(read_prelude(), user);
    CHECK(reports_of(checked) == "");
    return sgl::emit::emit(checked.module, index, t);
}

/// The same, with the emit's options.
inline sgl::emit::emitted_text emit_source(cc::string_view user,
                                           isize index,
                                           sgl::emit::target t,
                                           sgl::emit::options const& options)
{
    auto const checked = check_sources(read_prelude(), user);
    CHECK(reports_of(checked) == "");
    return sgl::emit::emit(checked.module, index, t, options);
}
} // namespace sgl_test
