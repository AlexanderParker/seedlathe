// Does Chrome's BiquadFilterNode actually follow automation on `frequency`?
// A 261 Hz sine through a highpass at 20 kHz must be almost silent. If the
// automated case is loud and the static case is quiet, the automation is not
// being applied the way a naive reading of the spec suggests.
import puppeteer from "puppeteer-core";

const CHROME = process.env.CHROME_PATH ??
  "C:/Program Files/Google/Chrome/Application/chrome.exe";

const browser = await puppeteer.launch({
  executablePath: CHROME, headless: "new",
  args: ["--autoplay-policy=no-user-gesture-required"],
});
const page = await browser.newPage();
await page.setContent("<html><body></body></html>");

const res = await page.evaluate(async () => {
  const SR = 48000, N = SR;

  async function run(mode) {
    const ctx = new OfflineAudioContext(1, N, SR);
    const osc = ctx.createOscillator();
    osc.type = "sine";
    osc.frequency.value = 261.63;

    const f = ctx.createBiquadFilter();
    f.type = "highpass";

    if (mode === "static") {
      f.frequency.value = 20000;
      f.Q.value = 30;
    } else if (mode === "automated") {
      // Exactly what Z.adsr does.
      f.frequency.setValueAtTime(0, 0);
      f.frequency.linearRampToValueAtTime(20000, 0.005);
      f.frequency.linearRampToValueAtTime(20000, 0.105);
      f.Q.setValueAtTime(0, 0);
      f.Q.linearRampToValueAtTime(30, 0.005);
      f.Q.linearRampToValueAtTime(30, 0.105);
    } else if (mode === "static-low") {
      f.frequency.value = 200;
      f.Q.value = 30;
    }

    osc.connect(f);
    f.connect(ctx.destination);
    osc.start(0);
    const buf = await ctx.startRendering();
    const ch = buf.getChannelData(0);
    let peak = 0;
    for (let i = SR / 2; i < N; i++) peak = Math.max(peak, Math.abs(ch[i]));
    return peak;
  }

  return {
    staticHigh: await run("static"),
    automated: await run("automated"),
    staticLow: await run("static-low"),
  };
});

await browser.close();
console.log("highpass on a 261 Hz sine, peak over the second half:");
console.log(`  static  cutoff 20000 Hz : ${res.staticHigh.toFixed(6)}`);
console.log(`  AUTOMATED ramp to 20000 : ${res.automated.toFixed(6)}`);
console.log(`  static  cutoff   200 Hz : ${res.staticLow.toFixed(6)}`);
