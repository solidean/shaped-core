#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/string_view.hh>
#include <versioned-document-file/fwd.hh>

/// The blob encoding seam: how stored bytes are made, and unmade.
///
/// `raw` is the only encoding v1 writes or reads, and the seam exists anyway — as real dispatch rather than a
/// placeholder, which is what makes adding compression later a codec entry and no format migration.
/// The argument is [decisions.md](../../../../versioned-document/docs/decisions.md#blobs-ship-raw-only-with-the-encoding-seam-reserved).
///
/// Internal rather than public, because v1 registers nothing from outside: a future compressor is a vendored dependency
/// plus one table entry here, not a registration API a caller drives.

namespace vdoc::file::impl
{
/// One blob encoding.
///
/// Both directions take and return the vector BY VALUE, which is what lets `raw` be a move rather than a copy: the
/// identity codec hands its argument straight back, so the seam costs nothing while it has only one entry.
struct blob_codec
{
    cc::string_view name;

    /// Decoded bytes -> the bytes to store.
    cc::result<cc::vector<byte>> (*encode)(cc::vector<byte> decoded) = nullptr;

    /// Stored bytes -> the decoded bytes.
    /// `decoded_size` is what the blob row promises, and a decode producing any other length is an error.
    cc::result<cc::vector<byte>> (*decode)(cc::vector<byte> stored, i64 decoded_size) = nullptr;

    /// True where a stored byte offset IS a decoded byte offset.
    /// Only such an encoding can serve a byte range without materializing the whole blob, which is what makes a
    /// 64-byte header readable out of a multi-gigabyte part.
    bool is_byte_addressable = false;
};

/// The encodings this build can read and write, `raw` first.
[[nodiscard]] cc::span<blob_codec const> blob_codecs();

/// The codec for `encoding`, or null where this build has none.
///
/// Null means two different things by where it is asked, and both are correct: a FILE naming an unknown encoding is a
/// load issue and the blob is skipped, while a CALLER asking for one is a publish error.
[[nodiscard]] blob_codec const* find_blob_codec(cc::string_view encoding);
} // namespace vdoc::file::impl
