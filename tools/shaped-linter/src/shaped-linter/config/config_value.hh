#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-linter/fwd.hh>

/// What a config node holds.
/// Three kinds is the whole data model — the format has no numbers, booleans or nulls, and a consumer
/// that wants one parses the scalar itself.
enum class scl::config_value_kind : scl::u8
{
    scalar,
    list,
    mapping,
};

/// One node of a parsed config, in an arena addressed by `isize` id — the same shape as `syntax_tree`.
///
/// `scalar` holds the text for a scalar node and nothing else.
/// `children` are the item ids of a list, or the value ids of a mapping; a mapping's `keys` runs parallel
/// to `children`, so entry `i` is `keys[i]` -> `children[i]`.
/// Duplicate keys are kept rather than collapsed, so a consumer can reject them with a line number.
struct scl::config_node
{
    config_value_kind kind = config_value_kind::scalar;
    u32 line = 0; // 1-based, where this node starts

    cc::string scalar;
    cc::vector<isize> children;
    cc::vector<cc::string> keys;
};

/// A parsed `.shaped-lint.yml`.
/// `root` is the top-level mapping, and is -1 only for an empty file.
struct scl::config_document
{
    cc::vector<config_node> nodes;
    isize root = -1;

    config_node const& operator[](isize id) const { return nodes[id]; }

    /// The value id for `key` in the mapping node `id`, or -1 when it has no such entry.
    /// Returns the FIRST of a duplicated key.
    isize find(isize id, cc::string_view key) const;
};
