#pragma once

#include "../check/check-test-support.hh"

#include <shaped-graphics-language/check/flat_builder.hh>
#include <shaped-graphics-language/interpret/interpret.hh>
#include <shaped-graphics-language/legalize/core.hh>
#include <shaped-graphics-language/legalize/legalize.hh>

namespace sgl_test
{
/// A module that holds the prelude and the two edge structs a hand-built entry point stands between.
/// A rule test builds its tree by hand, so the module's one function only has to check.
inline checked_sources flat_test_module()
{
    auto checked = check_sources(read_prelude(), "struct frag:\n"
                                                 "    a: float\n"
                                                 "    b: float\n"
                                                 "\n"
                                                 "@pixel struct target:\n"
                                                 "    color: float4\n"
                                                 "\n"
                                                 "@pixel fun seed_ps(p: frag) -> target:\n"
                                                 "    return {\n"
                                                 "        color = float4(p.a, p.b, 0.0, 1.0)\n"
                                                 "    }\n");
    CHECK(reports_of(checked) == "");
    return checked;
}

/// A builder for `fun main(p: frag) -> float`, which is what the interpreter's tests run.
inline sgl::check::flat_builder float_function(sgl::check::checked_module const& m)
{
    auto const float_type = sgl::check::flat_builder{.m = m}.type_named("float");
    auto const frag = sgl::check::flat_builder{.m = m}.type_named("frag");
    return sgl::check::flat_builder::create(m, {.input = frag, .result = float_type});
}

/// `frag { a = 0.25, b = 0.75 }`.
inline sgl::check::run_inputs test_inputs(sgl::check::checked_module const& m)
{
    auto inputs = sgl::check::run_inputs{
        .parameter = sgl::check::zero_value(m, sgl::check::flat_builder{.m = m}.type_named("frag"))};
    inputs.parameter.leaves[0] = sgl::check::scalar::of(0.25f);
    inputs.parameter.leaves[1] = sgl::check::scalar::of(0.75f);
    return inputs;
}

/// The body of the dump: every line after the `(entry …` line, without the closing parenthesis of the entry.
/// A rule test pins this, since the header is the same in all of them.
inline cc::string body_dump(sgl::check::checked_module const& m, sgl::check::flat_entry_point const& e)
{
    auto const text = sgl::check::dump_entry_point(m, e);
    auto const first = text.find('\n');
    if (first < 0 || text.size() < first + 3)
        return "";
    // drops the header line in front, and `)\n` behind
    return cc::string(cc::string_view(text).subview({.start = first + 1, .end = text.size() - 2})) + "\n";
}
} // namespace sgl_test
