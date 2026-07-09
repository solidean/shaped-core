# shaped-shader-compiler-dxc cheat sheet

DXC wrapper: HLSL -> `sg::compiled_shader`. Namespace `ssc::dxc`. Windows-only. Depends on shaped-graphics.
Headers are included by full path from `src/`: `#include <shaped-shader-compiler-dxc/<name>.hh>`.

> **Scope note:** covers the small surface that exists today — compute shaders to DXIL, with reflection.
> Fallible calls return `cc::result`. Format conventions live in
> [docs/guides/cheat-sheets.md](../../../docs/guides/cheat-sheets.md).

How to read this: each block leads with the include; one symbol per line with a trailing comment.

---

## options & inputs

```cpp
#include <shaped-shader-compiler-dxc/compile_options.hh>
ssc::dxc::compile_target        // dxil   (spirv/metal_lib slot in later)
ssc::dxc::shader_model          // sm_6_0 .. sm_6_8   -> profile suffix "6_8"
ssc::dxc::optimization_level    // disabled(-Od) | level_0..level_3(-O0..-O3)
ssc::dxc::compile_options       // { target; optimization; bool debug_info; bool warnings_as_errors;
                           //   cc::vector<cc::string> defines; cc::vector<cc::string> extra_args }
                           //   debug_info -> -Zi -Qembed_debug; warnings_as_errors -> -WX

#include <shaped-shader-compiler-dxc/shader_description.hh>
ssc::dxc::shader_description     // { cc::string source; cc::string entry_point="main";
                           //   sg::shader_stage stage=compute; ssc::dxc::shader_model model=sm_6_8 }
                           //   stage+model form the DXC profile ("cs_6_8")

#include <shaped-shader-compiler-dxc/preprocessed_source.hh>
ssc::dxc::preprocessed_source   // { cc::string source; cc::string warnings }  — flattened HLSL + diagnostics
```

## compiler — the two-step API

```cpp
#include <shaped-shader-compiler-dxc/compiler.hh>
ssc::dxc::include_resolver       // cc::function_ref<cc::optional<cc::string>(cc::string_view path)>
                           //   maps an #include path to source text; nullopt = not found (an error)
ssc::dxc::compiler               // move-only; owns IDxcUtils + IDxcCompiler3 (pimpl). Not thread-safe.
ssc::dxc::compiler::create()                     // -> cc::result<compiler>   (fails only on a broken DXC install)
c.preprocess(desc, resolve_include, opts={})// -> cc::result<preprocessed_source>  (-P; resolves #includes)
c.compile(desc, opts={})                    // -> cc::result<sg::compiled_shader>  (rejects #includes)
// compile() output: stage/format=dxil/entry_point set; bytecode = DXIL blob; bindings + (compute)
// workgroup_size from reflection; compiler = {"dxc", version, joined-args signature}
```

## reflection mapping (DXC -> sg::binding)

```
D3D12_SHADER_INPUT_BIND_DESC        ->  sg::binding
  BindPoint  -> index               (register number)      // sg address model: index=register,
  Space      -> set                 (register space)        //   set=space, class from binding_type
  BindCount  -> count                                       //   (see shaped-graphics binding.hh)
  Name       -> name
  Type -> binding_type: CBUFFER->uniform_buffer(+block_size); STRUCTURED->readonly_structured;
          BYTEADDRESS->readonly_raw; UAV_RWSTRUCTURED->readwrite_structured; UAV_RWBYTEADDRESS->readwrite_raw;
          TEXTURE->readonly_texture; UAV_RWTYPED->readwrite_texture; SAMPLER->sampler
          (TEXTURE/RWTYPED with a BUFFER dimension = a typed/texel buffer -> unsupported, see below)
// no remapping — each backend reinterprets (set,index,type). Recorded faithfully.
// unsupported kinds (texel/typed buffers, append/consume/counter buffers, accel) -> cc::error until sg grows.
// GOTCHA: DXC's DXIL reflection drops declared-but-unused bindings (a SPIR-V pass would keep them).
```
