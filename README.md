# Seedlathe

**Every whole number is an instrument.**

`3703184240` is a slow breathing pad. `2360196101` is a hard square lead.
`1671058337` is a short blipping kick. There are four billion more, and none
of them are stored anywhere — the number *is* the patch, and the synth builds
the instrument from it the moment you type it in.

A VST3 / CLAP / standalone synthesizer, built on
[zyn.js](https://github.com/AlexanderParker/zyn).

---

## The idea

Most synths start you at a default patch and ask you to build something. This
one starts you in the middle of four billion finished instruments and asks you
to go looking.

That changes what the work feels like. You are not dialling in an oscillator,
you are **hunting** — rolling, listening, rejecting, and occasionally stopping
dead because the thing that just came out of the speakers is better than
anything you would have thought to build.

And because a sound is just a number, a patch is something you can say out
loud. Text someone `3703184240` and they have your pad — in the plugin, or on
[the web demo](https://alexanderparker.github.io/zyn/), which sounds the same.
No file, no version, no "which preset pack was that in".

## Finding something

**Roll the dice.** Hit Random. If you want a particular flavour, set the roll
type first — Pad, Lead, Bass, Key, Pluck, Bell, String, Drum, Perc, FX — and
every roll stays in that family.

**Find more like this one.** Land on something promising but not quite right?
Find Similar searches about 1.6 million instruments a second and keeps the
twenty closest. The near misses are usually the interesting part: the one you
keep is often three rows down the list, not at the top.

**Hum it, or hand it a record.** Load a WAV on the Sample tab and Seedlathe
goes looking for the seed that sounds most like it. Every candidate is
actually rendered and compared on timbre, attack and brightness, so it works
on a real recording and not only on something the synth made itself.

**Or start from the shelf.** 115 factory presets, plus your own bank.

Nothing you find gets lost on the way: **Back** and **Next** step through every
sound you have visited, so the one you liked two rolls ago is one click away.

## Making it yours

Open the Design tab and the instrument comes apart: up to five oscillators,
draggable envelopes, three LFOs each, FM, pitch envelopes, distortion, and
per-oscillator delay and reverb. Drag anything and you hear it immediately.
The 5×5 FM matrix lets any oscillator modulate any other.

Patches copy and paste as plain JSON, so you can keep one in a text file or
paste it to somebody.

If you go too far, **Revert to seed** puts it back.

## Playing it

**Cutoff** and **Resonance** in the header reach notes that are *already
sounding* — hold a chord and sweep them. Everything else about a seed is fixed
the moment a note starts, so these two are the performance controls. Both are
host parameters, so your DAW can automate them.

It responds to velocity, the sustain pedal, pitch bend and program change.
Turn on **Multitimbral** in Settings and each MIDI channel gets its own
instrument, sixteen of them at once.

**Export** renders whatever you are playing to a stereo WAV.

## Getting it

There is no download yet — you build it. On Windows you need Visual Studio
2022 with the C++ desktop workload, and Node 22.

```bash
git clone --recurse-submodules https://github.com/AlexanderParker/seedlathe
cd seedlathe/third_party/iPlug2/Dependencies/IPlug
bash ./download-clap-sdks.sh
bash ./download-vst3-sdk.sh
```

Those two scripts are **not optional** — the SDK folders ship as empty
placeholders, so without them you get a build with no VST3 or CLAP target.

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Everything lands in `build/out/`.

**To just play with it:** run `build/out/Seedlathe.exe` and click the on-screen
keyboard — it needs no MIDI controller. If you hear nothing, open Preferences
and pick your audio device.

**To use it in a DAW:** run `tools\install-plugins.cmd`. It asks for
administrator access itself and copies the plugin where every other plugin on
the machine lives. Re-run it after each rebuild, then rescan in your DAW.

> **If your DAW cannot see it**, the cause is usually not the plugin. FL Studio
> records a plugin *class* against each search folder, and a folder you add by
> hand is registered as VST2 — so it looks inside for VST2 DLLs, sees a folder
> named `Seedlathe.vst3`, doesn't recognise it, and skips it silently. That is
> what the installer above sidesteps.

You can also render a seed straight to a file without opening anything:

```
build/out/sl_render.exe 3703184240 forest.wav --seconds 3
```

## A word of warning

Sounds are generated, not curated. Most are pleasant, some are dull, and every
so often one is **very** loud or very harsh. Keep the volume somewhere
forgiving while you are rolling.

---

## Under the hood

Everything below is for people working on the code rather than playing it.

### Layout

| Path | Responsibility |
|---|---|
| `core/` | Framework-free C++20 engine. Never includes an iPlug2, VST3 or CLAP header — enforced by `tests/test_layering.cpp` |
| `plugin/` | iPlug2 wrapper: parameters, state, MIDI, UI |
| `tools/` | Node scripts that export golden vectors and reference audio from zyn |
| `tests/` | Catch2 unit tests and the fidelity null-test |
| `vectors/` | Committed golden JSON and reference WAVs |

Run the tests with `ctest --test-dir build -C Release --output-on-failure`.

### Fidelity

The whole project rests on one claim: a seed sounds the same here as it does in
zyn.js. That is a test, not an aspiration. `tests/test_fidelity.cpp` renders 60
seeds through this engine and through real Chrome via an `OfflineAudioContext`,
and compares them as log-mel spectrograms and RMS envelopes.

| Set | Seeds | Mean mel distance | Threshold |
|---|---|---|---|
| One-shot | 60 | 2.28 dB | 2.75 dB |
| Sustained | 60 | 2.67 dB | 3.2 dB |

Regenerate the references with `node tools/export-reference-audio.mjs` (add
`--sustained` for the held-note set). Both need Chrome and `puppeteer-core`.

Two deliberate divergences are worth knowing about. **Oversampling** (2× / 4×,
off by default) changes the sound rather than only cleaning it up, because a
seed is defined at the host rate. And **Cutoff / Resonance** are additions zyn
never had — at their defaults they are exactly zero-sum, and a test asserts the
unmodulated render is bit-identical rather than merely close.

Design notes: [`docs/superpowers/specs/`](docs/superpowers/specs/) ·
performance baseline: [`docs/performance.md`](docs/performance.md)

### How this was built

[zyn.js](https://github.com/AlexanderParker/zyn) came first, and its synth core
was written by hand. This C++ port was not: Seedlathe was written by Claude
(Anthropic's Claude Code), working from that library, under my direction and
review.

That is a claim worth being able to check rather than take on trust, which is
what the fidelity testing above is for. The commit history records which
changes were AI-written; nearly all of them were.

### Licensing

- zyn core algorithm: MIT
- iPlug2: WDL/zlib-style permissive
- CLAP: MIT
- **The VST3 target requires Steinberg's VST3 SDK** (dual GPLv3 / proprietary).
  The proprietary option is free but requires signing Steinberg's agreement.
  Settle this before distributing any VST3 binary; CLAP and standalone are
  unaffected.
