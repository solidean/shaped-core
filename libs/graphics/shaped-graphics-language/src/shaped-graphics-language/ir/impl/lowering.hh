#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/function/function_ref.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics-language/check/checked_module.hh>
#include <shaped-graphics-language/emit/impl/plan.hh>
#include <shaped-graphics-language/ir/ir.hh>

#include <initializer_list>

/// One compile's walk of a flat entry point, which fills a `program`.
/// lower.cc holds its tables and its statements, lower_memory.cc its types and places, lower_expr.cc its expressions,
/// and lower_call.cc its calls of builtins.
namespace sgl::ir::impl
{
/// Where a value lives: what a store takes and a load reads from.
struct place
{
    value_ref pointer = value_ref::none;
    /// The value's SGL type, which is what a load gives.
    check::type_id type = check::type_id::none;
};

inline constexpr u32 k_no_element = 0xffffffffu;

class lowering
{
public:
    lowering(emit::impl::plan const& plan, program& out);

    void run();

    /// Empty unless a construct has no lowering yet; the first one found, for a reader.
    [[nodiscard]] cc::string const& failure() const { return _failure; }

private:
    using flat_expr_id = check::flat_expr_id;
    using flat_stmt_id = check::flat_stmt_id;
    using type_id = check::type_id;

    // ---- lower.cc: the program's tables, and its statements
    [[nodiscard]] u32 add_name(cc::string_view name);
    [[nodiscard]] block_ref new_block();
    void open_block(block_ref b);
    void set_merge(merge_kind kind, block_ref merge, block_ref continue_to = block_ref::none);
    value_ref add(op code, type_ref type, cc::span<u32 const> operands, u8 sub = 0, u8 aux = 0);
    value_ref add(op code, type_ref type, std::initializer_list<value_ref> operands, u8 sub = 0, u8 aux = 0);
    void branch(block_ref to);
    void branch_if(value_ref condition, block_ref yes, block_ref no);
    void unsupported(cc::string_view what);

    void declare_stage_inputs();
    /// LEGAL-69's shadow, one `uint` per 32 scalars of workgroup memory, which a `flat_written_word` indexes.
    [[nodiscard]] value_ref written_shadow();
    u32 add_stage_element(stage_element e, cc::string_view semantic, cc::string_view name);
    void elements_of_struct(check::type_id t, stage_part part, u32 array_size);
    u32 element_of_input(isize index);
    /// Everything the entry point's edge holds, in declared order; lower_stage.cc.
    void declare_stage_elements();
    /// A vertex or pixel stage's parameters, read from their elements ahead of the body.
    void read_stage_edge();
    /// Refuses a link the native routes do not carry yet.
    void check_links();
    /// What `_out.primitive` says of a geometry or a tessellation stage.
    void describe_primitive_stage();
    /// A geometry or tessellation stage's parameters, in the order written; an array of them stays unread (EMIT-240).
    void read_primitive_edge();
    void read_stage_input(isize input, u32 element);
    /// Each output element of the value a `return` gives.
    void store_stage_result(value_ref result);
    /// The elements of `part` written with `value` of `t`, member by member, at `index` where they stand per vertex.
    void store_struct(check::type_id t, value_ref value, stage_part part, value_ref index = value_ref::none);
    /// `element` as it stands, or at `index` where it stands once per control point or vertex.
    [[nodiscard]] value_ref read_element(u32 element, value_ref index = value_ref::none);
    [[nodiscard]] value_ref read_struct(check::type_id t, u32& element, value_ref index = value_ref::none);
    /// EMIT-196: the 64-bit value of `t` read back from the words its next elements hold.
    [[nodiscard]] value_ref joined_words(check::type_id t, u32& element, value_ref index = value_ref::none);
    /// The first element of the array parameter `id` names, or `k_no_element` for anything else.
    [[nodiscard]] u32 stage_array_of(flat_expr_id id) const;
    /// A whole array parameter of `t`, which only a use of the array itself asks for.
    [[nodiscard]] value_ref read_stage_array(check::type_id t, u32 first);
    /// `s.emit(v)` or `s.end_strip()` on a geometry stage's stream.
    void stream_call(check::flat_call const& c, bool is_emit);
    /// One of a mesh stage's `mesh_output` methods, each a write of what the workgroup hands the rasterizer.
    void mesh_call(check::flat_call const& c, cc::string_view method);
    /// `dispatch_mesh(x, y, z, p)`, which stores `p` and ends the task stage.
    void dispatch_mesh(check::flat_call const& c);
    /// The one global a task stage's payload stands in, of the struct `t`.
    [[nodiscard]] value_ref payload_global(check::type_id t);

