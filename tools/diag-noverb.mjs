// Renders one seed in Chrome with reverb stripped, so the voice path can be
// compared against the C++ engine in isolation from the convolver.
import { writeFileSync } from "node:fs";
import { resolve, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import puppeteer from "puppeteer-core";

const here = dirname(fileURLToPath(import.meta.url));
const ZYN = process.env.ZYN_DIR ?? "C:/dev/zyn";
const CHROME = process.env.CHROME_PATH ??
  "C:/Program Files/Google/Chrome/Application/chrome.exe";
const seed = Number(process.argv[2] ?? 125033892);

const browser = await puppeteer.launch({
  executablePath: CHROME, headless: "new",
  args: ["--autoplay-policy=no-user-gesture-required"],
});
const page = await browser.newPage();
await page.setContent("<html><body></body></html>");
await page.addScriptTag({ path: resolve(ZYN, "zyn-unminified.js") });

const data = await page.evaluate(async (seed) => {
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

  const inst = Z.getInstrument(seed);
  inst.oscs.forEach((o) => { o.fx.verb = null; });
  Z.play(0, inst, 1.0);
  const buf = await offline.startRendering();
  return Array.from(buf.getChannelData(0));
}, seed);

await browser.close();

const n = data.length, bytes = n * 4;
const b = Buffer.alloc(44 + bytes);
b.write("RIFF", 0); b.writeUInt32LE(36 + bytes, 4); b.write("WAVE", 8);
b.write("fmt ", 12); b.writeUInt32LE(16, 16); b.writeUInt16LE(3, 20);
b.writeUInt16LE(1, 22); b.writeUInt32LE(48000, 24); b.writeUInt32LE(48000 * 4, 28);
b.writeUInt16LE(4, 32); b.writeUInt16LE(32, 34);
b.write("data", 36); b.writeUInt32LE(bytes, 40);
for (let i = 0; i < n; i++) b.writeFloatLE(data[i], 44 + i * 4);
writeFileSync(resolve(here, "../build/chrome-noverb.wav"), b);

let peak = 0;
for (const v of data) peak = Math.max(peak, Math.abs(v));
console.log(`wrote build/chrome-noverb.wav  peak ${peak.toFixed(6)}`);
