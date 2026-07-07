#pragma once

#include <clean-core/common/utility.hh>
#include <shaped-graphics/fwd.hh>

#include <type_traits>

namespace sg
{
/// Arguments for a byte-granularity buffer→buffer region copy (cmd.copy.buffer_bytes_region). The
/// required fields (src, dst, size_in_bytes) have no default, so omitting one warns; the offsets are
/// optional and default to 0.
struct buffer_bytes_copy
{
    raw_buffer_handle src;             ///< copy source; must not be null; needs buffer_usage::copy_src
    raw_buffer_handle dst;             ///< copy destination; must not be null; needs buffer_usage::copy_dst
    cc::isize size_in_bytes;           ///< bytes to copy; 0 is a valid no-op
    cc::isize src_offset_in_bytes = 0; ///< byte offset into src
    cc::isize dst_offset_in_bytes = 0; ///< byte offset into dst
};

/// Typed variant in units of T (cmd.copy.buffer_data_region<T>) — count and offsets are in elements
/// of T, like taking a subspan on both sides. For sub-element (byte) granularity, use the
/// buffer_bytes_region escape hatch.
template <class T>
struct buffer_data_copy
{
    raw_buffer_handle src;
    raw_buffer_handle dst;
    cc::isize count;          ///< number of T elements to copy
    cc::isize src_offset = 0; ///< element offset into src
    cc::isize dst_offset = 0; ///< element offset into dst
};

/// Device→device copy facade for a command list, reached as `cmd.copy`.
///
/// A thin facade over its owning command list: it forwards each op to the list's backend impl.
/// Texture copy ops land here later, following the same `<resource>_<bytes|data>_region` scheme.
class command_list_copy_scope
{
public:
    /// Copies `size_in_bytes` from `src` (must have buffer_usage::copy_src) to `dst` (must have
    /// buffer_usage::copy_dst), each starting at its byte offset. The copy runs on the GPU and
    /// executes in-order with other commands in this list. A zero-size copy is a no-op. Copying
    /// within a single buffer is allowed only if the source and destination ranges do not overlap.
    /// Precondition: each offset + size_in_bytes <= that buffer's size.
    void buffer_bytes_region(buffer_bytes_copy args);

    /// Copies `count` elements of a trivially-copyable type; count and offsets are in elements of T.
    /// See buffer_bytes_region.
    template <class T>
    void buffer_data_region(buffer_data_copy<T> args)
    {
        static_assert(std::is_trivially_copyable_v<T>, "copy element type must be trivially copyable");
        auto const stride = cc::isize(sizeof(T));
        buffer_bytes_region({.src = cc::move(args.src),
                             .dst = cc::move(args.dst),
                             .size_in_bytes = args.count * stride,
                             .src_offset_in_bytes = args.src_offset * stride,
                             .dst_offset_in_bytes = args.dst_offset * stride});
    }

    // Pinned to its owning command list: neither copyable nor movable.
    command_list_copy_scope(command_list_copy_scope const&) = delete;
    command_list_copy_scope(command_list_copy_scope&&) = delete;
    command_list_copy_scope& operator=(command_list_copy_scope const&) = delete;
    command_list_copy_scope& operator=(command_list_copy_scope&&) = delete;

private:
    // Only a command list constructs its own scope; the scope in turn reaches the list's protected
    // backend virtuals (mutual friendship).
    friend class command_list;
    explicit command_list_copy_scope(command_list& cmd) : _cmd(cmd) {}

    command_list& _cmd;
};
} // namespace sg
