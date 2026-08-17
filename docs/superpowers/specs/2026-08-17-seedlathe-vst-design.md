# Seedlathe — Design

- **Date:** 2026-08-17
- **Status:** Approved product design. Requires decomposition before implementation (see §16).
- **Upstream reference:** `C:/dev/zyn` (zyn.js, MIT) — `zyn-unminified.js`, `demo.js`, `index.html`

## 1. Summary

Seedlathe is a professional instrument plugin (VST3 / CLAP / AU / standalone) built on
zyn.js, a procedural synthesizer that generates a complete instrument from a single
32-bit integer seed.

Three things define the product, and all three are first-class:

1. **The seed is the instrument.** Type or roll an integer, get a playable instrument.
   The same seed must sound the same in the plugin as it does on the zyn demo page.
2. **Similarity search.** Find seeds near a target — near another seed, near a
   hand-built design, or near an uploaded audio file.
3. **The designer.** Hand-edit oscillators, envelopes, LFOs, effects and the FM matrix,
   then search for the seed that comes closest to what you built.

## 2. Goals

- A seed produces audibly the same instrument in Seedlathe as in zyn.js, verified by an
  automated null-test rather than by ear.
- Search is fast enough to be interactive and never blocks audio or UI.
- The designer exposes every field of the instrument tree.
- Host integration is correct: stable parameter list, exact state recall, no audio-thread
  allocation, passes pluginval / clap-validator / auval.
- Permissive licensing throughout; no JUCE dependency.

## 3. Non-goals

- Improving on zyn's synthesis architecture. Where zyn is idiosyncratic, Seedlathe
  reproduces the idiosyncrasy (§6.3). The one exception is distortion (§3.1).
- Sample playback, wavetable import, or user-supplied impulse responses.
- A mobile or web build.

### 3.1 The distortion exception

Distortion is present in every instrument tree and has never rendered a sample.
`getInstrument` writes `osc.dist` with 5% probability, but `render` only tests
`layer.dist`, which nothing assigns. If it ever did fire it would throw: `getDistCurve`
reads a bare global `sampleRate` that does not exist in a `Window` context, the branch
references undefined identifiers `nDist` and `nGains`, and `dist.curve` is stored as a
function where `render` expects an array.

Decision: implement per-oscillator distortion properly in Seedlathe **and** upstream the
equivalent fix to zyn.js, so the two implementations continue to agree. Reference audio
is regenerated against fixed zyn. Roughly 5% of generated oscillators will change
character relative to the currently deployed demo page; that is accepted in exchange for
a single behaviour and a designer control that works.

## 4. Name

**Seedlathe** — *seed* for the generator, *lathe* for the designer that shapes raw stock
into form.

Availability verified 2026-08-17 via registry RDAP with a passing control test
(nonsense string 404, `google.com` 200):

| Namespace | State |
|---|---|
| `.com .net .io .app .dev .co .audio` | All available |
| X, YouTube, SoundCloud, GitHub | All available |
| Bandcamp | 403 bot-block — unverified, irrelevant for a plugin |
| Existing commercial use | None found. Web search returned only generic woodturning content |
| Trademark register | **Not searched.** USPTO search endpoint returned 404 |

This is a knockout search, not legal clearance. An attorney search in Nice class 9 and 42
is required before brand spend.

Rejected working name **Zynth**: collides with ZynAddSubFX / Zyn-Fusion in the same
audience, and sits one letter from "synth", which is a permanent search-substitution tax
on a name said aloud. `zynth.com` is registered regardless.

Identifiers: repo `C:/dev/seedlathe`, bundle `com.seedlathe.seedlathe`, preset extension
`.slathe`, core namespace `sl::`.

## 5. Architecture

