#pragma once

#include <cstddef>
#include <cstdint>

namespace cc::primitive_defines
{

//
// Primitives
//

// Explicitly-sized primitive types
// We encourage using these types wherever the range is important for correctness or memory layout.
// However, we happily use "int" as a default integer if the range doesn't matter much
// (e.g. well below a few millions, such as loop counters or small counts).

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

// signed size type (controversial but intentional)
// We use signed i64 for sizes and indices instead of size_t for several reasons:
// * Arithmetic with sizes often requires subtraction, which causes underflow bugs with unsigned
//   (e.g. "size - 1" underflows when size is 0, becoming a huge positive number)
// * Mixed signed/unsigned arithmetic is a major source of bugs and confusing implicit conversions
// * Negative values are useful for error returns, sentinel values, and relative offsets
// * We only target 64-bit platforms, so i64 provides plenty of range (2^63 - 1 > 9 quintillion)
// * As a greenfield standard library not interoperating with std:: most of the time,
//   we avoid the backwards compatibility friction that plagues existing C++ codebases
// * Modern practice (see Stroustrup's P1428R0) recognizes unsigned sizes as a historical mistake
// * Bounds checking happens at runtime anyway, so unsigned providing "extra range" is illusory
// * Mathematically: signed integers model a proper subset of the integers with correct arithmetic and ordering.
//   Unsigned integers model a modulo ring where operations wrap around and comparisons break
//   (a < b does NOT imply a + c < b + c). Overflow being UB for signed is good: it means we model
//   actual integers as long as we stay in bounds. Unsigned silently transitions to a different
//   algebraic structure outside bounds, which causes subtle bugs.
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
template <class T>
struct unique_ptr;
template <class T, class Traits>
struct shared_ptr;
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

/// Opt-in trait (std::ranges style): true for view types whose validity is independent
/// of the view object's lifetime (span, string_view, ...). Specialized to true at each
/// view's own header. Consumed e.g. by make_pinned_data to distinguish borrows from owners.
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
template <class K, class V, class Hash>
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
struct async_node_base;
namespace impl
{
struct async_node_traits;
}
struct async_scheduler;
struct async_worker_scope;
struct inline_scheduler;
struct async_thread_pool;
struct async_context;
template <class T>
struct async_result;
template <class T>
struct async;

/// The normal async handle: an 8 B intrusive cc::shared_ptr over one slab node (see thread/async.hh).
template <class T>
using shared_async = shared_ptr<async<T>, impl::async_node_traits>;


//
// Hashing
//

struct hash128;


//
// Utilities
//

template <class EnumT, isize Bits = 8 * sizeof(EnumT)>
struct flags;

struct unit;

} // namespace cc
