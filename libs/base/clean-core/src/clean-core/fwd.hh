#pragma once

#include <clean-core/record/domain_fwd.hh>

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
struct scoped_environment_variable; // one environment variable, set for a scope and put back (platform/environment.hh)
enum class cache_kind : u8;         // what a cache level holds: data, instructions or both (platform/system_info.hh)
struct cpu_cache_level;             // one level of one core class's cache hierarchy (platform/system_info.hh)
struct cpu_core_class;              // one group of alike cores, of which a modern CPU has several
struct numa_node;
struct system_info;        // what machine this is, gathered once (platform/system_info.hh)
enum class identity_field; // one identifying fact about the machine, requested by name (platform/system_identifier.hh)
struct system_identifier;  // the identifying facts that were asked for, and only those
struct resource_limits; // what THIS process may use, as opposed to what the machine has (platform/resource_limits.hh)

enum class query_status : u8; // why a live system query could not answer (platform/system_metrics.hh)
enum class metric : u8;       // one live quantity a dashboard can ask about
struct query_error;
struct cpu_counters;    // monotone CPU time as the OS accumulated it
struct cpu_counter_set; // those counters, per machine and per core
struct cpu_load;        // how busy the CPU was over one sampling interval
struct memory_usage;    // physical memory as it stands right now
class cpu_load_sampler; // CPU load, differenced against this sampler's previous reading

enum class process_id : u64; // a process, for the per-process queries (platform/process_metrics.hh)
struct process_usage;        // memory and handles this process holds right now
struct process_cpu_counters; // monotone per-process CPU time and I/O
struct process_cpu_load;     // how many cores' worth this process used over an interval
class process_cpu_sampler;
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

enum class glob_option;           // what glob_matches does beyond a byte-for-byte match (string/glob.hh)
enum class float_notation : char; // how cc::to_chars renders a float (string/to_string.hh)

class interned_string; // a handle to one canonical copy of a byte sequence (string/interned_string.hh)
class string_interner; // a table of them, for tests that want isolation from the process-wide one
namespace impl
{
struct intern_entry;
struct intern_shard;
} // namespace impl


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

template <class T, class ContainerT, class IndexT>
struct ringbuffer_container;

template <class ContainerT, class WordT>
struct bitset_container;

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

// Comparator vocabulary for the ordering algorithms (common/compare.hh).
// default_less / default_greater are both built on operator<; compare_by builds a lexicographic one from projections.
struct default_less;
struct default_greater;
template <class ProjF>
struct descending_projection;
template <class... ProjFs>
struct lexicographic_comparator;

template <class K, class V, class Hash = default_hash, class KeyEqual = default_equal>
struct map;
template <class T, class Hash = default_hash, class KeyEqual = default_equal>
struct set;

template <class K, class V>
struct key_value_provider;
template <class K, class V, class Hash = default_hash>
struct in_memory_key_value_provider;
template <class K, class V>
struct key_value_cache;

class byte_stream_builder;

template <class T>
struct ringbuffer;
template <class T, isize N>
struct fixed_ringbuffer;
template <class T, class RingT>
struct ringbuffer_iterator;

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
template <class WordT>
struct bit_ref;
template <class WordT, bool Set>
struct bit_index_iterator;
template <class WordT, bool Set>
struct bit_index_range;

template <class RangeT>
struct sequence;

// The virtual ranges the sorting algorithms permute (algorithm/sort.hh), built by cc::as_index_swap_range*.
template <class RangeT>
struct index_swap_range_of;
template <class RangeT, class KeyF>
struct index_swap_range_by;
template <class KeyRangeT, class... RangeTs>
struct index_swap_range_multi;
template <class KeyF, class... RangeTs>
struct index_swap_range_multi_by;


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

enum class thread_id : u64; // a thread's identity, compared for equality only (thread/thread.hh)

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

enum class async_error_kind : u8;  // an ordinary error, or a cancellation (thread/async_node.hh)
enum class async_step_status : u8; // what one compute step reports back to the poll loop
enum class async_node_state : u8;  // a node's lifecycle state, moved by CAS
struct async_error;
struct alignas(32) async_type_ops; // the type-erased node ops descriptor (thread/async_node.hh)
struct async_node_base;
namespace impl
{
struct async_node_traits;
}
struct async_scheduler;
struct async_worker_scope;
struct async_no_worker_scope;
struct singlethreaded_scheduler;
struct async_thread_pool;
struct thread_pump_registration; // one registered cooperative pump (thread/thread_pump.hh)
namespace impl
{
struct thread_pump_entry;
}
struct scoped_default_async_scheduler;
struct async_ambient_link; // one link of the ambient context chain (thread/async_ambient.hh)
struct async_ambient_scope;
struct async_ambient_handle; // a captured chain head, re-installable on another thread
struct async_ambient_install_scope;
struct async_context_base;
template <class T, class E = async_error>
struct async_context;
template <class T, class E = async_error>
struct async;
template <class T, class E = async_error>
struct async_scheduled; // the eager coroutine return type (thread/async_coroutine.hh)

/// The normal async handle: an 8 B intrusive cc::shared_ptr over one slab node (see thread/async.hh).
template <class T, class E = async_error>
using shared_async = shared_ptr<async<T, E>, impl::async_node_traits>;

/// A non-owning handle to the same node; lock() gives back a shared_async, or an empty one once nobody owns it.
/// For a registry that must COORDINATE live operations without keeping their results alive — a table of these
/// forgets an operation the moment its last consumer does, rather than becoming a second cache of the values.
template <class T, class E = async_error>
using weak_async = weak_ptr<async<T, E>, impl::async_node_traits>;


//
// Bytes
//

struct hash128;
struct hash256;
class blake3;

enum class compression_algorithm : u8;
enum class compression_framing : u8;
struct compression_config;
struct decompression_config;
struct compression_dictionary;
struct compressor;
struct decompressor;
struct decompressing_read_stream_adapter;
struct compressing_write_stream_adapter;


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
class vector_write_stream_adapter;
class string_write_stream_adapter;


//
// Utilities
//

enum class flag_encoding; // whether an enum's values are bit indices or bit patterns (common/enum_traits.hh)
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

struct calendar_time; // one wall-clock instant, split into the fields a person reads (common/time.hh)


//
// Recording
//

/// The domain every recording site in clean-core is attributed to.
/// cc::rec shadows this with the system domain, so the recorder's own bookkeeping stays separable from the library's.
CC_REC_DECLARE_DOMAIN(g_rec_domain);

} // namespace cc

//
// Console
//

namespace cc::console
{
enum class color_mode; // whether output is colored: automatic, always, never (platform/console.hh)
enum class color : u8; // the SGR set every terminal agrees on
} // namespace cc::console