```
seedlathe/
  core/      # C++20 static lib. No iPlug2, no VST, no UI. Headless-testable.
  plugin/    # iPlug2: parameters, state, MIDI, IGraphics UI
  tools/     # node scripts: golden-vector and reference-audio export from zyn
  tests/     # Catch2 unit + fidelity null-tests
  vectors/   # committed golden JSON + reference WAVs
```

**Layering rule:** `core/` never includes a plugin-framework header. Search and fidelity
tests run headless in CI with no host, and if iPlug2 disappoints, only `plugin/` is lost.

### 5.1 `core/` units

| Unit | Responsibility |
|---|---|
| `Mulberry32` | Exact PRNG port. `Math.imul` is a 32-bit truncating signed multiply |
| `InstrumentGen` | Port of `Z.getInstrument`. Identical `r()` call order, or seeds diverge |
| `Instrument` | Fixed-size POD tree (≤5 oscs, 5×5 `fmMatrix`, 5×5 `fmDelays`). No heap |
| `webaudio/` | `WaOscillator` (band-limited wavetable using Web Audio's harmonic-truncation rule), `WaBiquad` (RBJ, a-rate coefficient recompute, Blink's clamping), `WaParam` (setValueAtTime / linearRampToValueAtTime timeline), `WaDelay` (linear fractional interpolation), `WaConvolver`, `WaCompressor`, `WaPanner` (equal-power) |
| `SharedFxRack` | Reproduces `Z.fxNodes`: delay and reverb nodes shared across all voices by config hash, with `maxFxNodes = 50` eviction |
| `Voice` / `VoicePool` | Fixed pre-allocated pool, quietest-then-oldest stealing with 5 ms fade |
| `Oversampler` | Off / 2× / 4×, FIR polyphase halfband |
| `SeedSearch` | Port of `compareInstruments` scoring plus a cancellable threaded sweep |
| `AudioFeatures` | YIN pitch, amplitude envelope, 40-band mel → 13 MFCC, centroid / rolloff / flatness / flux, harmonic-to-noise ratio |
| `OfflineRender` | Instrument → buffer. Serves sample-match search and WAV export |
| `PresetIO` | nlohmann/json. Reads zyn's preset and design JSON unchanged |

## 6. Fidelity contract

### 6.1 Parameter exactness

`tools/export-vectors.mjs` runs `zyn-unminified.js` over ~2000 seeds — every type digit,
every FM-probability first digit — and writes `vectors/instruments.json`. A test asserts
that C++ `InstrumentGen` reproduces every field as exactly equal doubles. Any drift in
PRNG behaviour or `r()` call order fails the build.

### 6.2 Audio fidelity

`tools/export-reference-audio.mjs` drives headless Chrome, renders the 115 factory
presets plus 200 random seeds through `OfflineAudioContext`, and writes WAVs to
`vectors/audio/`. The null-test compares C++ offline renders against them using
mel-spectrogram L1 distance plus per-frame RMS envelope error, with thresholds
calibrated once and then locked.

This is what makes "sonically faithful" a number instead of an opinion. The test runs
with oversampling **off**; 2× and 4× get a looser variant that compares only below
Nyquist/2, because oversampling changes the sound deliberately.

### 6.3 Quirks that must survive the port

These are not defects to repair. They are the sound of every seed that already exists.

