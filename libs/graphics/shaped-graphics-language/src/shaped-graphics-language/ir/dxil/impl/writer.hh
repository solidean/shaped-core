#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/function/function_ref.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics-language/ir/dxil/impl/dx_ops.hh>
#include <shaped-graphics-language/ir/dxil/impl/llvm.hh>
#include <shaped-graphics-language/ir/dxil/signature.hh>
#include <shaped-graphics-language/ir/ir.hh>

/// One `ir::program` as an LLVM module of DXIL's dialect.
///
/// DXIL has no vectors, matrices or composite values, so every value of the program is a run of scalars here, its
/// leaves, in the order of the type's parts.
/// writer.cc holds the walk and the plain operations, writer_math.cc `math`, writer_memory.cc places and memory,
/// writer_resources.cc handles, textures, atomics and subgroups, and writer_stage.cc what differs per stage.
/// Every table a new case adds a row to stands in the file of its topic.
namespace sgl::ir::dxil::impl
{
/// What a stage's container parts and metadata say; a graphics stage fills the signatures.
struct resource_record
{
    /// The PSV's class of it.
    u32 psv_type = 0;
    u32 space = 0;
    u32 lower = 0;
    u32 upper = 0;
    u32 kind = 0;
    u32 psv_flags = 0;
    /// What `RDAT` adds: its class, its id in that class, and its name.
    u32 cls = 0;
    u32 id = 0;
    cc::string name;
};

/// All the container's parts need of the program, which the module alone does not say.
struct module_summary
{
    u64 metadata_flags = 0;
    u64 feature_flags = 0;
    u32 psv_stage = 0;
    bool is_library = false;
    /// A library function's attributes, `dx.op` and `RDAT` both state.
    u32 attribute_bytes = 0;
    u32 shader_kind = 0;
    u32 min_shader_minor = 0;
    u32 program_kind = 0;
    /// The shader model, as a program's version word states it.
    u32 program_version = 0;
    /// The DXIL version a program's header states.
    u32 dxil_version = 0;
    i32 workgroup[3] = {};
    cc::string entry_name;
    cc::vector<resource_record> resources;
    /// By table, then in declared order: `pack_signature`'s.
    cc::vector<signature_element> signature;
    /// Parallel to `signature`: the lanes the final operations read or write, counted from the element's first.
    cc::vector<u8> usage;
    /// Parallel to `signature`: `DXIL::SemanticKind` and `DXIL::InterpolationMode`.
    cc::vector<u8> semantic_kinds;
    cc::vector<u8> modes;
    /// A stage ahead of the rasterizer that writes `SV_Position`, which `PSV0` states.
    bool writes_position = false;
    /// A mesh or task stage's `PSV0` words: what it hands on, and a mesh stage's most of each and its primitives.
    u32 workgroup_bytes = 0;
    u32 payload_bytes = 0;
    u32 max_primitives = 0;
    u32 mesh_topology = 0;
    /// What a geometry or tessellation stage's `PSV0` words state, in DXIL's numbers.
    u32 input_primitive = 0;
    u32 output_topology = 0;
    u32 max_vertices = 0;
    u32 control_points = 0;
    u32 domain = 0;
    u32 output_primitive = 0;
    /// A pixel stage's `PSV0` words: that it writes a depth, and that it runs per sample.
    bool writes_depth = false;
    bool runs_per_sample = false;
    /// A stage that reads the view, whose `PSV0` then states which outputs depend on it.
    bool uses_view = false;
    /// How many rows each signature takes, and how many scalars up to its last.
    u32 input_rows = 0;
    u32 output_rows = 0;
    u32 patch_rows = 0;
    u32 input_scalars = 0;
    u32 output_scalars = 0;
    u32 patch_scalars = 0;
    /// One bit per output scalar: what depends on the view, where a stage reads it.
    cc::vector<u32> view_outputs;
    /// The same of a control stage's patch constants.
    cc::vector<u32> view_patch;
    /// One bit per output scalar for each input scalar: what reaches what.
    /// `(output_scalars + 31) / 32` words per input scalar, `input_rows * 4` of them, so `PSV0` can state it as it is.
    cc::vector<u32> dependencies;
    /// A control stage's input scalars onto its patch constants, or an evaluation stage's patch constants onto its outputs.
    cc::vector<u32> patch_dependencies;
};

/// What a module turned out to use, which the shader flags state.
struct writer_uses
{
    bool structured = false;
    bool double_ = false;
    bool double_extensions = false;
    bool waves = false;
    bool int64 = false;
    bool low_precision = false;
    /// A typed UAV read of more than one component.
    bool typed_uav_load = false;
    /// `startVertexLocation` or `startInstanceLocation`, which shader model 6.8 brought.
    bool start_location = false;
    bool view_id = false;
    /// `SV_RenderTargetArrayIndex` from a stage feeding the rasterizer.
    bool layer_output = false;
    /// A UAV in a stage that is neither pixel nor compute.
    bool uavs_at_every_stage = false;
    /// A ray query or a hit's geometry index, which raytracing tier 1.1 brought.
    bool ray_query = false;
    /// More than the 8 UAVs a device need not have more of.
    bool many_uavs = false;
    /// A 64-bit atomic in workgroup memory.
    bool int64_shared_atomics = false;
    /// A hit object or a regrouping, which Shader Model 6.9 brought.
    bool reorders = false;
};

class writer
{
public:
    explicit writer(program const& p) : _p(p) {}

