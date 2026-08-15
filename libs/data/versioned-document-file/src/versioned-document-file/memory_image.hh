#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <versioned-document-file/fwd.hh>
#include <versioned-document-file/rows.hh>

/// The whole of an in-memory store's state, in the same rows a file holds.
///
/// **It outlives the store that wrote it**, which is what lets a caller close one and reopen it exactly like a file —
/// re-running the load, its verification and its issues.
/// That is also what makes the in-memory store an oracle rather than a shortcut: the conformance suite's reopen,
/// torn-state and idempotence tests run on it unchanged.
///
/// Every payload is held as raw bytes for the same reason: a test that flips one byte of a stored op flips it here
/// exactly as it would in a file.

/// The in-memory backing of a store, mirroring [the format](../../docs/format.md#schema) table for table.
struct vdoc::file::memory_image
{
    /// 'VDOC' — the same stamp a file carries, so the identity check is the same check on both arms.
    static constexpr i32 vdoc_application_id = 0x56444F43;

    /// The format version this build writes and reads.
    static constexpr i32 current_user_version = 1;

    /// A table this build does not know, kept whole so a rewrite cannot drop it.
    /// It has no rows here because nothing this build can do would fill them — what matters is that it survives.
    struct unknown_table_entry
    {
        cc::string name;
        cc::vector<cc::string> columns;
    };

    /// 0 on an image nobody has stamped, exactly as a fresh file reads.
    i32 application_id = 0;
    i32 user_version = 0;

    cc::vector<op_row> ops;
    cc::vector<ref_row> refs;
    cc::vector<snapshot_row> snapshots;
    cc::vector<asset_row> assets;
    cc::vector<blob_row> blobs;
    cc::vector<chunk_row> blob_chunks;
    cc::vector<workspace_row> workspace;
    cc::vector<meta_row> meta;

    cc::vector<unknown_table_entry> unknown_tables;

    /// Extra columns on a table this build does know, as "<table>.<column>".
    /// Nothing writes them, which is exactly how a file preserves them: every statement names its own columns.
    cc::vector<cc::string> unknown_columns;

    /// The next rowid `blobs` hands out, since blobs is a rowid table.
    i64 next_blob_id = 1;

    /// Makes every write against this image fail.
    ///
    /// A FAULT-INJECTION knob, and the in-memory arm's answer to a full disk or a revoked permission.
    /// It lives on the image rather than on the store because refusing writes is a property of storage, and the
    /// conformance suite needs both arms to be able to fail the same way.
    bool writes_fail = false;
};