    // ---- lower_ray.cc: the ray-tracing stages
    [[nodiscard]] bool is_ray_stage() const;
    /// The stage element of `system`, declared on its first read, as a value of `type`.
    [[nodiscard]] value_ref ray_read(built_in system, type_ref type, cc::string_view name);
    void read_ray_edge();
    /// The payloads the entry point is handed or traces with, and what each field's shaders do with it (EMIT-137).
    void annotate_payloads();
    [[nodiscard]] value_ref hit_attribute_global(type_ref type);
    /// The one global of `space` that hands a value of `type` over.
    [[nodiscard]] value_ref output_global(type_ref type, storage space);
    /// The place `id` copied into its global, which `handed_back` copies out of.
    [[nodiscard]] value_ref handed_over(flat_expr_id id, storage space);
    void handed_back(flat_expr_id id, value_ref global);
    void trace_ray(check::flat_call const& c);
    void call_callable(check::flat_call const& c);
    /// A trace's operands as `op::trace_ray` takes them, its payload copied over; false where something failed.
    [[nodiscard]] bool trace_operands(cc::span<flat_expr_id const> arguments, storage space, cc::vector<u32>& out);
    [[nodiscard]] value_ref hit_object_call(check::flat_expr const& x,
                                            check::flat_call const& c,
                                            builtins::function_record const& record);
    /// A read of a ray-tracing stage's launch, ray or hit.
    [[nodiscard]] value_ref ray_stage_call(builtins::function_record const& record,
                                           cc::span<value_ref const> args,
                                           type_ref result);
    /// An any hit's `return`: its decision ends the stage where it is no acceptance.
    void decide_hit(value_ref decision);
    /// An intersection's `return`: where it hit, its attributes and the target's report.
    void report_intersection(value_ref reported, check::type_id type);
    /// `affine3_of_rows`, `affine3_row` and the three products.
    [[nodiscard]] value_ref affine_call(builtins::function_record const& record,
                                        cc::span<value_ref const> args,
                                        type_ref result);
    /// A step or a read of a query; `ray_query_begin` is `declare`'s, which knows the local's flags.
    [[nodiscard]] value_ref ray_query_call(check::flat_call const& c, builtins::function_record const& record);
    [[nodiscard]] value_ref ray_query_read(value_ref query, ray_query_value what, bool is_committed, type_ref result);
    /// Row `row` of a transform, which the targets hand over by column.
    [[nodiscard]] value_ref ray_query_row(value_ref query, ray_query_value what, bool is_committed, value_ref row);
    [[nodiscard]] value_ref begin_ray_query(check::local_id id);
    /// The structure `id` names, a member of a binding or an element of an array of them.
    /// On vulkan, that member's address in `sg_acceleration_addresses`, converted, so a partitioned tlas traces too.
    [[nodiscard]] value_ref acceleration_structure(check::flat_expr_id id);
    /// Resource `index`'s structure read from its device address, the vulkan form of `acceleration_structure`.
    [[nodiscard]] value_ref acceleration_by_address(i32 index);
    /// EMIT-196: the elements from `element` on written with the 64-bit `value` of `t`, its words low first.
    void store_words(check::type_id t, value_ref value, u32& element, value_ref index = value_ref::none);
    /// Whether the parameter at `position` is a pixel stage's second struct, what a mesh stage wrote per primitive.
    [[nodiscard]] bool is_primitive_parameter(isize position) const;
    void hold_parameter(check::local_id id, value_ref value);
    void statements(ast::range_of<flat_stmt_id> range);
    void statement(flat_stmt_id id);
    void if_(check::flat_if const& s);
    void loop_(check::flat_loop const& s);
    void while_(check::flat_while const& s);
    void for_(check::flat_for const& s);
    void once_(check::flat_once const& s);
    void switch_(check::flat_switch const& s);
    void declare(check::local_id id, flat_expr_id value, bool is_mut);
    void assign(check::flat_assign const& a);
    void assign_swizzle(check::flat_member const& place_of_it, flat_expr_id value);

