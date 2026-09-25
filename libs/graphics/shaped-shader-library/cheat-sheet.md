# shaped-shader-library cheat sheet

Shader packages + hot reload.
Namespace `slib`, depending on shaped-graphics, plus shaped-graphics-language privately for the SGL compiler edge.
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
    LANGUAGE   hlsl             # optional, hlsl by default; also wgsl and sgl
    SHADERS
        vignette.hlsl:compute:main          # path:stage:entry_point
        blit.hlsl:vertex:main_vs            # same file, two entry points -> two assets
        blit.hlsl:fragment:main_ps
        frame.hlsli:binding:frame_bindings  # path:binding:namespace -> a typed binding-group struct
        mesh.hlsl:vertex_input:vs_input     # path:vertex_input:struct -> a C++ mirror + vertex_layout_of
        rt.hlsl:payload:pt_payload          # path:payload:struct -> a C++ mirror + max_payload_size
        shade.hlsl:constants:gConstants)    # path:constants:name -> a C++ mirror with HLSL's padding
# stages are spelled as sg::shader_stage: compute vertex fragment tessellation_control
#   tessellation_evaluation geometry raygen closest_hit any_hit miss intersection callable
# an SGL package spells its stages as SGL does: `cube.sgl:vertex:main_vs`, `cube.sgl:pixel:main_ps`.
#   `pixel` reaches sg as shader_stage::fragment; the symbol has no stage in it (cube.main_ps), since the entry point carries one.
#   ONE file holds both stages, and ONE package serves dx12, vulkan and webgpu.
#   payload / constants entries are HLSL's alone, and a WGSL package names no generating entry at all.
#   `binding` and `vertex_input` mean an SGL declaration in an SGL package, below.
# an SGL package has its own generating kinds, read by the COMPILER (`sgl describe`), never by a parser here:
#   cube.sgl:*                          # every entry point, binding, @vertex / @pixel struct and pipeline the file declares
#   cube.sgl:binding:frame              # a `binding` block;  cube.sgl:vertex_input:v  a `@vertex struct`
#   cube.sgl:render_target:target       # a `@pixel struct`; a name the file does not declare is a build error
#   cube.sgl:pipeline:pipeline          # a `pipeline` declaration
#   `*` needs no stage word: an SGL entry point carries its stage in the source.
#   those need a runnable `sgl` while building: the tree's own natively, SC_SGL_TOOL otherwise (a cross build,
#   SC_BUILD_TOOLS=OFF). dev.py builds the host one for a cross preset itself. Entry points alone need neither.
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
lib.backlog()                    // -> cc::async_backlog const&; every shader any compiler handed out; a cold one counts once started
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
slib::shader_language              // hlsl | wgsl | sgl   (slang/glsl planned)
slib::include_resolver             // cc::function_ref<cc::optional<cc::string>(cc::string_view path)>
slib::shader_source_description    // { cc::string source; cc::string entry_point; sg::shader_stage stage; cc::string label; }
                                   //   label = what a diagnostic calls the source; never opened, may be empty
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

#include <shaped-shader-library/compiler/wgsl_compiler.hh>  // every platform, WebAssembly included
slib::create_wgsl_compiler()       // -> std::unique_ptr<shader_compiler>; wgsl -> wgsl, the source IS the bytecode
                                   //   reflection only: a stage or entry point other than the package's is an async error

#include <shaped-shader-library/compiler/sgl_compiler.hh>   // wherever the inner compiler exists
slib::create_sgl_compiler(std::unique_ptr<shader_compiler> inner)
                                   // -> std::unique_ptr<shader_compiler>; sgl -> inner->target_format()
                                   //   dxil -> HLSL for dx12, spirv -> HLSL for vulkan, wgsl -> WGSL
                                   //   preprocess IS SGL's pipeline, so the flattened source is the EMITTED TEXT;
                                   //   compile and reflection are the inner compiler's
                                   //   an SGL error is a preprocess error: `pkg/cube.sgl:12:5: error: unknown-name: foo`
                                   //   the binding pass runs behind it: the HLSL names each group, the pass writes registers
lib.add_compiler(slib::create_sgl_compiler(slib::create_wgsl_compiler()));   // one edge per format you can build

