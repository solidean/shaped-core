#pragma once

#include <clean-core/container/map.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/function/unique_function.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-linter/config/lint_config.hh>
#include <shaped-linter/fwd.hh>

namespace scl
{
/// The per-directory config file name.
inline constexpr cc::string_view k_config_file_name = ".shaped-lint.yml";
} // namespace scl

/// Finds the configs that apply to a file and merges them, once per directory.
///
/// From the file's directory it walks up to the filesystem root, reading a `.shaped-lint.yml` wherever it
/// finds one, and merges them root-first so a nearer entry decides.
/// Results are cached per directory, which is what keeps a 200-file batch from re-reading the same three
/// files 200 times.
///
/// A missing config is the normal case and says nothing.
/// A config that fails to parse is reported through `errors()` and otherwise treated as empty — the run
/// then fails as a whole rather than silently linting against half a policy.
///
/// This is the only part of the lint pipeline that touches the filesystem: the engine is handed a resolved
/// `lint_config`, so a test never reaches disk.
struct scl::config_resolver
{
    /// Reads the real filesystem.
    config_resolver();

    /// Reads through `read_file` instead, which returns the file's text or nothing when it is absent.
    /// This is the seam the resolver's own tests drive the walk through, so they need no directories.
    explicit config_resolver(cc::unique_function<cc::optional<cc::string>(cc::string_view)> read_file);

    lint_config const& resolve(cc::string_view file_path);

    [[nodiscard]] cc::span<cc::string const> errors() const { return _errors; }

private:
    /// The merged config for a directory, keyed by its normalized path.
    lint_config const& for_directory(cc::string_view dir);

    cc::unique_function<cc::optional<cc::string>(cc::string_view)> _read_file;
    cc::map<cc::string, lint_config> _by_directory;
    cc::vector<cc::string> _errors;
};
