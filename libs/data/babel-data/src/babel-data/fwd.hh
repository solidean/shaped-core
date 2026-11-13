#pragma once

#include <clean-core/fwd.hh>
#include <clean-core/record/domain_fwd.hh>

/// Aggregate forward declarations for babel-data — the base64, JSON, markdown and shader-binary half of babel.
/// It also declares `namespace babel` itself: the vocabulary aliases every babel header writes bare, and the
/// fallback recording domain a site falls back to when its format declares none of its own.
/// babel-serializer's own fwd.hh includes this one and adds the formats above it.
///
/// Each format lives in its own sub-namespace — babel::base64, babel::json, babel::markdown — and owns its own
/// header; include that header directly when it is all you need.

namespace babel
{
// Pull in the shaped-core vocabulary types (i32, u8, isize, ...) so we write them bare inside babel
// without leaking them into the global namespace.
using namespace cc::primitive_defines;

/// The domain a babel recording site falls back to when its format declares none of its own.
CC_REC_DECLARE_DOMAIN(g_rec_domain);
} // namespace babel

namespace babel::json
{
enum class node_kind : u8;
struct node;
class document;
struct ref;

enum class non_finite_policy : u8;
enum class large_integer_policy : u8;
enum class layout : u8;
struct write_report;
struct write_options;
class writer;
struct object_writer;
struct array_writer;
class string_writer;

/// The domain every recording site in babel::json is attributed to.
CC_REC_DECLARE_DOMAIN(g_rec_domain);
} // namespace babel::json

namespace babel::spirv
{
struct header;
struct opcode_info;
struct instruction;
class module;

/// The domain every recording site in babel::spirv is attributed to.
CC_REC_DECLARE_DOMAIN(g_rec_domain);
} // namespace babel::spirv

namespace babel::llvm
{
struct record;
struct block_entry;
struct block;
struct bitstream;
} // namespace babel::llvm

namespace babel::llvm37
{
enum class type_kind : u8;
struct type;
enum class inst_code : u16;
enum class value_kind : u8;
struct value;
struct constant;
struct global_variable;
struct function;
struct instruction;
enum class operand_role : u8;
struct body;
struct module;
} // namespace babel::llvm37

namespace babel::dxil
{
struct part;
struct program;
struct container;
} // namespace babel::dxil

namespace babel::markdown
{
enum class node_kind : u8;
struct node;
class document;
struct ref;

/// The domain every recording site in babel::markdown is attributed to.
CC_REC_DECLARE_DOMAIN(g_rec_domain);
} // namespace babel::markdown
