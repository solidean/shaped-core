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
[[nodiscard]] cc::result<cc::string, cc::string> compile_to_text(text_request const& request);
} // namespace sgl
