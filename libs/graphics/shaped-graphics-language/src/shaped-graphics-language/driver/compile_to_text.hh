#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics-language/check/features.hh>
#include <shaped-graphics-language/check/footprint.hh>
#include <shaped-graphics-language/check/symbols.hh>
#include <shaped-graphics-language/driver/describe.hh>
#include <shaped-graphics-language/emit/emit.hh>
#include <shaped-graphics-language/legalize/legalize.hh>

/// One entry point of one SGL source, asked for as the text of one target.
struct sgl::text_request
{
    cc::string_view source;
    /// What a diagnostic calls the source; it is never opened.
    cc::string_view source_name = "<sgl>";
    /// The files whose modules the source may `use`.
    cc::span<library_file const> library;
    /// The entry point's name as the source writes it.
    cc::string_view entry_point;
    /// The stage the caller expects the entry point to be; `none` takes whichever it is.
    check::stage stage = check::stage::none;
    emit::target target = emit::target::hlsl_dx12;
    /// The values this compile gives the source's options, by name (CHK-354); an option left out keeps its default.
    /// A used module's option is named `module.name`.
    /// A name no option has, a value of another type than the option's, and a name given twice are `invalid-option`.
    cc::span<check::option_value const> options;
    /// Runs the source's own tests after it checked, and makes a test that does not pass an error like any other.
    /// The tests run with every option at its default, whatever `options` says.
    bool run_tests = false;
    /// Writes `#line` into HLSL and MSL, so their compilers and debuggers name the SGL line (EMIT-182).
    /// The line table is written either way.
    bool line_directives = false;
    /// Writes every print and every failed assert into sg's debug buffer, and describes each site (EMIT-183).
    /// Without it the text holds neither.
    bool diagnostics = false;
    /// What the text does where an integer division by zero or an array index outside the array would run (LEGAL-65).
    /// `report` prints through the debug buffer, and so is `abort` without `diagnostics`.
    check::undefined_guard guards_undefined = check::undefined_guard::none;
    /// Writes an inline trace as its emulated form on HLSL too, reading sg's acceleration pool (EMIT-135).
    /// A package's `SOFTWARE_TRACE` asks for it, so an inline trace sees what moves wherever it runs; WGSL has no other form.
    bool software_trace = false;
    /// Compiles an entry point whose `@float` WGSL cannot keep at `fast`, with a warning, rather than refusing it (EMIT-185).
    /// The host's to give, since only the code that picked WebGPU knows whether running without the guarantee will do.
    bool weakens_float = false;
    /// What the device has, each named as `sg::feature` names it, which `has` answers once inlined (CHK-426).
    /// With `float_atomic_add` an `atomic[float]` is the target's own float atomic where the target can say one,
    /// vulkan and metal, and a `uint`'s bits elsewhere.
    check::feature_set device_features;
    /// `hlsl-vulkan` written as SPIR-V by SGL itself, which `emitted_source::spirv` then holds.
    emit::native_route native = emit::native_route::text;
    /// Writes the `.sgl` line of each operation into what the native route writes, for a debugger to read (EMIT-257).
    /// Off, the binary is byte for byte what it would be without this option; the text route has `line_directives`.
    bool native_line_table = false;
    /// A mesh or geometry entry point as one half of its polyfill (CHK-524, CHK-525), which
    /// `emitted_source::mesh_layout` then describes.
    emit::mesh_form mesh = emit::mesh_form::native;
    /// The vertex stage a geometry or tessellation control entry point's compute half runs (CHK-525, CHK-527): its
    /// pipeline's.
    cc::string_view polyfill_vertex;
    /// The tessellation control stage before a tessellation evaluation entry point whose compute half is asked for
    /// (CHK-527): its pipeline's.
    cc::string_view polyfill_control;
    /// The task stage that launches a mesh entry point whose compute half or scan is asked for (CHK-524): its
    /// pipeline's, where it has one.
    cc::string_view polyfill_task;
};

