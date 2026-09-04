# shaped-shader-library cheat sheet

Shader packages + hot reload.
Namespace `slib`, depending on shaped-graphics.
Headers are included by full path from `src/`: `#include <shaped-shader-library/<topic>/<name>.hh>`.

> **Start at [shaders.md](../shaped-graphics/docs/shaders.md)** for how the whole shader system fits together.
> Format conventions live in [docs/guides/cheat-sheets.md](../../../docs/guides/cheat-sheets.md).

How to read this: each block leads with the include; one symbol per line with a trailing comment.

---

**Recording domain:** `slib`.
Every `CC_LOG_*` and `CC_RECORD_*` site in this library is attributed to it; see [logging](../../base/clean-core/docs/logging.md).

## declaring a package (CMake)

```cmake
sc_add_shader_package(
    TARGET     my-renderer      # any target: a lib, an app, or a *-test binary
    NAME       my_shaders       # package id -> header name <my_shaders.hh>, and the mount point
    NAMESPACE  my::shaders      # where the generated symbols live
    SOURCE_DIR shaders          # relative to the calling CMakeLists (must define TARGET)
    LANGUAGE   hlsl             # optional; hlsl is the default and the only one today
    SHADERS
        vignette.hlsl:compute:main          # path:stage:entry_point
        blit.hlsl:vertex:main_vs            # same file, two entry points -> two assets
        blit.hlsl:fragment:main_ps
        frame.hlsli:binding:frame_bindings  # path:binding:namespace -> a typed binding-group struct
        mesh.hlsl:vertex_input:vs_input)    # path:vertex_input:struct -> a C++ mirror + vertex_layout_of
# stages are spelled as sg::shader_stage: compute vertex fragment tessellation_control
#   tessellation_evaluation geometry raygen closest_hit any_hit miss intersection callable
# generated at BUILD time into the binary dir; PRIVATE to TARGET. Editing a shader (or an .hlsli it
#   includes) regenerates; a reconfigure that changes nothing rebuilds nothing.
# a binding entry generates from the NAMED FILE and never from its includes, so an .hlsli that declares a
#   group is registered on its own -- otherwise every shader including it would generate the struct again.
# validates: the file exists, the stage is real, no duplicate entries, no two files colliding on one C++ id.
# call sc_finalize_shader_packages() ONCE at the bottom of the root CMakeLists: it turns "slib was never
#   added" into a clear message instead of a missing header inside generated code at build time.
# on Windows+DXC an EXECUTABLE that declares a package also gets dxcompiler.dll + dxil.dll staged beside
#   it; declared on a LIBRARY target it stages nothing, and the exe must copy them itself.
```

## generated symbols

```cpp
#include <my_shaders.hh>                    // the NAME above
my::shaders::vignette.compute.main          // slib::shader_asset_handle  — stem.stage.entry_point
my::shaders::blit.vertex.main_vs            //   a typo here is a COMPILE error; that is the point
// a subdirectory folds into the stem, and '-' becomes '_':  post/manga-render.hlsl -> post_manga_render
my::shaders::package()                      // -> slib::shader_package const&  (pass to add_package)
// the handles are null until add_package fills them in
```

## startup

