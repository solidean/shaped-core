#pragma once

#include <clean-core/error/result.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics-language/check/symbols.hh>
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

/// The text of one entry point, and the name that text declares it under.
struct sgl::emitted_source
{
    cc::string text;
    /// The source's name, unless the target reserves it: HLSL and MSL both reserve words an SGL author may pick.
    /// A caller compiling the text asks for THIS name.
    cc::string entry_point;
    /// What each resource the text declares is called there, and what the host calls it: `work.values`.
    /// A caller renames the compiler's reflected bindings with it, so no target's identifier rules reach the host.
    cc::vector<emit::bound_name> bound_names;
    /// A pixel entry point's render targets: how many, and the `@pixel struct` it returns; -1 and empty otherwise.
    i32 color_targets = -1;
    cc::string target_struct;
    /// What a device needs to run the entry point, each named as `sg::feature` names it; empty is portable.
    check::feature_set features;
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
