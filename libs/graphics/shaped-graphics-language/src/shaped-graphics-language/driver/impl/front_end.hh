#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics-language/ast/file_ast.hh>
#include <shaped-graphics-language/check/checked_module.hh>
#include <shaped-graphics-language/driver/prelude.hh>
#include <shaped-graphics-language/syntax/parsed_file.hh>

namespace sgl::driver::impl
{
/// One source checked against the prelude, which is what every driver starts from.
///
/// `module` refers into `files` and `asts`, so the three move together and are never copied apart.
struct front_end
{
    cc::span<prelude_file const> prelude;
    cc::vector<parsed_file> files;
    cc::vector<ast::file_ast> asts;
    check::checked_module module;
    cc::string_view source_name;
    /// Every error of every phase, one formatted diagnostic per line; empty when there is none.
    /// Warnings are left out: a driver's result is either what it asked for or the reasons there is none.
    cc::string errors;

    /// The program's file: the prelude's files come first.
    [[nodiscard]] i32 program_file() const { return i32(prelude.size()); }

    /// What a diagnostic calls file `file`.
    [[nodiscard]] cc::string_view name_of(isize file) const
    {
        return file < prelude.size() ? prelude[file].name : source_name;
    }
};

/// Parses, builds and checks `source` behind the prelude.
/// A source with errors still yields a module, whose `errors` say why nothing should be read from it.
[[nodiscard]] front_end run_front_end(cc::string_view source, cc::string_view source_name);
} // namespace sgl::driver::impl
