#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/function/unique_function.hh>
#include <shaped-graphics/fwd.hh>

namespace sg
{
/// Where a streaming download's bytes go, when a resident destination is not what the caller wants.
///
/// The default is one pinned buffer the whole readback lands in, reached through the handle's `bytes_future`.
/// A download that must not hold its result resident — writing it to a file, feeding it to an encoder — wants this
/// instead: each chunk is handed over as it arrives and nothing accumulates.
///
/// `offset` is in bytes from the start of the extent named at the call.
/// For a texture the bytes are the region's tightly-packed rows, delivered a run of whole rows at a time.
///
/// **Called on the copy actor thread, and it must not block**, for the same reason a source's poll must not: that
/// thread stages every other transfer in the system.
/// Copying the bytes somewhere and returning is fine; decoding them there is not.
///
/// **The span must not be retained.** It points into the readback staging window, which is recycled a few windows
/// later — so the bytes are valid for the duration of the call and no longer.
///
/// Return false to fail the transfer: the handle settles on its error channel and no further chunk is delivered.
///
/// Chunks of one transfer arrive **in order**, which is what lets a sink append rather than seek.
/// That is free rather than engineered: a readback's source is fully resident on the GPU, so its chunks have no
/// readiness constraint and are simply taken in cursor order.
/// Nothing is guaranteed *between* transfers.
using stream_sink = cc::unique_function<bool(cc::span<byte const> bytes, isize offset)>;
} // namespace sg
