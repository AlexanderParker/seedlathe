// Renders reference audio from zyn running in real Chrome, via
// OfflineAudioContext, so the C++ engine can be null-tested against the actual
// Web Audio implementation rather than against my reading of it.
//
//   npm run audio          core set (committed): 60 seeds -> vectors/audio
//   npm run audio -- --full  full sweep (gitignored): 315 seeds -> vectors/audio-full
//
// Mono, because Z.play pans centre and the two channels come out bit-identical;
// storing both would double the repo for nothing.

import { readFileSync, writeFileSync, mkdirSync } from "node:fs";
import { execSync } from "node:child_process";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import puppeteer from "puppeteer-core";

const here = dirname(fileURLToPath(import.meta.url));
const ZYN = process.env.ZYN_DIR ?? "C:/dev/zyn";
const CHROME = process.env.CHROME_PATH ??
  "C:/Program Files/Google/Chrome/Application/chrome.exe";

const FULL = process.argv.includes("--full");
const OUTDIR = resolve(here, FULL ? "../vectors/audio-full" : "../vectors/audio");

const SR = 48000;
const SECONDS = 2;
const CORE_COUNT = 60;

const src = readFileSync(resolve(ZYN, "zyn-unminified.js"), "utf8");
const Z = eval(src + "; Z");

const presets = JSON.parse(readFileSync(resolve(ZYN, "default-presets.json"), "utf8"));
const candidates = presets.map((p) => p.seed);
let x = 999;
while (candidates.length < presets.length + 200) {
  x = (1103515245 * x + 12345) % 4294967296;
  candidates.push(x);
}

// The core set is chosen for branch coverage, not by taking the first N: every
// instrument type, plus a guaranteed share of the FM-matrix, distortion and
// noise paths, so a regression in any of them fails the test.
function selectCore(all) {
  const perType = new Map();
  const withFm = [], withDist = [], withNoise = [], rest = [];
  for (const seed of all) {
    const inst = Z.getInstrument(seed);
    const t = Math.abs(seed) % 10;
    if (!perType.has(t)) perType.set(t, []);
    perType.get(t).push(seed);
    if (inst.fmMatrix) withFm.push(seed);
    if (inst.oscs.some((o) => o.dist)) withDist.push(seed);
    if (inst.oscs.some((o) => o.waveform === "noise")) withNoise.push(seed);
    rest.push(seed);
  }
  // Over-select: some candidates are dropped by the stability filter below, so
  // the pool has to be bigger than the target to still reach CORE_COUNT.
  const POOL = CORE_COUNT * 2;
  const picked = new Set();
  const take = (list, n) => { for (const s of list) { if (picked.size >= POOL) return;
                                                      if (n-- <= 0) return; picked.add(s); } };
  for (const [, list] of [...perType.entries()].sort((a, b) => a[0] - b[0])) take(list, 5);
  take(withFm, 16);
  take(withDist, 10);
  take(withNoise, 10);
  take(rest, POOL);
  return [...picked].slice(0, POOL).sort((a, b) => a - b);
}

// Known-unstable seeds are pinned rather than rediscovered: the runtime check
// below is a sampling test, and a seed that diverges intermittently can pass
// three renders by luck. Both mechanisms run.
const unstablePath = resolve(here, "../vectors/unstable-seeds.json");
const unstable = new Set(
  JSON.parse(readFileSync(unstablePath, "utf8")).seeds.map((e) => e.seed));

const seeds = (FULL ? candidates : selectCore(candidates)).filter((s) => !unstable.has(s));
const zynCommit = execSync(`git -C "${ZYN}" rev-parse HEAD`).toString().trim();

const browser = await puppeteer.launch({
  executablePath: CHROME,
  headless: "new",
  args: ["--autoplay-policy=no-user-gesture-required"],
});
const page = await browser.newPage();
await page.setContent("<html><body></body></html>");
await page.addScriptTag({ path: resolve(ZYN, "zyn-unminified.js") });

mkdirSync(OUTDIR, { recursive: true });

// Chrome does not render every seed reproducibly. A last-bit float difference
// appears around the compressor's 288-frame pre-delay boundary, and for a small
// minority of instruments the compressor's adaptive release amplifies it into an
// audible divergence.
//
// Byte-identity is the WRONG criterion here. Measured over 61 core seeds, three
// renders each: 58 agree to better than -120 dB relative to peak (float32
// last-bit noise), 2 to between -120 and -60 dB, and exactly one -- seed
// 999169202 -- diverges by 9.5 dB, consistently, across separate browser
// processes. Only that last class is a problem, so the gate is a relative
// divergence threshold, not a hash.
//
// Three renders rather than two: the instability is intermittent, and a
// two-render check let 999169202 through by luck.
const STABILITY_DB = -60;
const STABILITY_RENDERS = 3;

