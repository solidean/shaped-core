#pragma once

#include <clean-core/fwd.hh>

/// Aggregate forward declarations for versioned-document.
///
/// This header is also the API index: every name the library plans to expose is declared here, with the one line that says what it is.
/// Every layer here is implemented; persistence is the library above.
/// The design is [the concept docs](../../docs/_index.md#concepts), one file per concept.
///
/// The four layers, bottom to top:
///   value        — a canonically-encoded binary value, the only thing a property holds
///   op / op_graph — the immutable content-addressed DAG that is the source of truth
///   raw_document — the schema-agnostic materialization of a set of heads
///   document     — the typed, immutable index an application queries
///
/// Persistence is not here: it is versioned-document-file, one library up.

namespace vdoc
{
// Pull in the shaped-core vocabulary types (i32, u8, isize, ...) so we write them bare inside vdoc
// without leaking them into the global namespace.
using namespace cc::primitive_defines;
} // namespace vdoc

// ---- values ------------------------------------------------------------------------------------
//
// A value is a self-describing byte sequence: a tag byte plus its payload.
// Equality and hashing are defined on those bytes and on nothing else.

namespace vdoc
{
/// What a value holds — the tag byte that opens every encoded value.
enum class value_kind : u8;

/// An owning value.
/// Small values live inline, which is nearly all of them.
struct value;

/// A non-owning view of an encoded value, e.g. into an op's payload.
struct value_view;

/// Builds arrays and objects incrementally, then hands out the finished encoding.
class value_builder;

/// Why bytes would not decode into a value.
enum class value_decode_error_kind : u8;

/// What was wrong, and at which byte.
struct value_decode_error;

/// Why a builder's contents would not make a value.
enum class value_build_error : u8;
} // namespace vdoc

// ---- identifiers -------------------------------------------------------------------------------
//
// Distinct wrapper types over an interned string, so the three can never be mixed up.
// Interning is process-local: never serialize a raw interned id, always the canonical bytes.

namespace vdoc
{
/// Names an entity — an arbitrary application-chosen string, whether a name, a path or a uuid.
struct entity_id;

/// Names a component type, e.g. "Transform".
struct component_type_id;

/// Names a property within a component, e.g. "position".
struct property_id;

/// One property path — an entity, a component type and a property.
/// The addressable unit of the whole system: ops assign to paths, conflicts are per path, and diffs are lists of paths.
struct property_path;
} // namespace vdoc

// ---- ops and the DAG ---------------------------------------------------------------------------

namespace vdoc
{
/// Content hash of an op: a 32-byte BLAKE3 digest that recursively commits to the whole history behind it.
struct op_id;

/// What an assignment does to its path: write a value, or withdraw this op's contribution to it.
enum class assignment_kind : u8;

/// One property assignment — the unit an op is made of.
struct assignment;

/// Which encoding an op's assignment blob uses, as its leading tag byte.
/// The tag is what lets the assignment encoding change without touching the hashing rule.
enum class assignment_encoding : u8;

/// Walks an op's assignments in place, so materialization never builds a per-op vector.
class assignment_cursor;

/// The exact bytes a store persists for an op, and the only thing op_id commits to.
struct op_payload;

/// An immutable, content-addressed bundle of assignments plus the parent links and metadata that place it in the DAG.
struct op;

/// Builds one op from a set of edits, diffing against the parents so only changed properties are written.
class op_builder;

/// The op DAG: ops keyed by content hash, plus materialization of a set of heads.
class op_graph;

/// Why bytes read from a store would not become an op.
enum class op_decode_error : u8;

/// The result of checking an op against its own stored bytes.
/// A skeleton op left behind by pruning is unverifiable by construction and must never read as tampering.
enum class op_verification : u8;

/// One op exactly as a peer sent it, over the receive buffer's own bytes.
struct received_op;

/// Why a received batch was refused — malformed bytes, or parents that disagree with a held skeleton.
enum class integration_error : u8;

/// The refusal itself, naming the op that caused it.
struct integration_rejection;

/// What integrating a received batch did.
struct integration_result;
} // namespace vdoc

// ---- the raw document --------------------------------------------------------------------------
//
// Schema-agnostic, and the exact shape of the conceptual model:
//   raw_document  : entity_id         -> raw_entity
//   raw_entity    : component_type_id -> raw_component
//   raw_component : property_id       -> raw_property

