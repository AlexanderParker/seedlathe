// Verifies the zyn distortion fix renders in a real browser: no ReferenceError,
// and a distorted seed measurably differs from the same seed with dist removed.
import { readFileSync } from "node:fs";
import puppeteer from "puppeteer-core";

const ZYN = "C:/dev/zyn";
const CHROME = "C:/Program Files/Google/Chrome/Application/chrome.exe";

const browser = await puppeteer.launch({
  executablePath: CHROME,
  headless: "new",
  args: ["--autoplay-policy=no-user-gesture-required"],
});
const page = await browser.newPage();

const errors = [];
page.on("pageerror", (e) => errors.push(String(e)));
page.on("console", (m) => { if (m.type() === "error") errors.push(m.text()); });

await page.setContent("<html><body></body></html>");
await page.addScriptTag({ path: `${ZYN}/zyn-unminified.js` });

const result = await page.evaluate(async () => {
  const SR = 48000, SECONDS = 2;

  async function render(seed, stripDist) {
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

    // Deterministic noise so the two renders differ only by distortion.
    let a = 0x5eed1a7e >>> 0;
    Z.randSample = () => {
      a = (a + 0x6d2b79f5) >>> 0;
      let t = a;
      t = Math.imul(t ^ (t >>> 15), t | 1);
      t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
      return ((((t ^ (t >>> 14)) >>> 0) / 4294967296) * 2) - 1;
    };

    const inst = Z.getInstrument(seed);
    const distCount = inst.oscs.filter((o) => o.dist).length;
    if (stripDist) inst.oscs.forEach((o) => { delete o.dist; });

    Z.play(0, inst, 1.0);
    const buf = await offline.startRendering();
    const ch = buf.getChannelData(0);
    let sum = 0, peak = 0;
    for (let i = 0; i < ch.length; i++) { sum += ch[i] * ch[i]; peak = Math.max(peak, Math.abs(ch[i])); }
    return { rms: Math.sqrt(sum / ch.length), peak, distCount, data: Array.from(ch.slice(0, 48000)) };
  }

  const withDist = await render(3, false);
  const without = await render(3, true);

  let diff = 0;
  for (let i = 0; i < withDist.data.length; i++)
    diff += Math.abs(withDist.data[i] - without.data[i]);
  diff /= withDist.data.length;

  return {
    distOscs: withDist.distCount,
    rmsWith: withDist.rms,
    rmsWithout: without.rms,
    peakWith: withDist.peak,
    meanAbsDiff: diff,
  };
});

await browser.close();

console.log("distorted oscillators in seed 3:", result.distOscs);
console.log("rms with dist   :", result.rmsWith.toFixed(6));
console.log("rms without dist:", result.rmsWithout.toFixed(6));
console.log("peak with dist  :", result.peakWith.toFixed(6));
console.log("mean abs diff   :", result.meanAbsDiff.toExponential(3));
console.log("page errors     :", errors.length ? errors : "none");

const ok = errors.length === 0 && result.meanAbsDiff > 1e-6 && result.peakWith > 1e-4;
console.log(ok ? "PASS: distortion renders and changes the signal" : "FAIL");
process.exit(ok ? 0 : 1);