    void run();

    /// Empty unless the program holds a construct with no lowering; the first one found, for a reader.
    [[nodiscard]] cc::string const& failure() const { return _failure; }
    [[nodiscard]] module_builder const& module() const { return _m; }
    [[nodiscard]] module_summary const& summary() const { return _summary; }
    /// The program carries a line table, so the container holds the module once more with its debug info (EMIT-257).
    [[nodiscard]] bool has_debug_info() const { return !_p.lines.is_empty(); }
    /// What the debug name part calls the program's debug info.
    [[nodiscard]] cc::string debug_name() const { return cc::string(_p.entry_name) + ".pdb"; }

private:
    /// Where an access reaches.
    struct place
    {
        enum class kind_t : u8
        {
            none,
            /// A variable or workgroup memory: `offset` counts scalars into `slots`.
            memory,
            /// A block of constants: `offset` counts bytes from its start.
            constants,
            /// A buffer's element `index`: `offset` counts bytes into it.
            buffer,
            /// A task stage's payload: `path_first` and `path_count` are a range of `_paths`.
            payload,
            /// One texel of a storage image: `path_first` and `path_count` are the coordinate's lanes in `_paths`.
            texel,
        } kind = kind_t::none;
        /// The storage in `_memories`, or the global.
        u32 root = 0;
        /// A constant part and, where an index is known at run time alone, a dynamic one.
        u32 offset = 0;
        ll_value dynamic = ll_value::none;
        /// The element of a buffer, and the bytes between two of them.
        ll_value index = ll_value::none;
        u32 stride = 0;
        /// A raw buffer's element as the byte address of its word, which `raw_address` reads in place of `index`.
        ll_value index_bytes = ll_value::none;
        /// The bytes between two columns of the matrix this reaches, 0 for no matrix.
        i32 matrix_stride = 0;
        /// An element of a binding array's own handle, where the global has none.
        ll_value handle = ll_value::none;
        /// How a payload's member is reached from the struct.
        u32 path_first = 0;
        u32 path_count = 0;
        /// It reaches a column of a matrix, which a payload holds flat: the path's last index is the column's first lane.
        bool is_column = false;
    };

    /// Variables or workgroup memory: one array of each scalar type a value of it holds, indexed by leaf.
    struct memory
    {
        type_ref value = type_ref::none;
        u32 leaves = 0;
        /// One storage per `k_scalar_kinds`, `none` for a type it does not hold.
        ll_value storage[8] = {ll_value::none, ll_value::none, ll_value::none, ll_value::none,
                               ll_value::none, ll_value::none, ll_value::none, ll_value::none};
    };

    struct handle_entry
    {
        /// The handle as an operation takes it; none for a binding array, whose elements each make their own.
        ll_value annotated = ll_value::none;
        /// What a library loads the handle from, a global of its own or an array of them.
        ll_value variable = ll_value::none;
        ll_type variable_type = ll_type::none;
        bool is_array = false;
        u32 lower = 0;
        u32 upper = 0;
        u32 space = 0;
        u32 cls = 0;
        u32 word0 = 0;
        u32 word1 = 0;
    };

