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
#   module:view                         # every binding and @vertex / @pixel struct of module `view` (see modules below)
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

### SGL modules (CMake)

```cmake
sc_add_shader_package(
    ...
    LANGUAGE    sgl
    MODULE_DIRS shaders/modules         # more module directories; SOURCE_DIR always is one
    SHADERS
        module:view                     # export module `view`: its types into <NAMESPACE>::view
        blit.sgl:*)                     # `use view` in it resolves through the module directories
# a `use` finds a module among the .sgl files DIRECTLY in a module directory, grouped by their `module` line.
# module:view writes <sgl_modules/view.hh> (+ .cc) with the types AND `namespace sgl_modules { namespace view = ::NS::view; }`;
#   generated code names a module's types as ::sgl_modules::view::frame, so it needs that header and nothing else.
# that header's directory is PUBLIC on TARGET: linking the exporting target is what makes a module reachable.
#   another package using the module lists the exporter's module dir in its own MODULE_DIRS, by path.
#   a module nobody exports fails as a missing <sgl_modules/view.hh>, whose include line names the entry to add.
# every module file is embedded; a new one in a module dir re-runs configure and the generator on the next build.
# a module is exported by one package; a second `module:view` anywhere in the build is a configure error.
```

```cpp
#include <sgl_modules/view.hh>               // or through any package header naming one of its types
auto const layout = ctx.cached.acquire_binding_group_layout<sgl_modules::view::frame>();
auto const group = ctx.transient.create_binding_group(cmd, layout, sgl_modules::view::frame{...});
pass.bind_group(0, *group);                   // ONE group for every pipeline listing view.frame first: they share its layout
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
lib.add_module_dir(virtual_dir);            // a mounted dir of SGL modules that belongs to no package
lib.read_modules();                         // -> module_library { paths, texts, files() }: what every SGL compile `use`s, read now,
                                            //    directory by directory as added, each sorted by name; files() views the two vectors
// an SGL package adds its own module dirs; every SGL compile, compile_source and compile_hit_group included, sees all of them.
// a module file a compile reached is a dependency like an include: editing it reloads the shader.
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
asset->acquire(ctx, options)        // an SGL source's options set: cc::span<slib::shader_option const>, {name, value}
                                    //   value spelled as SGL spells it (`16`, `true`, `.rgba16_float`); slib::option_of(name, v)
                                    //   writes one from a bool, an int or an sg::pixel_format
                                    //   keyed per format AND per set of the options the entry point reaches (asset->options()):
                                    //   one it does not reach is dropped, so it never splits a compile; a reload recompiles each set
                                    //   a default left out and one given are two entries of one text, which the compiler's cache shares
asset->generation()                 // -> u64; moves when a reload replaced the shader. Cache it.
asset->last_error()                 // -> optional<string>; why the last reload was rejected
asset->virtual_path() / stage() / entry_point()
asset->dependencies()               // -> vector<string>; source, resolved includes, module files reached (what is watched)
// GOTCHA: acquire returns a COLD cc::async node. The ambient scheduler's workers run it, or block on it with
//   cc::try_async_blocking_get(sh). Lazy + per format: nothing compiles until asked, and each format is
//   compiled separately from the same source.
```

## the compiler seam

