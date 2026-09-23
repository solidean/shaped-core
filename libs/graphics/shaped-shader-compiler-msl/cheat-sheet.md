# shaped-shader-compiler-msl cheat sheet

Metal wrapper: MSL -> `sg::compiled_shader`.
Namespace `ssc::msl`.
Depends on shaped-graphics.
Apple only, and built there unconditionally — the toolchain is a run-time lookup, never a configure gate.
Headers are included by full path from `src/`: `#include <shaped-shader-compiler-msl/<name>.hh>`.

> **Scope note:** compute, vertex and fragment, plus the six ray-tracing stages; tessellation and geometry are refused by name.
> Fallible calls return `cc::result`.
> Format conventions live in [docs/guides/cheat-sheets.md](../../../docs/guides/cheat-sheets.md).

How to read this: each block leads with the include; one symbol per line with a trailing comment.

---

**Recording domain:** `ssc.msl`.
Every `CC_LOG_*` and `CC_RECORD_*` site in this library is attributed to it; see [logging](../../base/clean-core/docs/logging.md).

## options & inputs

```cpp
#include <shaped-shader-compiler-msl/compile_options.hh>
ssc::msl::artifact_kind      // automatic (metallib where there is a toolchain, else source) | metallib | msl_source
ssc::msl::optimization_level // disabled(-O0) | level_1 | level_2 (default) | level_3
ssc::msl::compile_options    // { artifact; optimization; bool debug_info; bool warnings_as_errors;
                             //   cc::string language_version ("metal3.2" -> -std=); defines; extra_args }
                             //   debug_info -> -frecord-sources; warnings_as_errors -> -Werror; -I goes in extra_args

#include <shaped-shader-compiler-msl/shader_description.hh>
ssc::msl::shader_description // { cc::string source; cc::string entry_point="main";
                             //   sg::shader_stage stage=compute; cc::optional<sg::compute_dimensions> workgroup_size }
                             //   workgroup_size overrides `#pragma sc numthreads x y z`; absent = derive at dispatch
```

## compiler

```cpp
#include <shaped-shader-compiler-msl/compiler.hh>
ssc::msl::toolchain_info     // { bool is_available; cc::string version; cc::string driver_path }
ssc::msl::compiler           // move-only; holds what `xcrun -f metal` resolved. One per thread.
ssc::msl::compiler::create() // -> cc::result<compiler>; NEVER fails for want of a toolchain
c.compile(desc, opts={})     // -> cc::result<sg::compiled_shader>
c.toolchain()                // -> toolchain_info const&; version belongs in any persistent cache key
// compile() output: stage/entry_point set; format = metal_lib or msl per the arm that ran;
// bindings + workgroup_size from the SOURCE; compiler = {"metal", version, "<arm> <args>"}
```

## reflection mapping (MSL -> sg::binding)

```
struct frame { texture2d<float> albedo [[id(0)]]; };     ->  group_index = the [[buffer(N)]] the struct is bound at
kernel void k(constant frame& f [[buffer(0)]])               index       = the member's [[id(n)]]
                                                             space       = absent (MSL has no register spaces)

texture*<...>            -> readonly_texture, + texture_dimension     (access::write / read_write -> readwrite_texture)
sampler                  -> sampler
constant T& / constant T*-> uniform_buffer
device T*                -> readwrite_structured_buffer              (a `const` pointee -> readonly_structured_buffer)
raytracing::*_acceleration_structure -> acceleration_structure
T name[k]                -> count = k, occupying k CONSECUTIVE indices
// GOTCHA: every `device T*` is STRUCTURED. MSL spells a raw byte-addressed buffer identically, so the text cannot
//   tell them apart — a shader needing a raw buffer is a reason to grow the rule, not to guess.
// GOTCHA: a declared-but-unreferenced binding IS reported, unlike DXIL reflection, because the text declares it.
// [[stage_in]] and the built-ins ([[thread_position_in_grid]], ...) bind nothing and are skipped.
```

## stages

```cpp
compute, raygen            -> `kernel`     // Metal schedules no raygen: the kernel runs the traversal itself
vertex                     -> `vertex`
fragment                   -> `fragment`
miss/closest_hit/any_hit/intersection/callable -> `[[visible]]`   // what a shader table links
tessellation_*, geometry   -> cc::error naming what Metal has instead
// An entry point declared with the wrong qualifier is an error HERE, not at pipeline creation.
```