    /// One binding the module reaches, as its metadata states it.
    struct binding_record
    {
        u32 global = 0;
        /// `DXIL::ResourceClass`: 0 SRV, 1 UAV, 2 CBV, 3 sampler.
        u32 cls = 0;
        u32 id = 0;
        u32 space = 0;
        u32 lower = 0;
        u32 kind = 0;
        u32 size = 0;
        u32 stride = 0;
        u32 component = 0;
        /// The PSV's class of it.
        u32 psv_type = 0;
        bool is_comparison = false;
        /// A 64-bit atomic reaches it, which its metadata and the PSV both say.
        bool used_by_atomic64 = false;
        /// The last slot it takes, and how many: all ones for an unbounded array.
        u32 upper = 0;
        u32 range = 1;
    };

    /// A row of a constant block this basic block read already, which a second read of it takes.
    struct row_entry
    {
        ll_value handle = ll_value::none;
        ll_value row = ll_value::none;
        ll_type overload = ll_type::none;
        ll_value fetched = ll_value::none;
    };

    struct function_entry
    {
        u64 key = 0;
        ll_value function = ll_value::none;
        ll_type type = ll_type::none;
    };

    // ---- writer.cc: the walk
    void unsupported(cc::string_view what);
    void declare_blocks();
    void lower_block(block_ref b);
    void operation(u32 index);
    void terminator(instruction const& i);
    void fill_phis();

    // ---- writer_lines.cc (EMIT-257)
    /// What the instructions written for `i` carry as their location: its statement, where the line table names one.
    void locate(instruction const& i);
    /// The debug info the located instructions name: their files, scopes and inlined calls, after the passes ran.
    void write_debug_info();
    [[nodiscard]] bool is_reachable(block_ref b) const { return _reachable[isize(b)]; }
    void find_reachable();

    // ---- writer.cc: values as their leaves
    [[nodiscard]] u32 leaves_of(type_ref t);
    [[nodiscard]] ll_type scalar_type(type_ref t);
    [[nodiscard]] type_ref scalar_at(type_ref t, u32 leaf);
    [[nodiscard]] u32 count_of(value_ref v);
    [[nodiscard]] u32 first_of_constant(u32 index);
    [[nodiscard]] ll_value leaf_at(value_ref v, u32 leaf = 0);
    /// The byte address of a raw buffer's word `index`, which SGL reaches as a byte address shifted right by 2.
    [[nodiscard]] ll_value word_address(value_ref index);
    /// The byte address `offset` bytes into raw place `p`'s word.
    [[nodiscard]] ll_value raw_address(place const& p, u32 offset);
    /// `v` as a value plus `added`, gathering the constants additions and multiplications by constants hold, so
    /// the addresses of neighbouring words share one base; none where `v` is a constant alone.
    [[nodiscard]] ll_value linear_of(value_ref v, u32& added, int depth = 0);
    /// `_scratch` as what the operation at `index` gave.
    void define(u32 index);
    void define_one(u32 index, ll_value leaf);
    [[nodiscard]] u32 leaf_offset(type_ref t, cc::span<u32 const> indices, type_ref& landed);
    [[nodiscard]] static u64 negative_zero_bits(u32 bits);
    [[nodiscard]] u8 flags_of(instruction const& i) const;

    // ---- writer.cc: the operations with one rule per lane
    void binary_lanes(u32 index, instruction const& i, binary_op code);
    void compare_lanes(u32 index, instruction const& i, predicate p);
    void cast_lanes(u32 index, instruction const& i, cast_op code);
    void resize_lanes(u32 index, instruction const& i, bool is_signed);
    void resize_floats(u32 index, instruction const& i);
    void bitcast_lanes(u32 index, instruction const& i);
    void dx_lanes(u32 index, instruction const& i, dx_op op);
    void negate_lanes(u32 index, instruction const& i);
    void not_lanes(u32 index, instruction const& i);
    void reduce_bools(u32 index, instruction const& i, bool is_all);
    void select_lanes(u32 index, instruction const& i);
    void linear_algebra(u32 index, instruction const& i);
    void construct(u32 index, instruction const& i);
    void extract(u32 index, instruction const& i);
    void insert(u32 index, instruction const& i);
    void shuffle(u32 index, instruction const& i);
    void phi(u32 index, instruction const& i);

