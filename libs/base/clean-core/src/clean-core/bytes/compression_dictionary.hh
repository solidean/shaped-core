#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/fwd.hh>

/// A compression dictionary: shared context that makes small payloads compress far better than they can alone.
///
/// A few hundred bytes of JSON, a vdoc op, a shader stub — each is too short for the codec to learn anything from before it ends.
/// A dictionary is that learning, done once over a corpus and then handed to every call.
///
/// This type holds ONLY the dictionary bytes and which algorithm they are for.
/// A const dictionary may be shared by compressors on several threads, unlike the compressors themselves.
/// The prepared backend form is cc::compressor's, because zstd's is built for one compression level and would be silently wrong at another.
///
/// The bytes are algorithm-specific even though the type is shared.
/// They are whatever that algorithm's dictionary format says they are, which is why algorithm() travels with them and why using one with another algorithm fails rather than producing garbage.
///
/// Compression and decompression must be given the SAME dictionary.
/// Bytes compressed with one cannot be read without it, so a format storing them must record which dictionary applies.
/// That is id() where the algorithm has one, and otherwise the format's own reference to the content.
struct cc::compression_dictionary
{
    /// Adopt raw dictionary content.
    ///
    /// For zstd this is normally the output of train(), but any bytes are valid: a raw-content dictionary is just text the compressor may reference, and a representative sample file works.
    /// For lz4 it is always raw content — the bytes are used as a prefix, and there is no other form.
    [[nodiscard]] static compression_dictionary from_bytes(compression_algorithm algorithm, cc::span<byte const> raw);

    /// Build a dictionary from representative samples.
    ///
    /// `dict_size` is a budget rather than a target, and 100 kB is upstream's usual starting point.
    /// Aim for at least a hundred samples that look like what will actually be compressed.
    /// Too few, or samples that do not resemble the real payloads, produce a dictionary that helps nothing.
    ///
    /// zstd only.
    /// LZ4 ships no trainer, so it fails there rather than inventing one — build its dictionary from representative bytes with from_bytes instead.
    [[nodiscard]] static cc::result<compression_dictionary> train(compression_algorithm algorithm,
                                                                  cc::span<cc::span<byte const> const> samples,
                                                                  isize dict_size);

    [[nodiscard]] compression_algorithm algorithm() const { return _algorithm; }

    /// The dictionary's self-declared id, which is what a format writes down to say which dictionary decodes a blob.
    /// 0 when the content carries none — every lz4 dictionary, and a zstd raw-content one.
    [[nodiscard]] u32 id() const { return _id; }

    /// The content itself, for a format that stores the dictionary rather than referencing it by id.
    [[nodiscard]] cc::span<byte const> bytes() const { return _bytes; }

    [[nodiscard]] bool is_empty() const { return _bytes.empty(); }

private:
    cc::vector<byte> _bytes;
    compression_algorithm _algorithm = {};
    u32 _id = 0;
};