| Quirk | Consequence |
|---|---|
| `Z.fxNodes` caches delay and reverb globally by config hash, shared across every voice and note | Delay feedback accumulates across notes; reverb tail is common. Per-voice effects would sound entirely different |
| `cleanupFxNodes` disconnects the oldest half once the cache exceeds 50 nodes | Audibly truncates delay tails under load |
| `pENV` calls `setValueAtTime(0, t)` then ramps to `oFreq * amount` | The pitch envelope *replaces* pitch rather than offsetting it — the oscillator starts at 0 Hz |
| `detune` is semitones: `r() < 0.2 ? 5 : 0`, summed in `Z.freq(root, note + oct*12 + detune)` | A "detuned" oscillator is a perfect fourth up, not a few cents |
| `filterQ` is assigned then immediately overwritten by `adsrFilterQ` scaled to 30 | Dead in the audio path, but `compareInstruments` still reads it — both behaviours must be kept or search scores change |
| `voiceGain = 1 / (notes × oscs)`, then `layer.gain = 0.5 × gain` | Output level depends on it exactly |
| Filter cutoff envelope spans 0 → 20000 Hz | RBJ coefficients degenerate near 0 Hz; port Blink's exact clamp, not a "sensible" one |
| FM matrix gain is `amt × targetFreq × 0.2`, routed through a delay of `fmDelays[s][t] ?? 0.001` s, skipped when either oscillator is noise | The delay is what stops feedback routes exploding; its phase shift is part of the sound |
| Master chain is `masterGain → DynamicsCompressor(threshold −12, knee 6, ratio 8, attack 0.003, release 0.15) → destination` | Ports as Blink's compressor algorithm, including 6 ms lookahead and knee curve |

**One deliberate divergence:** zyn's noise buffer is 2 s of `Math.random()`, so the same
seed differs between runs. Seedlathe derives it from the instrument seed. This makes
seeds fully reproducible and is a precondition for the null-test existing at all.

## 7. The seed cannot be an ordinary parameter

Seeds span 0 … 4,294,967,295. Host automation is float32 in practice — a 24-bit mantissa,
about 16.7 M distinct values. Four billion seeds do not round-trip. The failure is silent:
save seed 3703184240, reload, get a neighbour, and the instrument is unrecognisable. It
presents to the user as "presets are randomly broken".

**Two 16-bit stepped parameters**, `Seed Hi` and `Seed Lo`, each 0 … 65535, with
`seed = hi × 65536 + lo`. Both are exact in float32 because 65536 < 2²⁴.

These parameters are the authoritative seed. The seed is deliberately *not* duplicated in
the state chunk, so there is one source of truth and no reload ordering race.

Cost: a seed sweep needs two automation lanes and moves non-monotonically. Acceptable —
seeds are not ridden like a filter cutoff, and a `Seed Hi`-only sweep is a usable coarse
morph.

## 8. Engine internals

**Generation runs on the audio thread.** `Instrument` is a fixed-size POD of roughly 3 KB,
and generation is a few hundred PRNG calls of pure arithmetic — bounded time, zero
allocation. A seed change is therefore an in-place regeneration with no worker round-trip.

**Effects cannot.** Convolution impulses (up to 3.1 s stereo) and delay lines must never
be allocated on the audio thread. `SharedFxRack` pre-allocates 16 impulse buffers at
maximum length and pre-allocates delay lines at the 0.5 s maximum the generator can
produce. A worker fills impulse contents and flips a ready flag; until then that voice's
reverb is bypassed, which lasts a few milliseconds and is inaudible.

**Threading:**

- *Audio thread* — fixed POD instrument in a three-slot ring published by
  `std::atomic<int>`. Pre-allocated voice pool. No locks, no allocation, no logging.
- *UI thread* — designer edits build a new POD and publish atomically. A voice copies the
  instrument at note-on, so editing mid-chord never mutates ringing notes.
- *Worker pool* (`hardware_concurrency - 1`) — search, offline render, WAV export, sample
  analysis, impulse generation. Results returned over an SPSC queue. Search must be
  cancellable *within* a single candidate, or "Search Until" cannot stop responsively.

**Voices:** pool of 32 by default, 1–64 by parameter, quietest-then-oldest stealing with a
5 ms fade. zyn creates nodes without bound, which is tolerable in a browser tab and not in
a host.

**Oversampling:** global Off / 2× / 4×, FIR polyphase halfband wrapped around the
oscillator + filter + FM stage only; delay and reverb gain nothing from it. Latency is
reported to the host. FIR is chosen over IIR because IIR phase smearing would surface in
the null-tests as false failures. Offline export carries an independent quality setting.

