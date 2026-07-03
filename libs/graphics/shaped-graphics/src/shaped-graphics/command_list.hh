#pragma once

#include <clean-core/container/span.hh>
#include <shaped-graphics/bytes_future.hh>
#include <shaped-graphics/command_list.compute.hh>
#include <shaped-graphics/command_list.copy.hh>
#include <shaped-graphics/command_list.download.hh>
#include <shaped-graphics/command_list.upload.hh>
#include <shaped-graphics/fwd.hh>

namespace sg
{
/// Records GPU work, submitted through the context that created it. Single-use and single-threaded:
/// recorded by one thread, then submitted or dropped once — in the epoch it was opened in (command
/// lists must not span epochs; see libs/graphics/shaped-graphics/docs/concepts/epochs.md).
class command_list
{
public:
    virtual ~command_list();

    /// The epoch this list was opened in. It must be submitted or dropped before that epoch advances.
    [[nodiscard]] epoch created_in_epoch() const { return _epoch; }

    // buffer transfer — host↔device copies recorded at this point in the list

    /// Host→device upload facade: `cmd.upload.bytes_to_buffer(...)` / `cmd.upload.data_to_buffer(...)`.
    command_list_upload_scope upload;

    /// Device→host download facade: `cmd.download.bytes_from_buffer(...)` / `cmd.download.data_from_buffer<T>(...)`.
    command_list_download_scope download;

    /// Device→device copy facade: `cmd.copy.buffer_bytes_region(...)` / `cmd.copy.buffer_data_region<T>(...)`.
    command_list_copy_scope copy;

    /// Compute facade: `cmd.compute.bind_pipeline(...)` / `.bind_group(...)` / `.dispatch(...)`.
    command_list_compute_scope compute;

protected:
    explicit command_list(epoch created_in);

    // Backend seams the upload/download/copy/compute scopes forward to (contracts documented there);
    // friends so the scopes can reach them.
    friend class command_list_upload_scope;
    friend class command_list_download_scope;
    friend class command_list_copy_scope;
    friend class command_list_compute_scope;

    virtual void upload_bytes_to_buffer(buffer_handle buffer, cc::span<cc::byte const> data, cc::isize offset_in_bytes)
        = 0;

    [[nodiscard]] virtual bytes_future download_bytes_from_buffer(buffer_handle buffer,
                                                                  cc::isize offset_in_bytes,
                                                                  cc::isize size_in_bytes)
        = 0;

    virtual void copy_buffer_region(buffer_handle src,
                                    buffer_handle dst,
                                    cc::isize src_offset_in_bytes,
                                    cc::isize dst_offset_in_bytes,
                                    cc::isize size_in_bytes)
        = 0;

    virtual void compute_bind_pipeline(compute_pipeline const& pipeline) = 0;
    virtual void compute_bind_group(u32 set, binding_group const& group) = 0;
    virtual void compute_dispatch(u32 x, u32 y, u32 z) = 0;

    epoch _epoch = epoch::invalid;
};
} // namespace sg
