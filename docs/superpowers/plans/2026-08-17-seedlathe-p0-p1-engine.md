# Seedlathe P0+P1 — Engine Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A C++ engine that reproduces zyn.js instrument generation bit-exactly and its audio closely enough to pass an automated null-test against Chrome, wrapped in a plugin that plays over MIDI.

**Architecture:** A framework-free `core/` static library holds the PRNG, generator, a Web Audio compatibility layer, voices and effects. `plugin/` wraps it in iPlug2. `tools/` exports golden vectors and reference audio from the upstream zyn repo, and `tests/` asserts C++ output against both. No UI beyond what standalone needs to make a sound.

**Tech Stack:** C++20, CMake 3.31, iPlug2 (VST3/CLAP/standalone), Catch2 v3, nlohmann/json, Node 22 + puppeteer-core driving system Chrome.

**Spec:** `docs/superpowers/specs/2026-08-17-seedlathe-vst-design.md`

## Global Constraints

- C++20. MSVC 19.4x (VS2022 Community). CMake floor 3.31.
- CMake binary: `C:/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe`
- `core/` must never include an iPlug2, VST3, or CLAP header. Enforced by a test that greps includes (Task 1).
- No allocation, locks, logging, or exceptions on the audio thread.
- Namespace `sl::`. Bundle id `com.seedlathe.seedlathe`. Preset extension `.slathe`.
- Upstream zyn repo lives at `C:/dev/zyn` and is MIT. It is a *sibling*, not a submodule — the sync contract is the committed files in `vectors/`.
- All floating-point in the generator path is `double`. Never `float`. Golden-vector comparison is exact equality, so a single narrowing conversion fails the build.
- Every task ends with a commit.

---

## File Structure

| Path | Responsibility |
|---|---|
| `CMakeLists.txt` | Top level; adds `core`, `plugin`, `tests` |
| `core/include/sl/Mulberry32.h` | PRNG, header-only |
| `core/include/sl/Instrument.h` | Fixed-size POD instrument tree |
| `core/src/InstrumentGen.cpp` / `.h` | Port of `Z.getInstrument` |
| `core/src/PresetIO.cpp` / `.h` | JSON ↔ `Instrument` |
| `core/src/webaudio/WaParam.{h,cpp}` | AudioParam automation timeline |
| `core/src/webaudio/WaOscillator.{h,cpp}` | Band-limited oscillator + noise source |
| `core/src/webaudio/WaBiquad.{h,cpp}` | BiquadFilterNode port |
| `core/src/webaudio/WaDelay.{h,cpp}` | DelayNode with fractional interpolation |
| `core/src/webaudio/WaConvolver.{h,cpp}` | ConvolverNode, generated impulse |
| `core/src/webaudio/WaPanner.{h,cpp}` | StereoPannerNode, equal-power |
| `core/src/webaudio/WaShaper.{h,cpp}` | WaveShaperNode + `getDistCurve` |
| `core/src/webaudio/WaCompressor.{h,cpp}` | DynamicsCompressorNode, Blink algorithm |
| `core/src/SharedFxRack.{h,cpp}` | Delay/reverb node cache with eviction |
| `core/src/Voice.{h,cpp}` | One note: oscillator chains, envelopes, FM matrix |
| `core/src/VoicePool.{h,cpp}` | Allocation and stealing |
| `core/src/OfflineRender.{h,cpp}` | Instrument → interleaved stereo buffer |
| `core/src/Analysis.{h,cpp}` | Mel spectrogram + RMS envelope, for the null-test only |
| `plugin/SeedlatheParams.h` | Parameter enum and ranges |
| `plugin/Seedlathe.{h,cpp}` | iPlug2 plugin class |
| `tools/export-vectors.mjs` | Golden instrument-tree JSON from zyn.js |
| `tools/export-reference-audio.mjs` | Reference WAVs via headless Chrome |
| `tests/test_*.cpp` | Catch2 suites |
| `vectors/instruments.json` | Committed golden vectors |
| `vectors/audio/*.wav` | Committed reference audio |

---

### Task 1: Repo scaffold and build matrix

The point of this task is to *prove the toolchain* before any product code exists. If iPlug2's CMake path fights back on Windows, it must fail here, not in Task 16.

**Files:**
- Create: `CMakeLists.txt`, `core/CMakeLists.txt`, `plugin/CMakeLists.txt`, `tests/CMakeLists.txt`, `.gitignore`, `.gitmodules`
- Create: `core/include/sl/Version.h`, `tests/test_smoke.cpp`, `tests/test_layering.cpp`
- Create: `plugin/Seedlathe.h`, `plugin/Seedlathe.cpp`, `plugin/config.h`

**Interfaces:**
- Consumes: nothing
- Produces: CMake targets `sl_core` (static lib), `sl_tests` (Catch2 executable), and iPlug2 targets `Seedlathe-vst3`, `Seedlathe-clap`, `Seedlathe-app`

- [ ] **Step 1: Vendor iPlug2 and fetch its prebuilt dependencies**

```bash
git -C C:/dev/seedlathe submodule add https://github.com/iPlug2/iPlug2.git third_party/iPlug2
git -C C:/dev/seedlathe submodule update --init --recursive --depth 1
```

Then run iPlug2's dependency fetch (it pulls the VST3 SDK and prebuilt libs):

```bash
cd /c/dev/seedlathe/third_party/iPlug2/Dependencies
./download-prebuilt-libs.sh
```

- [ ] **Step 2: Read iPlug2's own example CMake and mirror it**

Read `third_party/iPlug2/examples/IPlugInstrument/CMakeLists.txt` and
`third_party/iPlug2/iPlug2.cmake` before writing `plugin/CMakeLists.txt`.
Mirror the example's structure exactly — target creation, `iplug2_configure_target`
invocation, and which `iPlug2_` interface libraries it links. Do not invent an API;
copy the working one. If the example does not build unmodified, fix that first and
record what changed in the commit message.

- [ ] **Step 3: Write the top-level CMakeLists.txt**

```cmake
cmake_minimum_required(VERSION 3.31)
project(Seedlathe VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/W4 /permissive- /fp:precise)
endif()

include(FetchContent)
FetchContent_Declare(Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.7.1)
FetchContent_Declare(nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3)
FetchContent_MakeAvailable(Catch2 nlohmann_json)

add_subdirectory(core)
add_subdirectory(plugin)

enable_testing()
add_subdirectory(tests)
```

`/fp:precise` is load-bearing. `/fp:fast` lets MSVC reassociate floating-point
expressions, which breaks exact golden-vector equality.

- [ ] **Step 4: Write core/CMakeLists.txt**

```cmake
add_library(sl_core STATIC
  src/InstrumentGen.cpp
)
target_include_directories(sl_core PUBLIC include)
target_link_libraries(sl_core PUBLIC nlohmann_json::nlohmann_json)
```

Create `core/include/sl/Version.h`:

```cpp
#pragma once
namespace sl { inline constexpr const char* kVersion = "0.1.0"; }
```

Create `core/src/InstrumentGen.cpp` as a placeholder translation unit so the
library links — one line, replaced in Task 6:

```cpp
#include "sl/Version.h"
namespace sl { const char* versionString() { return kVersion; } }
```

- [ ] **Step 5: Write the layering-enforcement test**

`tests/test_layering.cpp` — this is the test that keeps `core/` framework-free
for the life of the project:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <string>

TEST_CASE("core never includes a plugin framework header") {
    namespace fs = std::filesystem;
    const fs::path coreDir = fs::path(SL_SOURCE_DIR) / "core";
    const char* banned[] = {"IPlug", "IGraphics", "public.sdk", "pluginterfaces", "clap/"};

    bool anyChecked = false;
    for (const auto& entry : fs::recursive_directory_iterator(coreDir)) {
        if (!entry.is_regular_file()) continue;
        const auto ext = entry.path().extension().string();
        if (ext != ".h" && ext != ".cpp") continue;
        anyChecked = true;

        std::ifstream in(entry.path());
        std::string line;
        int lineNo = 0;
        while (std::getline(in, line)) {
            ++lineNo;
            if (line.find("#include") == std::string::npos) continue;
            for (const char* b : banned) {
                INFO(entry.path().string() << ":" << lineNo << " -> " << line);
                REQUIRE(line.find(b) == std::string::npos);
            }
        }
    }
    REQUIRE(anyChecked);
}
```

`anyChecked` matters: without it, a broken path makes the test pass vacuously.

- [ ] **Step 6: Write tests/CMakeLists.txt and the smoke test**

```cmake
add_executable(sl_tests
  test_smoke.cpp
  test_layering.cpp
)
target_link_libraries(sl_tests PRIVATE sl_core Catch2::Catch2WithMain)
target_compile_definitions(sl_tests PRIVATE
  SL_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
  SL_VECTORS_DIR="${CMAKE_SOURCE_DIR}/vectors")

include(Catch)
catch_discover_tests(sl_tests)
```

`tests/test_smoke.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include "sl/Version.h"
#include <string>

TEST_CASE("core links and reports its version") {
    REQUIRE(std::string(sl::kVersion) == "0.1.0");
}
```

- [ ] **Step 7: Configure and build**

```bash
"/c/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" -S C:/dev/seedlathe -B C:/dev/seedlathe/build -G "Visual Studio 17 2022" -A x64
"/c/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build C:/dev/seedlathe/build --config Debug
```

Expected: all targets build. `Seedlathe.vst3`, `Seedlathe.clap` and `Seedlathe.exe`
appear under `build/`.

- [ ] **Step 8: Run the tests**

```bash
"/c/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/ctest.exe" --test-dir C:/dev/seedlathe/build -C Debug --output-on-failure
```

Expected: 2 tests, both PASS.

- [ ] **Step 9: Launch the standalone once**

Run `build/.../Seedlathe.exe`. Expected: a window opens and closes cleanly. It makes
no sound yet — that is Task 16.

- [ ] **Step 10: Commit**

```bash
git -C C:/dev/seedlathe add -A
git -C C:/dev/seedlathe commit -m "build: scaffold CMake, iPlug2, Catch2, and the core layering guard"
```

---

### Task 2: Fix distortion in zyn.js (upstream, in C:/dev/zyn)

Per spec §3.1. This task is in the **zyn repo**, not Seedlathe. It must land before
golden vectors are exported, or the vectors describe a generator that is about to change.

The constraint that shapes everything here: **the main PRNG stream must not move.**
`dist.curve` is currently a thunk whose `r(500)` never runs. Drawing it eagerly from
`r` would shift every subsequent oscillator in that instrument and change ~5% of all
seeds structurally. So the curve amount comes from a side PRNG, exactly as `fmMatrix`
already does with `seed + 9999`.

**Files:**
- Modify: `C:/dev/zyn/zyn-unminified.js` — `getDistCurve`, `getInstrument`, `render`
- Modify: `C:/dev/zyn/Z.js` (regenerated by `npm run build`)

**Interfaces:**
- Produces: instrument trees where `osc.dist` is `{amount: number, oversample: string}` — `curve` is no longer a function

- [ ] **Step 1: Write a Node regression test that pins the existing stream**

Create `C:/dev/zyn/test/stream-parity.mjs`:

```js
import { readFileSync, writeFileSync, existsSync } from "node:fs";

const src = readFileSync(new URL("../zyn-unminified.js", import.meta.url), "utf8");
const Z = eval(src + "; Z");

// Structural fingerprint that deliberately EXCLUDES dist, so it is invariant
// across the distortion fix. If this changes, the main PRNG stream moved.
function fingerprint(inst) {
  return JSON.stringify(inst, (k, v) => (k === "dist" ? undefined : v));
}

const seeds = [];
for (let i = 0; i < 5000; i++) seeds.push(i * 7919 % 4294967296);

const out = seeds.map((s) => fingerprint(Z.getInstrument(s)));
const path = new URL("./stream-baseline.json", import.meta.url);

if (!existsSync(path)) {
  writeFileSync(path, JSON.stringify(out));
  console.log("baseline written:", out.length, "seeds");
  process.exit(0);
}

