# Seedlathe

A seed-driven procedural synthesizer plugin (VST3 / CLAP / standalone), built on
[zyn.js](https://github.com/alexanderparker/zyn).

Type or roll a 32-bit integer and get a complete instrument. The same seed sounds the
same here as it does on the zyn demo page — enforced by automated tests, not by ear.

- Design: [`docs/superpowers/specs/2026-08-17-seedlathe-vst-design.md`](docs/superpowers/specs/2026-08-17-seedlathe-vst-design.md)
- Current plan: [`docs/superpowers/plans/2026-08-17-seedlathe-p0-p1-engine.md`](docs/superpowers/plans/2026-08-17-seedlathe-p0-p1-engine.md)

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
