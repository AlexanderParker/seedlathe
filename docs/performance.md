# Performance baseline

Numbers from `tests/test_perf.cpp`, which is hidden from the default run
(`[.perf]`) because it measures rather than asserts. Run one by name:

```
build/tests/Release/sl_tests.exe "render path realtime factor"
build/tests/Release/sl_tests.exe "worst-case instruments"
build/tests/Release/sl_tests.exe "compressor and full plugin chain"
build/tests/Release/sl_tests.exe "search throughput"
```

They print rather than assert on purpose: a threshold tuned to one machine
fails on another for reasons that have nothing to do with the code. This file
is the comparison instead — if a change moves a number here, that is the thing
to explain.

## Measured

Release build, MSVC 2022, 13th Gen Intel Core i7-13650HX, 48 kHz.

| What | Realtime factor |
|---|---|
| seed 13 (2 osc, reverb), 1 voice | 43.7× |
| seed 13, 8 voices | 17.8× |
| seed 3703184240 (1 osc), 1 voice | 74.3× |
| seed 3703184240, 8 voices | 35.3× |
| Full plugin chain, 8 voices | 17.0× |
| Compressor alone | 348× |
| **Worst case:** seed 1559, 5 osc / 5 reverbs (9.9 s of tail), 8 voices | **8.0×** |
| seed 5085, 4 osc / 4 reverbs, 8 voices | 11.2× |
| seed 12309, 5 osc / 4 reverbs, 8 voices | 10.5× |

Similarity search: 1.80 million candidates/sec on one thread, reaching 92.3%
within 200,000 candidates.

Sample-match search: roughly 50 candidates/sec per thread, because each one is
a full offline render. Four orders of magnitude slower than the search above,
which is why it runs on a pool and reports its best as it goes.

## How it got here

The engine started at 8.3× for one voice and about 1.5× for the worst-case
five-reverb instrument, which is unusable. What moved it, roughly in order of
how much each was worth:

- **Non-uniform partitioned convolution** for the reverbs — a direct head for
  the first 64 samples, then FFT blocks of 64, 512, 4096 and 8192. This alone
  took a single convolver from 11.4× to 100.8×.
- **Block-based voice rendering.** Interleaving voices and oscillators sample
  by sample evicted each voice's state from L1 on every sample; at eight voices
  the path was memory-bound rather than arithmetic-bound, and micro-optimising
  the arithmetic had stopped helping.
- **Float convolver with half-spectrum accumulation and AVX2.**
- **Removing per-sample work that did not need to be per-sample:** a mutex in
  the oscillator's wavetable lookup, a `log2` for band selection, the panner's
  `cos`/`sin`, and a divide in the envelope.
- **Incremental envelope evaluation** rather than re-deriving the automation
  timeline position each sample.
- **Denormal flushing (FTZ/DAZ)** for the duration of the audio callback.
  Decaying tails run down toward 1e-38, where the CPU falls into microcode.

Things that turned out not to cost anything measurable, and so were kept for
correctness rather than traded away:

- The unconditional pitch-bend multiply in the per-sample path. The branch that
  would skip it when nothing is bent costs more than the multiply.
- Per-oscillator release ramps instead of one voice-wide ramp.
- Summing sixteen multitimbral parts — silent parts are skipped entirely, so
  the cost tracks what is actually sounding rather than what is allocated.