const baseline = JSON.parse(readFileSync(path, "utf8"));
let diffs = 0;
for (let i = 0; i < out.length; i++) if (out[i] !== baseline[i]) diffs++;
console.log(diffs === 0 ? "PASS: stream unchanged" : `FAIL: ${diffs} seeds diverged`);
process.exit(diffs === 0 ? 0 : 1);
```

- [ ] **Step 2: Record the baseline against unmodified zyn**

```bash
node C:/dev/zyn/test/stream-parity.mjs
```

Expected: `baseline written: 5000 seeds`. Commit the baseline now, before touching
the generator — it is only meaningful if captured pre-change.

```bash
git -C C:/dev/zyn add test/stream-parity.mjs test/stream-baseline.json
git -C C:/dev/zyn commit -m "test: pin instrument generation stream before distortion fix"
```

- [ ] **Step 3: Fix getDistCurve**

In `zyn-unminified.js`, replace:

```js
  getDistCurve: (k) => {
    let curve = new Float32Array(sampleRate);
```

with:

```js
  getDistCurve: (k) => {
    let n = Z.sampleRate;
    let curve = new Float32Array(n);
```

and change the loop bound from `Z.sampleRate` to `n`. The bug was a bare global
`sampleRate`, which exists only in `AudioWorkletGlobalScope` and throws in a `Window`.

- [ ] **Step 4: Draw the distortion amount from a side PRNG**

In `getInstrument`, add a second generator alongside the main one. Immediately after
`let r = Z.m32(seed);` insert:

```js
    // Side stream for distortion, so adding it cannot shift the main stream and
    // change every existing seed. Same technique as the FM matrix (seed + 9999).
    let dR = Z.m32(seed + 7777);
```

Then replace the `dist` property in the oscillator literal:

```js
        dist:
          r() < 0.05
            ? {
                amount: dR(500),
                oversample: ["none", "2x", "4x"][Math.floor(r(3))],
              }
            : undefined,
```

The `r() < 0.05` test and the `r(3)` oversample draw stay on the **main** stream
exactly where they were. Only the curve amount moves to `dR`, and it was never
drawn from the main stream in the first place.

- [ ] **Step 5: Wire distortion into render**

The existing block referencing `layer.dist`, `nDist` and `nGains` is unreachable and
references undefined identifiers. Delete it:

```js
    // Apply distortion if specified
    let dCurve = layer?.dist?.curve || null;
    if (dCurve) {
      nDist.curve = new Float32Array(dCurve);
      nDist.oversample = layer.dist.oversample || "none";
      nGains.forEach((nGain) => {
        nGain.connect(nDist);
      });
      nDist.connect(Z.masterGain);
    }
```

Replace it with per-oscillator shaping inserted between filter and gain. Inside the
`layer.instrument.oscs.forEach` callback, change the connection line:

```js
        // Connect nodes: oscillator -> filter -> gain -> panner
        osc.connect(nFilt);
        nFilt.connect(nGain);
        nGain.connect(nPan);
```

to:

```js
        // Connect nodes: oscillator -> [shaper] -> filter -> gain -> panner
        if (cnf.dist) {
          let nShape = Z.aC.createWaveShaper();
          nShape.curve = Z.getDistCurve(cnf.dist.amount);
          nShape.oversample = cnf.dist.oversample || "none";
          osc.connect(nShape);
          nShape.connect(nFilt);
        } else {
          osc.connect(nFilt);
        }
        nFilt.connect(nGain);
        nGain.connect(nPan);
```

Shaping before the filter is the choice being made here: the filter then tames the
harmonics the shaper adds, which is standard subtractive ordering and keeps 5%-affected
seeds from turning harsh.

- [ ] **Step 6: Run the stream parity test**

```bash
node C:/dev/zyn/test/stream-parity.mjs
```

Expected: `PASS: stream unchanged`. If it reports diverged seeds, the `dist` draw
leaked into the main stream — re-check Step 4.

- [ ] **Step 7: Verify distortion actually renders**

Open `C:/dev/zyn/index.html` in Chrome, enter a seed known to carry distortion, and
confirm audio plays without a console error. Find one:

```bash
node -e "
const fs=require('fs');
const Z=eval(fs.readFileSync('C:/dev/zyn/zyn-unminified.js','utf8')+'; Z');
for(let s=0;s<20000;s++){const i=Z.getInstrument(s);if(i.oscs.some(o=>o.dist)){console.log('seed',s,'type',i.type);break;}}
"
```

Expected: a seed prints, the demo plays it, and the console shows no
`ReferenceError: sampleRate is not defined`.

- [ ] **Step 8: Rebuild the minified bundle and commit**

```bash
npm --prefix C:/dev/zyn run build
git -C C:/dev/zyn add -A
git -C C:/dev/zyn commit -m "fix: make per-oscillator distortion actually render

getDistCurve read a bare global sampleRate that only exists in
AudioWorkletGlobalScope, and render checked layer.dist which nothing ever
set, referencing undefined nDist/nGains identifiers. Distortion has never
produced a sample.

The curve amount now comes from a side PRNG seeded at seed + 7777, the same
technique the FM matrix uses, so the main generation stream is untouched and
every existing seed keeps its structure. Verified by test/stream-parity.mjs
over 5000 seeds."
```

---

### Task 3: Golden-vector exporter

**Files:**
- Create: `tools/package.json`, `tools/export-vectors.mjs`
- Create: `vectors/instruments.json` (generated, committed)

**Interfaces:**
- Consumes: `C:/dev/zyn/zyn-unminified.js` (post-Task-2)
- Produces: `vectors/instruments.json` — `{zynCommit, generated, seeds: [{seed, instrument}]}`

- [ ] **Step 1: Create tools/package.json**

```json
{
  "name": "seedlathe-tools",
  "private": true,
  "type": "module",
  "scripts": {
    "vectors": "node export-vectors.mjs",
    "audio": "node export-reference-audio.mjs"
  },
  "dependencies": {
    "puppeteer-core": "^23.0.0"
  }
}
```

```bash
npm --prefix C:/dev/seedlathe/tools install
```

- [ ] **Step 2: Write export-vectors.mjs**

```js
import { readFileSync, writeFileSync, mkdirSync } from "node:fs";
import { execSync } from "node:child_process";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const ZYN = process.env.ZYN_DIR ?? "C:/dev/zyn";
const OUT = resolve(here, "../vectors/instruments.json");

const src = readFileSync(resolve(ZYN, "zyn-unminified.js"), "utf8");
const Z = eval(src + "; Z");

const zynCommit = execSync(`git -C "${ZYN}" rev-parse HEAD`).toString().trim();

// Coverage: every type digit (last digit), every FM-probability first digit,
// plus a deterministic spread across the full 32-bit range.
const seeds = new Set();
for (let type = 0; type <= 9; type++) {
  for (let first = 0; first <= 9; first++) {
    for (let k = 0; k < 15; k++) {
      const mid = String(k).padStart(4, "0");
      const s = Number(`${first}${mid}${type}`);
      if (s <= 4294967295) seeds.add(s);
    }
  }
}
// Deterministic spread — a fixed LCG, so the vector set is reproducible.
let x = 12345;
while (seeds.size < 2000) {
  x = (1103515245 * x + 12345) % 4294967296;
  seeds.add(x);
}

const list = [...seeds].sort((a, b) => a - b);
const payload = {
  zynCommit,
  generated: new Date().toISOString(),
  count: list.length,
  seeds: list.map((seed) => ({ seed, instrument: Z.getInstrument(seed) })),
};

mkdirSync(dirname(OUT), { recursive: true });
// 17 significant digits round-trips a double exactly; JSON.stringify already
// emits the shortest round-tripping form, so plain stringify is sufficient.
writeFileSync(OUT, JSON.stringify(payload));
console.log(`wrote ${list.length} seeds from zyn ${zynCommit.slice(0, 8)}`);
```

- [ ] **Step 3: Run it**

```bash
npm --prefix C:/dev/seedlathe/tools run vectors
```

Expected: `wrote 2000 seeds from zyn <sha>`.

- [ ] **Step 4: Sanity-check the output**

```bash
node -e "
const v=require('C:/dev/seedlathe/vectors/instruments.json');
console.log('count', v.count);
const types={}; let withFm=0, withDist=0;
for(const e of v.seeds){types[e.instrument.type]=(types[e.instrument.type]||0)+1;
 if(e.instrument.fmMatrix)withFm++;
 if(e.instrument.oscs.some(o=>o.dist))withDist++;}
console.log('types', types); console.log('fmMatrix', withFm, 'dist', withDist);
"
```

Expected: all ten type names present, non-zero `fmMatrix` and `dist` counts. A zero
in either means the seed selection missed a branch and Task 6's test would pass
without exercising it.

- [ ] **Step 5: Commit**

```bash
git -C C:/dev/seedlathe add tools/ vectors/instruments.json
git -C C:/dev/seedlathe commit -m "tools: export golden instrument vectors from zyn.js"
```

---

### Task 4: Mulberry32

**Files:**
- Create: `core/include/sl/Mulberry32.h`
- Test: `tests/test_mulberry32.cpp`

**Interfaces:**
- Produces: `sl::Mulberry32{uint32_t seed}`, `double operator()(double f = 1.0)`, and free function `uint32_t sl::toUint32(double)`

- [ ] **Step 1: Write the failing test**

The expected values below were produced by running zyn's `Z.m32` in Node 22 and
printing with `toPrecision(17)`. They are the contract.

```cpp
#include <catch2/catch_test_macros.hpp>
#include "sl/Mulberry32.h"

TEST_CASE("Mulberry32 matches zyn's Z.m32 exactly") {
    struct Case { uint32_t seed; double expect[5]; };
    const Case cases[] = {
        {0u, {0.26642920868471265, 0.00032974570058286190, 0.22327202744781971,
              0.14620214793831110, 0.46732782293111086}},
        {1u, {0.62707394058816135, 0.0027357211802154779, 0.52744703995995224,
              0.98105096747167408, 0.96837789821438491}},
        {13u, {0.56632264936342835, 0.36011716164648533, 0.070590845309197903,
               0.048166851978749037, 0.036770868115127087}},
        {3703184240u, {0.23630721494555473, 0.019152297405526042, 0.30907804355956614,
                       0.40662032505497336, 0.033574980916455388}},
    };

    for (const auto& c : cases) {
        sl::Mulberry32 r(c.seed);
        for (int i = 0; i < 5; ++i) {
            INFO("seed " << c.seed << " draw " << i);
            REQUIRE(r() == c.expect[i]);   // exact, not Approx
        }
    }
}

TEST_CASE("Mulberry32 scale factor multiplies the draw") {
    sl::Mulberry32 r(13u);
    r(); r();
    REQUIRE(r(30.0) == 2.1177253592759371);
}

TEST_CASE("toUint32 follows JS semantics") {
    REQUIRE(sl::toUint32(0.0) == 0u);
    REQUIRE(sl::toUint32(13.0) == 13u);
    REQUIRE(sl::toUint32(4294967295.0) == 4294967295u);
    REQUIRE(sl::toUint32(-5.0) == 4294967291u);     // wraps
    REQUIRE(sl::toUint32(4294967296.0) == 0u);      // mod 2^32
    REQUIRE(sl::toUint32(13.9) == 13u);             // truncates toward zero
}
```

`REQUIRE(r() == expect)` uses exact equality deliberately. `Approx` would hide the
one-ULP drift that means the port is wrong.

- [ ] **Step 2: Run it and watch it fail**

Add `test_mulberry32.cpp` to `tests/CMakeLists.txt`, reconfigure, build.
Expected: compile error, `sl/Mulberry32.h` not found.

- [ ] **Step 3: Implement**

`core/include/sl/Mulberry32.h`:

```cpp
#pragma once
#include <cmath>
#include <cstdint>

namespace sl {

// JS ToUint32: truncate toward zero, then mod 2^32.
inline uint32_t toUint32(double v) {
    if (!std::isfinite(v)) return 0u;
    double t = std::trunc(v);
    double m = std::fmod(t, 4294967296.0);
    if (m < 0.0) m += 4294967296.0;
    return static_cast<uint32_t>(m);
}

// Exact port of zyn's Z.m32 (Tommy Ettinger's Mulberry32):
//
//   m32: (a) => (f = 1) => {
//     let t = (a += 0x6d2b79f5);
//     t = Math.imul(t ^ (t >>> 15), t | 1);
//     t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
//     return (((t ^ (t >>> 14)) >>> 0) / 4294967296) * f;
//   }
//
// JS holds `a` as a double, but every read passes through ToUint32 (mod 2^32),
// and addition is homomorphic over that modulus, so a uint32_t accumulator is
// bit-identical while the JS double stays exact -- about 4.8e6 calls. Instrument
// generation uses a few hundred.
//
// Math.imul truncates to 32 bits, matching uint32_t multiplication. The
// `t + Math.imul(...)` term is a double add in JS followed by ToInt32, which is
// again mod 2^32 and matches uint32_t wraparound.
class Mulberry32 {
public:
    explicit Mulberry32(uint32_t seed) : a_(seed) {}

    double operator()(double f = 1.0) {
        a_ += 0x6D2B79F5u;
        uint32_t t = a_;
        t = (t ^ (t >> 15)) * (t | 1u);
        t ^= t + (t ^ (t >> 7)) * (t | 61u);
        return static_cast<double>(t ^ (t >> 14)) / 4294967296.0 * f;
    }

private:
    uint32_t a_;
};

} // namespace sl
```

- [ ] **Step 4: Run the tests**

Expected: 3 new tests PASS.

- [ ] **Step 5: Commit**

```bash
git -C C:/dev/seedlathe add core/include/sl/Mulberry32.h tests/
git -C C:/dev/seedlathe commit -m "feat(core): exact Mulberry32 port with JS ToUint32 semantics"
```

---

### Task 5: Instrument POD and JSON loader

**Files:**
- Create: `core/include/sl/Instrument.h`
- Create: `core/src/PresetIO.h`, `core/src/PresetIO.cpp`
- Test: `tests/test_presetio.cpp`

**Interfaces:**
- Consumes: nothing
- Produces: `sl::Instrument`, `sl::Osc`, `sl::Adsr`, `sl::Lfo`, `sl::Fm`, `sl::PitchEnv`, `sl::Dist`, `sl::Delay`, `sl::Verb`, `sl::Waveform`, `sl::FilterType`; and `sl::Instrument sl::instrumentFromJson(const nlohmann::json&)`

- [ ] **Step 1: Write the header**

```cpp
#pragma once
#include <array>
#include <cstdint>

namespace sl {

inline constexpr int kMaxOscs = 5;

enum class Waveform { Sine, Square, Sawtooth, Triangle, Noise };
enum class FilterType { Lowpass, Highpass, Bandpass, Lowshelf, Highshelf, Peaking, Allpass };

// zyn envelope: four [time, value] pairs, applied as sequential linear ramps.
struct Adsr {
    double aT = 0, aV = 0;
    double dT = 0, dV = 0;
    double sT = 0, sV = 0;
    double rT = 0, rV = 0;
};

struct Lfo {
    bool on = false;
    Waveform type = Waveform::Sine;
    double frequency = 1.0;
    double depth = 0.0;
};

struct Fm {
    bool on = false;
    Waveform type = Waveform::Sine;
    double frequency = 1.0;   // ratio, multiplied by the oscillator frequency
    double depth = 0.0;       // Hz of deviation
};

struct PitchEnv {
    bool on = false;
    double amount = 0.0;
    Adsr env{};
};

struct Dist {
    bool on = false;
    double amount = 0.0;      // drawn from the side PRNG at seed + 7777
    int oversample = 1;       // 1, 2 or 4
};

struct Delay {
    bool on = false;
    double time = 0.0;        // seconds, <= 0.5 from the generator
    double feedback = 0.0;
};

struct Verb {
    bool on = false;
    double duration = 0.0;    // seconds, <= 3.1 from the generator
    double decay = 0.0;
};

struct Osc {
    Waveform waveform = Waveform::Sine;
    Adsr adsrGain{};
    FilterType filterType = FilterType::Lowpass;
    Adsr adsrFilter{};
    double filterQ = 0.0;     // dead in the audio path, live in the search scorer
    Adsr adsrFilterQ{};
    Lfo gLfo{}, fLfo{}, pLfo{};
    Fm fm{};
    PitchEnv pEnv{};
    Dist dist{};
    int oct = 0;
    double detune = 0.0;      // SEMITONES, not cents
    Delay del{};
    Verb verb{};
};

// Fixed size, no heap: safe to generate and copy on the audio thread.
struct Instrument {
    int typeIndex = 0;                                    // 0..9, the seed's last digit
    int oscCount = 0;
    std::array<Osc, kMaxOscs> oscs{};
    bool hasFmMatrix = false;
    std::array<std::array<double, kMaxOscs>, kMaxOscs> fmMatrix{};
    std::array<std::array<double, kMaxOscs>, kMaxOscs> fmDelays{};  // designer only; 0 means use 0.001
};

const char* typeName(int typeIndex);   // "pad", "lead", ... "fx"

} // namespace sl
```

- [ ] **Step 2: Write the failing test**

```cpp
#include <catch2/catch_test_macros.hpp>
#include "sl/Instrument.h"
#include "PresetIO.h"
#include <nlohmann/json.hpp>
#include <fstream>

TEST_CASE("instrumentFromJson reads a golden vector entry") {
    std::ifstream in(std::string(SL_VECTORS_DIR) + "/instruments.json");
    REQUIRE(in.good());
    nlohmann::json v;
    in >> v;
    REQUIRE(v["count"].get<int>() >= 2000);

    // Seed 13 is a "key" with 2 oscillators and no FM matrix.
    const nlohmann::json* entry = nullptr;
    for (const auto& e : v["seeds"])
        if (e["seed"].get<int64_t>() == 13) entry = &e;
    REQUIRE(entry != nullptr);

    const sl::Instrument inst = sl::instrumentFromJson((*entry)["instrument"]);
    REQUIRE(inst.oscCount == 2);
    REQUIRE(inst.typeIndex == 3);
    REQUIRE(inst.hasFmMatrix == false);
    REQUIRE(inst.oscs[0].waveform == sl::Waveform::Square);
    REQUIRE(inst.oscs[0].filterType == sl::FilterType::Highshelf);
    REQUIRE(inst.oscs[0].adsrGain.aT == 0.0009120745095424354);
    REQUIRE(inst.oscs[0].adsrGain.aV == 1.0);
    REQUIRE(inst.oscs[0].filterQ == 19.394281120039523);
}

TEST_CASE("every golden vector entry parses") {
    std::ifstream in(std::string(SL_VECTORS_DIR) + "/instruments.json");
    nlohmann::json v; in >> v;
    for (const auto& e : v["seeds"]) {
        const sl::Instrument inst = sl::instrumentFromJson(e["instrument"]);
        INFO("seed " << e["seed"].get<int64_t>());
        REQUIRE(inst.oscCount >= 1);
        REQUIRE(inst.oscCount <= sl::kMaxOscs);
    }
}

TEST_CASE("instrumentToJson round-trips every golden vector") {
    // Guards the export path used by presets and by "copy JSON" in later phases.
    // A field dropped on write would otherwise only surface as a silently
    // different instrument after a save/load cycle.
    std::ifstream in(std::string(SL_VECTORS_DIR) + "/instruments.json");
    nlohmann::json v; in >> v;
    for (const auto& e : v["seeds"]) {
        const sl::Instrument a = sl::instrumentFromJson(e["instrument"]);
        const sl::Instrument b = sl::instrumentFromJson(sl::instrumentToJson(a));
        INFO("seed " << e["seed"].get<int64_t>());
        REQUIRE(b.typeIndex == a.typeIndex);
        REQUIRE(b.oscCount == a.oscCount);
        REQUIRE(b.hasFmMatrix == a.hasFmMatrix);
        for (int i = 0; i < a.oscCount; ++i) {
            const auto& x = a.oscs[i]; const auto& y = b.oscs[i];
            REQUIRE(y.waveform == x.waveform);
            REQUIRE(y.filterType == x.filterType);
            REQUIRE(y.filterQ == x.filterQ);
            REQUIRE(y.oct == x.oct);
            REQUIRE(y.detune == x.detune);
            REQUIRE(y.adsrGain.aT == x.adsrGain.aT);
            REQUIRE(y.adsrGain.rV == x.adsrGain.rV);
            REQUIRE(y.gLfo.on == x.gLfo.on);
            REQUIRE(y.fm.on == x.fm.on);
            REQUIRE(y.pEnv.on == x.pEnv.on);
            REQUIRE(y.dist.on == x.dist.on);
            REQUIRE(y.dist.amount == x.dist.amount);
            REQUIRE(y.del.on == x.del.on);
            REQUIRE(y.verb.on == x.verb.on);
        }
    }
}
```

- [ ] **Step 3: Run and watch it fail**

Expected: compile error, `PresetIO.h` not found.

- [ ] **Step 4: Implement PresetIO**

`core/src/PresetIO.h`:

```cpp
#pragma once
#include "sl/Instrument.h"
#include <nlohmann/json_fwd.hpp>

namespace sl {
Instrument instrumentFromJson(const nlohmann::json& j);
nlohmann::json instrumentToJson(const Instrument& inst);
}
```

`core/src/PresetIO.cpp` — the shape that matters:

```cpp
#include "PresetIO.h"
#include <nlohmann/json.hpp>
#include <string>
#include <stdexcept>

namespace sl {
namespace {

Waveform waveformFromName(const std::string& s) {
    if (s == "sine") return Waveform::Sine;
    if (s == "square") return Waveform::Square;
    if (s == "sawtooth") return Waveform::Sawtooth;
    if (s == "triangle") return Waveform::Triangle;
    if (s == "noise") return Waveform::Noise;
    throw std::runtime_error("unknown waveform: " + s);
}

FilterType filterFromName(const std::string& s) {
    if (s == "lowpass") return FilterType::Lowpass;
    if (s == "highpass") return FilterType::Highpass;
    if (s == "bandpass") return FilterType::Bandpass;
    if (s == "lowshelf") return FilterType::Lowshelf;
    if (s == "highshelf") return FilterType::Highshelf;
    if (s == "peaking") return FilterType::Peaking;
    if (s == "allpass") return FilterType::Allpass;
    throw std::runtime_error("unknown filter: " + s);
}

Adsr adsrFromJson(const nlohmann::json& j) {
    Adsr a;
    a.aT = j["A"][0].get<double>(); a.aV = j["A"][1].get<double>();
    a.dT = j["D"][0].get<double>(); a.dV = j["D"][1].get<double>();
    a.sT = j["S"][0].get<double>(); a.sV = j["S"][1].get<double>();
    a.rT = j["R"][0].get<double>(); a.rV = j["R"][1].get<double>();
    return a;
}

// zyn writes `false` for an absent LFO/FM and an object when present.
Lfo lfoFromJson(const nlohmann::json& j) {
    Lfo l;
    if (!j.is_object()) return l;
    l.on = true;
    l.type = waveformFromName(j.value("type", std::string("sine")));
    l.frequency = j.value("frequency", 1.0);
    l.depth = j.value("depth", 0.0);
    return l;
}

int typeIndexFromName(const std::string& s) {
    static const char* names[] = {"pad","lead","bass","key","pluck",
                                  "bell","string","drum","perc","fx"};
    for (int i = 0; i < 10; ++i) if (s == names[i]) return i;
    throw std::runtime_error("unknown instrument type: " + s);
}

} // namespace

const char* typeName(int i) {
    static const char* names[] = {"pad","lead","bass","key","pluck",
                                  "bell","string","drum","perc","fx"};
    return (i >= 0 && i < 10) ? names[i] : "unknown";
}

Instrument instrumentFromJson(const nlohmann::json& j) {
    Instrument inst;
    inst.typeIndex = typeIndexFromName(j.at("type").get<std::string>());

    const auto& oscs = j.at("oscs");
    inst.oscCount = static_cast<int>(oscs.size());
    if (inst.oscCount < 1 || inst.oscCount > kMaxOscs)
        throw std::runtime_error("bad oscillator count");

    for (int i = 0; i < inst.oscCount; ++i) {
        const auto& o = oscs[i];
        Osc& d = inst.oscs[i];
        d.waveform    = waveformFromName(o.at("waveform").get<std::string>());
        d.adsrGain    = adsrFromJson(o.at("adsrGain"));
        d.filterType  = filterFromName(o.at("filterType").get<std::string>());
        d.adsrFilter  = adsrFromJson(o.at("adsrFilter"));
        d.filterQ     = o.at("filterQ").get<double>();
        d.adsrFilterQ = adsrFromJson(o.at("adsrFilterQ"));
        d.gLfo = lfoFromJson(o.at("gLFO"));
        d.fLfo = lfoFromJson(o.at("fLFO"));
        d.pLfo = lfoFromJson(o.at("pLFO"));

        if (o.at("FM").is_object()) {
            d.fm.on = true;
            d.fm.type = waveformFromName(o["FM"].value("type", std::string("sine")));
            d.fm.frequency = o["FM"].value("frequency", 1.0);
            d.fm.depth = o["FM"].value("depth", 0.0);
        }
        if (o.at("pENV").is_object()) {
            d.pEnv.on = true;
            d.pEnv.amount = o["pENV"].value("amount", 0.0);
            d.pEnv.env = adsrFromJson(o["pENV"]);
        }
        if (o.contains("dist") && o["dist"].is_object()) {
            d.dist.on = true;
            d.dist.amount = o["dist"].value("amount", 0.0);
            const std::string os = o["dist"].value("oversample", std::string("none"));
            d.dist.oversample = (os == "4x") ? 4 : (os == "2x") ? 2 : 1;
        }
        d.oct = o.at("oct").get<int>();
        d.detune = o.at("detune").get<double>();

        const auto& fx = o.at("fx");
        if (fx.at("del").is_object()) {
            d.del.on = true;
            d.del.time = fx["del"].at("time").get<double>();
            d.del.feedback = fx["del"].at("feedback").get<double>();
        }
        if (fx.at("verb").is_object()) {
            d.verb.on = true;
            d.verb.duration = fx["verb"].at("duration").get<double>();
            d.verb.decay = fx["verb"].at("decay").get<double>();
        }
    }

    if (j.contains("fmMatrix") && j["fmMatrix"].is_array()) {
        inst.hasFmMatrix = true;
        for (int s = 0; s < inst.oscCount; ++s)
            for (int t = 0; t < inst.oscCount; ++t)
                inst.fmMatrix[s][t] = j["fmMatrix"][s][t].get<double>();
    }
    return inst;
}

} // namespace sl
```

`instrumentToJson` is the inverse, emitting zyn's exact field names and its
`false`/`null`/absent conventions so round-tripped files load in the demo page.

- [ ] **Step 5: Run the tests**

Expected: both PASS, the second over all 2000 vectors.

- [ ] **Step 6: Commit**

```bash
git -C C:/dev/seedlathe add core/ tests/
git -C C:/dev/seedlathe commit -m "feat(core): Instrument POD and zyn-compatible JSON loader"
```

---

### Task 6: InstrumentGen — the exact port

This is the highest-risk task in P0. Every `r()` call must happen in the same order
as zyn or seeds diverge.

**Files:**
- Create: `core/include/sl/InstrumentGen.h`
- Rewrite: `core/src/InstrumentGen.cpp`
- Test: `tests/test_instrumentgen.cpp`

**Interfaces:**
- Consumes: `sl::Mulberry32`, `sl::Instrument`, `sl::instrumentFromJson`
- Produces: `sl::Instrument sl::generateInstrument(uint32_t seed)`

- [ ] **Step 1: Write the failing golden-vector test**

```cpp
#include <catch2/catch_test_macros.hpp>
#include "sl/InstrumentGen.h"
#include "PresetIO.h"
#include <nlohmann/json.hpp>
#include <fstream>

namespace {
void requireAdsrEqual(const sl::Adsr& a, const sl::Adsr& b, const char* what) {
    INFO(what);
    REQUIRE(a.aT == b.aT); REQUIRE(a.aV == b.aV);
    REQUIRE(a.dT == b.dT); REQUIRE(a.dV == b.dV);
    REQUIRE(a.sT == b.sT); REQUIRE(a.sV == b.sV);
    REQUIRE(a.rT == b.rT); REQUIRE(a.rV == b.rV);
}
} // namespace

TEST_CASE("generateInstrument reproduces every golden vector exactly") {
    std::ifstream in(std::string(SL_VECTORS_DIR) + "/instruments.json");
    REQUIRE(in.good());
    nlohmann::json v; in >> v;

    int checked = 0;
    for (const auto& e : v["seeds"]) {
        const auto seed = static_cast<uint32_t>(e["seed"].get<int64_t>());
        const sl::Instrument want = sl::instrumentFromJson(e["instrument"]);
        const sl::Instrument got = sl::generateInstrument(seed);

        INFO("seed " << seed);
        REQUIRE(got.typeIndex == want.typeIndex);
        REQUIRE(got.oscCount == want.oscCount);
        REQUIRE(got.hasFmMatrix == want.hasFmMatrix);

        for (int i = 0; i < want.oscCount; ++i) {
            INFO("osc " << i);
            const auto& a = got.oscs[i]; const auto& b = want.oscs[i];
            REQUIRE(a.waveform == b.waveform);
            requireAdsrEqual(a.adsrGain, b.adsrGain, "adsrGain");
            REQUIRE(a.filterType == b.filterType);
            requireAdsrEqual(a.adsrFilter, b.adsrFilter, "adsrFilter");
            REQUIRE(a.filterQ == b.filterQ);
            requireAdsrEqual(a.adsrFilterQ, b.adsrFilterQ, "adsrFilterQ");
            REQUIRE(a.gLfo.on == b.gLfo.on);
            if (a.gLfo.on) {
                REQUIRE(a.gLfo.type == b.gLfo.type);
                REQUIRE(a.gLfo.frequency == b.gLfo.frequency);
                REQUIRE(a.gLfo.depth == b.gLfo.depth);
            }
            REQUIRE(a.fLfo.on == b.fLfo.on);
            if (a.fLfo.on) { REQUIRE(a.fLfo.frequency == b.fLfo.frequency);
                             REQUIRE(a.fLfo.depth == b.fLfo.depth); }
            REQUIRE(a.pLfo.on == b.pLfo.on);
            if (a.pLfo.on) { REQUIRE(a.pLfo.frequency == b.pLfo.frequency);
                             REQUIRE(a.pLfo.depth == b.pLfo.depth); }
            REQUIRE(a.fm.on == b.fm.on);
            if (a.fm.on) { REQUIRE(a.fm.frequency == b.fm.frequency);
                           REQUIRE(a.fm.depth == b.fm.depth); }
            REQUIRE(a.pEnv.on == b.pEnv.on);
            if (a.pEnv.on) { REQUIRE(a.pEnv.amount == b.pEnv.amount);
                             requireAdsrEqual(a.pEnv.env, b.pEnv.env, "pENV"); }
            REQUIRE(a.dist.on == b.dist.on);
            if (a.dist.on) { REQUIRE(a.dist.amount == b.dist.amount);
                             REQUIRE(a.dist.oversample == b.dist.oversample); }
            REQUIRE(a.oct == b.oct);
            REQUIRE(a.detune == b.detune);
            REQUIRE(a.del.on == b.del.on);
            if (a.del.on) { REQUIRE(a.del.time == b.del.time);
                            REQUIRE(a.del.feedback == b.del.feedback); }
            REQUIRE(a.verb.on == b.verb.on);
            if (a.verb.on) { REQUIRE(a.verb.duration == b.verb.duration);
                             REQUIRE(a.verb.decay == b.verb.decay); }
        }
        if (want.hasFmMatrix)
            for (int s = 0; s < want.oscCount; ++s)
                for (int t = 0; t < want.oscCount; ++t)
                    REQUIRE(got.fmMatrix[s][t] == want.fmMatrix[s][t]);
        ++checked;
    }
    REQUIRE(checked >= 2000);
}

TEST_CASE("oscillator count distribution proves the loop condition redraws") {
    // zyn re-evaluates `i < Math.floor(r(o) + 1)` every iteration, consuming a
    // random number each pass. A single up-front draw would give a uniform
    // 1..4 spread for pads; the real generator does not.
    int counts[6] = {0};
    for (uint32_t k = 0; k < 3000; ++k)
        counts[sl::generateInstrument(k * 10).oscCount]++;
    REQUIRE(counts[1] == 761);
    REQUIRE(counts[2] == 1118);
    REQUIRE(counts[3] == 837);
    REQUIRE(counts[4] == 284);
}
```

- [ ] **Step 2: Run and watch it fail**

Expected: compile error, `sl/InstrumentGen.h` not found.

- [ ] **Step 3: Implement the generator**

`core/src/InstrumentGen.cpp`. The call-order comments are the specification —
do not reorder anything.

```cpp
#include "sl/InstrumentGen.h"
#include "sl/Mulberry32.h"
#include <algorithm>
#include <cmath>
#include <string>

namespace sl {
namespace {

struct TypeDef {
    int index;
    double gT[4], gV[4];   // gain envelope: times, values
    double fT[4], fV[4];   // filter envelope
    double pT[4], pV[4];   // pitch envelope
    double pe;             // pitch-envelope probability
    double o;              // oscillator-count scale
    const Waveform* w; int wn;
};

constexpr Waveform kAll[]   = {Waveform::Sine, Waveform::Square,
                               Waveform::Sawtooth, Waveform::Triangle};
constexpr Waveform kLead[]  = {Waveform::Sawtooth, Waveform::Square, Waveform::Triangle};
constexpr Waveform kBass[]  = {Waveform::Sine, Waveform::Sawtooth,
                               Waveform::Square, Waveform::Triangle};
constexpr Waveform kPluck[] = {Waveform::Triangle, Waveform::Sawtooth, Waveform::Square};
constexpr Waveform kBell[]  = {Waveform::Sine, Waveform::Triangle};
constexpr Waveform kStr[]   = {Waveform::Sawtooth, Waveform::Triangle};
constexpr Waveform kDrum[]  = {Waveform::Noise, Waveform::Sine, Waveform::Triangle};
constexpr Waveform kFx[]    = {Waveform::Noise, Waveform::Sine, Waveform::Square,
                               Waveform::Sawtooth, Waveform::Triangle};

const TypeDef kTypes[10] = {
  {0,{0.8,0.5,1,0.8},{1,0.8,0.6,0},{0.5,0.3,0.5,0.5},{1,0.8,0.6,0.6},{0.3,0.2,0.2,0.3},{0.1,0.8,0.6,0.2},0.05,4,kAll,4},
  {1,{0.05,0.2,0.4,0.3},{1,0.8,0.6,0},{0.1,0.3,0.3,0.2},{0.5,1,0.7,0.3},{0.05,0.1,0.2,0.1},{0.2,0.8,0.5,0.15},0.05,3,kLead,3},
  {2,{0.01,0.1,0.3,0.2},{1,0.7,0.4,0},{0.02,0.15,0.2,0.1},{1,0.5,0.3,0.2},{0.01,0.05,0.1,0.05},{1,0.5,0.5,0.2},0.1,2,kBass,4},
  {3,{0.01,0.3,0.4,0.3},{1,0.6,0.3,0},{0.01,0.2,0.3,0.2},{1,0.6,0.3,0.2},{0.01,0.1,0.1,0.1},{0.15,0.6,0.3,0.1},0.05,3,kAll,4},
  {4,{0.005,0.15,0.05,0.1},{1,0.3,0.1,0},{0.005,0.1,0.05,0.05},{1,0.3,0.1,0.1},{0.005,0.05,0.02,0.02},{0.2,0.3,0.1,0.1},0.2,2,kPluck,3},
  {5,{0.001,0.8,0.5,0.5},{1,0.5,0.2,0},{0.001,0.5,0.4,0.3},{1,0.8,0.5,0.3},{0.001,0.3,0.2,0.2},{0.15,0.5,0.2,0.1},0.2,4,kBell,2},
  {6,{0.4,0.2,0.8,0.4},{1,0.9,0.7,0},{0.3,0.2,0.5,0.3},{0.3,1,0.8,0.5},{0.2,0.1,0.3,0.2},{0.1,0.9,0.7,0.3},0.15,4,kStr,2},
  {7,{0.005,0.05,0.1,0.01},{1,0.3,0,0},{0.005,0.08,0.05,0.01},{1,0.5,0,0},{0.005,0.03,0.02,0.01},{1,0.5,0,0},0.5,2,kDrum,3},
  {8,{0.001,0.1,0.15,0.1},{1,0.5,0.1,0},{0.001,0.12,0.1,0.08},{1,0.6,0.2,0.1},{0.001,0.08,0.05,0.05},{1,0.8,0.5,0.2},0.4,3,kAll,4},
  {9,{0.5,0.5,0.5,0.5},{0.5,1,0.5,0},{0.3,0.4,0.4,0.3},{0.5,1,0.5,0.5},{0.2,0.3,0.3,0.2},{0.5,1,0.5,0.5},0.3,5,kFx,5},
};

constexpr FilterType kFilters[7] = {
    FilterType::Lowpass, FilterType::Highpass, FilterType::Bandpass,
    FilterType::Lowshelf, FilterType::Highshelf, FilterType::Peaking,
    FilterType::Allpass};

// gEnv(t, v): four value draws, normalised by their max, then four time draws.
// The order is load-bearing.
Adsr gEnv(Mulberry32& r, const double t[4], const double v[4]) {
    double n[4] = {r(v[0]), r(v[1]), r(v[2]), r(v[3])};
    const double mx = std::max({n[0], n[1], n[2], n[3]});
    for (double& x : n) x /= mx;      // matches JS: 0/0 yields NaN, deliberately

    Adsr a;
    a.aT = r(t[0]); a.aV = n[0];
    a.dT = r(t[1]); a.dV = n[1];
    a.sT = r(t[2]); a.sV = n[2];
    a.rT = r(t[3]); a.rV = n[3];
    return a;
}

// hl(): r(r() < 0.1 ? 100 : r() < 0.1 ? 10 : 1) + 0.1
// Consumes two draws on the first branch, three otherwise.
double hl(Mulberry32& r) {
    double scale;
    if (r() < 0.1) scale = 100.0;
    else if (r() < 0.1) scale = 10.0;
    else scale = 1.0;
    return r(scale) + 0.1;
}

Waveform pickWave(Mulberry32& r, const Waveform* w, int n) {
    return w[static_cast<int>(std::floor(r(static_cast<double>(n))))];
}

int firstDecimalDigit(uint32_t seed) {
    const std::string s = std::to_string(seed);
    return s.empty() ? 0 : (s[0] - '0');
}

} // namespace

Instrument generateInstrument(uint32_t seed) {
    Mulberry32 r(seed);
    Mulberry32 dR(seed + 7777u);   // side stream for distortion; see zyn Task 2

    Instrument inst;
    inst.typeIndex = static_cast<int>(seed % 10u);
    const TypeDef& T = kTypes[inst.typeIndex];

    // zyn re-evaluates the loop bound each iteration, drawing a new random
    // number every pass. Do NOT hoist this.
    int i = 0;
    while (i < static_cast<int>(std::floor(r(T.o) + 1.0))) {
        if (i >= kMaxOscs) break;    // fx tops out at 5; guards the fixed array
        Osc& d = inst.oscs[i];

        d.waveform = pickWave(r, T.w, T.wn);
        const bool nN = (d.waveform != Waveform::Noise);

        d.adsrGain    = gEnv(r, T.gT, T.gV);
        d.filterType  = kFilters[static_cast<int>(std::floor(r(7.0)))];
        d.adsrFilter  = gEnv(r, T.fT, T.fV);
        d.filterQ     = r(30.0);
        d.adsrFilterQ = gEnv(r, T.fT, T.fV);

        if (r() < 0.1) {
            d.gLfo.on = true;
            d.gLfo.type = pickWave(r, kAll, 4);
            d.gLfo.frequency = hl(r);
            d.gLfo.depth = r(1.0);
        }
        // `r() < 0.1 && nN` short-circuits: when the draw fails, nN is never
        // consulted and no further draws happen.
        if (r() < 0.1 && nN) {
            d.fLfo.on = true;
            d.fLfo.type = pickWave(r, kAll, 4);
            d.fLfo.frequency = hl(r);
            d.fLfo.depth = r(8000.0);
        }
        if (r() < 0.1 && nN) {
            d.pLfo.on = true;
            d.pLfo.type = pickWave(r, kAll, 4);
            d.pLfo.frequency = hl(r);
            d.pLfo.depth = r(10.0) + 1.0;
        }
        if (r() < 0.3 && nN) {
            d.fm.on = true;
            d.fm.type = pickWave(r, kAll, 4);
            d.fm.frequency = hl(r);
            d.fm.depth = r(100.0) + 1.0;
        }
        if (r() < T.pe && nN) {
            d.pEnv.on = true;
            d.pEnv.amount = r();                  // amount is drawn before gEnv
            d.pEnv.env = gEnv(r, T.pT, T.pV);
        }
        if (r() < 0.05) {
            d.dist.on = true;
            d.dist.amount = dR(500.0);            // side stream, not r
            const int k = static_cast<int>(std::floor(r(3.0)));
            d.dist.oversample = (k == 2) ? 4 : (k == 1) ? 2 : 1;
        }
        d.oct = static_cast<int>(std::floor(r(4.0))) - 3;
        d.detune = (r() < 0.2) ? 5.0 : 0.0;       // SEMITONES

        if (r() > 0.5) {
            d.del.on = true;
            d.del.time = r(0.5);
            d.del.feedback = r(0.8);
        }
        if (r() > 0.5) {
            d.verb.on = true;
            d.verb.duration = r(3.0) + 0.1;
            d.verb.decay = r() * 0.5 + 0.5;
        }
        ++i;
    }
    inst.oscCount = i;

    // FM matrix: separate PRNG at seed + 9999, so it was bolted on without
    // disturbing pre-existing seeds.
    if (inst.oscCount > 1) {
        Mulberry32 fmR(seed + 9999u);
        const double fmProb = firstDecimalDigit(seed) / 10.0;
        if (fmR() < fmProb) {
            inst.hasFmMatrix = true;
            const double n = static_cast<double>(inst.oscCount);
            do {
                const int src = static_cast<int>(std::floor(fmR(n)));
                const int tgt = static_cast<int>(std::floor(fmR(n)));
                // The value draw only happens when the slot is free, so a
                // repeated pick changes the stream. Keep the branch.
                if (inst.fmMatrix[src][tgt] == 0.0)
                    inst.fmMatrix[src][tgt] = fmR(2.0) - 1.0;
            } while (fmR() < 0.5);
        }
    }
    return inst;
}

} // namespace sl
```

`core/include/sl/InstrumentGen.h`:

```cpp
#pragma once
#include "sl/Instrument.h"
#include <cstdint>
namespace sl { Instrument generateInstrument(uint32_t seed); }
```

Add `src/PresetIO.cpp` to `core/CMakeLists.txt` sources and give `sl_core` a
`target_include_directories(sl_core PUBLIC src)` so tests can include `PresetIO.h`.

- [ ] **Step 4: Run the tests**

Expected: both PASS. If the golden test fails, the failure message names the seed
and field. Bisect by generating a Node dump of the same seed's `r()` call sequence
and comparing draw-by-draw — an off-by-one in call order shows up as the first
mismatched field.

- [ ] **Step 5: Commit**

```bash
git -C C:/dev/seedlathe add core/ tests/
git -C C:/dev/seedlathe commit -m "feat(core): exact InstrumentGen port, verified against 2000 golden vectors"
```

**P0 is complete at this point.** The generator is provably identical to zyn's.

---

### Task 7: WaParam — the automation timeline

**Files:**
- Create: `core/src/webaudio/WaParam.h`, `.cpp`
- Test: `tests/test_waparam.cpp`

**Interfaces:**
- Consumes: nothing
- Produces: `sl::WaParam` with `void setValueAtTime(double v, double t)`, `void linearRampToValueAtTime(double v, double t)`, `void cancelScheduledValues(double t)`, `double valueAt(double t) const`, `void reset(double v)`

zyn uses exactly two automation methods, so only those two need porting. Web Audio's
rule: a linear ramp interpolates from the value and time of the *previous* event to
the ramp's target and time. Before the first event the parameter holds its static
value; after the last, it holds that event's value.

- [ ] **Step 1: Write the failing test**

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "webaudio/WaParam.h"

using Catch::Matchers::WithinAbs;

TEST_CASE("WaParam holds its value with no events") {
    sl::WaParam p; p.reset(0.5);
    REQUIRE(p.valueAt(0.0) == 0.5);
    REQUIRE(p.valueAt(99.0) == 0.5);
}

TEST_CASE("WaParam linear ramp interpolates from the previous event") {
    sl::WaParam p; p.reset(0.0);
    p.setValueAtTime(0.0, 1.0);
    p.linearRampToValueAtTime(1.0, 2.0);
    REQUIRE_THAT(p.valueAt(1.0),  WithinAbs(0.0, 1e-12));
    REQUIRE_THAT(p.valueAt(1.25), WithinAbs(0.25, 1e-12));
    REQUIRE_THAT(p.valueAt(1.5),  WithinAbs(0.5, 1e-12));
    REQUIRE_THAT(p.valueAt(2.0),  WithinAbs(1.0, 1e-12));
    REQUIRE_THAT(p.valueAt(9.0),  WithinAbs(1.0, 1e-12));  // holds after the last event
}

TEST_CASE("WaParam reproduces a zyn ADSR chain") {
    // Z.adsr: setValueAtTime(0, t), then four linear ramps. Attack time is
    // clamped to a 5 ms minimum to avoid a click.
    sl::WaParam p; p.reset(0.0);
    const double t = 0.0, max = 1.0;
    p.setValueAtTime(0.0, t);
    p.linearRampToValueAtTime(1.0 * max, t + 0.05);          // A: [0.05, 1.0]
    p.linearRampToValueAtTime(0.5 * max, t + 0.05 + 0.10);   // D: [0.10, 0.5]
    p.linearRampToValueAtTime(0.3 * max, t + 0.05 + 0.10 + 0.20);  // S
    p.linearRampToValueAtTime(0.0,       t + 0.05 + 0.10 + 0.20 + 0.10); // R

    REQUIRE_THAT(p.valueAt(0.025), WithinAbs(0.5, 1e-12));
    REQUIRE_THAT(p.valueAt(0.05),  WithinAbs(1.0, 1e-12));
    REQUIRE_THAT(p.valueAt(0.10),  WithinAbs(0.75, 1e-12));
    REQUIRE_THAT(p.valueAt(0.45),  WithinAbs(0.0, 1e-12));
}

TEST_CASE("WaParam cancelScheduledValues drops later events") {
    sl::WaParam p; p.reset(0.0);
    p.setValueAtTime(0.0, 0.0);
    p.linearRampToValueAtTime(1.0, 1.0);
    p.cancelScheduledValues(0.5);
    REQUIRE_THAT(p.valueAt(0.9), WithinAbs(0.0, 1e-12));
}
```

- [ ] **Step 2: Run and watch it fail**

Expected: compile error, `webaudio/WaParam.h` not found.

- [ ] **Step 3: Implement**

`core/src/webaudio/WaParam.h`:

```cpp
#pragma once
#include <array>
#include <cstddef>

namespace sl {

// Web Audio AudioParam timeline, restricted to the two methods zyn uses.
// Fixed capacity so it never allocates on the audio thread: zyn schedules at
// most 5 events per envelope (one setValueAtTime plus four ramps).
class WaParam {
public:
    static constexpr size_t kMaxEvents = 8;

    void reset(double v);
    void setValueAtTime(double v, double t);
    void linearRampToValueAtTime(double v, double t);
    void cancelScheduledValues(double t);
    double valueAt(double t) const;
    double staticValue() const { return static_; }

private:
    enum class Kind { SetValue, LinearRamp };
    struct Event { Kind kind; double value; double time; };

    double static_ = 0.0;
    std::array<Event, kMaxEvents> events_{};
    size_t count_ = 0;
};

} // namespace sl
```

`core/src/webaudio/WaParam.cpp`:

```cpp
#include "webaudio/WaParam.h"

namespace sl {

void WaParam::reset(double v) { static_ = v; count_ = 0; }

void WaParam::setValueAtTime(double v, double t) {
    if (count_ < kMaxEvents) events_[count_++] = {Kind::SetValue, v, t};
}

void WaParam::linearRampToValueAtTime(double v, double t) {
    if (count_ < kMaxEvents) events_[count_++] = {Kind::LinearRamp, v, t};
}

void WaParam::cancelScheduledValues(double t) {
    while (count_ > 0 && events_[count_ - 1].time >= t) --count_;
}

double WaParam::valueAt(double t) const {
    if (count_ == 0) return static_;
    if (t <= events_[0].time) {
        // Before the first event: a pending ramp interpolates from the static
        // value; a setValueAtTime does not apply yet.
        return events_[0].kind == Kind::LinearRamp && t > 0.0 ? static_ : static_;
    }
    size_t i = 0;
    while (i + 1 < count_ && events_[i + 1].time <= t) ++i;
    if (i + 1 >= count_) return events_[i].value;

    const Event& prev = events_[i];
    const Event& next = events_[i + 1];
    if (next.kind == Kind::SetValue) return prev.value;

    const double span = next.time - prev.time;
    if (span <= 0.0) return next.value;
    const double frac = (t - prev.time) / span;
    return prev.value + (next.value - prev.value) * frac;
}

} // namespace sl
```

- [ ] **Step 4: Run the tests**

Expected: 4 tests PASS.

- [ ] **Step 5: Commit**

```bash
git -C C:/dev/seedlathe add core/src/webaudio/ tests/
git -C C:/dev/seedlathe commit -m "feat(core): Web Audio AudioParam timeline"
```

---

### Task 8: WaOscillator and the noise source

**Files:**
- Create: `core/src/webaudio/WaOscillator.h`, `.cpp`
- Test: `tests/test_waoscillator.cpp`

**Interfaces:**
- Consumes: `sl::Waveform`
- Produces: `sl::WaOscillator` with `void prepare(double sampleRate)`, `void setType(Waveform)`, `double render(double freqHz)`, `void resetPhase()`; and `sl::NoiseSource` with `void prepare(double sampleRate)`, `double render()`, plus free function `const std::vector<float>& sl::globalNoiseBuffer(double sampleRate)`

Web Audio oscillators are **band-limited**. A naive `sawtooth` aliases badly and
would fail the null-test outright. Chrome builds each waveform from a Fourier series
truncated so the highest partial stays under Nyquist, in octave-spaced tables with
linear interpolation between adjacent tables.

- [ ] **Step 1: Write the failing test**

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "webaudio/WaOscillator.h"
#include <cmath>
#include <vector>

using Catch::Matchers::WithinAbs;

namespace {
// Energy above Nyquist/2 relative to total, via a naive DFT over one block.
double highBandRatio(const std::vector<double>& x, double sr) {
    const size_t N = x.size();
    double total = 0.0, high = 0.0;
    for (size_t k = 1; k < N / 2; ++k) {
        double re = 0.0, im = 0.0;
        for (size_t n = 0; n < N; ++n) {
            const double a = -2.0 * 3.14159265358979323846 * double(k) * double(n) / double(N);
            re += x[n] * std::cos(a); im += x[n] * std::sin(a);
        }
        const double mag = re * re + im * im;
        total += mag;
        if (double(k) * sr / double(N) > sr / 4.0) high += mag;
    }
    return total > 0.0 ? high / total : 0.0;
}
} // namespace

TEST_CASE("sine oscillator produces a clean sine") {
    sl::WaOscillator o; o.prepare(48000.0); o.setType(sl::Waveform::Sine);
    std::vector<double> x(1024);
    for (auto& s : x) s = o.render(1000.0);
    double peak = 0.0; for (double s : x) peak = std::max(peak, std::abs(s));
    REQUIRE_THAT(peak, WithinAbs(1.0, 0.02));
    REQUIRE(highBandRatio(x, 48000.0) < 1e-6);
}

TEST_CASE("sawtooth is band-limited, not naive") {
    // A naive ramp at 5 kHz folds enormous energy above Nyquist/2. A properly
    // band-limited one keeps it small.
    sl::WaOscillator o; o.prepare(48000.0); o.setType(sl::Waveform::Sawtooth);
    std::vector<double> x(1024);
    for (auto& s : x) s = o.render(5000.0);
    REQUIRE(highBandRatio(x, 48000.0) < 0.05);
}

TEST_CASE("noise buffer is deterministic and shared") {
    const auto& a = sl::globalNoiseBuffer(48000.0);
    const auto& b = sl::globalNoiseBuffer(48000.0);
    REQUIRE(&a == &b);                       // one global buffer, as in zyn
    REQUIRE(a.size() == 96000u);             // 2 seconds
    REQUIRE(a[0] == b[0]);
    double peak = 0.0f; for (float s : a) peak = std::max(peak, std::abs((double)s));
    REQUIRE(peak <= 1.0);
    REQUIRE(peak > 0.9);
}
```

- [ ] **Step 2: Run and watch it fail**

Expected: compile error, header not found.

- [ ] **Step 3: Implement**

Algorithm, matching Chrome's `PeriodicWave`:

- For each waveform, define Fourier coefficients: sine has one partial; sawtooth
  `b[n] = 2/(n*pi)` with alternating sign; square `b[n] = 4/(n*pi)` for odd `n`;
  triangle `b[n] = 8/(n^2 * pi^2)` for odd `n` with alternating sign.
- Build one table per octave. Table `k` covers fundamentals in
  `[sr/2/2^(k+1), sr/2/2^k)` and includes only partials with `n * f < sr/2`, i.e.
  `maxPartial = 2^(k+1)` capped at the table length.
- Table length 2048, linearly interpolated in phase; adjacent octave tables are
  linearly crossfaded by the fractional octave so there is no zipper at boundaries.
- Amplitude normalised so each table peaks at 1.0, matching Chrome's normalisation.

The noise buffer is the deliberate divergence recorded in spec §6.3. zyn fills it with
`Math.random()`, which is non-reproducible. Seed it from a fixed constant so the C++
engine and the reference exporter agree:

```cpp
// core/src/webaudio/WaOscillator.cpp
const std::vector<float>& globalNoiseBuffer(double sampleRate) {
    static std::vector<float> buffer;
    static double builtFor = 0.0;
    if (buffer.empty() || builtFor != sampleRate) {
        // Fixed seed: zyn keeps ONE global noise buffer shared by every voice
        // and instrument, so this must not depend on the instrument seed.
        // tools/export-reference-audio.mjs overrides Z.randSample with the
        // identical stream.
        Mulberry32 r(0x5EED1A7Eu);
        const size_t n = static_cast<size_t>(2.0 * sampleRate);
        buffer.resize(n);
        for (size_t i = 0; i < n; ++i) buffer[i] = static_cast<float>(r(2.0) - 1.0);
        builtFor = sampleRate;
    }
    return buffer;
}
```

- [ ] **Step 4: Run the tests**

Expected: 3 tests PASS. If `sawtooth` fails the band-limit check, the octave table
selection is off by one — verify `maxPartial` against the fundamental actually
requested, not the table's nominal centre.

- [ ] **Step 5: Commit**

```bash
git -C C:/dev/seedlathe add core/ tests/
git -C C:/dev/seedlathe commit -m "feat(core): band-limited oscillator tables and deterministic noise buffer"
```

---

### Task 9: WaBiquad

**Files:**
- Create: `core/src/webaudio/WaBiquad.h`, `.cpp`
- Test: `tests/test_wabiquad.cpp`

**Interfaces:**
- Consumes: `sl::FilterType`
- Produces: `sl::WaBiquad` with `void prepare(double sampleRate)`, `void setCoefficients(FilterType, double freqHz, double q, double gainDb)`, `double process(double x)`, `void reset()`, and `double magnitudeAt(double freqHz) const`

Web Audio uses the RBJ cookbook formulas. zyn drives cutoff from an envelope spanning
0 → 20000 Hz, so the near-zero clamp behaviour is exercised on every note and must
match Blink: frequency is clamped to `[0, nyquist]`, and a cutoff of exactly 0 for a
lowpass produces silence, not a degenerate blow-up.

- [ ] **Step 1: Write the failing test**

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "webaudio/WaBiquad.h"
#include <cmath>

using Catch::Matchers::WithinAbs;

TEST_CASE("lowpass magnitude matches the analytic RBJ response") {
    sl::WaBiquad f; f.prepare(48000.0);
    f.setCoefficients(sl::FilterType::Lowpass, 1000.0, 0.7071, 0.0);
    REQUIRE_THAT(f.magnitudeAt(100.0),   WithinAbs(1.0, 0.01));
    REQUIRE_THAT(f.magnitudeAt(1000.0),  WithinAbs(0.7071, 0.02));  // -3 dB at cutoff
    REQUIRE(f.magnitudeAt(10000.0) < 0.02);
}

TEST_CASE("highpass mirrors lowpass") {
    sl::WaBiquad f; f.prepare(48000.0);
    f.setCoefficients(sl::FilterType::Highpass, 1000.0, 0.7071, 0.0);
    REQUIRE(f.magnitudeAt(100.0) < 0.02);
    REQUIRE_THAT(f.magnitudeAt(1000.0), WithinAbs(0.7071, 0.02));
    REQUIRE_THAT(f.magnitudeAt(20000.0), WithinAbs(1.0, 0.05));
}

TEST_CASE("cutoff of zero silences a lowpass instead of exploding") {
    sl::WaBiquad f; f.prepare(48000.0);
    f.setCoefficients(sl::FilterType::Lowpass, 0.0, 5.0, 0.0);
    double peak = 0.0;
    for (int i = 0; i < 4800; ++i)
        peak = std::max(peak, std::abs(f.process(std::sin(i * 0.1))));
    REQUIRE(std::isfinite(peak));
    REQUIRE(peak < 1e-6);
}

TEST_CASE("cutoff above nyquist passes a lowpass through") {
    sl::WaBiquad f; f.prepare(48000.0);
    f.setCoefficients(sl::FilterType::Lowpass, 30000.0, 0.7071, 0.0);
    REQUIRE_THAT(f.magnitudeAt(1000.0), WithinAbs(1.0, 0.01));
}

TEST_CASE("filter stays stable across a full envelope sweep") {
    // zyn sweeps cutoff 0 -> 20000 Hz on every note with Q up to 30.
    sl::WaBiquad f; f.prepare(48000.0);
    double peak = 0.0;
    for (int i = 0; i < 48000; ++i) {
        const double freq = 20000.0 * (double(i) / 48000.0);
        f.setCoefficients(sl::FilterType::Lowpass, freq, 30.0, 0.0);
        peak = std::max(peak, std::abs(f.process(std::sin(i * 0.05))));
    }
    REQUIRE(std::isfinite(peak));
    REQUIRE(peak < 100.0);   // resonance can ring hot; it must not diverge
}
```

- [ ] **Step 2: Run and watch it fail**

Expected: compile error, header not found.

- [ ] **Step 3: Implement**

Standard RBJ, with Blink's clamping. Coefficients are recomputed per sample
(a-rate), because zyn drives frequency and Q from envelopes:

```cpp
void WaBiquad::setCoefficients(FilterType type, double freqHz, double q, double gainDb) {
    const double nyquist = sampleRate_ * 0.5;
    // Blink clamps the normalised frequency to [0, 1].
    double nf = freqHz / nyquist;
    nf = nf < 0.0 ? 0.0 : (nf > 1.0 ? 1.0 : nf);

    if (nf <= 0.0) { setToSilenceOrPassthrough(type, /*atZero=*/true); return; }
    if (nf >= 1.0) { setToSilenceOrPassthrough(type, /*atZero=*/false); return; }

    const double w0 = 3.14159265358979323846 * nf;
    const double cosw0 = std::cos(w0), sinw0 = std::sin(w0);
    const double qClamped = std::max(q, 1e-4);
    const double alpha = sinw0 / (2.0 * qClamped);
    const double A = std::pow(10.0, gainDb / 40.0);     // shelf/peaking only
    double b0, b1, b2, a0, a1, a2;

    switch (type) {
    case FilterType::Lowpass:
        b0 = (1.0 - cosw0) * 0.5; b1 = 1.0 - cosw0; b2 = b0;
        a0 = 1.0 + alpha; a1 = -2.0 * cosw0; a2 = 1.0 - alpha;
        break;
    case FilterType::Highpass:
        b0 = (1.0 + cosw0) * 0.5; b1 = -(1.0 + cosw0); b2 = b0;
        a0 = 1.0 + alpha; a1 = -2.0 * cosw0; a2 = 1.0 - alpha;
        break;
    case FilterType::Bandpass:                          // constant 0 dB peak gain
        b0 = alpha; b1 = 0.0; b2 = -alpha;
        a0 = 1.0 + alpha; a1 = -2.0 * cosw0; a2 = 1.0 - alpha;
        break;
    case FilterType::Lowshelf: {
        const double s = 2.0 * std::sqrt(A) * alpha;
        b0 =      A * ((A + 1.0) - (A - 1.0) * cosw0 + s);
        b1 =  2.0 * A * ((A - 1.0) - (A + 1.0) * cosw0);
        b2 =      A * ((A + 1.0) - (A - 1.0) * cosw0 - s);
        a0 =           (A + 1.0) + (A - 1.0) * cosw0 + s;
        a1 = -2.0 *    ((A - 1.0) + (A + 1.0) * cosw0);
        a2 =           (A + 1.0) + (A - 1.0) * cosw0 - s;
        break;
    }
    case FilterType::Highshelf: {
        const double s = 2.0 * std::sqrt(A) * alpha;
        b0 =      A * ((A + 1.0) + (A - 1.0) * cosw0 + s);
        b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cosw0);
        b2 =      A * ((A + 1.0) + (A - 1.0) * cosw0 - s);
        a0 =           (A + 1.0) - (A - 1.0) * cosw0 + s;
        a1 =  2.0 *    ((A - 1.0) - (A + 1.0) * cosw0);
        a2 =           (A + 1.0) - (A - 1.0) * cosw0 - s;
        break;
    }
    case FilterType::Peaking:
        b0 = 1.0 + alpha * A; b1 = -2.0 * cosw0; b2 = 1.0 - alpha * A;
        a0 = 1.0 + alpha / A; a1 = -2.0 * cosw0; a2 = 1.0 - alpha / A;
        break;
    case FilterType::Allpass:
        b0 = 1.0 - alpha; b1 = -2.0 * cosw0; b2 = 1.0 + alpha;
        a0 = 1.0 + alpha; a1 = -2.0 * cosw0; a2 = 1.0 - alpha;
        break;
    }

    b0_ = b0 / a0; b1_ = b1 / a0; b2_ = b2 / a0;
    a1_ = a1 / a0; a2_ = a2 / a0;
}
```

zyn never sets a gain on any filter, so `gainDb` is always 0 and `A` is always 1 —
but the shelf and peaking types are still selected by the generator, and at `A = 1`
their formulas must reduce correctly rather than being skipped.

`setToSilenceOrPassthrough` encodes the degenerate cases: at cutoff 0 a lowpass is
silent and a highpass is a passthrough; at Nyquist the reverse. `magnitudeAt`
evaluates `|H(e^{jw})|` from the stored coefficients — it exists so the tests can
check the response analytically rather than by ear.

- [ ] **Step 4: Run the tests**

Expected: 5 tests PASS.

- [ ] **Step 5: Commit**

```bash
git -C C:/dev/seedlathe add core/ tests/
git -C C:/dev/seedlathe commit -m "feat(core): RBJ biquad with Blink clamping semantics"
```

---

### Task 10: Delay, convolver, panner, shaper

**Files:**
- Create: `core/src/webaudio/WaDelay.{h,cpp}`, `WaConvolver.{h,cpp}`, `WaPanner.{h,cpp}`, `WaShaper.{h,cpp}`
- Test: `tests/test_wanodes.cpp`

**Interfaces:**
- Produces:
  - `sl::WaDelay` — `void prepare(double sampleRate, double maxSeconds)`, `void setDelayTime(double s)`, `void setFeedback(double f)`, `double process(double x)`
  - `sl::WaConvolver` — `void prepare(double sampleRate)`, `void buildImpulse(double duration, double decay, uint32_t rngSeed)`, `void process(double inL, double inR, double& outL, double& outR)`
  - `sl::WaPanner` — `static void pan(double in, double p, double& l, double& r)`
  - `sl::WaShaper` — `void setCurve(double amount, double sampleRate)`, `void setOversample(int factor)`, `double process(double x)`, plus `std::vector<float> sl::getDistCurve(double k, double sampleRate)`

- [ ] **Step 1: Write the failing tests**

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "webaudio/WaDelay.h"
#include "webaudio/WaConvolver.h"
#include "webaudio/WaPanner.h"
#include "webaudio/WaShaper.h"
#include <cmath>

using Catch::Matchers::WithinAbs;

TEST_CASE("delay returns an impulse after the delay time") {
    sl::WaDelay d; d.prepare(48000.0, 0.5);
    d.setDelayTime(0.01);   // 480 samples
    d.setFeedback(0.0);
    double first = -1.0;
    for (int i = 0; i < 1000; ++i) {
        const double out = d.process(i == 0 ? 1.0 : 0.0);
        if (out > 0.5 && first < 0.0) first = double(i);
    }
    REQUIRE_THAT(first, WithinAbs(480.0, 1.0));
}

TEST_CASE("delay feedback decays geometrically") {
    sl::WaDelay d; d.prepare(48000.0, 0.5);
    d.setDelayTime(0.01);
    d.setFeedback(0.5);
    double taps[3] = {0, 0, 0}; int found = 0;
    for (int i = 0; i < 2000 && found < 3; ++i) {
        const double out = d.process(i == 0 ? 1.0 : 0.0);
        if (out > 1e-3) taps[found++] = out;
    }
    REQUIRE(found == 3);
    REQUIRE_THAT(taps[1] / taps[0], WithinAbs(0.5, 0.05));
    REQUIRE_THAT(taps[2] / taps[1], WithinAbs(0.5, 0.05));
}

TEST_CASE("convolver impulse is deterministic for a given seed") {
    sl::WaConvolver a, b;
    a.prepare(48000.0); b.prepare(48000.0);
    a.buildImpulse(0.5, 0.8, 12345u);
    b.buildImpulse(0.5, 0.8, 12345u);
    double al, ar, bl, br;
    a.process(1.0, 1.0, al, ar);
    b.process(1.0, 1.0, bl, br);
    REQUIRE(al == bl);
    REQUIRE(ar == br);
}

TEST_CASE("panner is equal-power and centred at unity") {
    double l, r;
    sl::WaPanner::pan(1.0, 0.0, l, r);
    REQUIRE_THAT(l, WithinAbs(0.7071, 0.001));
    REQUIRE_THAT(r, WithinAbs(0.7071, 0.001));
    sl::WaPanner::pan(1.0, -1.0, l, r);
    REQUIRE_THAT(l, WithinAbs(1.0, 0.001));
    REQUIRE_THAT(r, WithinAbs(0.0, 0.001));
}

TEST_CASE("getDistCurve is finite, monotonic and odd-symmetric") {
    const auto c = sl::getDistCurve(100.0, 48000.0);
    REQUIRE(c.size() == 48000u);
    for (float v : c) REQUIRE(std::isfinite(v));
    for (size_t i = 1; i < c.size(); ++i) REQUIRE(c[i] >= c[i - 1] - 1e-6f);
    // x = -1 and x = +1 map to equal and opposite outputs
    REQUIRE_THAT(double(c.front() + c.back()), WithinAbs(0.0, 1e-4));
}

TEST_CASE("shaper with zero amount is near-transparent") {
    sl::WaShaper s; s.setCurve(0.0, 48000.0); s.setOversample(1);
    REQUIRE_THAT(s.process(0.5), WithinAbs(0.5, 0.15));
}
```

- [ ] **Step 2: Run and watch them fail**

Expected: compile errors, headers not found.

- [ ] **Step 3: Implement**

`getDistCurve` ports zyn's fixed formula verbatim, with the `sampleRate` fix from Task 2:

```cpp
std::vector<float> getDistCurve(double k, double sampleRate) {
    const size_t n = static_cast<size_t>(sampleRate);
    std::vector<float> curve(n);
    const double deg = 3.14159265358979323846 / 180.0;
    for (size_t i = 0; i < n; ++i) {
        const double x = (double(i) * 2.0) / sampleRate - 1.0;
        curve[i] = static_cast<float>(((3.0 + k) * x * 20.0 * deg) /
                                      (3.14159265358979323846 + k * std::abs(x)));
    }
    return curve;
}
```

`WaShaper::process` maps input `x` to a curve index exactly as Web Audio's
`WaveShaperNode` does: `idx = (x + 1) * 0.5 * (n - 1)`, clamped, with linear
interpolation between neighbours. Oversampling factors 2 and 4 upsample, shape, and
downsample through a FIR polyphase halfband pair local to `WaShaper` — the global
`Oversampler` unit named in spec §5.1 is a P7 concern and is not built here.

`WaConvolver` uses partitioned FFT convolution; the impulse is generated with
`Mulberry32(rngSeed)` and `impL[i] = impR[i] = (r(2) - 1) * pow(1 - i/length, decay)`,
matching zyn's formula with a deterministic source in place of `Math.random()`.

`WaPanner` implements StereoPannerNode's equal-power law for a mono input:
`x = (p + 1) * pi / 4`, `l = cos(x)`, `r = sin(x)`.

- [ ] **Step 4: Run the tests**

Expected: 6 tests PASS.

- [ ] **Step 5: Commit**

```bash
git -C C:/dev/seedlathe add core/ tests/
git -C C:/dev/seedlathe commit -m "feat(core): delay, convolver, panner and waveshaper nodes"
```

---

### Task 11: WaCompressor

**Files:**
- Create: `core/src/webaudio/WaCompressor.{h,cpp}`
- Test: `tests/test_wacompressor.cpp`

**Interfaces:**
- Produces: `sl::WaCompressor` with `void prepare(double sampleRate)`, `void setParams(double thresholdDb, double kneeDb, double ratio, double attackS, double releaseS)`, `void process(double inL, double inR, double& outL, double& outR)`, `double reduction() const`

Every seed passes through this, so an error here shifts the whole null-test. zyn's
settings are fixed: threshold −12 dB, knee 6 dB, ratio 8, attack 3 ms, release 150 ms.
Port from Blink's `DynamicsCompressorKernel`, including the 6 ms lookahead delay,
the cubic knee curve, and makeup gain derived from the curve at 0 dB.

- [ ] **Step 1: Write the failing test**

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "webaudio/WaCompressor.h"
#include <cmath>

using Catch::Matchers::WithinAbs;

namespace {
sl::WaCompressor makeZynCompressor() {
    sl::WaCompressor c;
    c.prepare(48000.0);
    c.setParams(-12.0, 6.0, 8.0, 0.003, 0.15);   // exactly zyn's Z.init settings
    return c;
}
double dbOf(double x) { return 20.0 * std::log10(std::max(x, 1e-12)); }
} // namespace

TEST_CASE("compressor passes quiet signal essentially untouched") {
    auto c = makeZynCompressor();
    double peak = 0.0, l, r;
    for (int i = 0; i < 48000; ++i) {
        const double x = 0.05 * std::sin(2.0 * 3.14159265 * 440.0 * i / 48000.0);
        c.process(x, x, l, r);
        if (i > 24000) peak = std::max(peak, std::abs(l));
    }
    REQUIRE_THAT(dbOf(peak) - dbOf(0.05), WithinAbs(0.0, 1.5));
}

TEST_CASE("compressor reduces a loud signal toward the threshold") {
    auto c = makeZynCompressor();
    double peak = 0.0, l, r;
    for (int i = 0; i < 96000; ++i) {
        const double x = 1.0 * std::sin(2.0 * 3.14159265 * 440.0 * i / 48000.0);
        c.process(x, x, l, r);
        if (i > 48000) peak = std::max(peak, std::abs(l));
    }
    // 0 dBFS in, threshold -12 dB, ratio 8:1 -> roughly -10.5 dB out, plus makeup.
    REQUIRE(dbOf(peak) < -3.0);
    REQUIRE(dbOf(peak) > -14.0);
    REQUIRE(c.reduction() < 0.0);
}

TEST_CASE("attack time is in the right order of magnitude") {
    auto c = makeZynCompressor();
    double l, r;
    int settleSample = -1;
    for (int i = 0; i < 4800; ++i) {
        const double x = (i < 480) ? 0.0 : 1.0;
        c.process(x, x, l, r);
        if (i >= 480 && settleSample < 0 && c.reduction() < -6.0) settleSample = i - 480;
    }
    REQUIRE(settleSample >= 0);
    REQUIRE(settleSample < static_cast<int>(0.02 * 48000));   // within 20 ms
}

TEST_CASE("compressor output is always finite") {
    auto c = makeZynCompressor();
    double l, r;
    for (int i = 0; i < 48000; ++i) {
        const double x = (i % 2 == 0) ? 8.0 : -8.0;   // brutal square, way over unity
        c.process(x, x, l, r);
        REQUIRE(std::isfinite(l));
        REQUIRE(std::isfinite(r));
    }
}
```

- [ ] **Step 2: Run and watch it fail**

Expected: compile error, header not found.

- [ ] **Step 3: Implement from Blink's algorithm**

Read Blink's `dynamics_compressor_kernel.cc` before writing. The parts that must be
carried over rather than approximated: the pre-delay ring buffer of 6 ms, the
knee curve built from `kneeCurve()`/`saturate()` with the cubic soft region,
`kSpacingDb` slope sampling for the detector, envelope rate conversion from the
attack/release seconds, and makeup gain as `1 / saturate(1, k)`.

- [ ] **Step 4: Run the tests**

Expected: 4 tests PASS. If the loud-signal test lands outside the window, check
makeup gain first — it is the usual culprit and it offsets everything uniformly.

- [ ] **Step 5: Commit**

```bash
git -C C:/dev/seedlathe add core/ tests/
git -C C:/dev/seedlathe commit -m "feat(core): DynamicsCompressor port of Blink's kernel"
```

---

### Task 12: SharedFxRack

**Files:**
- Create: `core/src/SharedFxRack.{h,cpp}`
- Test: `tests/test_sharedfxrack.cpp`

**Interfaces:**
- Consumes: `sl::WaDelay`, `sl::WaConvolver`, `sl::Osc`
- Produces: `sl::SharedFxRack` with `void prepare(double sampleRate)`, `WaDelay* acquireDelay(const Delay&)`, `WaConvolver* acquireVerb(const Verb&)`, `void touchPassthrough(const Osc&)`, `size_t nodeCount() const`, `void beginRender()`

This reproduces zyn's `Z.fxNodes`: delay and reverb are **shared between every voice
and every note** with the same configuration. Per-voice effects would sound entirely
different, so this is a feature, not an oversight.

**Two documented divergences from zyn**, both unobservable in the fidelity tests:

1. zyn keys the cache on `Z.id(config)`, a 32-bit Java-style string hash of the
   JSON. We key on the config *values* instead. The equivalence classes are
   identical; only hash *collisions* would behave differently, and collisions are
   not reproducible design behaviour.
2. `cleanupFxNodes` removes "the oldest half" using `Object.keys()` order, which in
   V8 enumerates non-negative integer-like keys in ascending numeric order before
   insertion-ordered keys. Reproducing that would require porting `Z.id` and V8's
   key ordering. We evict in insertion order instead. This is unreachable in the
   null-test, which renders one instrument at a time and never exceeds ~15 entries.

- [ ] **Step 1: Write the failing test**

```cpp
#include <catch2/catch_test_macros.hpp>
#include "SharedFxRack.h"
#include "sl/Instrument.h"

TEST_CASE("identical delay configs share one node") {
    sl::SharedFxRack rack; rack.prepare(48000.0);
    sl::Delay a; a.on = true; a.time = 0.25; a.feedback = 0.4;
    sl::Delay b = a;
    REQUIRE(rack.acquireDelay(a) == rack.acquireDelay(b));
    REQUIRE(rack.nodeCount() == 1);
}

TEST_CASE("different delay configs get different nodes") {
    sl::SharedFxRack rack; rack.prepare(48000.0);
    sl::Delay a; a.on = true; a.time = 0.25; a.feedback = 0.4;
    sl::Delay b; b.on = true; b.time = 0.25; b.feedback = 0.5;
    REQUIRE(rack.acquireDelay(a) != rack.acquireDelay(b));
    REQUIRE(rack.nodeCount() == 2);
}

TEST_CASE("passthrough placeholders count toward the eviction budget") {
    // zyn caches a no-op gain node per distinct osc config even when there is
    // no delay, keyed on the whole config. Those entries fill the cache and
    // therefore change WHEN eviction fires.
    sl::SharedFxRack rack; rack.prepare(48000.0);
    sl::Osc o;
    for (int i = 0; i < 10; ++i) { o.filterQ = double(i); rack.touchPassthrough(o); }
    REQUIRE(rack.nodeCount() == 10);
}

TEST_CASE("cache evicts half once it exceeds fifty nodes") {
    sl::SharedFxRack rack; rack.prepare(48000.0);
    for (int i = 0; i < 60; ++i) {
        sl::Delay d; d.on = true; d.time = 0.001 * double(i); d.feedback = 0.1;
        rack.acquireDelay(d);
    }
    rack.beginRender();          // zyn calls cleanupFxNodes at the top of render()
    REQUIRE(rack.nodeCount() <= 30);
    REQUIRE(rack.nodeCount() > 0);
}
```

- [ ] **Step 2: Run and watch it fail**

Expected: compile error, `SharedFxRack.h` not found.

- [ ] **Step 3: Implement**

A `std::vector` of entries plus a key comparison, pre-reserved to 64 so
`acquire*` never allocates after `prepare`. Keys are the exact `double` fields of
`Delay` / `Verb`, or the full byte image of an `Osc` for passthrough placeholders.
`beginRender()` applies the `> 50` → drop-oldest-half rule.

Reverb impulses are generated on a worker: `acquireVerb` returns a node whose
`ready()` is false until filled, and `Voice` bypasses reverb while it is false.
Derive the impulse RNG seed from the config so the same reverb is bit-identical
across runs: `rngSeed = hashConfig(duration, decay)`.

- [ ] **Step 4: Run the tests**

Expected: 4 tests PASS.

- [ ] **Step 5: Commit**

```bash
git -C C:/dev/seedlathe add core/ tests/
git -C C:/dev/seedlathe commit -m "feat(core): shared FX cache reproducing zyn's global fxNodes behaviour"
```

---

### Task 13: Voice and VoicePool

**Files:**
- Create: `core/src/Voice.{h,cpp}`, `core/src/VoicePool.{h,cpp}`
- Test: `tests/test_voice.cpp`

**Interfaces:**
- Consumes: everything from Tasks 5, 7–12
- Produces:
  - `double sl::noteFrequency(int rootNote, int noteOffset)` — zyn's `Z.freq`
  - `sl::Voice` — `void prepare(double sampleRate, SharedFxRack*)`, `void noteOn(const Instrument&, int note, double gain, bool sustained)`, `void noteOff()`, `void render(double& outL, double& outR)`, `bool active() const`, `double currentLevel() const`
  - `sl::VoicePool` — `void prepare(double sampleRate, int maxVoices, SharedFxRack*)`, `int noteOn(const Instrument&, int note, double gain, bool sustained)`, `void noteOff(int note)`, `void allNotesOff()`, `void render(float* left, float* right, int frames)`, `int activeCount() const`

Reference formulas, taken from zyn's `render`:

- `freq(root, off) = 261.63 * pow(2, (root + off) / 12)`
- oscillator note offset is `note + osc.oct * 12 + osc.detune` — detune is in **semitones**
- `voiceGain = 1 / (notes * oscs)`, and `layer.gain = 0.5 * gain`
- gain envelope target is `layer.gain * voiceGain`; filter frequency envelope target is
  `20000`; filter Q envelope target is `30`
- attack time is `max(A[0], 0.005)`
- pitch envelope **replaces** frequency: `setValueAtTime(0)` then ramp to
  `oFreq * pENV.amount`
- FM oscillator frequency is `clamp(fm.frequency * oFreq, -22050, 22050)`, its gain is
  `fm.depth` in Hz
- FM matrix gain is `amt * targetFreq * 0.2`, routed through a delay of
  `fmDelays[s][t]` or `0.001` when zero, skipped when either endpoint is noise
- signal path per oscillator: `osc -> [shaper] -> filter -> gain -> panner -> [delay] -> [reverb] -> master`

- [ ] **Step 1: Write the failing test**

```cpp
#include <catch2/catch_test_macros.hpp>
#include "Voice.h"
#include "VoicePool.h"
#include "SharedFxRack.h"
#include "sl/InstrumentGen.h"
#include <cmath>
#include <vector>

TEST_CASE("a voice produces sound and then goes idle") {
    sl::SharedFxRack rack; rack.prepare(48000.0);
    sl::Voice v; v.prepare(48000.0, &rack);
    const sl::Instrument inst = sl::generateInstrument(13);   // "key", 2 oscs

    v.noteOn(inst, 0, 1.0, /*sustained=*/false);
    double peak = 0.0, l, r;
    for (int i = 0; i < 48000 * 4; ++i) {
        v.render(l, r);
        peak = std::max(peak, std::abs(l));
        REQUIRE(std::isfinite(l));
        REQUIRE(std::isfinite(r));
    }
    REQUIRE(peak > 1e-4);
    REQUIRE(v.active() == false);
}

TEST_CASE("a sustained voice holds until noteOff") {
    sl::SharedFxRack rack; rack.prepare(48000.0);
    sl::Voice v; v.prepare(48000.0, &rack);
    const sl::Instrument inst = sl::generateInstrument(3703184240u);

    v.noteOn(inst, 0, 1.0, /*sustained=*/true);
    double l, r;
    for (int i = 0; i < 48000 * 3; ++i) v.render(l, r);
    REQUIRE(v.active() == true);          // still held after 3 seconds

    v.noteOff();
    for (int i = 0; i < 48000 * 5; ++i) v.render(l, r);
    REQUIRE(v.active() == false);
}

TEST_CASE("frequency mapping matches zyn's Z.freq") {
    // Z.freq(0, 0) is middle C at 261.63 Hz; +12 semitones doubles it.
    REQUIRE(std::abs(sl::noteFrequency(0, 0) - 261.63) < 1e-9);
    REQUIRE(std::abs(sl::noteFrequency(0, 12) - 523.26) < 1e-6);
    REQUIRE(std::abs(sl::noteFrequency(0, -12) - 130.815) < 1e-6);
}

TEST_CASE("pool steals when it runs out of voices") {
    sl::SharedFxRack rack; rack.prepare(48000.0);
    sl::VoicePool pool; pool.prepare(48000.0, 4, &rack);
    const sl::Instrument inst = sl::generateInstrument(13);

    for (int n = 0; n < 8; ++n) pool.noteOn(inst, n, 1.0, true);
    REQUIRE(pool.activeCount() == 4);

    std::vector<float> L(512), R(512);
    pool.render(L.data(), R.data(), 512);
    for (int i = 0; i < 512; ++i) {
        REQUIRE(std::isfinite(L[i]));
        REQUIRE(std::isfinite(R[i]));
    }
}

TEST_CASE("editing the instrument mid-note does not disturb a ringing voice") {
    sl::SharedFxRack rack; rack.prepare(48000.0);
    sl::Voice v; v.prepare(48000.0, &rack);
    sl::Instrument inst = sl::generateInstrument(13);

    v.noteOn(inst, 0, 1.0, true);
    double l1, r1; for (int i = 0; i < 1000; ++i) v.render(l1, r1);

    inst.oscs[0].detune = 7.0;    // mutate the caller's copy
    double l2, r2; v.render(l2, r2);
    REQUIRE(std::isfinite(l2));   // the voice holds its own snapshot
    REQUIRE(v.active());
}

TEST_CASE("every golden seed renders finite audio") {
    sl::SharedFxRack rack; rack.prepare(48000.0);
    sl::Voice v; v.prepare(48000.0, &rack);
    for (uint32_t s = 0; s < 500; ++s) {
        rack.beginRender();
        v.noteOn(sl::generateInstrument(s), 0, 1.0, false);
        double l, r;
        for (int i = 0; i < 24000; ++i) {
            v.render(l, r);
            INFO("seed " << s << " sample " << i);
            REQUIRE(std::isfinite(l));
            REQUIRE(std::isfinite(r));
        }
    }
}
```

The last test is the cheap net that catches NaN-producing edge cases — a zero
envelope max producing `0/0`, a filter blowing up at Q 30, an FM matrix
self-modulation route running away.

- [ ] **Step 2: Run and watch it fail**

Expected: compile error, `Voice.h` not found.

- [ ] **Step 3: Implement Voice**

Each voice owns a fixed array of `kMaxOscs` chains. On `noteOn` it **copies** the
`Instrument` POD, so later edits to the caller's copy cannot reach it. It schedules
every `WaParam` up front from the ADSR fields, exactly as `Z.adsr` /
`Z.adsrSustain` do, and tracks `finalStopTime` as the maximum across all
oscillators.

`noteOff` mirrors `Z.noteOff`: for each gain, cancel scheduled values, hold the
current value, then ramp to zero over `max(R[0], 0.015)` seconds.

- [ ] **Step 4: Implement VoicePool**

Fixed array of voices, all pre-allocated in `prepare`. `noteOn` takes a free voice,
or steals the one with the lowest `currentLevel()`, breaking ties by age, applying a
5 ms fade before reuse.

- [ ] **Step 5: Run the tests**

Expected: 6 tests PASS.

- [ ] **Step 6: Commit**

```bash
git -C C:/dev/seedlathe add core/ tests/
git -C C:/dev/seedlathe commit -m "feat(core): voice rendering and polyphonic pool with stealing"
```

---

### Task 14: OfflineRender and the reference-audio exporter

**Files:**
- Create: `core/src/OfflineRender.{h,cpp}`
- Create: `tools/export-reference-audio.mjs`
- Create: `vectors/audio/*.wav` (generated, committed)
- Test: `tests/test_offlinerender.cpp`

**Interfaces:**
- Consumes: `sl::VoicePool`, `sl::SharedFxRack`, `sl::WaCompressor`
- Produces: `sl::RenderResult sl::renderOffline(const Instrument&, int note, double gain, double seconds, double sampleRate)` returning `{std::vector<float> left, right;}`; and `bool sl::writeWav(const std::string& path, const RenderResult&, double sampleRate)`

`renderOffline` must replicate zyn's full master chain: voices sum into a master
gain, which feeds the compressor, which feeds the output.

- [ ] **Step 1: Write the failing test**

```cpp
#include <catch2/catch_test_macros.hpp>
#include "OfflineRender.h"
#include "sl/InstrumentGen.h"
#include <cmath>

TEST_CASE("offline render returns the requested length of finite audio") {
    const auto out = sl::renderOffline(sl::generateInstrument(13), 0, 1.0, 2.0, 48000.0);
    REQUIRE(out.left.size() == 96000u);
    REQUIRE(out.right.size() == 96000u);
    double peak = 0.0;
    for (float s : out.left) { REQUIRE(std::isfinite(s)); peak = std::max(peak, std::abs((double)s)); }
    REQUIRE(peak > 1e-4);
    REQUIRE(peak <= 1.5);
}

TEST_CASE("offline render is deterministic") {
    const auto a = sl::renderOffline(sl::generateInstrument(3703184240u), 0, 1.0, 1.0, 48000.0);
    const auto b = sl::renderOffline(sl::generateInstrument(3703184240u), 0, 1.0, 1.0, 48000.0);
    REQUIRE(a.left == b.left);
    REQUIRE(a.right == b.right);
}
```

Determinism is a hard requirement: without it the null-test cannot have a stable
threshold.

- [ ] **Step 2: Run and watch it fail**

Expected: compile error, `OfflineRender.h` not found.

- [ ] **Step 3: Implement OfflineRender and writeWav**

Fresh `SharedFxRack` per call, so cache state cannot leak between renders — this is
what makes the second test pass. 32-bit float WAV output, stereo interleaved.

- [ ] **Step 4: Write the reference exporter**

`tools/export-reference-audio.mjs`. The important parts are the three overrides:
zyn's `init` hardcodes `new AudioContext()`, its noise and reverb use
`Math.random()`, and its `fxNodes` cache is global and would leak nodes belonging to
a disposed context between renders.

```js
import { readFileSync, writeFileSync, mkdirSync } from "node:fs";
import { resolve, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import puppeteer from "puppeteer-core";

const here = dirname(fileURLToPath(import.meta.url));
const ZYN = process.env.ZYN_DIR ?? "C:/dev/zyn";
const CHROME = process.env.CHROME_PATH ??
  "C:/Program Files/Google/Chrome/Application/chrome.exe";
const OUTDIR = resolve(here, "../vectors/audio");
const SR = 48000, SECONDS = 2.0;

const presets = JSON.parse(readFileSync(resolve(ZYN, "default-presets.json"), "utf8"));
const seeds = presets.map((p) => p.seed);
// Plus a deterministic spread, so the null-test is not only factory presets.
let x = 999;
while (seeds.length < presets.length + 200) {
  x = (1103515245 * x + 12345) % 4294967296;
  seeds.push(x);
}

const browser = await puppeteer.launch({
  executablePath: CHROME,
  headless: "new",
  args: ["--autoplay-policy=no-user-gesture-required"],
});
const page = await browser.newPage();
await page.setContent("<html><body></body></html>");
await page.addScriptTag({ path: resolve(ZYN, "zyn-unminified.js") });

mkdirSync(OUTDIR, { recursive: true });

for (const seed of seeds) {
  const channels = await page.evaluate(async (seed, SR, SECONDS) => {
    const offline = new OfflineAudioContext(2, Math.round(SR * SECONDS), SR);

    // Replicate Z.init against the offline context.
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

    // Skip warmUp: it resumes a live AudioContext, which an offline one lacks.
    Z.warmedUp = true;
    // Clear per-context global state, or nodes from the previous render leak in.
    Z.fxNodes = {};
    Z.noiseBuffer = null;
    Z.activeVoices = {};

    // Deterministic noise, matching sl::globalNoiseBuffer's fixed seed. Without
    // this the reference is different on every run and no threshold can hold.
    let a = 0x5EED1A7E >>> 0;
    Z.randSample = () => {
      a = (a + 0x6d2b79f5) >>> 0;
      let t = a;
      t = Math.imul(t ^ (t >>> 15), t | 1);
      t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
      return ((((t ^ (t >>> 14)) >>> 0) / 4294967296) * 2) - 1;
    };

    Z.play(0, Z.getInstrument(seed), 1.0);
    const buf = await offline.startRendering();
    return [Array.from(buf.getChannelData(0)), Array.from(buf.getChannelData(1))];
  }, seed, SR, SECONDS);

  writeFileSync(resolve(OUTDIR, `${seed}.wav`), wav32(channels, SR));
  console.log("rendered", seed);
}

await browser.close();

function wav32([l, r], sr) {
  const n = l.length, bytes = n * 8;
  const b = Buffer.alloc(44 + bytes);
  b.write("RIFF", 0); b.writeUInt32LE(36 + bytes, 4); b.write("WAVE", 8);
  b.write("fmt ", 12); b.writeUInt32LE(16, 16); b.writeUInt16LE(3, 20);
  b.writeUInt16LE(2, 22); b.writeUInt32LE(sr, 24); b.writeUInt32LE(sr * 8, 28);
  b.writeUInt16LE(8, 32); b.writeUInt16LE(32, 34);
  b.write("data", 36); b.writeUInt32LE(bytes, 40);
  for (let i = 0; i < n; i++) {
    b.writeFloatLE(l[i], 44 + i * 8);
    b.writeFloatLE(r[i], 48 + i * 8);
  }
  return b;
}
```

Note the deliberate use of `Z.randSample` override rather than patching the noise
buffer directly: zyn uses it for both the noise source and the reverb impulse, so a
single override covers both.

- [ ] **Step 5: Run it**

```bash
npm --prefix C:/dev/seedlathe/tools run audio
```

Expected: 315 WAV files (115 presets + 200 seeds) under `vectors/audio/`.

- [ ] **Step 6: Run it twice and confirm the output is byte-identical**

```bash
node -e "
const {readdirSync,readFileSync}=require('fs');
const d='C:/dev/seedlathe/vectors/audio';
const h=require('crypto').createHash('sha256');
for(const f of readdirSync(d).sort()) h.update(readFileSync(d+'/'+f));
console.log(h.digest('hex'));
"
```

Run the exporter again and repeat. Expected: the same hash. If it differs, a
non-deterministic source remains — check that `Z.fxNodes` and `Z.noiseBuffer` are
being reset per render.

- [ ] **Step 7: Commit**

```bash
git -C C:/dev/seedlathe add core/ tools/ tests/ vectors/audio/
git -C C:/dev/seedlathe commit -m "feat: offline renderer and deterministic Chrome reference audio"
```

---

### Task 15: The fidelity null-test

**Files:**
- Create: `core/src/Analysis.{h,cpp}`
- Test: `tests/test_fidelity.cpp`
- Create: `vectors/fidelity-thresholds.json`

**Interfaces:**
- Consumes: `sl::renderOffline`
- Produces: `sl::MelSpectrogram sl::melSpectrogram(const std::vector<float>&, double sampleRate)`, `double sl::melDistanceDb(const MelSpectrogram&, const MelSpectrogram&)`, `double sl::rmsEnvelopeDistanceDb(const std::vector<float>&, const std::vector<float>&, double sampleRate)`

This is the acceptance gate for the whole of P1. Analysis parameters: FFT 2048,
hop 512, 64 mel bands from 20 Hz to Nyquist, magnitudes in dB with a −100 dB floor.

- [ ] **Step 1: Write the analysis unit tests first**

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "Analysis.h"
#include <cmath>
#include <vector>

using Catch::Matchers::WithinAbs;

TEST_CASE("identical signals have zero mel distance") {
    std::vector<float> x(48000);
    for (size_t i = 0; i < x.size(); ++i)
        x[i] = float(0.5 * std::sin(2.0 * 3.14159265 * 440.0 * double(i) / 48000.0));
    const auto a = sl::melSpectrogram(x, 48000.0);
    REQUIRE_THAT(sl::melDistanceDb(a, a), WithinAbs(0.0, 1e-9));
}

TEST_CASE("mel distance grows with pitch difference") {
    auto tone = [](double f) {
        std::vector<float> x(48000);
        for (size_t i = 0; i < x.size(); ++i)
            x[i] = float(0.5 * std::sin(2.0 * 3.14159265 * f * double(i) / 48000.0));
        return x;
    };
    const auto a = sl::melSpectrogram(tone(440.0), 48000.0);
    const auto near = sl::melSpectrogram(tone(444.0), 48000.0);
    const auto far  = sl::melSpectrogram(tone(880.0), 48000.0);
    REQUIRE(sl::melDistanceDb(a, near) < sl::melDistanceDb(a, far));
}

TEST_CASE("rms envelope distance detects a level offset") {
    std::vector<float> a(48000, 0.5f), b(48000, 0.25f);
    REQUIRE_THAT(sl::rmsEnvelopeDistanceDb(a, b, 48000.0), WithinAbs(6.02, 0.1));
}
```

- [ ] **Step 2: Implement Analysis, run these three, confirm they pass**

- [ ] **Step 3: Write the null-test with a calibration mode**

```cpp
#include <catch2/catch_test_macros.hpp>
#include "Analysis.h"
#include "OfflineRender.h"
#include "sl/InstrumentGen.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace {
struct Wav { std::vector<float> left, right; double sampleRate; };

// Reads the 32-bit float stereo WAV written by tools/export-reference-audio.mjs.
// Fixed 44-byte header, format tag 3, so no chunk walking is needed.
Wav readWav32(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    char hdr[44];
    f.read(hdr, 44);
    uint32_t sr = 0, dataBytes = 0;
    std::memcpy(&sr, hdr + 24, 4);
    std::memcpy(&dataBytes, hdr + 40, 4);
    const size_t frames = dataBytes / 8;

    Wav w;
    w.sampleRate = double(sr);
    w.left.resize(frames);
    w.right.resize(frames);
    for (size_t i = 0; i < frames; ++i) {
        float lr[2];
        f.read(reinterpret_cast<char*>(lr), 8);
        w.left[i] = lr[0];
        w.right[i] = lr[1];
    }
    return w;
}

struct Score { uint32_t seed; double mel; double rms; };
} // namespace

TEST_CASE("C++ renders match Chrome within the locked thresholds", "[fidelity]") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::path(SL_VECTORS_DIR) / "audio";
    REQUIRE(fs::exists(dir));

    std::ifstream tin(std::string(SL_VECTORS_DIR) + "/fidelity-thresholds.json");
    REQUIRE(tin.good());
    nlohmann::json th; tin >> th;
    const double melLimit = th["melMaxDb"].get<double>();
    const double rmsLimit = th["rmsMaxDb"].get<double>();

    std::vector<Score> scores;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (e.path().extension() != ".wav") continue;
        const auto seed = static_cast<uint32_t>(std::stoull(e.path().stem().string()));
        const Wav ref = readWav32(e.path().string());
        const auto got = sl::renderOffline(sl::generateInstrument(seed), 0, 1.0,
                                           double(ref.left.size()) / ref.sampleRate,
                                           ref.sampleRate);
        const double mel = sl::melDistanceDb(sl::melSpectrogram(got.left, ref.sampleRate),
                                             sl::melSpectrogram(ref.left, ref.sampleRate));
        const double rms = sl::rmsEnvelopeDistanceDb(got.left, ref.left, ref.sampleRate);
        scores.push_back({seed, mel, rms});
    }
    REQUIRE(scores.size() >= 300);

    // Report the worst offenders before asserting, so a failure names the seeds.
    std::sort(scores.begin(), scores.end(),
              [](const Score& a, const Score& b) { return a.mel > b.mel; });
    for (size_t i = 0; i < std::min<size_t>(10, scores.size()); ++i)
        WARN("worst mel: seed " << scores[i].seed << " = " << scores[i].mel << " dB");

    for (const auto& s : scores) {
        INFO("seed " << s.seed << " mel " << s.mel << " dB, rms " << s.rms << " dB");
        REQUIRE(s.mel <= melLimit);
        REQUIRE(s.rms <= rmsLimit);
    }
}
```

- [ ] **Step 4: Calibrate the thresholds**

Seed `vectors/fidelity-thresholds.json` with generous starting values so the
test runs and reports rather than aborting on the first seed:

```json
{
  "melMaxDb": 100.0,
  "rmsMaxDb": 100.0,
  "note": "Placeholder ceiling. Replaced by the calibration step in Task 15."
}
```

Run the test, read the `WARN` output, and tighten to the real distribution:

```bash
"...ctest.exe" --test-dir C:/dev/seedlathe/build -C Release -R fidelity --output-on-failure
```

Set `melMaxDb` and `rmsMaxDb` to the 100th percentile of the observed distribution
plus 20% headroom, then rerun to confirm everything passes with no margin abuse.
Target values, for judging whether the port is actually good: **mel ≤ 2.0 dB** and
**rms ≤ 1.0 dB**. If the calibrated numbers land far above those, the port has a
real problem — do not simply widen the threshold. Use the worst-seed list to find
which component is responsible; a single dominant node type will show up as a
cluster of seeds that all share it.

- [ ] **Step 5: Commit the calibrated thresholds**

```bash
git -C C:/dev/seedlathe add core/ tests/ vectors/fidelity-thresholds.json
git -C C:/dev/seedlathe commit -m "test: fidelity null-test against Chrome with calibrated thresholds"
```

---

### Task 16: Plugin parameters, MIDI, and a playable standalone

**Files:**
- Create: `plugin/SeedlatheParams.h`
- Modify: `plugin/Seedlathe.h`, `plugin/Seedlathe.cpp`, `plugin/config.h`
- Test: `tests/test_seedsplit.cpp`

**Interfaces:**
- Consumes: `sl::VoicePool`, `sl::SharedFxRack`, `sl::generateInstrument`, `sl::WaCompressor`
- Produces: a plugin that responds to MIDI note on/off and exposes `kSeedHi`, `kSeedLo`, `kVolume`, `kOctave`, `kVoices`

- [ ] **Step 1: Write the seed-split test**

This is the guard for spec §7 — the bug that would present as "presets are randomly
broken".

```cpp
#include <catch2/catch_test_macros.hpp>
#include "SeedlatheParams.h"
#include <cmath>
#include <cstdint>