```cpp
#include <shaped-shader-library/compiler/shader_compiler.hh>
slib::shader_language              // hlsl | wgsl | sgl | metal   (slang/glsl planned)
slib::include_resolver             // cc::function_ref<cc::optional<cc::string>(cc::string_view path)>
slib::shader_source_description    // { cc::string source; cc::string entry_point; sg::shader_stage stage; cc::string label; options }
                                   //   label = what a diagnostic calls the source; never opened, may be empty
                                   //   options = an SGL source's option values, which its preprocess writes the text with
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
slib::create_dxc_compiler(slib::sgl_dxc_options())  // the one behind an SGL edge: -enable-16bit-types, for float16_t

#include <shaped-shader-library/compiler/wgsl_compiler.hh>  // every platform, WebAssembly included
slib::create_wgsl_compiler()       // -> std::unique_ptr<shader_compiler>; wgsl -> wgsl, the source IS the bytecode
                                   //   reflection only: a stage or entry point other than the package's is an async error

#include <shaped-shader-library/compiler/metal_compiler.hh>  // Apple only: SLIB_HAS_METAL says whether it is there
slib::create_metal_compiler(language_version = {})
                                   // -> std::unique_ptr<shader_compiler>; metal -> metal_lib
                                   //   language_version ("metal3.2") reaches a metallib's -std=; source ignores it
                                   //   add_available_compilers gives the SGL edge "metal3.2", for coherent(device) and texture atomics
                                   //   the artifact is a metallib, or MSL source where Apple's Metal toolchain is not
                                   //   installed — target_format() is metal_lib either way, and the shader says which
                                   //   compiles through an ssc::msl::shader_cache: async, in memory and in the blob cache
                                   //   preprocess hands the text back: MSL here has no #include to flatten
                                   //   reflection reads the SOURCE (see shaped-shader-compiler-msl/cheat-sheet.md)

#include <shaped-shader-library/compiler/sgl_compiler.hh>   // wherever the inner compiler exists
slib::create_sgl_compiler(std::unique_ptr<shader_compiler> inner)
                                   // -> std::unique_ptr<shader_compiler>; sgl -> inner->target_format()
                                   //   dxil -> HLSL for dx12, spirv -> HLSL for vulkan, wgsl -> WGSL, metal_lib -> MSL
                                   //   preprocess IS SGL's pipeline, so the flattened source is the EMITTED TEXT;
                                   //   compile and reflection are the inner compiler's
                                   //   an SGL error is a preprocess error: `pkg/cube.sgl:12:5: error: unknown-name: foo`
                                   //   the binding pass runs behind it: the HLSL names each group, the pass writes registers
lib.add_compiler(slib::create_sgl_compiler(slib::create_wgsl_compiler()));   // one edge per format you can build
lib.add_compiler(slib::create_sgl_compiler(slib::create_metal_compiler()));  // ... and this is how a package reaches metal

#include <shaped-shader-library/compiler/available_compilers.hh>
slib::add_available_compilers(lib) // every edge this build can make: wgsl, dxil + spirv where DXC is, metal_lib on Apple,
                                   //   and SGL over each; what a library serving any backend registers instead of the list above
                                   //   a DXC that fails to create is left out, with a warning naming why; lib.supported_formats(language) says what is left

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
// `#pragma sc format <sg::pixel_format>` before an RWTexture* states its image_format, and on SPIR-V
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
// a texture or image member is a typed view too, sg's typedef for its shape (`texture_view_2d`, `texture_view_cube`, `image_view_2d_array<F>`…):
//   `albedo: texture_2d[float4]`        -> sg::texture_view_2d albedo
//   `dst: out image_2d[.rgba8_unorm]`   -> sg::image_view_2d<sg::pixel_format::rgba8_unorm> dst   (any access: read, out, mut)
//   `user_smp: sampler`                -> sg::sampler user_smp, which gather() hands sg by its host name (`work.user_smp`)
//   `sampler albedo_smp:` block        -> NO field: an sg::named_sampler in declared_samplers(), which the layout carries
// `@inline binding constants` -> shaders::constants: the block byte for byte, and to_block() its own bytes:
pass.set_inline_constants(shaders::constants{.view_projection = vp}.to_block());
// a struct a binding places in GPU memory -> shaders::particle: tg members at SGL's offsets, padding as `cc::u32 _padN = {}`,
//   and static_asserts on sizeof and every offsetof. A buffer of it is sg::readwrite_buffer_view<shaders::particle>:
auto const items = ctx.persistent.create_buffer_from_data(cc::vector<shaders::particle>{...}, sg::buffer_usage::readwrite_buffer);
//   a constant block packs as an HLSL cbuffer, a buffer element tight like a tg struct (the SGL spec's layout rules).
// SGL `bool32` -> slib::gpu_bool (gpu_bool.hh): a bool as one 32-bit lane; a plain bool assigns into it.
// SGL `half` -> tg::f16, `short`/`ushort` -> cc::i16/cc::u16, their vectors tg::vec<N, T>; a 2-byte gap is `cc::u16 _padN`.
// every name lives in the package namespace, so two files declaring one name is a generator error.
// a module's types live in NS::<module> instead, reached as ::sgl_modules::<module>::<name> (SGL modules, above).
// sg sees an SGL binding by its path, `work.values`, and a group's constant block by the binding's name:
//   the identifier the target text spells it with stays on each binding as `reflected_name`, for diagnostics.
// `@inline binding constants` also gives constants::inline_binding(): the pipeline layout's inline block, no reflection.
// an entry point of a `*`-declared file is a small wrapper: `->acquire(ctx)` as before, plus the layout its list states:
auto const layout = shaders::cube.main_vs.acquire_layout(ctx);                  // {constants}, nothing reflected
//   it also holds the file-scope samplers the entry point reaches, as sg::bound_samplers: s<i> of
//   slib::bound_samplers_space (10) on dx12, binding i + 1 of sg's reserved group elsewhere, a `pipeline`'s the same.
auto const pipeline = co_await shaders::double_values.main.acquire_pipeline(ctx); // compute: needs nothing else
// an entry point that reaches `@option const`s gets <file>_<entry>_t::options: one field per option, at the source's default
auto const tuned = co_await shaders::taa.reproject.acquire_pipeline(ctx, {.tile = 16, .lowres = true}); // a compile per set
shaders::taa.reproject.acquire(ctx, {.tile = 16})  // the shader alone; .values() is the struct as an acquire's span takes it
//   an option of a type the generator has no C++ for (a program's own enum) is a generator error;
// an image whose format names an option is an sg::any_texture_view field, and the group's layout is per set of values:
auto const values = shaders::upscale_main_t::options{.output_format = sg::pixel_format::rgba16_float};
auto const upscale = co_await shaders::upscale.main.acquire_pipeline(ctx, values);   // its layout follows the values
auto const group_layout = ctx.cached.acquire_binding_group_layout(shaders::outputs::declared_bindings({.output_format = f}),
                                                                  shaders::outputs::declared_samplers());
