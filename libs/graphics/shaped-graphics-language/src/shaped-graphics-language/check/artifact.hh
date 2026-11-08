#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics-language/check/check.hh>

/// A checked layer as bytes, and back: the compilation model's artifacts, so a process continues from a layer it never
/// checked
/// (`libs/graphics/shaped-graphics-language/docs/spec/incubator/compilation-model.md`, "Artifacts").
///
/// An artifact is a cache entry, never a file anyone keeps.
/// It reads back only under the `key` it was written with, which a caller makes from the compiler's build
/// (`compiler_fingerprint()`), and only against the same prelude sources; anything else, a truncated or a corrupt
/// artifact among them, is a miss.
/// So a miss is always safe: the caller checks the layer instead, and deleting every artifact never changes a result.
/// The bytes hold the interface and the bodies alike, and the files' parses are not among them: a load takes them.
namespace sgl::check
{
/// `layer` as bytes, under `key`; none where it holds what no artifact carries yet.
/// Only the prelude's layer is written so far: one of modules (`check_modules`) gives none, and so does one holding an
/// entry point's or a test's flat tree.
[[nodiscard]] cc::optional<cc::vector<byte>> save_artifact(checked_prelude const& layer, cc::string_view key);

/// What `save_artifact` wrote for the prelude of `prelude` under `key`, read back as the layer `check_prelude` gives.
/// None where `bytes` are anything else: another key, other prelude sources, or bytes cut short or changed.
/// The files and `builtins` must outlive the result, as they must `check_prelude`'s.
[[nodiscard]] cc::optional<checked_prelude> load_artifact(cc::span<byte const> bytes,
                                                          cc::string_view key,
                                                          cc::span<module_file const> prelude,
                                                          builtins::registry const& builtins);
} // namespace sgl::check
