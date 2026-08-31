#pragma once

#include <clean-core/error/optional.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics/fwd.hh>

/// Which GPU a context is actually running on.
///
/// Read it for a log line or a bug report, and to key anything the driver produced: a serialized pipeline blob is
/// only valid for the adapter and driver that wrote it, and nothing else in sg identifies those.

struct sg::adapter_info
{
    /// Human-readable, as the driver reports it — "NVIDIA GeForce RTX 4090", "Microsoft Basic Render Driver".
    cc::string name;

    /// PCI vendor and device ids, 0 where the backend cannot report them.
    u32 vendor_id = 0;
    u32 device_id = 0;

    /// The user-mode driver's version, formatted by the backend that read it.
    ///
    /// OPAQUE: compare it for equality, never parse or order it — dx12 and vulkan encode entirely different things,
    /// and a vendor is free to change its own encoding.
    /// Empty where the backend cannot report one, which must read as "unknown" rather than as a version.
    cc::string driver_version;

    /// Memory physically on the card, as the adapter reports it.
    ///
    /// A property of the hardware, so it belongs here rather than with the live readings.
    /// **Never compare it against sg::gpu_memory_usage::budget_bytes as if they were the same scale.**
    /// The budget is what this process may use right now and shrinks as other processes take memory; this is what the
    /// board has.
    /// Reporting one as a fraction of the other is how a dashboard says a card is 20% full while the driver is paging.
    /// Absent where the backend cannot report it, and 0 is a real answer for an integrated GPU with no dedicated
    /// memory at all.
    cc::optional<i64> dedicated_video_memory_bytes;

    /// A software rasterizer rather than real hardware — WARP, lavapipe.
    /// Its "driver" is the runtime, so a blob it produced is worth even less across machines than a real driver's.
    bool is_software = false;
};
