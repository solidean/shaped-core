// The pixel stage of a shaped-core raster material, as its shading-language compiler wrote it for DXC, reduced.
// Reduced mechanically: right-hand sides replaced by constants, then everything nothing reads removed, each step kept only if it still reproduced.
// So no variable is ever read uninitialized; what is left is load-bearing as a whole, mostly the BSDF evaluation in the light loop.
// The structs the structured buffers hold keep their full layout, so each view's stride matches what the shader declares.
// NO_LIGHT_LOOP or NO_AMBIENT each makes a restored pipeline draw again, though the light loop never runs (light_count is 0).
// NONCE only changes the bytecode, so a fresh nonce is a shader the driver has not cached.

struct camera_record
{
    float3 position;
    float pad0;
    float3 forward;
    float pad1;
    float3 right_scaled;
    float pad2;
    float3 up_scaled;
    float pad3;
};

struct varyings
{
    float4 position : SV_Position;
    nointerpolation uint instance : SGL0;
    nointerpolation uint primitive : SGL1;
    float3 barycentrics : SGL2;
    float3 world : SGL3;
};

struct targets
{
    float4 color : SV_Target0;
    float4 normal : SV_Target1;
    float depth : SV_Target2;
    float4 albedo : SV_Target3;
    float4 specular_albedo : SV_Target4;
    float roughness : SV_Target5;
    float2 motion : SV_Target6;
    float primary_depth : SV_Target7;
};

struct instance_record
{
    uint param_buffer;
    uint param_offset;
    uint vertices;
    uint indices;
    uint is_indexed;
    uint link_mask;
    uint opacity_kind;
    uint opacity_offset;
    uint opacity_uv_offset;
    uint opacity_frequency;
    uint opacity_channel;
    uint opacity_sampler;
    uint opacity_transform;
    uint opacity_cutoff;
    uint shade_group;
    uint pad;
    float4 to_world_x;
    float4 to_world_y;
    float4 to_world_z;
};

struct instance
{
    uint param_buffer;
    uint param_offset;
    uint vertices;
    uint is_indexed;
    float4 to_world_x;
    float4 to_world_y;
    float4 to_world_z;
};

struct shading_context
{
    uint param_buffer;
    uint param_offset;
};

struct surface
{
    float base_weight;
    float3 base_color;
    float base_metalness;
    float3 specular_color;
    float transmission_weight;
    float coat_weight;
    float3 coat_color;
    float coat_roughness_anisotropy;
    float coat_ior;
    float coat_darkening;
    float fuzz_weight;
    float3 fuzz_color;
    float fuzz_roughness;
    float thin_film_weight;
    float thin_film_ior;
    float3 geometry_normal;
    float3 geometry_coat_normal;
    float3 geometry_tangent;
};

struct frame_constants
{
    camera_record camera;
    camera_record previous_camera;
    float near_plane;
    uint light_count;
    uint decal_count;
    float pixel_spread;
    float2 size;
    float2 pad0;
};

struct frame
{
    float3 t;
    float3 b;
    float3 n;
};

struct bsdf
{
    float3 diffuse_albedo;
    float3 metal_f0;
    float3 spec_f0;
    float spec_weight;
    float trans_weight;
    float3 trans_tint;
    float thin_walled;
    float sss_weight;
    float coat_f0;
    float2 coat_alpha;
    float3 coat_n;
    float3 coat_t;
    float3 coat_b;
    float3 coat_tint;
    float coat_weight;
    float coat_darkening;
    float3 fuzz_color;
    float fuzz_alpha;
    float fuzz_weight;
    float metalness;
    float thin_film_weight;
    float thin_film_ior;
    float3 emission;
};

struct light_record
{
    float3 position;
    uint path;
    float3 u;
    float area;
    float3 v;
    uint flags;
    float3 emission;
    float cone_scale;
    float3 normal;
    float cone_offset;
    float one_minus_cos_angular_radius;
    uint link_mask;
    float2 pad0;
};

struct light
{
    float3 position;
    uint path;
    float3 u;
    float3 v;
    float3 emission;
    float3 normal;
};

StructuredBuffer<frame_constants> drawn_frame : register(t0, space0);
StructuredBuffer<instance_record> drawn_instances : register(t1, space0);
StructuredBuffer<light_record> drawn_lights : register(t2, space0);

ByteAddressBuffer bindless_buffers[4096] : register(t4656, space1);


