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
struct line_column;

struct text_request;
struct all_text_request;
struct entry_text;
struct tested_source;
struct emitted_source;
struct interface_binding;
struct prelude_file;
struct library_file;
struct parsed_prelude_file;

enum class token_class : u8;
struct classified_span;
struct classify_options;
struct unannotated_binding;
struct inferred_result;

enum class described_member_kind : u8;
struct described_sampler;
struct described_file_sampler;
struct described_binding_member;
struct described_binding;
struct described_struct_member;
struct described_struct;
struct described_memory_member;
struct described_memory_struct;
struct described_entry_point;
struct described_pipeline_setting;
struct described_pipeline;
struct described_ray_set;
struct described_hit_group;
struct described_raytracing_pipeline;
struct described_callables;
struct described_option;
struct module_description;
struct describe_request;

enum class line_kind : u8;
struct line;

enum class token_kind : u8;
struct token;

enum class group_kind : u8;
struct group;

enum class form_kind : u8;
struct form;

struct parsed_file;

enum class builtin_id : i32;
enum class builtin_type_id : i32;

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
struct setting;

enum class literal_kind : u8;
enum class call_spelling : u8;
struct invalid_expr;
struct literal;
struct name;
struct self_ref;
struct void_ref;
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
struct if_expr;
struct loop_expr;
struct return_expr;
struct yield_expr;
struct break_expr;
struct continue_expr;
struct discard_expr;
struct struct_type;
struct function_type;
struct with_bindings;
struct qualified_type;
enum class type_access : u8;
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
struct require_decl;
struct fun_decl;
struct struct_decl;
struct enum_decl;
struct type_decl;
struct const_decl;
struct binding_decl;
struct sampler_decl;
struct pipeline_decl;
struct notation_decl;
struct test_decl;
struct field_decl;
struct property_decl;
struct enum_case_decl;
struct decl;

struct file_ast;
} // namespace sgl::ast

namespace sgl::builtins
{
enum class language : u8;
enum class precedence : u8;
struct written;
struct call_context;
struct helper_context;
enum class spelling_kind : u8;
enum class judged_operand : u8;
struct spelling;
struct block_layout;
struct type_record;
struct function_record;
struct registry_item;
struct registry;
} // namespace sgl::builtins

namespace sgl::check
{
enum class type_id : i32;
enum class symbol_id : i32;
enum class flat_expr_id : i32;
enum class flat_stmt_id : i32;
enum class local_id : i32;
enum class label_id : i32;

enum class type_kind : u8;
enum class texture_shape : u8;
enum class access_mode : u8;
enum class feature : u8;
struct sampler_state;
struct shape_info;
struct image_format_info;
struct type_info;
struct member_info;
struct enum_case_info;

enum class stage : u8;
enum class stage_input : u8;
enum class tessellation_partitioning : u8;
enum class tessellation_factor : u8;
struct interpolation;
enum class pixel_output : u8;
struct flat_stage_input;
struct stage_input_info;
enum class symbol_kind : u8;
enum class symbol_state : u8;
enum class function_role : u8;
struct symbol;
enum class constant_kind : u8;
struct constant_info;
struct option_use;
struct option_value;
struct parameter;
struct function_info;
enum class block_layout : u8;
struct binding_info;
enum class setting_kind : u8;
enum class setting_source : u8;
struct pipeline_setting;
enum class pipeline_kind : u8;
struct pipeline_info;
enum class target_kind : u8;
struct target;
struct swizzle;
struct written_argument;
struct ray_trace;
struct callable_call;
struct texel_store;
struct flat_traced_ray;
struct call_record;
enum class miss_reason : u8;
struct near_miss;
struct file_tables;

struct origin;
struct call_site;
struct name_mint;
enum class local_kind : u8;
struct flat_local;
struct flat_label;
struct flat_invalid;
struct flat_literal;
struct flat_int_literal;
struct flat_bool_literal;
struct flat_enum_value;
struct flat_arm;
struct flat_case;
struct flat_switch;
struct flat_local_ref;
struct flat_binding_member;
struct flat_file_sampler;
struct flat_member;
struct flat_buffer_element;
struct flat_element;
struct flat_construct;
struct flat_call;
struct flat_not;
struct flat_and;
struct flat_or;
struct flat_block;
struct flat_by_target;
struct flat_expr;
struct flat_let;
struct flat_var;
struct flat_assign;
struct flat_print;
struct flat_eval;
struct flat_if;
struct flat_leave;
struct flat_loop;
struct flat_while;
struct flat_for;
struct flat_continue;
struct flat_discard;
struct flat_once;
struct flat_break;
struct flat_return;
enum class check_node_kind : u8;
struct flat_check_node;
struct flat_check_site;
struct flat_check;
struct flat_stmt;
struct flat_entry_point;
enum class slot_view : u8; // which kind of view a footprint slot is bound through (check/footprint.hh)
struct slot_footprint;     // how an entry point's code touches one slot of a binding
struct flat_builder;

struct core_violation;

enum class value_kind : u8;
struct scalar;
struct value;
struct buffer_contents;
enum class run_status : u8;
struct run_inputs;
struct member_data;
struct bound_group;
struct driver_bindings;
struct run_limits;
struct check_failure;
struct site_tally;
struct outcome;

struct legalize_options;

enum class expectation_kind : u8;
struct test_expectation;
struct test_info;
struct related_note;
struct located_diagnostic;
struct checked_module;
struct module_file;
struct checked_prelude;
} // namespace sgl::check

namespace sgl::test
{
enum class test_status : u8;
struct narrowed_part;
struct check_report;
struct site_mark;
struct test_result;
struct test_options;
} // namespace sgl::test

namespace sgl::emit
{
enum class target : u8;
enum class error_kind : u8;
struct error;
struct emitted_text;
struct bound_name;
struct emitted_field;
struct emitted_layout;
} // namespace sgl::emit
