#pragma once

#include "fwd.hh"
#include "protocol/documents.hh"

#include <clean-core/container/map.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/memory/shared_ptr.hh>
#include <clean-core/thread/async.hh>
#include <shaped-graphics-language/check/check.hh>

/// The prelude's files as the library carries them, parsed once for the whole process.
struct sgl_lsp::prelude
{
    cc::vector<cc::string> names;
    cc::vector<sgl::parsed_file> files;
    cc::vector<sgl::ast::file_ast> asts;
    cc::vector<lsp::text_index> indices;

    /// The `sgl-prelude:` uri a note into prelude file `i` names, which the client asks `sgl/preludeText` for.
    [[nodiscard]] cc::string uri_of(isize i) const { return cc::string("sgl-prelude:/") + names[i]; }
};

namespace sgl_lsp
{
/// Built on first use, safe from any thread.
[[nodiscard]] prelude const& the_prelude();
} // namespace sgl_lsp

/// What every feature reads: one document checked as one unnamed module behind the prelude.
/// A document that is a file of the prelude is checked in that file's place instead.
///
/// Today a unit is one file, since every file is its own unnamed module.
/// Once modules exist, an analysis covers every file of one, and a feature finds its file with `file_of`.
struct sgl_lsp::analysis
{
    cc::shared_ptr<lsp::document> document;
    sgl::parsed_file file;
    sgl::ast::file_ast ast;
    /// The file that stands behind the prelude when the document is one of the prelude's own, empty then.
    sgl::parsed_file empty_file;
    sgl::ast::file_ast empty_ast;
    /// The document's position among the module's files: the last, or its place in the prelude (`sgl::prelude_file_of`).
    i32 own_file = 0;
    sgl::check::checked_module module;
    /// Every phase's diagnostics, the ones a test's `@expect` declared taken out.
    cc::vector<sgl::check::located_diagnostic> diagnostics;

    /// The position of the document among the files the module was checked from.
    [[nodiscard]] i32 user_file() const { return own_file; }

    /// The files the module was checked from, in order, prelude first, the document in its own place.
    [[nodiscard]] cc::vector<sgl::check::module_file> module_files() const;

    /// The text of file `i`, the prelude's or the document's.
    [[nodiscard]] cc::string_view text_of(i32 file) const;
    [[nodiscard]] lsp::text_index const& index_of(i32 file) const;
    /// The uri a location in file `i` names.
    [[nodiscard]] cc::string uri_of(i32 file) const;
};

namespace sgl_lsp
{
/// Parses and checks `document`; runs wherever it is called, which is compute.
[[nodiscard]] cc::shared_ptr<analysis> analyze(cc::shared_ptr<lsp::document> document);
} // namespace sgl_lsp

/// One analysis per open document and version, shared by every request that asks for it.
/// Touched on the thread driving the server only.
class sgl_lsp::analysis_cache
{
public:
    using handle = cc::shared_async<cc::shared_ptr<analysis>>;

    /// The analysis of `uri` as `snap` has it; an empty handle when the document is not open there.
    /// A cold handle: whoever awaits it starts it, and a second caller for the same version shares the first's.
    [[nodiscard]] handle of(lsp::snapshot const& snap, cc::string_view uri);

    void forget(cc::string_view uri) { _entries.erase(uri); }

private:
    struct entry
    {
        i32 version = -1;
        handle analysis;
    };
    cc::map<cc::string, entry> _entries;
};