    // ---- writer_math.cc
    void math_op(u32 index, instruction const& i);
    [[nodiscard]] ll_value dx_unary(dx_op op, ll_value a);
    [[nodiscard]] ll_value dx_binary(dx_op op, ll_value a, ll_value b);
    [[nodiscard]] ll_value dx_tertiary(dx_op op, ll_value a, ll_value b, ll_value c);
    [[nodiscard]] ll_value dot_of(cc::span<ll_value const> a, cc::span<ll_value const> b, u8 flags);
    [[nodiscard]] ll_value dx_bits(dx_op op, ll_value a);
    [[nodiscard]] ll_value float_like(ll_value like, f64 value);
    [[nodiscard]] ll_value lane_fsign(cc::span<ll_value const> a, u8 flags);
    [[nodiscard]] ll_value lane_ssign(cc::span<ll_value const> a, u8 flags);
    [[nodiscard]] ll_value lane_sabs(cc::span<ll_value const> a, u8 flags);
    [[nodiscard]] ll_value lane_atan2(cc::span<ll_value const> a, u8 flags);
    [[nodiscard]] ll_value lane_pow(cc::span<ll_value const> a, u8 flags);
    [[nodiscard]] ll_value lane_exp(cc::span<ll_value const> a, u8 flags);
    [[nodiscard]] ll_value lane_log(cc::span<ll_value const> a, u8 flags);
    [[nodiscard]] ll_value lane_fclamp(cc::span<ll_value const> a, u8 flags);
    [[nodiscard]] ll_value lane_sclamp(cc::span<ll_value const> a, u8 flags);
    [[nodiscard]] ll_value lane_uclamp(cc::span<ll_value const> a, u8 flags);
    [[nodiscard]] ll_value lane_fmix(cc::span<ll_value const> a, u8 flags);
    [[nodiscard]] ll_value lane_step(cc::span<ll_value const> a, u8 flags);
    [[nodiscard]] ll_value lane_smooth_step(cc::span<ll_value const> a, u8 flags);
    [[nodiscard]] ll_value lane_fma(cc::span<ll_value const> a, u8 flags);
    [[nodiscard]] ll_value lane_find_msb(ll_value counted, u32 width);
    [[nodiscard]] ll_value lane_umsb(cc::span<ll_value const> a, u8 flags);
    [[nodiscard]] ll_value lane_smsb(cc::span<ll_value const> a, u8 flags);
    [[nodiscard]] ll_value lane_ilsb(cc::span<ll_value const> a, u8 flags);
    void whole_length(instruction const& i, u8 flags);
    void whole_distance(instruction const& i, u8 flags);
    void whole_cross(instruction const& i, u8 flags);
    void whole_normalize(instruction const& i, u8 flags);
    void whole_reflect(instruction const& i, u8 flags);
    void whole_refract(instruction const& i, u8 flags);