//   a binding array whose length names an option, and a `pipeline` over such a group, have no generated type yet
// a raster pipeline whose stages list different groups takes their union instead: acquire_pipeline_layout<frame, work>().
// a wrapper's layout holds only the samplers ITS entry point reaches, so a file used as a library never fills the sampler slots:
//   a raster pipeline whose stages reach file samplers is built from the file's `pipeline`, whose layout holds every stage's.
// a `pipeline` declaration -> shaders::<file>.<name> (an unnamed `pipeline:` is `.pipeline`), built from ITS stages,
//   layout, vertex input, targets and settings; the host states only what the declaration left `.host`.
//   It is an sg::raster_pipeline_source, so ctx.cached acquires it like a description:
auto const p = co_await ctx.cached.acquire_raster_pipeline(shaders::cube.pipeline, {.color = swapchain_format}); // open: one field per `.host` part
//   nothing `.host` -> acquire_raster_pipeline(shaders::cube.pipeline); a last argument customize(sg::raster_pipeline_description&) runs last
//   options its stages reach ride in the open parts: {.color = f, .options = {.tile = 16}}, every stage keying on its own
//   .description(ctx, parts)          the description itself, to build or inspect
//   .description_latest(ctx, parts)   the newest stages and settings even where the frozen part moved; acquire it yourself
//   an open field left unset (a format still `undefined`, a sample count still 0) asserts: the declaration said the host would state it
//   the build's settings are generated field writes (slib::impl::fields, from impl/pipeline_fields.hh, which `sgl pipeline-fields` writes)
//   hot reload: cull, depth, blend… follow the source; a moved frozen part (layout, vertex input, targets, features, formats, samples,
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
// an SGL shader's compiled_shader is SGL's own statement of it: bindings, workgroup, targets, features, footprint.
//   The inner compiler adds only bytecode; its reflection is compared on every compile, and a mismatch logs an error.
```

### an SGL ray-tracing pipeline  (docs/raytracing-pipelines.md)

```cpp
// package entry: `rt.sgl:*`, or `rt.sgl:raytracing_pipeline:path` (its binding list's groups must be generated too)
// `@raytracing pipeline path` of rt.sgl -> type shaders::rt_path_t, instance shaders::rt.path
using path_t = shaders::rt_path_t;
path_t::ray_count                              // constexpr int — ray types of its `rays` set; ALSO build_blas's hit_record_stride
path_t::hit_groups_t::textured                 // constexpr path_t::hit_group {int index} — typed per pipeline, for add_row
path_t::first_host_hit_group                   // only when `hit_groups = (…, .host)`: the host's first; the next is {.index + 1}
path_t::first_host_callable                    // only when a callables list ends in `.host`: the index a shader calls it by
auto const desc = co_await shaders::rt.path.description(ctx);         // shared_async<raytracing_pipeline_description>, cold
auto const pipeline = co_await ctx.cached.acquire_raytracing_pipeline(desc);
auto table_desc = path_t::table_description(pipeline);                // raygen, a miss per ray type, every callable
auto const row = path_t::add_row(table_desc, path_t::hit_groups_t::textured);  // -> sg::hit_row, ray_count records
auto const table = ctx.uncached.create_raytracing_shader_table(table_desc);
cmd.raytracing.build_blas(tris, sg::accel_build_flag::fast_trace, path_t::ray_count);  // metal bakes the stride
// tlas_instance{.hit_group_offset = table->offset_of(row)}; dispatch_rays(*table, sg::raygen_index(0), w, h)
path_t::definition()                           // slib::raytracing_pipeline_definition — what everything above reads

