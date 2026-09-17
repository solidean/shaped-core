// Gives node a WebGPU the way a browser has one: `navigator.gpu`, backed by Dawn through the `webgpu` npm package.
//
// dev.py loads this with `node --import` ahead of every wasm artifact it launches, so it must cost nothing for the many that never touch a GPU.
// The binding is therefore required on first access to `navigator.gpu`, not here.
//
// Where the package is not installed, `navigator.gpu` stays undefined and a WebGPU test SKIPs for want of an adapter, as it would in a browser without WebGPU.
// The package is pinned in the package.json beside this file.

import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);

let gpu;
let loaded = false;

function load() {
    if (loaded)
        return gpu;
    loaded = true;
    try {
        const { create, globals } = require('webgpu');
        Object.assign(globalThis, globals);
        gpu = create([]);
    } catch (e) {
        if (e && e.code !== 'MODULE_NOT_FOUND')
            throw e;
        gpu = undefined;
    }
    return gpu;
}

const navigator = globalThis.navigator ?? {};
Object.defineProperty(navigator, 'gpu', { configurable: true, enumerable: true, get: load });
if (globalThis.navigator !== navigator)
    Object.defineProperty(globalThis, 'navigator', { configurable: true, enumerable: true, value: navigator });
