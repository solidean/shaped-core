#pragma once

#include <clean-core/fwd.hh>
#include <clean-core/record/domain_fwd.hh>

/// Forward declarations for shaped-graphics-language: the SGL compiler, linter, formatter and language server.
///
/// Two kinds of diagnostics exist here and must not be confused.
/// `sgl::diagnostic` is what the *user's source* did wrong, and is data a caller receives.
/// The recording domain is what the *library* has to say about itself, like every other library's.

namespace sgl
{
using namespace cc::primitive_defines;

struct source_span;

enum class severity : u8;
enum class diagnostic_kind : u8;
struct diagnostic;

enum class line_kind : u8;
struct line;

enum class token_kind : u8;
struct token;

enum class group_kind : u8;
struct group;

enum class form_kind : u8;
struct form;

struct parsed_file;

/// The domain every recording site in sgl is attributed to.
CC_REC_DECLARE_DOMAIN(g_rec_domain);
} // namespace sgl