/// Every entry point of one SGL source, asked for as the text of each of `targets`.
struct sgl::all_text_request
{
    cc::string_view source;
    /// What a diagnostic calls the source; it is never opened.
    cc::string_view source_name = "<sgl>";
    /// The files whose modules the source may `use`.
    cc::span<library_file const> library;
    cc::span<emit::target const> targets;
};

/// One slot the text declares for the host to bind: a block of constants or a resource, with every fact its API needs.
/// Enums are spelled as the sg enum value they are, as `sgl describe` spells them; empty where a fact does not apply.
struct sgl::interface_binding
{
    /// What the host binds it by: `work.values` for a resource, the binding's own name for a block of constants.
    cc::string name;
    /// As the text spells it, which is what the target's compiler reflects.
    cc::string emitted;
    /// `constant` for a block of constants.
    described_member_kind kind = described_member_kind::constant;
    /// The `@inline` block, which takes no group and rides as inline constants.
    bool is_inline = false;
    /// A file-scope sampler, a static sampler of the pipeline layout that no group holds: `slot` is its index there.
    bool is_file_sampler = false;
    /// The group it is listed at, counted without the `@inline` binding, and its slot there.
    /// -1 and 0 for the `@inline` block, and -1 and its index for a file-scope sampler.
    i32 group = -1;
    i32 slot = 0;
    /// A binding array's length, taking `count` consecutive slots from `slot`; 1 for everything else.
    i32 count = 1;
    /// Whether the entry point's code reaches it; the text declares every slot of every binding it lists either way.
    bool is_used = false;
    /// A block's size in bytes, where its last constant ends; 0 for a resource.
    i32 block_size = 0;
    cc::string access;
    cc::string texture_dimension;
    cc::string sample_type;
    cc::string image_format;
    cc::string sampler_type;
};

