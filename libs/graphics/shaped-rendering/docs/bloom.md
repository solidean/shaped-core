# Bloom

`sr::bloom_routine` scatters light as a lens does, on a linear image.
This is its definition, and how close the routine comes to it.

## The definition

$$ L_\text{out} = L - s\,S(L) + s\,(k * S(L)) $$

- $L$ is linear light, and $s$ the share that scatters: `bloom_settings::strength`.
- $k$ is a kernel summing to 1: four Gaussians of 1, 3, 8 and 20% of the image's height, weighted 0.4, 0.3, 0.2 and 0.1.
  `radius_scale` scales the radii.
- $S(L)$ is the light above a knee, with a soft transition half the knee wide to each side, and its hue kept.

A pixel gives up what it scatters, and the image is mirrored at its edges, so total light is unchanged.
That is what sets this apart from a threshold, which adds light.

The knee is `bloom_settings::knee`, on light as shown after exposure, so its default of 1 is display white.
Whatever the display can show then stays true, and gains only what lands on it.
A knee of 0 is the pure lens form, $S(L) = L$, in which every pixel scatters and the whole image takes on a veil.

The kernel's numbers are a choice, not a measurement of a lens.

## The routine

A chain of half-size images carries the light that scatters, and is added back up with a weight per level.
Level $m$ carries a blur of about $2.16 \cdot 2^m$ pixels, measured on an impulse.
Each Gaussian of the kernel is shared between the two levels around its variance, by `level_weights`.

The input is any linear float image, and the output a transient `rgba16_float` image of the same size.

## How close it comes

`bloom-test.cc` holds the routine to the exact convolution, computed on the CPU, on a bright window in a dim room at 640x360.
The final image is within 1% at the median and within 5% at the 99th percentile, and total light within 0.5%.
The worst pixels are right at hard edges.

An image under about 220 pixels tall cannot represent the kernel, since its narrowest Gaussian is finer than the chain's finest level, and misses that bound.

## Cost

GPU time per image, on dx12, from the benchmark in `bloom-test.cc`:

| Size | Median |
|---|---|
| 1920x1080 | 0.23 ms |
| 3840x2160 | 0.84 ms |

These are from one machine's default adapter, so they say how far under a frame this is, not what it costs elsewhere.
