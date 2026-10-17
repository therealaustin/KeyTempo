# KeyTempo

Cross-platform JUCE audio plug-in (VST3, AU, LV2, Standalone; AAX if the SDK is present). It measures
**tempo/BPM** (the main focus), **key / relative key** (Camelot) and **time signature**. Its analysis core
(`src/core`, no JUCE dependency) is also the backend for the TrueGrid librarian (github.com/therealaustin/TrueGrid).

Owner priority: **tempo accuracy on rhythmically complex material**, especially drum & bass (half/double and
3:2 traps) and syncopated house/trance. Key detection is secondary. Don't trade tempo accuracy for anything else.

## Build & test

```bash
# Full build (plug-in + tests); JUCE 9.0.3 and clap-juce-extensions are fetched
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure          # kt_core_tests

# Core only (fast, no GUI deps)
cmake -S . -B build-core -G Ninja -DCMAKE_BUILD_TYPE=Release -DKT_BUILD_PLUGIN=OFF
cmake --build build-core --parallel

# Tempo benchmark: 66 synthetic genre tracks (tests/bench/GrooveLab.h). CI gate: --hard --min 64
build-core/kt_tempo_bench --hard --min 64
build-core/kt_tempo_bench --only "DnB" --genre 3     # filter by style name, force a genre preset
```

Other options:
- `-DKT_BUILD_TOOLS=ON` builds `kt_tool`: `analyze [--trace] [--genre N] file`, demo and UI snapshot.
- `-DKT_GRID_DEBUG` prints downbeat scoring from `TrackAnalyzer.cpp`.

CI (`.github/workflows/build.yml`) builds on Windows, macOS and Linux, runs the tests and benchmark, and
validates the plug-in with pluginval (pinned version) and, on macOS, auval. clap-validator is not run in CI.

## Rules for tempo changes

1. Run `kt_tempo_bench` (normal and `--hard`) **before and after** any change to `TempoDetector`,
   `GrooveModel`, `AnalysisEngine` or `TrackAnalyzer`. Both must stay at 66/66 (CI fails below 64).
2. Re-check the real tracks too (see below). The current result is 10/10 within 0.02 BPM, and a change that
   wins on synthetic tracks has broken real ones before.
3. Keep the result stable. Changes to `updateIntervalSeconds` (1.2 s) have shifted downbeats before (2.0 was
   rejected for that reason).
4. When analysis output changes, bump `analysisVersion` in `src/core/TrackAnalyzer.h`. TrueGrid uses it to decide
   which tracks to re-analyse.

Approaches already tried and reverted (don't retry them without a new idea):
- A symmetric 2× check broke hard boom bap. The hat-based one-sided check on the kick-corrected grid stays.
- Triplet-grid alignment caused 2:3 errors. 9/8 is handled by the kick-structure gate instead.

## Real-track checks

The owner's 10 test tracks (5 DnB, 5 house/trance) are **copyrighted and must never be committed**. Results and
the re-run recipe (ffmpeg → `.f32` → manifest → `kt_tempo_bench --files`) are in `tests/REAL_TRACKS.md`.
Ask the owner for the folder path on their machine.

## Architecture (src/core)

- `TempoDetector`:
  - Spectral-flux onsets in kick, snare and hat bands. The snare signal is the geometric mean of the mid and high bands.
  - Autocorrelation candidates, expanded to 2×, ½, 3:2 and 2:3 readings.
- `GrooveModel`:
  - Folds each bar into 16 steps and matches four-on-the-floor, backbeat and half-time templates.
  - Tempo plausibility comes from genre conventions (`Genre.h`).
  - Extra evidence: grid alignment, a kick-structure gate, and an 8th-note phase shift.
- `AnalysisEngine`: real-time engine used by the plug-in.
  - Tempo memory with octave-family switching and a weighted-median BPM.
  - A 30 s precision window.
- `TrackAnalyzer`: streaming whole-file analysis used by TrueGrid.
  - Interface: `push()` blocks, then call `finish()` once.
  - Beat grid uses a kick-led phase lock and a multi-cue downbeat.
  - Grids are 4/4 unless the track is confidently in 3.
- `KeyDetector`, `MusicTheory`: key, Camelot and Open Key.
- `Fft`: real FFT done as a half-size complex transform.

The plug-in UI is in `src/plugin`. `CMakeLists.txt` exports a `kt::core` alias target for other projects.

## Conventions

- C++20, JUCE style (spaces before parentheses, Allman braces). Keep `src/core` free of JUCE.
- Write UTF-8 literals in JUCE code with `juce::String::fromUTF8 ("...")`.
- TrueGrid pins KeyTempo to a commit. After changing the core here, push, then bump the `GIT_TAG` in TrueGrid's
  `CMakeLists.txt`.
