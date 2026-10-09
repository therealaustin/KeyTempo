#pragma once
// GrooveLab: synthesises genre-typical tracks (drums, bass, pads, arrangement with a
// breakdown) for benchmarking tempo detection. Deterministic for a given seed.
//
// Patterns are strings over 16th-note steps; each character is a velocity:
//   'X' = 1.0, 'x' = 0.65, 'o' = 0.35 (ghost), '.' = rest.

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <random>
#include <string>
#include <vector>

namespace groove
{
constexpr double pi = 3.14159265358979323846;

enum class Style
{
    House, TechHouse, Trance, ProgDotted, Techno,
    DnbTwoStep, DnbAmen, Neurofunk, Liquid, DnbHalftime,
    Dubstep, Trap, BoomBap, UKGarage, PopRock
};

inline const char* styleName (Style s)
{
    switch (s)
    {
        case Style::House:       return "House";
        case Style::TechHouse:   return "Tech house";
        case Style::Trance:      return "Trance";
        case Style::ProgDotted:  return "Prog (dotted perc)";
        case Style::Techno:      return "Techno";
        case Style::DnbTwoStep:  return "DnB two-step";
        case Style::DnbAmen:     return "DnB amen/jungle";
        case Style::Neurofunk:   return "Neurofunk";
        case Style::Liquid:      return "Liquid DnB";
        case Style::DnbHalftime: return "DnB halftime";
        case Style::Dubstep:     return "Dubstep";
        case Style::Trap:        return "Trap";
        case Style::BoomBap:     return "Boom bap";
        case Style::UKGarage:    return "UK garage";
        case Style::PopRock:     return "Pop/rock";
    }
    return "?";
}

struct Clip
{
    std::vector<float> audio;
    double sampleRate = 44100.0;
    double bpm = 120.0;
    Style style = Style::House;
};

// -----------------------------------------------------------------------------
class Synth
{
public:
    Synth (std::vector<float>& out, double sr, unsigned seed) : buf (out), sr (sr), rng (seed) {}

    float noise() { return dist (rng); }
    float rand01() { return 0.5f * (dist (rng) + 1.0f); }

    void add (size_t i, float v) { if (i < buf.size()) buf[i] += v; }

    void kick (double t, float vel, double tune = 1.0)
    {
        const auto s0 = (size_t) (t * sr);
        double ph = 0.0;
        for (size_t n = 0; n < (size_t) (0.35 * sr); ++n)
        {
            const double x = n / sr;
            ph += 2 * pi * tune * (45.0 + 110.0 * std::exp (-x * 35.0)) / sr;
            const double click = n < 40 ? 0.3 * noise() * (1.0 - n / 40.0) : 0.0;
            add (s0 + n, vel * (float) (0.9 * std::sin (ph) * std::exp (-x * 9.0) + click));
        }
    }

    void sub808 (double t, float vel, double freq, double length)
    {
        const auto s0 = (size_t) (t * sr);
        double ph = 0.0;
        for (size_t n = 0; n < (size_t) (length * sr); ++n)
        {
            const double x = n / sr;
            ph += 2 * pi * freq * (1.0 + 1.5 * std::exp (-x * 40.0)) / sr;
            const double env = std::min (1.0, x / 0.003) * std::exp (-x * 1.8) * (n > (size_t) ((length - 0.02) * sr) ? 0.0 : 1.0);
            add (s0 + n, vel * 0.8f * (float) (std::tanh (1.5 * std::sin (ph)) * env));
        }
    }

    void snare (double t, float vel)
    {
        const auto s0 = (size_t) (t * sr);
        float hp = 0, prev = 0;
        for (size_t n = 0; n < (size_t) (0.2 * sr); ++n)
        {
            const double x = n / sr;
            const float w = noise();
            hp = 0.7f * (hp + w - prev); prev = w;
            const double body = 0.35 * std::sin (2 * pi * 185.0 * x) * std::exp (-x * 30.0);
            add (s0 + n, vel * (float) ((0.55 * hp) * std::exp (-x * 18.0) + body));
        }
    }