```cpp
#include <shaped-shader-library/shader_library.hh>
slib::shader_library lib;                   // not a singleton, but only ONE may exist at a time
lib.add_compiler(std::unique_ptr<shader_compiler>);   // a later compiler for the same edge replaces it
lib.add_package(my::shaders::package());    // mounts embedded, then SOURCE_DIR over it (a missing dir finds nothing)
lib.add_package(pkg, filesystem_handle fs); // explicit fs instead (tests: a memory_filesystem)
lib.mount(virtual_dir, fs);                 // shared includes that belong to no package
lib.start_hot_reload(cfg = {});             // AFTER every add_package (adding later asserts)
lib.poll_hot_reload();                      // no-op unless started unthreaded; safe every frame
lib.is_hot_reloading();                     // -> bool
lib.generation();                           // -> u64; coarse "some shader changed" (prefer the asset's). Reads the global below
slib::current_reload_generation();          // -> u64; it *is* sg::reload_generation()
// The counter is sg's: a consumer like an sg render routine reads it directly, with no slib dependency.
lib.can_compile(language, format);          // -> bool;  lib.supported_formats(language) -> vector
lib.assets();                               // -> span<shader_asset_handle const>
lib.filesystem();                           // -> mount_table const&  (everything mounted)
lib.compile_source(src, stage, entry, format, opts = {})  // -> sg::async_compiled_shader; text that was never a file
slib::compile_source_options     // { shader_language language = hlsl; string_view include_dir; string_view label = "<generated>"; }
// The door for GENERATED / downloaded / UI-authored shader text: compile_shader resolves the package owning a path, and such a
// source is under none. Includes still resolve against the mount table, so generated code may pull in a package's .hlsli files.
// NOTHING is cached — a caller generating a source already has a better key for it than a hash of the text would be.

slib::reload_config                         // { double interval_ms = 200; bool unthreaded = false;
                                            //   bool force_polling = false; }
                                            //   the filesystem NOTIFIES where it can -> no interval, no
                                            //   periodic wakeup, idle costs nothing. interval_ms only
                                            //   applies to the polling fallback (no backend / no threads /
                                            //   force_polling), and force_polling is for testing that path.
                                            //   unthreaded: no thread; poll_hot_reload() IS the scan
                                            //   (and the recompile). Forced where there are no threads.
```

## using a shader

```cpp
#include <shaped-shader-library/shader_asset.hh>
asset->acquire(sg::context const&)  // -> sg::async_compiled_shader in a format THE CONTEXT accepts;
                                    //    async error if no registered compiler reaches one
asset->acquire(sg::shader_format)   // -> explicit format (tests/tools with no context)
asset->generation()                 // -> u64; moves when a reload replaced the shader. Cache it.
asset->last_error()                 // -> optional<string>; why the last reload was rejected
asset->virtual_path() / stage() / entry_point()
asset->dependencies()               // -> vector<string>; source + resolved includes (what is watched)
// GOTCHA: acquire returns a COLD cc::async node. The ambient scheduler's workers run it, or block on it with
//   cc::try_async_blocking_get(sh). Lazy + per format: nothing compiles until asked, and each format is
//   compiled separately from the same source.
```

## the compiler seam

```cpp
#include <shaped-shader-library/compiler/shader_compiler.hh>
slib::shader_language              // hlsl   (slang/glsl/wgsl planned)
slib::include_resolver             // cc::function_ref<cc::optional<cc::string>(cc::string_view path)>
slib::shader_source_description    // { cc::string source; cc::string entry_point; sg::shader_stage stage; }
slib::shader_compiler              // ONE edge: source_language() -> target_format()
                                   //   preprocess(desc, resolve) -> cc::result<cc::string>  (flattens #includes)
                                   //   compile(desc) -> sg::async_compiled_shader  (errors on the node, no throw)
                                   //   must be callable from several threads at once

#include <shaped-shader-library/compiler/dxc_compiler.hh>   // only when SLIB_HAS_DXC
slib::create_dxc_compiler()        // -> cc::result<std::unique_ptr<shader_compiler>>; hlsl -> dxil
                                   //   Windows in practice: DXIL reflection needs the Windows SDK
slib::create_dxc_spirv_compiler()  // the same, hlsl -> spirv; works everywhere DXC does
                                   //   register BOTH: a shader_asset picks by what the context accepts
                                   //   content-keyed cache inside: an identical recompile is free
```

## binding groups

```cpp
#include <shaped-shader-library/binding/binding_groups.hh>
slib::shader_binding_group         // { cc::string name; u32 group; cc::vector<sg::binding> bindings; }
                                   //   bindings are in declaration order -> position IS the layout slot
slib::shader_inline_constants      // { cc::string name; u32 space; } -- register is always b0
slib::shader_vertex_input          // { name; u32 slot; bool per_instance; vector<shader_struct_member> }
slib::shader_bindings              // { groups; optional<inline_constants>; vector<vertex_inputs> }
slib::parse_binding_groups(hlsl)   // -> cc::result<shader_bindings>; the error names file:line
                                   //   (recovered from the flatten's #line directives)
slib::rewrite_binding_groups(hlsl, format)
                                   // -> cc::result<cc::string>; writes register()/[[vk::binding]] and strips
                                   //   the pragmas. Runs in _compile_text, between preprocess and compile.
```

