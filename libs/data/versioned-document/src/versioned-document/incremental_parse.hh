#pragma once

#include <versioned-document/change_summary.hh>
#include <versioned-document/document_builder.hh>
#include <versioned-document/op_graph.hh>
#include <versioned-document/parse.hh>
#include <versioned-document/snapshot_cache.hh>

/// Evolving a typed document from one op to another, without re-parsing the whole thing.
///
/// The design is [the typed document](../../docs/concepts/the-typed-document.md#evolving-a-document) and
/// [interpretation](../../docs/concepts/interpretation.md#applying-an-op-incrementally).

/// Knobs on an incremental apply.
struct vdoc::incremental_apply_options
{
    /// How many ops back from `to` the chain walk may go before giving up and re-parsing in full.
    ///
    /// **A bound rather than a query.** Proving `to` descends from `from` exactly is the global ancestor test
    /// [decisions.md](../../docs/decisions.md#dominance-is-resolved-by-propagating-a-superseded-set-not-by-ancestor-queries)
    /// declines to pay for; a bounded walk is the cheap sufficient case for a mostly-linear history.
    isize max_chain_ops = 64;

    /// Where the filtered materialization may terminate.
    /// Null is correct and slow — the walk then goes back to the root on every apply.
    snapshot_cache* cache = nullptr;

    /// Compact when the document's dead arena bytes exceed this multiple of its live bytes; 0 never compacts.
    f64 compaction_ratio = 1.0;

    /// Take the slow path unconditionally, which is what the differential test pins the fast one against.
    bool force_full_reparse = false;
};

/// What one apply did, for a test that has to see which path ran.
struct vdoc::incremental_apply_stats
{
    bool took_fast_path = false;
    isize chain_ops = 0;
    isize touched_entities = 0;
    bool compacted = false;
};

namespace vdoc
{
/// Evolves `doc` from the document at `from` to the document at `to`, and says what changed.
///
/// **The fast path applies exactly where `to` reaches `from` through single-parent edges within
/// `options.max_chain_ops`.**
/// Single-parentage is what makes the chain's own assignments the complete delta: every op on it dominates what it
/// overwrites and contributes nothing else, so no untouched entity changed and none became multi-valued.
/// Multi-values *inside* the touched set are fine — those entities go through the full selection-and-construction
/// path, exactly as a parse would run it.
///
/// Everything else — a merge on the chain, a chain longer than the bound, a `to` that does not reach `from` at all —
/// re-materializes and re-parses, which costs time and never a result.
/// The two are defined to produce the same document and the same change summary.
///
/// `doc` is **consumed** — see [document_builder](document_builder.hh).
/// The caller owes that `doc` really is the document at `from`, which nothing here can check.
///
/// The report is **edited rather than appended to**: entries for the entities this apply re-decides are dropped
/// first, because carrying a stale diagnostic is a correctness trap.
/// The one exception is `unsupported_component_type`, which names no entity and is never retracted.
[[nodiscard]] document apply(document&& doc,
                             op_graph const& graph,
                             op_id const& from,
                             op_id const& to,
                             parse_policy const& policy,
                             parse_report& report,
                             change_summary& out_changes,
                             incremental_apply_options options = {},
                             incremental_apply_stats* stats = nullptr);
} // namespace vdoc
