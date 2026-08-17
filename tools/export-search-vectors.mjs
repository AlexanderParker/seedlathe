// Exports golden similarity scores from the demo page's compareInstruments, so
// the C++ port can be asserted against them. A score has to mean the same thing
// in the plugin as it does on the demo page, or "92% match" is meaningless.

import { readFileSync, writeFileSync, mkdirSync } from "node:fs";
import { execSync } from "node:child_process";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const ZYN = process.env.ZYN_DIR ?? "C:/dev/zyn";
const OUT = resolve(here, "../vectors/search-scores.json");

const Z = eval(readFileSync(resolve(ZYN, "zyn-unminified.js"), "utf8") + "; Z");

// demo.js touches the DOM at load, so lift the scorer out by text rather than
// evaluating the whole file.
const demo = readFileSync(resolve(ZYN, "demo.js"), "utf8");
const start = demo.indexOf("  compareInstruments(a, b) {");
if (start < 0) throw new Error("compareInstruments not found in demo.js");
const end = demo.indexOf("\n  handleFindSimilar(", start);
if (end < 0) throw new Error("end of compareInstruments not found");
const body = demo.slice(start, end);

const compareInstruments = eval(`(function(){ const o = { ${body} }; return o.compareInstruments; })()`);

// Pairs chosen to exercise every branch of the scorer: same type and different,
// matching and mismatched oscillator counts, FM matrix present on one side,
// both, and neither.
const pairs = [];
const seeds = [];
let x = 4242;
for (let i = 0; i < 120; i++) {
  x = (1103515245 * x + 12345) % 4294967296;
  seeds.push(x);
}
for (let i = 0; i < seeds.length; i++) {
  pairs.push([seeds[i], seeds[(i + 1) % seeds.length]]);
  pairs.push([seeds[i], seeds[(i + 37) % seeds.length]]);
}
// Identical instruments must score exactly 100.
for (const s of seeds.slice(0, 20)) pairs.push([s, s]);
// Every type digit against every other, so the type-mismatch paths are covered.
for (let a = 0; a <= 9; a++)
  for (let b = 0; b <= 9; b++)
    pairs.push([1230 + a, 4560 + b]);

const zynCommit = execSync(`git -C "${ZYN}" rev-parse HEAD`).toString().trim();

const out = pairs.map(([a, b]) => ({
  a, b,
  score: compareInstruments(Z.getInstrument(a), Z.getInstrument(b)),
}));

mkdirSync(dirname(OUT), { recursive: true });
writeFileSync(OUT, JSON.stringify({ zynCommit, count: out.length, pairs: out }));

const perfect = out.filter((p) => p.a === p.b);
console.log(`wrote ${out.length} score pairs from zyn ${zynCommit.slice(0, 8)}`);
console.log(`identical-instrument scores: ${[...new Set(perfect.map((p) => p.score))].join(", ")}`);