    void clap (double t, float vel)
    {
        const auto s0 = (size_t) (t * sr);
        float bp = 0, lp = 0, prev = 0;
        for (size_t n = 0; n < (size_t) (0.25 * sr); ++n)
        {
            const double x = n / sr;
            const float w = noise();
            lp += 0.35f * (w - lp);
            bp = 0.8f * (bp + lp - prev); prev = lp;
            double env = std::exp (-x * 14.0);
            for (double burst : { 0.0, 0.011, 0.022 })
                if (x >= burst && x < burst + 0.008)
                    env = std::max (env, 1.4 * std::exp (-(x - burst) * 250.0));
            add (s0 + n, vel * 0.7f * (float) (bp * env));
        }
    }

    void hat (double t, float vel, double decay)
    {
        const auto s0 = (size_t) (t * sr);
        float prev = 0, hp = 0;
        for (size_t n = 0; n < (size_t) (decay * 6 * sr); ++n)
        {
            const float w = noise();
            hp = 0.55f * (hp + w - prev); prev = w;
            add (s0 + n, vel * 0.35f * hp * (float) std::exp (-(n / sr) / decay));
        }
    }

    void perc (double t, float vel, double freq, double decay = 0.06)
    {
        const auto s0 = (size_t) (t * sr);
        for (size_t n = 0; n < (size_t) (decay * 6 * sr); ++n)
        {
            const double x = n / sr;
            add (s0 + n, vel * 0.35f * (float) (std::sin (2 * pi * freq * x) * std::exp (-x / decay)));
        }
    }

    /** Filtered saw note with an attack time (soft attacks = no sharp onset). */
    void saw (double t, double length, double freq, float vel, double attack, double cutoffHz, double detune = 0.0)
    {
        const auto s0 = (size_t) (t * sr);
        const double a = 1.0 - std::exp (-2 * pi * cutoffHz / sr);
        double p1 = rand01(), p2 = rand01(), lp1 = 0, lp2 = 0;
        for (size_t n = 0; n < (size_t) (length * sr); ++n)
        {
            const double x = n / sr;
            p1 += freq / sr; p1 -= std::floor (p1);
            p2 += freq * (1.0 + detune) / sr; p2 -= std::floor (p2);
            const double s = (2 * p1 - 1) + (detune > 0 ? (2 * p2 - 1) : 0.0);
            lp1 += a * (s - lp1);
            lp2 += a * (lp1 - lp2);
            const double rel = std::min (1.0, (length - x) / 0.01);
            const double env = std::min (1.0, x / std::max (1e-4, attack)) * rel;
            add (s0 + n, vel * 0.25f * (float) (lp2 * env));
        }
    }

