// Self-contained tests for the analysis core, using synthesised audio.
// Build: cmake -B build -DKT_BUILD_PLUGIN=OFF && cmake --build build && ctest --test-dir build

#include "AnalysisEngine.h"
#include "MusicTheory.h"

#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace kt;

static int failures = 0;

#define CHECK(cond, ...)                                    \
    do {                                                    \
        if (! (cond)) {                                     \
            ++failures;                                     \
            std::printf ("  FAIL: " __VA_ARGS__);           \
            std::printf ("   [%s:%d]\n", __FILE__, __LINE__); \
        }                                                   \
    } while (0)

static constexpr double pi = 3.14159265358979323846;

// --------------------------------------------------------------------------
// Signal generators
// --------------------------------------------------------------------------
static void addDrums (std::vector<float>& out, double sr, double bpm, unsigned seed = 1)
{
    std::mt19937 rng (seed);
    std::uniform_real_distribution<float> noise (-1.0f, 1.0f);
    const double beat = 60.0 / bpm;
    const double length = out.size() / sr;

    for (int i = 0; i * beat * 0.5 < length; ++i) // eighth notes
    {
        const auto start = (size_t) (i * beat * 0.5 * sr);
        const bool onBeat = (i % 2) == 0;
        const int beatInBar = (i / 2) % 4;

        // Hi-hat every eighth
        for (size_t n = 0; n < (size_t) (0.03 * sr) && start + n < out.size(); ++n)
            out[start + n] += 0.15f * noise (rng) * (float) std::exp (-(double) n / (0.008 * sr));

        if (onBeat && (beatInBar == 0 || beatInBar == 2)) // kick on 1 and 3
        {
            double phase = 0.0;
            for (size_t n = 0; n < (size_t) (0.25 * sr) && start + n < out.size(); ++n)
            {
                const double t = n / sr;
                phase += 2.0 * pi * (50.0 + 100.0 * std::exp (-t * 30.0)) / sr;
                out[start + n] += 0.8f * (float) (std::sin (phase) * std::exp (-t * 12.0));
            }
        }
        if (onBeat && (beatInBar == 1 || beatInBar == 3)) // snare on 2 and 4
        {
            for (size_t n = 0; n < (size_t) (0.15 * sr) && start + n < out.size(); ++n)
            {
                const double t = n / sr;
                out[start + n] += (float) ((0.4 * noise (rng) + 0.3 * std::sin (2 * pi * 190 * t)) * std::exp (-t * 25.0));
            }
        }
    }
}

// Pattern-based groove. Each character is one subdivision step:
//   'K' kick+hat, 'S' snare+hat, 'X' kick+snare+hat, 'H' hat, 'k' kick only, '.' rest.
static void addGroove (std::vector<float>& out, double sr, double stepsPerMinute, const std::string& bar, unsigned seed = 7)
{
    std::mt19937 rng (seed);
    std::uniform_real_distribution<float> noise (-1.0f, 1.0f);
    const double stepSec = 60.0 / stepsPerMinute;
    for (int i = 0; i * stepSec * sr < out.size(); ++i)
    {
        const auto start = (size_t) (i * stepSec * sr);
        const char c = bar[(size_t) i % bar.size()];
        if (c == 'K' || c == 'S' || c == 'X' || c == 'H')
            for (size_t n = 0; n < (size_t) (0.03 * sr) && start + n < out.size(); ++n)
                out[start + n] += 0.15f * noise (rng) * (float) std::exp (-(double) n / (0.008 * sr));
        if (c == 'K' || c == 'X' || c == 'k')
        {
            double phase = 0.0;
            for (size_t n = 0; n < (size_t) (0.25 * sr) && start + n < out.size(); ++n)
            {
                const double t = n / sr;
                phase += 2.0 * pi * (50.0 + 100.0 * std::exp (-t * 30.0)) / sr;
                out[start + n] += 0.8f * (float) (std::sin (phase) * std::exp (-t * 12.0));
            }
        }
        if (c == 'S' || c == 'X')
            for (size_t n = 0; n < (size_t) (0.15 * sr) && start + n < out.size(); ++n)
            {
                const double t = n / sr;
                out[start + n] += (float) ((0.4 * noise (rng) + 0.3 * std::sin (2 * pi * 190 * t)) * std::exp (-t * 25.0));
            }
    }
}