/// The text of one entry point, and the name that text declares it under.
struct sgl::emitted_source
{
    cc::string text;
    /// The SPIR-V `text_request::native` asked for; empty where the text is what was written.
    cc::vector<u32> spirv;
    /// The DXIL container the same request asked for on dx12.
    cc::vector<byte> dxil;
    /// What kept a `prefer` from writing either, for a reader.
    cc::string native_gap;
    /// Where DXIL's packer places what crosses the entry point's edge, wherever a dx12 request took the native route.
    /// It stands beside `native_gap`, since a signature needs no lowering of the body.
    cc::vector<ir::dxil::signature_element> signature;
    check::stage stage = check::stage::none;
    /// The source's name, unless the target reserves it: HLSL and MSL both reserve words an SGL author may pick.
    /// A caller compiling the text asks for THIS name.
    cc::string entry_point;
    /// Every slot the text declares, in the order of the binding list and then of each binding's slots, and then the
    /// file-scope samplers its code reaches.
    /// This is the interface a host builds against; a compiler's reflection of the text can only confirm it.
    cc::vector<interface_binding> bindings;
    /// A compute entry point's grid; `{1, 1, 1}` for every other stage.
    i32 workgroup[3] = {1, 1, 1};
    /// A bare @compute's: the compile chose `workgroup`, so the host dispatches it by threads alone (CHK-520).
    bool is_dispatched_by_threads = false;
    /// The subgroup size a compute entry point prefers, which the host asks the device for; 0 where it prefers none.
    i32 preferred_subgroup_size = 0;
    /// A pixel entry point's render targets: how many, and the `@pixel struct` it returns; -1 and empty otherwise.
    i32 color_targets = -1;
    cc::string target_struct;
    /// What a device needs to run the entry point, each named as `sg::feature` names it; empty is portable.
    check::feature_set features;
    /// The float guarantee the text keeps (CHK-435, EMIT-185): the entry point's, or `fast` where it was weakened.
    check::float_level float_guarantee = check::float_level::fast;
    /// The function that raised the entry point's guarantee, by name; empty at `fast`.
    cc::string float_raised_by;
    /// LEGAL-69: the bytes of workgroup memory the profile could not shadow, whose reads it tests not; 0 where it did.
    i32 unshadowed_bytes = 0;
    /// The options the entry point reaches, by name, in declaration order (CHK-355).
    cc::vector<cc::string> options;
    /// What the entry point's code does to each binding it lists, keyed by host name; a slot it never touches is absent.
    cc::vector<check::slot_footprint> footprint;
    /// Every constant block and buffer element as the text declares it, which a compiler reflecting the text reports.
    cc::vector<emit::emitted_layout> layouts;
    /// Where each line of the function body came from, in text order (EMIT-181).
    cc::vector<emit::line_origin> lines;
    /// What each file a `line_origin` names is called, by its position.
    cc::vector<cc::string> line_files;
    /// Every print and assert the text writes into sg's debug buffer; empty without `text_request::diagnostics`.
    cc::vector<emit::debug_site> debug_sites;
    /// The slots a polyfilled mesh or geometry entry point's halves agree on (CHK-524, CHK-525); empty unless
    /// `text_request::mesh` asked.
    cc::optional<check::mesh_polyfill_layout> mesh_layout;
    /// The records a polyfilled task stage's forms agree on (CHK-524): the task entry point's compute form, and a mesh
    /// entry point's compute half and scan where `text_request::polyfill_task` named the task; empty otherwise.
    cc::optional<check::task_polyfill_layout> task_layout;
    /// What a polyfilled tessellation's forms agree on (CHK-527): the control stage's compute half, and the evaluation
    /// stage's compute and vertex halves; empty otherwise.
    cc::optional<check::tessellation_polyfill_layout> tessellation_layout;
    /// The names of the library files the source reached through `use`, which an edit of any of them changes the text of.
    cc::vector<cc::string> library_files;
    /// One entry per warning, `cube.sgl:12:5: warning: no-effect: ...` and its notes on the lines under it.
    /// Each is written as an error is, but with no line end after its last line.
    /// They are the source's and its library files', and those judged of this entry point's inlined body (CHK-402).
    cc::vector<cc::string> warnings;
};

/// One entry point on one target, as `compile_all_to_text` wrote it or said why it could not.
struct sgl::entry_text
{
    cc::string entry_point;
    emit::target target = emit::target::hlsl_dx12;
    cc::result<emitted_source, cc::string> text;
};

namespace sgl
{
/// The whole pipeline in one call: parse, build the AST, check against the embedded prelude, and emit one entry point.
///
/// Total: every failure is the error of the result, as text that is meant to be read.
/// A diagnostic is one line, `cube.sgl:12:5: error: unknown-name: foo`, and an error of any phase means there is no text.
/// An error judged of another entry point's inlined body is that one's alone, and fails only its compile (CHK-402).
/// An entry point the source does not hold, and one whose stage is not `request.stage`, are errors that name what the source does hold.
/// Warnings alone do not fail, and come back as the text's `warnings`; a failure's error holds the errors alone.
///
/// Deterministic, and it reads nothing but its arguments, so the text may be cached under the source and the request.
/// A cache that outlives the process keys on `compiler_fingerprint()` too.
[[nodiscard]] cc::result<emitted_source, cc::string> compile_to_text(text_request const& request);

/// A hash of this build's compiler sources and embedded prelude, generated by the build.
/// It changes with any edit to them, so a persistent cache keyed on it never serves another compiler's text.
[[nodiscard]] cc::string_view compiler_fingerprint();

/// Every entry point of one source on every target asked for, from one pass of the front end.
/// A call of `compile_to_text` per entry point and target checks the source, prelude included, that many times.
/// An error of the front end is the error of the result; an entry point one target cannot write is that element's error.
/// Ordered by entry point as the source declares them, then by target as asked.
[[nodiscard]] cc::result<cc::vector<entry_text>, cc::string> compile_all_to_text(all_text_request const& request);
} // namespace sgl