    std::vector<float>& buf;
    double sr;
    std::mt19937 rng;
    std::uniform_real_distribution<float> dist { -1.0f, 1.0f };
};

inline float velocity (char c)
{
    switch (c)
    {
        case 'X': return 1.0f;
        case 'x': return 0.65f;
        case 'o': return 0.35f;
        default:  return 0.0f;
    }
}

inline double midiToHz (double m) { return 440.0 * std::pow (2.0, (m - 69.0) / 12.0); }

// -----------------------------------------------------------------------------
struct Layers
{
    bool kick = true, snare = true, hats = true, perc = true, bass = true, pad = true, arp = false;
};

/** Plays a 16th-step pattern string starting at bar `bar`. */
inline void playPattern (const std::string& p, int bar, int barsInPattern, double stepSec, double swing,
                         const std::function<void (double, float)>& hit)
{
    const int stepsPerBar = 16;
    const int offset = (bar % barsInPattern) * stepsPerBar;
    for (int s = 0; s < stepsPerBar; ++s)
    {
        const char c = p[(size_t) ((offset + s) % (int) p.size())];
        const float v = velocity (c);
        if (v <= 0.0f)
            continue;
        const double swingDelay = (s % 2 == 1) ? swing * stepSec : 0.0;
        hit ((bar * stepsPerBar + s) * stepSec + swingDelay, v);
    }
}

/** Feedback delay (in seconds) mixed into a buffer: dotted-eighth echoes are a classic
    source of false 3:4 / 4:3 periodicities in house and trance. */
inline void applyDelay (std::vector<float>& x, double sr, double delaySec, float feedback, float wet)
{
    const auto d = (size_t) (delaySec * sr);
    std::vector<float> line (x.size(), 0.0f);
    for (size_t n = d; n < x.size(); ++n)
        line[n] = x[n - d] + feedback * line[n - d];
    for (size_t n = 0; n < x.size(); ++n)
        x[n] += wet * line[n];
}

/** Cheap Schroeder reverb: four combs + two allpasses. Smears onsets like a real mix. */
inline void applyReverb (std::vector<float>& x, double sr, float wet)
{
    const double combMs[4] = { 29.7, 37.1, 41.1, 43.7 };
    std::vector<float> out (x.size(), 0.0f);
    for (double ms : combMs)
    {
        const auto d = (size_t) (ms * 0.001 * sr * 1.7);
        std::vector<float> c (x.size(), 0.0f);
        for (size_t n = 0; n < x.size(); ++n)
            c[n] = x[n] + (n >= d ? 0.84f * c[n - d] : 0.0f);
        for (size_t n = 0; n < x.size(); ++n)
            out[n] += 0.25f * c[n];
    }
    for (double ms : { 5.0, 1.7 })
    {
        const auto d = (size_t) (ms * 0.001 * sr);
        std::vector<float> a (x.size(), 0.0f);
        for (size_t n = 0; n < x.size(); ++n)
            a[n] = -0.7f * out[n] + (n >= d ? out[n - d] + 0.7f * a[n - d] : 0.0f);
        out.swap (a);
    }
    for (size_t n = 0; n < x.size(); ++n)
        x[n] += wet * out[n];
}

/** Makes a ~40-bar arrangement: 8 intro (drums), 16 main, 8 breakdown (no drums), 8 drop.
    `hard` adds what real productions throw at a detector: dotted-eighth delays, reverb,
    triplet percussion, dropped kicks and snare rolls in the build-up. */
inline Clip make (Style style, double bpm, unsigned seed, double sr = 44100.0, bool hard = false)
{
    Clip clip;
    clip.bpm = bpm;
    clip.style = style;
    clip.sampleRate = sr;

    const double beat = 60.0 / bpm, step = beat / 4.0, barSec = beat * 4.0;
    const int bars = 40;
    clip.audio.assign ((size_t) ((bars * barSec + 1.0) * sr), 0.0f);

    std::vector<float> drums (clip.audio.size(), 0.0f), music (clip.audio.size(), 0.0f);
    Synth d (drums, sr, seed), m (music, sr, seed * 7 + 1);

    std::mt19937 rng (seed * 31 + 5);
    std::uniform_real_distribution<float> u (0.0f, 1.0f);
    auto chance = [&] (double p) { return u (rng) < p; };

    // Chord roots for the progression (minor-ish loop), varied per seed.
    const int key = 33 + (int) (seed % 12);
    const int prog[4] = { 0, -4, 3, -2 };

    double swing = 0.0;
    std::string kickP, snareP, clapP, hatP, ohatP, shakerP, percP, bassP;
    int patternBars = 1;
    bool fourOnFloorDucking = false, rollingBass = false, reese = false, use808 = false, dotted = false, amenFill = false;
    bool wobble = false, neuroStabs = false, arpInMain = false;

    switch (style)
    {
        case Style::House:
            kickP = "X...X...X...X..."; clapP = "....X.......X...";
            ohatP = "..X...X...X...X."; shakerP = "oooooooooooooooo";
            bassP = "..X...X...X..X.X"; percP = ".......o......o.";
            fourOnFloorDucking = true;
            break;
        case Style::TechHouse:
            kickP = "X...X...X...X..."; clapP = "....X.......X...";
            ohatP = "..x...x...x...x."; shakerP = "oxooxoooxoxooxoo";
            percP = "..x..x.x...x.x.." ; bassP = "..XX..X...XX..X.";
            fourOnFloorDucking = true;
            break;
        case Style::Trance:
            kickP = "X...X...X...X..."; clapP = "....X.......X...";
            ohatP = "..X...X...X...X."; hatP = "o.o.o.o.o.o.o.o.";
            rollingBass = true; fourOnFloorDucking = true; arpInMain = true;
            break;
        case Style::ProgDotted:
            // 4/4 kick against a dotted-eighth (3-step) percussion/bass ostinato and a
            // half-time clap: classic material for 3:2 tempo errors.
            kickP = "X...X...X...X..."; clapP = "........X.......";
            ohatP = "..x...x...x...x."; dotted = true; fourOnFloorDucking = true; arpInMain = true;
            break;
        case Style::Techno:
            kickP = "X...X...X...X..."; hatP = "xoxoxoxoxoxoxoxo";
            ohatP = "..X...X...X...X."; bassP = ".xx..xx..xx..xx.";
            for (int i = 0; i < 48; ++i) percP += (i % 3 == 0) ? 'x' : '.'; // 3-step loop over 3 bars
            patternBars = 3;
            fourOnFloorDucking = true;
            break;
        case Style::DnbTwoStep:
            kickP = "X.........X.....";  snareP = "....X.......X...";
            hatP = "x.x.x.x.x.x.x.x."; shakerP = "..o...o...o...o."; reese = true;
            break;
        case Style::DnbAmen:
            kickP  = "X.X.......XX....X.X.......X.....";
            snareP = "....X..X.X..X..X....X..X.X....X.";
            hatP   = "x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.x."; patternBars = 2; reese = true; amenFill = true;
            break;
        case Style::Neurofunk:
            kickP = "X.........X.....X.....X...X.....";
            snareP = "....X.......X.......X.......X...";
            hatP = "..x...x...x...x...x...x...x...x."; patternBars = 2;
            reese = true; neuroStabs = true;
            break;
        case Style::Liquid:
            kickP = "X.........X.....";  snareP = "....X..o....X..o";
            hatP = "x.x.x.x.x.x.x.x."; shakerP = "oooooooooooooooo"; reese = false;
            arpInMain = true;
            break;
        case Style::DnbHalftime:
            kickP = "X.....X...X.....";  snareP = "........X.......";
            hatP = "x.x.x.x.x.x.x.x."; reese = true;
            break;
        case Style::Dubstep:
            kickP = "X.........X.....";  snareP = "........X.......";
            hatP = "x.x.x.x.x.x.x.x."; ohatP = "......x.......x."; wobble = true;
            break;
        case Style::Trap:
            kickP = "X......X..X.....X.....X...X.....";  clapP = "........X...............X.......";
            hatP = "x.x.x.x.x.x.x.x.x.x.x.xxx.x.xxxx"; patternBars = 2; use808 = true;
            break;
        case Style::BoomBap:
            kickP = "X......X.X......";  snareP = "....X.......X...";
            hatP = "x.x.x.x.x.x.x.x."; swing = 0.16;
            break;
        case Style::UKGarage:
            kickP = "X.........X..X..";  snareP = "....X.......X...";
            hatP = "xoxoxoxoxoxoxoxo"; ohatP = "..x...x...x...x."; swing = 0.14;
            break;
        case Style::PopRock:
            kickP = "X.......X.X.....";  snareP = "....X.......X...";
            hatP = "x.x.x.x.x.x.x.x.";
            break;
    }

    for (int bar = 0; bar < bars; ++bar)
    {
        const bool intro = bar < 8, breakdown = bar >= 24 && bar < 32;
        Layers L;
        L.kick = ! breakdown;
        L.snare = ! breakdown && ! (intro && bar < 4);
        L.hats = ! breakdown;
        L.perc = ! breakdown && ! intro;
        L.bass = ! breakdown && ! intro;
        L.pad = ! intro;
        L.arp = breakdown || (arpInMain && ! intro);

        const double t0 = bar * barSec;
        const int chord = key + prog[(bar / 2) % 4];

        if (L.kick && ! kickP.empty())
            playPattern (kickP, bar, patternBars, step, swing, [&] (double t, float v) {
                if (use808) { d.kick (t, 0.6f * v); d.sub808 (t, v, midiToHz (chord - 12 + 12), beat * 0.9); }
                else d.kick (t, v);
            });
        if (L.snare && ! snareP.empty())
            playPattern (snareP, bar, patternBars, step, swing, [&] (double t, float v) { d.snare (t, 0.8f * v); });
        if (L.snare && ! clapP.empty())
            playPattern (clapP, bar, patternBars, step, swing, [&] (double t, float v) { d.clap (t, 0.8f * v); });
        if (L.hats && ! hatP.empty())
            playPattern (hatP, bar, patternBars, step, swing, [&] (double t, float v) { d.hat (t, v * (0.8f + 0.2f * d.rand01()), 0.012); });
        if (L.hats && ! ohatP.empty())
            playPattern (ohatP, bar, patternBars, step, swing, [&] (double t, float v) { d.hat (t, 0.8f * v, 0.06); });
        if (L.hats && ! shakerP.empty())
            playPattern (shakerP, bar, patternBars, step, swing, [&] (double t, float v) { d.hat (t, 0.5f * v, 0.02); });
        if (L.perc && ! percP.empty())
            playPattern (percP, bar, patternBars, step, swing, [&] (double t, float v) { d.perc (t, v, 820.0, 0.04); });

        // Random ghost snares and a fill in the last 16ths of every 8th bar.
        if (L.snare && (! snareP.empty() || ! clapP.empty()))
        {
            for (int s = 0; s < 16; ++s)
                if (chance (0.06))
                    d.snare (t0 + s * step, 0.25f);
            if (bar % 8 == 7)
                for (int s = 12; s < 16; ++s)
                    d.snare (t0 + s * step, 0.5f + 0.1f * (s - 12));
        }

        if (dotted && ! breakdown && ! intro)
            for (int s = 0; s < 16; ++s)
                if (((bar * 16 + s) % 3) == 0)
                {
                    d.perc (t0 + s * step, 0.9f, 620.0, 0.05);
                    m.saw (t0 + s * step, step * 1.5, midiToHz (chord + 12), 0.9f, 0.002, 900.0);
                }

        if (L.bass)
        {
            if (rollingBass)
                for (int s = 0; s < 16; ++s)
                    if (s % 4 != 0)
                        m.saw (t0 + s * step, step * 0.9, midiToHz (chord), 1.0f, 0.002, 600.0);
            if (! bassP.empty())
                playPattern (bassP, bar, 1, step, swing, [&] (double t, float v) { m.saw (t, step * 1.6, midiToHz (chord), v, 0.003, 500.0); });
            if (reese)
                m.saw (t0, barSec, midiToHz (chord - 12), 1.2f, 0.03, 400.0, 0.006);
            if (wobble)
            {
                // Sustained bass with an LFO "wobble" in 8th-note triplets.
                const auto s0 = (size_t) (t0 * sr);
                double ph = 0.0;
                const double f = midiToHz (chord - 12);
                for (size_t n = 0; n < (size_t) (barSec * sr); ++n)
                {
                    const double x = n / sr;
                    ph += f / sr; ph -= std::floor (ph);
                    const double lfo = 0.5 + 0.5 * std::sin (2 * pi * x * 3.0 / beat);
                    m.add (s0 + n, (float) (0.35 * lfo * std::tanh (3.0 * (2 * ph - 1))));
                }
            }
            if (neuroStabs)
                for (int s : { 2, 5, 7, 11, 13 })
                    m.saw (t0 + s * step, step * 0.8, midiToHz (chord + 7), 1.3f, 0.001, 2500.0, 0.01);
        }

        if (L.pad && bar % 2 == 0)
            for (int iv : { 12, 15, 19 })
                m.saw (t0, barSec * 2, midiToHz (chord + iv), 0.45f, 0.25, 1800.0, 0.004);

        if (L.arp)
            for (int s = 0; s < 16; ++s)
            {
                const int notes[4] = { 24, 27, 31, 36 };
                m.saw (t0 + s * step, step * 0.8, midiToHz (chord + notes[s % 4]), 0.35f, 0.002, 3000.0);
            }

        if (hard)
        {
            // Triplet percussion (3 against 4) through the main sections.
            if (L.perc)
                for (int k = 0; k < 12; ++k)
                    if (k % 2 == 0 || chance (0.3))
                        d.perc (t0 + k * beat / 3.0, 0.55f, 330.0 + 90.0 * (k % 3), 0.05);
            // Build-up: accelerating snare roll in the last 4 bars of the breakdown.
            if (breakdown && bar >= 28)
            {
                const int div = bar < 30 ? 4 : (bar < 31 ? 8 : 16);
                for (int k = 0; k < div; ++k)
                    d.snare (t0 + k * barSec / div, 0.3f + 0.5f * (float) k / (float) div);
            }
            // Occasionally drop kicks (DJ edits, broken sections).
            if (L.kick && chance (0.15))
                for (int s = 0; s < 16; s += 4)
                    if (chance (0.5))
                        d.perc (t0 + s * step, 0.7f, 95.0, 0.12); // a tom where a kick would be
        }

        if (amenFill && bar % 4 == 3 && ! breakdown)
            for (int s = 8; s < 16; s += 1)
                if (chance (0.5))
                    d.snare (t0 + s * step, 0.6f);
    }

    // Side-chain style ducking of the music under each kick (house/trance/techno).
    if (fourOnFloorDucking)
        for (size_t n = 0; n < music.size(); ++n)
        {
            const double t = n / sr;
            const double sinceBeat = std::fmod (t, beat);
            const int bar = (int) (t / barSec);
            const bool breakdown = bar >= 24 && bar < 32;
            if (! breakdown)
                music[n] *= (float) (1.0 - 0.6 * std::exp (-sinceBeat / (beat * 0.25)));
        }

    if (hard)
    {
        applyDelay (music, sr, beat * 0.75, 0.45f, 0.6f); // dotted-eighth echoes
        std::vector<float> hatsCopy = drums;
        applyDelay (hatsCopy, sr, beat * 0.75, 0.35f, 0.35f);
        drums.swap (hatsCopy);
        applyReverb (drums, sr, 0.25f);
        applyReverb (music, sr, 0.4f);
    }

    float peak = 1e-9f;
    for (size_t n = 0; n < clip.audio.size(); ++n)
    {
        clip.audio[n] = 0.8f * drums[n] + 0.7f * music[n];
        peak = std::max (peak, std::abs (clip.audio[n]));
    }
    for (auto& v : clip.audio)
        v = std::tanh (1.2f * v / peak) * 0.8f; // gentle "mastering" saturation
    return clip;
}

/** Benchmark set: each style at its typical tempos. */
struct Case
{
    Style style;
    double bpm;
};

inline std::vector<Case> benchmarkCases()
{
    std::vector<Case> cases;
    auto add = [&] (Style s, std::initializer_list<double> tempos) { for (double t : tempos) cases.push_back ({ s, t }); };
    add (Style::House,       { 118, 122, 124, 126, 128 });
    add (Style::TechHouse,   { 124, 125, 126, 128 });
    add (Style::Trance,      { 128, 132, 136, 138, 140 });
    add (Style::ProgDotted,  { 120, 124, 128, 132 });
    add (Style::Techno,      { 128, 132, 136, 140, 145 });
    add (Style::DnbTwoStep,  { 160, 168, 170, 172, 174, 176, 178 });
    add (Style::DnbAmen,     { 160, 165, 170, 174 });
    add (Style::Neurofunk,   { 172, 174, 175 });
    add (Style::Liquid,      { 170, 172, 174 });
    add (Style::DnbHalftime, { 170, 174 });
    add (Style::Dubstep,     { 138, 140, 142, 150 });
    add (Style::Trap,        { 130, 140, 145, 150 });
    add (Style::BoomBap,     { 85, 88, 90, 93, 96 });
    add (Style::UKGarage,    { 130, 132, 134, 136 });
    add (Style::PopRock,     { 100, 110, 120, 128 });
    return cases;
}
} // namespace groove
