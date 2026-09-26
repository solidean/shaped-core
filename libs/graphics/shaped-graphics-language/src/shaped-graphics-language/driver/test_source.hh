#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics-language/check/symbols.hh>

/// One SGL source checked against the prelude with its tests run: what `sgl test` and the corpus read.
struct sgl::tested_source
{
    /// Every error of every phase, then every test of the source that did not pass, one diagnostic per line with its
    /// notes under it, as `compile_to_text` writes them; a diagnostic an `@expect` names is in neither list.
    cc::string errors;
    cc::string warnings;
    /// Every test of the source, the ones judged by the diagnostics they expect included.
    i32 test_count = 0;
    /// The tests an `@expect` of a diagnostic judges, which never run.
    i32 tests_expecting_diagnostics = 0;
    /// The tests that ran, and of those the ones that passed.
    i32 tests_run = 0;
    i32 tests_passed = 0;
    /// The source's entry points, which a caller emits one by one.
    struct entry_point
    {
        cc::string name;
        check::stage stage = check::stage::none;
    };
    cc::vector<entry_point> entry_points;

    /// Nothing to report at all: no error, no warning, and no test that did not pass.
    [[nodiscard]] bool is_clean() const { return errors.empty() && warnings.empty(); }
};

namespace sgl
{
/// Parses and checks `source` behind the prelude and runs its tests, which only the source's own are.
/// Total and deterministic, like `compile_to_text`: a source that does not check still has its errors reported, and
/// its tests that have a flat tree still run.
[[nodiscard]] tested_source test_source(cc::string_view source, cc::string_view source_name = "<sgl>");
} // namespace sgl
