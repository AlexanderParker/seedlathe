// How big is the run-to-run variation in Chrome's OfflineAudioContext output?
// Byte-identical would be ideal; what actually matters is whether the variation
// is far below the fidelity threshold.
import { resolve } from "node:path";
import puppeteer from "puppeteer-core";

const ZYN = process.env.ZYN_DIR ?? "C:/dev/zyn";
const CHROME = process.env.CHROME_PATH ??
  "C:/Program Files/Google/Chrome/Application/chrome.exe";
const seeds = [1494359974, 999169202, 478693073];

async function renderIn(page, seed) {
  return page.evaluate(async (seed) => {
    const SR = 48000, SECONDS = 2;
    const offline = new OfflineAudioContext(2, SR * SECONDS, SR);
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
    Z.warmedUp = true;
    Z.fxNodes = {};
    Z.noiseBuffer = null;
    Z.activeVoices = {};
    let a = 0x5eed1a7e >>> 0;
    Z.randSample = () => {
      a = (a + 0x6d2b79f5) >>> 0;
      let t = a;
      t = Math.imul(t ^ (t >>> 15), t | 1);
      t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
      return ((((t ^ (t >>> 14)) >>> 0) / 4294967296) * 2) - 1;
    };
    Z.play(0, Z.getInstrument(seed), 1.0);
    const buf = await offline.startRendering();
    return Array.from(buf.getChannelData(0));
  }, seed);
}

function compare(a, b) {
  let maxAbs = 0, peak = 0, firstIdx = -1;
  for (let i = 0; i < a.length; i++) {
    peak = Math.max(peak, Math.abs(a[i]));
    const d = Math.abs(a[i] - b[i]);
    if (d > maxAbs) maxAbs = d;
    if (d !== 0 && firstIdx < 0) firstIdx = i;
  }
  return { maxAbs, peak, firstIdx, relDb: 20 * Math.log10(Math.max(maxAbs, 1e-20) / Math.max(peak, 1e-20)) };
}

const launch = () => puppeteer.launch({
  executablePath: CHROME, headless: "new",
  args: ["--autoplay-policy=no-user-gesture-required"],
});

const b1 = await launch();
const p1 = await b1.newPage();
await p1.setContent("<html><body></body></html>");
await p1.addScriptTag({ path: resolve(ZYN, "zyn-unminified.js") });

for (const seed of seeds) {
  const a = await renderIn(p1, seed);
  const b = await renderIn(p1, seed);
  const c = compare(a, b);
  console.log(`seed ${String(seed).padEnd(11)} same page   maxDiff ${c.maxAbs.toExponential(3)}  peak ${c.peak.toFixed(4)}  rel ${c.relDb.toFixed(1)} dB  firstDiff@${c.firstIdx}`);
}
await b1.close();

const b2 = await launch();
const p2 = await b2.newPage();
await p2.setContent("<html><body></body></html>");
await p2.addScriptTag({ path: resolve(ZYN, "zyn-unminified.js") });
const b3 = await launch();
const p3 = await b3.newPage();
await p3.setContent("<html><body></body></html>");
await p3.addScriptTag({ path: resolve(ZYN, "zyn-unminified.js") });

for (const seed of seeds) {
  const a = await renderIn(p2, seed);
  const b = await renderIn(p3, seed);
  const c = compare(a, b);
  console.log(`seed ${String(seed).padEnd(11)} two browsers maxDiff ${c.maxAbs.toExponential(3)}  peak ${c.peak.toFixed(4)}  rel ${c.relDb.toFixed(1)} dB  firstDiff@${c.firstIdx}`);
}
await b2.close();
await b3.close();