A `path:binding:namespace` entry generates a typed struct for one group, in `<NAMESPACE>::<namespace>`:

```cpp
using group = my::shaders::frame_bindings::group;
group::group_index                 // -> constexpr sg::u32; the number the attribute gave
group::declared_bindings()         // -> cc::span<sg::binding const>; the WHOLE table, in slot order
group::declared_samplers()         // -> cc::span<sg::named_sampler const>; the ones marked `static`
group::acquire_layout(ctx)         // -> sg::binding_group_layout_handle; constant, no reflection consulted
group::acquire_layout(ctx, samplers)  // + static samplers for the ones the shader left undeclared;
                                   //   a declared one WINS, and supplying it again asserts
group::self_check()                // -> cc::string; empty while the table still describes its own shader
group{.albedo = tex.as_readonly_view(), .linear_sampler = {}, ...}.create(ctx)
                                   // -> cc::result<sg::binding_group_handle>
// one member per binding: sg::bound_view for a resource, sg::sampler for a (non-static) sampler.
// a `static` sampler has NO member -- it is baked into the layout, though it still takes its slot.
// the layout is built from the full DECLARED table, not from whatever subset one stage reflected.
```

A `path:vertex_input:struct` entry generates the C++ struct the buffer holds, plus its `sg::vertex_layout_of`:

```cpp
my::shaders::vs_input              // struct { float position[3]; float normal[3]; ... }
sg::vertex_input_layout::create<my::shaders::vs_input, my::shaders::instance_input>()
// the mirror DEFINES the byte layout and the specialization states that same layout, so the two cannot
//   disagree; generated static_asserts pin the stride and every member's offsetof.
// members are naturally packed -- a vertex buffer is a byte stream the IA decodes per attribute offset,
//   so HLSL's constant-buffer packing never enters into it.
```

```hlsl
#pragma sc group 0                        // the group number is both the SPIR-V set and the HLSL space
namespace frame_bindings
{
    Texture2D<float4> albedo;             // index 0 -> t0/space0 and binding(0, 0)
    SamplerState linear_sampler;          // index 1 -> s1/space0 and binding(1, 0): ONE counter per group
}
// an attribute stands on its own line and applies to the declaration after it.
// a PRAGMA, not a comment: DXC's include flatten erases comments and keeps pragmas verbatim (spike Q11/Q12),
//   and the pass reads the FLATTENED source. The rewrite then strips the pragmas, since -Wall would reject them.
// a `#pragma sc` name the pass does not know is an ERROR naming the line, never a directive nobody reads.
// a pragma whose first word is not `sc` is passed through untouched.
// an array consumes `count` indices — DXIL numbers every element, SPIR-V numbers the array once.
// inside a group only `Type name;` / `Type name[N];` with N a literal; anything else is an error.
// `#pragma sc static <sg::sampler field>=<value>` before a sampler bakes it into the layout;
//   `filter=linear` sets all three filters, `address=clamp_edge` all three axes, and a tuple form
//   `filter=(linear, linear, nearest)` addresses them individually, in sg::sampler's declaration order.
// `#pragma sc push_constants space=<n>` before a ConstantBuffer makes it inline constants: register(b0,
//   space<n>) on DXIL, [[vk::push_constant]] on SPIR-V. At most one per translation unit; block_size still
//   comes from reflection.
// `#pragma sc vertex_input [slot=<n>] [per_instance]` before a struct numbers its members by declaration
//   order -- [[vk::location(n)]] on SPIR-V, nothing on DXIL, where the semantic already names the input.
// payload parses and is reported as not supported yet.
// text carrying no attribute is not interpreted, so hand-written register() at file scope stays fine.
```

## include resolution

```
#include "x.hlsli" is looked for, most specific first — all three resolved from the SHADER being
compiled, at every include depth, not from whichever file issued the #include:
  1. the shader's own directory        dir/a.hlsl  ->  dir/x.hlsli
  2. at the package root               dir/a.hlsl  ->  x.hlsli
  3. at the mount root                 reaches a shared mount:  #include "common/brdf.hlsli"
