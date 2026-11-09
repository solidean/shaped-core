#pragma once

#include <clean-core/bytes/hash128.hh>
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
/// What names the artifact of the prelude of `prelude` under `key`: the key and every source it holds.
/// Equal ids are the same artifact, so a store keeps entries under it and never replaces one.
[[nodiscard]] cc::hash128 artifact_id(cc::string_view key, cc::span<module_file const> prelude);

/// What names the artifact of the layer of modules `program` reaches in `library` over `prelude`, as `artifact_id`.
/// None where `program` reaches no modules a layer holds.
[[nodiscard]] cc::optional<cc::hash128> modules_artifact_id(cc::string_view key,
                                                            checked_prelude const& prelude,
                                                            cc::span<module_file const> library,
                                                            module_file program);

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
} // namespace sgl::check
