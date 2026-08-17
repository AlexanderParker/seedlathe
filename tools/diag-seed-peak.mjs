// Diagnostic: what peak does Chrome actually produce for a given seed, before
// and after the master compressor? Answers "is my engine diverging, or is the
// instrument genuinely this loud".
import { resolve } from "node:path";
import puppeteer from "puppeteer-core";

const ZYN = process.env.ZYN_DIR ?? "C:/dev/zyn";
const CHROME = process.env.CHROME_PATH ??
  "C:/Program Files/Google/Chrome/Application/chrome.exe";
const seeds = process.argv.slice(2).map(Number);
if (!seeds.length) seeds.push(13, 60);

const browser = await puppeteer.launch({
  executablePath: CHROME,
  headless: "new",
  args: ["--autoplay-policy=no-user-gesture-required"],
});
const page = await browser.newPage();
await page.setContent("<html><body></body></html>");
await page.addScriptTag({ path: resolve(ZYN, "zyn-unminified.js") });

for (const seed of seeds) {
  for (const gain of [0.25, 1.0]) {
    for (const withComp of [true, false]) {
      const peak = await page.evaluate(async (seed, gain, withComp) => {
        const SR = 48000, SECONDS = 4;
        const offline = new OfflineAudioContext(2, SR * SECONDS, SR);
        Z.ctx = offline;
        Z.masterGain = offline.createGain();
        if (withComp) {
          Z.compressor = offline.createDynamicsCompressor();
          Z.compressor.threshold.value = -12;
          Z.compressor.knee.value = 6;
          Z.compressor.ratio.value = 8;
          Z.compressor.attack.value = 0.003;
          Z.compressor.release.value = 0.15;
          Z.masterGain.connect(Z.compressor);
          Z.compressor.connect(offline.destination);
        } else {
          Z.masterGain.connect(offline.destination);
        }
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

        Z.play(0, Z.getInstrument(seed), gain);
        const buf = await offline.startRendering();
        const ch = buf.getChannelData(0);
        let p = 0;
        for (let i = 0; i < ch.length; i++) p = Math.max(p, Math.abs(ch[i]));
        return p;
      }, seed, gain, withComp);

      console.log(
        `seed ${String(seed).padEnd(11)} gain ${gain.toFixed(2)}  ` +
        `${withComp ? "post-comp" : "pre-comp "}  peak ${peak.toFixed(4)}`);
    }
  }
}

await browser.close();
