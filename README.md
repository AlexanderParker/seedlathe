# Seedlathe

A synthesizer where the patch is a number. VST3, CLAP, and a standalone app,
built on [zyn.js](https://github.com/AlexanderParker/zyn).

Type `3703184240` and you get a slow pad. `2360196101` is a hard square lead.
`1671058337` is a short blipping kick. The instrument is generated from the
number, so nothing is stored and there are about four billion of them.

One consequence worth knowing up front: a patch is a number you can write
down. The same seed gives the same instrument here and in
[the web version](https://alexanderparker.github.io/zyn/), so sharing a sound
means sharing ten digits.

## Finding a sound

**Random** rolls a new seed. The roll type selector restricts it to a family —
pad, lead, bass, key, pluck, bell, string, drum, perc, FX — so you can look
for a bass without wading through drums.

**Find Similar** takes what you have and searches for seeds near it, about 1.6
million a second. It keeps the twenty closest rather than just the winner,
because the exact match is usually less interesting than the near misses.

**Sample match** takes a WAV and looks for the seed that sounds most like it.
Each candidate is rendered and compared on timbre, attack and brightness, so
it works on a recording and not only on something the synth made.

**Presets**: 115 factory sounds, plus your own. Presets live in packs, each
with categories, and the browser filters by both. A pack is one JSON file, so
sharing a set of sounds is sending one attachment — import it from the
Presets tab, or drop it in the presets folder and hit Rescan. A preset stores
volume, octave, cutoff and resonance alongside the seed, so it loads at the
level it was voiced at.

**Back** and **Next** step through everything you have loaded this session, so
a sound you passed over is one click away rather than gone.

## Editing

The Design tab opens the instrument up: up to five oscillators, each with
envelopes for gain, cutoff and resonance, three LFOs, FM, a pitch envelope,
distortion, delay and reverb. There is also a 5×5 matrix for FM between
oscillators. Changes are audible immediately, and **Revert to seed** undoes
all of them.

You can also go the other way: design what you want, then **Find Similar** to
get the nearest seed to it. The design travels in the project either way, but
a seed is the thing you can write down.

Patches copy and paste as JSON if you want to keep one outside the plugin.

## Playing

Volume, octave, cutoff and resonance sit in the header. All four are host
parameters, so they automate.

The plugin responds to velocity, sustain (CC 64), pitch bend and program
change. **Multitimbral** in Settings gives each MIDI channel its own
instrument, up to sixteen.

**Export** writes the current sound to a stereo WAV.

Generated sounds vary in level, and some are harsh. Leave yourself some
headroom while browsing.

## Building it

No binaries yet. On Windows you need Visual Studio 2022 with the C++ desktop
workload, and Node 22.

```bash
git clone --recurse-submodules https://github.com/AlexanderParker/seedlathe
cd seedlathe/third_party/iPlug2/Dependencies/IPlug
bash ./download-clap-sdks.sh
bash ./download-vst3-sdk.sh
```

Those two scripts are required. The SDK folders ship as empty placeholders, so
without them the build produces no VST3 or CLAP target.

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Output goes to `build/out/`.

Run `build/out/Seedlathe.exe` for the standalone; the on-screen keyboard plays
it, so no MIDI controller is needed. If it is silent, set your audio device in
Preferences.

For a DAW, run `tools\install-plugins.cmd`. It requests administrator access
itself and copies the plugin to the machine-wide plugin folders. Re-run it
after each rebuild and rescan in your DAW.

If your DAW cannot find it, the cause is usually the search folder rather than
the plugin. FL Studio records a plugin class per folder, and a folder added by
hand is registered as VST2, so it looks for VST2 DLLs, sees a directory named
`Seedlathe.vst3`, and skips it without reporting anything. Installing to the
machine-wide folders avoids this.

To render a seed without opening anything:

```
build/out/sl_render.exe 3703184240 forest.wav --seconds 3
```

---

## Under the hood

### Layout

| Path | Responsibility |
|---|---|
| `core/` | Framework-free C++20 engine. Never includes an iPlug2, VST3 or CLAP header — enforced by `tests/test_layering.cpp` |
| `plugin/` | iPlug2 wrapper: parameters, state, MIDI, UI |
| `tools/` | Node scripts that export golden vectors and reference audio from zyn |
| `tests/` | Catch2 unit tests and the fidelity null-test |
| `vectors/` | Committed golden JSON and reference WAVs |

Tests: `ctest --test-dir build -C Release --output-on-failure`.

### Fidelity

The project rests on one claim: a seed sounds the same here as in zyn.js.
`tests/test_fidelity.cpp` checks it. Sixty seeds are rendered by this engine
and by Chrome through an `OfflineAudioContext`, then compared as log-mel
spectrograms and RMS envelopes.

| Set | Seeds | Mean mel distance | Threshold |
|---|---|---|---|
| One-shot | 60 | 2.28 dB | 2.75 dB |
| Sustained | 60 | 2.67 dB | 3.2 dB |

Regenerate the references with `node tools/export-reference-audio.mjs` (add
`--sustained` for the held-note set). Both need Chrome and `puppeteer-core`.

Two deliberate divergences. Oversampling (2× / 4×, off by default) changes the
sound rather than only cleaning it up, because a seed is defined at the host
rate. Cutoff and Resonance are additions zyn never had; at their defaults they
are exactly zero-sum, and a test asserts the unmodulated render is
bit-identical rather than close.

Design notes: [`docs/superpowers/specs/`](docs/superpowers/specs/) ·
performance baseline: [`docs/performance.md`](docs/performance.md)

### How this was built

[zyn.js](https://github.com/AlexanderParker/zyn) came first, and its synth core
was written by hand. This C++ port was not: Seedlathe was written by Claude
(Anthropic's Claude Code), working from that library, under my direction and
review.

The fidelity testing above is what makes that checkable rather than something
to take on trust. The commit history records which changes were AI-written;
nearly all of them were.

### Licensing

- zyn core algorithm: MIT
- iPlug2: WDL/zlib-style permissive
- CLAP: MIT
- The VST3 target requires Steinberg's VST3 SDK (dual GPLv3 / proprietary).
  The proprietary option is free but requires signing Steinberg's agreement.
  Settle this before distributing any VST3 binary; CLAP and standalone are
  unaffected.
