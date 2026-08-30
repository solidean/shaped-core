#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/platform/process_metrics.hh>
#include <clean-core/platform/resource_limits.hh>
#include <clean-core/platform/storage_devices.hh>
#include <clean-core/platform/system_metrics.hh>

/// Every level-valued reading, taken at one instant.
///
/// The third of cc's three concepts, and the one that only exists because the other two are not enough on their own:
///
/// - a **description** (platform/system_info.hh) is what cannot change while the process runs,
/// - a **snapshot** is what is true right now,
/// - a **sampler** is what changed since last time.
///
/// **Samplers are excluded on purpose, not merely omitted.**
/// A rate has no value at an instant: CPU load, disk I/O and network traffic are all differences between two readings,
/// and there is no honest number for any of them at a single moment.
/// Putting one in would mean either a zero that reads like an idle machine or a hidden baseline somewhere.
///
/// This is what a recording stamps at open and at close, and what a "how did the machine look when this happened"
/// question is answered from.
struct cc::resource_snapshot
{
    /// When this was taken, as epoch seconds — the clock to compare across processes and machines.
    f64 at_wall_secs = 0;

    /// Absent where the platform cannot answer, never zero-filled.
    cc::optional<cc::memory_usage> memory;
    cc::optional<cc::process_usage> process;

    /// The ceilings in force at this instant.
    ///
    /// Included even though they are not a "reading": a cgroup quota is rewritten under a running process by vertical
    /// autoscaling, so what bound the process at open may not be what bound it at close, and that difference is exactly
    /// what makes a stamp pair worth having.
    cc::resource_limits limits;

    /// Filesystem usage per mount, which is the one device family that answers without privileges anywhere.
    /// Empty where mounts could not be read.
    cc::vector<cc::mount_point> mounts;
};

namespace cc
{
/// Takes one snapshot, reading each level in turn.
///
/// Costs a handful of syscalls plus, on Linux, a few small file reads — cheap enough to stamp a recording with, and far
/// too expensive for a per-frame loop.
/// A caller wanting one reading should call that reading's own query instead.
///
/// **Never fails.** A part that cannot be read is absent from the result rather than failing the whole, since a
/// snapshot missing its disk section is still worth having.
[[nodiscard]] cc::resource_snapshot take_resource_snapshot();
} // namespace cc
