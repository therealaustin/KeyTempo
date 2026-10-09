# KeyTempo

![KeyTempo](docs/screenshot.png)

A utility plug-in that listens to whatever you put it on and tells you three things:

- **Tempo** — BPM to ±0.05, with ½× / 1× / 2× display and selectable range
- **Key** — e.g. *A minor*, with its **relative key** (*C major*), Camelot code (*8A*) and tuning offset
- **Time signature** — 4/4, 3/4, 6/8, 9/8 (12/8 and 2/4 are reported as 6/8 and 4/4)

Audio passes through untouched. Analysis runs on a low-priority background thread (~4% of one core),
never on the audio thread.

| Format     | Windows | macOS | Linux |
|------------|:-------:|:-----:|:-----:|
| VST3       | ✓ | ✓ | ✓ |
| CLAP       | ✓ | ✓ | ✓ |
| AU         |   | ✓ |   |
| LV2        | ✓ | ✓ | ✓ |
| Standalone | ✓ | ✓ | ✓ |
| AAX        | with the Avid SDK (see below) | | |

## Building

Requires CMake 3.22+ and a C++20 compiler (Visual Studio 2022, Xcode 15+, GCC 11+/Clang 14+).
JUCE 9 and the CLAP extensions are downloaded automatically.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release        # analysis unit tests
```

Plug-ins land in `build/KeyTempo_artefacts/Release/<FORMAT>/`.

**Linux packages** (Ubuntu/Debian):
```bash
sudo apt install libasound2-dev libjack-jackd2-dev libfreetype-dev libfontconfig1-dev \
  libx11-dev libxcomposite-dev libxcursor-dev libxext-dev libxinerama-dev libxrandr-dev \
  libxrender-dev libxi-dev libglu1-mesa-dev mesa-common-dev libegl-dev
```

Useful options:

| Option | Effect |
|---|---|
| `-DKT_JUCE_PATH=/path/to/JUCE` | Use a local JUCE checkout instead of downloading |
| `-DKT_AAX_SDK_PATH=/path/to/aax-sdk` | Also build AAX (needs PACE signing to load in Pro Tools) |
| `-DKT_BUILD_PLUGIN=OFF` | Build/test only the analysis core (no JUCE, builds in seconds) |
| `-DKT_BUILD_TOOLS=ON` | Build `kt_tool` (below) |

Before releasing, change the identity block at the top of `CMakeLists.txt`
(company name, 4-character manufacturer and plug-in codes, bundle ID, LV2 URI).

### `kt_tool` — checking accuracy on real music

```bash
cmake -S . -B build -DKT_BUILD_TOOLS=ON && cmake --build build --target kt_tool
build/kt_tool_artefacts/Release/kt_tool analyze song1.wav song2.mp3 ...
build/kt_tool_artefacts/Release/kt_tool snapshot ui.png [song.wav]   # renders the editor
```

### CI

`.github/workflows/build.yml` builds on Windows, macOS (universal arm64/x86_64) and Linux, runs the
unit tests, validates the VST3 with pluginval (strictness 10) and the AU with `auval`, and uploads
zipped builds. Pushing a `v*` tag creates a draft GitHub release with all three zips.

## How it works

All analysis lives in `src/core/` — plain C++ with no JUCE dependency, so it is unit-tested on its own.

**Tempo** (`TempoDetector`) — A log-compressed spectral-flux onset envelope (~6 ms resolution) is
autocorrelated over a 12-second window. Candidate tempos are scored on periodicity at 1–4 beats plus
their subdivision, weighted by a tempo prior centred in the selected range. The top candidates are
re-ranked by *beat salience* (do the weakest beats still land on real hits, rather than hi-hats?),
which rejects 3:2 mistakes. The winner is then refined by phase-locking a pulse train to the onsets
across the whole window, which is what gets precision to a few hundredths of a BPM.

**Time signature** — With the beat known, a kick/bass-band envelope shows whether the bar repeats every
2/4 beats or every 3, and the full-band envelope shows whether beats divide in two (simple) or three
(compound). Evidence is averaged over several seconds before a meter is shown.

**Key** (`KeyDetector`) — A ~370 ms FFT with spectral peak picking builds a 12-note chromagram (65 Hz–2 kHz),
corrected for the track's tuning so A=432 material still works. The time-averaged chroma is correlated
against 24 key profiles (Sha'ath/KeyFinder profiles), and a separate bass chroma favours keys whose
tonic the bass keeps returning to — that's what separates a minor key from its relative major.

## Controls

- **½× / 1× / 2×** — display the tempo halved or doubled (for half-time / double-time feels)
- **Tempo range** — 70–180 (default), 50–100, 80–160, 100–200 BPM. Above ~150 BPM a backbeat is
  genuinely ambiguous with its half-time feel; pick 100–200 for DnB, hardcore, fast punk, etc.
- **Key memory** — how far back key evidence counts: 15 s, 45 s, 2 min or the whole track.
  Shorter follows modulations; longer is steadier.
- **Hold** freezes the readings; **Reset** forgets everything heard so far.

## Known limitations (v0.1)

- Relative major/minor is the hardest call for any key detector. If a song never leans on its tonic
  (equal time on every chord, no bass), KeyTempo may name the relative key; the card always shows
  both, and the confidence bar drops when it's unsure.
- Time signature covers 4/4, 3/4, 6/8 and 9/8. Odd meters (5/4, 7/8) are not detected yet, and
  2/4 vs 4/4 and 6/8 vs 12/8 are not distinguished.
- Results are estimates from listening; music with rubato, no percussion, or atonal content will
  produce low confidence rather than a reliable answer.
- Builds are unsigned. On macOS, Gatekeeper may block them until you run
  `xattr -dr com.apple.quarantine <plugin>` or sign/notarize them.

## Licensing note

JUCE 8 and later is dual-licensed under AGPLv3 or a commercial JUCE licence (there is a free tier
for small developers). If you distribute closed-source builds you need the commercial licence.
CLAP and the clap-juce-extensions are MIT-licensed. AAX requires a free Avid developer account and
PACE signing.

## Project layout

```
src/core/      analysis engine (no JUCE): FFT, tempo/meter, key, music theory
src/plugin/    JUCE processor, editor and look-and-feel
tests/         unit tests with synthesised grooves and chord progressions
tools/         kt_tool: offline analyzer and UI snapshot
```
