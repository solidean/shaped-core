#pragma once

#include <babel-data/fwd.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>

// LLVM bitstream reader (shader/): the container format of LLVM bitcode, below any one LLVM version.
//
// A bitstream is nested blocks of records, each record a code and a list of integers, written with abbreviations
// that say how wide each integer is.
// This reader undoes the abbreviations and nothing else: every record comes back as its code and its operands as
// plain u64s, a blob as a view of the input, in the order the stream states them.
// What a block or a record means is a module reader's to say, per LLVM version; babel::llvm37 is the first.
//
// The stream's own BLOCKINFO block is applied and kept out of the tree: its abbreviations reach every block of the id
// it names.
//
//   auto const bc = babel::llvm::read_bitstream(bytes).value();
//   for (auto const& e : bc.blocks[0].entries)      // block 0 is the stream itself
//       if (e.is_block && bc.blocks[e.index].id == 8)  // MODULE_BLOCK
//           walk(bc, bc.blocks[e.index]);

/// One record: its code, which abbreviation wrote it (3 for none), and a range of `bitstream::operands`.
struct babel::llvm::record
{
    u32 code = 0;
    u32 abbreviation = 0;
    u32 first = 0;
    u32 count = 0;
    /// A blob's bytes as a range of `bitstream::bytes`, or a count of 0 where the record has none.
    u32 blob_first = 0;
    u32 blob_count = 0;
};

/// One record or block of a block, in stream order.
struct babel::llvm::block_entry
{
    bool is_block = false;
    /// An index into `bitstream::blocks` or `bitstream::records`.
    u32 index = 0;
};

struct babel::llvm::block
{
    u32 id = 0;
    /// The block it stands in; -1 for the stream itself.
    i32 parent = -1;
    cc::vector<block_entry> entries;
};

/// A bitstream, read once: its blocks, block 0 being the stream itself, and every record's operands.
struct babel::llvm::bitstream
{
    cc::vector<block> blocks;
    cc::vector<record> records;
    cc::vector<u64> operands;
    /// The bytes the stream was read from, which blobs are ranges of.
    cc::vector<byte> bytes;

    [[nodiscard]] cc::span<u64 const> operands_of(record const& r) const
    {
        return cc::span<u64 const>(operands).subspan({.offset = isize(r.first), .size = isize(r.count)});
    }
    [[nodiscard]] cc::span<byte const> blob_of(record const& r) const
    {
        return cc::span<byte const>(bytes).subspan({.offset = isize(r.blob_first), .size = isize(r.blob_count)});
    }
};

namespace babel::llvm
{
/// The stream after its magic `BC 0xC0DE`, and after the wrapper header that may stand before it.
/// An error where the magic is missing, a block runs past the end, or an abbreviation is malformed or unknown.
[[nodiscard]] cc::result<bitstream> read_bitstream(cc::span<byte const> bytes);
} // namespace babel::llvm
