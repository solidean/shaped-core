#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics/binding/compiled_shader.hh>
#include <shaped-shader-library/compiler/shader_compiler.hh>
#include <shaped-shader-library/filesystem/embedded_filesystem.hh>
#include <shaped-shader-library/fwd.hh>

/// One shader in a package: which file, which stage, which entry point — plus the generated global to
/// write the asset handle back into.
struct slib::shader_definition
{
    cc::string_view path; ///< package-relative and must stay inside the package, e.g. "compute/invert.hlsl"
    sg::shader_stage stage;
    cc::string_view entry_point;
    /// The options of its SGL source the entry point reaches, by name; an acquire keys its compiles on these alone.
    cc::span<cc::string_view const> options;

    /// The generated global that call sites read; shader_library::add_package fills it in.
    /// Required — add_package asserts on a definition that names no global.
    shader_asset_handle* asset = nullptr;
};

/// A directory whose `.sgl` files are modules every SGL compile of the library may `use`.
struct slib::module_dir
{
    /// Where its files are, relative to the package's mount: empty for the package's own source dir.
    cc::string_view path;
    /// The directory on disk as the build saw it, which names the directory across packages: two packages listing one
    /// directory add its modules once.
    /// Mounted over the embedded copy where `path` is not empty, as `source_dir` is over the package's.
    cc::string_view source_dir;
};

/// A target's shaders, as emitted by sc_add_shader_package.
/// A pure description with static storage — generated code owns one and hands it out through its package() function.
struct slib::shader_package
{
    /// Identifies the package and, by default, where it mounts.
    cc::string_view name;

    /// The C++ namespace the package's generated types live in, which is what an SGL render target is named under.
    cc::string_view host_namespace;

    shader_language language = shader_language::hlsl;

    /// Absolute path to the shader sources, baked at configure time.
    /// May be empty, and may name a directory that is not there — a shipped build has no source tree, and the embedded files answer instead.
    cc::string_view source_dir;

    /// Every source file the package needs, including the transitive `#include` closure.
    cc::span<embedded_file const> embedded_files;

    cc::span<shader_definition const> definitions;

    /// An SGL package's module directories, its own source dir first; their files are among `embedded_files`.
    cc::span<module_dir const> module_dirs;
};