    // ---- writer_memory.cc
    void declare_memory();
    [[nodiscard]] ll_value add_dynamic(ll_value dynamic, ll_value index, u32 scale);
    /// A table's memory: an internal constant per scalar type it holds.
    [[nodiscard]] u32 memory_of_table(global const& g);
    [[nodiscard]] u32 memory_of_type(type_ref value, bool is_workgroup, cc::string_view name);
    /// DXC's shape of a payload's `t`: a vector or a matrix is an array of scalars, and a `bool` an `i32`.
    [[nodiscard]] ll_type payload_type(type_ref t);
    /// The bytes `t` takes in a payload, and its alignment, as LLVM lays `payload_type` out.
    [[nodiscard]] u32 payload_layout(type_ref t, u32& alignment);
    void payload_chain(u32 index, instruction const& i);
    /// `a + b`, folded where both are known.
    [[nodiscard]] ll_value index_sum(ll_value a, ll_value b);
    /// A pointer to every leaf of `t` reached by `path`, in leaf order.
    void payload_pointers(type_ref t, cc::vector<ll_value>& path, bool is_column, cc::vector<ll_value>& out);
    [[nodiscard]] cc::vector<ll_value> payload_pointers_of(place const& p, type_ref t);
    [[nodiscard]] place place_of(value_ref pointer);
    void access_chain(u32 index, instruction const& i);
    /// A texel's place, whose coordinate an atomic hands DXIL as its offsets.
    void texel_pointer(u32 index, instruction const& i);
    /// An atomic's three offsets: a buffer's element and its bytes, or a texel's coordinate.
    void atomic_offsets(place const& p, ll_value (&offsets)[3]);
    void load(u32 index, instruction const& i);
    void store(instruction const& i);
    void load_memory(place const& p, type_ref t, cc::vector<ll_value>& out);
    void store_memory(place const& p, type_ref t, cc::span<ll_value const> leaves);
    [[nodiscard]] ll_value memory_pointer(place const& p, u32 leaf, type_ref scalar);
    void set_mesh_counts(instruction const& i);
    void write_primitive(instruction const& i);
    void load_bytes(place const& p, type_ref t, u32 offset, i32 matrix_stride, cc::vector<ll_value>& out);
    void store_bytes(place const& p, type_ref t, u32 offset, i32 matrix_stride, cc::span<ll_value const> leaves, u32& at);
    [[nodiscard]] ll_value constant_lane(place const& p, type_ref scalar, u32 offset);
    void buffer_lanes(place const& p, type_ref scalar, u32 offset, u32 lanes, cc::vector<ll_value>& out);
    void store_lanes(place const& p, type_ref scalar, u32 offset, cc::span<ll_value const> lanes);
    [[nodiscard]] ll_value byte_offset(place const& p, u32 offset);

    // ---- writer_resources.cc
    void declare_handles();
    [[nodiscard]] u32 constant_block_size(type_ref block);
    [[nodiscard]] u32 memory_size(type_ref t, i32 matrix_stride);
    [[nodiscard]] ll_value handle_of(value_ref global);
    /// The handle of the resource `p` stands in.
    [[nodiscard]] ll_value handle_at(place const& p);
    /// A handle of one element of the binding, or of the binding itself where `index` is none.
    [[nodiscard]] ll_value made_handle(handle_entry const& h, ll_value index, bool is_nonuniform);
    void texture_op(u32 index, instruction const& i);
    void image_size(u32 index, instruction const& i);
    void array_length(u32 index, instruction const& i);
    void atomic_op(u32 index, instruction const& i);
    void barrier_op(instruction const& i);
    void subgroup_op(u32 index, instruction const& i);
    void ray_query_op(u32 index, instruction const& i);
    void ray_query_read(u32 index, instruction const& i);

    // ---- writer_ray.cc
    [[nodiscard]] bool is_library() const;
    /// The library function's type, which takes the payload and the attributes as pointers.
    [[nodiscard]] ll_type entry_function_type();
    [[nodiscard]] ll_type triangle_attributes_type();
    /// What the global of a ray-tracing stage's memory is at DXIL, which is a struct whatever the program's type.
    [[nodiscard]] ll_type root_type(u32 global);
    void declare_ray_roots();
    void ray_element(u32 index, stage_element const& e, instruction const& i);
    void ray_stage_op(u32 index, instruction const& i);
    void end_of_hit(dx_op code);
    void hit_object_op(u32 index, instruction const& i);
    [[nodiscard]] ll_value atomic_on_buffer(place const& p, u32 op, ll_value value, ll_type t);

    // ---- writer_stage.cc
    [[nodiscard]] u32 entry_attributes();
    void write_entry_metadata();
    void built_in(u32 index, stage_element const& e, instruction const& i);
    void number_signature();
    /// A control stage's entry point, generated from its elements: each control point handed on as it came.
    void write_hull_function();
    /// `dx.viewIdState`'s words: a bit table of what reaches what, and the scalars either side.
    [[nodiscard]] cc::vector<ll_value> view_id_state();
    /// The properties a stage adds to its entry after its flags, as metadata operands.
    void stage_properties(cc::vector<u32>& properties);
    void stage_load(u32 index, instruction const& i);
    void stage_store(instruction const& i);
    /// A call through a sampler: the coordinate's lanes, its three offsets, none of them stated, then what `tail` adds.
    void sampled(u32 index,
                 instruction const& i,
                 dx_op op,
                 cc::function_ref<void(cc::vector<ll_value>&)> tail,
                 isize offsets = 3,
                 value_ref offset = value_ref::none);
    void discard();
    void derivative_lanes(u32 index, instruction const& i);
    /// Which lanes the module's final operations touch, once nothing more can go.
    void read_usage();
    void fill_dependencies();
    [[nodiscard]] u32 signature_list(signature_table table);

