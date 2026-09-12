#pragma once

#include <clean-core/string/string.hh>
#include <shaped-graphics/fwd.hh>

/// An error that arrived after the call that caused it.
///
/// Every sg call that can fail *synchronously* says so synchronously — it throws, or it hands back a result.
/// This channel is for the ones that cannot: a device removed while work was in flight, a validation layer speaking
/// up a frame later, and — the reason the channel exists at all — a backend whose resource creation is asynchronous.
/// WebGPU reports a bad buffer or a failed pipeline through a promise that settles long after the call returned, so
/// there is no synchronous answer to give; sg's answer is to hand the object back and report the failure here.
///
/// **Drain it once a frame.** Errors accumulate until taken, so a caller that never asks grows a list rather than
/// losing anything — and one that asks sees the whole frame's worth in one place, in the order the backend saw them.

/// What kind of thing went wrong, at the granularity a caller would act on.
/// Deliberately coarse: a caller that branches on a fine-grained cause is a caller that behaves differently per
/// backend, which is the thing sg exists to avoid.
enum class sg::device_error_kind
{
    /// The device is gone — driver reset, TDR, removed adapter.
    /// Sticky, and also visible as ctx.is_device_lost(); tearing the context down is the only recovery.
    device_lost,

    /// A resource, pipeline or layout could not be created, reported after the call that asked for it.
    /// The object a caller holds is unusable; nothing else is affected.
    creation_failed,

    /// The backend's validation layer objected to something already recorded or submitted.
    /// Always a programming error, and always worth reading — it is the one kind that names a bug rather than a limit.
    validation,
};

/// One entry on the deferred error channel.
struct sg::device_error
{
    device_error_kind kind = device_error_kind::validation;

    /// What the backend said, verbatim where it says anything.
    /// Backend wording, so it is for a human and never for a branch.
    cc::string message;
};
