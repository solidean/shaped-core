#pragma once

// The view's environment probe: an order-3 real spherical-harmonics radiance function (16 RGB coefficients),
// evaluated along a ray direction to give the radiance a missed ray sees. Mirrors sv::background_gpu.
//
// Each coefficient occupies a full 16-byte lane (.rgb = radiance, .a unused) because HLSL pads cbuffer array
// elements to a float4 lane; index 0 is the constant (DC) term. Bound at b1, separate from the b0 frame block.
struct Background
{
    float4 sh[16];
};

ConstantBuffer<Background> background : register(b1);

// Reconstructs radiance L(d) = sum_i sh[i] * Y_i(d) from the standard real-SH basis up to order 3.
// `d` must be a unit direction. Negative lobes are clamped away — radiance can't go below zero.
float3 background_radiance(float3 d)
{
    float const x = d.x;
    float const y = d.y;
    float const z = d.z;

    // Real-SH basis Y_i(d), band by band (l = 0..3): the constants fold in the Condon-Shortley phase.
    float y_basis[16];
    y_basis[0] = 0.282095;

    y_basis[1] = 0.488603 * y;
    y_basis[2] = 0.488603 * z;
    y_basis[3] = 0.488603 * x;

    y_basis[4] = 1.092548 * x * y;
    y_basis[5] = 1.092548 * y * z;
    y_basis[6] = 0.315392 * (3.0 * z * z - 1.0);
    y_basis[7] = 1.092548 * x * z;
    y_basis[8] = 0.546274 * (x * x - y * y);

    y_basis[9] = 0.590044 * y * (3.0 * x * x - y * y);
    y_basis[10] = 2.890611 * x * y * z;
    y_basis[11] = 0.457046 * y * (5.0 * z * z - 1.0);
    y_basis[12] = 0.373176 * z * (5.0 * z * z - 3.0);
    y_basis[13] = 0.457046 * x * (5.0 * z * z - 1.0);
    y_basis[14] = 1.445306 * z * (x * x - y * y);
    y_basis[15] = 0.590044 * x * (x * x - 3.0 * y * y);

    float3 L = float3(0, 0, 0);
    [unroll] for (int i = 0; i < 16; ++i)
        L += background.sh[i].rgb * y_basis[i];

    return max(L, float3(0, 0, 0));
}

// Diffuse irradiance E(n) from the SH environment, via the clamped-cosine convolution (Ramamoorthi &
// Hanrahan, "An Efficient Representation for Irradiance Environment Maps", 2001). Only bands 0..2 contribute —
// the cosine kernel's band-3 term vanishes. For the Lambertian exitance, multiply by albedo and divide by PI.
float3 background_irradiance(float3 n)
{
    // Convolution constants: the clamped-cosine SH coefficients folded with the basis normalization.
    float const c1 = 0.429043;
    float const c2 = 0.511664;
    float const c3 = 0.743125;
    float const c4 = 0.886227;
    float const c5 = 0.247708;

    float3 const L00 = background.sh[0].rgb;
    float3 const L1m1 = background.sh[1].rgb; // y
    float3 const L10 = background.sh[2].rgb;  // z
    float3 const L11 = background.sh[3].rgb;  // x
    float3 const L2m2 = background.sh[4].rgb; // x*y
    float3 const L2m1 = background.sh[5].rgb; // y*z
    float3 const L20 = background.sh[6].rgb;  // 3z^2 - 1
    float3 const L21 = background.sh[7].rgb;  // x*z
    float3 const L22 = background.sh[8].rgb;  // x^2 - y^2

    float const x = n.x;
    float const y = n.y;
    float const z = n.z;

    return c4 * L00                                             //
         + 2.0 * c2 * (L11 * x + L1m1 * y + L10 * z)            // band 1
         + c1 * L22 * (x * x - y * y) + c3 * L20 * (z * z) - c5 * L20
         + 2.0 * c1 * (L2m2 * x * y + L21 * x * z + L2m1 * y * z); // band 2
}
