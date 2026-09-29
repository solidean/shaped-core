#include <clean-core/string/format.hh>
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

/// What one query record writes, as `data`.
enum class query_op : u32
{
    begin,
    trace,
    proceed,
    candidate_is_triangle,
    commit_triangle,
    abort,
    committed_kind,
    // the reads, each once of the candidate and once of the committed hit: `read_base + read * 2 + is_committed`
    read_base = 16,
};

enum class query_read : u32
{
    t,
    instance_id,
    instance_index,
    geometry_index,
    primitive_index,
    barycentrics,
    is_front_face,
    object_to_world_row,
    world_to_object_row,
};

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
};

constexpr cc::string_view k_query_hlsl[] = {
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
        // (q, world, origin, direction, t_min, t_max, flags, mask)
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
        return {.text = is_hlsl ? cc::format("({}.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)", q)
                                : cc::format("({}.get_candidate_intersection_type() == intersection_type::triangle)", q),
                .binds = precedence::primary};
    case query_op::commit_triangle:
        return {.text = is_hlsl ? cc::format("{}.CommitNonOpaqueTriangleHit()", q)
                                : cc::format("{}.commit_triangle_intersection()", q)};
    case query_op::abort:
        return {.text = is_hlsl ? cc::format("{}.Abort()", q) : cc::format("{}.abort()", q)};
    case query_op::committed_kind:
        return {.text = is_hlsl ? cc::format("int({}.CommittedStatus())", q)
                                : cc::format("int({}.get_committed_intersection_type())", q)};
    default:
        return {};
    }
}

/// `sg_acceleration_roots[k / 4][k % 4]`: the root of the entry point's k-th acceleration member, which only the
/// emulated form reads (the internal doc `raytracing-polyfill.md`).
written write_polyfill(call_context const& ctx)
{
    if (ctx.data == 0)
    {
        auto const k = wrapped(ctx.arguments[0], precedence::multiplicative);
        return {.text = cc::format("sg_acceleration_roots[{0} / 4][{0} % 4]", k)};
    }
    return {.text = cc::format("sg_acceleration_pool[{}]", ctx.arguments[0].text)};
}

spelling query_spelling(query_op op)
{
    return {.kind = spelling_kind::custom,
            .custom = write_query,
            .helper = op == query_op::trace ? msl_params_helper : nullptr,
            .data = u32(op),
            .hlsl_names = k_query_hlsl,
            .msl_names = k_query_msl};
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
                       "    skip_procedural = 512",
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
        .msl = "intersection_query<triangle_data, instancing>",
    });

    auto const add = [&](cc::string signature, evaluator evaluate, query_op op, cc::string_view doc)
    {
        r.add(function_record{
            .signature = cc::move(signature),
            .doc = cc::string(doc),
            .evaluate = evaluate,
            .write = query_spelling(op),
            .features = check::feature_set(check::feature::ray_query),
            .declares_only = op == query_op::begin,
        });
    };
    add("fun ray_query_begin() -> ray_query", nothing, query_op::begin, "/// A query nothing traced yet.");
    for (auto const geometry : {"triangles", "procedural", "mixed"})
        add(cc::format("fun ray_query_trace(q: ray_query, world: acceleration_structure[.{}], origin: pos3, "
                       "direction: vec3, t_min: float, t_max: float, flags: ray_flags, mask: int)",
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

    r.add_comment("// The emulated trace's view of sg's acceleration pool, which WGSL alone writes (the internal doc "
                  "raytracing-polyfill.md).");
    r.add(function_record{
        .signature = "fun acceleration_root_at(k: int) -> uint",
        .doc = "/// The pool unit of the entry point's k-th acceleration member's TLAS.",
        .evaluate = zero_uint,
        .write = {.kind = spelling_kind::custom, .custom = write_polyfill, .data = 0, .wgsl_names = k_polyfill_wgsl},
        .features = check::feature_set(check::feature::ray_query),
    });
    r.add(function_record{
        .signature = "@pure fun acceleration_pool_load(unit: uint) -> uint4",
        .doc = "/// One unit of the pool.",
        .evaluate = zero_uint4,
        .write = {.kind = spelling_kind::custom, .custom = write_polyfill, .data = 1, .wgsl_names = k_polyfill_wgsl},
        .features = check::feature_set(check::feature::ray_query),
    });
}
