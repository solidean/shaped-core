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

enum class line_id : i32;
enum class token_id : i32;
enum class group_id : i32;
enum class form_id : i32;

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

namespace sgl::ast
{
enum class expr_id : i32;
enum class stmt_id : i32;
enum class decl_id : i32;
enum class field_id : i32;
template <class T>
struct range_of;

struct attribute;
struct argument;
struct field;
enum class body_kind : u8;
struct body;
struct case_arm;
struct if_branch;

enum class literal_kind : u8;
enum class call_spelling : u8;
struct invalid_expr;
struct literal;
struct name;
struct self_ref;
struct wildcard;
struct leading_dot;
struct member;
struct index;
struct call;
struct tuple;
struct array;
struct object;
struct comparison_chain;
struct cast;
struct membership;
struct ascription;
struct range;
enum class lambda_spelling : u8;
struct lambda;
struct case_expr;
struct loop_expr;
struct return_expr;
struct yield_expr;
struct break_expr;
struct continue_expr;
struct struct_type;
struct function_type;
struct with_bindings;
struct expr;

struct invalid_stmt;
struct let_stmt;
struct assign_stmt;
struct if_stmt;
struct for_stmt;
struct while_stmt;
struct assert_stmt;
struct print_stmt;
struct decl_stmt;
struct expr_stmt;
struct stmt;

enum class receiver_kind : u8;
struct invalid_decl;
struct module_decl;
struct use_decl;
struct fun_decl;
struct struct_decl;
struct enum_decl;
struct type_decl;
struct const_decl;
struct binding_decl;
struct sampler_decl;
struct notation_decl;
struct field_decl;
struct property_decl;
struct enum_case_decl;
struct decl;

struct file_ast;
} // namespace sgl::ast