#include <shaped-shader-library/binding/wgsl_declarations.hh>
slib::parse_wgsl_declarations(src) // -> cc::result<wgsl_declarations>; { stage; entry_point; workgroup_size; bindings }
                                   //   exactly ONE entry point per module; never looks inside a function body
                                   //   module-scope order is free: a const may be declared below its use
                                   //   group 3 is sg's: binding 0 = inline constants, k >= 1 = static sampler k - 1
                                   //   gotcha: every `sampler` reports filtering, every f32 texture filterable_float
                                   //   (multisampled excepted); a caller binding unfilterable data changes them
```

## binding groups

```cpp
#include <shaped-shader-library/binding/binding_groups.hh>
slib::shader_binding_group         // { name; u32 group; vector<sg::binding> bindings; vector<declared_sampler> static_samplers }
                                   //   bindings are in declaration order -> position IS the layout slot
slib::declared_sampler             // { cc::string name; sg::sampler sampler } -- one marked `static`
slib::shader_vertex_input          // { name; u32 slot; bool per_instance; vector<shader_struct_member> }
slib::shader_payload               // { name; vector<shader_struct_member> members; isize size }
slib::shader_inline_constants      // { name; u32 space; type; members (with offsets); isize size }
slib::shader_bindings              // { groups; optional<inline_constants>; vertex_inputs; payloads }
slib::parse_binding_groups(hlsl)   // -> cc::result<shader_bindings>; the error names file:line
                                   //   (recovered from the flatten's #line directives)
slib::rewrite_binding_groups(hlsl, format)
                                   // -> cc::result<cc::string>; writes register()/[[vk::binding]] and strips
                                   //   the pragmas. Runs in _compile_text, between preprocess and compile.
```

A `path:binding:namespace` entry generates a typed struct for one group, in `<NAMESPACE>::<namespace>`:

```cpp
using group = my::shaders::frame_bindings;   // the annotated namespace IS the type
group::group_index                 // -> constexpr int; the number the attribute gave
group::declared_bindings()         // -> cc::span<sg::binding const>; the WHOLE table, in slot order
group::declared_samplers()         // -> cc::span<sg::named_sampler const>; the ones marked `static`
group::self_check()                // -> cc::string; empty while the table still describes its own shader
<NAMESPACE>::self_check()          // -> cc::string; every group in the package, for the owning target's test
                                   //   NOT called on the render path: it re-parses the embedded source
                                   //   a LIBRARY's generated header reaches its sibling <target>-test, which is what calls this
                                   //   (sc_finalize_shader_packages hands it the include dir)
// one member per binding: sg::bound_view for a resource, sg::sampler for a (non-static) sampler.
// a `static` sampler has NO member -- it is baked into the layout, though it still takes its slot.
// the layout is built from the full DECLARED table, not from whatever subset one stage reflected.
// the struct is DATA: acquiring, creating and binding are sg's scopes' -- see the section below.
```

A `path:vertex_input:struct` entry generates the C++ struct the buffer holds, plus its `sg::vertex_layout_of`:

```cpp
my::shaders::vs_input              // struct { float position[3]; float normal[3]; ... }
sg::vertex_input_layout::create<my::shaders::vs_input, my::shaders::instance_input>()
// the mirror DEFINES the byte layout and the specialization states that same layout, so the two cannot
//   disagree; generated static_asserts pin the stride and every member's offsetof.
// members are naturally packed -- a vertex buffer is a byte stream the IA decodes per attribute offset,
//   so HLSL's constant-buffer packing never enters into it.

my::shaders::pt_payload::max_payload_size   // constexpr cc::isize; what the pipeline must declare
// a payload mirror is naturally packed too, for a different reason: a payload is registers, not a buffer.