    // ---- lower_memory.cc: types, and where values live
    [[nodiscard]] type_ref intern(type const& t);
    [[nodiscard]] type_ref scalar_type(check::value_kind kind);
    [[nodiscard]] type_ref vector_type(type_ref element, u32 lanes);
    [[nodiscard]] type_ref pointer_type(type_ref element, storage space);
    [[nodiscard]] type_ref array_type(type_ref element, u32 count, i32 stride);
    [[nodiscard]] type_ref runtime_array_type(type_ref element, i32 stride);
    [[nodiscard]] type_ref image_type(check::type_info const& t);
    [[nodiscard]] type_ref value_type(type_id t);
    [[nodiscard]] type_ref memory_type(type_id t, emit::impl::address_space space);
    [[nodiscard]] i32 column_stride(type_id t, emit::impl::address_space space);
    [[nodiscard]] type_ref block_of_members(cc::string_view name,
                                            cc::span<emit::impl::planned_member const> members,
                                            emit::impl::address_space space);

    [[nodiscard]] value_ref bound_block(emit::impl::planned_constants const& block);
    [[nodiscard]] value_ref bound_resource(i32 index);
    [[nodiscard]] value_ref bound_workgroup(i32 index);
    [[nodiscard]] value_ref bound_sampler(i32 index);
    [[nodiscard]] value_ref add_global(global g, cc::string_view name);
    [[nodiscard]] value_ref add_local(type_ref value, cc::string_view name);

    [[nodiscard]] cc::optional<place> place_of(flat_expr_id id);
    /// One texel of the storage image `image` as a place, which only an atomic takes so far.
    /// `coordinate` holds an array's layer as its last lane, as SPIR-V takes it.
    [[nodiscard]] cc::optional<place> texel_of(flat_expr_id image, value_ref coordinate);
    [[nodiscard]] cc::optional<place> chain_of(flat_expr_id id, cc::vector<u32>& chain);
    /// `nonuniform i`, whose value is `i`; `uniform i` is no such mark, since it is the plain index.
    [[nodiscard]] bool is_nonuniform(flat_expr_id id) const;
    /// A pointer to one element of a binding array, or none where `x` is no element of one.
    [[nodiscard]] value_ref bound_element(flat_expr_id id);
    /// `k_ir_nonuniform` where `pointer` came through such an index.
    /// The length of a binding array's element at a non-uniform index, asked at uniform indices alone; none where the
    /// target or the stage asks it as it is.
    [[nodiscard]] value_ref uniform_element_length(flat_expr_id id);
    [[nodiscard]] u8 nonuniform_of(value_ref pointer) const;
    [[nodiscard]] value_ref load(place const& p);
    void store(place const& p, value_ref value);
    [[nodiscard]] value_ref spilled(value_ref value, type_id type);
    [[nodiscard]] bool holds_array(type_ref t) const;

    // ---- lower_expr.cc: expressions
    [[nodiscard]] value_ref expr(flat_expr_id id);
    [[nodiscard]] value_ref literal(check::flat_expr const& x);
    [[nodiscard]] value_ref constant_of(type_ref type, u32 low, u32 high = 0);
    [[nodiscard]] value_ref null_of(type_ref type);
    [[nodiscard]] value_ref int_constant(i32 value);
    [[nodiscard]] value_ref uint_constant(u32 value);
    [[nodiscard]] value_ref bool_constant(bool value);
    [[nodiscard]] value_ref float_constant(type_ref type, f64 value);
    [[nodiscard]] value_ref short_circuit(flat_expr_id lhs, flat_expr_id rhs, bool is_and);
    [[nodiscard]] bool is_safe_early(flat_expr_id id) const;
    [[nodiscard]] value_ref member_of(check::flat_expr const& x, check::flat_member const& m);
    [[nodiscard]] value_ref extracted(value_ref composite, type_ref type, u32 index);
    [[nodiscard]] i32 position_in(type_id object, i32 member) const;
    [[nodiscard]] value_ref splat(value_ref scalar, type_ref vector);

