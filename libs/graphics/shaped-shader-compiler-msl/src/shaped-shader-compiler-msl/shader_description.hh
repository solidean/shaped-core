#pragma once

#include <clean-core/error/optional.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics/binding/compiled_shader.hh>
#include <shaped-graphics/binding/shader_stage.hh>
#include <shaped-shader-compiler-msl/fwd.hh>

/// One shader to compile: the MSL text, which entry point in it, and what that entry point is.

/// What to compile, and as what.
///
/// `source` is MSL, complete — this wrapper resolves no `#include`, so a file that has one passes `-I` through
/// `compile_options::extra_args` and lets the Metal compiler resolve it.
/// `entry_point` must name a function the text declares with the qualifier `stage` implies, or the compile fails here
/// rather than at pipeline creation.
struct ssc::msl::shader_description
{
    cc::string source;
    cc::string entry_point = "main";
    sg::shader_stage stage = sg::shader_stage::compute;

    /// The threadgroup shape a compute entry point is dispatched with, when the shader depends on one.
    ///
    /// MSL states no shape of its own — a Metal kernel's threadgroup size is a dispatch parameter, and
    /// `[[max_total_threads_per_threadgroup]]` is a scalar cap rather than three numbers.
    /// So a kernel that reads `threadgroup` memory, a simdgroup reduction or `thread_position_in_threadgroup` must say
    /// what it assumed, here or as `#pragma sc numthreads x y z` above the entry point.
    /// Absent means the kernel does not care, and the shape is derived from the pipeline state at dispatch.
    cc::optional<sg::compute_dimensions> workgroup_size;
};
