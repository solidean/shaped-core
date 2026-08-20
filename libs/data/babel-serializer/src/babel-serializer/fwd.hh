#pragma once

#include <clean-core/fwd.hh>

/// Aggregate forward declarations for babel-serializer.
/// Each format lives in its own sub-namespace — babel::base64, babel::json, babel::markdown, babel::sqlite,
/// babel::obj, babel::gltf, babel::png, babel::jpg — plus the babel::image aggregator on top of the last two.
/// Each owns its own header; include that header directly when it is all you need.

namespace babel
{
// Pull in the shaped-core vocabulary types (i32, u8, isize, ...) so we write them bare inside babel
// without leaking them into the global namespace.
using namespace cc::primitive_defines;
} // namespace babel

namespace babel::chrome_trace
{
struct write_options;
} // namespace babel::chrome_trace

namespace babel::json
{
enum class node_kind : u8;
struct node;
class document;
struct ref;
} // namespace babel::json

namespace babel::markdown
{
enum class node_kind : u8;
struct node;
class document;
struct ref;
} // namespace babel::markdown

namespace babel::obj
{
struct corner;
struct face;
struct group;
struct data;
} // namespace babel::obj

namespace babel::gltf
{
// The index roles.
// A forward declaration must repeat the underlying type exactly, hence the `: int` here too.
enum class buffer_index : int;
enum class buffer_view_index : int;
enum class accessor_index : int;
enum class mesh_index : int;
enum class node_index : int;
enum class scene_index : int;
enum class material_index : int;
enum class texture_index : int;
enum class image_index : int;
enum class sampler_index : int;

enum class container : u8;
enum class issue_kind : u8;
enum class component_type : u16;
enum class accessor_type : u8;
enum class primitive_mode : u8;
enum class buffer_target : u16;
enum class alpha_mode : u8;
enum class filter : u16;
enum class wrap_mode : u16;

struct issue;
struct asset_info;
struct buffer;
struct buffer_view;
struct accessor;
struct attribute;
struct primitive;
struct mesh;
struct node;
struct scene;
struct texture_ref;
struct material;
struct texture;
struct image;
struct sampler;
struct accessor_view;
struct read_options;
struct data;
} // namespace babel::gltf

namespace babel::sqlite
{
enum class error_code : u8;
enum class column_kind : u8;
enum class journal_mode : u8;
struct error;
struct row;
struct blob_location;
class statement;
class blob_handle;
class transaction;
class database;
} // namespace babel::sqlite

namespace babel::png
{
enum class color_type : u8;
enum class interlace_method : u8;
enum class component : u8;
struct text_entry;
struct physical_dimensions;
struct data;
} // namespace babel::png

namespace babel::jpg
{
enum class subsampling : u8;
enum class density_unit : u8;
struct density;
struct data;
} // namespace babel::jpg

namespace babel::image
{
enum class format : u8;
enum class component : u8;
struct image;
} // namespace babel::image
