// Does zyn.js's live filter modulation actually reach a sounding note?
//
// Renders one sustained note twice in an OfflineAudioContext: once with the
// modulation buses left at zero, once with the cutoff bus scheduled to drop
// five octaves half way through. The second render must lose its high end at
// that point and the first must not, and the two must be IDENTICAL before it
// -- otherwise merely wiring the buses in has changed the sound, which would
// break fidelity against the C++ port for every existing seed.
//
//   node tools/diag-filter-mod.mjs

import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import puppeteer from "puppeteer-core";

const here = dirname(fileURLToPath(import.meta.url));
const ZYN = process.env.ZYN_DIR ?? "C:/dev/zyn";
const CHROME = process.env.CHROME_PATH ??
  "C:/Program Files/Google/Chrome/Application/chrome.exe";

const browser = await puppeteer.launch({
  executablePath: CHROME,
  headless: "new",
  args: ["--autoplay-policy=no-user-gesture-required", "--no-sandbox"],
});
const page = await browser.newPage();
await page.setContent("<html><body></body></html>");
await page.addScriptTag({ path: resolve(ZYN, "zyn-unminified.js") });

const out = await page.evaluate(async () => {
  const SR = 48000, SECONDS = 4, SWEEP_AT = 2.0;

  async function render(modulate) {
    const offline = new OfflineAudioContext(2, SR * SECONDS, SR);
    Z.ctx = offline;
    Z.cutoffMod = null;
    Z.resMod = null;
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
    Z.activeVoices = {};
    Z.ensureFilterMod();

    // An offline render cannot be knob-twisted, so the bus is scheduled
    // instead. Same path, same node, same summing into detune.
    if (modulate) Z.cutoffMod.offset.setValueAtTime(-6000, SWEEP_AT);

    // A hand-built instrument, not a seed. Generated reverb impulses are
    // filled with Math.random(), so any seed carrying one renders differently
    // every time and two renders cannot be compared at all.
    const inst = {
      oscs: [{
        waveform: "sawtooth",
        oct: 0,
        detune: 0,
        adsrGain:    { A: [0.005, 1], D: [0, 1], S: [0, 1], R: [0.05, 0] },
        adsrFilter:  { A: [0.001, 1], D: [0, 1], S: [0, 1], R: [0.05, 1] },
        adsrFilterQ: { A: [0.001, 0], D: [0, 0], S: [0, 0], R: [0.05, 0] },
        filterQ: 0,
      }],
    };
    Z.render(0, [0], { rootNote: 0, gain: 1, pan: 0, instrument: inst }, true);

    const buf = await offline.startRendering();
    return Array.from(buf.getChannelData(0));
  }

  const plain = await render(false);
  const swept = await render(true);

  // Brightness: the RMS of the first difference over the RMS of the signal.
  // A ratio rather than a level, because the master compressor pulls the
  // level of the darker render back up and would hide most of the change.
  const brightness = (x, fromSec, toSec) => {
    let d2 = 0, x2 = 0;
    const a = Math.floor(fromSec * SR), b = Math.floor(toSec * SR);
    for (let i = a + 1; i < b; ++i) {
      const d = x[i] - x[i - 1];
      d2 += d * d;
      x2 += x[i] * x[i];
    }
    return Math.sqrt(d2 / Math.max(x2, 1e-12));
  };
  const hf = brightness;
  const dB = (a, b) => 20 * Math.log10(Math.max(a, 1e-12) / Math.max(b, 1e-12));

  // Identical before the sweep?
  let maxDiff = 0;
  for (let i = 0; i < Math.floor(SWEEP_AT * SR); ++i)
    maxDiff = Math.max(maxDiff, Math.abs(plain[i] - swept[i]));

  return {
    maxDiffBeforeSweep: maxDiff,
    plainAfter: hf(plain, SWEEP_AT + 0.2, SECONDS),
    sweptAfter: hf(swept, SWEEP_AT + 0.2, SECONDS),
    changeDb: dB(hf(swept, SWEEP_AT + 0.2, SECONDS), hf(plain, SWEEP_AT + 0.2, SECONDS)),
  };
});

await browser.close();

console.log(`identical before the sweep: max sample difference ${out.maxDiffBeforeSweep.toExponential(3)}`);
console.log(`brightness after the sweep: plain ${out.plainAfter.toExponential(3)}, swept ${out.sweptAfter.toExponential(3)}`);
console.log(`change: ${out.changeDb.toFixed(2)} dB`);

const clean = out.maxDiffBeforeSweep === 0;
const heard = out.changeDb < -10;
console.log(clean ? "PASS: unmodulated audio is untouched" : "FAIL: the buses changed the sound at rest");
console.log(heard ? "PASS: the sweep reached the sounding note" : "FAIL: the sweep did nothing");
process.exit(clean && heard ? 0 : 1);
