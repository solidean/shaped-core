// Does a WebGPU implementation accept this WGSL, as a module AND as a pipeline?
//
// A module-only check reports the opposite of the truth for anything whose rules live on the bind group layout —
// a storage texture's format, a workgroup size against the device's limits — so both halves run here.
//
// Standalone: it reaches for `navigator.gpu`, and under node for the `webgpu` npm package, and for nothing else.
// node binds Dawn (what Chrome ships) and deno carries wgpu (what Firefox ships), so running both covers both engines.

import { readFileSync } from 'node:fs';

async function webgpu()
{
    if (globalThis.navigator?.gpu)
        return globalThis.navigator.gpu;

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

// The first @compute function the text declares; a pipeline has to be built for one by name.
function entryOf(code)
{
    const m = code.match(/@compute[\s\S]*?fn\s+([A-Za-z_][A-Za-z0-9_]*)/);
    return m ? m[1] : undefined;
}

const files = process.argv.slice(2);
if (files.length === 0)
{
    console.log('usage: wgsl-probe.mjs <file.wgsl>...');
    globalThis.process?.exit?.(2);
}

const gpu = await webgpu();
if (!gpu)
{
    console.log('SKIP: this runtime has no WebGPU (node needs the `webgpu` package on NODE_PATH)');
    globalThis.process?.exit?.(2);
}

const adapter = await gpu.requestAdapter();
if (!adapter)
{
    console.log('SKIP: no adapter');
    globalThis.process?.exit?.(2);
}
const device = await adapter.requestDevice();

let failures = 0;
for (const file of files)
{
    const code = readFileSync(file, 'utf8');

    device.pushErrorScope('validation');
    const module = device.createShaderModule({ code });
    const info = await module.getCompilationInfo();
    const scoped = await device.popErrorScope();
    const errors = Array.from(info.messages).filter(m => m.type === 'error');
    if (errors.length > 0 || scoped)
    {
        const text = errors.map(m => m.message).join(' | ') || String(scoped.message);
        console.log(`MODULE  ${file}  :: ${text.replace(/\s+/g, ' ').slice(0, 300)}`);
        ++failures;
        continue;
    }

    const entry = entryOf(code);
    if (!entry)
    {
        console.log(`MODULE-OK  ${file}  (no @compute entry point, so no pipeline was built)`);
        continue;
    }

    // createComputePipelineAsync REJECTS rather than raising a device error, so the rejection is the result.
    let failed = null;
    device.pushErrorScope('validation');
    try
    {
        await device.createComputePipelineAsync({ layout: 'auto', compute: { module, entryPoint: entry } });
    }
    catch (e)
    {
        failed = String(e.message || e);
    }
    const pipelineError = await device.popErrorScope();
    if (!failed && pipelineError)
        failed = String(pipelineError.message);

    if (failed)
    {
        console.log(`PIPELINE  ${file}  :: ${failed.replace(/\s+/g, ' ').slice(0, 300)}`);
        ++failures;
    }
    else
    {
        console.log(`PASS  ${file}  (module and compute pipeline for '${entry}')`);
    }
}

globalThis.process?.exit?.(failures === 0 ? 0 : 1);
