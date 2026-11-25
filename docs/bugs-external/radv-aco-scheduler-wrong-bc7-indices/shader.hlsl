// SGL's BC7 encoder (libs/graphics/shaped-rendering/shaders/compression/bc.sgl, entry bc7_cs) as SGL writes it for vulkan,
// then cut down by hand; the readme lists every cut and which ones make the bug disappear.
// Each 8 x 8 thread is one 4 x 4 block of a 32 x 32 rgba8 image, and encodes it over its own partition, by * 8 + bx,
// trying BC7 mode 1 and then mode 3 and keeping the one with less error.

struct subset_fit
{
    int4 q0;
    int4 q1;
    int p0;
    int p1;
    float error;
};

[[vk::binding(0, 0)]] StructuredBuffer<uint> texels; // 32 x 32, rgba8
[[vk::binding(1, 0)]] RWStructuredBuffer<uint4> bc_io16_blocks;



[numthreads(8, 8, 1)]
void bc7_cs(uint3 id_in : SV_DispatchThreadID)
{
    const int3 id = int3(id_in);
    const int bx = id.x;
    const int by = id.y;
    if (bx >= 8 || by >= 8)
    {
        return;
    }
    const float4 fill = (float4)0.0;
    float4 px[16];
    px[0] = fill;
    px[1] = fill;
    px[2] = fill;
    px[3] = fill;
    px[4] = fill;
    px[5] = fill;
    px[6] = fill;
    px[7] = fill;
    px[8] = fill;
    px[9] = fill;
    px[10] = fill;
    px[11] = fill;
    px[12] = fill;
    px[13] = fill;
    px[14] = fill;
    px[15] = fill;
    bool opaque = true;
    for (int i = 0; i < 16; ++i)
    {
        const uint t = texels[(by * 4 + (i >> 2)) * 32 + bx * 4 + (i & 3)];
        const float4 c = float4(t & 0xFF, (t >> 8) & 0xFF, (t >> 16) & 0xFF, t >> 24) / 255.0;
        px[i] = round(saturate(c) * 255.0);
        if (px[i].w < 255.0)
        {
            opaque = false;
        }
    }
    float best_error = 1.0e30;
    uint4 best = (uint4)0u;
    {
        const int first = by * 8 + bx;
        const int second = first;
        for (int k_2 = 0; k_2 < 1; ++k_2)
        {
            int p_8;
            if (k_2 == 0)
            {
                p_8 = first;
            }
            else
            {
                p_8 = second;
            }
            for (int m = 0; m < 2; ++m)
            {
                if (m == 1 && !opaque)
                {
                    continue;
                }
                int mode;
                if (!opaque)
                {
                    mode = 7;
                }
                else
                {
                    int if_41_result;
                    if (m == 0)
                    {
                        if_41_result = 1;
                    }
                    else
                    {
                        if_41_result = 3;
                    }
                    mode = if_41_result;
                }
                float error_2 = 0.0;
                const float4 px_3[16] = px;
                int bits;
                if (mode == 1)
                {
                    bits = 6;
                }
                else
                {
                    int if_43_result;
                    if (mode == 3)
                    {
                        if_43_result = 7;
                    }
                    else
                    {
                        if_43_result = 5;
                    }
                    bits = if_43_result;
                }
                int pmode_1;
                if (mode == 1)
                {
                    pmode_1 = 1;
                }
                else
                {
                    pmode_1 = 2;
                }
                int index_bits;
                if (mode == 1)
                {
                    index_bits = 3;
                }
                else
                {
                    index_bits = 2;
                }
                const bool alpha_2 = mode == 7;
                int if_46_result;
                if (alpha_2)
                {
                    if_46_result = 4;
                }
                else
                {
                    if_46_result = 3;
                }
                const int channels = if_46_result;
                const int top_12 = (int(1) << index_bits) - 1;
                static const uint t_4[64] = {0xccccu, 0x8888u, 0xeeeeu, 0xecc8u, 0xc880u, 0xfeecu, 0xfec8u, 0xec80u, 0xc800u, 0xffecu, 0xfe80u, 0xe800u, 0xffe8u, 0xff00u, 0xfff0u, 0xf000u, 0xf710u, 0x8eu, 0x7100u, 0x8ceu, 0x8cu, 0x7310u, 0x3100u, 0x8cceu, 0x88cu, 0x3110u, 0x6666u, 0x366cu, 0x17e8u, 0xff0u, 0x718eu, 0x399cu, 0xaaaau, 0xf0f0u, 0x5a5au, 0x33ccu, 0x3c3cu, 0x55aau, 0x9696u, 0xa55au, 0x73ceu, 0x13c8u, 0x324cu, 0x3bdcu, 0x6996u, 0xc33cu, 0x9966u, 0x660u, 0x272u, 0x4e4u, 0x4e40u, 0x2720u, 0xc936u, 0x936cu, 0x39c6u, 0x639cu, 0x9336u, 0x9cc6u, 0x817eu, 0xe718u, 0xccf0u, 0xfccu, 0x7744u, 0xee22u};
                const uint mask_1 = t_4[p_8];
                static const int t_5[64] = {15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2, 8, 2, 2, 8, 8, 15, 2, 8, 2, 2, 8, 8, 2, 2, 15, 15, 6, 8, 2, 8, 15, 15, 2, 8, 2, 2, 2, 15, 15, 6, 6, 2, 6, 8, 15, 15, 2, 2, 15, 15, 15, 15, 15, 2, 2, 15};
                const int anchor = t_5[p_8];
                int idx_1[16];
                idx_1[0] = 0;
                idx_1[1] = 0;
                idx_1[2] = 0;
                idx_1[3] = 0;
                idx_1[4] = 0;
                idx_1[5] = 0;
                idx_1[6] = 0;
                idx_1[7] = 0;
                idx_1[8] = 0;
                idx_1[9] = 0;
                idx_1[10] = 0;
                idx_1[11] = 0;
                idx_1[12] = 0;
                idx_1[13] = 0;
                idx_1[14] = 0;
                idx_1[15] = 0;
                float n_2 = 0.0;
                float4 mean_2 = (float4)0.0;
                for (int i_16 = 0; i_16 < 16; ++i_16)
                {
                    if (((mask_1 >> uint(i_16)) & 1u) != 0u)
                    {
                        continue;
                    }
                    n_2 = n_2 + 1.0;
                    mean_2 = mean_2 + px_3[i_16];
                }
                mean_2 = mean_2 / max(n_2, 1.0);
                float4 c0_4 = (float4)0.0;
                float4 c1_4 = (float4)0.0;
                float4 c2_4 = (float4)0.0;
                float4 c3_4 = (float4)0.0;
                for (int i_17 = 0; i_17 < 16; ++i_17)
                {
                    if (((mask_1 >> uint(i_17)) & 1u) != 0u)
                    {
                        continue;
                    }
                    float4 d_16;
                    if (alpha_2)
                    {
                        d_16 = px_3[i_17] - mean_2;
                    }
                    else
                    {
                        const float4 splat_1 = px_3[i_17] - mean_2;
                        d_16 = float4(splat_1.x, splat_1.y, splat_1.z, 0.0);
                    }
                    c0_4 = c0_4 + d_16 * d_16.x;
                    c1_4 = c1_4 + d_16 * d_16.y;
                    c2_4 = c2_4 + d_16 * d_16.z;
                    c3_4 = c3_4 + d_16 * d_16.w;
                }
                const float4 c0_5 = c0_4;
                const float4 c1_5 = c1_4;
                const float4 c2_5 = c2_4;
                const float4 c3_5 = c3_4;
                float4 axis_5 = c0_5;
                float spread_3 = c0_5.x;
                if (c1_5.y > spread_3)
                {
                    axis_5 = c1_5;
                    spread_3 = c1_5.y;
                }
                if (c2_5.z > spread_3)
                {
                    axis_5 = c2_5;
                    spread_3 = c2_5.z;
                }
                if (c3_5.w > spread_3)
                {
                    axis_5 = c3_5;
                }
                float4 axis_4 = axis_5;
                for (int k_3 = 0; k_3 < 4; ++k_3)
                {
                    const float4 next_2 = c0_4 * axis_4.x + c1_4 * axis_4.y + c2_4 * axis_4.z + c3_4 * axis_4.w;
                    const float len_2 = sqrt(dot(next_2, next_2));
                    if (len_2 > 1.0e-06)
                    {
                        axis_4 = next_2 / len_2;
                    }
                }
                const float axis_len_1 = dot(axis_4, axis_4);
                float tmin_1 = 0.0;
                float tmax_1 = 0.0;
                if (axis_len_1 > 1.0e-12)
                {
                    tmin_1 = 1.0e+30;
                    tmax_1 = -1.0e+30;
                    for (int i_18 = 0; i_18 < 16; ++i_18)
                    {
                        if (((mask_1 >> uint(i_18)) & 1u) != 0u)
                        {
                            continue;
                        }
                        const float t_6 = dot(px_3[i_18] - mean_2, axis_4) / axis_len_1;
                        tmin_1 = min(tmin_1, t_6);
                        tmax_1 = max(tmax_1, t_6);
                    }
                }
                const float4 opaque_3 = float4(0.0, 0.0, 0.0, 255.0);
                float if_48_result;
                if (alpha_2)
                {
                    if_48_result = 1.0;
                }
                else
                {
                    if_48_result = 0.0;
                }
                const float4 keep_2 = float4(1.0, 1.0, 1.0, if_48_result);
                const float4 e0_1 = clamp(mean_2 + axis_4 * tmin_1, (float4)0.0, (float4)255.0) * keep_2 + opaque_3 * ((float4)1.0 - keep_2);
                const float4 e1_1 = clamp(mean_2 + axis_4 * tmax_1, (float4)0.0, (float4)255.0) * keep_2 + opaque_3 * ((float4)1.0 - keep_2);
                const bool has_p_5 = pmode_1 != 0;
                subset_fit fit_4;
                if (pmode_1 == 3)
                {
                    const int top_13 = (int(1) << bits) - 1;
                    const int total_top_12 = (int(1) << (bits + 1)) - 1;
                    const float4 scaled_12 = e0_1 * float(total_top_12) / 255.0;
                    const int top_14 = (int(1) << bits) - 1;
                    const int total_top_13 = (int(1) << (bits + 1)) - 1;
                    const float4 scaled_13 = e1_1 * float(total_top_13) / 255.0;
                    subset_fit subset_fit_value_2;
                    subset_fit_value_2.q0 = clamp(int4(round((scaled_12 - float(1)) * 0.5)), (int4)0, (int4)top_13);
                    subset_fit_value_2.q1 = clamp(int4(round((scaled_13 - float(1)) * 0.5)), (int4)0, (int4)top_14);
                    subset_fit_value_2.p0 = 1;
                    subset_fit_value_2.p1 = 1;
                    subset_fit_value_2.error = 0.0;
                    fit_4 = subset_fit_value_2;
                }
                else
                {
                    int4 quantize_14_result;
                    const int top_15 = (int(1) << bits) - 1;
                    if (!(pmode_1 != 0))
                    {
                        quantize_14_result = clamp(int4(round(e0_1 * float(top_15) / 255.0)), (int4)0, (int4)top_15);
                    }
                    else
                    {
                        const int total_top_14 = (int(1) << (bits + 1)) - 1;
                        const float4 scaled_14 = e0_1 * float(total_top_14) / 255.0;
                        quantize_14_result = clamp(int4(round((scaled_14 - float(0)) * 0.5)), (int4)0, (int4)top_15);
                    }
                    int4 quantize_15_result;
                    const int top_16 = (int(1) << bits) - 1;
                    if (!(pmode_1 != 0))
                    {
                        quantize_15_result = clamp(int4(round(e1_1 * float(top_16) / 255.0)), (int4)0, (int4)top_16);
                    }
                    else
                    {
                        const int total_top_15 = (int(1) << (bits + 1)) - 1;
                        const float4 scaled_15 = e1_1 * float(total_top_15) / 255.0;
                        quantize_15_result = clamp(int4(round((scaled_15 - float(0)) * 0.5)), (int4)0, (int4)top_16);
                    }
                    subset_fit fit_5;
                    fit_5.q0 = quantize_14_result;
                    fit_5.q1 = quantize_15_result;
                    fit_5.p0 = 0;
                    fit_5.p1 = 0;
                    fit_5.error = 0.0;
                    if (pmode_1 == 0)
                    {
                        fit_4 = fit_5;
                    }
                    else
                    {
                        float best0_2 = 1.0e+30;
                        float best1_2 = 1.0e+30;
                        float best_shared_2 = 1.0e+30;
                        for (int p_9 = 0; p_9 < 2; ++p_9)
                        {
                            const int top_17 = (int(1) << bits) - 1;
                            const int total_top_16 = (int(1) << (bits + 1)) - 1;
                            const float4 scaled_16 = e0_1 * float(total_top_16) / 255.0;
                            const int4 q0_2 = clamp(int4(round((scaled_16 - float(p_9)) * 0.5)), (int4)0, (int4)top_17);
                            const int top_18 = (int(1) << bits) - 1;
                            const int total_top_17 = (int(1) << (bits + 1)) - 1;
                            const float4 scaled_17 = e1_1 * float(total_top_17) / 255.0;
                            const int4 q1_2 = clamp(int4(round((scaled_17 - float(p_9)) * 0.5)), (int4)0, (int4)top_18);
                            int4 expand_8_result;
                            const int total_9 = bits + 1;
                            const int4 v_19 = (q0_2 << (int4)1) | (int4)p_9;
                            if (total_9 >= 8)
                            {
                                expand_8_result = v_19;
                            }
                            else
                            {
                                expand_8_result = (v_19 << (int4)(8 - total_9)) | (v_19 >> (int4)(2 * total_9 - 8));
                            }
                            const float4 v_20 = float4(expand_8_result) - e0_1;
                            float4 d_17;
                            if (alpha_2)
                            {
                                d_17 = v_20;
                            }
                            else
                            {
                                d_17 = float4(v_20.x, v_20.y, v_20.z, 0.0);
                            }
                            const float err0_2 = dot(d_17, d_17);
                            int4 expand_9_result;
                            const int total_10 = bits + 1;
                            const int4 v_21 = (q1_2 << (int4)1) | (int4)p_9;
                            if (total_10 >= 8)
                            {
                                expand_9_result = v_21;
                            }
                            else
                            {
                                expand_9_result = (v_21 << (int4)(8 - total_10)) | (v_21 >> (int4)(2 * total_10 - 8));
                            }
                            const float4 v_22 = float4(expand_9_result) - e1_1;
                            float4 d_18;
                            if (alpha_2)
                            {
                                d_18 = v_22;
                            }
                            else
                            {
                                d_18 = float4(v_22.x, v_22.y, v_22.z, 0.0);
                            }
                            const float err1_2 = dot(d_18, d_18);
                            if (pmode_1 == 2)
                            {
                                if (err0_2 < best0_2)
                                {
                                    best0_2 = err0_2;
                                    fit_5.q0 = q0_2;
                                    fit_5.p0 = p_9;
                                }
                                if (err1_2 < best1_2)
                                {
                                    best1_2 = err1_2;
                                    fit_5.q1 = q1_2;
                                    fit_5.p1 = p_9;
                                }
                            }
                            else if (err0_2 + err1_2 < best_shared_2)
                            {
                                best_shared_2 = err0_2 + err1_2;
                                fit_5.q0 = q0_2;
                                fit_5.q1 = q1_2;
                                fit_5.p0 = p_9;
                                fit_5.p1 = p_9;
                            }
                        }
                        fit_4 = fit_5;
                    }
                }
                int4 a_2;
                const int4 q_5 = fit_4.q0;
                const int p_10 = fit_4.p0;
                int total_11;
                if (has_p_5)
                {
                    total_11 = bits + 1;
                }
                else
                {
                    total_11 = bits;
                }
                int4 v_23;
                if (has_p_5)
                {
                    v_23 = (q_5 << (int4)1) | (int4)p_10;
                }
                else
                {
                    v_23 = q_5;
                }
                if (total_11 >= 8)
                {
                    a_2 = v_23;
                }
                else
                {
                    a_2 = (v_23 << (int4)(8 - total_11)) | (v_23 >> (int4)(2 * total_11 - 8));
                }
                int4 b_2;
                const int4 q_6 = fit_4.q1;
                const int p_11 = fit_4.p1;
                int total_12;
                if (has_p_5)
                {
                    total_12 = bits + 1;
                }
                else
                {
                    total_12 = bits;
                }
                int4 v_24;
                if (has_p_5)
                {
                    v_24 = (q_6 << (int4)1) | (int4)p_11;
                }
                else
                {
                    v_24 = q_6;
                }
                if (total_12 >= 8)
                {
                    b_2 = v_24;
                }
                else
                {
                    b_2 = (v_24 << (int4)(8 - total_12)) | (v_24 >> (int4)(2 * total_12 - 8));
                }
                const int count_3 = int(1) << index_bits;
                const float4 fa_2 = float4(a_2);
                const float4 d_19 = float4(b_2 - a_2);
                float4 d_20;
                if (alpha_2)
                {
                    d_20 = d_19;
                }
                else
                {
                    d_20 = float4(d_19.x, d_19.y, d_19.z, 0.0);
                }
                const float dd_2 = dot(d_20, d_20);
                float error_3 = 0.0;
                for (int i_19 = 0; i_19 < 16; ++i_19)
                {
                    if (((mask_1 >> uint(i_19)) & 1u) != 0u)
                    {
                        continue;
                    }
                    float t_7;
                    if (dd_2 > 0.0)
                    {
                        float if_57_result;
                        if (alpha_2)
                        {
                            if_57_result = dot(px_3[i_19] - fa_2, d_19);
                        }
                        else
                        {
                            if_57_result = dot((px_3[i_19] - fa_2).xyz, d_19.xyz);
                        }
                        t_7 = if_57_result / dd_2;
                    }
                    else
                    {
                        t_7 = 0.0;
                    }
                    const int guess_2 = clamp(int(round(t_7 * float(count_3 - 1))), 0, count_3 - 1);
                    int best_3 = guess_2;
                    int w_7;
                    if (index_bits == 2)
                    {
                        static const int t2[4] = {0, 21, 43, 64};
                        w_7 = t2[guess_2];
                    }
                    else if (index_bits == 3)
                    {
                        static const int t3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
                        w_7 = t3[guess_2];
                    }
                    else
                    {
                        static const int t4_7[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
                        w_7 = t4_7[guess_2];
                    }
                    const float4 v_25 = float4(((64 - w_7) * a_2 + w_7 * b_2 + (int4)32) >> (int4)6) - px_3[i_19];
                    float4 d_21;
                    if (alpha_2)
                    {
                        d_21 = v_25;
                    }
                    else
                    {
                        d_21 = float4(v_25.x, v_25.y, v_25.z, 0.0);
                    }
                    float best_error_3 = dot(d_21, d_21);
                    if (guess_2 > 0)
                    {
                        int w_8;
                        const int i_20 = guess_2 - 1;
                        if (index_bits == 2)
                        {
                            static const int t2_1[4] = {0, 21, 43, 64};
                            w_8 = t2_1[i_20];
                        }
                        else if (index_bits == 3)
                        {
                            static const int t3_1[8] = {0, 9, 18, 27, 37, 46, 55, 64};
                            w_8 = t3_1[i_20];
                        }
                        else
                        {
                            static const int t4_8[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
                            w_8 = t4_8[i_20];
                        }
                        const float4 v_26 = float4(((64 - w_8) * a_2 + w_8 * b_2 + (int4)32) >> (int4)6) - px_3[i_19];
                        float4 d_22;
                        if (alpha_2)
                        {
                            d_22 = v_26;
                        }
                        else
                        {
                            d_22 = float4(v_26.x, v_26.y, v_26.z, 0.0);
                        }
                        const float e_7 = dot(d_22, d_22);
                        if (e_7 < best_error_3)
                        {
                            best_3 = guess_2 - 1;
                            best_error_3 = e_7;
                        }
                    }
                    if (guess_2 < count_3 - 1)
                    {
                        int w_9;
                        const int i_21 = guess_2 + 1;
                        if (index_bits == 2)
                        {
                            static const int t2_2[4] = {0, 21, 43, 64};
                            w_9 = t2_2[i_21];
                        }
                        else if (index_bits == 3)
                        {
                            static const int t3_2[8] = {0, 9, 18, 27, 37, 46, 55, 64};
                            w_9 = t3_2[i_21];
                        }
                        else
                        {
                            static const int t4_9[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
                            w_9 = t4_9[i_21];
                        }
                        const float4 v_27 = float4(((64 - w_9) * a_2 + w_9 * b_2 + (int4)32) >> (int4)6) - px_3[i_19];
                        float4 d_23;
                        if (alpha_2)
                        {
                            d_23 = v_27;
                        }
                        else
                        {
                            d_23 = float4(v_27.x, v_27.y, v_27.z, 0.0);
                        }
                        const float e_8 = dot(d_23, d_23);
                        if (e_8 < best_error_3)
                        {
                            best_3 = guess_2 + 1;
                            best_error_3 = e_8;
                        }
                    }
                    idx_1[i_19] = best_3;
                    error_3 = error_3 + best_error_3;
                }
                fit_4.error = error_3;
                float aa_1 = 0.0;
                float ab_1 = 0.0;
                float bb_1 = 0.0;
                float4 ax_1 = (float4)0.0;
                float4 bx_2 = (float4)0.0;
                for (int i_22 = 0; i_22 < 16; ++i_22)
                {
                    if (((mask_1 >> uint(i_22)) & 1u) != 0u)
                    {
                        continue;
                    }
                    int weight_10_result;
                    const int i_23 = idx_1[i_22];
                    if (index_bits == 2)
                    {
                        static const int t2_3[4] = {0, 21, 43, 64};
                        weight_10_result = t2_3[i_23];
                    }
                    else if (index_bits == 3)
                    {
                        static const int t3_3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
                        weight_10_result = t3_3[i_23];
                    }
                    else
                    {
                        static const int t4_10[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
                        weight_10_result = t4_10[i_23];
                    }
                    const float w_10 = float(weight_10_result) / 64.0;
                    aa_1 = aa_1 + (1.0 - w_10) * (1.0 - w_10);
                    ab_1 = ab_1 + (1.0 - w_10) * w_10;
                    bb_1 = bb_1 + w_10 * w_10;
                    ax_1 = ax_1 + px_3[i_22] * (1.0 - w_10);
                    bx_2 = bx_2 + px_3[i_22] * w_10;
                }
                const float det_1 = aa_1 * bb_1 - ab_1 * ab_1;
                if (abs(det_1) > 1.0e-06)
                {
                    const float4 r0_1 = clamp((ax_1 * bb_1 - bx_2 * ab_1) / det_1, (float4)0.0, (float4)255.0) * keep_2 + opaque_3 * ((float4)1.0 - keep_2);
                    const float4 r1_1 = clamp((bx_2 * aa_1 - ax_1 * ab_1) / det_1, (float4)0.0, (float4)255.0) * keep_2 + opaque_3 * ((float4)1.0 - keep_2);
                    subset_fit refit_1;
                    if (pmode_1 == 3)
                    {
                        const int top_19 = (int(1) << bits) - 1;
                        const int total_top_18 = (int(1) << (bits + 1)) - 1;
                        const float4 scaled_18 = r0_1 * float(total_top_18) / 255.0;
                        const int top_20 = (int(1) << bits) - 1;
                        const int total_top_19 = (int(1) << (bits + 1)) - 1;
                        const float4 scaled_19 = r1_1 * float(total_top_19) / 255.0;
                        subset_fit subset_fit_value_3;
                        subset_fit_value_3.q0 = clamp(int4(round((scaled_18 - float(1)) * 0.5)), (int4)0, (int4)top_19);
                        subset_fit_value_3.q1 = clamp(int4(round((scaled_19 - float(1)) * 0.5)), (int4)0, (int4)top_20);
                        subset_fit_value_3.p0 = 1;
                        subset_fit_value_3.p1 = 1;
                        subset_fit_value_3.error = 0.0;
                        refit_1 = subset_fit_value_3;
                    }
                    else
                    {
                        int4 quantize_20_result;
                        const int top_21 = (int(1) << bits) - 1;
                        if (!(pmode_1 != 0))
                        {
                            quantize_20_result = clamp(int4(round(r0_1 * float(top_21) / 255.0)), (int4)0, (int4)top_21);
                        }
                        else
                        {
                            const int total_top_20 = (int(1) << (bits + 1)) - 1;
                            const float4 scaled_20 = r0_1 * float(total_top_20) / 255.0;
                            quantize_20_result = clamp(int4(round((scaled_20 - float(0)) * 0.5)), (int4)0, (int4)top_21);
                        }
                        int4 quantize_21_result;
                        const int top_22 = (int(1) << bits) - 1;
                        if (!(pmode_1 != 0))
                        {
                            quantize_21_result = clamp(int4(round(r1_1 * float(top_22) / 255.0)), (int4)0, (int4)top_22);
                        }
                        else
                        {
                            const int total_top_21 = (int(1) << (bits + 1)) - 1;
                            const float4 scaled_21 = r1_1 * float(total_top_21) / 255.0;
                            quantize_21_result = clamp(int4(round((scaled_21 - float(0)) * 0.5)), (int4)0, (int4)top_22);
                        }
                        subset_fit fit_6;
                        fit_6.q0 = quantize_20_result;
                        fit_6.q1 = quantize_21_result;
                        fit_6.p0 = 0;
                        fit_6.p1 = 0;
                        fit_6.error = 0.0;
                        if (pmode_1 == 0)
                        {
                            refit_1 = fit_6;
                        }
                        else
                        {
                            float best0_3 = 1.0e+30;
                            float best1_3 = 1.0e+30;
                            float best_shared_3 = 1.0e+30;
                            for (int p_12 = 0; p_12 < 2; ++p_12)
                            {
                                const int top_23 = (int(1) << bits) - 1;
                                const int total_top_22 = (int(1) << (bits + 1)) - 1;
                                const float4 scaled_22 = r0_1 * float(total_top_22) / 255.0;
                                const int4 q0_3 = clamp(int4(round((scaled_22 - float(p_12)) * 0.5)), (int4)0, (int4)top_23);
                                const int top_24 = (int(1) << bits) - 1;
                                const int total_top_23 = (int(1) << (bits + 1)) - 1;
                                const float4 scaled_23 = r1_1 * float(total_top_23) / 255.0;
                                const int4 q1_3 = clamp(int4(round((scaled_23 - float(p_12)) * 0.5)), (int4)0, (int4)top_24);
                                int4 expand_12_result;
                                const int total_13 = bits + 1;
                                const int4 v_28 = (q0_3 << (int4)1) | (int4)p_12;
                                if (total_13 >= 8)
                                {
                                    expand_12_result = v_28;
                                }
                                else
                                {
                                    expand_12_result = (v_28 << (int4)(8 - total_13)) | (v_28 >> (int4)(2 * total_13 - 8));
                                }
                                const float4 v_29 = float4(expand_12_result) - r0_1;
                                float4 d_24;
                                if (alpha_2)
                                {
                                    d_24 = v_29;
                                }
                                else
                                {
                                    d_24 = float4(v_29.x, v_29.y, v_29.z, 0.0);
                                }
                                const float err0_3 = dot(d_24, d_24);
                                int4 expand_13_result;
                                const int total_14 = bits + 1;
                                const int4 v_30 = (q1_3 << (int4)1) | (int4)p_12;
                                if (total_14 >= 8)
                                {
                                    expand_13_result = v_30;
                                }
                                else
                                {
                                    expand_13_result = (v_30 << (int4)(8 - total_14)) | (v_30 >> (int4)(2 * total_14 - 8));
                                }
                                const float4 v_31 = float4(expand_13_result) - r1_1;
                                float4 d_25;
                                if (alpha_2)
                                {
                                    d_25 = v_31;
                                }
                                else
                                {
                                    d_25 = float4(v_31.x, v_31.y, v_31.z, 0.0);
                                }
                                const float err1_3 = dot(d_25, d_25);
                                if (pmode_1 == 2)
                                {
                                    if (err0_3 < best0_3)
                                    {
                                        best0_3 = err0_3;
                                        fit_6.q0 = q0_3;
                                        fit_6.p0 = p_12;
                                    }
                                    if (err1_3 < best1_3)
                                    {
                                        best1_3 = err1_3;
                                        fit_6.q1 = q1_3;
                                        fit_6.p1 = p_12;
                                    }
                                }
                                else if (err0_3 + err1_3 < best_shared_3)
                                {
                                    best_shared_3 = err0_3 + err1_3;
                                    fit_6.q0 = q0_3;
                                    fit_6.q1 = q1_3;
                                    fit_6.p0 = p_12;
                                    fit_6.p1 = p_12;
                                }
                            }
                            refit_1 = fit_6;
                        }
                    }
                    int refit_idx_1[16] = idx_1;
                    int4 a_3;
                    const int4 q_7 = refit_1.q0;
                    const int p_13 = refit_1.p0;
                    int total_15;
                    if (has_p_5)
                    {
                        total_15 = bits + 1;
                    }
                    else
                    {
                        total_15 = bits;
                    }
                    int4 v_32;
                    if (has_p_5)
                    {
                        v_32 = (q_7 << (int4)1) | (int4)p_13;
                    }
                    else
                    {
                        v_32 = q_7;
                    }
                    if (total_15 >= 8)
                    {
                        a_3 = v_32;
                    }
                    else
                    {
                        a_3 = (v_32 << (int4)(8 - total_15)) | (v_32 >> (int4)(2 * total_15 - 8));
                    }
                    int4 b_3;
                    const int4 q_8 = refit_1.q1;
                    const int p_14 = refit_1.p1;
                    int total_16;
                    if (has_p_5)
                    {
                        total_16 = bits + 1;
                    }
                    else
                    {
                        total_16 = bits;
                    }
                    int4 v_33;
                    if (has_p_5)
                    {
                        v_33 = (q_8 << (int4)1) | (int4)p_14;
                    }
                    else
                    {
                        v_33 = q_8;
                    }
                    if (total_16 >= 8)
                    {
                        b_3 = v_33;
                    }
                    else
                    {
                        b_3 = (v_33 << (int4)(8 - total_16)) | (v_33 >> (int4)(2 * total_16 - 8));
                    }
                    const int count_4 = int(1) << index_bits;
                    const float4 fa_3 = float4(a_3);
                    const float4 d_26 = float4(b_3 - a_3);
                    float4 d_27;
                    if (alpha_2)
                    {
                        d_27 = d_26;
                    }
                    else
                    {
                        d_27 = float4(d_26.x, d_26.y, d_26.z, 0.0);
                    }
                    const float dd_3 = dot(d_27, d_27);
                    float error_4 = 0.0;
                    for (int i_24 = 0; i_24 < 16; ++i_24)
                    {
                        if (((mask_1 >> uint(i_24)) & 1u) != 0u)
                        {
                            continue;
                        }
                        float t_8;
                        if (dd_3 > 0.0)
                        {
                            float if_69_result;
                            if (alpha_2)
                            {
                                if_69_result = dot(px_3[i_24] - fa_3, d_26);
                            }
                            else
                            {
                                if_69_result = dot((px_3[i_24] - fa_3).xyz, d_26.xyz);
                            }
                            t_8 = if_69_result / dd_3;
                        }
                        else
                        {
                            t_8 = 0.0;
                        }
                        const int guess_3 = clamp(int(round(t_8 * float(count_4 - 1))), 0, count_4 - 1);
                        int best_4 = guess_3;
                        int w_11;
                        if (index_bits == 2)
                        {
                            static const int t2_4[4] = {0, 21, 43, 64};
                            w_11 = t2_4[guess_3];
                        }
                        else if (index_bits == 3)
                        {
                            static const int t3_4[8] = {0, 9, 18, 27, 37, 46, 55, 64};
                            w_11 = t3_4[guess_3];
                        }
                        else
                        {
                            static const int t4_11[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
                            w_11 = t4_11[guess_3];
                        }
                        const float4 v_34 = float4(((64 - w_11) * a_3 + w_11 * b_3 + (int4)32) >> (int4)6) - px_3[i_24];
                        float4 d_28;
                        if (alpha_2)
                        {
                            d_28 = v_34;
                        }
                        else
                        {
                            d_28 = float4(v_34.x, v_34.y, v_34.z, 0.0);
                        }
                        float best_error_4 = dot(d_28, d_28);
                        if (guess_3 > 0)
                        {
                            int w_12;
                            const int i_25 = guess_3 - 1;
                            if (index_bits == 2)
                            {
                                static const int t2_5[4] = {0, 21, 43, 64};
                                w_12 = t2_5[i_25];
                            }
                            else if (index_bits == 3)
                            {
                                static const int t3_5[8] = {0, 9, 18, 27, 37, 46, 55, 64};
                                w_12 = t3_5[i_25];
                            }
                            else
                            {
                                static const int t4_12[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
                                w_12 = t4_12[i_25];
                            }
                            const float4 v_35 = float4(((64 - w_12) * a_3 + w_12 * b_3 + (int4)32) >> (int4)6) - px_3[i_24];
                            float4 d_29;
                            if (alpha_2)
                            {
                                d_29 = v_35;
                            }
                            else
                            {
                                d_29 = float4(v_35.x, v_35.y, v_35.z, 0.0);
                            }
                            const float e_9 = dot(d_29, d_29);
                            if (e_9 < best_error_4)
                            {
                                best_4 = guess_3 - 1;
                                best_error_4 = e_9;
                            }
                        }
                        if (guess_3 < count_4 - 1)
                        {
                            int w_13;
                            const int i_26 = guess_3 + 1;
                            if (index_bits == 2)
                            {
                                static const int t2_6[4] = {0, 21, 43, 64};
                                w_13 = t2_6[i_26];
                            }
                            else if (index_bits == 3)
                            {
                                static const int t3_6[8] = {0, 9, 18, 27, 37, 46, 55, 64};
                                w_13 = t3_6[i_26];
                            }
                            else
                            {
                                static const int t4_13[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
                                w_13 = t4_13[i_26];
                            }
                            const float4 v_36 = float4(((64 - w_13) * a_3 + w_13 * b_3 + (int4)32) >> (int4)6) - px_3[i_24];
                            float4 d_30;
                            if (alpha_2)
                            {
                                d_30 = v_36;
                            }
                            else
                            {
                                d_30 = float4(v_36.x, v_36.y, v_36.z, 0.0);
                            }
                            const float e_10 = dot(d_30, d_30);
                            if (e_10 < best_error_4)
                            {
                                best_4 = guess_3 + 1;
                                best_error_4 = e_10;
                            }
                        }
                        refit_idx_1[i_24] = best_4;
                        error_4 = error_4 + best_error_4;
                    }
                    refit_1.error = error_4;
                    if (refit_1.error < fit_4.error)
                    {
                        fit_4 = refit_1;
                        idx_1 = refit_idx_1;
                    }
                }
                subset_fit fit0 = fit_4;
                float n_3 = 0.0;
                float4 mean_3 = (float4)0.0;
                for (int i_27 = 0; i_27 < 16; ++i_27)
                {
                    if (((mask_1 >> uint(i_27)) & 1u) != 1u)
                    {
                        continue;
                    }
                    n_3 = n_3 + 1.0;
                    mean_3 = mean_3 + px_3[i_27];
                }
                mean_3 = mean_3 / max(n_3, 1.0);
                float4 c0_6 = (float4)0.0;
                float4 c1_6 = (float4)0.0;
                float4 c2_6 = (float4)0.0;
                float4 c3_6 = (float4)0.0;
                for (int i_28 = 0; i_28 < 16; ++i_28)
                {
                    if (((mask_1 >> uint(i_28)) & 1u) != 1u)
                    {
                        continue;
                    }
                    float4 d_31;
                    if (alpha_2)
                    {
                        d_31 = px_3[i_28] - mean_3;
                    }
                    else
                    {
                        const float4 splat_2 = px_3[i_28] - mean_3;
                        d_31 = float4(splat_2.x, splat_2.y, splat_2.z, 0.0);
                    }
                    c0_6 = c0_6 + d_31 * d_31.x;
                    c1_6 = c1_6 + d_31 * d_31.y;
                    c2_6 = c2_6 + d_31 * d_31.z;
                    c3_6 = c3_6 + d_31 * d_31.w;
                }
                const float4 c0_7 = c0_6;
                const float4 c1_7 = c1_6;
                const float4 c2_7 = c2_6;
                const float4 c3_7 = c3_6;
                float4 axis_7 = c0_7;
                float spread_4 = c0_7.x;
                if (c1_7.y > spread_4)
                {
                    axis_7 = c1_7;
                    spread_4 = c1_7.y;
                }
                if (c2_7.z > spread_4)
                {
                    axis_7 = c2_7;
                    spread_4 = c2_7.z;
                }
                if (c3_7.w > spread_4)
                {
                    axis_7 = c3_7;
                }
                float4 axis_6 = axis_7;
                for (int k_4 = 0; k_4 < 4; ++k_4)
                {
                    const float4 next_3 = c0_6 * axis_6.x + c1_6 * axis_6.y + c2_6 * axis_6.z + c3_6 * axis_6.w;
                    const float len_3 = sqrt(dot(next_3, next_3));
                    if (len_3 > 1.0e-06)
                    {
                        axis_6 = next_3 / len_3;
                    }
                }
                const float axis_len_2 = dot(axis_6, axis_6);
                float tmin_2 = 0.0;
                float tmax_2 = 0.0;
                if (axis_len_2 > 1.0e-12)
                {
                    tmin_2 = 1.0e+30;
                    tmax_2 = -1.0e+30;
                    for (int i_29 = 0; i_29 < 16; ++i_29)
                    {
                        if (((mask_1 >> uint(i_29)) & 1u) != 1u)
                        {
                            continue;
                        }
                        const float t_9 = dot(px_3[i_29] - mean_3, axis_6) / axis_len_2;
                        tmin_2 = min(tmin_2, t_9);
                        tmax_2 = max(tmax_2, t_9);
                    }
                }
                const float4 opaque_4 = float4(0.0, 0.0, 0.0, 255.0);
                float if_74_result;
                if (alpha_2)
                {
                    if_74_result = 1.0;
                }
                else
                {
                    if_74_result = 0.0;
                }
                const float4 keep_3 = float4(1.0, 1.0, 1.0, if_74_result);
                const float4 e0_2 = clamp(mean_3 + axis_6 * tmin_2, (float4)0.0, (float4)255.0) * keep_3 + opaque_4 * ((float4)1.0 - keep_3);
                const float4 e1_2 = clamp(mean_3 + axis_6 * tmax_2, (float4)0.0, (float4)255.0) * keep_3 + opaque_4 * ((float4)1.0 - keep_3);
                const bool has_p_10 = pmode_1 != 0;
                subset_fit fit_7;
                if (pmode_1 == 3)
                {
                    const int top_25 = (int(1) << bits) - 1;
                    const int total_top_24 = (int(1) << (bits + 1)) - 1;
                    const float4 scaled_24 = e0_2 * float(total_top_24) / 255.0;
                    const int top_26 = (int(1) << bits) - 1;
                    const int total_top_25 = (int(1) << (bits + 1)) - 1;
                    const float4 scaled_25 = e1_2 * float(total_top_25) / 255.0;
                    subset_fit subset_fit_value_4;
                    subset_fit_value_4.q0 = clamp(int4(round((scaled_24 - float(1)) * 0.5)), (int4)0, (int4)top_25);
                    subset_fit_value_4.q1 = clamp(int4(round((scaled_25 - float(1)) * 0.5)), (int4)0, (int4)top_26);
                    subset_fit_value_4.p0 = 1;
                    subset_fit_value_4.p1 = 1;
                    subset_fit_value_4.error = 0.0;
                    fit_7 = subset_fit_value_4;
                }
                else
                {
                    int4 quantize_26_result;
                    const int top_27 = (int(1) << bits) - 1;
                    if (!(pmode_1 != 0))
                    {
                        quantize_26_result = clamp(int4(round(e0_2 * float(top_27) / 255.0)), (int4)0, (int4)top_27);
                    }
                    else
                    {
                        const int total_top_26 = (int(1) << (bits + 1)) - 1;
                        const float4 scaled_26 = e0_2 * float(total_top_26) / 255.0;
                        quantize_26_result = clamp(int4(round((scaled_26 - float(0)) * 0.5)), (int4)0, (int4)top_27);
                    }
                    int4 quantize_27_result;
                    const int top_28 = (int(1) << bits) - 1;
                    if (!(pmode_1 != 0))
                    {
                        quantize_27_result = clamp(int4(round(e1_2 * float(top_28) / 255.0)), (int4)0, (int4)top_28);
                    }
                    else
                    {
                        const int total_top_27 = (int(1) << (bits + 1)) - 1;
                        const float4 scaled_27 = e1_2 * float(total_top_27) / 255.0;
                        quantize_27_result = clamp(int4(round((scaled_27 - float(0)) * 0.5)), (int4)0, (int4)top_28);
                    }
                    subset_fit fit_8;
                    fit_8.q0 = quantize_26_result;
                    fit_8.q1 = quantize_27_result;
                    fit_8.p0 = 0;
                    fit_8.p1 = 0;
                    fit_8.error = 0.0;
                    if (pmode_1 == 0)
                    {
                        fit_7 = fit_8;
                    }
                    else
                    {
                        float best0_4 = 1.0e+30;
                        float best1_4 = 1.0e+30;
                        float best_shared_4 = 1.0e+30;
                        for (int p_15 = 0; p_15 < 2; ++p_15)
                        {
                            const int top_29 = (int(1) << bits) - 1;
                            const int total_top_28 = (int(1) << (bits + 1)) - 1;
                            const float4 scaled_28 = e0_2 * float(total_top_28) / 255.0;
                            const int4 q0_4 = clamp(int4(round((scaled_28 - float(p_15)) * 0.5)), (int4)0, (int4)top_29);
                            const int top_30 = (int(1) << bits) - 1;
                            const int total_top_29 = (int(1) << (bits + 1)) - 1;
                            const float4 scaled_29 = e1_2 * float(total_top_29) / 255.0;
                            const int4 q1_4 = clamp(int4(round((scaled_29 - float(p_15)) * 0.5)), (int4)0, (int4)top_30);
                            int4 expand_16_result;
                            const int total_17 = bits + 1;
                            const int4 v_37 = (q0_4 << (int4)1) | (int4)p_15;
                            if (total_17 >= 8)
                            {
                                expand_16_result = v_37;
                            }
                            else
                            {
                                expand_16_result = (v_37 << (int4)(8 - total_17)) | (v_37 >> (int4)(2 * total_17 - 8));
                            }
                            const float4 v_38 = float4(expand_16_result) - e0_2;
                            float4 d_32;
                            if (alpha_2)
                            {
                                d_32 = v_38;
                            }
                            else
                            {
                                d_32 = float4(v_38.x, v_38.y, v_38.z, 0.0);
                            }
                            const float err0_4 = dot(d_32, d_32);
                            int4 expand_17_result;
                            const int total_18 = bits + 1;
                            const int4 v_39 = (q1_4 << (int4)1) | (int4)p_15;
                            if (total_18 >= 8)
                            {
                                expand_17_result = v_39;
                            }
                            else
                            {
                                expand_17_result = (v_39 << (int4)(8 - total_18)) | (v_39 >> (int4)(2 * total_18 - 8));
                            }
                            const float4 v_40 = float4(expand_17_result) - e1_2;
                            float4 d_33;
                            if (alpha_2)
                            {
                                d_33 = v_40;
                            }
                            else
                            {
                                d_33 = float4(v_40.x, v_40.y, v_40.z, 0.0);
                            }
                            const float err1_4 = dot(d_33, d_33);
                            if (pmode_1 == 2)
                            {
                                if (err0_4 < best0_4)
                                {
                                    best0_4 = err0_4;
                                    fit_8.q0 = q0_4;
                                    fit_8.p0 = p_15;
                                }
                                if (err1_4 < best1_4)
                                {
                                    best1_4 = err1_4;
                                    fit_8.q1 = q1_4;
                                    fit_8.p1 = p_15;
                                }
                            }
                            else if (err0_4 + err1_4 < best_shared_4)
                            {
                                best_shared_4 = err0_4 + err1_4;
                                fit_8.q0 = q0_4;
                                fit_8.q1 = q1_4;
                                fit_8.p0 = p_15;
                                fit_8.p1 = p_15;
                            }
                        }
                        fit_7 = fit_8;
                    }
                }
                int4 a_4;
                const int4 q_9 = fit_7.q0;
                const int p_16 = fit_7.p0;
                int total_19;
                if (has_p_10)
                {
                    total_19 = bits + 1;
                }
                else
                {
                    total_19 = bits;
                }
                int4 v_41;
                if (has_p_10)
                {
                    v_41 = (q_9 << (int4)1) | (int4)p_16;
                }
                else
                {
                    v_41 = q_9;
                }
                if (total_19 >= 8)
                {
                    a_4 = v_41;
                }
                else
                {
                    a_4 = (v_41 << (int4)(8 - total_19)) | (v_41 >> (int4)(2 * total_19 - 8));
                }
                int4 b_4;
                const int4 q_10 = fit_7.q1;
                const int p_17 = fit_7.p1;
                int total_20;
                if (has_p_10)
                {
                    total_20 = bits + 1;
                }
                else
                {
                    total_20 = bits;
                }
                int4 v_42;
                if (has_p_10)
                {
                    v_42 = (q_10 << (int4)1) | (int4)p_17;
                }
                else
                {
                    v_42 = q_10;
                }
                if (total_20 >= 8)
                {
                    b_4 = v_42;
                }
                else
                {
                    b_4 = (v_42 << (int4)(8 - total_20)) | (v_42 >> (int4)(2 * total_20 - 8));
                }
                const int count_5 = int(1) << index_bits;
                const float4 fa_4 = float4(a_4);
                const float4 d_34 = float4(b_4 - a_4);
                float4 d_35;
                if (alpha_2)
                {
                    d_35 = d_34;
                }
                else
                {
                    d_35 = float4(d_34.x, d_34.y, d_34.z, 0.0);
                }
                const float dd_4 = dot(d_35, d_35);
                float error_5 = 0.0;
                for (int i_30 = 0; i_30 < 16; ++i_30)
                {
                    if (((mask_1 >> uint(i_30)) & 1u) != 1u)
                    {
                        continue;
                    }
                    float t_10;
                    if (dd_4 > 0.0)
                    {
                        float if_83_result;
                        if (alpha_2)
                        {
                            if_83_result = dot(px_3[i_30] - fa_4, d_34);
                        }
                        else
                        {
                            if_83_result = dot((px_3[i_30] - fa_4).xyz, d_34.xyz);
                        }
                        t_10 = if_83_result / dd_4;
                    }
                    else
                    {
                        t_10 = 0.0;
                    }
                    const int guess_4 = clamp(int(round(t_10 * float(count_5 - 1))), 0, count_5 - 1);
                    int best_5 = guess_4;
                    int w_14;
                    if (index_bits == 2)
                    {
                        static const int t2_7[4] = {0, 21, 43, 64};
                        w_14 = t2_7[guess_4];
                    }
                    else if (index_bits == 3)
                    {
                        static const int t3_7[8] = {0, 9, 18, 27, 37, 46, 55, 64};
                        w_14 = t3_7[guess_4];
                    }
                    else
                    {
                        static const int t4_14[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
                        w_14 = t4_14[guess_4];
                    }
                    const float4 v_43 = float4(((64 - w_14) * a_4 + w_14 * b_4 + (int4)32) >> (int4)6) - px_3[i_30];
                    float4 d_36;
                    if (alpha_2)
                    {
                        d_36 = v_43;
                    }
                    else
                    {
                        d_36 = float4(v_43.x, v_43.y, v_43.z, 0.0);
                    }
                    float best_error_5 = dot(d_36, d_36);
                    if (guess_4 > 0)
                    {
                        int w_15;
                        const int i_31 = guess_4 - 1;
                        if (index_bits == 2)
                        {
                            static const int t2_8[4] = {0, 21, 43, 64};
                            w_15 = t2_8[i_31];
                        }
                        else if (index_bits == 3)
                        {
                            static const int t3_8[8] = {0, 9, 18, 27, 37, 46, 55, 64};
                            w_15 = t3_8[i_31];
                        }
                        else
                        {
                            static const int t4_15[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
                            w_15 = t4_15[i_31];
                        }
                        const float4 v_44 = float4(((64 - w_15) * a_4 + w_15 * b_4 + (int4)32) >> (int4)6) - px_3[i_30];
                        float4 d_37;
                        if (alpha_2)
                        {
                            d_37 = v_44;
                        }
                        else
                        {
                            d_37 = float4(v_44.x, v_44.y, v_44.z, 0.0);
                        }
                        const float e_11 = dot(d_37, d_37);
                        if (e_11 < best_error_5)
                        {
                            best_5 = guess_4 - 1;
                            best_error_5 = e_11;
                        }
                    }
                    if (guess_4 < count_5 - 1)
                    {
                        int w_16;
                        const int i_32 = guess_4 + 1;
                        if (index_bits == 2)
                        {
                            static const int t2_9[4] = {0, 21, 43, 64};
                            w_16 = t2_9[i_32];
                        }
                        else if (index_bits == 3)
                        {
                            static const int t3_9[8] = {0, 9, 18, 27, 37, 46, 55, 64};
                            w_16 = t3_9[i_32];
                        }
                        else
                        {
                            static const int t4_16[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
                            w_16 = t4_16[i_32];
                        }
                        const float4 v_45 = float4(((64 - w_16) * a_4 + w_16 * b_4 + (int4)32) >> (int4)6) - px_3[i_30];
                        float4 d_38;
                        if (alpha_2)
                        {
                            d_38 = v_45;
                        }
                        else
                        {
                            d_38 = float4(v_45.x, v_45.y, v_45.z, 0.0);
                        }
                        const float e_12 = dot(d_38, d_38);
                        if (e_12 < best_error_5)
                        {
                            best_5 = guess_4 + 1;
                            best_error_5 = e_12;
                        }
                    }
                    idx_1[i_30] = best_5;
                    error_5 = error_5 + best_error_5;
                }
                fit_7.error = error_5;
                float aa_2 = 0.0;
                float ab_2 = 0.0;
                float bb_2 = 0.0;
                float4 ax_2 = (float4)0.0;
                float4 bx_3 = (float4)0.0;
                for (int i_33 = 0; i_33 < 16; ++i_33)
                {
                    if (((mask_1 >> uint(i_33)) & 1u) != 1u)
                    {
                        continue;
                    }
                    int weight_17_result;
                    const int i_34 = idx_1[i_33];
                    if (index_bits == 2)
                    {
                        static const int t2_10[4] = {0, 21, 43, 64};
                        weight_17_result = t2_10[i_34];
                    }
                    else if (index_bits == 3)
                    {
                        static const int t3_10[8] = {0, 9, 18, 27, 37, 46, 55, 64};
                        weight_17_result = t3_10[i_34];
                    }
                    else
                    {
                        static const int t4_17[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
                        weight_17_result = t4_17[i_34];
                    }
                    const float w_17 = float(weight_17_result) / 64.0;
                    aa_2 = aa_2 + (1.0 - w_17) * (1.0 - w_17);
                    ab_2 = ab_2 + (1.0 - w_17) * w_17;
                    bb_2 = bb_2 + w_17 * w_17;
                    ax_2 = ax_2 + px_3[i_33] * (1.0 - w_17);
                    bx_3 = bx_3 + px_3[i_33] * w_17;
                }
                const float det_2 = aa_2 * bb_2 - ab_2 * ab_2;
                if (abs(det_2) > 1.0e-06)
                {
                    const float4 r0_2 = clamp((ax_2 * bb_2 - bx_3 * ab_2) / det_2, (float4)0.0, (float4)255.0) * keep_3 + opaque_4 * ((float4)1.0 - keep_3);
                    const float4 r1_2 = clamp((bx_3 * aa_2 - ax_2 * ab_2) / det_2, (float4)0.0, (float4)255.0) * keep_3 + opaque_4 * ((float4)1.0 - keep_3);
                    subset_fit refit_2;
                    if (pmode_1 == 3)
                    {
                        const int top_31 = (int(1) << bits) - 1;
                        const int total_top_30 = (int(1) << (bits + 1)) - 1;
                        const float4 scaled_30 = r0_2 * float(total_top_30) / 255.0;
                        const int top_32 = (int(1) << bits) - 1;
                        const int total_top_31 = (int(1) << (bits + 1)) - 1;
                        const float4 scaled_31 = r1_2 * float(total_top_31) / 255.0;
                        subset_fit subset_fit_value_5;
                        subset_fit_value_5.q0 = clamp(int4(round((scaled_30 - float(1)) * 0.5)), (int4)0, (int4)top_31);
                        subset_fit_value_5.q1 = clamp(int4(round((scaled_31 - float(1)) * 0.5)), (int4)0, (int4)top_32);
                        subset_fit_value_5.p0 = 1;
                        subset_fit_value_5.p1 = 1;
                        subset_fit_value_5.error = 0.0;
                        refit_2 = subset_fit_value_5;
                    }
                    else
                    {
                        int4 quantize_32_result;
                        const int top_33 = (int(1) << bits) - 1;
                        if (!(pmode_1 != 0))
                        {
                            quantize_32_result = clamp(int4(round(r0_2 * float(top_33) / 255.0)), (int4)0, (int4)top_33);
                        }
                        else
                        {
                            const int total_top_32 = (int(1) << (bits + 1)) - 1;
                            const float4 scaled_32 = r0_2 * float(total_top_32) / 255.0;
                            quantize_32_result = clamp(int4(round((scaled_32 - float(0)) * 0.5)), (int4)0, (int4)top_33);
                        }
                        int4 quantize_33_result;
                        const int top_34 = (int(1) << bits) - 1;
                        if (!(pmode_1 != 0))
                        {
                            quantize_33_result = clamp(int4(round(r1_2 * float(top_34) / 255.0)), (int4)0, (int4)top_34);
                        }
                        else
                        {
                            const int total_top_33 = (int(1) << (bits + 1)) - 1;
                            const float4 scaled_33 = r1_2 * float(total_top_33) / 255.0;
                            quantize_33_result = clamp(int4(round((scaled_33 - float(0)) * 0.5)), (int4)0, (int4)top_34);
                        }
                        subset_fit fit_9;
                        fit_9.q0 = quantize_32_result;
                        fit_9.q1 = quantize_33_result;
                        fit_9.p0 = 0;
                        fit_9.p1 = 0;
                        fit_9.error = 0.0;
                        if (pmode_1 == 0)
                        {
                            refit_2 = fit_9;
                        }
                        else
                        {
                            float best0_5 = 1.0e+30;
                            float best1_5 = 1.0e+30;
                            float best_shared_5 = 1.0e+30;
                            for (int p_18 = 0; p_18 < 2; ++p_18)
                            {
                                const int top_35 = (int(1) << bits) - 1;
                                const int total_top_34 = (int(1) << (bits + 1)) - 1;
                                const float4 scaled_34 = r0_2 * float(total_top_34) / 255.0;
                                const int4 q0_5 = clamp(int4(round((scaled_34 - float(p_18)) * 0.5)), (int4)0, (int4)top_35);
                                const int top_36 = (int(1) << bits) - 1;
                                const int total_top_35 = (int(1) << (bits + 1)) - 1;
                                const float4 scaled_35 = r1_2 * float(total_top_35) / 255.0;
                                const int4 q1_5 = clamp(int4(round((scaled_35 - float(p_18)) * 0.5)), (int4)0, (int4)top_36);
                                int4 expand_20_result;
                                const int total_21 = bits + 1;
                                const int4 v_46 = (q0_5 << (int4)1) | (int4)p_18;
                                if (total_21 >= 8)
                                {
                                    expand_20_result = v_46;
                                }
                                else
                                {
                                    expand_20_result = (v_46 << (int4)(8 - total_21)) | (v_46 >> (int4)(2 * total_21 - 8));
                                }
                                const float4 v_47 = float4(expand_20_result) - r0_2;
                                float4 d_39;
                                if (alpha_2)
                                {
                                    d_39 = v_47;
                                }
                                else
                                {
                                    d_39 = float4(v_47.x, v_47.y, v_47.z, 0.0);
                                }
                                const float err0_5 = dot(d_39, d_39);
                                int4 expand_21_result;
                                const int total_22 = bits + 1;
                                const int4 v_48 = (q1_5 << (int4)1) | (int4)p_18;
                                if (total_22 >= 8)
                                {
                                    expand_21_result = v_48;
                                }
                                else
                                {
                                    expand_21_result = (v_48 << (int4)(8 - total_22)) | (v_48 >> (int4)(2 * total_22 - 8));
                                }
                                const float4 v_49 = float4(expand_21_result) - r1_2;
                                float4 d_40;
                                if (alpha_2)
                                {
                                    d_40 = v_49;
                                }
                                else
                                {
                                    d_40 = float4(v_49.x, v_49.y, v_49.z, 0.0);
                                }
                                const float err1_5 = dot(d_40, d_40);
                                if (pmode_1 == 2)
                                {
                                    if (err0_5 < best0_5)
                                    {
                                        best0_5 = err0_5;
                                        fit_9.q0 = q0_5;
                                        fit_9.p0 = p_18;
                                    }
                                    if (err1_5 < best1_5)
                                    {
                                        best1_5 = err1_5;
                                        fit_9.q1 = q1_5;
                                        fit_9.p1 = p_18;
                                    }
                                }
                                else if (err0_5 + err1_5 < best_shared_5)
                                {
                                    best_shared_5 = err0_5 + err1_5;
                                    fit_9.q0 = q0_5;
                                    fit_9.q1 = q1_5;
                                    fit_9.p0 = p_18;
                                    fit_9.p1 = p_18;
                                }
                            }
                            refit_2 = fit_9;
                        }
                    }
                    int refit_idx_2[16] = idx_1;
                    int4 a_5;
                    const int4 q_11 = refit_2.q0;
                    const int p_19 = refit_2.p0;
                    int total_23;
                    if (has_p_10)
                    {
                        total_23 = bits + 1;
                    }
                    else
                    {
                        total_23 = bits;
                    }
                    int4 v_50;
                    if (has_p_10)
                    {
                        v_50 = (q_11 << (int4)1) | (int4)p_19;
                    }
                    else
                    {
                        v_50 = q_11;
                    }
                    if (total_23 >= 8)
                    {
                        a_5 = v_50;
                    }
                    else
                    {
                        a_5 = (v_50 << (int4)(8 - total_23)) | (v_50 >> (int4)(2 * total_23 - 8));
                    }
                    int4 b_5;
                    const int4 q_12 = refit_2.q1;
                    const int p_20 = refit_2.p1;
                    int total_24;
                    if (has_p_10)
                    {
                        total_24 = bits + 1;
                    }
                    else
                    {
                        total_24 = bits;
                    }
                    int4 v_51;
                    if (has_p_10)
                    {
                        v_51 = (q_12 << (int4)1) | (int4)p_20;
                    }
                    else
                    {
                        v_51 = q_12;
                    }
                    if (total_24 >= 8)
                    {
                        b_5 = v_51;
                    }
                    else
                    {
                        b_5 = (v_51 << (int4)(8 - total_24)) | (v_51 >> (int4)(2 * total_24 - 8));
                    }
                    const int count_6 = int(1) << index_bits;
                    const float4 fa_5 = float4(a_5);
                    const float4 d_41 = float4(b_5 - a_5);
                    float4 d_42;
                    if (alpha_2)
                    {
                        d_42 = d_41;
                    }
                    else
                    {
                        d_42 = float4(d_41.x, d_41.y, d_41.z, 0.0);
                    }
                    const float dd_5 = dot(d_42, d_42);
                    float error_6 = 0.0;
                    for (int i_35 = 0; i_35 < 16; ++i_35)
                    {
                        if (((mask_1 >> uint(i_35)) & 1u) != 1u)
                        {
                            continue;
                        }
                        float t_11;
                        if (dd_5 > 0.0)
                        {
                            float if_95_result;
                            if (alpha_2)
                            {
                                if_95_result = dot(px_3[i_35] - fa_5, d_41);
                            }
                            else
                            {
                                if_95_result = dot((px_3[i_35] - fa_5).xyz, d_41.xyz);
                            }
                            t_11 = if_95_result / dd_5;
                        }
                        else
                        {
                            t_11 = 0.0;
                        }
                        const int guess_5 = clamp(int(round(t_11 * float(count_6 - 1))), 0, count_6 - 1);
                        int best_6 = guess_5;
                        int w_18;
                        if (index_bits == 2)
                        {
                            static const int t2_11[4] = {0, 21, 43, 64};
                            w_18 = t2_11[guess_5];
                        }
                        else if (index_bits == 3)
                        {
                            static const int t3_11[8] = {0, 9, 18, 27, 37, 46, 55, 64};
                            w_18 = t3_11[guess_5];
                        }
                        else
                        {
                            static const int t4_18[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
                            w_18 = t4_18[guess_5];
                        }
                        const float4 v_52 = float4(((64 - w_18) * a_5 + w_18 * b_5 + (int4)32) >> (int4)6) - px_3[i_35];
                        float4 d_43;
                        if (alpha_2)
                        {
                            d_43 = v_52;
                        }
                        else
                        {
                            d_43 = float4(v_52.x, v_52.y, v_52.z, 0.0);
                        }
                        float best_error_6 = dot(d_43, d_43);
                        if (guess_5 > 0)
                        {
                            int w_19;
                            const int i_36 = guess_5 - 1;
                            if (index_bits == 2)
                            {
                                static const int t2_12[4] = {0, 21, 43, 64};
                                w_19 = t2_12[i_36];
                            }
                            else if (index_bits == 3)
                            {
                                static const int t3_12[8] = {0, 9, 18, 27, 37, 46, 55, 64};
                                w_19 = t3_12[i_36];
                            }
                            else
                            {
                                static const int t4_19[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
                                w_19 = t4_19[i_36];
                            }
                            const float4 v_53 = float4(((64 - w_19) * a_5 + w_19 * b_5 + (int4)32) >> (int4)6) - px_3[i_35];
                            float4 d_44;
                            if (alpha_2)
                            {
                                d_44 = v_53;
                            }
                            else
                            {
                                d_44 = float4(v_53.x, v_53.y, v_53.z, 0.0);
                            }
                            const float e_13 = dot(d_44, d_44);
                            if (e_13 < best_error_6)
                            {
                                best_6 = guess_5 - 1;
                                best_error_6 = e_13;
                            }
                        }
                        if (guess_5 < count_6 - 1)
                        {
                            int w_20;
                            const int i_37 = guess_5 + 1;
                            if (index_bits == 2)
                            {
                                static const int t2_13[4] = {0, 21, 43, 64};
                                w_20 = t2_13[i_37];
                            }
                            else if (index_bits == 3)
                            {
                                static const int t3_13[8] = {0, 9, 18, 27, 37, 46, 55, 64};
                                w_20 = t3_13[i_37];
                            }
                            else
                            {
                                static const int t4_20[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
                                w_20 = t4_20[i_37];
                            }
                            const float4 v_54 = float4(((64 - w_20) * a_5 + w_20 * b_5 + (int4)32) >> (int4)6) - px_3[i_35];
                            float4 d_45;
                            if (alpha_2)
                            {
                                d_45 = v_54;
                            }
                            else
                            {
                                d_45 = float4(v_54.x, v_54.y, v_54.z, 0.0);
                            }
                            const float e_14 = dot(d_45, d_45);
                            if (e_14 < best_error_6)
                            {
                                best_6 = guess_5 + 1;
                                best_error_6 = e_14;
                            }
                        }
                        refit_idx_2[i_35] = best_6;
                        error_6 = error_6 + best_error_6;
                    }
                    refit_2.error = error_6;
                    if (refit_2.error < fit_7.error)
                    {
                        fit_7 = refit_2;
                        idx_1 = refit_idx_2;
                    }
                }
                subset_fit fit1 = fit_7;
                error_2 = fit0.error + fit1.error;
                const int half_range = (top_12 + 1) / 2;
                if (idx_1[0] >= half_range)
                {
                    const int4 q_13 = fit0.q0;
                    fit0.q0 = fit0.q1;
                    fit0.q1 = q_13;
                    const int pb = fit0.p0;
                    fit0.p0 = fit0.p1;
                    fit0.p1 = pb;
                    for (int i_38 = 0; i_38 < 16; ++i_38)
                    {
                        if (((mask_1 >> uint(i_38)) & 1u) == 0u)
                        {
                            idx_1[i_38] = top_12 - idx_1[i_38];
                        }
                    }
                }
                if (idx_1[anchor] >= half_range)
                {
                    const int4 q_14 = fit1.q0;
                    fit1.q0 = fit1.q1;
                    fit1.q1 = q_14;
                    const int pb_1 = fit1.p0;
                    fit1.p0 = fit1.p1;
                    fit1.p1 = pb_1;
                    for (int i_39 = 0; i_39 < 16; ++i_39)
                    {
                        if (((mask_1 >> uint(i_39)) & 1u) == 1u)
                        {
                            idx_1[i_39] = top_12 - idx_1[i_39];
                        }
                    }
                }
                uint4 block_2 = (uint4)0u;
                int pos_1 = 0;
                const uint value_5 = 1u << uint(mode);
                const int count_7 = mode + 1;
                const int word_6 = pos_1 >> 5;
                const uint shift_6 = uint(pos_1 & 31);
                const uint low_6 = value_5 << shift_6;
                uint high_6;
                if (shift_6 == 0u)
                {
                    high_6 = 0u;
                }
                else
                {
                    high_6 = value_5 >> (32u - shift_6);
                }
                const bool spills_6 = (pos_1 & 31) + count_7 > 32;
                if (word_6 == 0)
                {
                    block_2.x = block_2.x | low_6;
                    if (spills_6)
                    {
                        block_2.y = block_2.y | high_6;
                    }
                }
                else if (word_6 == 1)
                {
                    block_2.y = block_2.y | low_6;
                    if (spills_6)
                    {
                        block_2.z = block_2.z | high_6;
                    }
                }
                else if (word_6 == 2)
                {
                    block_2.z = block_2.z | low_6;
                    if (spills_6)
                    {
                        block_2.w = block_2.w | high_6;
                    }
                }
                else
                {
                    block_2.w = block_2.w | low_6;
                }
                pos_1 = pos_1 + count_7;
                const uint value_6 = uint(p_8);
                const int word_7 = pos_1 >> 5;
                const uint shift_7 = uint(pos_1 & 31);
                const uint low_7 = value_6 << shift_7;
                uint high_7;
                if (shift_7 == 0u)
                {
                    high_7 = 0u;
                }
                else
                {
                    high_7 = value_6 >> (32u - shift_7);
                }
                const bool spills_7 = (pos_1 & 31) + 6 > 32;
                if (word_7 == 0)
                {
                    block_2.x = block_2.x | low_7;
                    if (spills_7)
                    {
                        block_2.y = block_2.y | high_7;
                    }
                }
                else if (word_7 == 1)
                {
                    block_2.y = block_2.y | low_7;
                    if (spills_7)
                    {
                        block_2.z = block_2.z | high_7;
                    }
                }
                else if (word_7 == 2)
                {
                    block_2.z = block_2.z | low_7;
                    if (spills_7)
                    {
                        block_2.w = block_2.w | high_7;
                    }
                }
                else
                {
                    block_2.w = block_2.w | low_7;
                }
                pos_1 = pos_1 + 6;
                for (int c_5 = 0; c_5 < channels; ++c_5)
                {
                    const uint value_7 = uint(fit0.q0[c_5]);
                    const int word_8 = pos_1 >> 5;
                    const uint shift_8 = uint(pos_1 & 31);
                    const uint low_8 = value_7 << shift_8;
                    uint high_8;
                    if (shift_8 == 0u)
                    {
                        high_8 = 0u;
                    }
                    else
                    {
                        high_8 = value_7 >> (32u - shift_8);
                    }
                    const bool spills_8 = (pos_1 & 31) + bits > 32;
                    if (word_8 == 0)
                    {
                        block_2.x = block_2.x | low_8;
                        if (spills_8)
                        {
                            block_2.y = block_2.y | high_8;
                        }
                    }
                    else if (word_8 == 1)
                    {
                        block_2.y = block_2.y | low_8;
                        if (spills_8)
                        {
                            block_2.z = block_2.z | high_8;
                        }
                    }
                    else if (word_8 == 2)
                    {
                        block_2.z = block_2.z | low_8;
                        if (spills_8)
                        {
                            block_2.w = block_2.w | high_8;
                        }
                    }
                    else
                    {
                        block_2.w = block_2.w | low_8;
                    }
                    pos_1 = pos_1 + bits;
                    const uint value_8 = uint(fit0.q1[c_5]);
                    const int word_9 = pos_1 >> 5;
                    const uint shift_9 = uint(pos_1 & 31);
                    const uint low_9 = value_8 << shift_9;
                    uint high_9;
                    if (shift_9 == 0u)
                    {
                        high_9 = 0u;
                    }
                    else
                    {
                        high_9 = value_8 >> (32u - shift_9);
                    }
                    const bool spills_9 = (pos_1 & 31) + bits > 32;
                    if (word_9 == 0)
                    {
                        block_2.x = block_2.x | low_9;
                        if (spills_9)
                        {
                            block_2.y = block_2.y | high_9;
                        }
                    }
                    else if (word_9 == 1)
                    {
                        block_2.y = block_2.y | low_9;
                        if (spills_9)
                        {
                            block_2.z = block_2.z | high_9;
                        }
                    }
                    else if (word_9 == 2)
                    {
                        block_2.z = block_2.z | low_9;
                        if (spills_9)
                        {
                            block_2.w = block_2.w | high_9;
                        }
                    }
                    else
                    {
                        block_2.w = block_2.w | low_9;
                    }
                    pos_1 = pos_1 + bits;
                    const uint value_9 = uint(fit1.q0[c_5]);
                    const int word_10 = pos_1 >> 5;
                    const uint shift_10 = uint(pos_1 & 31);
                    const uint low_10 = value_9 << shift_10;
                    uint high_10;
                    if (shift_10 == 0u)
                    {
                        high_10 = 0u;
                    }
                    else
                    {
                        high_10 = value_9 >> (32u - shift_10);
                    }
                    const bool spills_10 = (pos_1 & 31) + bits > 32;
                    if (word_10 == 0)
                    {
                        block_2.x = block_2.x | low_10;
                        if (spills_10)
                        {
                            block_2.y = block_2.y | high_10;
                        }
                    }
                    else if (word_10 == 1)
                    {
                        block_2.y = block_2.y | low_10;
                        if (spills_10)
                        {
                            block_2.z = block_2.z | high_10;
                        }
                    }
                    else if (word_10 == 2)
                    {
                        block_2.z = block_2.z | low_10;
                        if (spills_10)
                        {
                            block_2.w = block_2.w | high_10;
                        }
                    }
                    else
                    {
                        block_2.w = block_2.w | low_10;
                    }
                    pos_1 = pos_1 + bits;
                    const uint value_10 = uint(fit1.q1[c_5]);
                    const int word_11 = pos_1 >> 5;
                    const uint shift_11 = uint(pos_1 & 31);
                    const uint low_11 = value_10 << shift_11;
                    uint high_11;
                    if (shift_11 == 0u)
                    {
                        high_11 = 0u;
                    }
                    else
                    {
                        high_11 = value_10 >> (32u - shift_11);
                    }
                    const bool spills_11 = (pos_1 & 31) + bits > 32;
                    if (word_11 == 0)
                    {
                        block_2.x = block_2.x | low_11;
                        if (spills_11)
                        {
                            block_2.y = block_2.y | high_11;
                        }
                    }
                    else if (word_11 == 1)
                    {
                        block_2.y = block_2.y | low_11;
                        if (spills_11)
                        {
                            block_2.z = block_2.z | high_11;
                        }
                    }
                    else if (word_11 == 2)
                    {
                        block_2.z = block_2.z | low_11;
                        if (spills_11)
                        {
                            block_2.w = block_2.w | high_11;
                        }
                    }
                    else
                    {
                        block_2.w = block_2.w | low_11;
                    }
                    pos_1 = pos_1 + bits;
                }
                if (mode == 1)
                {
                    const uint value_11 = uint(fit0.p0);
                    const int word_12 = pos_1 >> 5;
                    const uint shift_12 = uint(pos_1 & 31);
                    const uint low_12 = value_11 << shift_12;
                    uint high_12;
                    if (shift_12 == 0u)
                    {
                        high_12 = 0u;
                    }
                    else
                    {
                        high_12 = value_11 >> (32u - shift_12);
                    }
                    const bool spills_12 = (pos_1 & 31) + 1 > 32;
                    if (word_12 == 0)
                    {
                        block_2.x = block_2.x | low_12;
                        if (spills_12)
                        {
                            block_2.y = block_2.y | high_12;
                        }
                    }
                    else if (word_12 == 1)
                    {
                        block_2.y = block_2.y | low_12;
                        if (spills_12)
                        {
                            block_2.z = block_2.z | high_12;
                        }
                    }
                    else if (word_12 == 2)
                    {
                        block_2.z = block_2.z | low_12;
                        if (spills_12)
                        {
                            block_2.w = block_2.w | high_12;
                        }
                    }
                    else
                    {
                        block_2.w = block_2.w | low_12;
                    }
                    pos_1 = pos_1 + 1;
                    const uint value_12 = uint(fit1.p0);
                    const int word_13 = pos_1 >> 5;
                    const uint shift_13 = uint(pos_1 & 31);
                    const uint low_13 = value_12 << shift_13;
                    uint high_13;
                    if (shift_13 == 0u)
                    {
                        high_13 = 0u;
                    }
                    else
                    {
                        high_13 = value_12 >> (32u - shift_13);
                    }
                    const bool spills_13 = (pos_1 & 31) + 1 > 32;
                    if (word_13 == 0)
                    {
                        block_2.x = block_2.x | low_13;
                        if (spills_13)
                        {
                            block_2.y = block_2.y | high_13;
                        }
                    }
                    else if (word_13 == 1)
                    {
                        block_2.y = block_2.y | low_13;
                        if (spills_13)
                        {
                            block_2.z = block_2.z | high_13;
                        }
                    }
                    else if (word_13 == 2)
                    {
                        block_2.z = block_2.z | low_13;
                        if (spills_13)
                        {
                            block_2.w = block_2.w | high_13;
                        }
                    }
                    else
                    {
                        block_2.w = block_2.w | low_13;
                    }
                    pos_1 = pos_1 + 1;
                }
                else
                {
                    const uint value_13 = uint(fit0.p0);
                    const int word_14 = pos_1 >> 5;
                    const uint shift_14 = uint(pos_1 & 31);
                    const uint low_14 = value_13 << shift_14;
                    uint high_14;
                    if (shift_14 == 0u)
                    {
                        high_14 = 0u;
                    }
                    else
                    {
                        high_14 = value_13 >> (32u - shift_14);
                    }
                    const bool spills_14 = (pos_1 & 31) + 1 > 32;
                    if (word_14 == 0)
                    {
                        block_2.x = block_2.x | low_14;
                        if (spills_14)
                        {
                            block_2.y = block_2.y | high_14;
                        }
                    }
                    else if (word_14 == 1)
                    {
                        block_2.y = block_2.y | low_14;
                        if (spills_14)
                        {
                            block_2.z = block_2.z | high_14;
                        }
                    }
                    else if (word_14 == 2)
                    {
                        block_2.z = block_2.z | low_14;
                        if (spills_14)
                        {
                            block_2.w = block_2.w | high_14;
                        }
                    }
                    else
                    {
                        block_2.w = block_2.w | low_14;
                    }
                    pos_1 = pos_1 + 1;
                    const uint value_14 = uint(fit0.p1);
                    const int word_15 = pos_1 >> 5;
                    const uint shift_15 = uint(pos_1 & 31);
                    const uint low_15 = value_14 << shift_15;
                    uint high_15;
                    if (shift_15 == 0u)
                    {
                        high_15 = 0u;
                    }
                    else
                    {
                        high_15 = value_14 >> (32u - shift_15);
                    }
                    const bool spills_15 = (pos_1 & 31) + 1 > 32;
                    if (word_15 == 0)
                    {
                        block_2.x = block_2.x | low_15;
                        if (spills_15)
                        {
                            block_2.y = block_2.y | high_15;
                        }
                    }
                    else if (word_15 == 1)
                    {
                        block_2.y = block_2.y | low_15;
                        if (spills_15)
                        {
                            block_2.z = block_2.z | high_15;
                        }
                    }
                    else if (word_15 == 2)
                    {
                        block_2.z = block_2.z | low_15;
                        if (spills_15)
                        {
                            block_2.w = block_2.w | high_15;
                        }
                    }
                    else
                    {
                        block_2.w = block_2.w | low_15;
                    }
                    pos_1 = pos_1 + 1;
                    const uint value_15 = uint(fit1.p0);
                    const int word_16 = pos_1 >> 5;
                    const uint shift_16 = uint(pos_1 & 31);
                    const uint low_16 = value_15 << shift_16;
                    uint high_16;
                    if (shift_16 == 0u)
                    {
                        high_16 = 0u;
                    }
                    else
                    {
                        high_16 = value_15 >> (32u - shift_16);
                    }
                    const bool spills_16 = (pos_1 & 31) + 1 > 32;
                    if (word_16 == 0)
                    {
                        block_2.x = block_2.x | low_16;
                        if (spills_16)
                        {
                            block_2.y = block_2.y | high_16;
                        }
                    }
                    else if (word_16 == 1)
                    {
                        block_2.y = block_2.y | low_16;
                        if (spills_16)
                        {
                            block_2.z = block_2.z | high_16;
                        }
                    }
                    else if (word_16 == 2)
                    {
                        block_2.z = block_2.z | low_16;
                        if (spills_16)
                        {
                            block_2.w = block_2.w | high_16;
                        }
                    }
                    else
                    {
                        block_2.w = block_2.w | low_16;
                    }
                    pos_1 = pos_1 + 1;
                    const uint value_16 = uint(fit1.p1);
                    const int word_17 = pos_1 >> 5;
                    const uint shift_17 = uint(pos_1 & 31);
                    const uint low_17 = value_16 << shift_17;
                    uint high_17;
                    if (shift_17 == 0u)
                    {
                        high_17 = 0u;
                    }
                    else
                    {
                        high_17 = value_16 >> (32u - shift_17);
                    }
                    const bool spills_17 = (pos_1 & 31) + 1 > 32;
                    if (word_17 == 0)
                    {
                        block_2.x = block_2.x | low_17;
                        if (spills_17)
                        {
                            block_2.y = block_2.y | high_17;
                        }
                    }
                    else if (word_17 == 1)
                    {
                        block_2.y = block_2.y | low_17;
                        if (spills_17)
                        {
                            block_2.z = block_2.z | high_17;
                        }
                    }
                    else if (word_17 == 2)
                    {
                        block_2.z = block_2.z | low_17;
                        if (spills_17)
                        {
                            block_2.w = block_2.w | high_17;
                        }
                    }
                    else
                    {
                        block_2.w = block_2.w | low_17;
                    }
                    pos_1 = pos_1 + 1;
                }
                for (int i_40 = 0; i_40 < 16; ++i_40)
                {
                    const bool anchored = i_40 == 0 || i_40 == anchor;
                    const uint value_17 = uint(idx_1[i_40]);
                    int count_8;
                    if (anchored)
                    {
                        count_8 = index_bits - 1;
                    }
                    else
                    {
                        count_8 = index_bits;
                    }
                    const int word_18 = pos_1 >> 5;
                    const uint shift_18 = uint(pos_1 & 31);
                    const uint low_18 = value_17 << shift_18;
                    uint high_18;
                    if (shift_18 == 0u)
                    {
                        high_18 = 0u;
                    }
                    else
                    {
                        high_18 = value_17 >> (32u - shift_18);
                    }
                    const bool spills_18 = (pos_1 & 31) + count_8 > 32;
                    if (word_18 == 0)
                    {
                        block_2.x = block_2.x | low_18;
                        if (spills_18)
                        {
                            block_2.y = block_2.y | high_18;
                        }
                    }
                    else if (word_18 == 1)
                    {
                        block_2.y = block_2.y | low_18;
                        if (spills_18)
                        {
                            block_2.z = block_2.z | high_18;
                        }
                    }
                    else if (word_18 == 2)
                    {
                        block_2.z = block_2.z | low_18;
                        if (spills_18)
                        {
                            block_2.w = block_2.w | high_18;
                        }
                    }
                    else
                    {
                        block_2.w = block_2.w | low_18;
                    }
                    pos_1 = pos_1 + count_8;
                }
                const uint4 block_1 = block_2;
                if (error_2 < best_error)
                {
                    best_error = error_2;
                    best = block_1;
                }
            }
        }
    }
    bc_io16_blocks[by * 8 + bx] = best;
}
