#pragma once

#include "../check/check-test-support.hh"

#include <clean-core/math/bit.hh>
#include <shaped-graphics-language/test/run_tests.hh>

namespace sgl_test
{
using namespace cc::primitive_defines;

/// Every test of `source` run with `bindings`, each result that is no pass as its diagnostic, in the form `reports_of` writes.
/// The check pass reports nothing first, and a test that expects diagnostics is judged by them and left out.
inline cc::string driven_failures_of(cc::string_view source, sgl::check::driver_bindings const& bindings)
{
    auto checked = check_sources(read_prelude(), source);
    REQUIRE(reports_of(checked) == "");
    auto files = cc::vector<sgl::check::module_file>();
    for (auto i = isize(0); i < checked.files.size(); ++i)
        files.push_back({.file = *checked.files[i], .ast = *checked.asts[i]});

    auto shown = cc::vector<sgl::check::located_diagnostic>();
    auto const results = sgl::test::run_tests(checked.module, files, {.file = checked.user_file(), .bindings = bindings});
    REQUIRE(!results.empty());
    for (auto const& r : results)
        if (!r.is_passed())
            shown.push_back(sgl::test::diagnostic_of(checked.module, r));
    checked.module.diagnostics = cc::move(shown);
    return reports_of(checked);
}

/// Words as the bytes a driver binds: each little-endian, as `member_data` reads them.
inline cc::pinned_data<byte> bytes_of(cc::span<u32 const> words)
{
    auto result = cc::pinned_data<byte>::create_filled(words.size() * 4, byte(0));
    for (auto i = isize(0); i < words.size(); ++i)
        for (auto b = 0; b < 4; ++b)
            result[i * 4 + b] = byte(u8(words[i] >> (8 * b)));
    return result;
}

inline u32 bits_of(f32 v)
{
    return cc::bit_cast<u32>(v);
}

/// Floats as the bytes a driver binds.
template <class... F>
cc::pinned_data<byte> floats(F... values)
{
    u32 const words[] = {bits_of(f32(values))...};
    return bytes_of(words);
}

/// Element `i` of bytes laid out as `floats` writes them.
inline f32 float_at(cc::pinned_data<byte const> const& bytes, isize i)
{
    auto word = u32(0);
    for (auto b = 0; b < 4; ++b)
        word |= u32(bytes[i * 4 + b]) << (8 * b);
    return cc::bit_cast<f32>(word);
}
} // namespace sgl_test
