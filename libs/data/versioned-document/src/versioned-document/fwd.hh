#pragma once

#include <clean-core/fwd.hh>

/// Aggregate forward declarations for versioned-document.
///
/// This header is also the API index: every name the library plans to expose is declared here, with the one line that says what it is.
/// The value layer is implemented; everything below it is not yet.
/// The design is [the concept](../../docs/concept.md).
/// The milestones that land it are [the todo list](../../docs/todo/_index.md).
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

/// One property assignment — the unit an op is made of.
struct assignment;

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
} // namespace vdoc
