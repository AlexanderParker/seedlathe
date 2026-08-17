// Renders every core seed three times and reports the worst run-to-run
// divergence, relative to that seed's peak. Distinguishes harmless last-bit
// float wobble from chaotic amplification through the compressor.
import { readdirSync } from "node:fs";
import { resolve, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import puppeteer from "puppeteer-core";

const here = dirname(fileURLToPath(import.meta.url));
const ZYN = process.env.ZYN_DIR ?? "C:/dev/zyn";
const CHROME = process.env.CHROME_PATH ??
  "C:/Program Files/Google/Chrome/Application/chrome.exe";
const DIR = resolve(here, "../vectors/audio");

const seeds = readdirSync(DIR).filter((f) => f.endsWith(".wav"))
  .map((f) => Number(f.replace(".wav", ""))).sort((a, b) => a - b);

const browser = await puppeteer.launch({
  executablePath: CHROME, headless: "new",
  args: ["--autoplay-policy=no-user-gesture-required"],
});
const page = await browser.newPage();
await page.setContent("<html><body></body></html>");
await page.addScriptTag({ path: resolve(ZYN, "zyn-unminified.js") });

async function render(seed) {
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

const rows = [];
for (const seed of seeds) {
  const runs = [await render(seed), await render(seed), await render(seed)];
  let peak = 0;
  for (const v of runs[0]) peak = Math.max(peak, Math.abs(v));
  let worst = 0;
  for (let i = 0; i < runs[0].length; i++) {
    worst = Math.max(worst,
      Math.abs(runs[0][i] - runs[1][i]),
      Math.abs(runs[0][i] - runs[2][i]),
      Math.abs(runs[1][i] - runs[2][i]));
  }
  const db = peak > 0 ? 20 * Math.log10(Math.max(worst, 1e-20) / peak) : -Infinity;
  rows.push({ seed, db });
}
await browser.close();

rows.sort((a, b) => b.db - a.db);
console.log("worst run-to-run divergence, relative to peak (3 renders each):");
for (const r of rows.slice(0, 20))
  console.log(`  seed ${String(r.seed).padEnd(11)} ${r.db === -Infinity ? "identical" : r.db.toFixed(1) + " dB"}`);

const buckets = { "identical": 0, "<-120dB": 0, "-120..-60": 0, "-60..-20": 0, ">-20dB": 0 };
for (const r of rows) {
  if (r.db === -Infinity) buckets["identical"]++;
  else if (r.db < -120) buckets["<-120dB"]++;
  else if (r.db < -60) buckets["-120..-60"]++;
  else if (r.db < -20) buckets["-60..-20"]++;
  else buckets[">-20dB"]++;
}
console.log("\ndistribution:", JSON.stringify(buckets));
