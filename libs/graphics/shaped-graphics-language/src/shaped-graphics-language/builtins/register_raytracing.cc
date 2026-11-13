#include <clean-core/string/format.hh>
#include <shaped-graphics-language/builtins/ray_query_calls.hh>
#include <shaped-graphics-language/builtins/register.hh>
#include <shaped-graphics-language/builtins/registry.hh>

using namespace sgl;
using namespace sgl::builtins;
using namespace sgl::builtins::impl;
using check::scalar;
using check::value_kind;

namespace
{
// ---- evaluators -----------------------------------------------------------------------------------------------------
// The interpreter runs a trace's emulated form, which reads the pool; the native query is never evaluated, and these
// only give each record the value of its result type that a miss would leave.

void nothing(cc::span<scalar const>, cc::vector<scalar>&)
{
}

void no_bool(cc::span<scalar const>, cc::vector<scalar>& out)
{
    out.push_back(scalar::of(false));
}

void zero_int(cc::span<scalar const>, cc::vector<scalar>& out)
{
    out.push_back(scalar::of(i32(0)));
}

/// No cluster: what a hit is in wherever no BLAS is built from clusters, the interpreter's trace among them.
void no_cluster(cc::span<scalar const>, cc::vector<scalar>& out)
{
    out.push_back(scalar::of(i32(-1)));
}

void zero_uint(cc::span<scalar const>, cc::vector<scalar>& out)
{
    out.push_back(scalar::of_uint(0));
}

void zero_float(cc::span<scalar const>, cc::vector<scalar>& out)
{
    out.push_back(scalar::of(0.0f));
}

void zero_float2(cc::span<scalar const>, cc::vector<scalar>& out)
{
    for (auto i = 0; i < 2; ++i)
        out.push_back(scalar::of(0.0f));
}

void zero_float3(cc::span<scalar const>, cc::vector<scalar>& out)
{
    for (auto i = 0; i < 3; ++i)
        out.push_back(scalar::of(0.0f));
}

void zero_float4(cc::span<scalar const>, cc::vector<scalar>& out)
{
    for (auto i = 0; i < 4; ++i)
        out.push_back(scalar::of(0.0f));
}

void zero_uint4(cc::span<scalar const>, cc::vector<scalar>& out)
{
    for (auto i = 0; i < 4; ++i)
        out.push_back(scalar::of_uint(0));
}

void bit_or(cc::span<scalar const> in, cc::vector<scalar>& out)
{
    out.push_back(scalar::of(i32(in[0].bits | in[1].bits)));
}

void bit_and(cc::span<scalar const> in, cc::vector<scalar>& out)
{
    out.push_back(scalar::of(i32(in[0].bits & in[1].bits)));
}

void has_flags(cc::span<scalar const> in, cc::vector<scalar>& out)
{
    out.push_back(scalar::of((in[0].bits & in[1].bits) == in[1].bits));
}

written write_has(call_context const& ctx)
{
    auto const f = wrapped(ctx.arguments[1], precedence::primary);
    return {.text = cc::format("(({} & {}) == {})", wrapped(ctx.arguments[0], precedence::primary), f, f)};
}

// ---- how a query is written -----------------------------------------------------------------------------------------

struct read_spelling
{
    cc::string_view name;
    cc::string_view type;
    evaluator evaluate;
    cc::string_view hlsl_candidate;
    cc::string_view hlsl_committed;
    cc::string_view msl_candidate;
    cc::string_view msl_committed;
};

// indexed by query_read
constexpr read_spelling k_reads[] = {
    {"t", "float", zero_float, "CandidateTriangleRayT", "CommittedRayT", "get_candidate_triangle_distance",
     "get_committed_distance"},
    {"instance_id", "int", zero_int, "CandidateInstanceID", "CommittedInstanceID", "get_candidate_user_instance_id",
     "get_committed_user_instance_id"},
    {"instance_index", "int", zero_int, "CandidateInstanceIndex", "CommittedInstanceIndex", "get_candidate_instance_id",
     "get_committed_instance_id"},
    {"geometry_index", "int", zero_int, "CandidateGeometryIndex", "CommittedGeometryIndex", "get_candidate_geometry_id",
     "get_committed_geometry_id"},
    {"primitive_index", "int", zero_int, "CandidatePrimitiveIndex", "CommittedPrimitiveIndex",
     "get_candidate_primitive_id", "get_committed_primitive_id"},
    {"barycentrics", "float2", zero_float2, "CandidateTriangleBarycentrics", "CommittedTriangleBarycentrics",
     "get_candidate_triangle_barycentric_coord", "get_committed_triangle_barycentric_coord"},
    {"is_front_face", "bool", no_bool, "CandidateTriangleFrontFace", "CommittedTriangleFrontFace",
     "is_candidate_triangle_front_facing", "is_committed_triangle_front_facing"},
    {"object_to_world_row", "float4", zero_float4, "CandidateObjectToWorld3x4", "CommittedObjectToWorld3x4",
     "get_candidate_object_to_world_transform", "get_committed_object_to_world_transform"},
    {"world_to_object_row", "float4", zero_float4, "CandidateWorldToObject3x4", "CommittedWorldToObject3x4",
     "get_candidate_world_to_object_transform", "get_committed_world_to_object_transform"},
    {"object_ray_origin", "float3", zero_float3, "CandidateObjectRayOrigin", "CommittedObjectRayOrigin",
     "get_candidate_ray_origin", "get_committed_ray_origin"},
    {"object_ray_direction", "float3", zero_float3, "CandidateObjectRayDirection", "CommittedObjectRayDirection",
     "get_candidate_ray_direction", "get_committed_ray_direction"},
    // spelled by `sgl_ray_query_cluster_id`, which the HLSL dialect declares per API, and none on metal (EMIT-260)
    {"cluster_id", "int", no_cluster, "", "", "", ""},
};

constexpr cc::string_view k_query_hlsl[] = {
    "sgl_ray_query_cluster_id",
    "CandidateClusterID",
    "CommittedClusterID",
    "ray_desc",
    "RayDesc",
    "TraceRayInline",
    "Proceed",
    "CandidateType",
    "CANDIDATE_NON_OPAQUE_TRIANGLE",
    "CommitNonOpaqueTriangleHit",
    "Abort",
    "CommittedStatus",
    "CandidateTriangleRayT",
    "CommittedRayT",
    "CandidateInstanceID",
    "CommittedInstanceID",
    "CandidateInstanceIndex",
    "CommittedInstanceIndex",
    "CandidateGeometryIndex",
    "CommittedGeometryIndex",
    "CandidatePrimitiveIndex",
    "CommittedPrimitiveIndex",
    "CandidateTriangleBarycentrics",
    "CommittedTriangleBarycentrics",
    "CandidateTriangleFrontFace",
    "CommittedTriangleFrontFace",
    "CandidateObjectToWorld3x4",
    "CommittedObjectToWorld3x4",
    "CandidateWorldToObject3x4",
    "CommittedWorldToObject3x4",
    "CandidateObjectRayOrigin",
    "CommittedObjectRayOrigin",
    "CandidateObjectRayDirection",
    "CommittedObjectRayDirection",
    "CommitProceduralPrimitiveHit",
    "CandidateProceduralPrimitiveNonOpaque",
};
constexpr cc::string_view k_query_msl[] = {
    "ray_desc",
    "forced_opacity",
    "triangle_cull_mode",
    "opacity_cull_mode",
    "ray",
    "reset",
    "next",
    "abort",
    "get_candidate_intersection_type",
    "commit_triangle_intersection",
    "get_committed_intersection_type",
    "intersection_type",
    "sgl_intersection_params",
    "intersection_params",
    "get_candidate_triangle_distance",
    "get_committed_distance",
    "get_candidate_user_instance_id",
    "get_committed_user_instance_id",
    "get_candidate_instance_id",
    "get_committed_instance_id",
    "get_candidate_geometry_id",
    "get_committed_geometry_id",
    "get_candidate_primitive_id",
    "get_committed_primitive_id",
    "get_candidate_triangle_barycentric_coord",
    "get_committed_triangle_barycentric_coord",
    "is_candidate_triangle_front_facing",
    "is_committed_triangle_front_facing",
    "get_candidate_object_to_world_transform",
    "get_committed_object_to_world_transform",
    "get_candidate_world_to_object_transform",
    "get_committed_world_to_object_transform",
    "get_candidate_ray_origin",
    "get_committed_ray_origin",
    "get_candidate_ray_direction",
    "get_committed_ray_direction",
    "commit_bounding_box_intersection",
    "is_candidate_non_opaque_bounding_box",
};
constexpr cc::string_view k_polyfill_wgsl[] = {"sg_acceleration_roots", "sg_acceleration_pool"};

/// The MSL function that turns sg's ray flags into Metal's intersection parameters; the two skip flags are the trace's
/// own branch on the candidate's type, since Metal takes the geometry types as the query's template arguments.
cc::string msl_params_helper(helper_context const& ctx)
{
    if (ctx.target != language::msl)
        return {};
    return "intersection_params sgl_intersection_params(uint flags)\n"
           "{\n"
           "    intersection_params p;\n"
           "    p.accept_any_intersection((flags & 4u) != 0u);\n"
           "    if ((flags & 1u) != 0u)\n"
           "        p.force_opacity(forced_opacity::opaque);\n"
           "    else if ((flags & 2u) != 0u)\n"
           "        p.force_opacity(forced_opacity::non_opaque);\n"
           "    if ((flags & 16u) != 0u)\n"
           "        p.set_triangle_cull_mode(triangle_cull_mode::back);\n"
           "    else if ((flags & 32u) != 0u)\n"
           "        p.set_triangle_cull_mode(triangle_cull_mode::front);\n"
           "    if ((flags & 64u) != 0u)\n"
           "        p.set_opacity_cull_mode(opacity_cull_mode::opaque);\n"
           "    else if ((flags & 128u) != 0u)\n"
           "        p.set_opacity_cull_mode(opacity_cull_mode::non_opaque);\n"
           "    return p;\n"
           "}\n";
}

written write_query(call_context const& ctx)
{
    auto const arg = [&](isize i) { return wrapped(ctx.arguments[i], precedence::primary); };
    auto const& q = ctx.arguments.empty() ? cc::string() : ctx.arguments[0].text;
    auto const is_hlsl = ctx.target == language::hlsl;
    auto const is_msl = ctx.target == language::msl;
    // WGSL traces through the emulated form, which calls none of these
    if (!is_hlsl && !is_msl)
        return {};
    if (ctx.data >= u32(query_op::read_base))
    {
        auto const code = ctx.data - u32(query_op::read_base);
        auto const read = query_read(code / 2);
        auto const is_committed = code % 2 == 1;
        auto const& s = k_reads[isize(read)];
        auto const getter = is_hlsl ? (is_committed ? s.hlsl_committed : s.hlsl_candidate)
                                    : (is_committed ? s.msl_committed : s.msl_candidate);
        // EMIT-260: a helper the dialect declares, and no cluster where the target has none
        if (read == query_read::cluster_id)
            return {.text = is_hlsl ? cc::format("sgl_ray_query_cluster_id({}, {}u)", q, is_committed ? 1 : 0)
                                    : cc::string("(-1)")};
        if (read == query_read::object_to_world_row || read == query_read::world_to_object_row)
        {
            // a row of the 3x4 matrix: HLSL indexes its float3x4, and MSL's float4x3 holds it as its columns' components
            auto const row = arg(1);
            if (is_hlsl)
                return {.text = cc::format("{}.{}()[{}]", q, getter, row)};
            auto const m = cc::format("{}.{}()", q, getter);
            return {.text = cc::format("float4({0}[0][{1}], {0}[1][{1}], {0}[2][{1}], {0}[3][{1}])", m, row)};
        }
        if (s.type == "int")
            return {.text = cc::format("int({}.{}())", q, getter)};
        return {.text = cc::format("{}.{}()", q, getter)};
    }
    switch (query_op(ctx.data))
    {
    case query_op::begin:
        // declared by the local it initializes: `RayQuery<…> q;`, which `declares_only` makes of a `let`
        return {};
    case query_op::trace:
    {
        // (q, world, origin, direction, t_min, t_max, flags, mask, time)
        auto const desc = ctx.mint.is_valid() ? ctx.mint("ray_desc") : cc::string("ray_desc");
        auto result = written();
        if (is_hlsl)
        {
            result.lines.push_back(cc::format("RayDesc {};", desc));
            result.lines.push_back(cc::format("{}.Origin = {};", desc, ctx.arguments[2].text));
            result.lines.push_back(cc::format("{}.TMin = {};", desc, ctx.arguments[4].text));
            result.lines.push_back(cc::format("{}.Direction = {};", desc, ctx.arguments[3].text));
            result.lines.push_back(cc::format("{}.TMax = {};", desc, ctx.arguments[5].text));
            result.text = cc::format("{}.TraceRayInline({}, uint({}), uint({}), {})", q, ctx.arguments[1].text,
                                     ctx.arguments[6].text, ctx.arguments[7].text, desc);
            return result;
        }
        if (is_msl)
        {
            result.lines.push_back(cc::format("ray {}({}, {}, {}, {});", desc, ctx.arguments[2].text,
                                              ctx.arguments[3].text, ctx.arguments[4].text, ctx.arguments[5].text));
            result.text = cc::format("{}.reset({}, {}, uint({}), sgl_intersection_params(uint({})))", q, desc,
                                     ctx.arguments[1].text, ctx.arguments[7].text, ctx.arguments[6].text);
            return result;
        }
        return {};
    }
    case query_op::proceed:
        return {.text = is_hlsl ? cc::format("{}.Proceed()", q) : cc::format("{}.next()", q)};
    case query_op::candidate_is_triangle:
        return {.text = is_hlsl ? cc::format("{}.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE", q)
                                : cc::format("{}.get_candidate_intersection_type() == intersection_type::triangle", q),
                .binds = precedence::comparison};
    case query_op::commit_triangle:
        return {.text = is_hlsl ? cc::format("{}.CommitNonOpaqueTriangleHit()", q)
                                : cc::format("{}.commit_triangle_intersection()", q)};
    case query_op::abort:
        return {.text = is_hlsl ? cc::format("{}.Abort()", q) : cc::format("{}.abort()", q)};
    case query_op::committed_kind:
        return {.text = is_hlsl ? cc::format("int({}.CommittedStatus())", q)
                                : cc::format("int({}.get_committed_intersection_type())", q)};
    case query_op::commit_procedural:
        return {.text = is_hlsl ? cc::format("{}.CommitProceduralPrimitiveHit({})", q, ctx.arguments[1].text)
                                : cc::format("{}.commit_bounding_box_intersection({})", q, ctx.arguments[1].text)};
    case query_op::candidate_is_opaque_box:
        return {.text = is_hlsl ? cc::format("(!{}.CandidateProceduralPrimitiveNonOpaque())", q)
                                : cc::format("(!{}.is_candidate_non_opaque_bounding_box())", q),
                .binds = precedence::unary};
    default:
        return {};
    }
}

/// What a ray-tracing stage reads of its launch, its ray and its hit, as `data`: a position in `k_stage_reads`.
struct stage_read
{
    cc::string_view signature;
    cc::string_view hlsl;
    evaluator evaluate;
    /// A field of the hit record or of the context every metal function of a pipeline is handed (EMIT-139).
    cc::string_view msl = {};
    /// The launch's size, which every invocation of the launch shares; every other read differs between them (CHK-283).
    bool is_shared = false;
};

constexpr stage_read k_stage_reads[] = {
    {"@stages(.raygen, .miss, .closest_hit, .any_hit, .intersection, .callable) fun launch_index() -> int3",
     "int3(DispatchRaysIndex())", nothing, "int3(sgl_ctx.launch_id)"},
    {"@stages(.raygen, .miss, .closest_hit, .any_hit, .intersection, .callable) fun launch_dimensions() -> int3",
     "int3(DispatchRaysDimensions())", nothing, "int3(sgl_ctx.launch_size)", true},
    {"@stages(.miss, .closest_hit, .any_hit, .intersection) fun world_ray_origin() -> pos3", "WorldRayOrigin()",
     nothing, "sgl_hit.origin"},
    {"@stages(.miss, .closest_hit, .any_hit, .intersection) fun world_ray_direction() -> vec3", "WorldRayDirection()",
     nothing, "sgl_hit.direction"},
    {"@stages(.miss, .closest_hit, .any_hit, .intersection) fun ray_t_min() -> float", "RayTMin()", zero_float,
     "sgl_hit.t_min"},
    {"@stages(.miss, .closest_hit, .any_hit, .intersection) fun ray_t_current() -> float", "RayTCurrent()", zero_float,
     "sgl_hit.t"},
    {"@stages(.closest_hit, .any_hit, .intersection) fun hit_instance_id() -> int", "int(InstanceID())", zero_int,
     "int(sgl_hit.instance_id)"},
    {"@stages(.closest_hit, .any_hit, .intersection) fun hit_instance_index() -> int", "int(InstanceIndex())", zero_int,
     "int(sgl_hit.instance_index)"},
    {"@stages(.closest_hit, .any_hit, .intersection) fun hit_geometry_index() -> int", "int(GeometryIndex())", zero_int,
     "int(sgl_hit.geometry_index)"},
    {"@stages(.closest_hit, .any_hit, .intersection) fun hit_primitive_index() -> int", "int(PrimitiveIndex())",
     zero_int, "int(sgl_hit.primitive_index)"},
    {"@stages(.closest_hit, .any_hit) fun hit_barycentrics() -> float2", "sgl_attributes.barycentrics", zero_float2,
     "sgl_hit.barycentrics"},
    {"@stages(.closest_hit, .any_hit) fun hit_is_front_face() -> bool", "(HitKind() == HIT_KIND_TRIANGLE_FRONT_FACE)",
     no_bool, "(sgl_hit.front_face != 0u)"},
    {"@stages(.closest_hit, .any_hit, .intersection) fun hit_object_to_world_row(row: int) -> float4",
     "ObjectToWorld3x4()[{}]", zero_float4,
     "float4(sgl_hit.object_to_world[0][{}], sgl_hit.object_to_world[1][{}], sgl_hit.object_to_world[2][{}], "
     "sgl_hit.object_to_world[3][{}])"},
    {"@stages(.closest_hit, .any_hit, .intersection) fun hit_world_to_object_row(row: int) -> float4",
     "WorldToObject3x4()[{}]", zero_float4,
     "float4(sgl_hit.world_to_object[0][{}], sgl_hit.world_to_object[1][{}], sgl_hit.world_to_object[2][{}], "
     "sgl_hit.world_to_object[3][{}])"},
    {"@stages(.closest_hit, .any_hit, .intersection) fun hit_object_ray_origin() -> float3", "ObjectRayOrigin()",
     zero_float3, "sgl_hit.object_origin"},
    {"@stages(.closest_hit, .any_hit, .intersection) fun hit_object_ray_direction() -> float3", "ObjectRayDirection()",
     zero_float3, "sgl_hit.object_direction"},
    // EMIT-260: a helper the HLSL dialect declares per API, and no cluster on metal
    {"@stages(.closest_hit, .any_hit) fun hit_cluster_id() -> int", "sgl_hit_cluster_id()", no_cluster, "(-1)"},
};

constexpr cc::string_view k_stage_reads_hlsl[] = {
    "sgl_hit_cluster_id",
    "sgl_cluster_id_input",
    "ClusterID",
    "ObjectRayOrigin",
    "ObjectRayDirection",
    "ReportHit",
    "DispatchRaysIndex",
    "DispatchRaysDimensions",
    "WorldRayOrigin",
    "WorldRayDirection",
    "RayTMin",
    "RayTCurrent",
    "InstanceID",
    "InstanceIndex",
    "GeometryIndex",
    "PrimitiveIndex",
    "sgl_attributes",
    "HitKind",
    "HIT_KIND_TRIANGLE_FRONT_FACE",
    "ObjectToWorld3x4",
    "WorldToObject3x4",
};

written write_stage_read(call_context const& ctx)
{
    // the pipeline has no WGSL form
    if (ctx.target != language::hlsl && ctx.target != language::msl)
        return {};
    auto const& entry = k_stage_reads[ctx.data];
    auto const read = stage_read{.hlsl = ctx.target == language::msl ? entry.msl : entry.hlsl};
    if (ctx.arguments.empty())
        return {.text = cc::string(read.hlsl)};
    // a row of a matrix: the argument stands wherever the spelling holds `{}`
    auto text = cc::string();
    auto rest = read.hlsl;
    for (auto hole = rest.find("{}"); hole >= 0; hole = rest.find("{}"))
    {
        text += rest.subview({.offset = 0, .size = hole});
        text += ctx.arguments[0].text;
        rest = rest.subview({.offset = hole + 2, .size = rest.size() - hole - 2});
    }
    text += rest;
    return {.text = cc::move(text)};
}

/// `sg_acceleration_roots[k / 4][k % 4]`: the root of the entry point's k-th acceleration member, which only the
/// emulated form reads (the internal doc libs/graphics/shaped-graphics-language/docs/raytracing-polyfill.md).
written write_polyfill(call_context const& ctx)
{
    if (ctx.data == 0)
    {
        // the member itself has no WGSL binding: flatten appended its position, which is all the roots are indexed by
        auto const k = wrapped(ctx.arguments[ctx.arguments.size() - 1], precedence::multiplicative);
        return {.text = cc::format("sg_acceleration_roots[{0} / 4][{0} % 4]", k)};
    }
    return {.text = cc::format("sg_acceleration_pool[{}]", ctx.arguments[0].text)};
}

/// A ray-tracing stage's trace: (world, origin, direction, t_min, t_max, flags, mask, contribution, multiplier, miss,
/// payload).
/// EMIT-139: metal's trace is the kernel's own intersector, then a miss or a closest hit called through sg's tables, the
/// closest hit's record found from the instance's offset as DXR finds it, in the offsets of the structure it traced.
written write_trace_ray_msl(call_context const& ctx)
{
    auto const& a = ctx.arguments;
    // empty only in the registry's own checks, which write no program; the emitter always names the world's offsets
    auto const offsets
        = ctx.hit_offsets.empty() ? cc::string_view("sgl_ctx.hit_offsets->groups[0][0].instances") : ctx.hit_offsets;
    auto const mint = [&](cc::string_view name) { return ctx.mint.is_valid() ? ctx.mint(name) : cc::string(name); };
    auto const data = mint("sgl_d");
    auto const traversal = mint("sgl_i");
    auto const r = mint("sgl_r");
    auto const found = mint("sgl_h");
    auto const record = mint("sgl_rec");
    auto result = written();
    auto& lines = result.lines;
    lines.push_back(cc::format("sgl_ray_data {};", data));
    lines.push_back(cc::format("sgl_store({}.payload, {});", data, a[a.size() - 1].text));
    lines.push_back(cc::format("{}.world_origin = {};", data, a[1].text));
    lines.push_back(cc::format("{}.world_direction = {};", data, a[2].text));
    lines.push_back(cc::format("{}.context = sgl_ctx;", data));
    lines.push_back(
        cc::format("intersector<triangle_data, instancing, world_space_data" SGL_MSL_MOTION_TAGS "> {};", traversal));
    lines.push_back(cc::format("sgl_configure({}, uint({}));", traversal, a[5].text));
    lines.push_back(cc::format("ray {}({}, {}, {}, {});", r, a[1].text, a[2].text, a[3].text, a[4].text));
    // The ray's time, just ahead of the payload, places what moves; an intersector without the motion tags takes none.
    // A member is the plain structure an inline query takes, which Metal's query has no motion form of; the same
    // resource id read as the motion form is what the intersector takes.
    lines.push_back(cc::format("auto const {} = {}.intersect({}, reinterpret_cast<constant acceleration_structure<"
                               "instancing" SGL_MSL_MOTION_TAGS "> const&>({}), uint({}) sgl_motion_time({}), "
                               "sgl_t.hit_{}, {});",
                               found, traversal, r, a[0].text, a[6].text, a[a.size() - 2].text, a[7].text, data));
    lines.push_back(cc::format("sgl_hit_record {} = sgl_record_of({}, {}, {});", record, found, r, data));
    lines.push_back(cc::format("if ({}.type == intersection_type::none)", found));
    lines.push_back(cc::format("    sgl_t.miss[uint({})]({}.payload, {}, sgl_ctx);", a[9].text, data, record));
    lines.push_back(cc::format("else if ((uint({}) & 8u) == 0u)", a[5].text));
    lines.push_back(cc::format("    sgl_t.closest_hit[{5}[{0}.instance_id] + {0}.geometry_id * "
                               "uint({1}) + "
                               "uint({2})]({3}.payload, {4}, sgl_ctx);",
                               found, a[8].text, a[7].text, data, record, offsets));
    result.text = cc::format("sgl_load({}, {}.payload)", a[a.size() - 1].text, data);
    return result;
}

/// The intersector's options from DXR's ray flags, and the hit record a closest hit or a miss is handed.
cc::string msl_trace_helper(helper_context const& ctx)
{
    if (ctx.target != language::msl)
        return {};
    return "void sgl_configure(thread intersector<triangle_data, instancing, world_space_data" SGL_MSL_MOTION_TAGS
           ">& i, uint flags)\n"
           "{\n"
           "    i.accept_any_intersection((flags & 4u) != 0u);\n"
           "    if ((flags & 1u) != 0u)\n"
           "        i.force_opacity(forced_opacity::opaque);\n"
           "    else if ((flags & 2u) != 0u)\n"
           "        i.force_opacity(forced_opacity::non_opaque);\n"
           "    if ((flags & 16u) != 0u)\n"
           "        i.set_triangle_cull_mode(triangle_cull_mode::back);\n"
           "    else if ((flags & 32u) != 0u)\n"
           "        i.set_triangle_cull_mode(triangle_cull_mode::front);\n"
           "    if ((flags & 64u) != 0u)\n"
           "        i.set_opacity_cull_mode(opacity_cull_mode::opaque);\n"
           "    else if ((flags & 128u) != 0u)\n"
           "        i.set_opacity_cull_mode(opacity_cull_mode::non_opaque);\n"
           "    if ((flags & 256u) != 0u)\n"
           "        i.assume_geometry_type(geometry_type::bounding_box);\n"
           "    else if ((flags & 512u) != 0u)\n"
           "        i.assume_geometry_type(geometry_type::triangle);\n"
           "}\n"
           "\n"
           "sgl_hit_record sgl_record_of(intersection_result<triangle_data, instancing, "
           "world_space_data" SGL_MSL_MOTION_TAGS "> h, ray r, "
           "thread sgl_ray_data const& d)\n"
           "{\n"
           "    sgl_hit_record rec;\n"
           "    rec.t = h.type == intersection_type::none ? r.max_distance : h.distance;\n"
           "    rec.t_min = r.min_distance;\n"
           "    rec.origin = r.origin;\n"
           "    rec.direction = r.direction;\n"
           "    rec.instance_id = h.user_instance_id;\n"
           "    rec.instance_index = h.instance_id;\n"
           "    rec.geometry_index = h.geometry_id;\n"
           "    rec.primitive_index = h.primitive_id;\n"
           "    rec.barycentrics = h.triangle_barycentric_coord;\n"
           "    rec.front_face = h.triangle_front_facing ? 1u : 0u;\n"
           "    rec.object_to_world = float4x3(float3(1, 0, 0), float3(0, 1, 0), float3(0, 0, 1), float3(0));\n"
           "    rec.world_to_object = rec.object_to_world;\n"
           "    if (h.type != intersection_type::none)\n"
           "    {\n"
           "        rec.object_to_world = h.object_to_world_transform;\n"
           "        rec.world_to_object = h.world_to_object_transform;\n"
           "    }\n"
           "    rec.object_origin = rec.world_to_object * float4(r.origin, 1.0f);\n"
           "    rec.object_direction = rec.world_to_object * float4(r.direction, 0.0f);\n"
           "    rec.attributes[0] = d.attributes[0];\n"
           "    rec.attributes[1] = d.attributes[1];\n"
           "    return rec;\n"
           "}\n";
}

/// The `RayDesc` a pipeline trace hands DXR, from its origin, direction, t_min and t_max (arguments 1 to 4), as lines.
void ray_desc_lines(call_context const& ctx, cc::string_view desc, written& result)
{
    result.lines.push_back(cc::format("RayDesc {};", desc));
    result.lines.push_back(cc::format("{}.Origin = {};", desc, ctx.arguments[1].text));
    result.lines.push_back(cc::format("{}.TMin = {};", desc, ctx.arguments[3].text));
    result.lines.push_back(cc::format("{}.Direction = {};", desc, ctx.arguments[2].text));
    result.lines.push_back(cc::format("{}.TMax = {};", desc, ctx.arguments[4].text));
}

written write_trace_ray(call_context const& ctx)
{
    if (ctx.target == language::msl)
        return write_trace_ray_msl(ctx);
    if (ctx.target != language::hlsl)
        return {};
    auto const desc = ctx.mint.is_valid() ? ctx.mint("ray_desc") : cc::string("ray_desc");
    auto result = written();
    ray_desc_lines(ctx, desc, result);
    result.text = cc::format("TraceRay({}, uint({}), uint({}), {}, {}, {}, {}, {})", ctx.arguments[0].text,
                             ctx.arguments[5].text, ctx.arguments[6].text, ctx.arguments[7].text, ctx.arguments[8].text,
                             ctx.arguments[9].text, desc, ctx.arguments[ctx.arguments.size() - 1].text);
    return result;
}

// ---- EMIT-225: vulkan's hit objects, as SPV_EXT_shader_invocation_reorder's instructions ----
//
// DXC lowers none of HLSL's reordering to SPIR-V, so vulkan's text declares each instruction it uses as a function of
// DXC's inline SPIR-V, and spells a hit object as SPIR-V's own type.
// An instruction that hands a payload over takes a variable of the ray-payload storage class, as a trace does.
// So each payload type has one such variable, which the call fills before and reads back after.

/// OpTypeHitObjectEXT.
constexpr cc::string_view k_spirv_hit_object = "vk::SpirvOpaqueType<5313>";
/// The extension and the capability, ShaderInvocationReorderEXT, that each declared instruction needs.
constexpr cc::string_view k_spirv_reorder = "[[vk::ext_extension(\"SPV_EXT_shader_invocation_reorder\")]] "
                                            "[[vk::ext_capability(5388)]]";
/// A trace's operands past the hit object, as `OpHitObjectTraceRayEXT` and its kin take them.
constexpr cc::string_view k_spirv_trace_operands
    = "RaytracingAccelerationStructure world, uint flags, uint mask, uint contribution, uint multiplier, uint miss, "
      "float3 origin, float t_min, float3 direction, float t_max";

/// The variable of the ray-payload storage class that hands a payload of type `payload` over.
cc::string spirv_payload_name(cc::string_view payload)
{
    auto name = cc::string("sgl_hit_payload_");
    for (auto const c : payload)
        name += (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ? c : '_';
    return name;
}

/// What vulkan's text declares for the instructions that take a payload of the call's last argument's type.
cc::string spirv_payload_helper(helper_context const& h)
{
    if (h.target != language::hlsl || !h.is_vulkan || h.argument_types.empty())
        return {};
    auto const& payload = h.argument_types[h.argument_types.size() - 1];
    auto const reference = cc::format("[[vk::ext_reference]] {} h", k_spirv_hit_object);
    return cc::format("[[vk::ext_storage_class(/* RayPayloadKHR */ 5338)]] static {0} {1};\n"
                      "{2} [[vk::ext_instruction(/* OpHitObjectTraceRayEXT */ 5316)]]\n"
                      "void sgl_hit_object_trace_ray({3}, {4}, [[vk::ext_reference]] {0} payload);\n"
                      "{2} [[vk::ext_instruction(/* OpHitObjectExecuteShaderEXT */ 5319)]]\n"
                      "void sgl_hit_object_execute({3}, [[vk::ext_reference]] {0} payload);\n"
                      "{2} [[vk::ext_instruction(/* OpHitObjectTraceReorderExecuteEXT */ 5311)]]\n"
                      "void sgl_hit_object_trace_reorder_execute({3}, {4}, [[vk::ext_reference]] {0} payload);\n"
                      "{2} [[vk::ext_instruction(/* OpHitObjectTraceReorderExecuteEXT */ 5311)]]\n"
                      "void sgl_hit_object_trace_reorder_execute({3}, {4}, [[vk::ext_reference]] {0} payload, uint "
                      "hint, uint bits);\n",
                      payload, spirv_payload_name(payload), k_spirv_reorder, reference, k_spirv_trace_operands);
}

/// A trace into a hit object on vulkan: the local `hit` it fills, and the payload handed over in the variable of its
/// type; `call` is the instruction and `tail` what follows the payload.
void spirv_trace_lines(call_context const& ctx,
                       cc::string_view call,
                       cc::string_view hit,
                       cc::string_view tail,
                       written& result)
{
    auto const payload_index = ctx.argument_types.size() - 1;
    auto const global = spirv_payload_name(ctx.argument_types[payload_index]);
    auto const payload = ctx.arguments[payload_index].text;
    result.lines.push_back(cc::format("{} {};", k_spirv_hit_object, hit));
    result.lines.push_back(cc::format("{} = {};", global, payload));
    result.lines.push_back(
        cc::format("{}({}, {}, uint({}), uint({}), uint({}), uint({}), uint({}), {}, {}, {}, {}, {}{});", call, hit,
                   ctx.arguments[0].text, ctx.arguments[5].text, ctx.arguments[6].text, ctx.arguments[7].text,
                   ctx.arguments[8].text, ctx.arguments[9].text, ctx.arguments[1].text, ctx.arguments[3].text,
                   ctx.arguments[2].text, ctx.arguments[4].text, global, tail));
}

/// The payload read back from the variable of its type, as the call's own statement; it is embedded nowhere.
written spirv_payload_read_back(call_context const& ctx, cc::string_view payload)
{
    auto const& type = ctx.argument_types[ctx.argument_types.size() - 1];
    return {.text = cc::format("{} = {}", payload, spirv_payload_name(type)), .binds = precedence::bitwise};
}

/// `trace_ray_reordered`: the trace as a hit object, the invocations regrouped by it, and its shaders run from it.
/// `data` 1 adds a coherence hint and its bits, the arguments past the miss index.
written write_trace_ray_reordered(call_context const& ctx)
{
    if (ctx.target != language::hlsl)
        return {};
    if (ctx.is_vulkan)
    {
        auto const hit = ctx.mint.is_valid() ? ctx.mint("hit_object") : cc::string("hit_object");
        auto const tail = ctx.data == 1
                            ? cc::format(", uint({}), uint({})", ctx.arguments[10].text, ctx.arguments[11].text)
                            : cc::string();
        auto lines = written();
        spirv_trace_lines(ctx, "sgl_hit_object_trace_reorder_execute", hit, tail, lines);
        auto result = spirv_payload_read_back(ctx, ctx.arguments[ctx.arguments.size() - 1].text);
        result.lines = cc::move(lines.lines);
        return result;
    }
    auto const desc = ctx.mint.is_valid() ? ctx.mint("ray_desc") : cc::string("ray_desc");
    auto const hit = ctx.mint.is_valid() ? ctx.mint("hit_object") : cc::string("hit_object");
    auto const payload = ctx.arguments[ctx.arguments.size() - 1].text;
    auto result = written();
    ray_desc_lines(ctx, desc, result);
    // EMIT-263: NVAPI's hit object, which it invokes by tracing it again through the same structure
    if (ctx.reorders_through_nvapi)
    {
        result.lines.push_back(
            cc::format("NvHitObject {} = NvTraceRayHitObject({}, uint({}), uint({}), {}, {}, {}, {}, "
                       "{});",
                       hit, ctx.arguments[0].text, ctx.arguments[5].text, ctx.arguments[6].text, ctx.arguments[7].text,
                       ctx.arguments[8].text, ctx.arguments[9].text, desc, payload));
        if (ctx.data == 1)
            result.lines.push_back(cc::format("NvReorderThread({}, uint({}), uint({}));", hit, ctx.arguments[10].text,
                                              ctx.arguments[11].text));
        else
            result.lines.push_back(cc::format("NvReorderThread({});", hit));
        result.text = cc::format("NvInvokeHitObject({}, {}, {})", ctx.arguments[0].text, hit, payload);
        return result;
    }
    result.lines.push_back(
        cc::format("dx::HitObject {} = dx::HitObject::TraceRay({}, uint({}), uint({}), {}, {}, {}, {}, {});", hit,
                   ctx.arguments[0].text, ctx.arguments[5].text, ctx.arguments[6].text, ctx.arguments[7].text,
                   ctx.arguments[8].text, ctx.arguments[9].text, desc, payload));
    if (ctx.data == 1)
        result.lines.push_back(cc::format("dx::MaybeReorderThread({}, uint({}), uint({}));", hit,
                                          ctx.arguments[10].text, ctx.arguments[11].text));
    else
        result.lines.push_back(cc::format("dx::MaybeReorderThread({});", hit));
    result.text = cc::format("dx::HitObject::Invoke({}, {})", hit, payload);
    return result;
}

/// `trace_hit_ray`: the trace as a hit object, whose shaders run only when it is invoked (CHK-434).
written write_trace_hit_ray(call_context const& ctx)
{
    if (ctx.target != language::hlsl)
        return {};
    if (ctx.is_vulkan)
    {
        auto const hit = ctx.mint.is_valid() ? ctx.mint("hit_object") : cc::string("hit_object");
        auto result = written();
        spirv_trace_lines(ctx, "sgl_hit_object_trace_ray", hit, "", result);
        auto const back = spirv_payload_read_back(ctx, ctx.arguments[ctx.arguments.size() - 1].text);
        result.lines.push_back(cc::format("{};", back.text));
        result.text = hit;
        return result;
    }
    auto const desc = ctx.mint.is_valid() ? ctx.mint("ray_desc") : cc::string("ray_desc");
    auto result = written();
    ray_desc_lines(ctx, desc, result);
    result.text
        = cc::format("{}({}, uint({}), uint({}), {}, {}, {}, {}, {})",
                     ctx.reorders_through_nvapi ? "NvTraceRayHitObject" : "dx::HitObject::TraceRay",
                     ctx.arguments[0].text, ctx.arguments[5].text, ctx.arguments[6].text, ctx.arguments[7].text,
                     ctx.arguments[8].text, ctx.arguments[9].text, desc, ctx.arguments[ctx.arguments.size() - 1].text);
    return result;
}

/// What a hit object answers before its shaders run, by `data`, and HLSL's member that answers it.
struct hit_object_read
{
    cc::string_view name;
    cc::string_view type;
    evaluator evaluate;
    cc::string_view hlsl;
    /// SPV_EXT_shader_invocation_reorder's instruction, as vulkan declares it (EMIT-225).
    cc::string_view spirv;
    int opcode;
    cc::string_view doc;
};
constexpr hit_object_read k_hit_object_reads[] = {
    {"is_hit", "bool", no_bool, "IsHit", "OpHitObjectIsHitEXT", 5351, "/// Whether the trace hit a primitive."},
    {"is_miss", "bool", no_bool, "IsMiss", "OpHitObjectIsMissEXT", 5352,
     "/// Whether the trace left the scene, so invoking it runs the miss."},
    {"t", "float", zero_float, "GetRayTCurrent", "OpHitObjectGetRayTMaxEXT", 5333,
     "/// How far along the ray the hit is, or the ray's t_max where it missed."},
    {"instance_id", "int", zero_int, "GetInstanceID", "OpHitObjectGetInstanceCustomIndexEXT", 5326,
     "/// The instance's own id, as the TLAS gives it."},
    {"instance_index", "int", zero_int, "GetInstanceIndex", "OpHitObjectGetInstanceIdEXT", 5325,
     "/// The instance's position in the TLAS."},
    {"geometry_index", "int", zero_int, "GetGeometryIndex", "OpHitObjectGetGeometryIndexEXT", 5324,
     "/// The geometry's position in its BLAS."},
    {"primitive_index", "int", zero_int, "GetPrimitiveIndex", "OpHitObjectGetPrimitiveIndexEXT", 5323,
     "/// The primitive's position in its geometry."},
};

/// What vulkan's text declares for the instructions that read a hit object or regroup invocations (EMIT-225).
cc::string spirv_hit_object_helper(helper_context const& h)
{
    if (h.target != language::hlsl || !h.is_vulkan)
        return {};
    auto const reference = cc::format("[[vk::ext_reference]] {} h", k_spirv_hit_object);
    auto text = cc::string();
    for (auto const& read : k_hit_object_reads)
        text += cc::format("{} [[vk::ext_instruction(/* {} */ {})]]\n{} sgl_hit_object_{}({});\n", k_spirv_reorder,
                           read.spirv, read.opcode, read.type == "int" ? cc::string_view("uint") : read.type, read.name,
                           reference);
    text += cc::format("{} [[vk::ext_instruction(/* OpReorderThreadWithHitObjectEXT */ 5315)]]\n"
                       "void sgl_reorder_thread({});\n",
                       k_spirv_reorder, reference);
    text += cc::format("{} [[vk::ext_instruction(/* OpReorderThreadWithHitObjectEXT */ 5315)]]\n"
                       "void sgl_reorder_thread({}, uint hint, uint bits);\n",
                       k_spirv_reorder, reference);
    text += cc::format("{} [[vk::ext_instruction(/* OpReorderThreadWithHintEXT */ 5314)]]\n"
                       "void sgl_reorder_thread(uint hint, uint bits);\n",
                       k_spirv_reorder);
    return text;
}

written write_hit_object_read(call_context const& ctx)
{
    if (ctx.target != language::hlsl)
        return {};
    auto const& read = k_hit_object_reads[ctx.data];
    if (ctx.is_vulkan && read.type == "int")
        return {.text = cc::format("int(sgl_hit_object_{}({}))", read.name, ctx.arguments[0].text)};
    if (ctx.is_vulkan)
        return {.text = cc::format("sgl_hit_object_{}({})", read.name, ctx.arguments[0].text)};
    // EMIT-263: NVAPI reads the hit's distance as its ray's t_max, as vulkan's instruction does
    if (ctx.reorders_through_nvapi && read.name == "t")
        return {.text = cc::format("{}.GetRayDesc().TMax", ctx.arguments[0].text)};
    if (read.type == "int")
        return {.text = cc::format("int({}.{}())", ctx.arguments[0].text, read.hlsl)};
    return {.text = cc::format("{}.{}()", ctx.arguments[0].text, read.hlsl)};
}

/// `hit_object_reorder`, `data` 0, or by a hint, 1; `hit_object_invoke` is 2; `reorder_thread_by_hint` is 3.
written write_hit_object_step(call_context const& ctx)
{
    if (ctx.target != language::hlsl)
        return {};
    auto const reorder = ctx.is_vulkan              ? cc::string_view("sgl_reorder_thread")
                       : ctx.reorders_through_nvapi ? cc::string_view("NvReorderThread")
                                                    : cc::string_view("dx::MaybeReorderThread");
    if (ctx.data == 3)
        return {.text = cc::format("{}(uint({}), uint({}))", reorder, ctx.arguments[0].text, ctx.arguments[1].text)};
    if (ctx.is_vulkan && ctx.data == 2)
    {
        auto const global = spirv_payload_name(ctx.argument_types[1]);
        auto result = spirv_payload_read_back(ctx, ctx.arguments[1].text);
        result.lines.push_back(cc::format("{} = {};", global, ctx.arguments[1].text));
        result.lines.push_back(cc::format("sgl_hit_object_execute({}, {});", ctx.arguments[0].text, global));
        return result;
    }
    if (ctx.is_vulkan)
        return {.text = ctx.data == 1 ? cc::format("sgl_reorder_thread({}, uint({}), uint({}))", ctx.arguments[0].text,
                                                   ctx.arguments[1].text, ctx.arguments[2].text)
                                      : cc::format("sgl_reorder_thread({})", ctx.arguments[0].text)};
    // EMIT-263: NVAPI invokes through the structure the hit object's trace took, which the emit found for it
    if (ctx.data == 2 && ctx.reorders_through_nvapi)
        return {.text = cc::format("NvInvokeHitObject({}, {}, {})", ctx.invoked_structure, ctx.arguments[0].text,
                                   ctx.arguments[1].text)};
    if (ctx.data == 2)
        return {.text = cc::format("dx::HitObject::Invoke({}, {})", ctx.arguments[0].text, ctx.arguments[1].text)};
    if (ctx.data == 1)
        return {.text = cc::format("{}({}, uint({}), uint({}))", reorder, ctx.arguments[0].text, ctx.arguments[1].text,
                                   ctx.arguments[2].text)};
    return {.text = cc::format("{}({})", reorder, ctx.arguments[0].text)};
}

constexpr cc::string_view k_trace_ray_hlsl[] = {"RayDesc", "TraceRay"};
constexpr cc::string_view k_hit_object_hlsl[] = {
    "RayDesc",
    "vk",
    "SpirvOpaqueType",
    "dx",
    "HitObject",
    "TraceRay",
    "Invoke",
    "MaybeReorderThread",
    "IsHit",
    "IsMiss",
    "GetRayTCurrent",
    "GetInstanceID",
    "GetInstanceIndex",
    "GetGeometryIndex",
    "GetPrimitiveIndex",
    // the nvapi fill's, and the counter UAV `nvHLSLExtns.h` declares (EMIT-263)
    "NvHitObject",
    "NvTraceRayHitObject",
    "NvReorderThread",
    "NvInvokeHitObject",
    "GetRayDesc",
    "TMax",
    "g_NvidiaExt",
};
constexpr cc::string_view k_trace_ray_reordered_hlsl[]
    = {"RayDesc",         "dx",          "HitObject",        "TraceRay",    "MaybeReorderThread",
       "Invoke",          "vk",          "SpirvOpaqueType",  "NvHitObject", "NvTraceRayHitObject",
       "NvReorderThread", "g_NvidiaExt", "NvInvokeHitObject"};
constexpr cc::string_view k_trace_ray_msl[] = {
    "sgl_configure",
    "sgl_record_of",
    "sgl_motion_time",
    "intersector",
    "sgl_ctx",
    "sgl_t",
    "sgl_hit",
    "sgl_store",
    "sgl_load",
    "sgl_d",
    "sgl_i",
    "sgl_r",
    "sgl_h",
    "sgl_rec",
    "forced_opacity",
    "geometry_type",
    "intersection_result",
    "intersection_type",
    "opacity_cull_mode",
    "triangle_cull_mode",
};

/// A call of callable `index` of the pipeline's callable section: (index, parameter).
written write_call_callable(call_context const& ctx)
{
    // EMIT-139: metal's callable table, whose functions take the parameter as words
    if (ctx.target == language::msl)
        return {.text = cc::format("sgl_t.callable[uint({})](reinterpret_cast<thread uint4*>(&{}), sgl_ctx)",
                                   ctx.arguments[0].text, ctx.arguments[1].text)};
    if (ctx.target != language::hlsl)
        return {};
    return {.text = cc::format("CallShader(uint({}), {})", ctx.arguments[0].text, ctx.arguments[1].text)};
}

constexpr cc::string_view k_call_callable_hlsl[] = {"CallShader"};

spelling query_spelling(query_op op)
{
    return {.kind = spelling_kind::custom,
            .custom = write_query,
            .helper = op == query_op::trace ? msl_params_helper : nullptr,
            .writes_lines = op == query_op::trace ? is_not_wgsl : nullptr,
            .data = u32(op),
            .hlsl_names = k_query_hlsl,
            .msl_names = k_query_msl};
}

/// `RayQuery<…>` around `flags`, each by DXR's name, which is `ray_flags`' own name in capitals behind `RAY_FLAG_`.
cc::string hlsl_ray_query_type(u32 flags)
{
    constexpr cc::string_view names[] = {
        "FORCE_OPAQUE",
        "FORCE_NON_OPAQUE",
        "ACCEPT_FIRST_HIT_AND_END_SEARCH",
        "SKIP_CLOSEST_HIT_SHADER",
        "CULL_BACK_FACING_TRIANGLES",
        "CULL_FRONT_FACING_TRIANGLES",
        "CULL_OPAQUE",
        "CULL_NON_OPAQUE",
        "SKIP_TRIANGLES",
        "SKIP_PROCEDURAL_PRIMITIVES",
        "FORCE_OMM_2_STATE",
    };
    auto text = cc::string();
    auto bit = 0;
    for (auto const name : names)
        if ((flags >> bit++) & 1u)
            text.appendf("{}RAY_FLAG_{}", text.empty() ? "" : " | ", name);
    return cc::format("RayQuery<{}>", text.empty() ? cc::string("RAY_FLAG_NONE") : text);
}
} // namespace

void sgl::builtins::register_raytracing(registry& r)
{
    r.add_comment("// Ray tracing: what a ray is traced with, and what a trace reads.\n"
                  "// A shader calls `trace` of raytracing.sgl, never these: each is a step of HLSL's `RayQuery` and "
                  "MSL's `intersection_query`.");

    // DXR's ray flags, whose values HLSL and Vulkan take as they are and Metal reads off one by one (CHK-321).
    r.add(type_record{
        .declaration = "@bitflags enum ray_flags:\n"
                       "    none = 0\n"
                       "    force_opaque = 1\n"
                       "    force_non_opaque = 2\n"
                       "    accept_first_hit_and_end_search = 4\n"
                       "    skip_closest_hit = 8\n"
                       "    cull_back_facing = 16\n"
                       "    cull_front_facing = 32\n"
                       "    cull_opaque = 64\n"
                       "    cull_non_opaque = 128\n"
                       "    skip_triangles = 256\n"
                       "    skip_procedural = 512\n"
                       "    force_omm_2_state = 1024",
        .doc = "/// What a trace may skip, cull or decide without asking: DXR's `RAY_FLAG_*` without the prefix.",
        .hlsl = "int",
        .wgsl = "i32",
        .msl = "int",
        .hlsl_layout = {.size = 4, .alignment = 4},
        .wgsl_layout = {.size = 4, .alignment = 4},
        .msl_layout = {.size = 4, .alignment = 4},
        .leaf_kind = value_kind::scalar_int,
        .leaf_count = 1,
    });

    add_operator(r, "|", "combine_ray_flags", "ray_flags", "ray_flags", "ray_flags", bit_or,
                 {.kind = spelling_kind::infix, .text = "|", .binds = precedence::bitwise});
    add_operator(r, "&", "intersect_ray_flags", "ray_flags", "ray_flags", "ray_flags", bit_and,
                 {.kind = spelling_kind::infix, .text = "&", .binds = precedence::bitwise});
    r.add(function_record{
        .signature = "@pure fun has(flags: ray_flags, f: ray_flags) -> bool",
        .doc = "/// Whether every flag of `f` is set in `flags`.",
        .evaluate = has_flags,
        .write = {.kind = spelling_kind::custom, .custom = write_has},
    });

    r.add(type_record{
        .declaration = "struct ray_query",
        .doc = "/// One trace in flight, which only raytracing.sgl steps through.",
        .hlsl = "RayQuery<RAY_FLAG_NONE>",
        // Metal's query takes no motion tags, so an inline trace sees the scene as it stands
        .msl = "intersection_query<triangle_data, instancing>",
        // the template's flags are ORed with the trace's, and a driver may specialize on them
        .hlsl_flagged = hlsl_ray_query_type,
    });

    auto const add = [&](cc::string signature, evaluator evaluate, query_op op, cc::string_view doc)
    {
        // what a query finds depends on the ray it traced, and on what the shader committed and in which control flow
        auto const advances = op == query_op::trace || op == query_op::proceed || op == query_op::commit_triangle
                           || op == query_op::abort || op == query_op::commit_procedural;
        r.add(function_record{
            .signature = cc::format("@internal {}", signature),
            .doc = cc::string(doc),
            .evaluate = evaluate,
            .write = query_spelling(op),
            .is_ray_query = true,
            .writes_through = advances ? written_argument::first : written_argument::none,
            .features = check::feature_set(check::feature::ray_query),
            .declares_only = op == query_op::begin,
            .flags_argument = op == query_op::trace ? 6 : -1,
            .reads_only = !advances && op != query_op::begin,
        });
    };
    add("fun ray_query_begin() -> ray_query", nothing, query_op::begin, "/// A query nothing traced yet.");
    for (auto const geometry : {"triangles", "procedural", "mixed"})
        add(cc::format("fun ray_query_trace(q: ray_query, world: acceleration_structure[.{}], origin: pos3, "
                       "direction: vec3, t_min: float, t_max: float, flags: ray_flags, mask: int, time: float)",
                       geometry),
            nothing, query_op::trace, "/// Starts the trace of a ray.");
    add("fun ray_query_proceed(q: ray_query) -> bool", no_bool, query_op::proceed,
        "/// Traverses to the next candidate the shader decides, and false once there is none.");
    add("fun ray_query_candidate_is_triangle(q: ray_query) -> bool", no_bool, query_op::candidate_is_triangle,
        "/// Whether the candidate is a non-opaque triangle rather than a procedural primitive's box.");
    add("fun ray_query_commit_triangle(q: ray_query)", nothing, query_op::commit_triangle,
        "/// Accepts the candidate triangle.");
    add("fun ray_query_abort(q: ray_query)", nothing, query_op::abort,
        "/// Ends the traversal, keeping what it committed.");
    add("fun ray_query_committed_kind(q: ray_query) -> int", zero_int, query_op::committed_kind,
        "/// 0 for nothing, 1 for a triangle, 2 for a procedural primitive.");
    add("fun ray_query_commit_procedural(q: ray_query, t: float)", nothing, query_op::commit_procedural,
        "/// Accepts the candidate box's primitive, hit at `t`.");
    add("fun ray_query_candidate_is_opaque_box(q: ray_query) -> bool", no_bool, query_op::candidate_is_opaque_box,
        "/// Whether the candidate box's geometry is opaque, after the instance's and the ray's forcing.");

    for (auto read = u32(0); read < u32(sizeof(k_reads) / sizeof(k_reads[0])); ++read)
        for (auto const is_committed : {false, true})
        {
            auto const& s = k_reads[read];
            auto const is_row = query_read(read) == query_read::object_to_world_row
                             || query_read(read) == query_read::world_to_object_row;
            auto const name = cc::format("ray_query_{}_{}", is_committed ? "committed" : "candidate", s.name);
            add(cc::format("fun {}(q: ray_query{}) -> {}", name, is_row ? ", row: int" : "", s.type), s.evaluate,
                query_op(u32(query_op::read_base) + read * 2 + (is_committed ? 1 : 0)),
                is_committed ? "/// Of the committed hit." : "/// Of the candidate.");
        }

    r.add_comment("// What a ray-tracing stage reads of its launch, its ray and its hit, which raytracing.sgl hands "
                  "its "
                  "entry point.");
    for (auto i = u32(0); i < u32(sizeof(k_stage_reads) / sizeof(k_stage_reads[0])); ++i)
        r.add(function_record{
            .signature = cc::format("@internal {}", k_stage_reads[i].signature),
            .evaluate = k_stage_reads[i].evaluate,
            .write
            = {.kind = spelling_kind::custom, .custom = write_stage_read, .data = i, .hlsl_names = k_stage_reads_hlsl},
            .differs_between_invocations = !k_stage_reads[i].is_shared,
            .features = check::feature_set(check::feature::raytracing_pipeline),
        });

    r.add_comment("// A ray-tracing stage's trace, which a `trace` naming a ray type of its set lowers to (CHK-329).");
    for (auto const geometry : {"triangles", "procedural", "mixed"})
        r.add(function_record{
            .signature = cc::format(
                "@internal @stages(.raygen, .closest_hit, .miss) fun trace_ray(world: acceleration_structure[.{}], "
                "origin: pos3, direction: vec3, t_min: float, t_max: float, flags: ray_flags, mask: int, "
                "contribution: int, multiplier: int, miss: int)",
                geometry),
            .doc = "/// Traces a ray through the pipeline's tables, handing the payload over past the signature.",
            .evaluate = nothing,
            .write = {.kind = spelling_kind::custom,
                      .custom = write_trace_ray,
                      .helper = msl_trace_helper,
                      .writes_lines = is_not_wgsl,
                      .hlsl_names = k_trace_ray_hlsl,
                      .msl_names = k_trace_ray_msl},
            .writes_through = written_argument::element,
            .takes_element = true,
            .features = check::feature_set(check::feature::raytracing_pipeline),
            .takes_ray_time = true,
        });

    r.add_comment("// A trace of a ray type with `reorder` where the device has shader execution reordering "
                  "(CHK-433).");
    for (auto const is_hinted : {false, true})
        for (auto const geometry : {"triangles", "procedural", "mixed"})
            r.add(function_record{
                .signature = cc::format("@internal @stages(.raygen) fun {}(world: acceleration_structure[.{}], origin: "
                                        "pos3, direction: vec3, "
                                        "t_min: float, t_max: float, flags: ray_flags, mask: int, contribution: int, "
                                        "multiplier: int, "
                                        "miss: int{})",
                                        is_hinted ? "trace_ray_reordered_by_hint" : "trace_ray_reordered", geometry,
                                        is_hinted ? ", hint: uint, bits: int" : ""),
                .doc = "/// Traces as `trace_ray` does, regrouping the invocations by the hit before its shaders run.",
                .evaluate = nothing,
                .write = {.kind = spelling_kind::custom,
                          .custom = write_trace_ray_reordered,
                          .helper = spirv_payload_helper,
                          .writes_lines = is_not_wgsl,
                          .data = is_hinted ? 1u : 0u,
                          .hlsl_names = k_trace_ray_reordered_hlsl},
                .writes_through = written_argument::element,
                .takes_element = true,
                // shader execution reordering too, which the trace that chose it records as the device's grant
                .features = check::feature_set(check::feature::raytracing_pipeline),
                .takes_ray_time = true,
            });

    r.add_comment("// A hit object: a trace whose shaders have not run, which only a device with shader execution "
                  "reordering has (CHK-434).");
    auto const ser = check::feature_set(check::feature::raytracing_pipeline)
                   | check::feature_set(check::feature::shader_execution_reordering);
    r.add(type_record{
        .declaration = "struct hit_object",
        .doc = "/// A trace whose shaders have not run yet: `let h = trace_hit(...)`, then `h.invoke(mut p)`.",
        .hlsl = "dx::HitObject",
        .hlsl_vulkan = cc::string(k_spirv_hit_object),
        .hlsl_nvapi = "NvHitObject",
        .features = ser,
        .local_only_reason = "it is a trace in flight, which only its invocation completes",
    });
    for (auto const geometry : {"triangles", "procedural", "mixed"})
        r.add(function_record{
            .signature = cc::format("@internal @stages(.raygen) fun trace_hit_ray(world: acceleration_structure[.{}], "
                                    "origin: pos3, direction: vec3, t_min: float, t_max: float, flags: ray_flags, "
                                    "mask: int, contribution: int, multiplier: int, miss: int) -> hit_object",
                                    geometry),
            .doc = "/// Traces as `trace_ray` does, running the any-hit and intersection shaders and no other.",
            .evaluate = nothing,
            .write = {.kind = spelling_kind::custom,
                      .custom = write_trace_hit_ray,
                      .helper = spirv_payload_helper,
                      .writes_lines = is_not_wgsl,
                      .hlsl_names = k_hit_object_hlsl},
            .writes_through = written_argument::element,
            .takes_element = true,
            .features = ser,
            .takes_ray_time = true,
        });
    for (auto i = u32(0); i < u32(sizeof(k_hit_object_reads) / sizeof(k_hit_object_reads[0])); ++i)
        r.add(function_record{
            .signature = cc::format("@internal @pure @stages(.raygen) fun hit_object_{}(h: hit_object) -> {}",
                                    k_hit_object_reads[i].name, k_hit_object_reads[i].type),
            .doc = cc::string(k_hit_object_reads[i].doc),
            .evaluate = k_hit_object_reads[i].evaluate,
            .write = {.kind = spelling_kind::custom,
                      .custom = write_hit_object_read,
                      .helper = spirv_hit_object_helper,
                      .data = i,
                      .hlsl_names = k_hit_object_hlsl},
            .features = ser,
        });
    r.add(function_record{
        .signature = "@internal @stages(.raygen) fun hit_object_reorder(h: hit_object)",
        .doc = "/// Regroups the invocations by what their hit objects run.",
        .evaluate = nothing,
        .write = {.kind = spelling_kind::custom,
                  .custom = write_hit_object_step,
                  .helper = spirv_hit_object_helper,
                  .hlsl_names = k_hit_object_hlsl},
        .features = ser,
    });
    r.add(function_record{
        .signature = "@internal @stages(.raygen) fun hit_object_reorder_by_hint(h: hit_object, hint: uint, bits: int)",
        .doc = "/// Regroups the invocations by what their hit objects run, and by the low `bits` bits of `hint`.",
        .evaluate = nothing,
        .write = {.kind = spelling_kind::custom,
                  .custom = write_hit_object_step,
                  .helper = spirv_hit_object_helper,
                  .data = 1,
                  .hlsl_names = k_hit_object_hlsl},
        .features = ser,
    });
    r.add(function_record{
        .signature = "@internal @stages(.raygen) fun hit_object_invoke(h: hit_object)",
        .doc = "/// Runs the closest hit or the miss the hit object holds, handing the payload over past the "
               "signature.",
        .evaluate = nothing,
        .write = {.kind = spelling_kind::custom,
                  .custom = write_hit_object_step,
                  .helper = spirv_payload_helper,
                  .writes_lines = is_hlsl,
                  .data = 2,
                  .hlsl_names = k_hit_object_hlsl},
        .writes_through = written_argument::element,
        .takes_element = true,
        .features = ser,
    });
    r.add(function_record{
        .signature = "@internal @stages(.raygen) fun reorder_thread_by_hint(hint: uint, bits: int)",
        .doc = "/// Regroups the invocations by the low `bits` bits of `hint`, which `maybe_reorder` writes.",
        .evaluate = nothing,
        .write = {.kind = spelling_kind::custom,
                  .custom = write_hit_object_step,
                  .helper = spirv_hit_object_helper,
                  .data = 3,
                  .hlsl_names = k_hit_object_hlsl},
        .features = ser,
    });

    r.add_comment("// A call of a callable, which `table[i](mut p)` lowers to with the table's place in the section "
                  "added "
                  "(CHK-344).");
    r.add(function_record{
        .signature = "@internal @stages(.raygen, .closest_hit, .miss, .callable) fun call_callable(index: int)",
        .doc = "/// Calls the callable at `index` of the pipeline's callable section, handing the parameter over past "
               "the "
               "signature.",
        .evaluate = nothing,
        .write = {.kind = spelling_kind::custom, .custom = write_call_callable, .hlsl_names = k_call_callable_hlsl},
        .writes_through = written_argument::element,
        .takes_element = true,
        .features = check::feature_set(check::feature::raytracing_pipeline),
    });

    r.add_comment("// The emulated trace's view of sg's acceleration pool, which WGSL alone writes (the internal doc "
                  "libs/graphics/shaped-graphics-language/docs/raytracing-polyfill.md).");
    for (auto const geometry : {"triangles", "procedural", "mixed"})
        r.add(function_record{
            .signature
            = cc::format("@internal @pure fun acceleration_root(world: acceleration_structure[.{}]) -> uint", geometry),
            .doc = "/// The pool unit of the TLAS bound as `world`, which the emulated trace starts at.",
            .evaluate = zero_uint,
            .write = {.kind = spelling_kind::custom, .custom = write_polyfill, .data = 0, .wgsl_names = k_polyfill_wgsl},
            .features = check::feature_set(check::feature::ray_query),
            .takes_acceleration_index = true,
        });
    r.add(function_record{
        .signature = "@internal @pure fun acceleration_pool_load(unit: uint) -> uint4",
        .doc = "/// One unit of the pool.",
        .evaluate = zero_uint4,
        .write = {.kind = spelling_kind::custom, .custom = write_polyfill, .data = 1, .wgsl_names = k_polyfill_wgsl},
        .features = check::feature_set(check::feature::ray_query),
    });
}
