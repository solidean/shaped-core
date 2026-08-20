#include "overhead.hh"

#include <clean-core/common/time.hh>
#include <clean-core/container/span.hh>
#include <clean-core/record/domain.hh>
#include <clean-core/record/record.hh>
#include <clean-core/record/system.hh>
#include <clean-core/record/writer.hh>

namespace
{
using namespace cc::primitive_defines;

/// Events per round.
/// Large enough that the loop's own overhead disappears into the measurement, small enough that a round fits
/// comfortably in one chunk at any sane configuration.
constexpr isize events_per_round = 2000;

/// Rounds per sample point.
/// The minimum across them is what gets reported.
constexpr int rounds = 5;

/// The payload the per-byte term is fitted from.
/// Far enough from zero that the difference is not noise, small enough to stay a plain copy.
constexpr isize wide_payload_bytes = 256;

constexpr cc::rec::desc probe_desc = {
    .kind = cc::rec::event_kind::value,
    .enable_bit = cc::rec::enable_bit_of(cc::rec::category::logging),
    .name = "record.overhead_probe",
    .dom = &cc::rec::g_system_domain,
    .fixed_payload_size = cc::rec::desc::variable_payload,
};

cc::rec::overhead_model g_model = {
    // A deliberately conservative stand-in until something measures: roughly what a modern x86 core does with a
    // timestamp, a bounds check and a publish.
    .fixed_cycles = 40,
    .cycles_per_byte = 0.5,
    .disabled_cycles = 2,
    .is_measured = false,
};

/// The cheapest per-event cost across `rounds`, in cycles.
f64 measure_per_event(cc::span<byte const> payload)
{
    auto best = 0.0;
    for (auto r = 0; r < rounds; ++r)
    {
        auto const begin = cc::current_cycles();
        for (isize i = 0; i < events_per_round; ++i)
            cc::rec::record_event(probe_desc, payload);
        auto const end = cc::current_cycles();

        auto const per_event = f64(end - begin) / f64(events_per_round);
        if (best == 0.0 || per_event < best)
            best = per_event;
    }
    return best;
}
} // namespace

cc::rec::overhead_model cc::rec::measure_overhead()
{
    if (!rec::is_initialized())
        return g_model;

    // The disabled cost is measured with the site's own category silenced, which is exactly what a disabled site is.
    auto const saved_mask = rec::g_system_domain.enabled_mask();
    rec::g_system_domain.set_enabled(rec::category::logging, false);

    auto disabled = 0.0;
    for (auto r = 0; r < rounds; ++r)
    {
        auto const begin = cc::current_cycles();
        for (isize i = 0; i < events_per_round; ++i)
            rec::record_event(probe_desc);
        auto const end = cc::current_cycles();

        auto const per_event = f64(end - begin) / f64(events_per_round);
        if (disabled == 0.0 || per_event < disabled)
            disabled = per_event;
    }

    rec::g_system_domain.set_enabled_mask(saved_mask);

    byte wide[wide_payload_bytes] = {};
    auto const narrow_cost = measure_per_event(cc::span<byte const>());
    auto const wide_cost = measure_per_event(cc::span<byte const>(wide, wide_payload_bytes));

    auto model = rec::overhead_model{
        .fixed_cycles = narrow_cost,
        .cycles_per_byte = (wide_cost - narrow_cost) / f64(wide_payload_bytes),
        .disabled_cycles = disabled,
        .is_measured = true,
    };

    // A negative slope means the two points were closer than the noise; reporting zero is more honest than reporting
    // that bytes make recording faster.
    if (model.cycles_per_byte < 0)
        model.cycles_per_byte = 0;

    g_model = model;
    return model;
}

cc::rec::overhead_model const& cc::rec::overhead()
{
    return g_model;
}

void cc::rec::set_overhead(cc::rec::overhead_model const& model)
{
    g_model = model;
}