## 9. Parameters and state

**Global (28):** Seed Hi, Seed Lo, Type Filter, Volume, Octave, Transpose, Fine, Glide,
Voices, Bend Range, Oversampling, Cutoff Offset, Reso Offset, Attack Scale, Release Scale,
FM Depth Scale, Delay Mix, Reverb Mix, Drive, Macro 1–8.

**Multitimbral (64):** 16 parts × Volume, Pan, Octave, Mute.

Macro *values* are host parameters. Macro *mappings* — target path into the design tree,
depth, curve, up to 8 slots each — live in the state chunk, along with the design tree,
part configuration, user presets and UI state.

## 10. UI

iPlug2 IGraphics with the Skia backend. Resizable, DPI-aware, minimum 1200×760. Persistent
top bar and keyboard; tabbed centre.

*Top bar:* seed field and copy, roll button with type filter, preset name with prev/next
and browser, volume, octave, voices, oversampling, CPU meter, panic.
*Bottom:* piano keyboard with velocity from Y position, scope/spectrum toggle.

| Tab | Contents |
|---|---|
| Instrument | Seed, type radios, Generate Random, Find Similar / Retry, threshold + Search Until. Read-only visualiser of the generated tree. JSON view and copy |
| Design | Five oscillator panels — waveform, octave, detune, filter type, Q, three draggable ADSR editors, gain/filter/pitch LFOs, FM, pENV, distortion, delay, reverb, enable toggle. Global volume and octave. 5×5 FM matrix with per-cell delay. Load from Generator, Find Seed, Find Improved |
| Search | Unified: match-a-seed, match-my-design, match-an-audio-file. Shared result list (score, seed, type, audition, make current, send to designer), progress, candidates/sec, cancel that preserves best-so-far |
| Presets | 115 factory presets grouped by type, user bank, filter, save / rename / delete, JSON export and import, drag-drop `.slathe`. Host program list mirrors the active bank |
| Parts | 16 rows: seed or design, octave, volume, pan, mute/solo, output bus, activity LED. Channel-configuration presets |
| Settings | Oversampling, voices, MIDI pedal handling (sustain, sostenuto, soft), scope options, GUI scale, export defaults |

`compareInstruments` weights are ported verbatim, so a score in Seedlathe means the same
thing as a score on the demo page.

## 11. Search

### 11.1 Parameter-space search

Direct port of zyn's scorer: 0–100 across oscillator count, waveforms, envelopes, filter,
LFOs, FM, effects and `fmMatrix`. Candidate generation is pure arithmetic, so this runs at
millions of candidates per minute. Modes: similar-to-current-seed, and find-seed-for-design.
"Search Until" runs to a threshold or cancellation, keeping best-so-far on cancel.

### 11.2 Sample-match search

1. Analyse the uploaded file — normalise, trim silence, YIN pitch detection to fix the
   render note, amplitude envelope, 40-band mel → 13 MFCCs over time, spectral centroid /
   rolloff / flatness / flux, harmonic-to-noise ratio.
2. Time-warp to 32 frames plus scalar summaries, giving a fixed-length vector. Four user
   weight sliders: Timbre, Envelope, Brightness, Noisiness.
3. Per worker: `getInstrument(seed)` → offline render at the detected pitch → identical
   extraction → weighted distance.
4. Live best-so-far, auditionable while the search runs.

**Open measurement.** Throughput is unknown and it is the number that decides this
feature's UX. Each candidate is roughly 96 k samples across up to five oscillators;
estimate 500–2000 candidates/sec across eight cores, three to four orders of magnitude
slower than parameter search. Build the single-stage version, but structure the scorer so
a cheap envelope/centroid prefilter can be inserted without redesign, and measure before
committing to the screen design. A search taking twenty minutes needs a different
interface from one taking twenty seconds.

## 12. WAV export

