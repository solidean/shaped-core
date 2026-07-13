#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/error/optional.hh>
#include <shaped-graphics/bytes_future.hh>
#include <shaped-graphics/command_list.compute.hh>
#include <shaped-graphics/command_list.copy.hh>
#include <shaped-graphics/command_list.download.hh>
#include <shaped-graphics/command_list.query.hh>
#include <shaped-graphics/command_list.raster.hh>
#include <shaped-graphics/command_list.raytracing.hh>
#include <shaped-graphics/command_list.upload.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/gpu_timestamp.hh>

namespace sg
{
/// Records GPU work, submitted through the context that created it. Single-use and single-threaded:
/// recorded by one thread, then submitted or dropped once — in the epoch it was opened in (command
/// lists must not span epochs; see libs/graphics/shaped-graphics/docs/concepts/epochs.md).
///
/// Submit or drop every list explicitly through the context (`ctx.submit_command_list` /
/// `ctx.drop_command_list`); after either it is consumed. Letting a list go out of scope un-consumed
/// auto-drops it but prints a warning — a safety net, not the intended path.
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

    /// Raster facade: `cmd.raster.render_to(...)` opens a rendering scope over a set of targets;
    /// `cmd.raster.manual.begin_rendering(...)` / `.end_rendering()` do it by hand.
    command_list_raster_scope raster;

    /// Ray-tracing facade: `cmd.raytracing.build_blas(...)` / `.build_tlas(...)` / `.is_supported()`.
    command_list_raytracing_scope raytracing;

    /// GPU-query facade: `cmd.query.record_gpu_timestamp()` / `.is_supported()`.
    command_list_query_scope query;

protected:
    explicit command_list(epoch created_in);

    // Backend seams the upload/download/copy/compute scopes forward to (contracts documented there);
    // friends so the scopes can reach them.
    friend class command_list_upload_scope;
    friend class command_list_download_scope;
    friend class command_list_copy_scope;
    friend class command_list_compute_scope;
    friend class command_list_raytracing_scope;
    friend class command_list_query_scope;
    friend class command_list_raster_scope;
    friend class command_list_raster_manual_scope;
    friend class rendering_scope;

    virtual void upload_bytes_to_buffer(raw_buffer_handle buffer, cc::span<cc::byte const> data, cc::isize offset_in_bytes)
        = 0;

    virtual void upload_bytes_to_texture(raw_texture_handle texture,
                                         cc::span<cc::byte const> pixels,
                                         subresource_index const& subresource,
                                         texture_region const& region)
        = 0;

    [[nodiscard]] virtual bytes_future download_bytes_from_buffer(raw_buffer_handle buffer,
                                                                  cc::isize offset_in_bytes,
                                                                  cc::isize size_in_bytes)
        = 0;

    [[nodiscard]] virtual bytes_future download_bytes_from_texture(raw_texture_handle texture,
                                                                   subresource_index const& subresource,
                                                                   texture_region const& region)
        = 0;

    virtual void copy_buffer_region(raw_buffer_handle src,
                                    raw_buffer_handle dst,
                                    cc::isize src_offset_in_bytes,
                                    cc::isize dst_offset_in_bytes,
                                    cc::isize size_in_bytes)
        = 0;

    virtual void compute_bind_pipeline(compute_pipeline const& pipeline) = 0;
    virtual void compute_bind_group(int set, binding_group const& group) = 0;
    virtual void compute_dispatch(int x, int y, int z) = 0;
    virtual void compute_set_inline_constants(cc::span<cc::byte const> data, cc::optional<cc::isize> offset) = 0;

    // Records explicit per-element access for an array/bindless binding (reached through cmd.compute).
    // Split by resource family: buffers carry no layout, textures do.
    virtual void compute_declare_array_buffer_access(cc::string_view binding_name,
                                                     cc::span<array_buffer_access const> elements)
        = 0;
    virtual void compute_declare_array_texture_access(cc::string_view binding_name,
                                                      cc::span<array_texture_access const> elements)
        = 0;

    // Raster rendering scope (reached through cmd.raster). begin_rendering transitions each target to
    // its render-target / depth-stencil layout, binds the color/depth targets to the output-merger, and
    // applies each target's clear / discard; end_rendering closes the scope and releases its RTV/DSV descriptors.
    // Calls must be balanced. Draw recording lands with the graphics pipeline.
    virtual void raster_begin_rendering(rendering_info const& info) = 0;
    virtual void raster_end_rendering() = 0;

    // Ray-tracing acceleration-structure builds (reached through cmd.raytracing). Split by geometry family
    // (a BLAS is triangles or AABBs, never both) since span-element overloads can't dispatch through one
    // vtable slot. Each sizes + allocates the persistent result buffer, records the build with transient
    // scratch, and returns the handle. raytracing_is_supported gates them (a backend without RT returns false).
    [[nodiscard]] virtual bool raytracing_is_supported() const = 0;
    [[nodiscard]] virtual blas_handle raytracing_build_blas_triangles(cc::span<blas_triangles const> geometries,
                                                                      accel_build_flags flags)
        = 0;
    [[nodiscard]] virtual blas_handle raytracing_build_blas_aabbs(cc::span<blas_aabbs const> geometries,
                                                                  accel_build_flags flags)
        = 0;
    [[nodiscard]] virtual tlas_handle raytracing_build_tlas(cc::span<tlas_instance const> instances,
                                                            accel_build_flags flags)
        = 0;

    // Ray-tracing dispatch (reached through cmd.raytracing). bind_pipeline sets the DXR state object + global
    // root signature; bind_group binds through that root signature (like compute); dispatch_rays traces a
    // width x height x depth grid, launching the raygen at `raygen` in `table`.
    virtual void raytracing_bind_pipeline(raytracing_pipeline const& pipeline) = 0;
    virtual void raytracing_bind_group(int set, binding_group const& group) = 0;
    virtual void raytracing_dispatch_rays(raytracing_shader_table const& table,
                                          raygen_index raygen,
                                          int width,
                                          int height,
                                          int depth)
        = 0;

    // GPU queries (reached through cmd.query). record_gpu_timestamp records a point-in-time timestamp and
    // returns a handle whose tick is resolved + read back when the list is submitted. A backend without
    // timestamp support answers false and returns an invalid query (record is always callable).
    [[nodiscard]] virtual bool query_timestamps_supported() const = 0;
    [[nodiscard]] virtual gpu_timestamp query_record_gpu_timestamp() = 0;

    epoch _epoch = epoch::invalid;
};
} // namespace sg
