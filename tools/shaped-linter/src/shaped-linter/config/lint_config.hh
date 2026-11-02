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

} // namespace scl

/// What the config says about one include in one file.
/// `unblessed` is the DEFAULT — every non-project include is denied until something allows it, and a
/// `denied` verdict differs only in carrying a reason that names the replacement.
enum class scl::include_verdict : scl::u8
{
    allowed,
    denied,
    unblessed,
};

/// What an entry is about: an include spelling, a blocking wait called from the files it names, or a reference bound to awaiting a call.
enum class scl::directive_subject : scl::u8
{
    include,            // allow-include / deny-include: `values` are include spellings
    blocking_wait,      // allow-blocking-wait / deny-blocking-wait: `values` are the names of blocking calls
    co_await_temporary, // allow-co-await-temporary / deny-co-await-temporary: `values` are the names of awaited calls
};

/// One entry of a `rules:` section, as the file spelled it: an include, blocking-wait or co-await-temporary allow or deny.
///
/// `values` are lowercased include spellings (`<atomic>`), each a glob in its own right, so `<d3d12*.h>`
/// covers a family.
/// `files` / `exclude_files` are globs over the source path **relative to `base_dir`** — the directory of
/// the config that declared the entry, which is what lets clean-core's file say `src/clean-core/fwd.hh`
/// while a root entry says `libs/base/clean-core/**`.
/// An entry whose `files` is empty applies to every file under `base_dir`.
struct scl::include_directive
{
    directive_subject subject = directive_subject::include;
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

    bool checks_includes() const { return any_directive_about(directive_subject::include); }

    /// Whether any config above this file said something about blocking waits; the rule stays silent otherwise.
    bool checks_blocking_waits() const { return any_directive_about(directive_subject::blocking_wait); }

    /// Whether any config above this file said something about awaiting temporaries; the rule stays silent otherwise.
    bool checks_co_await_temporaries() const { return any_directive_about(directive_subject::co_await_temporary); }

    bool any_directive_about(directive_subject subject) const
    {
        for (auto const& d : include_directives)
            if (d.subject == subject)
                return true;
        return false;
    }

    /// Decide one include for one file.
    /// `file_path` must already be normalized (see `cc::glob_normalize_path`); `include` is the spelling as written,
    /// brackets included (`<atomic>`), and is lowercased here.
    include_decision classify_include(cc::string_view file_path, cc::string_view include) const;

    /// Decide one call of a blocking wait named `name` in one file.
    /// The last matching entry decides; with none matching the verdict is `unblessed`, which the rule treats as allowed —
    /// unlike an include, a call is only a finding where a config denied it.
    include_decision classify_blocking_wait(cc::string_view file_path, cc::string_view name) const;

    /// Decide one reference bound to awaiting a call to `name` in one file, as classify_blocking_wait decides a call:
    /// with no entry matching it is allowed.
    include_decision classify_co_await_temporary(cc::string_view file_path, cc::string_view name) const;

private:
    /// The last entry about `subject` that matches the file and `name` decides; `name` is lowercased here.
    include_decision classify_named(directive_subject subject, cc::string_view file_path, cc::string_view name) const;
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