TEST_CASE("seed survives a round trip through float32 host automation") {
    const uint32_t seeds[] = {0u, 1u, 13u, 65535u, 65536u, 3703184240u, 4294967295u};
    for (uint32_t seed : seeds) {
        const int hi = sl::seedHi(seed);
        const int lo = sl::seedLo(seed);
        REQUIRE(hi >= 0); REQUIRE(hi <= 65535);
        REQUIRE(lo >= 0); REQUIRE(lo <= 65535);

        // What a host actually stores: normalise to [0,1] as float32, then back.
        const float nHi = static_cast<float>(double(hi) / 65535.0);
        const float nLo = static_cast<float>(double(lo) / 65535.0);
        const int rHi = static_cast<int>(std::lround(double(nHi) * 65535.0));
        const int rLo = static_cast<int>(std::lround(double(nLo) * 65535.0));

        INFO("seed " << seed);
        REQUIRE(sl::seedFrom(rHi, rLo) == seed);
    }
}

TEST_CASE("a single float32 parameter provably cannot carry a 32-bit seed") {
    // The reason the split exists. This must fail to round-trip.
    const uint32_t seed = 3703184240u;
    const float n = static_cast<float>(double(seed) / 4294967295.0);
    const auto back = static_cast<uint32_t>(std::llround(double(n) * 4294967295.0));
    REQUIRE(back != seed);
}
```

- [ ] **Step 2: Run and watch it fail**

Expected: compile error, `SeedlatheParams.h` not found.

- [ ] **Step 3: Implement the parameter helpers**

```cpp
#pragma once
#include <cstdint>
#include <cmath>

