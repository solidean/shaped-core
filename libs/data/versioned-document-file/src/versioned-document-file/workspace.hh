#pragma once

#include <clean-core/string/string.hh>
#include <versioned-document-file/fwd.hh>
#include <versioned-document/value.hh>

/// The workspace: disposable UI state, in its own table so it can be deleted wholesale without a second thought.
///
/// **Nothing here is load-bearing.** Discarding every row leaves a fully valid document.
/// **A workspace write creates no op and moves no ref**, so moving a camera does not become edit history and does not make a document look unsaved.
///
/// Keys are slash-namespaced and coarse: one per cohesive settings struct, never one per field.
/// A per-field key would multiply the rows a frame can dirty, and the cadence of writing them belongs to the caller rather than to this layer.

/// One workspace value plus the version that describes its shape.
///
/// The store cannot know an application's versions, so "a reader that does not know a version skips the entry" is
/// resolved at READ time: a caller names the version it can handle, and a row stored under a different one reads as absent.
/// It is left in the table, which is what keeps an older build from clobbering a newer one's state.
struct vdoc::file::workspace_value
{
    i32 version = 0;
    vdoc::value value;
};

/// One keyed workspace entry, as written to and read from the file.
struct vdoc::file::workspace_entry
{
    cc::string key;
    workspace_value value;
};
