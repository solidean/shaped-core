#pragma once

#include <clean-core/string/string.hh>
#include <shaped-graphics/binding/compiled_shader.hh>
#include <shaped-shader-compiler-dxc/compile_options.hh>
#include <shaped-shader-compiler-dxc/fwd.hh>

/// One shader to compile: its HLSL source plus what and how to build it.
/// For compile(), `source` must already be fully preprocessed (see compiler::preprocess).

struct ssc::dxc::shader_description
{
    /// HLSL source text.
    /// For compile() this must be self-contained, with every `#include` already resolved (compile() rejects includes).
    /// For preprocess() it may still contain `#include`s.
    cc::string source;

    /// Entry-point function name.
    /// Ignored by preprocess().
    cc::string entry_point = "main";

    /// Pipeline stage — selects the profile prefix (compute -> "cs", vertex -> "vs", any ray-tracing stage -> "lib").
    sg::shader_stage stage = sg::shader_stage::compute;

    /// HLSL shader model — the profile suffix (sm_6_8 -> "cs_6_8").
    shader_model model = shader_model::sm_6_8;
};
