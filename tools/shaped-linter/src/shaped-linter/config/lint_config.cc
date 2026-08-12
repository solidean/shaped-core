#include "lint_config.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/container/span.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/string/char_predicates.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/glob.hh>
#include <shaped-linter/config/config_parser.hh>

namespace scl
{
namespace
{
cc::string lowered(cc::string_view s)
{
    cc::string out;
    for (auto const c : s)
        out += cc::to_lower(c);
    return out;
}

/// `path` seen from `base`, or an empty optional when it does not live there.
///
/// An empty `base` means the paths are already relative to it, which is what an in-memory config uses.
/// So does `.`, the working directory a run given repo-relative paths climbs to — every such path is
/// already spelled from there.
cc::optional<cc::string_view> path_under(cc::string_view base, cc::string_view path)
{
    if (base.empty() || base == ".")
        return path;
    if (!path.starts_with(base))
        return {};
    if (path.size() == base.size())
        return cc::string_view("");
    if (path[base.size()] != '/')
        return {};
    return path.subview(base.size() + 1);
}

bool any_glob_matches(cc::span<cc::string const> patterns, cc::string_view path)
{
    for (auto const& p : patterns)
        if (cc::glob_matches(p, path, {}))
            return true;
    return false;
}

/// The scalars behind a key: one for a scalar node, all of them for a list.
cc::result<cc::vector<cc::string>> read_scalars(config_document const& doc, isize id, cc::string_view key)
{
    auto const& n = doc[id];
    if (n.kind == config_value_kind::scalar)
    {
        cc::vector<cc::string> one;
        one.push_back(n.scalar);
        return one;
    }

    if (n.kind != config_value_kind::list)
        return cc::error(cc::format("line {}: '{}' takes a value or a list of them", n.line, key));

    cc::vector<cc::string> out;
    for (auto const child : n.children)
    {
        if (doc[child].kind != config_value_kind::scalar)
            return cc::error(cc::format("line {}: '{}' takes plain values, not a nested block", doc[child].line, key));
        out.push_back(doc[child].scalar);
    }
    return out;
}

cc::result<include_directive> read_entry(config_document const& doc, isize id, cc::string_view base_dir)
{
    auto const& n = doc[id];
    if (n.kind != config_value_kind::mapping)
        return cc::error(cc::format("line {}: every item of 'rules' must be a 'key: value' block", n.line));

    include_directive d;
    d.base_dir = cc::string(base_dir);

    auto saw_kind = false;
    auto saw_value = false;
    for (auto i = isize(0); i < n.keys.size(); ++i)
    {
        auto const& key = n.keys[i];
        auto const child = n.children[i];
        auto const line = doc[child].line;

        if (key == "kind")
        {
            if (doc[child].kind != config_value_kind::scalar)
                return cc::error(cc::format("line {}: 'kind' takes one value", line));
            auto const& k = doc[child].scalar;
            if (k == "allow-include")
                d.allow = true;
            else if (k == "deny-include")
                d.allow = false;
            else
                return cc::error(
                    cc::format("line {}: unknown rule kind '{}' — expected allow-include or deny-include", line, k));
            saw_kind = true;
        }
        else if (key == "value")
        {
            auto values = read_scalars(doc, child, "value");
            CC_RETURN_IF_ERROR(values);
            for (auto const& v : values.value())
                d.values.push_back(lowered(v));
            saw_value = true;
        }
        else if (key == "reason")
        {
            if (doc[child].kind != config_value_kind::scalar)
                return cc::error(cc::format("line {}: 'reason' takes one value", line));
            d.reason = doc[child].scalar;
        }
        else if (key == "files")
        {
            auto files = read_scalars(doc, child, "files");
            CC_RETURN_IF_ERROR(files);
            d.files = cc::move(files.value());
        }
        else if (key == "exclude-files")
        {
            auto files = read_scalars(doc, child, "exclude-files");
            CC_RETURN_IF_ERROR(files);
            d.exclude_files = cc::move(files.value());
        }
        else
        {
            return cc::error(cc::format("line {}: unknown key '{}' in a rule", line, key));
        }
    }

    if (!saw_kind)
        return cc::error(cc::format("line {}: a rule needs a 'kind'", n.line));
    if (!saw_value || d.values.empty())
        return cc::error(cc::format("line {}: a rule needs a 'value'", n.line));
    // The reason is what a finding prints, and an entry without one teaches the reader nothing.
    if (d.reason.empty())
        return cc::error(cc::format("line {}: a rule needs a 'reason' — it is what the finding prints", n.line));

    return d;
}
} // namespace

include_decision lint_config::classify_include(cc::string_view file_path, cc::string_view include) const
{
    auto const needle = lowered(include);

    include_decision decision;
    for (auto const& d : include_directives)
    {
        auto const rel = path_under(d.base_dir, file_path);
        if (!rel.has_value())
            continue;
        if (!d.files.empty() && !any_glob_matches(d.files, rel.value()))
            continue;
        if (any_glob_matches(d.exclude_files, rel.value()))
            continue;
        if (!any_glob_matches(d.values, needle))
            continue;

        // Last match wins, so a nearer config's narrower entry overrides the blanket one above it.
        decision = {.verdict = d.allow ? include_verdict::allowed : include_verdict::denied, .reason = d.reason};
    }
    return decision;
}

cc::result<cc::vector<include_directive>> read_include_directives(config_document const& doc, cc::string_view base_dir)
{
    cc::vector<include_directive> out;
    if (doc.root < 0)
        return out;

    auto const& root = doc[doc.root];
    for (auto i = isize(0); i < root.keys.size(); ++i)
    {
        // An unknown section is an error for the same reason an unknown key is: a misspelled `rules` would
        // otherwise disable every blessing in the file without a word.
        if (root.keys[i] != "rules")
            return cc::error(cc::format("line {}: unknown section '{}' — the only one is 'rules'",
                                        doc[root.children[i]].line, root.keys[i]));

        auto const& list = doc[root.children[i]];
        if (list.kind != config_value_kind::list)
            return cc::error(cc::format("line {}: 'rules' takes a list of '- kind: …' items", list.line));

        for (auto const item : list.children)
        {
            auto entry = read_entry(doc, item, base_dir);
            CC_RETURN_IF_ERROR(entry);
            out.push_back(cc::move(entry.value()));
        }
    }
    return out;
}

cc::result<cc::vector<include_directive>> load_include_directives(cc::string_view text, cc::string_view base_dir)
{
    auto doc = parse_config(text);
    CC_RETURN_IF_ERROR(doc);
    return read_include_directives(doc.value(), base_dir);
}
} // namespace scl
