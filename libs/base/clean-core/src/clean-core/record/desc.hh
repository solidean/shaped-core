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
    rec::domain* dom = nullptr;

    cc::source_location site = {};

    rec::field const* fields = nullptr;
    u16 field_count = 0;

    u32 fixed_payload_size = 0;
};
