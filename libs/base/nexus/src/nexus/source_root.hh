#pragma once

#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>

// Where a test, an example or a tool finds the files of the source tree it was built from: its data, corpora, specs.
//
// A binary never carries an absolute path of the checkout it was built in.
// CMake passes such a directory relative to the top-level source directory (`sc_target_source_paths`).
// The code joins it onto `nx::source_root()` at run time:
//
//     auto const corpus = nx::source_path(MY_CORPUS_DIR);       // MY_CORPUS_DIR is "libs/x/tests/corpus"
//
// A shipped library never asks: it has no source tree to ask about, which is why this lives in nexus.

namespace nx
{
/// The top-level source directory of the build this binary belongs to, absolute, with `/` separators and no trailing one.
///
/// Resolved once per process, in this order:
///  1. the environment variable `SC_SOURCE_ROOT`, which `dev.py` sets for every binary it runs;
///  2. the `CMAKE_HOME_DIRECTORY` of the first `CMakeCache.txt` found walking up from the executable's directory.
/// The walk holds for a build directory anywhere, on another drive included.
/// Neither finding one ends the process with a message naming both, since no answer would be a right one.
/// Thread-safe.
[[nodiscard]] cc::string_view source_root();

/// `relative`, a path below the top-level source directory, made absolute by joining it onto `source_root()`.
/// `relative` must not be absolute; separators may be either kind.
[[nodiscard]] cc::string source_path(cc::string_view relative);
} // namespace nx
