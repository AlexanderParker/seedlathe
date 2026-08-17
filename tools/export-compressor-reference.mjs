// Renders a stimulus through Chrome's real DynamicsCompressorNode at zyn's
// settings and writes both the input and the output, so the C++ port can be
// compared sample-for-sample against the actual browser implementation.
//
// Usage: node export-compressor-reference.mjs

import { writeFileSync, mkdirSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import puppeteer from "puppeteer-core";

const here = dirname(fileURLToPath(import.meta.url));
const CHROME = process.env.CHROME_PATH ??
  "C:/Program Files/Google/Chrome/Application/chrome.exe";
const OUTDIR = resolve(here, "../vectors/compressor");

const SR = 48000;
const SECONDS = 4;

const browser = await puppeteer.launch({
  executablePath: CHROME,
  headless: "new",
  args: ["--autoplay-policy=no-user-gesture-required"],
});
const page = await browser.newPage();
await page.setContent("<html><body></body></html>");

const { input, output } = await page.evaluate(async (SR, SECONDS) => {
  const n = SR * SECONDS;

  // Stimulus covering every branch of the kernel: quiet (below threshold, so
  // makeup gain only), loud (deep compression), silence (release), and a hard
  // step (attack transient).
  const sig = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    const t = i / SR;
    const tone = Math.sin(2 * Math.PI * 440 * t);
    if (t < 1.0) sig[i] = 0.05 * tone;
    else if (t < 2.0) sig[i] = 1.0 * tone;
    else if (t < 2.5) sig[i] = 0.0;
    else sig[i] = 0.9 * tone;
  }

  const offline = new OfflineAudioContext(2, n, SR);
  const buf = offline.createBuffer(2, n, SR);
  buf.copyToChannel(sig, 0);
  buf.copyToChannel(sig, 1);

  const src = offline.createBufferSource();
  src.buffer = buf;

  const comp = offline.createDynamicsCompressor();
  comp.threshold.value = -12;
  comp.knee.value = 6;
  comp.ratio.value = 8;
  comp.attack.value = 0.003;
  comp.release.value = 0.15;

  src.connect(comp);
  comp.connect(offline.destination);
  src.start(0);

  const rendered = await offline.startRendering();
  return {
    input: Array.from(sig),
    output: Array.from(rendered.getChannelData(0)),
  };
}, SR, SECONDS);

await browser.close();

mkdirSync(OUTDIR, { recursive: true });
writeFileSync(resolve(OUTDIR, "input.f32"), Buffer.from(Float32Array.from(input).buffer));
writeFileSync(resolve(OUTDIR, "output.f32"), Buffer.from(Float32Array.from(output).buffer));

let peakIn = 0, peakOut = 0;
for (const v of input) peakIn = Math.max(peakIn, Math.abs(v));
for (const v of output) peakOut = Math.max(peakOut, Math.abs(v));
console.log(`wrote ${input.length} frames`);
console.log(`peak in ${peakIn.toFixed(4)}  peak out ${peakOut.toFixed(4)}`);
