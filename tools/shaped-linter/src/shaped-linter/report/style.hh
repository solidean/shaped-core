#pragma once

#include <shaped-linter/fwd.hh>

namespace scl
{
/// Everything the diagnostic renderer needs to know about presentation.
/// One struct for the whole `report/` layer, so a caller configures once and every layer below reads the same knobs.
///
/// `color` is passed in explicitly and never read from the process-global `cc::console` state.
/// That is what keeps the renderer a pure function, and its tests independent of how the binary was invoked.
struct report_style
{
    bool color = false;
    i32 context_lines = 2;  // source lines shown before and after a labelled line
    i32 tab_width = 4;      // a tab advances to the next multiple of this
    i32 max_span_lines = 6; // a span covering more lines has its middle elided
    i32 wrap_columns = 100; // where the rationale paragraphs are wrapped
};
} // namespace scl
