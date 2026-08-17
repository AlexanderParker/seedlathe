// Renders a hand-built minimal instrument in Chrome: one sine oscillator, a
// passthrough-ish allpass filter, a flat gain envelope, no effects. If the C++
// engine disagrees on THIS, the problem is in the core chain rather than in any
// one component.
import { resolve } from "node:path";
import puppeteer from "puppeteer-core";

const ZYN = process.env.ZYN_DIR ?? "C:/dev/zyn";
const CHROME = process.env.CHROME_PATH ??
  "C:/Program Files/Google/Chrome/Application/chrome.exe";

const INSTRUMENT = {
  type: "pad",
  fmMatrix: null,
  oscs: [{
    waveform: "sine",
    adsrGain:    { A: [0.01, 1], D: [0.1, 1], S: [0.5, 1], R: [0.1, 0] },
    filterType:  "allpass",
    adsrFilter:  { A: [0.001, 1], D: [0.1, 1], S: [0.5, 1], R: [0.1, 1] },
    filterQ: 1,
    adsrFilterQ: { A: [0.001, 1], D: [0.1, 1], S: [0.5, 1], R: [0.1, 1] },
    gLFO: false, fLFO: false, pLFO: false, FM: false, pENV: false,
    oct: 0, detune: 0,
    fx: { del: null, verb: null },
  }],
};

const browser = await puppeteer.launch({
  executablePath: CHROME, headless: "new",
  args: ["--autoplay-policy=no-user-gesture-required"],
});
const page = await browser.newPage();
await page.setContent("<html><body></body></html>");
await page.addScriptTag({ path: resolve(ZYN, "zyn-unminified.js") });

const TYPES = ["lowpass", "highpass", "bandpass", "lowshelf", "highshelf", "peaking", "allpass"];
for (const ft of TYPES) {
const inst2 = JSON.parse(JSON.stringify(INSTRUMENT));
inst2.oscs[0].filterType = ft;
const out = await page.evaluate(async (inst) => {
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

  Z.play(0, inst, 1.0);
  const buf = await offline.startRendering();
  const ch = buf.getChannelData(0);

  let peak = 0, sum = 0;
  for (let i = 0; i < ch.length; i++) { peak = Math.max(peak, Math.abs(ch[i])); sum += ch[i] * ch[i]; }

  // RMS of the first 100 ms, where the envelope is at its sustain plateau.
  let e = 0;
  for (let i = 4800; i < 9600; i++) e += ch[i] * ch[i];

  return { peak, rms: Math.sqrt(sum / ch.length), plateauRms: Math.sqrt(e / 4800) };
}, inst2);
console.log(`chrome ${ft.padEnd(10)} peak ${out.peak.toFixed(6)}  rms ${out.rms.toFixed(6)}  plateau ${out.plateauRms.toFixed(6)}`);
}

await browser.close();
