#pragma once

#include <clean-core/platform/source_location.hh>
#include <clean-core/record/fwd.hh>

// cc::rec::desc — everything about a recording site that is known at compile time.
//
// A site defines exactly one `static constexpr desc`, and the event it writes carries a pointer to it and nothing else.
// So the name, the source location, the domain, the unit and the payload layout cost the stream zero bytes, and the
// call site costs one `lea`.
//
// **A descriptor is constant-initialized**, which is what keeps a site free of the guard variable and the one-time
// initialization dance a function-local `static` would otherwise carry.
// Never give one a member that cannot be built in a constant expression.

/// Where a recording site is in the source.
///
/// Deliberately not a cc::source_location: that type has no constructor, so a loader could never rebuild one, and a
/// recording that has been through a file would lose the one field that says where it came from.
/// The pointers are into the binary's string data and outlive the process; a deserialized one owns its strings instead.
struct cc::rec::source_ref
{
    char const* file = "";
    char const* function = "";
    u32 line = 0;

    /// Built at the call site from cc::source_location::current(), which is where the values actually come from.
    [[nodiscard]] static constexpr source_ref from(cc::source_location const& l)
    {
        return {.file = l.file_name(), .function = l.function_name(), .line = u32(l.line())};
    }
};

/// One field of an event payload, so a consumer that has never heard of the type can still print, filter and compare it.
struct cc::rec::field
{
    char const* name = "";
    rec::type_code type = rec::type_code::none;
    u16 offset = 0; ///< byte offset into the payload
    u16 size = 0;   ///< byte size of the field's fixed part
};

/// What a stat or a numeric value MEANS, as a static object the event points at.
///
/// Deliberately a struct and not an enum: everyone's enum of units is missing the one case the next consumer needs,
/// and adding a member here breaks nobody, while adding an enumerator forces every switch to be revisited.
/// Only analysis ever reads one, so its cost is a pointer in the descriptor.
struct cc::rec::unit
{
    char const* singular = "";
    char const* plural = "";
    char const* symbol = "";

    /// 1000 for SI quantities, 1024 for bytes, 0 for a quantity that takes no prefix at all.
    u32 prefix_base = 0;

    rec::axis_scale scale = rec::axis_scale::linear;
    rec::aggregation aggregate = rec::aggregation::sum;

    /// The axis range to prefer; equal values mean "fit to the data".
    f64 default_min = 0;
    f64 default_max = 0;

    /// Whether a rising value is good news, for coloring.
    /// Meaningless when the quantity has no direction.
    bool higher_is_better = false;
};

/// What a relation between trace ids MEANS, as a static object the event points at.
///
/// The same protocol as cc::rec::unit and for the same reason: an enum of relation kinds is missing the case the next
/// consumer needs, and adding a member here breaks nobody where adding an enumerator forces every switch to be
/// revisited.
///
/// The flags are not decoration.
/// `is_equivalence` says a reconstruction may union-find the members into one logical operation, which is exactly the
/// "these two turned out to be the same work" case; `inverse_name` is what lets a viewer render an edge from either
/// end without hardcoding a vocabulary.
struct cc::rec::relation_type
{
    /// How the edge reads from the subject: "parent_of".
    char const* name = "";

    /// How it reads from an object: "child_of". Empty when the relation is symmetric.
    char const* inverse_name = "";

    /// Order carries no meaning, and every member is a peer.
    bool is_symmetric = false;

    /// A relates to B and B to C implies A relates to C.
    bool is_transitive = false;

    /// Reflexive, symmetric and transitive: the members are interchangeable, and a reader may merge them.
    bool is_equivalence = false;
};

/// The compile-time half of one recording site.
///
/// `enable_bit` is precomputed rather than derived, so the gate is a single AND against the domain's mask.
/// `fixed_payload_size` is `variable_payload` for a site whose payload size is only known at the call — a formatted
/// log message, an inline string.
struct cc::rec::desc
{
    /// A payload whose size the descriptor cannot state.
    static constexpr u32 variable_payload = ~u32(0);

    rec::event_kind kind = rec::event_kind::invalid;
    rec::level lvl = rec::level::info;

    /// The single bit in domain::enabled_mask this site gates on.
    u32 enable_bit = 0;

    /// The scope / stat / value name, or — for a log site with no format arguments — the message itself.
    char const* name = "";

    rec::unit const* quantity = nullptr;

    /// What a trace_relation site's edge means; null for every other kind.
    /// A second meta pointer rather than one slot shared with `quantity`: eight bytes on a static object is nothing
    /// next to a `void const*` whose meaning a reader has to derive from `kind`.
    rec::relation_type const* relation = nullptr;

    rec::domain* dom = nullptr;

    rec::source_ref site;

    rec::field const* fields = nullptr;
    u16 field_count = 0;

    u32 fixed_payload_size = 0;
};