// A chord is a list of MIDI notes; each held for `chordSeconds`.
static void addChords (std::vector<float>& out, double sr, const std::vector<std::vector<int>>& chords,
                       double chordSeconds, double a4 = 440.0, float gain = 0.08f)
{
    const auto chordLen = (size_t) (chordSeconds * sr);
    for (size_t start = 0, c = 0; start < out.size(); start += chordLen, ++c)
    {
        const auto& chord = chords[c % chords.size()];
        for (int note : chord)
        {
            const double f = a4 * std::pow (2.0, (note - 69) / 12.0);
            for (size_t n = 0; n < chordLen && start + n < out.size(); ++n)
            {
                const double t = n / sr;
                const double env = std::min (1.0, t / 0.02) * std::exp (-t * 0.6);
                double s = 0.0;
                for (int h = 1; h <= 6; ++h) // bright-ish harmonic tone
                    s += std::sin (2 * pi * f * h * t) / h;
                out[start + n] += gain * (float) (env * s);
            }
        }
    }
}

struct Outcome
{
    TempoResult tempo;
    KeyResult key;
    MeterResult meter;
};

static Outcome runEngine (const std::vector<float>& audio, double sr, double minBpm = 70, double maxBpm = 180)
{
    AnalysisEngine engine;
    engine.prepare (sr);
    engine.setTempoRange (minBpm, maxBpm);

    const int block = 512;
    const auto updateEvery = (size_t) (0.5 * sr);
    size_t sinceUpdate = 0;
    for (size_t i = 0; i < audio.size(); i += block)
    {
        const int n = (int) std::min<size_t> (block, audio.size() - i);
        engine.push (audio.data() + i, n);
        sinceUpdate += (size_t) n;
        if (sinceUpdate >= updateEvery)
        {
            engine.update();
            sinceUpdate = 0;
        }
    }
    engine.update();
    return { engine.getTempo(), engine.getKey(), engine.getMeter() };
}

// MIDI helpers
static std::vector<int> triad (int root, bool minor) { return { root, root + (minor ? 3 : 4), root + 7, root - 12 }; }

// --------------------------------------------------------------------------
// Tests
// --------------------------------------------------------------------------
static void testTheory()
{
    std::printf ("Music theory\n");
    using namespace theory;
    const int aMinor = makeKey (9, true), cMajor = makeKey (0, false);
    CHECK (relativeKey (aMinor) == cMajor, "relative of A minor should be C major\n");
    CHECK (relativeKey (makeKey (2, false)) == makeKey (11, true), "relative of D major should be B minor\n");
    CHECK (keyName (makeKey (3, false)) == "E♭ major", "got %s\n", keyName (makeKey (3, false)).c_str());
    CHECK (keyName (makeKey (6, true)) == "F♯ minor", "got %s\n", keyName (makeKey (6, true)).c_str());
    CHECK (keyName (makeKey (10, true)) == "B♭ minor", "got %s\n", keyName (makeKey (10, true)).c_str());
    CHECK (keySignature (makeKey (2, false)).summary == "2♯", "D major sig\n");
    CHECK (keySignature (makeKey (5, true)).summary == "4♭", "F minor sig %s\n", keySignature (makeKey (5, true)).summary.c_str());
    CHECK (keySignature (cMajor).count == 0, "C major sig\n");
    CHECK (keySignature (makeKey (6, false)).alternative == "6♭", "F# major alt\n");
    CHECK (enharmonicKeyName (makeKey (6, false)) == "G♭ major", "F# enharmonic\n");
    CHECK (camelot (aMinor) == "8A", "A minor camelot %s\n", camelot (aMinor).c_str());
    CHECK (camelot (cMajor) == "8B", "C major camelot\n");
    CHECK (camelot (makeKey (11, false)) == "1B", "B major camelot %s\n", camelot (makeKey (11, false)).c_str());
    CHECK (camelot (makeKey (7, true)) == "6A", "G minor camelot %s\n", camelot (makeKey (7, true)).c_str());
    const auto sig = keySignature (makeKey (4, false)); // E major
    CHECK (sig.accidentals.size() == 4 && sig.accidentals[3] == "D♯", "E major accidentals\n");
}

