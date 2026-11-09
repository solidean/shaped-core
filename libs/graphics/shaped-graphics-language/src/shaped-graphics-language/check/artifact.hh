#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics-language/check/check.hh>

/// A checked layer as bytes, and back, so a process continues from a layer it never checked.
/// These are the compilation model's artifacts: `libs/graphics/shaped-graphics-language/docs/spec/incubator/compilation-model.md`.
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
/// The prelude's layer and one of modules (`check_modules`) alike; a test's flat tree among them keeps no names its
/// mint did not take itself, which only an emitter would read.
[[nodiscard]] cc::optional<cc::vector<byte>> save_artifact(checked_prelude const& layer, cc::string_view key);

/// What `save_artifact` wrote for the prelude of `prelude` under `key`, read back as the layer `check_prelude` gives.
/// None where `bytes` are anything else: another key, other prelude sources, or bytes cut short or changed.
/// The files and `builtins` must outlive the result, as they must `check_prelude`'s.
[[nodiscard]] cc::optional<checked_prelude> load_artifact(cc::span<byte const> bytes,
                                                          cc::string_view key,
                                                          cc::span<module_file const> prelude,
                                                          builtins::registry const& builtins);

/// What `save_artifact` wrote under `key` for a layer of modules (`check_modules`), read back for `program`.
/// It is the layer of the modules `program` reaches in `library`, planned as `check_modules` plans them, over `prelude`.
/// None where `bytes` are anything else: another key, other sources of any file the layer holds, the prelude's
/// included, or bytes cut short or changed; and where `program` reaches no modules a layer holds.
/// `prelude`, the library's files and `builtins` must outlive the result, as they must `check_modules`'s.
[[nodiscard]] cc::optional<checked_prelude> load_modules_artifact(cc::span<byte const> bytes,
                                                                  cc::string_view key,
                                                                  checked_prelude const& prelude,
                                                                  cc::span<module_file const> library,
                                                                  module_file program,
                                                                  builtins::registry const& builtins);

/// The link `link` adds to its chain as bytes, under `key`: its own part, and where the parts of the links it was
/// checked over lay, which a load moves it past.
/// `base` is the link it was checked over, or the prelude's layer for the chain's first.
/// None for a layer that is no link of a chain, one checked with an option's value, or one holding what no artifact
/// carries yet.
[[nodiscard]] cc::optional<cc::vector<byte>> save_link_artifact(checked_prelude const& link,
                                                                checked_prelude const& base,
                                                                cc::string_view key);

/// What `save_link_artifact` wrote under `key` for `module` of `library`, read back as the link `check_next_module`
/// makes of it over `base`, moved past every link of `base`'s chain that changed since (`relocate_link`).
/// None where `bytes` are anything else: another key, other sources of the module's files or the prelude's, or bytes
/// cut short or changed; and where `base` is a chain of other modules, or one a fresh check of the module could read
/// otherwise.
/// `base` and the library's files must outlive the result, as they must `check_next_module`'s.
[[nodiscard]] cc::optional<checked_prelude> load_link_artifact(cc::span<byte const> bytes,
                                                               cc::string_view key,
                                                               checked_prelude const& base,
                                                               cc::span<module_file const> library,
                                                               cc::string_view module);
} // namespace sgl::check