Falls out of the offline renderer: single note or chromatic sweep, length and tail, sample
rate, bit depth, independent oversampling setting, per-note file naming. This doubles as a
sampler-export feature.

## 13. Multitimbral

Sixteen parts, each holding a seed or a design with its own octave, volume, pan, mute/solo
and output bus. One main stereo bus plus eight aux stereo buses. Part engines are
instantiated lazily so unused parts cost nothing. MIDI channel maps to part; CLAP note
ports behave identically.

## 14. Testing

| Test | Gate |
|---|---|
| Golden vectors | 2000 seeds, every tree field, exact double equality against zyn.js |
| Fidelity null-test | 115 presets + 200 seeds, C++ vs Chrome `OfflineAudioContext`, mel-spectrogram L1 and per-frame RMS below locked thresholds |
| Unit | Mulberry32 against known JS values, `WaParam` timeline, biquad magnitude against analytic RBJ, compressor step response, feature extraction on synthetic signals of known pitch and centroid |
| State | Random designs round-trip byte-identical; seed recall exact across the 16-bit split |
| Host | pluginval at strictness 10, clap-validator, auval |
| Sanitiser | ThreadSanitizer build covering search ↔ audio interaction |

## 15. Licensing

- zyn core: MIT, unchanged.
- iPlug2: WDL/zlib-style permissive, free for commercial use.
- CLAP: MIT.
- **VST3 target requires Steinberg's VST3 SDK**, dual GPLv3 / proprietary. The proprietary
  option is free but requires signing Steinberg's agreement. This applies regardless of
  plugin framework and must be settled before any VST3 binary is distributed.
- Third-party: nlohmann/json (MIT), Catch2 (BSL-1.0), Skia (BSD-3).

## 16. Decomposition

This design is too large for one implementation plan. Seven phases, each taking its own
spec → plan → build cycle:

| Phase | Content |
|---|---|
| **P0** Foundations | Repo, CMake, iPlug2 vendored, distortion fix upstreamed to zyn.js, vector and reference-audio exporters, `Mulberry32` + `InstrumentGen` exact, empty plugin builds VST3/CLAP/standalone |
| **P1** Engine | Web Audio compatibility layer, voice pool, shared FX rack, compressor, null-tests passing, playable over MIDI |
| **P2** Plugin shell | Parameters, state chunk, IGraphics frame, top bar, piano, scope, presets and factory bank |
| **P3** Designer | Full editor and FM matrix — the largest UI chunk |
| **P4** Parameter search | Find Similar, Search Until, Find Seed |
| **P5** Sample match | Audio import, feature extraction, render-and-score search, WAV export |
| **P6** Multitimbral | Sixteen parts, aux buses, channel presets |
| **P7** Release | Oversampling polish, installer, signing, validators, documentation, preset packs |

**P0 and P1 are specified together as the first sub-project.** P0 alone produces nothing
playable, and P1 cannot be verified without P0's golden vectors. Together they answer the
only question capable of killing the project — whether C++ can reproduce zyn's sound
closely enough that a seed means the same thing in both places — before any UI is built.

## 17. Risks

| Risk | Mitigation |
|---|---|
| Web Audio's `DynamicsCompressor` is under-specified; Blink's implementation is the real reference | Port from Blink source, validate with step-response tests. If it cannot be matched, it is the one component permitted a looser threshold, since it sits on the master bus and affects all seeds equally |
| Band-limited oscillator tables may not match Chrome's harmonic truncation exactly | Compare single-oscillator renders in isolation before assembling full voices; this isolates the error source |
| Sample-match throughput may be too low for interactive use | Measured in P5 before UX is fixed; scorer structured for a prefilter |
| iPlug2 has thinner documentation than JUCE, and Windows CMake setup is fiddly | P0 proves the full build matrix before any product code is written |
| VST3 SDK licensing unresolved | Settle before P7. CLAP and standalone are unaffected and can ship first |
