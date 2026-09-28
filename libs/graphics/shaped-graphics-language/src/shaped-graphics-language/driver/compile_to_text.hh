#pragma once

#include <clean-core/error/result.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics-language/check/footprint.hh>
#include <shaped-graphics-language/check/symbols.hh>
#include <shaped-graphics-language/driver/describe.hh>
#include <shaped-graphics-language/emit/emit.hh>

/// One entry point of one SGL source, asked for as the text of one target.
struct sgl::text_request
{
    cc::string_view source;
    /// What a diagnostic calls the source; it is never opened.
    cc::string_view source_name = "<sgl>";
    /// The entry point's name as the source writes it.
    cc::string_view entry_point;
    /// The stage the caller expects the entry point to be; `none` takes whichever it is.
    check::stage stage = check::stage::none;
    emit::target target = emit::target::hlsl_dx12;
    /// Runs the source's own tests after it checked, and makes a test that does not pass an error like any other.
    bool run_tests = false;
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
    /// The group it is listed at, counted without the `@inline` binding, and its slot there; -1 and 0 for the `@inline` block.
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
    /// The source's name, unless the target reserves it: HLSL and MSL both reserve words an SGL author may pick.
    /// A caller compiling the text asks for THIS name.
    cc::string entry_point;
    /// Every slot the text declares, in the order of the binding list and then of each binding's slots.
    /// This is the interface a host builds against; a compiler's reflection of the text can only confirm it.
    cc::vector<interface_binding> bindings;
    /// A compute entry point's grid; `{1, 1, 1}` for every other stage.
    i32 workgroup[3] = {1, 1, 1};
    /// A pixel entry point's render targets: how many, and the `@pixel struct` it returns; -1 and empty otherwise.
    i32 color_targets = -1;
    cc::string target_struct;
    /// What a device needs to run the entry point, each named as `sg::feature` names it; empty is portable.
    check::feature_set features;
    /// What the entry point's code does to each binding it lists, keyed by host name; a slot it never touches is absent.
    cc::vector<check::slot_footprint> footprint;
    /// Every constant block and buffer element as the text declares it, which a compiler reflecting the text reports.
    cc::vector<emit::emitted_layout> layouts;
};

namespace sgl
{
/// The whole pipeline in one call: parse, build the AST, check against the embedded prelude, and emit one entry point.
///
/// Total: every failure is the error of the result, as text that is meant to be read.
/// A diagnostic is one line, `cube.sgl:12:5: error: unknown-name: foo`, and an error of any phase means there is no text.
/// An entry point the source does not hold, and one whose stage is not `request.stage`, are errors that name what the source does hold.
/// Warnings alone do not fail, and are dropped: a caller that wants them runs the phases itself.
///
/// Deterministic, and it reads nothing but its arguments, so the text may be cached under the source and the request.
[[nodiscard]] cc::result<emitted_source, cc::string> compile_to_text(text_request const& request);
} // namespace sgl
