#pragma once

#include <cstddef>
#include <cstdint>

namespace cc::primitive_defines
{

//
// Primitives
//

// Explicitly-sized primitive types.
// Use these wherever the range matters for correctness or memory layout.
// Plain "int" stays the default where it does not — loop counters and small counts, well below a few millions.

// signed integers
using i8 = int8_t;
using i16 = int16_t;
using i32 = int32_t;
using i64 = int64_t;

// unsigned integers
using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

// floating point
using f32 = float;
using f64 = double;

// generic bytes
using byte = std::byte;

// Signed size type, deliberately, and against std's convention.
// Size arithmetic subtracts, and "size - 1" on an empty container must go negative rather than wrap to a huge positive number.
// docs/coding-guidelines.md, section "Integer & Numeric Types", carries the full reasoning.
using isize = i64;

// pointer
using nullptr_t = std::nullptr_t;
} // namespace cc::primitive_defines

namespace cc
{

using namespace cc::primitive_defines;

//
// Memory
//

struct memory_resource;
template <class T>
struct allocation;

enum class node_class_index : u8;
enum class node_class_size : u64;
struct node_memory_resource;
struct node_allocator;
struct any_node_allocation;
template <class T>
struct node_allocation;
template <class T, class NodeTraits>
struct poly_node_allocation;
struct scoped_default_node_allocator; // scoped override of the default node allocator (memory/node_allocation.hh)
template <class T>
struct unique_ptr;
template <class T, class Traits>
struct shared_ptr;
struct shared_release; // what a shared block's traits do at refcount zero (memory/shared_ptr.hh)
struct fused_refcount;
template <class T>
struct default_shared_traits;
template <class T, class Traits>
struct weak_ptr;


//
// Strings
//

struct string;
struct string_view;
template <class... Args>
struct format_string;


//
// Views
//

template <class T>
struct span;
template <class T, isize N>
struct fixed_span;
template <class T>
struct strided_iterator;
template <class T>
struct strided_span;

/// Opt-in trait, std::ranges style: true for view types whose validity is independent of the view object's lifetime (span, string_view, ...).
/// Specialized to true at each view's own header.
/// Consumed e.g. by make_pinned_data to distinguish borrows from owners.
template <class T>
inline constexpr bool enable_borrowed_range = false;


//
// Container
//

template <class T, class ContainerT>
struct allocating_container;

template <class T>
struct pinned_data;

template <class T>
struct array;
template <class T>
struct unique_array;
template <class T, isize N>
struct fixed_array;

template <class T>
struct vector;
template <class T>
struct unique_vector;
template <class T, isize N>
struct fixed_vector;
template <class T, isize N>
struct small_vector;

// template <class T>
// struct devector;
// template <class T, isize N>
// struct fixed_devector;

// Default transparent hasher/equality for the node-chaining associative containers:
// default_hash (common/hash.hh) finalizes via cc::make_hash_finalized; default_equal (common/utility.hh)
// compares with operator==.
struct default_hash;
struct default_equal;
template <class K, class V, class Hash = default_hash, class KeyEqual = default_equal>
struct map;
template <class T, class Hash = default_hash, class KeyEqual = default_equal>
struct set;

template <class K, class V>
struct key_value_provider;
namespace impl
{
template <class K>
struct cc_key_hash;
}
template <class K, class V, class Hash = impl::cc_key_hash<K>>
struct in_memory_key_value_provider;
template <class K, class V>
struct key_value_cache;

class byte_stream_builder;

template <class T>
struct ringbuffer;

template <class... Ts>
struct tuple;

template <class... Ts>
struct variant;

template <class T, class U = T>
struct pair;

template <class IdxT>
struct disjoint_set;

struct bitset;
template <isize N>
struct fixed_bitset;

template <class RangeT>
struct sequence;


//
// Functions
//

template <class T>
struct function_ref;
template <class T>
struct unique_function;

// The small callable objects of common/utility.hh.
template <class... Fs>
struct overloaded;
struct void_function;
struct identify_function;
template <auto C>
struct constant_function;
template <unsigned I>
struct projection_function;


//
// Fallibility
//

struct nullopt_t;
template <class T>
struct optional;

struct result_exception;
struct any_error;
template <class E>
struct as_error_t;

template <class T, class E = any_error>
struct result;


//
// Concurrency
//

template <class T>
struct mutex;
template <class T>
class mutex_guard;

enum class threaded_actor_mode;
struct threaded_actor_base;
struct threaded_actor_impl_base;
template <class... MessageT>
struct threaded_actor;
template <class... MessageT>
struct threaded_actor_impl;


//
// Async / dataflow
//

struct async_error;
struct alignas(32) async_type_ops; // the type-erased node ops descriptor (thread/async_node.hh)
struct async_node_base;
namespace impl
{
struct async_node_traits;
}
struct async_scheduler;
struct async_worker_scope;
struct singlethreaded_scheduler;
struct async_thread_pool;
struct async_context_base;
template <class T, class E = async_error>
struct async_context;
template <class T, class E = async_error>
struct async;

/// The normal async handle: an 8 B intrusive cc::shared_ptr over one slab node (see thread/async.hh).
template <class T, class E = async_error>
using shared_async = shared_ptr<async<T, E>, impl::async_node_traits>;


//
// Hashing
//

struct hash128;


//
// Math
//

struct random; // the pseudo-random generator (defined in math/random.hh)

// 128-bit intermediates and the carry-out pairs of math/wide_arith.hh.
struct u128;
struct i128;
struct carrying_add_result;
struct borrowing_sub_result;


//
// Streams
//

enum class seek_dir : u8; // the flush direction (public authoring API; defined in streams/stream_flush.hh)

// The six non-owning stream views (concrete types over the shared engine; defined in streams/stream.hh).
struct read_stream;
struct write_stream;
struct read_write_stream;
struct seekable_read_stream;
struct seekable_write_stream;
struct seekable_read_write_stream;

// Their owning adapters:
class span_read_stream_adapter;
class span_write_stream_adapter;
class span_read_write_stream_adapter;
class file_read_stream_adapter;
class file_write_stream_adapter;
class file_read_write_stream_adapter;


//
// Utilities
//

template <class EnumT>
struct flags;

struct unit;

// The vocabulary of common/utility.hh.
// The character comparators of string/char_predicates.hh, and format's output sink.
struct equal_case_sensitive;
struct debug_string_config; // how to_debug_string renders (string/to_debug_string.hh)
struct equal_case_insensitive;
struct compare_ascii_case_sensitive;
struct compare_ascii_case_insensitive;
struct format_sink;

template <class T>
union storage_for;
struct sentinel;
struct offset_size;
struct start_end;

} // namespace cc
