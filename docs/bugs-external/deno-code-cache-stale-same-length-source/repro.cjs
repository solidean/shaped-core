// run.py pads this file to 16 KiB and swaps the two functions between runs, so the file keeps its length.
// Both functions are lazily compiled: V8 parses each from its source offset when it is first called.
// That offset is what a stale code cache gets wrong.

function short_one(index) {
  return index + 1;
}

function a_longer_one(index, a1, a2) {
  return index + a1 + a2;
}

console.log("result", short_one(1) + a_longer_one(1, 2, 3));