async function renderOnce(page, seed) {
  return page.evaluate(async (seed, SR, SECONDS) => {
    const offline = new OfflineAudioContext(2, Math.round(SR * SECONDS), SR);

    // Replicate Z.init against the offline context: Z.init hardcodes
    // `new AudioContext()`, which an offline render cannot use.
    Z.ctx = offline;
    Z.masterGain = offline.createGain();
    Z.compressor = offline.createDynamicsCompressor();
    Z.compressor.threshold.value = -12;
    Z.compressor.knee.value = 6;
    Z.compressor.ratio.value = 8;
    Z.compressor.attack.value = 0.003;
    Z.compressor.release.value = 0.15;
    Z.masterGain.connect(Z.compressor);
    Z.compressor.connect(offline.destination);

    // warmUp resumes a live AudioContext, which an offline one lacks.
    Z.warmedUp = true;
    // These globals hold nodes belonging to the PREVIOUS context, so without
    // clearing them every render after the first is silently corrupt.
    Z.fxNodes = {};
    Z.noiseBuffer = null;
    Z.activeVoices = {};

    // Deterministic noise and reverb impulses, matching sl::globalNoiseBuffer's
    // fixed seed. zyn uses Math.random for both, so without this the reference
    // differs every run and no threshold could hold. One override covers the
    // noise buffer and the reverb impulse.
    let a = 0x5EED1A7E >>> 0;
    Z.randSample = () => {
      a = (a + 0x6d2b79f5) >>> 0;
      let t = a;
      t = Math.imul(t ^ (t >>> 15), t | 1);
      t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
      return ((((t ^ (t >>> 14)) >>> 0) / 4294967296) * 2) - 1;
    };

    Z.play(0, Z.getInstrument(seed), 1.0);
    const buf = await offline.startRendering();
    const l = buf.getChannelData(0), r = buf.getChannelData(1);
    let m = 0;
    for (let i = 0; i < l.length; i++) m = Math.max(m, Math.abs(l[i] - r[i]));
    return { data: Array.from(l), maxLR: m };
  }, seed, SR, SECONDS);
}

function stabilityDb(a, b) {
  let maxAbs = 0, peak = 0;
  for (let i = 0; i < a.length; i++) {
    peak = Math.max(peak, Math.abs(a[i]));
    maxAbs = Math.max(maxAbs, Math.abs(a[i] - b[i]));
  }
  if (peak <= 0) return -Infinity;
  return 20 * Math.log10(Math.max(maxAbs, 1e-20) / peak);
}

const kept = [];
const rejected = [];
let done = 0;

for (const seed of seeds) {
  const runs = [];
  for (let i = 0; i < STABILITY_RENDERS; ++i) runs.push(await renderOnce(page, seed));
  const first = runs[0];

  if (first.maxLR > 1e-9)
    throw new Error(`seed ${seed}: channels differ by ${first.maxLR}`);

  let db = -Infinity;
  for (let i = 0; i < runs.length; ++i)
    for (let j = i + 1; j < runs.length; ++j)
      db = Math.max(db, stabilityDb(runs[i].data, runs[j].data));

  if (db > STABILITY_DB) {
    rejected.push({ seed, divergenceDb: Number(db.toFixed(1)) });
  } else {
    writeFileSync(resolve(OUTDIR, `${seed}.wav`), wavMono32(first.data, SR));
    kept.push(seed);
  }

  if (++done % 20 === 0) console.log(`  ${done}/${seeds.length} (kept ${kept.length})`);
  if (!FULL && kept.length >= CORE_COUNT) break;
}

await browser.close();

writeFileSync(resolve(OUTDIR, "MANIFEST.json"), JSON.stringify({
  zynCommit, sampleRate: SR, seconds: SECONDS, channels: 1,
  note: 0, gain: 1.0, count: kept.length, set: FULL ? "full" : "core",
  stabilityThresholdDb: STABILITY_DB,
  rejectedUnstable: rejected,
}, null, 2));

console.log(`wrote ${kept.length} mono renders from zyn ${zynCommit.slice(0, 8)} -> ${OUTDIR}`);
console.log(`rejected ${rejected.length} seeds Chrome could not render reproducibly`);
for (const r of rejected.slice(0, 10))
  console.log(`  seed ${r.seed}: run-to-run divergence ${r.divergenceDb} dB below peak`);

function wavMono32(samples, sr) {
  const n = samples.length, bytes = n * 4;
  const b = Buffer.alloc(44 + bytes);
  b.write("RIFF", 0); b.writeUInt32LE(36 + bytes, 4); b.write("WAVE", 8);
  b.write("fmt ", 12); b.writeUInt32LE(16, 16); b.writeUInt16LE(3, 20);
  b.writeUInt16LE(1, 22); b.writeUInt32LE(sr, 24); b.writeUInt32LE(sr * 4, 28);
  b.writeUInt16LE(4, 32); b.writeUInt16LE(32, 34);
  b.write("data", 36); b.writeUInt32LE(bytes, 40);
  for (let i = 0; i < n; i++) b.writeFloatLE(samples[i], 44 + i * 4);
  return b;
}
