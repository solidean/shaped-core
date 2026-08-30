#pragma once

#include <clean-core/error/optional.hh>

/// What THIS process may use, as opposed to what the machine has.
///
/// Inside a container the two are wildly different and only one of them is useful.
/// A CI runner on a 64-core host with a 4-CPU cgroup quota reports 64 through every conventional API — /proc/cpuinfo,
/// sysconf, std::thread::hardware_concurrency — and a thread pool sized off that oversubscribes by 16x.
///
/// **cc::system_info is never clamped to these limits**, and that is deliberate.
/// A stamp in a crash report wants "this was a 64-core host"; a thread pool wants "you may use 4".
/// Folding the two together would make them indistinguishable and destroy the first.
///
/// So: reach for `cc::recommended_worker_count()` when sizing anything, and for cc::system_info when describing the
/// machine.

/// The ceilings the OS puts on this process, as far as it will say.
struct cc::resource_limits
{
    /// Effective CPUs this process may use, which may be fractional — a cgroup quota of 1.5 is a normal setting.
    /// Absent where no quota is configured, which is the usual case outside a container.
    cc::optional<f32> cpu_quota;

    /// The hard memory ceiling, past which the process is killed rather than slowed.
    /// Absent where nothing imposes one.
    cc::optional<i64> memory_limit_bytes;

    /// How many logical cores this process may actually be scheduled on.
    /// The machine's count where nothing narrowed it, and 0 only where the platform cannot say.
    i32 affinity_cores = 0;

    /// Whether the usual container markers are present — a best-effort guess, and the doc says so because that is all
    /// it is.
    bool containerized = false;

    /// Whether the CPU reports a hypervisor.
    ///
    /// **This is NOT "we are in a VM".**
    /// Windows 11 turns on virtualization-based security by default, so the bit is set on ordinary bare-metal desktops
    /// where the OS itself runs in the root partition.
    /// It is named for what it measures because the reading everyone jumps to is the wrong one, and a hypervisor can
    /// hide the bit anyway.
    bool hypervisor_present = false;
};

namespace cc
{
/// The current limits, read fresh.
///
/// **Not memoized, unlike cc::get_system_info.**
/// A cgroup quota can be rewritten under a running process — that is what vertical autoscaling does — and an affinity
/// mask can be narrowed at any time, so a cached answer would go quietly stale.
/// Costs a few small file reads on Linux and one syscall elsewhere, which is nothing next to being wrong.
[[nodiscard]] cc::resource_limits query_resource_limits();

/// How many worker threads to start: the machine, the affinity mask and the CPU quota, whichever binds first.
///
/// **This is the number to size a thread pool from**, not `cc::get_system_info().logical_cores()` — that one describes
/// the machine and is the wrong answer inside every container.
/// Never less than 1, so a caller can divide by it.
[[nodiscard]] i32 recommended_worker_count();
} // namespace cc