targets sv_raster(varyings p)
{
    // A value no run reaches, there only to make each nonce a shader the driver has never compiled.
    if (p.instance == NONCE)
        discard;
    const instance_record splat = drawn_instances[int(p.instance)];
    instance inst;
    inst.param_buffer = splat.param_buffer;
    inst.param_offset = splat.param_offset;
    inst.vertices = 1u;
    inst.is_indexed = 1u;
    inst.to_world_x = (float4)0.5;
    inst.to_world_y = (float4)0.5;
    inst.to_world_z = (float4)0.5;
    if (inst.is_indexed != 0u)
    {
        const uint x = 1u;
        const uint y = 1u;
        const uint z = 1u;
    }
    shading_context ctx;
    ctx.param_buffer = inst.param_buffer;
    ctx.param_offset = inst.param_offset;
    const float base_weight = 0.5;
    const int block_1 = 1;
    const float3 base_color = asfloat(bindless_buffers[block_1].Load3(ctx.param_offset + 4u));
    const int block_2 = 1;
    const float base_metalness = asfloat(bindless_buffers[block_2].Load(ctx.param_offset + 16u));
    const int block_5 = 1;
    const float3 specular_color = asfloat(bindless_buffers[block_5].Load3(ctx.param_offset + 28u));
    const float transmission_weight = 0.5;
    const int block_21 = 1;
    const float coat_weight = asfloat(bindless_buffers[block_21].Load(ctx.param_offset + 132u));
    const int block_22 = 1;
    const float3 coat_color = asfloat(bindless_buffers[block_22].Load3(ctx.param_offset + 136u));
    const int block_24 = 1;
    const float coat_roughness_anisotropy = asfloat(bindless_buffers[block_24].Load(ctx.param_offset + 152u));
    const int block_25 = 1;
    const float coat_ior = asfloat(bindless_buffers[block_25].Load(ctx.param_offset + 156u));
    const float coat_darkening = 0.5;
    const int block_27 = 1;
    const float fuzz_weight = asfloat(bindless_buffers[block_27].Load(ctx.param_offset + 164u));
    const int block_28 = 1;
    const float3 fuzz_color = asfloat(bindless_buffers[block_28].Load3(ctx.param_offset + 168u));
    const int block_29 = 1;
    const float fuzz_roughness = asfloat(bindless_buffers[block_29].Load(ctx.param_offset + 180u));
    const int block_30 = 1;
    const float thin_film_weight = asfloat(bindless_buffers[block_30].Load(ctx.param_offset + 184u));
    const int block_32 = 1;
    const float thin_film_ior = asfloat(bindless_buffers[block_32].Load(ctx.param_offset + 192u));
    const float3 normal = (float3)0.5;
    const float thin_walled = 0.5;
    const float3 geometry_normal = (float3)0.5;
    const float3 geometry_coat_normal = (float3)0.5;
    const float3 geometry_tangent = (float3)0.5;
    surface surface_1;
    surface_1.base_weight = 0.5;
    surface_1.base_color = (float3)0.5;
    surface_1.base_metalness = 0.5;
    surface_1.specular_color = (float3)0.5;
    surface_1.transmission_weight = 0.5;
    surface_1.coat_weight = 0.5;
    surface_1.coat_color = (float3)0.5;
    surface_1.coat_roughness_anisotropy = 0.5;
    surface_1.coat_ior = 0.5;
    surface_1.coat_darkening = 0.5;
    surface_1.fuzz_weight = 0.5;
    surface_1.fuzz_color = (float3)0.5;
    surface_1.fuzz_roughness = 0.5;
    surface_1.thin_film_weight = 0.5;
    surface_1.thin_film_ior = 0.5;
    surface_1.geometry_normal = (float3)0.5;
    surface_1.geometry_coat_normal = (float3)0.5;
    surface_1.geometry_tangent = (float3)0.5;
    surface_1.base_weight = 0.5;
    surface_1.base_color = saturate(base_color);
    surface_1.base_metalness = saturate(base_metalness);
    surface_1.specular_color = saturate(specular_color);
    surface_1.transmission_weight = 0.5;
    surface_1.coat_weight = saturate(coat_weight);
    surface_1.coat_color = saturate(coat_color);
    surface_1.coat_roughness_anisotropy = saturate(coat_roughness_anisotropy);
    surface_1.coat_ior = max(1.0, coat_ior);
    surface_1.coat_darkening = 0.5;
    surface_1.fuzz_weight = saturate(fuzz_weight);
    surface_1.fuzz_color = saturate(fuzz_color);
    surface_1.fuzz_roughness = saturate(fuzz_roughness);
    surface_1.thin_film_weight = saturate(thin_film_weight);
    surface_1.thin_film_ior = max(1.0, thin_film_ior);
    surface_1.geometry_normal = (float3)0.5;
    surface_1.geometry_coat_normal = (float3)0.5;
    surface_1.geometry_tangent = (float3)0.5;
    const surface surface_in = surface_1;
    const int block_44 = int(ctx.param_buffer);
    const float occlusion_1 = asfloat(bindless_buffers[block_44].Load(ctx.param_offset + 232u));
    const frame_constants f = drawn_frame[0];
    const instance_record splat_2 = drawn_instances[int(p.instance)];
    instance inst_1;
    inst_1.param_buffer = 1u;
    inst_1.param_offset = 1u;
    inst_1.vertices = splat_2.vertices;
    inst_1.is_indexed = 1u;
    inst_1.to_world_x = (float4)0.5;
    inst_1.to_world_y = (float4)0.5;
    inst_1.to_world_z = splat_2.to_world_z;
    const uint vertex = 1u;
    const int buffer = int(inst_1.vertices);
    const float3 p_1 = asfloat(bindless_buffers[buffer].Load3(vertex * 12u));
    const float4 q = float4(p_1.x, p_1.y, p_1.z, 1.0);
    const float3 w0 = float3(dot(inst_1.to_world_x, q), dot(inst_1.to_world_y, q), dot(inst_1.to_world_z, q));
    const float4 q_1 = (float4)0.5;
    const float3 w1 = float3(dot(inst_1.to_world_x, q_1), dot(inst_1.to_world_y, q_1), dot(inst_1.to_world_z, q_1));
    const float3 w2 = (float3)0.5;
    const float sign_1 = 0.5;
    float3 n = normalize(cross(w1 - w0, w2 - w0)) * sign_1;
    const float3 v = (float3)0.5;
    const bool exiting = false;
    if (exiting)
    {
        n = (float3)0.5;
    }
    surface surface_2 = surface_in;
    const float3 n_2 = n;
    const float3 f_n = normalize(n_2);
    frame f_1;
    f_1.t = (float3)0.5;
    f_1.b = (float3)0.5;
    f_1.n = f_n;
    if (any(surface_2.geometry_normal != float3(0.0, 0.0, 1.0)))
    {
        frame perturb_frame_result;
        const frame f_2 = f_1;
        const float3 w = (float3)0.5;
        const float3 n_3 = normalize(w.x * f_2.t + w.y * f_2.b + w.z * f_2.n);
        const float3 t = (float3)0.5;
        const float len = 0.5;
        if (len < 1.0e-06)
        {
            frame frame_value;
            frame_value.t = (float3)0.5;
            frame_value.b = (float3)0.5;
            frame_value.n = (float3)0.5;
            perturb_frame_result = frame_value;
        }
        else
        {
            const float3 rt = (float3)0.5;
            const float keep = 0.5;
            frame frame_value_1;
            frame_value_1.t = (float3)0.5;
            frame_value_1.b = cross(n_3, rt) * keep;
            frame_value_1.n = n_3;
            perturb_frame_result = frame_value_1;
        }
        f_1 = perturb_frame_result;
    }
    if (any(surface_2.geometry_tangent != float3(1.0, 0.0, 0.0)))
    {
        frame rotate_frame_to_tangent_result;
        const frame f_3 = f_1;
        const float3 t_tangent = (float3)0.5;
        const float3 t_1 = t_tangent.x * f_3.t + t_tangent.y * f_3.b;
        const float len_1 = length(t_1);
        if (len_1 < 1.0e-06)
        {
            rotate_frame_to_tangent_result = f_3;
        }
        else
        {
            const float3 rt_1 = t_1 / len_1;
            frame frame_value_2;
            frame_value_2.t = rt_1;
            frame_value_2.b = (float3)0.5;
            frame_value_2.n = (float3)0.5;
            rotate_frame_to_tangent_result = frame_value_2;
        }
        f_1 = rotate_frame_to_tangent_result;
    }
    if (any(surface_2.geometry_coat_normal != float3(0.0, 0.0, 1.0)))
    {
        const float3 coat_world = (float3)0.5;
        const frame f_4 = f_1;
        surface_2.geometry_coat_normal = normalize(float3(dot(coat_world, f_4.t), dot(coat_world, f_4.b), dot(coat_world, f_4.n)));
    }
    const frame shading = f_1;
    const surface s_1 = surface_2;
    const float metalness = saturate(s_1.base_metalness);
    const float3 diffuse_albedo = saturate(s_1.base_weight) * saturate(s_1.base_color);
    const float3 metal_f0 = saturate(s_1.base_weight * s_1.base_color);
    const float roughness = 0.5;
    const float r0 = 0.5;
    const float3 spec_f0 = saturate(saturate(s_1.specular_color) * (r0 * r0));
    const float spec_weight = 0.5;
    const float3 coat_n = normalize(s_1.geometry_coat_normal);
    const float3 coat_t = (float3)0.5;
    const float3 coat_b = cross(coat_n, coat_t);
    const float trans_weight = 0.5;
    const float3 trans_tint = (float3)0.5;
    const float depth = 0.5;
    const float sss_weight = 0.5;
    const float cior = max(1.0 + 0.001, s_1.coat_ior);
    const float cr0 = (cior - 1.0) / (cior + 1.0);
    const float anisotropy_2 = s_1.coat_roughness_anisotropy;
    const float a_7 = 0.5;
    const float t_4 = 1.0 - saturate(anisotropy_2);
    const float ax_4 = a_7 * sqrt(2.0 / (1.0 + t_4 * t_4));
    bsdf b_3;
    b_3.diffuse_albedo = diffuse_albedo;
    b_3.metal_f0 = metal_f0;
    b_3.spec_f0 = spec_f0;
    b_3.spec_weight = 0.5;
    b_3.trans_weight = 0.5;
    b_3.trans_tint = (float3)0.5;
    b_3.thin_walled = 0.5;
    b_3.sss_weight = 0.5;
    b_3.coat_f0 = cr0 * cr0;
    b_3.coat_alpha = float2(max(0.001, ax_4), max(0.001, t_4 * ax_4));
    b_3.coat_n = coat_n;
    b_3.coat_t = (float3)0.5;
    b_3.coat_b = coat_b;
    b_3.coat_tint = saturate(s_1.coat_color);
    b_3.coat_weight = saturate(s_1.coat_weight);
    b_3.coat_darkening = 0.5;
    b_3.fuzz_color = saturate(s_1.fuzz_color);
    b_3.fuzz_alpha = clamp(s_1.fuzz_roughness, 0.001, 1.0);
    b_3.fuzz_weight = saturate(s_1.fuzz_weight);
    b_3.metalness = metalness;
    b_3.thin_film_weight = saturate(s_1.thin_film_weight);
    b_3.thin_film_ior = max(1.0 + 0.001, s_1.thin_film_ior);
    b_3.emission = (float3)0.5;
    const float3 wo_local = float3(dot(v, shading.t), dot(v, shading.b), dot(v, shading.n));
    float3 color_1 = b_3.emission;
    if (wo_local.z > 0.0)
    {
#ifndef NO_LIGHT_LOOP
        for (int i_3 = 0; i_3 < int(f.light_count); ++i_3)
        {
            float3 direct_light_result;
            const light_record splat_3 = drawn_lights[i_3];
            light l;
            l.position = splat_3.position;
            l.path = splat_3.path;
            l.u = splat_3.u;
            l.v = splat_3.v;
            l.emission = (float3)0.5;
            l.normal = (float3)0.5;
            const float3 p_5 = p.world;
            if (l.path == 1u)
            {
                float3 sum = (float3)0.0;
                const float cells = 0.5;
                for (int i_4 = 0; i_4 < 4; ++i_4)
                {
                    for (int j = 0; j < 4; ++j)
                    {
                        const float s_2 = 0.5;
                        const float t_5 = (float(j) + 0.5) / cells * 2.0 - 1.0;
                        const float3 to_light = l.position + s_2 * l.u + t_5 * l.v - p_5;
                        const float dist2 = 0.5;
                        const float3 wi = to_light / sqrt(dist2);
                        const float cos_light = 0.5;
                        if (cos_light <= 0.0)
                        {
                            continue;
                        }
                        float3 reflected_result;
                        const float3 incoming = (float3)0.5;
                        const float3 wi_local = float3(dot(wi, shading.t), dot(wi, shading.b), dot(wi, shading.n));
                        if (wi_local.z <= 0.0)
                        {
                            reflected_result = (float3)0.5;
                        }
                        else
                        {
                            float3 diffuse = (float3)0.0;
                            float3 specular = (float3)0.0;
                            diffuse = (float3)0.5;
                            specular = (float3)0.5;
                            if (!(wo_local.z <= 0.0))
                            {
                                if (wi_local.z < 0.0)
                                {
                                    const float3 tint = (float3)0.5;
                                    if (!all(tint <= (float3)0.0))
                                    {
                                        float3 transmission_btdf_result;
                                        if (b_3.thin_walled != 0.0)
                                        {
                                            const float3 wi_mirror = (float3)0.5;
                                            const float dm = 0.5;
                                            const float gm = 0.5;
                                            const float3 f0 = b_3.spec_f0;
                                            const float t_7 = 0.5;
                                            transmission_btdf_result = saturate((float3)1.0 - b_3.spec_weight * (f0 + ((float3)1.0 - f0) * t_7)) * (dm * gm / (4.0 * wo_local.z * max(wi_mirror.z, 1.0e-06)));
                                        }
                                        else
                                        {
                                            const float dot_o = 0.5;
                                            const float dot_i = 0.5;
                                            if (dot_o <= 0.0 || dot_i >= 0.0)
                                            {
                                                transmission_btdf_result = (float3)0.5;
                                            }
                                            else
                                            {
                                                transmission_btdf_result = (float3)0.5;
                                            }
                                        }
                                        const float3 f_btdf = transmission_btdf_result;
                                        const float t_coat_x = 0.5;
                                        const float mu_2 = 0.5;
                                        const float alpha_8 = 0.5;
                                        const float mu_3 = abs(wi_local.z);
                                        const float alpha_9 = 0.5;
                                        const float t_fuzz_x = (1.0 - b_3.fuzz_weight * (0.5 * alpha_8 * pow(saturate(1.0 - mu_2), 3.0))) * (1.0 - b_3.fuzz_weight * (0.5 * alpha_9 * pow(saturate(1.0 - mu_3), 3.0)));
                                        const float3 coat_absorption_x = (float3)0.5;
                                        const float3 crossing = f_btdf * t_coat_x * coat_absorption_x * t_fuzz_x * (1.0 - b_3.metalness);
                                        specular = crossing * b_3.trans_weight * b_3.trans_tint;
                                        diffuse = crossing * (1.0 - b_3.trans_weight) * b_3.sss_weight;
                                    }
                                }
                                else if (!(wi_local.z <= 0.0))
                                {
                                    const float mu_o_1 = 0.5;
                                    const float mu_i_1 = 0.5;
                                    bool and_result = b_3.thin_walled == 0.0;
                                    if (and_result)
                                    {
                                        and_result = false;
                                    }
                                    const float roughness_3 = 0.5;
                                    if (roughness_3 <= 0.0)
                                    {
                                    }
                                    else
                                    {
                                        const float2 to = (float2)0.5;
                                    }
                                    const float3 f_base_specular = (float3)0.5;
                                    const float3 f_base_diffuse = (float3)0.5;
                                    const float3 wo_c_1 = float3(dot(wo_local, b_3.coat_t), dot(wo_local, b_3.coat_b), dot(wo_local, b_3.coat_n));
                                    const float3 wi_c = float3(dot(wi_local, b_3.coat_t), dot(wi_local, b_3.coat_b), dot(wi_local, b_3.coat_n));
                                    float3 f_coat = (float3)0.0;
                                    float t_coat = 1.0;
                                    if (wo_c_1.z > 0.0 && wi_c.z > 0.0)
                                    {
                                        const float3 h_c = normalize(wo_c_1 + wi_c);
                                        const float mu_hc = max(dot(wo_c_1, h_c), 1.0e-06);
                                        const float2 alpha_20 = b_3.coat_alpha;
                                        const float3 v_4 = float3(h_c.x / alpha_20.x, h_c.y / alpha_20.y, h_c.z);
                                        const float t_14 = dot(v_4, v_4);
                                        const float d_coat = 1.0 / max(3.141592653589793 * alpha_20.x * alpha_20.y * t_14 * t_14, 1.0e-09);
                                        const float2 alpha_21 = b_3.coat_alpha;
                                        const float t2_8 = 0.5;
                                        const float c2_7 = wi_c.z * wi_c.z;
                                        const float t2_9 = (alpha_21.x * alpha_21.x * wi_c.x * wi_c.x + alpha_21.y * alpha_21.y * wi_c.y * wi_c.y) / max(c2_7, 1.0e-09);
                                        const float g_coat = 1.0 / (1.0 + 0.5 * (sqrt(1.0 + t2_8) - 1.0) + 0.5 * (sqrt(1.0 + t2_9) - 1.0));
                                        const float coat_ior_1 = 0.5;
                                        float fresnel_dielectric_result;
                                        const float c_8 = saturate(mu_hc);
                                        const float s2_7 = 0.5;
                                        if (s2_7 >= 1.0)
                                        {
                                            fresnel_dielectric_result = 0.5;
                                        }
                                        else
                                        {
                                            const float ct = 0.5;
                                            const float rs = 0.5;
                                            const float rp = (coat_ior_1 * c_8 - ct) / (coat_ior_1 * c_8 + ct);
                                            fresnel_dielectric_result = 0.5 * (rs * rs + rp * rp);
                                        }
                                        f_coat = (float3)(b_3.coat_weight * fresnel_dielectric_result * (d_coat * g_coat / (4.0 * wo_c_1.z * wi_c.z)));
                                        const float2 ab_6 = (float2)0.5;
                                        const float mu_5 = wi_c.z;
                                        const float2 alpha_24 = b_3.coat_alpha;
                                        const float alpha_25 = sqrt(max(alpha_24.x * alpha_24.y, 1.0e-12));
                                        const float4 c0_7 = float4(-1.0, -0.0275, -0.572, 0.022);
                                        const float4 c1_7 = (float4)0.5;
                                        const float4 r_14 = alpha_25 * c0_7 + c1_7;
                                        const float a004_7 = min(r_14.x * r_14.x, exp2(-9.28 * saturate(mu_5))) * r_14.x + r_14.y;
                                        const float2 ab_7 = float2(a004_7 * -1.04 + r_14.z, a004_7 * 1.04 + r_14.w);
                                        t_coat = (1.0 - b_3.coat_weight * (b_3.coat_f0 * ab_6.x + ab_6.y)) * (1.0 - b_3.coat_weight * (b_3.coat_f0 * ab_7.x + ab_7.y));
                                    }
                                    const float3 coat_absorption = lerp((float3)1.0, b_3.coat_tint, b_3.coat_weight);
                                    float3 coat_darkening_factor_result;
                                    if (b_3.coat_weight <= 0.0)
                                    {
                                        coat_darkening_factor_result = (float3)0.5;
                                    }
                                    else
                                    {
                                        const float f_avg_internal = 0.5;
                                        const float3 base_albedo = saturate(lerp(b_3.diffuse_albedo, b_3.metal_f0, b_3.metalness));
                                        const float3 escaped = (1.0 - f_avg_internal) / max((float3)0.001, (float3)1.0 - f_avg_internal * base_albedo);
                                        coat_darkening_factor_result = lerp((float3)1.0, saturate(escaped), b_3.coat_darkening * b_3.coat_weight);
                                    }
                                    const float3 through_coat = t_coat * coat_absorption * coat_darkening_factor_result;
                                    const float alpha_26 = b_3.fuzz_alpha;
                                    const float inv_a = 1.0 / max(alpha_26, 0.001);
                                    const float s2_8 = 0.5;
                                    const float3 f_fuzz = b_3.fuzz_weight * b_3.fuzz_color * ((2.0 + inv_a) * pow(max(s2_8, 1.0e-06), inv_a * 0.5) / (2.0 * 3.141592653589793)) * (1.0 / max(4.0 * (wo_local.z + wi_local.z - wo_local.z * wi_local.z), 1.0e-06));
                                    const float alpha_27 = 0.5;
                                    const float alpha_28 = b_3.fuzz_alpha;
                                    const float t_fuzz = (1.0 - b_3.fuzz_weight * (0.5 * alpha_27 * pow(saturate(1.0 - mu_o_1), 3.0))) * (1.0 - b_3.fuzz_weight * (0.5 * alpha_28 * pow(saturate(1.0 - mu_i_1), 3.0)));
                                    diffuse = t_fuzz * f_base_diffuse * through_coat;
                                    specular = f_fuzz + t_fuzz * (f_coat + f_base_specular * through_coat);
                                }
                            }
                            reflected_result = (diffuse + specular) * incoming * wi_local.z;
                        }
                        sum = sum + reflected_result;
                    }
                }
                direct_light_result = sum;
            }
            else
            {
                float3 reflected_3_result;
                const float3 incoming_3 = (float3)0.5;
                const float3 wi_local_3 = (float3)0.5;
                if (wi_local_3.z <= 0.0)
                {
                    reflected_3_result = (float3)0.5;
                }
                else
                {
                    float3 diffuse_3 = (float3)0.0;
                    float3 specular_3 = (float3)0.0;
                    diffuse_3 = (float3)0.5;
                    specular_3 = (float3)0.5;
                    if (!(wo_local.z <= 0.0))
                    {
                        if (wi_local_3.z < 0.0)
                        {
                            const float3 tint_6 = (float3)0.5;
                            if (!all(tint_6 <= (float3)0.0))
                            {
                                specular_3 = (float3)0.5;
                                diffuse_3 = (float3)0.5;
                            }
                        }
                        else if (!(wi_local_3.z <= 0.0))
                        {
                            const float mu_o_7 = 0.5;
                            const float mu_i_7 = 0.5;
                            const float mu_h_3 = 0.5;
                            const float denom_7 = 0.5;
                            const float d_spec_3 = 0.5;
                            const float g_spec_3 = 0.5;
                            const float3 f0_35 = b_3.metal_f0;
                            const float3 a_16 = (float3)0.5;
                            const float t_39 = 0.5;
                            float3 f_metal_fresnel_3 = max((float3)0.0, f0_35 + ((float3)1.0 - f0_35) * t_39 - a_16 * mu_h_3 * pow(saturate(1.0 - mu_h_3), 6.0));
                            const float3 f0_36 = b_3.spec_f0;
                            const float t_40 = 0.5;
                            float3 f_spec_fresnel_3 = f0_36 + ((float3)1.0 - f0_36) * t_40;
                            bool and_result_3 = b_3.thin_walled == 0.0;
                            if (and_result_3)
                            {
                                and_result_3 = false;
                            }
                            const bool tir_3 = false;
                            if (tir_3)
                            {
                                f_spec_fresnel_3 = (float3)0.5;
                            }
                            if (b_3.thin_film_weight > 0.0 && !tir_3)
                            {
                                float3 thin_film_reflectance_6_result;
                                const float film_ior_6 = b_3.thin_film_ior;
                                const float thickness_6 = 0.5;
                                const float eta_film_6 = max(1.0 + 0.001, film_ior_6);
                                const float s2_27 = 0.5;
                                if (s2_27 >= 1.0)
                                {
                                    thin_film_reflectance_6_result = (float3)0.5;
                                }
                                else
                                {
                                    const float cos_film_6 = 0.5;
                                    const float3 phase_6 = 4.0 * 3.141592653589793 * eta_film_6 * max(0.0, thickness_6) * cos_film_6 / float3(620.0, 550.0, 450.0);
                                    const float3 num_12 = (float3)0.5;
                                    const float3 den_12 = (float3)0.5;
                                    const float3 r1_13 = (float3)0.5;
                                    const float3 r2_13 = (float3)0.5;
                                    const float3 c_23 = cos(phase_6);
                                    const float3 num_13 = (float3)0.5;
                                    const float3 den_13 = 1.0 + r1_13 * r1_13 * r2_13 * r2_13 + 2.0 * r1_13 * r2_13 * c_23;
                                    thin_film_reflectance_6_result = 0.5 * (saturate(num_12 / max(den_12, (float3)1.0e-06)) + saturate(num_13 / max(den_13, (float3)1.0e-06)));
                                }
                                const float3 film_metal_3 = thin_film_reflectance_6_result;
                                f_metal_fresnel_3 = lerp(f_metal_fresnel_3, film_metal_3, b_3.thin_film_weight);
                                float3 thin_film_reflectance_7_result;
                                const float3 f0_38 = b_3.spec_f0;
                                const float film_ior_7 = b_3.thin_film_ior;
                                const float thickness_7 = 0.5;
                                const float eta_film_7 = max(1.0 + 0.001, film_ior_7);
                                const float s2_30 = (1.0 - mu_h_3 * mu_h_3) / (eta_film_7 * eta_film_7);
                                if (s2_30 >= 1.0)
                                {
                                    thin_film_reflectance_7_result = (float3)0.5;
                                }
                                else
                                {
                                    const float cos_film_7 = sqrt(1.0 - s2_30);
                                    float3 r01_s_7 = (float3)0.0;
                                    float3 r01_p_7 = (float3)0.0;
                                    const float3 eta_11 = float3(eta_film_7, eta_film_7, eta_film_7);
                                    const float3 s2_31 = (1.0 - mu_h_3 * mu_h_3) / max(eta_11 * eta_11, (float3)1.0e-06);
                                    const float3 cos_t_14 = (float3)0.5;
                                    r01_s_7 = (float3)0.5;
                                    r01_p_7 = (eta_11 * mu_h_3 - cos_t_14) / max(abs(eta_11 * mu_h_3 + cos_t_14), (float3)1.0e-06);
                                    const bool3 total_14 = s2_31 >= (float3)1.0;
                                    r01_s_7 = select(total_14, (float3)1.0, r01_s_7);
                                    r01_p_7 = select(total_14, (float3)1.0, r01_p_7);
                                    const float3 root_7 = clamp(sqrt(saturate(f0_38)), (float3)0.0, (float3)0.99);
                                    const float3 eta_base_7 = (1.0 + root_7) / (1.0 - root_7) / eta_film_7;
                                    float3 r12_s_7 = (float3)0.0;
                                    float3 r12_p_7 = (float3)0.0;
                                    const float3 s2_32 = (1.0 - cos_film_7 * cos_film_7) / max(eta_base_7 * eta_base_7, (float3)1.0e-06);
                                    const float3 cos_t_15 = (float3)0.5;
                                    r12_s_7 = (float3)0.5;
                                    r12_p_7 = (eta_base_7 * cos_film_7 - cos_t_15) / max(abs(eta_base_7 * cos_film_7 + cos_t_15), (float3)1.0e-06);
                                    const bool3 total_15 = s2_32 >= (float3)1.0;
                                    r12_s_7 = select(total_15, (float3)1.0, r12_s_7);
                                    r12_p_7 = select(total_15, (float3)1.0, r12_p_7);
                                    const float3 phase_7 = 4.0 * 3.141592653589793 * eta_film_7 * max(0.0, thickness_7) * cos_film_7 / float3(620.0, 550.0, 450.0);
                                    const float3 r1_14 = r01_s_7;
                                    const float3 r2_14 = r12_s_7;
                                    const float3 c_24 = cos(phase_7);
                                    const float3 num_14 = r1_14 * r1_14 + r2_14 * r2_14 + 2.0 * r1_14 * r2_14 * c_24;
                                    const float3 den_14 = 1.0 + r1_14 * r1_14 * r2_14 * r2_14 + 2.0 * r1_14 * r2_14 * c_24;
                                    const float3 r1_15 = r01_p_7;
                                    const float3 r2_15 = r12_p_7;
                                    const float3 c_25 = cos(phase_7);
                                    const float3 num_15 = r1_15 * r1_15 + r2_15 * r2_15 + 2.0 * r1_15 * r2_15 * c_25;
                                    const float3 den_15 = 1.0 + r1_15 * r1_15 * r2_15 * r2_15 + 2.0 * r1_15 * r2_15 * c_25;
                                    thin_film_reflectance_7_result = 0.5 * (saturate(num_14 / max(den_14, (float3)1.0e-06)) + saturate(num_15 / max(den_15, (float3)1.0e-06)));
                                }
                                const float3 film_spec_3 = thin_film_reflectance_7_result;
                                f_spec_fresnel_3 = lerp(f_spec_fresnel_3, film_spec_3, b_3.thin_film_weight);
                            }
                            const float3 f0_39 = b_3.metal_f0;
                            const float alpha_100 = 0.5;
                            const float4 c0_26 = (float4)0.5;
                            const float4 c1_26 = (float4)0.5;
                            const float4 r_33 = alpha_100 * c0_26 + c1_26;
                            const float a004_26 = 0.5;
                            const float2 ab_26 = float2(a004_26 * -1.04 + r_33.z, a004_26 * 1.04 + r_33.w);
                            const float e_ss_6 = ab_26.x + ab_26.y;
                            const float3 f_metal_3 = f_metal_fresnel_3 * (d_spec_3 * g_spec_3 / denom_7) * ((float3)1.0 + f0_39 * (1.0 / max(e_ss_6, 0.001) - 1.0));
                            const float3 f0_40 = (float3)0.5;
                            const float e_ss_7 = 0.5;
                            const float3 f_spec_3 = b_3.spec_weight * f_spec_fresnel_3 * (d_spec_3 * g_spec_3 / denom_7) * ((float3)1.0 + f0_40 * (1.0 / max(e_ss_7, 0.001) - 1.0));
                            const float3 t_spec_3 = (float3)0.5;
                            float oren_nayar_3_result;
                            const float roughness_6 = 0.5;
                            if (roughness_6 <= 0.0)
                            {
                                oren_nayar_3_result = 0.5;
                            }
                            else
                            {
                                oren_nayar_3_result = 0.5;
                            }
                            const float3 f_diffuse_3 = (1.0 - b_3.trans_weight) * (1.0 - b_3.sss_weight) * b_3.diffuse_albedo * (oren_nayar_3_result / 3.141592653589793) * t_spec_3;
                            const float3 f_base_specular_3 = lerp(f_spec_3, f_metal_3, b_3.metalness);
                            const float3 f_base_diffuse_3 = (1.0 - b_3.metalness) * f_diffuse_3;
                            const float3 wo_c_7 = float3(dot(wo_local, b_3.coat_t), dot(wo_local, b_3.coat_b), dot(wo_local, b_3.coat_n));
                            const float3 wi_c_3 = (float3)0.5;
                            float3 f_coat_3 = (float3)0.0;
                            if (wo_c_7.z > 0.0 && wi_c_3.z > 0.0)
                            {
                                f_coat_3 = (float3)0.5;
                            }
                            const float3 through_coat_3 = (float3)0.5;
                            const float3 f_fuzz_3 = (float3)0.5;
                            const float alpha_114 = 0.5;
                            const float alpha_115 = 0.5;
                            const float t_fuzz_3 = (1.0 - b_3.fuzz_weight * (0.5 * alpha_114 * pow(saturate(1.0 - mu_o_7), 3.0))) * (1.0 - b_3.fuzz_weight * (0.5 * alpha_115 * pow(saturate(1.0 - mu_i_7), 3.0)));
                            diffuse_3 = t_fuzz_3 * f_base_diffuse_3 * through_coat_3;
                            specular_3 = f_fuzz_3 + t_fuzz_3 * (f_coat_3 + f_base_specular_3 * through_coat_3);
                        }
                    }
                    reflected_3_result = (diffuse_3 + specular_3) * incoming_3 * wi_local_3.z;
                }
                direct_light_result = reflected_3_result;
            }
            color_1 = color_1 + direct_light_result;
        }
#endif
        const surface surface_3 = surface_2;
        const float3 diffuse_4 = (float3)0.5;
        const float3 specular_4 = (float3)0.5;
        const float3 diffuse_5 = (float3)0.5;
        const float3 dielectric = (float3)0.5;
        const float3 metal = saturate(surface_3.base_weight * surface_3.base_color);
#ifndef NO_AMBIENT
        color_1 = color_1 + (saturate(surface_3.base_weight * diffuse_5 * (1.0 - saturate(surface_3.base_metalness)) * (1.0 - saturate(surface_3.transmission_weight))) * diffuse_4 + saturate(lerp(dielectric, metal, saturate(surface_3.base_metalness))) * specular_4) * occlusion_1;
#endif
    }
    const float2 motion = (float2)0.5;
    targets result;
    result.color = float4(color_1.x, color_1.y, color_1.z, 1.0);
    result.normal = float4(0, 0, 1, 0);
    result.depth = 1.0;
    result.albedo = float4(1, 1, 1, 0);
    result.specular_albedo = float4(0, 0, 0, 0);
    result.roughness = 1.0;
    result.motion = float2(0, 0);
    result.primary_depth = 0.970588;
    return result;
}