static void testTempo (double bpm, double sr, double minBpm = 70, double maxBpm = 180, double expected = 0)
{
    if (expected == 0)
        expected = bpm;
    std::vector<float> audio ((size_t) (20.0 * sr), 0.0f);
    addDrums (audio, sr, bpm, (unsigned) bpm);
    const auto r = runEngine (audio, sr, minBpm, maxBpm);
    std::printf ("  %6.2f BPM @ %5.0f Hz, range %3.0f-%3.0f -> %6.2f (conf %.2f)\n",
                 bpm, sr, minBpm, maxBpm, r.tempo.bpm, r.tempo.confidence);
    CHECK (r.tempo.valid, "tempo not valid\n");
    CHECK (std::abs (r.tempo.bpm - expected) < 0.15, "expected %.2f, got %.2f\n", expected, r.tempo.bpm);
}

static void testKey (const char* label, int expectedKey, const std::vector<std::vector<int>>& progression,
                     double sr = 44100.0, double a4 = 440.0, bool drums = false)
{
    std::vector<float> audio ((size_t) (24.0 * sr), 0.0f);
    addChords (audio, sr, progression, 2.0, a4);
    if (drums)
        addDrums (audio, sr, 122.0);
    const auto r = runEngine (audio, sr);
    std::printf ("  %-30s -> %-10s (conf %.2f, tuning %+5.1f c, runner-up %s)\n", label,
                 r.key.valid ? theory::keyName (r.key.key).c_str() : "-", r.key.confidence, r.key.tuningCents,
                 r.key.runnerUp >= 0 ? theory::keyName (r.key.runnerUp).c_str() : "-");
    CHECK (r.key.valid && r.key.key == expectedKey, "expected %s\n", theory::keyName (expectedKey).c_str());
}

static void testMeter (const char* label, double stepsPerMinute, const std::string& bar, double expectedBpm,
                       const char* expectedSig, bool withChords = false)
{
    const double sr = 44100.0;
    std::vector<float> audio ((size_t) (24.0 * sr), 0.0f);
    addGroove (audio, sr, stepsPerMinute, bar);
    if (withChords)
        addChords (audio, sr, { triad (60, false), triad (65, false), triad (67, false), triad (60, false) }, 2.0, 440.0, 0.05f);
    const auto r = runEngine (audio, sr);
    const auto sig = theory::TimeSignature { r.meter.numerator, r.meter.denominator }.text();
    std::printf ("  %-30s -> %6.2f BPM, %-5s (%s, conf %.2f)\n", label, r.tempo.bpm, r.meter.valid ? sig.c_str() : "-",
                 theory::meterDescription (r.meter.beatsPerBar, r.meter.compound).c_str(), r.meter.confidence);
    CHECK (std::abs (r.tempo.bpm - expectedBpm) < 0.2, "expected %.1f BPM\n", expectedBpm);
    CHECK (r.meter.valid && sig == expectedSig, "expected %s\n", expectedSig);
}

static void testSilence()
{
    std::printf ("Silence\n");
    std::vector<float> audio ((size_t) (10.0 * 44100), 0.0f);
    const auto r = runEngine (audio, 44100.0);
    CHECK (! r.tempo.valid, "tempo should be invalid on silence\n");
    CHECK (! r.key.valid, "key should be invalid on silence\n");
}

