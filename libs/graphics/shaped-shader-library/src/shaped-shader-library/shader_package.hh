#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics/compiled_shader.hh>
#include <shaped-shader-library/compiler/shader_compiler.hh>
#include <shaped-shader-library/filesystem/embedded_filesystem.hh>
#include <shaped-shader-library/fwd.hh>

namespace slib
{
/// One shader in a package: which file, which stage, which entry point — plus the generated global to
/// write the asset handle back into.
struct shader_definition
{
    cc::string_view path; ///< package-relative, e.g. "compute/invert.hlsl"
    sg::shader_stage stage;
    cc::string_view entry_point;

    /// The generated global that call sites read. shader_library::add_package fills it in; nothing else
    /// writes it.
    shader_asset_handle* asset = nullptr;
};

/// A target's shaders, as emitted by sc_add_shader_package. A pure description with static storage —
/// generated code owns one and hands it out through its package() function.
struct shader_package
{
    /// Identifies the package and, by default, where it mounts.
    cc::string_view name;

    shader_language language = shader_language::hlsl;

    /// Absolute path to the shader sources, baked at configure time. May not exist — a shipped build
    /// has no source tree, and then the embedded files answer instead.
    cc::string_view source_dir;

    /// Every source file the package needs, including the transitive `#include` closure.
    cc::span<embedded_file const> embedded_files;

    cc::span<shader_definition const> definitions;
};
} // namespace slib
