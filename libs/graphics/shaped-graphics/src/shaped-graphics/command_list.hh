#pragma once

#include <clean-core/container/span.hh>
#include <shaped-graphics/bytes_future.hh>
#include <shaped-graphics/command_list.compute.hh>
#include <shaped-graphics/command_list.copy.hh>
#include <shaped-graphics/command_list.download.hh>
#include <shaped-graphics/command_list.upload.hh>
#include <shaped-graphics/command_list_slot.hh>
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

    /// The access-tracking slot this list holds for its lifetime — keys its private access-state entry in
    /// every resource it touches, so concurrent lists don't share state. See
    /// libs/graphics/shaped-graphics/docs/concepts/barriers.md.
    [[nodiscard]] command_list_slot slot() const { return _slot; }

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
    command_list(epoch created_in, command_list_slot slot);

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
    virtual void compute_bind_group(int set, binding_group const& group) = 0;
    virtual void compute_dispatch(int x, int y, int z) = 0;

    // Records explicit per-element access for an array/bindless binding (reached through cmd.compute).
    // Split by resource family: buffers carry no layout, textures do.
    virtual void compute_declare_array_buffer_access(cc::string_view binding_name,
                                                     cc::span<array_buffer_access const> elements)
        = 0;
    virtual void compute_declare_array_texture_access(cc::string_view binding_name,
                                                      cc::span<array_texture_access const> elements)
        = 0;

    epoch _epoch = epoch::invalid;
    command_list_slot _slot = command_list_slot::invalid;
};
} // namespace sg