    // ---- DXIL's vocabulary
    /// `is_stable` says what it reads never changes, so a second one with these arguments is the first.
    [[nodiscard]] ll_value call(dx_op op, ll_type overload, cc::span<ll_value const> args, bool is_stable = false);
    [[nodiscard]] ll_value op_function(dx_op op, ll_type overload);
    [[nodiscard]] ll_type signature_type(char code, ll_type overload);
    [[nodiscard]] ll_type handle_type();
    [[nodiscard]] ll_type hit_object_type();
    [[nodiscard]] ll_type res_ret_type(ll_type overload);
    [[nodiscard]] ll_type cbuf_ret_type(ll_type overload);
    [[nodiscard]] ll_type dimensions_type();
    [[nodiscard]] ll_type res_bind_type();
    [[nodiscard]] ll_type res_props_type();
    [[nodiscard]] static cc::string_view overload_name(module_builder const& m, ll_type overload);
    [[nodiscard]] static u8 overload_mask(module_builder const& m, ll_type overload);

    program const& _p;
    module_builder _m;
    module_summary _summary;
    cc::string _failure;

    ll_value _entry = ll_value::none;
    /// What the program is written as: `_entry`, but for a control stage's patch-constant function.
    ll_value _body = ll_value::none;
    cc::vector<ll_block> _blocks;
    cc::vector<bool> _reachable;

    /// Every value's leaves, as runs.
    cc::vector<ll_value> _values;
    /// What an operation is building, which `define` makes its value.
    cc::vector<ll_value> _scratch;
    /// What a load or a store reads through, beside `_scratch`.
    cc::vector<ll_value> _loaded;
    /// The paths of every payload place, each a run in here, and what they are paths into.
    cc::vector<ll_value> _paths;
    /// Parallel to `program::globals`: where each one reached through a path of a struct stands.
    struct payload_root
    {
        ll_value pointer = ll_value::none;
        ll_type type = ll_type::none;
        /// A program's value the struct holds as its one member.
        bool is_wrapped = false;
    };
    cc::vector<payload_root> _payload_roots;
    /// The root `payload_pointers` is reaching into.
    payload_root const* _reached = nullptr;
    ll_value _payload = ll_value::none;
    ll_type _payload_type = ll_type::none;
    /// The positions of the phis, filled once every value exists.
    cc::vector<u32> _phis;
    /// Parallel to `_p.instructions`: where its leaves start, and how many.
    cc::vector<u32> _value_first;
    cc::vector<u32> _value_count;
    /// Parallel to `_p.constants`, the same; a constant's leaves are made once something reads them.
    cc::vector<u32> _constant_first;
    cc::vector<u32> _constant_count;
    /// Parallel to `_p.types`.
    cc::vector<u32> _leaves;
    /// Parallel to `_p.instructions`: where a pointer points.
    cc::vector<place> _places;

    cc::vector<memory> _memories;
    /// Parallel to `_p.locals` and `_p.globals`: the position in `_memories`, or -1.
    cc::vector<i32> _local_memory;
    cc::vector<i32> _global_memory;
    /// Parallel to `_p.globals`.
    cc::vector<handle_entry> _handles;

    /// Parallel to `_p.stage_elements`: its position in `_summary.signature`, or -1 for one that stands in none.
    cc::vector<i32> _element_signature;
    /// Parallel to `_summary.signature`: the id an operation names it by, its position in its table.
    cc::vector<u32> _signature_ids;

    cc::vector<binding_record> _bindings;
    cc::vector<row_entry> _rows;
    /// The operations' functions this module declared; a few dozen at most.
    cc::vector<function_entry> _functions;
    ll_type _handle = ll_type::none;
    ll_type _res_bind = ll_type::none;
    ll_type _res_props = ll_type::none;
    ll_type _dimensions = ll_type::none;

    writer_uses _uses;
};

/// The bytes of a DXIL container: the module's bitcode with the parts a pipeline asks for.
[[nodiscard]] cc::vector<byte> write_container(writer const& w);
} // namespace sgl::ir::dxil::impl
