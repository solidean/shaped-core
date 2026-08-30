#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/record/fwd.hh>

/// What a recording says about the machine it was made on.
///
/// A stamp is a block of bytes written into a recording when it opens and again when it closes, describing the machine
/// and how loaded it was.
/// Nobody reads one until something has already gone wrong, and then it is the first thing anyone asks for.
///
/// cc contributes a `cc.system` section — the machine description — and a `cc.resources` section, the levels at that
/// instant.
/// A higher library adds its own with `register_stamp_contributor`: `sg` stamps which GPU is in the machine without cc
/// ever knowing GPUs exist.
///
/// **A contributor is called ONLY at recording open and close, and never to answer a query.**
/// That restriction is the whole difference between this and a provider seam.
/// A stamp contributor writes into a recording at a known moment, with one call site and one ordering; a provider
/// answers an arbitrary call at an arbitrary time, and its results then depend on link order.
/// The moment something calls a contributor outside these two points, this has become the second thing, and the reason
/// GPU load lives in sg rather than behind a seam in cc has evaporated.

/// Which end of a recording a stamp is being taken at.
enum class cc::rec::stamp_moment : cc::u8
{
    open,
    close,
};

namespace cc::rec
{
/// Produces one contributor's section.
///
/// Returning an empty span leaves the section out; a contributor that cannot answer never fails the recording.
/// Called on the thread that is opening or closing the recording, under no lock of the recorder's.
using stamp_provider = cc::span<byte const> (*)(rec::stamp_moment moment);

/// Adds a contributor, named by a string that must outlive the process.
///
/// Registering twice under one name replaces the earlier provider rather than emitting two sections.
/// Returns false when the table is full, which is the only way this fails.
bool register_stamp_contributor(char const* name, rec::stamp_provider provider);

/// Emits every section as one `event_kind::stamp` event each.
///
/// **Explicit, and not emitted for you when a recording opens.**
/// A stamp is an ordinary event, so it reaches every listener registered at that moment rather than only the one being
/// opened.
/// Stamping automatically on registration therefore drops the second recording's stamp into the first, and two
/// recordings running side by side stop seeing the same events — which is a property the splicing listener and its
/// tests rely on.
/// Addressing one listener would need a delivery path the recorder does not have.
///
/// So the caller stamps, around the part it cares about:
///
///     auto listener = cc::rec::recording_listener();
///     auto const h = cc::rec::register_listener(listener);
///     cc::rec::emit_stamp(cc::rec::stamp_moment::open);
///     ... the work ...
///     cc::rec::emit_stamp(cc::rec::stamp_moment::close);
///     cc::rec::flush_blocking();
///     cc::rec::unregister_listener(h);
///
/// The close stamp has to be emitted and flushed BEFORE unregistering, or it never reaches the recording.
void emit_stamp(rec::stamp_moment moment);
} // namespace cc::rec
