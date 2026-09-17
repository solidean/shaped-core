// Does a WebGPU implementation accept 4294967295 as a pass timestamp write index?
//
// The spec says a provided index must be less than the query set's count, so every implementation should refuse it.
// Dawn accepts it, because 4294967295 is the sentinel its own C API uses for "no write at that end of the pass".
//
// Standalone: it reaches for `navigator.gpu`, and under node for the `webgpu` npm package, and for nothing else.

async function webgpu()
{
    if (globalThis.navigator?.gpu)
        return globalThis.navigator.gpu;

    // node has no WebGPU of its own; the `webgpu` package binds Dawn.
    try
    {
        const { createRequire } = await import('node:module');
        const { create, globals } = createRequire(import.meta.url)('webgpu');
        Object.assign(globalThis, globals);
        return create([]);
    }
    catch
    {
        return undefined;
    }
}

const gpu = await webgpu();
if (!gpu)
{
    console.log('SKIP: this runtime has no WebGPU (node needs the `webgpu` package on NODE_PATH)');
    globalThis.process?.exit?.(2);
}

const adapter = await gpu.requestAdapter();
if (!adapter || !adapter.features.has('timestamp-query'))
{
    console.log('SKIP: no adapter with timestamp-query');
    globalThis.process?.exit?.(2);
}

const device = await adapter.requestDevice({ requiredFeatures: ['timestamp-query'] });
const querySet = device.createQuerySet({ type: 'timestamp', count: 32 });

async function attempt(label, timestampWrites)
{
    device.pushErrorScope('validation');
    const encoder = device.createCommandEncoder();
    try
    {
        encoder.beginComputePass({ timestampWrites }).end();
        device.queue.submit([encoder.finish()]);
    }
    catch (e)
    {
        await device.popErrorScope();
        console.log(`${label}: REFUSED (threw ${e.name})`);
        return;
    }
    const error = await device.popErrorScope();
    console.log(`${label}: ${error ? 'REFUSED (' + error.message.split('\n')[0] + ')' : 'ACCEPTED'}`);
}

// What emdawnwebgpu passes for WGPU_QUERY_SET_INDEX_UNDEFINED, which means "no write at that end of the pass".
await attempt('end = 4294967295 ', { querySet, beginningOfPassWriteIndex: 0, endOfPassWriteIndex: 4294967295 });
// What it should pass instead: the field simply absent.
await attempt('end omitted      ', { querySet, beginningOfPassWriteIndex: 0 });
// The same out-of-range shape one past the end, to show the value is what decides rather than the field.
await attempt('begin = 32 of 32 ', { querySet, beginningOfPassWriteIndex: 32, endOfPassWriteIndex: 1 });

globalThis.process?.exit?.(0);
