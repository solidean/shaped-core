#pragma once

#include <versioned-document-file/fwd.hh>
#include <versioned-document-file/memory_image.hh>
#include <versioned-document-file/store.hh>

#include <memory>

/// The in-memory store: the seam's other arm, and the oracle the conformance suite compares against.
///
/// It has **no actor**. The connection an actor exists to own exclusively is the thing this arm does not have, so a
/// thread per unsaved document would buy nothing and would put a second scheduler under a test that exists to be
/// deterministic.
/// Its hooks complete inline, and on_pump reports nothing left — which is exactly what a threaded build's actor
/// reports too, so no caller can tell the difference from the outside.

namespace vdoc::file::impl
{
/// Builds an in-memory store over `image`, loading it exactly as a file is loaded.
///
/// **Null where the load failed hard** — a required snapshot that will not decode is the one way that happens on this
/// arm, since nothing here can be "not a database".
/// A null handle is what stands in for the file arm's failed open, which has no store to hand back either.
[[nodiscard]] store_handle make_memory_store(std::shared_ptr<memory_image> const& image);
} // namespace vdoc::file::impl
