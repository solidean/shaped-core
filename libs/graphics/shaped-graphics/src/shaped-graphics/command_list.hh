#pragma once

#include <clean-core/common/utility.hh>
#include <clean-core/container/span.hh>
#include <shaped-graphics/bytes_future.hh>
#include <shaped-graphics/fwd.hh>

#include <ranges>
#include <type_traits>

namespace sg
{
/// Records GPU work, submitted through the context that created it. Single-use and single-threaded:
/// recorded by one thread, then submitted or dropped once — in the epoch it was opened in (command
/// lists must not span epochs; see libs/graphics/shaped-graphics/docs/concepts/epochs.md).
///
/// Abstract: a backend subclasses it.
class command_list
{
public:
    virtual ~command_list();

    /// The epoch this list was opened in. It must be submitted or dropped before that epoch advances.
    [[nodiscard]] epoch created_in_epoch() const { return _epoch; }

    // buffer transfer — host↔device copies recorded at this point in the list

    /// Uploads `data` into `buffer` starting at `offset_in_bytes`. The buffer must have been created
    /// with buffer_usage::copy_dst. The source bytes are copied immediately, so it is safe to mutate
    /// or free them once this returns. The write is visible to later commands in the same list. An
    /// empty span is a no-op. Precondition: offset_in_bytes + data.size() <= buffer size.
    /// TODO: version with pinned_data that tries to copy it in parallel and blocks on submit?
    virtual void upload_to_buffer(buffer_handle buffer, cc::span<cc::byte const> data, cc::isize offset_in_bytes = 0) = 0;

    /// Reads `size_in_bytes` from `buffer` starting at `offset_in_bytes` back to the host. The buffer
    /// must have been created with buffer_usage::copy_src. Returns a bytes_future that becomes ready
    /// once the submitted list has finished on the GPU and the bytes have been copied to the host. A
    /// zero-size read yields an already-ready, empty future. Precondition: offset_in_bytes +
    /// size_in_bytes <= buffer size.
    [[nodiscard]] virtual bytes_future download_from_buffer(buffer_handle buffer,
                                                            cc::isize offset_in_bytes,
                                                            cc::isize size_in_bytes)
        = 0;

    /// Uploads a trivially-copyable contiguous range as raw bytes. See upload_to_buffer.
    template <std::ranges::contiguous_range RangeT>
    void upload_data_to_buffer(buffer_handle buffer, RangeT const& data, cc::isize offset_in_bytes = 0)
    {
        using element_t = std::remove_cvref_t<std::ranges::range_value_t<RangeT>>;
        static_assert(std::is_trivially_copyable_v<element_t>, "upload element type must be trivially copyable");
        auto const bytes = cc::span<cc::byte const>(reinterpret_cast<cc::byte const*>(std::ranges::data(data)),
                                                    cc::isize(std::ranges::size(data)) * cc::isize(sizeof(element_t)));
        upload_to_buffer(cc::move(buffer), bytes, offset_in_bytes);
    }

    /// Downloads `count` elements of a trivially-copyable type. See download_from_buffer.
    template <class T>
    [[nodiscard]] data_future<T> download_data_from_buffer(buffer_handle buffer, cc::isize offset_in_bytes, cc::isize count)
    {
        static_assert(std::is_trivially_copyable_v<T>, "download element type must be trivially copyable");
        return data_future<T>(download_from_buffer(cc::move(buffer), offset_in_bytes, count * cc::isize(sizeof(T))));
    }

protected:
    explicit command_list(epoch created_in);

    epoch _epoch = epoch::invalid;
};
} // namespace sg