every resolved path is recorded as a dependency -> editing an .hlsli reloads what includes it.
```

## the virtual filesystem

```cpp
#include <shaped-shader-library/filesystem/filesystem.hh>
slib::file_revision                 // enum : u64; `none` = absent. Opaque — NOT a timestamp.
slib::filesystem                    // read_text(path) -> optional<string>;  revision(path);  exists(path)
                                    //   paths are '/'-separated, normalized, root-relative; '..' cannot escape
fs.watch(prefix, sink)              // -> optional<watch_subscription>; OPTIONAL capability, default nullopt

#include <shaped-shader-library/filesystem/watch.hh>
slib::watch_sink                    // cc::unique_function<void()>; fires from ANY thread -> enqueue + return
slib::watch_subscription            // move-only; ~it unsubscribes AND guarantees the sink is neither
                                    //   running nor callable again. Must not outlive its filesystem.
                                    //   A default-constructed one is VALID and never fires.
// watch() is a HINT TO RESCAN, never a report of what changed: it may coalesce, fire spuriously, and watch
// a whole directory when you asked for one file. revision() stays the truth. nullopt = "I cannot notify,
// poll me" -> which is NOT the same as a subscription that never fires ("nothing here can ever change").

#include <shaped-shader-library/filesystem/mount_table.hh>
slib::mount_table                   // a filesystem built from others: mount(virtual_dir, fs); mount_count()
                                    //   lookup: longest prefix first, then MOST RECENTLY mounted first
                                    //   -> an overlay is just two mounts at one prefix (embedded, then source)
                                    //   watch: composes every INTERSECTING mount (inside the prefix, or
                                    //   containing it). ANY of them nullopt -> the whole watch is nullopt.

#include <shaped-shader-library/filesystem/memory_filesystem.hh>
slib::memory_filesystem             // write(path, text) bumps the revision; remove(path). TESTS USE THIS:
                                    //   a hot reload is a write(), not a sleep — and write() fires the
                                    //   watch synchronously, so the notify path is deterministic too.
#include <shaped-shader-library/filesystem/embedded_filesystem.hh>
slib::embedded_file                 // { cc::string_view path; cc::string_view text; }  (generated, static)
slib::embedded_filesystem           // over a span of those; constant revision (nothing to reload)
                                    //   watch -> a subscription that NEVER fires (and never nullopt)
#include <shaped-shader-library/filesystem/real_filesystem.hh>
slib::real_filesystem               // rooted at a real dir; revision folds mtime+size.
                                    //   THE ONLY THING IN slib THAT TOUCHES THE DISK (with its watch
                                    //   backends). A missing root is not an error — it just finds nothing,
                                    //   which is how ship-vs-dev works.
                                    //   watch -> ReadDirectoryChangesW on Windows; nullopt on Linux/macOS
                                    //   (not yet written), under SC_THREADS=OFF, and for a missing dir.
```

## package types

```cpp
#include <shaped-shader-library/shader_package.hh>
slib::shader_definition   // { string_view path; sg::shader_stage stage; string_view entry_point;
                          //   shader_asset_handle* asset; }   asset = the generated global to fill in
slib::shader_package      // { string_view name; shader_language language; string_view source_dir;
                          //   span<embedded_file const> embedded_files; span<shader_definition const> definitions; }
                          //   source_dir is absolute + baked at configure; MAY NOT EXIST (a shipped build)
```

## gotchas

```
- ONE shader_library at a time, and a package added ONCE: the generated symbols are process-wide globals.
- add every package BEFORE start_hot_reload (asserts): the watcher walks the asset list from its thread.
- an asset only WEAKLY references its library, because a generated global (a static) outlives it.
  Acquiring through a stale global reports an error rather than dangling.
- a generated package header is PRIVATE to its target. To publish a shader, re-expose it from your own
  public header and own the drift (docs/coding-guidelines.md).
- a reload only recompiles formats someone has already acquired.
- watch() is a hint to rescan, NOT a report. If you find yourself plumbing changed *paths* through it,
  stop — revision() is the source of truth and that is what makes overflow/rename/coalescing all free.
- a shader is only watched once a compile has recorded what it is built from, i.e. after its first
  acquire(). Nothing is watching a shader nobody ever asked for.
```