my::shaders::frame_constants               // the inline-constants mirror, with HLSL's padding
// a constant block REPRODUCES a layout rather than defining one: an element may not straddle a 16-byte row,
//   a row is filled before it is left, and the total rounds up to a row (spike Q14).
// the block's struct must be declared in the same file. scalars, vectors, bool and the float4xC matrices;
//   an array or a nested struct is refused, because the member after one packs into its last row (Q14b, Q14d).
// on SPIR-V the pass also writes [[vk::offset(n)]] per member: -fvk-use-dx-layout does NOT reach a
//   push-constant block, so without them DXC packs it scalar-tight and the mirror reads the wrong bytes.
```

```hlsl
#pragma sc group 0                        // the SPIR-V set, and the ONLY address anyone writes; register and
                                          //   space are the pass's output (group n -> space n today, a choice)
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
// `#pragma sc format <sg::pixel_format>` before an RWTexture* states its storage_format, and on SPIR-V
//   writes [[vk::image_format]] too; it is how SGL's HLSL states an image's format.
// `#pragma sc push_constants` before a ConstantBuffer makes it inline constants: register(b0, space9) on
//   DXIL, [[vk::push_constant]] on SPIR-V. NO arguments -- the space is slib::inline_constants_space,
//   reserved, and a group numbered 9 is refused rather than the block naming a space to avoid.
//   At most one per translation unit; block_size still comes from reflection, and the mirror is generated.
//   On SPIR-V each member also gets [[vk::offset(n)]]: -fvk-use-dx-layout does NOT reach a push-constant
//   block, so without them DXC packs it scalar-tight and the generated mirror is wrong on vulkan.
// `#pragma sc vertex_input [slot=<n>] [per_instance]` before a struct numbers its members by declaration
//   order -- [[vk::location(n)]] on SPIR-V, nothing on DXIL, where the semantic already names the input.
//   ONE counter across every annotated struct in the file, since a location is flat per stage.
//   the STRUCT's slot is its own declaration order too; `slot=` overrides that, for two shaders sharing a
//   vertex-input header while declaring their structs in a different order. Two structs on one slot is an error.
//   a member's type must have a vertex attribute format, so `bool` is refused here (it has none).
// `#pragma sc attribute format=<sg::vertex_attribute_format>` before a MEMBER states a packed format.
//   the only way to reach rgba8_unorm / rgba8_uint: HLSL spells a float4 fed by four normalized bytes
//   exactly like one fed by four floats, so it cannot be derived from the type.
// `#pragma sc payload` before a struct generates its C++ mirror and the max_payload_size a pipeline must
//   declare. A payload packs at NATURAL alignment, not in a constant buffer's 16-byte rows -- the spike's
//   Q13 measured that: CreateStateObject accepts the natural size and refuses one field less.
// a MATRIX is declared BARE and the pass writes `column_major` in front of it, the way it writes an address.
//   `row_major` / `column_major` by hand are both errors: MSL and WGSL have no row-major matrices at all.
//   Admitted: float4x1..float4x4, at 16C bytes, mirrored as float[4C] -- a column of four is the only shape
//   whose extent matches on all four targets, since a narrower one pads (spike Q14g).
// text carrying no attribute is not interpreted, so the rewrite provably touches only what it parsed --
//   a source with no `#pragma sc` keeps whatever it wrote, which is how ordinary HLSL still compiles.
// in a source that DOES carry one, a hand-written `register(...)` or `[[vk::...]]` is an ERROR naming the line.
//   there is no opt-out mark: a shader wanting its own addresses is ordinary HLSL, outside a package.
```

## the generated group is data; the verbs are sg's scopes'

```cpp
// an annotated namespace becomes ONE type, named after it, satisfying sg::declared_binding_group:
//   the fields, `group_index`, `declared_bindings()`, `declared_samplers()`, `gather()` and `self_check()`.
auto const layout = ctx.cached.acquire_binding_group_layout<shaders::frame_bindings>();
auto const layout = ctx.cached.acquire_binding_group_layout<shaders::frame_bindings>(runtime_samplers);
                                    // + static samplers for the ones the shader left undeclared;
                                    //   supplying one it DID declare asserts -- it is a mistake, not an override
auto const g = ctx.transient.create_binding_group(cmd, layout, shaders::frame_bindings{.albedo = tex.as_texture_view()});
                                    // the LAYOUT is passed in: a group is created on the frame path, and
                                    //   acquiring hashes the table and takes the pipeline cache's lock
                                    // a sampler the group gathers that `layout` declares static is dropped