// registration order (every index a constant): raygen 0 | miss r = ray type r | hit g*ray_count + r (listed groups,
//   then the host's) | callables: the module's in declaration order, then the host's
// geometry i of an instance on ray type r reaches record offset_of(row) + i*ray_count + r, on every backend

#include <shaped-shader-library/raytracing_pipeline.hh>
// what the declaration leaves `.host`, compiled from SGL at run time, handed over in slib::raytracing_host_parts:
auto hits = co_await slib::compile_hit_group(&ctx, &library, &open_t::definition(), source, "material", "label.sgl");
//   -> vector<sg::hit_shader>, ray_count of them; the group must be for the pipeline's ray set: its name, every ray
//   type's name, every payload's name, size and shape
//   a bad source / missing group / other ray set = an ASYNC error carrying the exception's message, not a throw at the call
auto sq = co_await slib::compile_callable(&ctx, &library, &open_t::definition(), source, "squared", "label.sgl");
//   -> sg::compiled_shader; it must take the parameter of the pipeline's `.host` callables, by name and shape
auto host = slib::raytracing_host_parts{.hit_groups = hits, .callables = {sq}};
//   a pipeline whose shaders reach options: .options = path_t::options{.bounces = 2}.values(), handed to every shader
auto desc2 = co_await shaders::rt.open_path.description(ctx, host);
open_t::table_description(pipeline, host);     // the same host parts: a record for each of the host's callables
open_t::add_row(table_desc, open_t::first_host_hit_group);
// a part the declaration left closed must stay empty in `host` (asserts)
// metal: a procedural record's intersection + any hit become ONE fused traversal function, and an empty closest-hit
//   slot gets sgl_empty_closest_hit — description() and compile_hit_group() both do it; nothing changes for the host
// hot reload: a shader body follows; a moved frozen part (ray set, payload sizes, records, sizes, layout, samplers,
//   features) keeps the module's shaders last built on that ctx and logs why; slib::frozen_moved_of(def) says what moved
// refused at generation: a ray type without a miss; a binding list naming a group that was not generated
// webgpu has no pipeline: gate on ctx.supports(sg::feature::raytracing_pipeline)
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
                          //   span<embedded_file const> embedded_files; span<shader_definition const> definitions;
                          //   span<module_dir const> module_dirs; }
                          //   source_dir is absolute + baked at configure; MAY NOT EXIST (a shipped build)
slib::module_dir          // { string_view path; string_view source_dir; }  an SGL package's module directory:
                          //   path "" is its own source dir; a MODULE_DIRS one stands at ".modules/<i>" of the mount
```

## gotchas

```
- ONE shader_library at a time, and a package added ONCE: the generated symbols are process-wide globals.
- add every package BEFORE start_hot_reload (asserts): the watcher walks the asset list from its thread.
- an asset only WEAKLY references its library, because a generated global (a static) outlives it.
  Acquiring through a stale global reports an error rather than dangling.
- a generated package header is PRIVATE to its target. To publish a shader, re-expose it from your own
  public header and own the drift (docs/coding-guidelines.md). A module's <sgl_modules/m.hh> is the exception: PUBLIC.
- a reload only recompiles formats someone has already acquired.
- watch() is a hint to rescan, NOT a report. If you find yourself plumbing changed *paths* through it,
  stop — revision() is the source of truth and that is what makes overflow/rename/coalescing all free.
- a shader is only watched once a compile has recorded what it is built from, i.e. after its first
  acquire(). Nothing is watching a shader nobody ever asked for.
```