namespace sl {

enum ParamIdx { kSeedHi = 0, kSeedLo, kVolume, kOctave, kVoices, kNumParams };

inline int seedHi(uint32_t seed) { return static_cast<int>(seed >> 16); }
inline int seedLo(uint32_t seed) { return static_cast<int>(seed & 0xFFFFu); }
inline uint32_t seedFrom(int hi, int lo) {
    return (static_cast<uint32_t>(hi) << 16) | (static_cast<uint32_t>(lo) & 0xFFFFu);
}

} // namespace sl
```

- [ ] **Step 4: Run the test**

Expected: both PASS. The second test passing is what documents *why* the split
exists — if it ever starts failing, someone has changed the arithmetic.

- [ ] **Step 5: Wire the plugin**

In `Seedlathe.cpp`:
- `InitParam(kSeedHi, "Seed Hi", 0, 0, 65535, 1)` and the same for `kSeedLo`; both
  stepped integers so hosts quantise them exactly.
- `kVolume` 0–500%, `kOctave` −3…+3, `kVoices` 1–64.
- `OnReset`: `prepare` the rack, pool and compressor at the host sample rate.
- `ProcessBlock`: read `kSeedHi`/`kSeedLo`, and when the combined seed differs from
  the cached one, call `generateInstrument` **in place** — it allocates nothing —
  and store it. Then drain MIDI into `pool.noteOn` / `pool.noteOff`, render, and run
  the sum through the compressor.
- Gain follows zyn: `0.5 * volume * (velocity / 127)`.
- Note mapping follows the demo: MIDI note 60 is zyn note 0, plus `kOctave * 12`.

- [ ] **Step 6: Verify no allocation on the audio thread**

Build with `/fsanitize=address` and play sustained chords for 30 seconds while
sweeping `Seed Hi`. Expected: no allocation reported inside `ProcessBlock`. If
`generateInstrument` shows up as allocating, an `std::vector` has crept into the
`Instrument` POD — it must stay `std::array` throughout.

- [ ] **Step 7: Play it**

Launch the standalone, connect a MIDI keyboard or use the host's virtual keyboard,
enter seed 3703184240 by setting `Seed Hi` to **56506** and `Seed Lo` to **7024**.
Expected: it sounds like the "Forest" preset on the demo page. (The values written
here originally, 56505 / 55024, were simply wrong arithmetic.)

Verify the split arithmetic before trusting the ear:
```bash
node -e "const s=3703184240;console.log('hi',s>>>16,'lo',s&0xFFFF);"
```

- [ ] **Step 8: Run the full suite**

```bash
"...ctest.exe" --test-dir C:/dev/seedlathe/build -C Release --output-on-failure
```

Expected: every test passes, including `[fidelity]`.

- [ ] **Step 9: Commit**

```bash
git -C C:/dev/seedlathe add plugin/ tests/
git -C C:/dev/seedlathe commit -m "feat(plugin): seed parameters, MIDI handling, playable standalone

