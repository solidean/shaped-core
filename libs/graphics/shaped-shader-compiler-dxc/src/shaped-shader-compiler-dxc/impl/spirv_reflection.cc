#include <shaped-shader-compiler-dxc/impl/reflection.hh>

namespace ssc::dxc::impl
{
cc::result<reflected_shader> reflect_spirv(cc::span<byte const> spirv, sg::shader_stage, cc::string_view)
{
    // A SPIR-V module carries its bindings in its own instruction stream rather than in a container beside it, so
    // this reads the module — which is a parser, not a query.
    //
    // Not implemented yet: it wants SPIRV-Reflect vendored under extern/, which is a separate change.
    // Erroring rather than returning empty bindings is deliberate.
    // An empty reflection is not a degraded answer but a wrong one: a pipeline built from it declares no resources
    // at all, and either fails at bind time or silently binds nothing.
    // That is exactly the quiet wrongness a stub should never produce.
    (void)spirv;
    return cc::error("SPIR-V reflection is not implemented yet (needs SPIRV-Reflect); the compile itself succeeded");
}
} // namespace ssc::dxc::impl
