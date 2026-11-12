// Draws one multisampled pixel, with or without writing @builtin(sample_mask), and optionally reads its samples back.
// Usage: deno run --allow-all --unstable-webgpu repro.mjs <mask|nomask> [read]
const args = typeof Deno !== "undefined" ? Deno.args : process.argv.slice(2);
const mask = args[0] === "mask";
const read = args[1] === "read";

const gpu = navigator.gpu;
const adapter = await gpu.requestAdapter();
console.log("adapter", adapter.info?.description ?? adapter.info?.vendor);
const device = await adapter.requestDevice();

const pixel = mask
    ? `struct O { @location(0) c: vec4f, @builtin(sample_mask) m: u32 }
       @fragment fn fs() -> O { return O(vec4f(1, 0, 0, 1), 5u); }`
    : `@fragment fn fs() -> @location(0) vec4f { return vec4f(1, 0, 0, 1); }`;
const module = device.createShaderModule({
    code: `@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {
               var p = array(vec2f(-1, -1), vec2f(3, -1), vec2f(-1, 3));
               return vec4f(p[i], 0.5, 1);
           }
           ${pixel}`,
});
const pipeline = await device.createRenderPipelineAsync({
    layout: "auto",
    vertex: { module, entryPoint: "vs" },
    fragment: { module, entryPoint: "fs", targets: [{ format: "rgba8unorm" }] },
    multisample: { count: 4 },
});
console.log("pipeline built");

const target = device.createTexture({
    size: [1, 1],
    format: "rgba8unorm",
    sampleCount: 4,
    usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.TEXTURE_BINDING,
});
const encoder = device.createCommandEncoder();
const pass = encoder.beginRenderPass({
    colorAttachments: [{ view: target.createView(), clearValue: [0, 0, 0, 1], loadOp: "clear", storeOp: "store" }],
});
pass.setPipeline(pipeline);
pass.draw(3);
pass.end();

let readback;
if (read) {
    const reader = device.createShaderModule({
        code: `@group(0) @binding(0) var t: texture_multisampled_2d<f32>;
               @group(0) @binding(1) var<storage, read_write> v: array<f32, 4>;
               @compute @workgroup_size(1) fn main() {
                   for (var s = 0; s < 4; s++) { v[s] = textureLoad(t, vec2i(0, 0), s).x; }
               }`,
    });
    const compute = await device.createComputePipelineAsync({ layout: "auto", compute: { module: reader, entryPoint: "main" } });
    const values = device.createBuffer({ size: 16, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC });
    readback = device.createBuffer({ size: 16, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
    const group = device.createBindGroup({
        layout: compute.getBindGroupLayout(0),
        entries: [{ binding: 0, resource: target.createView() }, { binding: 1, resource: { buffer: values } }],
    });
    const cpass = encoder.beginComputePass();
    cpass.setPipeline(compute);
    cpass.setBindGroup(0, group);
    cpass.dispatchWorkgroups(1);
    cpass.end();
    encoder.copyBufferToBuffer(values, 0, readback, 0, 16);
}
device.queue.submit([encoder.finish()]);
console.log("submitted");
await device.queue.onSubmittedWorkDone();
if (read) {
    await readback.mapAsync(GPUMapMode.READ);
    console.log("samples", [...new Float32Array(readback.getMappedRange())]);
}
console.log("done");
