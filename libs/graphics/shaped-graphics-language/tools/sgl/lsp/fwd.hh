#pragma once

#include "protocol/fwd.hh"

#include <clean-core/fwd.hh>

/// The SGL half of the language server: everything that knows SGL, and translates what the library answers into LSP.
/// The library answers every question; nothing here re-derives what a token or a type is.

namespace sgl_lsp
{
using namespace cc::primitive_defines;

struct prelude;
struct analysis;
class analysis_cache;
struct test_run;
struct check_mark;
struct check_results_params;
class language_server;
} // namespace sgl_lsp
