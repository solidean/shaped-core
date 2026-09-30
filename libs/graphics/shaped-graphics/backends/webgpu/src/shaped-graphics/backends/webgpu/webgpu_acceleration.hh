#pragma once

#include <clean-core/container/vector.hh>
#include <shaped-graphics/backends/webgpu/fwd.hh>
#include <shaped-graphics/backends/webgpu/webgpu_common.hh>
#include <shaped-graphics/raytracing/acceleration_structure.hh>

// The ray-query polyfill's storage and builds.
// The layout every region follows is an internal contract with SGL's traversal:
// libs/graphics/shaped-graphics-language/docs/raytracing-polyfill.md.

namespace sg::backend::webgpu
{
/// Bytes per pool unit, the `vec4u` every offset in the pool counts.
inline constexpr isize acceleration_unit_bytes = 16;

/// Where group 3 carries the pool and the roots, past the inline constants and the 16 register-bound samplers.
inline constexpr u32 acceleration_pool_binding = 17;
inline constexpr u32 acceleration_roots_binding = 18;

/// The roots uniform, `array<vec4u, 4>`: one u32 per acceleration member, so at most 16 of them.
inline constexpr isize acceleration_roots_bytes = 64;
inline constexpr isize max_acceleration_members = acceleration_roots_bytes / 4;
} // namespace sg::backend::webgpu

/// The internal compute kernels a build records, all in one WGSL module.
enum class sg::backend::webgpu::acceleration_kernel
{
    write_triangles,
    write_boxes,
    write_leaves,
    write_levels,
};

/// A pool region one command list wrote, and the pool buffer it wrote it into.
struct sg::backend::webgpu::acceleration_pool_write
{
    u64 generation = 0;
    wgpu_buffer buffer;
    u32 unit = 0;
    u32 units = 0;
};

/// The one storage buffer per context that holds every BLAS and TLAS, suballocated first-fit in 16-byte units.
///
/// **Unit 0 is the empty TLAS and never handed out**; WebGPU zero-fills a new buffer, which is exactly its header.
///
/// Growing allocates a larger buffer and copies the old one into it, in a command buffer submitted at once, so every offset survives.
/// The generation counts the buffers, and whatever binds the pool keys on it.
/// A list that wrote a region into an older buffer and is still open copies it forward itself: see webgpu_command_list::bring_pool_writes_forward.
///
/// A released region is reused only once the epoch it was released in retires, since the GPU may still trace it until then.
///
/// Device thread only.
class sg::backend::webgpu::webgpu_acceleration_pool
{
public:
    void initialize(webgpu_context& ctx);

    /// Drops the buffer and the kernels; a region released afterwards is forgotten.
    void shutdown();

    /// The pool buffer, created on first use.
    [[nodiscard]] wgpu_buffer const& buffer();
    [[nodiscard]] u64 generation() const { return _generation; }
    [[nodiscard]] isize capacity_units() const { return _capacity_units; }

    /// The first unit of `units` contiguous free ones, growing the pool when no free range fits.
    /// Throws sg::allocation_exception past the largest storage binding the device allows.
    [[nodiscard]] u32 allocate(isize units);

    /// Frees a region once the epoch open now retires, running the structure's finalizers after it.
    void release_when_retired(u32 unit, u32 units, cc::vector<cc::unique_function<void()>> finalizers);

    /// The build kernels, and the layout their one bind group follows, made on first use.
    [[nodiscard]] WGPUComputePipeline kernel(acceleration_kernel k);
    [[nodiscard]] WGPUBindGroupLayout kernel_layout();

    /// A tiny storage buffer bound where a kernel reads no input, since a writable pool excludes binding it there too.
    [[nodiscard]] WGPUBuffer unused_input();

private:
    void grow(isize min_units);
    void free_now(u32 unit, u32 units);
    void create_kernels();

    struct free_range
    {
        u32 unit = 0;
        u32 units = 0;
    };

    webgpu_context* _ctx = nullptr;
    bool _shut_down = false;
    wgpu_buffer _buffer;
    u64 _generation = 0;
    isize _capacity_units = 0;
    cc::vector<free_range> _free; // sorted by unit, never adjacent

    wgpu_bind_group_layout _kernel_layout;
    wgpu_pipeline_layout _kernel_pipeline_layout;
    wgpu_shader_module _kernel_module;
    cc::vector<wgpu_compute_pipeline> _kernels;
    wgpu_buffer _unused_input;
};

/// WebGPU bottom-level acceleration structure: a region of the context's acceleration pool.
class sg::backend::webgpu::webgpu_blas final : public sg::blas
{
public:
    /// What the region's primitives are, as its header's kind says.
    enum class primitive_kind : u32
    {
        triangles = 0,
        boxes = 1,
    };

    webgpu_blas(webgpu_context& ctx,
                u32 unit,
                u32 units,
                primitive_kind kind,
                sg::accel_build_flags build_flags,
                int geometry_count);
    ~webgpu_blas() override;

    [[nodiscard]] u32 unit() const { return _unit; }
    [[nodiscard]] u32 units() const { return _units; }
    [[nodiscard]] primitive_kind kind() const { return _kind; }

private:
    void on_expired() const override { release_region(); }
    void release_region() const;

    webgpu_context& _ctx;
    u32 _unit = 0;
    u32 _units = 0;
    primitive_kind _kind = primitive_kind::triangles;
    mutable bool _released = false;
};

/// WebGPU top-level acceleration structure: a region of the context's acceleration pool.
/// Its unit is the root a shader traces from; the base keeps every referenced BLAS, and so its region, alive.
class sg::backend::webgpu::webgpu_tlas final : public sg::tlas
{
public:
    webgpu_tlas(webgpu_context& ctx,
                u32 unit,
                u32 units,
                sg::accel_build_flags build_flags,
                int instance_count,
                cc::vector<sg::blas_handle> referenced_blases);
    ~webgpu_tlas() override;

    [[nodiscard]] u32 unit() const { return _unit; }
    [[nodiscard]] u32 units() const { return _units; }

private:
    void on_expired() const override { release_region(); }
    void release_region() const;

    webgpu_context& _ctx;
    u32 _unit = 0;
    u32 _units = 0;
    mutable bool _released = false;
};