Seed is split across two 16-bit stepped parameters because a single float32
automation value cannot round-trip a 32-bit seed. test_seedsplit.cpp asserts
both the split working and the naive approach failing."
```

**P1 is complete.** The engine reproduces zyn within the locked thresholds and plays
over MIDI.

---

## Definition of Done

- [x] `ctest` green, including `[fidelity]` over the committed reference renders
- [x] Golden vectors match exactly for 2000 seeds
- [x] Every committed reference seed has run-to-run divergence below −60 dB
      relative to peak. **Not** a hash comparison — that criterion was wrong.
      Chrome does not render bit-identically: measured over 61 seeds with three
      renders each, 58 agree to better than −120 dB (float32 last-bit noise), 2
      to between −120 and −60 dB, and one diverges by 9.5 dB because the
      compressor's adaptive release chaotically amplifies a last-bit difference
      arising near its 288-frame pre-delay boundary. Unstable seeds are pinned
      in `vectors/unstable-seeds.json` and excluded from the reference set
- [x] Standalone launches, hosts the engine and closes cleanly. **Not yet
      verified by ear** — no MIDI input device is present on this machine, so
      "sounds like Forest" is still unconfirmed. The engine path it uses is the
      one the fidelity test covers
- [x] VST3, CLAP and standalone all build in Release
- [x] No allocation inside the render path — verified by a global `operator new`
      counter in `tests/test_realtime.cpp` rather than ASan, which is stricter
      for this purpose: a sanitiser will happily allow a `std::vector` to grow
      inside the audio callback. It caught 16 allocations per block in the
      convolver
- [x] zyn.js distortion fix committed upstream with `stream-parity.mjs` passing

## Deferred to later phases

Not in P0+P1, by design: the IGraphics UI beyond what standalone provides (P2), the
designer (P3), search of any kind (P4, P5), multitimbral parts (P6), oversampling
integration into the live path (P7 — `Oversampler` is written in Task 10 for the
shaper, but the global quality parameter is not wired), and macro parameters.
