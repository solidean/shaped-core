#pragma once

#include <clean-core/common/utility.hh>
#include <clean-core/error/optional.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/texture_region.hh>

#include <type_traits>

/// Arguments for a byte-granularity buffer→buffer region copy (cmd.copy.buffer_bytes_region).
/// The required fields — src, dst, size_in_bytes — have no default, so omitting one warns.
/// The offsets are optional and default to 0.
struct sg::buffer_bytes_copy
{
    raw_buffer_handle src;         ///< copy source; must not be null; needs buffer_usage::copy_src
    raw_buffer_handle dst;         ///< copy destination; must not be null; needs buffer_usage::copy_dst
    isize size_in_bytes;           ///< bytes to copy; 0 is a valid no-op
    isize src_offset_in_bytes = 0; ///< byte offset into src
    isize dst_offset_in_bytes = 0; ///< byte offset into dst
};

/// Typed variant in units of T (cmd.copy.buffer_data_region<T>) — count and offsets are in elements of T, like taking a subspan on both sides.
/// For sub-element (byte) granularity, use the buffer_bytes_region escape hatch.
template <class T>
struct sg::buffer_data_copy
{
    raw_buffer_handle src;
    raw_buffer_handle dst;
    isize count;          ///< number of T elements to copy
    isize src_offset = 0; ///< element offset into src
    isize dst_offset = 0; ///< element offset into dst
};

namespace sg
{
/// What a buffer's row pitch must be a multiple of to be copied into a texture, on every backend.
/// D3D12 and WebGPU both ask for 256; Vulkan and Metal accept it.
inline constexpr isize buffer_texture_row_pitch_alignment = 256;

/// What a buffer offset must be a multiple of to be copied into a texture, on every backend; D3D12's placement alignment.
inline constexpr isize buffer_texture_offset_alignment = 512;

/// The smallest legal row pitch for `width` texels of `format`: one row of texels, or of blocks for a block-compressed
/// format, rounded up to `buffer_texture_row_pitch_alignment`.
[[nodiscard]] isize buffer_texture_row_pitch(pixel_format format, int width);
} // namespace sg

/// Arguments for a GPU copy from a buffer into one subresource of a texture (cmd.copy.buffer_to_texture).
///
/// The buffer holds the region row after row, `src_row_pitch_in_bytes` apart, and depth slice after depth slice.
/// A row of a block-compressed format is a row of blocks, so a 4x4-block format's region of height h is ceil(h / 4) rows.
/// This is the copy a compute shader's output takes into a texture it could not write directly, such as a BC encoder's.
/// `dst` must not have a depth or depth-stencil format: the backends disagree on a depth texel's buffer layout, and WebGPU refuses the copy.
/// Reading depth back, through `cmd.download` or `ctx.download`, is fine on every backend.
struct sg::buffer_texture_copy
{
    raw_buffer_handle src;                    ///< copy source; must not be null; needs buffer_usage::copy_src
    raw_texture_handle dst;                   ///< copy destination; must not be null; needs texture_usage::copy_dst
    isize src_row_pitch_in_bytes;             ///< a multiple of buffer_texture_row_pitch_alignment, at least one row
    isize src_offset_in_bytes = 0;            ///< a multiple of buffer_texture_offset_alignment
    subresource_index subresource = {};       ///< the one subresource written
    cc::optional<texture_region> region = {}; ///< the box written; none is the whole subresource
};

/// Device→device copy facade for a command list, reached as `cmd.copy`.
/// Buffer regions and buffer→texture today; the rest of the texture copies land here later.
/// See libs/graphics/shaped-graphics/docs/concepts/command-recording.md.
class sg::command_list_copy_scope
{
public:
    /// Copies `size_in_bytes` from `src` to `dst`, each starting at its byte offset.
    /// `src` needs buffer_usage::copy_src and `dst` buffer_usage::copy_dst.
    /// The copy runs on the GPU and executes in-order with the other commands in this list.
    /// A zero-size copy is a no-op, and copying within one buffer is allowed only if the source and destination ranges do not overlap.
    /// Precondition: each offset + size_in_bytes <= that buffer's size.
    void buffer_bytes_region(buffer_bytes_copy args);

    /// Copies `count` elements of a trivially-copyable type; count and offsets are in elements of T.
    /// See buffer_bytes_region.
    template <class T>
    void buffer_data_region(buffer_data_copy<T> args)
    {
        static_assert(std::is_trivially_copyable_v<T>, "copy element type must be trivially copyable");
        auto const stride = isize(sizeof(T));
        buffer_bytes_region({.src = cc::move(args.src),
                             .dst = cc::move(args.dst),
                             .size_in_bytes = args.count * stride,
                             .src_offset_in_bytes = args.src_offset * stride,
                             .dst_offset_in_bytes = args.dst_offset * stride});
    }

    /// Copies `args.src`, laid out row by row, into a region of one subresource of `args.dst`.
    /// The copy runs on the GPU in order with this list's other commands, and the write is visible to later ones.
    /// An empty region is a no-op.
    /// Preconditions: the pitch and offset keep their alignments, the region is in bounds and block-aligned, the buffer
    /// holds every row the region needs, and the subresource is not one an open rendering scope draws into.
    void buffer_to_texture(buffer_texture_copy args);

    // Pinned to its owning command list: neither copyable nor movable.
    command_list_copy_scope(command_list_copy_scope const&) = delete;
    command_list_copy_scope(command_list_copy_scope&&) = delete;
    command_list_copy_scope& operator=(command_list_copy_scope const&) = delete;
    command_list_copy_scope& operator=(command_list_copy_scope&&) = delete;

private:
    // Only a command list constructs its own scope, and the scope reaches the list's protected backend virtuals — mutual friendship.
    friend class command_list;
    explicit command_list_copy_scope(command_list& cmd) : _cmd(cmd) {}

    command_list& _cmd;
};
