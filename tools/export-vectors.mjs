// Exports golden instrument trees from zyn.js so the C++ port can be asserted
// against them field-for-field as exact doubles.
//
// Usage: npm run vectors      (ZYN_DIR overrides the zyn repo location)

import { readFileSync, writeFileSync, mkdirSync } from "node:fs";
import { execSync } from "node:child_process";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const ZYN = process.env.ZYN_DIR ?? "C:/dev/zyn";
const OUT = resolve(here, "../vectors/instruments.json");

const src = readFileSync(resolve(ZYN, "zyn-unminified.js"), "utf8");
const Z = eval(src + "; Z");

const zynCommit = execSync(`git -C "${ZYN}" rev-parse HEAD`).toString().trim();

// Coverage matters more than count. The last digit selects the instrument type
// and the first decimal digit sets the FM-matrix probability, so sweep both
// axes explicitly rather than hoping a random sample hits every branch.
const seeds = new Set();
for (let type = 0; type <= 9; type++) {
  for (let first = 0; first <= 9; first++) {
    for (let k = 0; k < 15; k++) {
      const mid = String(k).padStart(4, "0");
      const s = Number(`${first}${mid}${type}`);
      if (s <= 4294967295) seeds.add(s);
    }
  }
}

// Deterministic spread across the 32-bit range, so the vector set is
// reproducible rather than depending on Math.random.
let x = 12345;
while (seeds.size < 2000) {
  x = (1103515245 * x + 12345) % 4294967296;
  seeds.add(x);
}

const list = [...seeds].sort((a, b) => a - b);
const payload = {
  zynCommit,
  generated: new Date().toISOString(),
  count: list.length,
  seeds: list.map((seed) => ({ seed, instrument: Z.getInstrument(seed) })),
};

mkdirSync(dirname(OUT), { recursive: true });
// JSON.stringify emits the shortest round-tripping form for a double, so the
// values reload bit-identically.
writeFileSync(OUT, JSON.stringify(payload));
console.log(`wrote ${list.length} seeds from zyn ${zynCommit.slice(0, 8)}`);
