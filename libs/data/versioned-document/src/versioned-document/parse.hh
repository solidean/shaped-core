#pragma once

#include <versioned-document/document.hh>
#include <versioned-document/op_graph.hh>

/// The parser and the policy that bakes in the library's conventions.
///
/// The design is [the concept](../../docs/concept.md#interpretation).

/// The conventions, ready to use: registry lookup, `$alive`-based deletion, and local-closure conflict resolution.
///
/// Built by a factory rather than a constructor, because the local-closure variant collects the closure eagerly and a
/// constructor doing that reads like a cheap one.
class vdoc::default_parse_policy final : public parse_policy
{
    // construction
public:
    default_parse_policy() = default;

    /// Registry lookup and `$alive` deletion, with no local branch: a genuine conflict resolves by smallest op id.
    /// `registry` must outlive the policy.
    [[nodiscard]] static default_parse_policy create_with_registry(component_registry const& registry);

    /// Adds the local-closure branch, collected here once.
    ///
    /// When exactly one surviving writer is inside the closure of `local_head`, that value wins and a remote_conflict
    /// diagnostic reports it — which is the bias toward the local user, and the only reason the graph is needed.
    [[nodiscard]] static default_parse_policy create_with_local_head(component_registry const& registry,
                                                                     op_graph const& graph,
                                                                     op_id const& local_head);
    [[nodiscard]] static default_parse_policy create_with_local_heads(component_registry const& registry,
                                                                      op_graph const& graph,
                                                                      cc::span<op_id const> local_heads);

    // interpretation
public:
    [[nodiscard]] component_schema const* query_component_schema(component_type_id type) const override;

    /// `$alive` on the `$entity` component type, by the unambiguously-false rule.
    [[nodiscard]] bool should_instantiate_entity(entity_id entity,
                                                 raw_entity const& raw,
                                                 parse_report& report) const override;

    [[nodiscard]] cc::optional<value_view> resolve_multi_value(property_path const& path,
                                                               cc::span<property_value const> candidates,
                                                               parse_report& report) const override;

    // queries
public:
    /// Whether the local-closure branch is available at all.
    [[nodiscard]] bool has_local_closure() const { return !_local_closure.empty(); }

private:
    component_registry const* _registry = nullptr;

    /// The ops reachable from the local heads, sorted by op id bytes as collect_reachable returns them.
    /// Empty means there is no local branch, and every conflict resolves by smallest op id.
    cc::vector<op_id> _local_closure;
};

namespace vdoc
{
/// Whether something is alive, by the unambiguously-false rule.
///
/// Dead only when every surviving writer of `$alive` says false.
/// A contested `$alive` stays alive and files a diagnostic, because resurrecting is recoverable and vanishing is not,
/// and a non-boolean value counts as not-false for the same reason.
///
/// `$alive` deliberately never goes through property_reader::try_get, so no policy can ever vote a thing out of
/// existence.
[[nodiscard]] bool is_alive(raw_component const& raw, property_path const& path, parse_report& report);

/// Interprets a raw document into a typed one.
///
/// **This never fails.**
/// There is no input for which it refuses to produce a document: an unknown component type, an unknown schema version
/// and an unresolvable conflict each become an entry in `report` while the rest of the document loads.
/// A build that does not understand a component is not entitled to refuse the document that contains it.
///
/// Nothing is mutated — not the raw document, and not the op_graph behind it.
/// The report is appended to rather than cleared, so one report may collect several parses.
///
/// The result owns copies of everything it keeps, so it outlives both `raw` and that graph.
[[nodiscard]] document parse(raw_document const& raw, parse_policy const& policy, parse_report& report);
} // namespace vdoc
