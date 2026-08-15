#pragma once

#include <clean-core/container/vector.hh>
#include <versioned-document/ids.hh>
#include <versioned-document/op.hh>

/// What a parse found: diagnostics, and the agreed multi-values it collapsed on the way.
///
/// The two lists are separate because they are different things — one is a problem, the other is housekeeping.
///
/// **Everything here is string-free.** A diagnostic is a kind plus the ids it concerns, and the kind, the path and
/// the raw document together recover the specifics, so a message can be written in any language later without
/// anything being re-parsed.
///
/// Both lists come out in a fixed order, because the parser only ever walks vectors sorted by id bytes: first what
/// selection found, entity by entity; then the unsupported types, by type; then what reading properties found, one
/// component type at a time.
/// Determinism is structural here rather than merely tested.
///
/// The design is [the concept](../../docs/concept.md#parsing-never-refuses).

/// One thing parsing found, and where.
enum class vdoc::diagnostic_kind : vdoc::u8
{
    /// The policy did not understand this component type, so it was skipped and its stored properties left untouched.
    /// Filed once per type per parse, not once per entity.
    unsupported_component_type,

    /// The stored `$schema_version` is newer than this build's, or the writers disagreed on it.
    /// The component was skipped, and its stored data is untouched for a build that does understand it.
    unknown_schema_version,

    /// Several writers disagreed, and the smallest op id won.
    multi_valued_conflict,

    /// Several writers disagreed, and the one inside the local closure won.
    remote_conflict,

    /// `$alive` was contested, so the thing stayed alive.
    /// Resurrecting is recoverable and vanishing is not, which is the whole reason this is a diagnostic and not a
    /// deletion.
    contested_alive,
};

/// One diagnostic: what happened, and the narrowest path that describes it.
///
/// A member of `path` is the empty id where the diagnostic is not that specific — `unsupported_component_type` names
/// only the component type, and an entity-level `contested_alive` only the entity.
struct vdoc::diagnostic
{
    diagnostic_kind kind = diagnostic_kind::unsupported_component_type;
    property_path path;

    /// The writer the resolution chose, or the all-zero id where nothing was chosen.
    op_id chosen_writer;

    /// How many writers were in play, or 0 where the kind does not involve writers.
    isize writer_count = 0;

    [[nodiscard]] friend bool operator==(diagnostic const&, diagnostic const&) = default;
};

/// A property that was structurally multi-valued but whose writers all wrote the same bytes.
///
/// Collapsed silently to that value, and kept only as a tidy-up hint for a later write.
/// This is **not** a problem, which is why it is not a diagnostic.
struct vdoc::agreed_multi_value
{
    property_path path;

    /// How many writers agreed; always at least two.
    isize writer_count = 0;

    [[nodiscard]] friend bool operator==(agreed_multi_value const&, agreed_multi_value const&) = default;
};

/// The out-parameter of a parse.
///
/// A parse appends rather than clears, so one report may collect several parses.
struct vdoc::parse_report
{
    cc::vector<diagnostic> diagnostics;
    cc::vector<agreed_multi_value> agreed_multi_values;

    [[nodiscard]] bool is_empty() const { return diagnostics.empty() && agreed_multi_values.empty(); }

    [[nodiscard]] isize count_of(diagnostic_kind kind) const;

    void clear();
};
