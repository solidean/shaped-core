#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics-language/driver/library_file.hh>

namespace sgl_tool
{
struct module_library;
} // namespace sgl_tool

/// The `.sgl` files of a set of module directories, read, which a compile takes as its library.
/// `files` views `names` and `sources`, so the three move together and are never copied apart.
struct sgl_tool::module_library
{
    cc::vector<cc::string> names;
    cc::vector<cc::string> sources;
    cc::vector<sgl::library_file> files;
};

namespace sgl_tool
{
/// Every `.sgl` file directly in each of `dirs`, not in their subdirectories, named by its path as `dirs` spell it.
/// A directory that does not exist or a file that does not read is an error naming it.
[[nodiscard]] cc::result<module_library, cc::string> read_module_dirs(cc::span<cc::string const> dirs);
} // namespace sgl_tool
