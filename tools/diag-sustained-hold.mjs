// Does a sustained note actually hold in an OfflineAudioContext?
//
// The sustained reference renders decay steadily instead of holding at the
// sustain level, which is either a bug in the exporter's use of Z.noteOn or
// something in zyn's sustained path. This renders a deliberately trivial
// instrument -- one sine, flat sustain, no filter movement, no effects -- and
// prints its level over time. A flat trace means sustain works and the
// divergence is elsewhere; a decaying one means it does not.
//
//   node tools/diag-sustained-hold.mjs

import { readFileSync } from "node:fs";
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
  args: ["--autoplay-policy=no-user-gesture-required"],
});
const page = await browser.newPage();
await page.setContent("<html><body></body></html>");
await page.addScriptTag({ path: resolve(ZYN, "zyn-unminified.js") });

const WHICH = process.argv[2] ?? "none";
const out = await page.evaluate(async (WHICH) => {
  const SR = 48000, SECONDS = 4;
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

  // Strip one feature at a time from a real seed. Whichever removal makes the
  // trace hold flat is the one whose sustained behaviour differs.
  const base = Z.getInstrument(1596900054);
  const strip = (which) => {
    const inst = JSON.parse(JSON.stringify(base));
    for (const o of inst.oscs) {
      if (which === "fx") o.fx = {};
      if (which === "lfo") { o.gLFO = false; o.fLFO = false; o.pLFO = false; }
      if (which === "fm") o.FM = false;
      if (which === "penv") o.pENV = false;
      if (which === "filter")
        o.adsrFilter = { A: [0.01, 1], D: [0.01, 1], S: [0.01, 1], R: [0.1, 1] };
    }
    return inst;
  };
  const inst = strip(WHICH);

  Z.noteOn(0, inst, 1.0);
  const buf = await offline.startRendering();
  const l = buf.getChannelData(0);

  const win = SR / 4;
  const rows = [];
  for (let s = 0; s + win <= l.length; s += win) {
    let sum = 0;
    for (let i = s; i < s + win; i++) sum += l[i] * l[i];
    rows.push({ t: s / SR, rms: Math.sqrt(sum / win) });
  }
  return rows;
}, WHICH);

await browser.close();
console.log(`stripped: ${WHICH}`);
for (const r of out) console.log(`t=${r.t.toFixed(2)}s  rms ${r.rms.toFixed(6)}`);