    // ---- lower_call.cc: calls of builtins
    [[nodiscard]] value_ref call(check::flat_expr const& x, check::flat_call const& c);
    [[nodiscard]] value_ref arithmetic(u32 shape, cc::span<value_ref const> args, type_ref result);
    [[nodiscard]] value_ref converted(value_ref value, type_ref from, type_ref to);
    [[nodiscard]] value_ref column_wise(op code, value_ref a, value_ref b, type_ref matrix);
    [[nodiscard]] value_ref constructed(cc::span<value_ref const> args, type_ref result);
    [[nodiscard]] value_ref atomic(check::flat_call const& c, check::flat_expr const& x);
    [[nodiscard]] value_ref bytes_call(check::flat_call const& c, builtins::function_record const& record);
    [[nodiscard]] value_ref texture_call(check::flat_call const& c,
                                         builtins::function_record const& record,
                                         type_ref result);
    [[nodiscard]] value_ref subgroup_call(check::flat_call const& c,
                                          builtins::function_record const& record,
                                          type_ref result);
    [[nodiscard]] type_ref scalar_of(type_ref t) const;
    [[nodiscard]] u32 lanes_of(type_ref t) const;

    emit::impl::plan const& _plan;
    check::checked_module const& _m;
    check::flat_entry_point const& _e;
    program& _out;

    cc::string _failure;
    block_ref _open = block_ref::none;
    /// Past the open block's last operation: what stands after a `break` never runs.
    bool _is_terminated = true;
    u32 _statement = 0xffffffffu;
    check::float_level _guarantee = check::float_level::fast;

    /// Parallel to `_e.locals`: an immutable local's value, or a mutable one's variable.
    cc::vector<value_ref> _locals;
    cc::vector<bool> _is_variable;
    /// Parallel to `_e.locals`: the first element of a geometry or tessellation parameter, or `k_no_element`.
    cc::vector<u32> _parameter_elements;
    /// What a `break` and a `continue` reach, innermost last.
    cc::vector<block_ref> _breaks;
    cc::vector<block_ref> _continues;

    value_ref _written = value_ref::none;
    value_ref _payload = value_ref::none;
    value_ref _hit_attributes = value_ref::none;
    struct ray_output_global
    {
        type_ref type = type_ref::none;
        storage space = storage::ray_output;
        value_ref global = value_ref::none;
    };
    cc::vector<ray_output_global> _ray_outputs;

    /// Parallel to `_m.types`.
    cc::vector<type_ref> _value_types;
    struct memory_entry
    {
        type_id type = type_id::none;
        emit::impl::address_space space = {};
        type_ref ir = type_ref::none;
    };
    cc::vector<memory_entry> _memory_types;
    /// Parallel to `_out.types` per storage class: the pointer to each.
    cc::vector<type_ref> _pointers;
    /// An open table of positions in `_out.constants`, plus one, with room to spare.
    cc::vector<u32> _constant_slots;

    /// Parallel to the plan's tables: the global each is declared as, once something reaches it.
    value_ref _constants_global = value_ref::none;
    /// `sg_acceleration_addresses`, declared once a trace reads it.
    value_ref _acceleration_addresses = value_ref::none;
    cc::vector<value_ref> _block_globals;
    cc::vector<value_ref> _resource_globals;
    cc::vector<value_ref> _workgroup_globals;
    cc::vector<value_ref> _sampler_globals;

    type_ref _void = type_ref::none;
    type_ref _bool = type_ref::none;
    type_ref _int = type_ref::none;
    type_ref _uint = type_ref::none;
    type_ref _float = type_ref::none;
};
} // namespace sgl::ir::impl
