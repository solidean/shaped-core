#pragma once

#include "../check/check-test-support.hh"

#include <shaped-graphics-language/check/flat_builder.hh>
#include <shaped-graphics-language/interpret/interpret.hh>
#include <shaped-graphics-language/legalize/core.hh>
#include <shaped-graphics-language/legalize/legalize.hh>

namespace sgl_test
{
/// A module that holds the prelude, the two edge structs a hand-built entry point stands between, and `store`,
/// a binding of two buffers that a tree may read and write once it lists it.
/// A rule test builds its tree by hand, so the module's one function only has to check.
inline checked_sources flat_test_module()
{
    auto checked = check_sources(read_prelude(), "struct frag:\n"
                                                 "    a: float\n"
                                                 "    b: float\n"
                                                 "\n"
                                                 "binding store:\n"
                                                 "    data: mut buffer[float]\n"
                                                 "    counts: mut buffer[int]\n"
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

/// The binding `store` of `flat_test_module`, or `none` in a module without it.
inline sgl::check::symbol_id store_binding(sgl::check::checked_module const& m)
{
    for (auto i = isize(0); i < m.symbols.size(); ++i)
        if (m.symbols[i].kind == sgl::check::symbol_kind::binding && m.symbols[i].name == "store")
            return sgl::check::symbol_id(i);
    return sgl::check::symbol_id::none;
}

/// How many elements each buffer of `store` holds in `test_inputs`.
constexpr auto k_store_elements = 4;

/// `frag { a = 0.25, b = 0.75 }`, and where the module has `store`, `data = [0.5, 1.5, 2.5, 3.5]` and `counts = [1, 2, 3, 4]`.
inline sgl::check::run_inputs test_inputs(sgl::check::checked_module const& m)
{
    auto inputs = sgl::check::run_inputs{
        .parameter = sgl::check::zero_value(m, sgl::check::flat_builder{.m = m}.type_named("frag"))};
    inputs.parameter.leaves[0] = sgl::check::scalar::of(0.25f);
    inputs.parameter.leaves[1] = sgl::check::scalar::of(0.75f);

    if (auto const store = store_binding(m); sgl::check::is_valid(store))
    {
        auto data = sgl::check::buffer_contents{.binding = store, .member = 0};
        auto counts = sgl::check::buffer_contents{.binding = store, .member = 1};
        for (auto i = 0; i < k_store_elements; ++i)
        {
            data.leaves.push_back(sgl::check::scalar::of(float(i) + 0.5f));
            counts.leaves.push_back(sgl::check::scalar::of(i + 1));
        }
        inputs.buffers.push_back(cc::move(data));
        inputs.buffers.push_back(cc::move(counts));
    }
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