namespace vdoc
{
/// One surviving write of a property: the value, and the op that wrote it.
struct property_value;

/// Every surviving write of one property.
/// More than one means concurrent writers that do not dominate each other — which is normal, and resolved at parse time.
struct raw_property;

/// One component's properties.
struct raw_component;

/// One entity's components.
struct raw_entity;

/// The materialized but still untyped document.
struct raw_document;

/// A materialized document owning every byte it points at, so it outlives the ops that wrote them.
class snapshot_document;

/// Materialization results cached against the op they were computed at.
/// Caller-owned and passed in explicitly; op_graph holds no cache of its own.
class snapshot_cache;

/// When installing a snapshot is worth its memory.
struct snapshot_policy;

/// How much of each entry's path a change set actually addresses.
enum class change_granularity : u8;

/// The paths that have to be re-interpreted — the input side of an incremental re-interpretation.
/// Over-reports at worst, so every consumer is correct at every granularity.
class change_set;

/// Collects paths in any order and establishes a change set's invariant once.
class change_set_builder;
} // namespace vdoc

// ---- interpretation ----------------------------------------------------------------------------
//
// Parsing is a non-destructive projection: it never mutates the DAG, and it never refuses to load.
// Policy goes in, a report comes out, and everything unrecognized becomes a diagnostic rather than a failure.

namespace vdoc
{
/// The set of component types an application understands.
/// Type-erased and runtime, so tests can hand over a subset.
class component_registry;

/// What the registry knows about one component type.
struct component_schema;

/// How an application's component type reads and writes itself — specialize this per component.
template <class ComponentT>
struct component_traits;

/// The sink a component's write emits its properties into, bound to one entity and one component type.
class component_writer;

/// Reads one component's properties, applying the multi-value rules exactly once.
/// What a component's parse is handed, and the only supported way to read a property.
class property_reader;

/// How to interpret a raw document: which components are known, which entities are alive, how genuine conflicts resolve.
class parse_policy;

/// The conventions, ready to use: registry lookup, `alive`-based deletion, and local-closure conflict resolution.
class default_parse_policy;

/// What parsing found — string-free by design, so it localizes and machine-reads.
enum class diagnostic_kind : u8;

/// One thing parsing found, and where.
struct diagnostic;

/// A property that was structurally multi-valued but whose writers agreed.
/// Collapsed silently, and kept as a tidy-up hint.
struct agreed_multi_value;

/// The out-parameter of a parse: diagnostics plus agreed multi-values.
struct parse_report;

/// The typed document: an immutable index, built once, queried many times, shareable as a snapshot.
class document;

/// Evolves a document by consuming it — the only way from one document to the next without a full parse.
class document_builder;

/// Whether an apply added, removed or re-parsed a thing.
enum class change_kind : u8;

/// What an apply did, so an application can invalidate its own projections without diffing two documents.
struct change_summary;

/// Knobs on an incremental apply: how far it will look for the chain, and where its materialization may terminate.
struct incremental_apply_options;

/// Why an apply re-parsed instead of evolving the document it was handed.
enum class apply_fallback_reason : u8;

/// A layer written property by property by a producer, with no op graph behind it.
class direct_layer;

/// Names one layer in a stack, and keeps naming it as others come and go.
struct layer_handle;

/// An ordered stack of independent histories, composed into one document per property path.
class layer_stack;

/// Knobs on a layered apply.
struct layered_apply_options;

/// What one layered apply did.
struct layered_apply_stats;

/// What one apply did, for a caller that has to see whether the fast path ran.
struct incremental_apply_stats;

/// Evolves a document from one op to a descendant, incrementally where it can and by re-parsing where it cannot.
[[nodiscard]] document apply(document&& doc,
                             op_graph const& graph,
                             op_id const& from,
                             op_id const& to,
                             parse_policy const& policy,
                             parse_report& report,
                             change_summary& out_changes,
                             incremental_apply_options options,
                             incremental_apply_stats* stats);

/// Interprets a raw document into a typed one.
/// Never fails: everything questionable lands in the report.
[[nodiscard]] document parse(raw_document const& raw, parse_policy const& policy, parse_report& report);

/// Whether something is alive, by the unambiguously-false rule; a contested `$alive` stays alive plus a diagnostic.
[[nodiscard]] bool is_alive(raw_component const& raw, property_path const& path, parse_report& report);
} // namespace vdoc

namespace vdoc::impl
{
/// The bump allocator a document's storage lives in — vdoc-local and temporary, see document.cc.
class document_arena;

/// Drives one parse, and the only thing that fills a document.
class parser;

/// One layer as the composition sees it — just its raw document.
struct layer_view;

/// Reusable buffers for composing one entity across several layers.
struct compose_scratch;
} // namespace vdoc::impl
