#pragma once

#include <clean-core/string/string_view.hh>
#include <shaped-graphics-language/fwd.hh>

/// One file of the library a compile may `use` modules of: every `.sgl` file of the module directories it was given.
/// The caller reads them; the compiler never opens a file.
struct sgl::library_file
{
    /// What a diagnostic calls the file.
    /// A library file named like the compile's own source (`sgl::is_same_path`) is that source, and is left out.
    cc::string_view name;
    cc::string_view source;
};
