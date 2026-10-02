#pragma once

#include "../module_dirs.hh"
#include "fwd.hh"
#include "protocol/documents.hh"

#include <clean-core/container/map.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/memory/shared_ptr.hh>
#include <clean-core/thread/async.hh>
#include <shaped-graphics-language/check/check.hh>
#include <shaped-graphics-language/driver/prelude.hh>

/// The prelude's files as the library carries them, parsed once for the whole process, with what the server adds.
struct sgl_lsp::prelude
{
    cc::vector<cc::string> names;
    cc::span<sgl::parsed_prelude_file const> files;
    cc::vector<lsp::text_index> indices;

    /// The `sgl-prelude:` uri a note into prelude file `i` names, which the client asks `sgl/preludeText` for.
    [[nodiscard]] cc::string uri_of(isize i) const { return cc::string("sgl-prelude:/") + names[i]; }
};

namespace sgl_lsp
{
/// Built on first use, safe from any thread.
[[nodiscard]] prelude const& the_prelude();

/// The module directories every document is checked against, absolute; the client states them once it initializes.
/// Safe from any thread.
void set_module_dirs(cc::vector<cc::string> dirs);
[[nodiscard]] cc::vector<cc::string> module_dirs();
} // namespace sgl_lsp

/// What every feature reads: one document checked behind the prelude and the modules it `use`s.
/// A document that is a file of the prelude is checked in that file's place instead, with no modules.
///
/// A feature reads the document's own file; the modules' files are there for what a diagnostic or a test names.
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
    /// Every file of the module directories as read for this analysis, and the ones the document reached parsed.
    /// `module.library_files` says which of `library.files` each checked file between the prelude and the document is.
    sgl_tool::module_library library;
    cc::vector<cc::unique_ptr<sgl::parsed_file>> library_parsed;
    cc::vector<cc::unique_ptr<sgl::ast::file_ast>> library_asts;
    /// Parallel to `module.library_files`.
    cc::vector<lsp::text_index> library_indices;
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

/// One analysis per open document as one change left it, shared by every request that asks for it.
/// Touched on the thread driving the server only.
class sgl_lsp::analysis_cache
{
public:
    using handle = cc::shared_async<cc::shared_ptr<analysis>>;

    /// The analysis of `uri` as `snap` has it; an empty handle when the document is not open there.
    /// A cold handle: whoever awaits it starts it, and a second caller for the same document shares the first's.
    [[nodiscard]] handle of(lsp::snapshot const& snap, cc::string_view uri);

    void forget(cc::string_view uri) { _entries.erase(uri); }

private:
    /// Keyed on the document itself, which every change makes anew: a version is the client's to repeat, as a reopen does.
    struct entry
    {
        cc::shared_ptr<lsp::document> document;
        handle analysis;
    };
    cc::map<cc::string, entry> _entries;
};
