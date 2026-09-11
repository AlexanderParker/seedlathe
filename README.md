# Seedlathe

A seed-driven procedural synthesizer plugin (VST3 / CLAP / standalone), built on
[zyn.js](https://github.com/alexanderparker/zyn).

Type or roll a 32-bit integer and get a complete instrument. The same seed sounds the
same here as it does on the zyn demo page — enforced by automated tests, not by ear.

## What it does

- **Seeds.** Type, drag or roll a seed; the whole 32-bit value is one control, even
  though the host sees it as two parameters (see `plugin/SeedlatheParams.h` for why).
- **Similarity search.** Finds seeds close to the current instrument at around 1.6
  million candidates a second, and keeps the twenty best so the near misses can be
  auditioned too — they are often the ones worth keeping.
- **Sample match.** Load a recording and search for the seed that sounds most like it.
  Each candidate is rendered offline and compared on timbre, amplitude shape and
  brightness, which runs at hundreds of candidates a second rather than millions.
  Each band's own noise floor is estimated and subtracted before comparison, so a
  recorded sample works and not only a rendered one — at 20 dB SNR the right seed
  still lands in the top ten of the factory bank. Scores drop as noise rises, which
  is the honest signal: the match really is looser, so a threshold still means
  something.
- **Live modulation.** Cutoff and Resonance ride on top of every oscillator's filter
  envelope and reach notes that are already sounding — the only two parameters that
  do, since everything else about a seed is scheduled at note-on. Both are host
  parameters, so they automate. At their defaults they are exactly zero-sum and the
  render is bit-identical to one without them. zyn.js gained the same control as
  `Z.setFilterMod`.
- **Instrument overview.** The Instrument tab draws every oscillator the seed
  produced at once — envelopes, what is switched on, the FM matrix — read-only, for
  answering "what is this" without clicking through the designer one slot at a time.
- **Undo.** Back and Next step through the sounds visited on the current part: seeds,
  presets, search results, pasted patches and reverts, with the designer edits each
  carried.
- **Designer.** Edit any generated instrument directly: five oscillators, draggable
  envelopes, three LFOs, FM, pitch envelope, distortion, per-oscillator delay and
  reverb, and the 5×5 FM matrix. Patches copy and paste as zyn's own JSON.
- **Presets.** 115 factory presets from the demo page, plus your own bank.
- **Multitimbral.** Sixteen parts, one per MIDI channel, allocated as you use them.
- **Settings.** Engine and MIDI options live on their own tab, so the header holds only
  what you reach for while playing.
- **Oversampling.** 2× or 4×, off by default — it is a deviation from zyn, which runs
  its graph at the host rate.
- **Export.** Render the current instrument to a 32-bit float stereo WAV.

- Design: [`docs/superpowers/specs/2026-08-17-seedlathe-vst-design.md`](docs/superpowers/specs/2026-08-17-seedlathe-vst-design.md)
- Current plan: [`docs/superpowers/plans/2026-08-17-seedlathe-p0-p1-engine.md`](docs/superpowers/plans/2026-08-17-seedlathe-p0-p1-engine.md)
- Performance baseline: [`docs/performance.md`](docs/performance.md)

## Layout

| Path | Responsibility |
|---|---|
| `core/` | Framework-free C++20 engine. Never includes an iPlug2, VST3 or CLAP header — enforced by `tests/test_layering.cpp` |
| `plugin/` | iPlug2 wrapper: parameters, state, MIDI, UI |
| `tools/` | Node scripts that export golden vectors and reference audio from zyn |
| `tests/` | Catch2 unit tests and the fidelity null-test |
| `vectors/` | Committed golden JSON and reference WAVs |

## Building on Windows

Requires Visual Studio 2022 (or newer) with the C++ desktop workload, and Node 22 for
the `tools/` scripts.

```bash
git clone --recurse-submodules <repo> seedlathe
cd seedlathe/third_party/iPlug2/Dependencies/IPlug
bash ./download-clap-sdks.sh
bash ./download-vst3-sdk.sh
```

Those two scripts are **not optional and not captured by the submodule pointer** — the
SDK directories ship as empty placeholders holding only a readme, so a fresh
`--recurse-submodules` clone will configure without CLAP or VST3 targets until you run
them. The prebuilt-libs download (`Dependencies/download-prebuilt-libs.sh`) is *not*
needed: IGraphics uses its default NanoVG backend, which has no external dependencies.

Then, from the repo root:

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

Artefacts land in `build/out/`.

**Note:** iPlug2 copies the built VST3 and CLAP into
`%LOCALAPPDATA%/Programs/Common/VST3` and `.../CLAP` as a post-build step, so the
plugin appears in your DAW's scan after every build.

## Licensing

- zyn core algorithm: MIT
- iPlug2: WDL/zlib-style permissive
- CLAP: MIT
- **VST3 target requires Steinberg's VST3 SDK** (dual GPLv3 / proprietary). The
  proprietary option is free but requires signing Steinberg's agreement. Settle this
  before distributing any VST3 binary; CLAP and standalone are unaffected.

## Running it

Three ways, in increasing order of setup.

### 1. Render a seed to a WAV (no DAW, no MIDI needed)

```
build/out/sl_render.exe 3703184240 forest.wav --seconds 3
```

Options: `--note N` (0 is middle C), `--gain G` (0..1), `--seconds S`,
`--rate R`, `--json` to also print the generated instrument.

This is the quickest way to A/B against
[the demo page](https://alexanderparker.github.io/zyn/) — load the same seed
there and compare.

### 2. Standalone app

Run `build/out/Seedlathe.exe`. **Click the on-screen keyboard** to play —
it sends real MIDI internally, so no controller is required.

The seed box at the top left takes the whole 32-bit value: click to type, drag to
nudge, wheel to step, shift for coarse and ctrl for fine. The host still sees two
16-bit parameters underneath (`seed = SeedHi * 65536 + SeedLo`, so 3703184240 is
`Seed Hi 56506`, `Seed Lo 7024`), because a single float32 automation value cannot
round-trip a 32-bit integer.

If you hear nothing, open the app's **Preferences** dialog and pick the right
audio output device and sample rate.

The standalone keeps its own state — seed, designer edits, parts, settings — in
`%LOCALAPPDATA%/Seedlathe/standalone.state`, written a couple of seconds after
anything changes and read back on launch. Delete that file to start from defaults.

### 3. In a DAW

Every build copies the plugin into the **per-user** plugin folders:

- VST3: `%LOCALAPPDATA%\Programs\Common\VST3\Seedlathe.vst3`
- CLAP: `%LOCALAPPDATA%\Programs\Common\CLAP\Seedlathe.clap`

Some hosts do not scan those. Run this once from an **elevated** prompt to
install alongside every other plugin on the machine instead:

```
tools\install-plugins.cmd
```

Re-run it after each rebuild, or replace the copy with a junction so rebuilds
go live on their own:

```
mklink /J "C:\Program Files\Common Files\VST3\Seedlathe.vst3" ^
          "C:\dev\seedlatheuild\out\Seedlathe.vst3"
```

**If a host still cannot see it**, the usual cause is not the plugin. FL Studio
records a plugin *class* against every search folder, and a folder added by
hand through its Plugin Manager is registered as VST2. It then scans that
folder for VST2 DLLs, sees a directory named `Seedlathe.vst3`, does not
recognise it, and skips it — silently, with "verify plugins" ticked and
nothing in the log. The machine-wide folders are already registered with the
right classes, which is why installing there sidesteps it.

Rescan plugins in your DAW, add Seedlathe to an instrument track, and play.
It responds to note on/off, velocity, all-notes-off, the sustain pedal (CC 64)
the pitch wheel (±2 semitones, the MIDI default) and program change, which
selects a factory preset by number.

With **Multitimbral** on (Instrument page), the MIDI channel selects the part, and a
channel whose part has not been allocated yet is silent — click its number in the part
strip to bring it up. Only part 1's seed is a host parameter; the rest travel in the
state chunk, so they survive save and reload but cannot be automated.

## Fidelity

The acceptance gate is `tests/test_fidelity.cpp`: 60 seeds rendered by this engine and
by real Chrome through an `OfflineAudioContext`, compared as log-mel spectrograms and
RMS envelopes. Both one-shot notes and held notes are covered.

| Set | Seeds | Mean mel distance | Threshold |
|---|---|---|---|
| One-shot | 60 | 2.28 dB | 2.75 dB |
| Sustained | 60 | 2.67 dB | 3.2 dB |

Regenerate the references with `node tools/export-reference-audio.mjs` (add
`--sustained` for the held-note set). Both need Chrome and `puppeteer-core`.
