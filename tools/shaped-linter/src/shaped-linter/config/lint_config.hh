#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-linter/config/config_value.hh>
#include <shaped-linter/fwd.hh>

namespace scl
{
struct include_decision;

/// What the config says about one include in one file.
/// `unblessed` is the DEFAULT — every non-project include is denied until something allows it, and a
/// `denied` verdict differs only in carrying a reason that names the replacement.
enum class include_verdict : u8
{
    allowed,
    denied,
    unblessed,
};
} // namespace scl

/// One `allow-include` / `deny-include` entry, as the file spelled it.
///
/// `values` are lowercased include spellings (`<atomic>`), each a glob in its own right, so `<d3d12*.h>`
/// covers a family.
/// `files` / `exclude_files` are globs over the source path **relative to `base_dir`** — the directory of
/// the config that declared the entry, which is what lets clean-core's file say `src/clean-core/fwd.hh`
/// while a root entry says `libs/base/clean-core/**`.
/// An entry whose `files` is empty applies to every file under `base_dir`.
struct scl::include_directive
{
    bool allow = false;
    cc::vector<cc::string> values;
    cc::string reason;
    cc::vector<cc::string> files;
    cc::vector<cc::string> exclude_files;
    cc::string base_dir;
};

/// The verdict plus the reason the deciding entry carried, which is what a finding prints.
struct scl::include_decision
{
    include_verdict verdict = include_verdict::unblessed;
    cc::string_view reason;
};

/// Every config that applies to one file, already merged.
///
/// `include_directives` is ordered root-first, nearest-last, and the **last** matching entry decides — so a
/// library re-opens what the root closed by naming the narrower case later, and never by editing the root.
/// A nearer config extends its ancestors; nothing replaces them.
///
/// An empty `include_directives` means no config above this file said anything about includes, and the include rule then stays silent.
/// That is what keeps a test snippet, a corpus block and an as-yet-unconfigured directory out of the gate.
struct scl::lint_config
{
    cc::vector<include_directive> include_directives;

    /// The nearest config file found, normalized — where a new blessing should be written.
    /// Empty when there was none.
    cc::string nearest_config_path;

    bool checks_includes() const { return !include_directives.empty(); }

    /// Decide one include for one file.
    /// `file_path` must already be normalized (see `cc::glob_normalize_path`); `include` is the spelling as written,
    /// brackets included (`<atomic>`), and is lowercased here.
    include_decision classify_include(cc::string_view file_path, cc::string_view include) const;
};

namespace scl
{

/// Read a parsed document's `rules:` section into directives, tagging each with `base_dir`.
///
/// Every entry needs a `kind`, a `value` and a `reason`; `files` and `exclude-files` are optional, and each
/// of the three list-shaped fields takes either one scalar or a list.
/// An unknown key or an unknown `kind` is an ERROR — a typo must fail the run rather than quietly bless
/// nothing.
cc::result<cc::vector<include_directive>> read_include_directives(config_document const& doc, cc::string_view base_dir);

/// `parse_config` then `read_include_directives`, for a config file's text.
cc::result<cc::vector<include_directive>> load_include_directives(cc::string_view text, cc::string_view base_dir);

} // namespace scl