scope.bind<shaders::frame_bindings>(*g);   // binds at G::group_index, on raster / compute / raytracing
```

### an SGL package's generated types

```cpp
// `binding work` -> shaders::work: one field per member, in the shader's order, plus declared_bindings(), declared_samplers(), gather().
//   a buffer member is a TYPED view: `mut buffer[float]` -> sg::readwrite_buffer_view<float>, so a read-only view or
//   a buffer<int> does not compile. A plain member is a plain field (`scale: float` -> float): the group's own
//   constant buffer, which create_binding_group allocates with the scope's lifetime: a transient one is uploaded
//   inline into the `cmd` it is given, a persistent one through ctx.upload.
//   sg::declared_binding_set, NOT declared_binding_group: no group_index, because SGL numbers a group by its
//   position in each entry point's list. Bind it at the index the pipeline has it at:
auto const layout = ctx.cached.acquire_binding_group_layout<shaders::work>();
auto const group = ctx.transient.create_binding_group(cmd, layout, shaders::work{.scale = 2.0f, .values = buf.as_readwrite_buffer()});
cmd.compute.bind_group(0, *group);        // group 0 of `main`, group 1 of an entry point listing {factor, work}
// a texture or image member is a typed view too, its traits from the shape (`sg::tv_2d`, `sg::tv_cube`, `sg::tv_2d_array`…):
//   `albedo: texture2d[float4]`        -> sg::texture_view<sg::tv_2d> albedo
//   `dst: out image2d[.rgba8_unorm]`   -> sg::image_view<sg::tv_2d> dst   (any access: read, out, mut)
//   `user_smp: sampler`                -> sg::sampler user_smp, which gather() hands sg by its host name (`work.user_smp`)
//   `sampler albedo_smp:` block        -> NO field: an sg::named_sampler in declared_samplers(), which the layout carries
// `@inline binding constants` -> shaders::constants: plain fields in C++'s layout, and the block the shader reads:
pass.set_inline_constants(shaders::constants{.view_projection = vp}.to_block());
// every name lives in the package namespace, so two files declaring one name is a generator error.
// sg sees an SGL binding by its path, `work.values`, and a group's constant block by the binding's name:
//   slib renames what the target's compiler reflected, which stays on each binding as `reflected_name`.
// `@inline binding constants` also gives constants::inline_binding(): the pipeline layout's inline block, no reflection.
// an entry point of a `*`-declared file is a small wrapper: `->acquire(ctx)` as before, plus the layout its list states:
auto const layout = shaders::cube.main_vs.acquire_layout(ctx);                  // {constants}, nothing reflected
auto const pipeline = co_await shaders::double_values.main.acquire_pipeline(ctx); // compute: needs nothing else
// a raster pipeline whose stages list different groups takes their union instead: acquire_pipeline_layout<frame, work>().
// a `pipeline` declaration -> shaders::<file>.<name> (an unnamed `pipeline:` is `.pipeline`), built from ITS stages,
//   layout, vertex input, targets and settings; the host states only what the declaration left `.host`.
//   It is an sg::raster_pipeline_source, so ctx.cached acquires it like a description:
auto const p = co_await ctx.cached.acquire_raster_pipeline(shaders::cube.pipeline, {.color = swapchain_format}); // open: one field per `.host` part
//   nothing `.host` -> acquire_raster_pipeline(shaders::cube.pipeline); a last argument customize(sg::raster_pipeline_description&) runs last
//   .description(ctx, parts)          the description itself, to build or inspect
//   .description_latest(ctx, parts)   the newest stages and settings even where the frozen part moved; acquire it yourself
//   an open field left unset (a format still `undefined`, a sample count still 0) asserts: the declaration said the host would state it
//   the build's settings are generated field writes (slib::impl::fields, from impl/pipeline_fields.hh, which `sgl pipeline-fields` writes)
//   hot reload: cull, depth, blend… follow the source; a moved frozen part (layout, vertex input, targets, formats, samples,
//   each struct by name AND shape) keeps the stages and settings this context last built with, and logs what moved
// `@vertex struct v` -> shaders::v and v::layout(): attributes in the shader's order, no semantic or offset by hand.
//   members marked `@per_instance` / `@stream(name)` split it over buffers: then v::<stream> per buffer, in slot order,
//   and v::buffers{.per_vertex = verts, .per_instance = insts}.views() for bind_vertex_buffers — typed, so a
//   buffer of the wrong stream does not compile.
// `@pixel struct target` -> shaders::target: one sg::color_target per member, by name, plus an optional depth_stencil.
cmd.raster.render_to(shaders::target{.color = rt.cleared(c), .depth_stencil = depth.cleared(1.0f)}); // -> rendering_info
//   the pipeline side, for one built by hand: .color_targets = shaders::target::states{.color = {.format = f}}, .target_set = shaders::target::name
//   sg then refuses to bind that pipeline in a rendering of another target set, even one of the same shape.
//   .target_set may be left out: a compiled SGL pixel shader states its own (and its target count), which sg takes.
// a package with wrapped entry points also gets check_reflection(ctx) -> shared_async<string>: empty while every
//   entry point's compiled reflection fits the groups it lists. Compiles them all, so it belongs in a test.
CHECK(co_await shaders::check_reflection(*ctx) == "");
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
