#pragma once

#include <shaped-graphics/fwd.hh>

// A GPU capture taken in process, by the tool `SC_SG_CAPTURE` names, of one epoch of the first context that reaches it.
// docs/concepts/gpu-capture.md is the contract: the variables, which epoch, and what each tool writes.
//
// The tool's library has to be in the process before the first device exists, or it sees nothing.
// Every device creation takes `device_lifecycle_hold`, so its constructor attaches the tool, once per process.

namespace sg::impl
{
/// Loads the tool `SC_SG_CAPTURE` names, the first time any thread calls it; nothing at all without the variable.
void attach_gpu_capture();

/// After `ctx` advanced into `opened`: ends a capture of the epoch just closed, and begins one of `opened` where it is
/// the one asked for.
void gpu_capture_on_epoch_opened(context const& ctx, epoch opened);

/// Before `ctx` releases its device: ends a capture still open on it.
void gpu_capture_on_shutdown(context const& ctx);
} // namespace sg::impl