int main()
{
    using theory::makeKey;
    testTheory();

    std::printf ("Tempo\n");
    for (double bpm : { 72.0, 85.0, 90.0, 100.5, 120.0, 128.0, 140.0 })
        testTempo (bpm, 44100.0);
    // A backbeat at 160 is read as 160 (DnB / fast rock), not as an 80 BPM half-time feel.
    testTempo (160.0, 44100.0, 70, 180);
    testTempo (160.0, 44100.0, 100, 200);
    testTempo (174.0, 44100.0, 100, 200);
    testTempo (124.0, 48000.0);
    testTempo (97.0, 96000.0);
    testTempo (174.0, 44100.0, 60, 120, 87.0); // DnB in a half-time range
    testTempo (70.0, 44100.0, 100, 200, 140.0); // and the reverse

    std::printf ("Time signature\n");
    testMeter ("4/4 rock beat @ 120", 240, "KHSHKHSH", 120.0, "4/4");
    testMeter ("4/4 four-on-the-floor @ 126", 252, "KHXHKHXH", 126.0, "4/4");
    testMeter ("4/4 half-time @ 92 + chords", 184, "KHHHSHHH", 92.0, "4/4", true);
    testMeter ("3/4 waltz @ 150", 300, "KHSHSH", 150.0, "3/4");
    testMeter ("3/4 waltz @ 96", 192, "KHSHSH", 96.0, "3/4");
    testMeter ("3/4 waltz @ 108 + chords", 216, "KHSHSH", 108.0, "3/4", true);
    testMeter ("6/8 @ 80 (dotted quarter)", 240, "KHHSHH", 80.0, "6/8");
    testMeter ("6/8 @ 100 (dotted quarter)", 300, "KHHSHH", 100.0, "6/8");
    testMeter ("9/8 @ 90 (dotted quarter)", 270, "KHHSHHSHH", 90.0, "9/8");

    std::printf ("Key\n");
    // I - IV - V - I in C major
    testKey ("C major I-IV-V-I", makeKey (0, false), { triad (60, false), triad (65, false), triad (67, false), triad (60, false) });
    // i - iv - V - i in A minor (harmonic minor dominant)
    testKey ("A minor i-iv-V-i", makeKey (9, true), { triad (57, true), triad (62, true), triad (64, false), triad (57, true) });
    // i - VI - III - VII in A minor (natural minor, the classic pop loop)
    testKey ("A minor i-VI-III-VII-i-i", makeKey (9, true), { triad (57, true), triad (65, false), triad (60, false), triad (67, false), triad (57, true), triad (57, true) });
    // I - V - vi - IV in E♭ major
    testKey ("E♭ major I-V-vi-IV-I-I", makeKey (3, false), { triad (63, false), triad (58, false), triad (60, true), triad (56, false), triad (63, false), triad (63, false) });
    // I - vi - IV - V in F# major
    testKey ("F♯ major I-vi-IV-V-I-I", makeKey (6, false), { triad (66, false), triad (63, true), triad (59, false), triad (61, false), triad (66, false), triad (66, false) });
    // i - iv - v - i in F minor, with drums
    testKey ("F minor + drums", makeKey (5, true), { triad (53, true), triad (58, true), triad (60, true), triad (53, true) }, 44100.0, 440.0, true);
    // G major I-IV-V-I at 48 kHz with drums
    testKey ("G major + drums @48k", makeKey (7, false), { triad (55, false), triad (60, false), triad (62, false), triad (55, false) }, 48000.0, 440.0, true);
    // A minor tuned to A = 432 Hz
    testKey ("A minor @ A=432", makeKey (9, true), { triad (57, true), triad (62, true), triad (64, false), triad (57, true) }, 44100.0, 432.0);

    testSilence();

    std::printf ("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
