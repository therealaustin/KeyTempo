#include "TrackAnalyzer.h"

#include "AnalysisEngine.h"
#include "Fft.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>
#ifdef KT_GRID_DEBUG
#include <cstdio>
#endif

namespace kt
{
namespace
{
    /** Full-band and kick-band onset envelopes for the whole track (~5.8 ms frames at 44.1 kHz). */
    struct Envelopes
    {
        std::vector<float> full, low, snare;
        std::vector<std::array<float, 24>> bands; // coarse log spectrum per frame (harmony / timbre)
        double rate = 0.0;
    };

    Envelopes onsetEnvelopes (const float* x, size_t n, double sr)
    {
        Envelopes e;
        const int size = Fft::nextPowerOfTwo ((int) std::lround (sr * 0.023));
        const int hop = size / 4;
        e.rate = sr / hop;
        if (n < (size_t) size)
            return e;

        Fft fft (size);
        const int maxBin = std::min (size / 2, (int) (11000.0 * size / sr));
        const int lowBin = std::max (2, (int) std::lround (150.0 * size / sr));
        const int snareLo = (int) std::lround (1500.0 * size / sr), snareHi = std::min (size / 2, (int) std::lround (8000.0 * size / sr));
        std::vector<float> mags, prevLog ((size_t) size / 2 + 1, 0.0f), prevLow ((size_t) size / 2 + 1, 0.0f);

        // 24 log-spaced bands from 80 Hz to 5 kHz
        std::array<int, 25> bandEdges {};
        for (int b = 0; b <= 24; ++b)
            bandEdges[(size_t) b] = std::clamp ((int) std::lround (80.0 * std::pow (5000.0 / 80.0, b / 24.0) * size / sr), 1, size / 2);
        const float scale = 2.0f / (float) size;

        const size_t frames = (n - (size_t) size) / (size_t) hop + 1;
        e.full.reserve (frames);
        e.low.reserve (frames);
        e.snare.reserve (frames);
        e.bands.reserve (frames);
        for (size_t f = 0; f < frames; ++f)
        {
            fft.magnitudeSpectrum (x + f * (size_t) hop, mags);
            float flux = 0.0f, low = 0.0f, snare = 0.0f;
            for (int k = 1; k <= maxBin; ++k)
            {
                const float m = mags[(size_t) k] * scale;
                const float l = std::log1p (1000.0f * m);
                if (f > 0)
                {
                    const float d = std::max (0.0f, l - prevLog[(size_t) k]);
                    flux += d;
                    if (k >= snareLo && k <= snareHi)
                        snare += d;
                    if (k <= lowBin)
                        low += std::max (0.0f, m - prevLow[(size_t) k]);
                }
                prevLog[(size_t) k] = l;
                if (k <= lowBin)
                    prevLow[(size_t) k] = m;
            }
            e.full.push_back (flux);
            e.low.push_back (low);
            e.snare.push_back (snare);

            std::array<float, 24> bands {};
            for (size_t b = 0; b < 24; ++b)
            {
                double sum = 0.0;
                const int lo = bandEdges[b], hi = std::max (lo + 1, bandEdges[b + 1]);
                for (int k = lo; k < hi; ++k)
                    sum += mags[(size_t) k] * scale;
                bands[b] = (float) std::log1p (100.0 * sum / (hi - lo));
            }
            e.bands.push_back (bands);
        }

        // Remove the local mean (~0.25 s) so loudness changes don't bias the grid.
        auto detrend = [&] (std::vector<float>& v)
        {
            const int half = std::max (1, (int) (e.rate * 0.125));
            const int m = (int) v.size();
            std::vector<double> prefix ((size_t) m + 1, 0.0);
            for (int i = 0; i < m; ++i)
                prefix[(size_t) i + 1] = prefix[(size_t) i] + v[(size_t) i];
            std::vector<float> out ((size_t) m);
            for (int i = 0; i < m; ++i)
            {
                const int a = std::max (0, i - half), b = std::min (m, i + half + 1);
                out[(size_t) i] = (float) std::max (0.0, v[(size_t) i] - (prefix[(size_t) b] - prefix[(size_t) a]) / (b - a));
            }
            v.swap (out);
        };
        detrend (e.full);
        detrend (e.low);
        detrend (e.snare);
        return e;
    }

    double interpolate (const std::vector<float>& v, double pos)
    {
        const int i = (int) pos;
        if (i < 0 || i + 1 >= (int) v.size())
            return 0.0;
        const double f = pos - i;
        return v[(size_t) i] * (1.0 - f) + v[(size_t) i + 1] * f;
    }
} // namespace

TrackAnalysis analyseTrack (const float* mono, size_t numSamples, double sampleRate, const TrackAnalysisOptions& options)
{
    TrackAnalysis out;
    out.durationSeconds = sampleRate > 0.0 ? (double) numSamples / sampleRate : 0.0;
    if (mono == nullptr || numSamples == 0 || sampleRate <= 0.0)
        return out;

    // ---- Tempo, key and meter: stream the whole track through the same engine the
    // plug-in uses, so the librarian and the plug-in always agree.
    AnalysisEngine engine;
    engine.prepare (sampleRate);
    engine.setUpdateInterval (options.updateIntervalSeconds);
    engine.setGenre (options.genre);

    const auto interval = (size_t) std::max (1.0, options.updateIntervalSeconds * sampleRate);
    const size_t block = 4096;
    size_t since = 0;
    for (size_t p = 0; p < numSamples; p += block)
    {
        const int n = (int) std::min (block, numSamples - p);
        engine.push (mono + p, n);
        if ((since += (size_t) n) >= interval)
        {
            engine.update();
            since = 0;
        }
    }
    engine.update();

    const auto& t = engine.getTempo();
    const auto& k = engine.getKey();
    const auto& m = engine.getMeter();

    out.key = k.valid ? k.key : -1;
    out.keyConfidence = k.valid ? k.confidence : 0.0f;
    out.tuningCents = k.tuningCents;
    out.meterValid = m.valid;
    out.numerator = m.numerator;
    out.denominator = m.denominator;
    out.meterConfidence = m.confidence;

    if (! t.valid || t.bpm <= 0.0)
        return out;

    out.valid = true;
    out.bpm = t.bpm;
    out.bpmConfidence = t.confidence;
    out.alternateBpm = t.alternateBpm;
    out.feel = t.feel;

    // ---- Beat grid: phase-lock a pulse train at the final BPM to the whole track, then
    // pick which beat of the bar is "1" from where the kick hits hardest.
    const auto env = onsetEnvelopes (mono, numSamples, sampleRate);
    if (env.full.size() < 16)
        return out;

    const double period = 60.0 * env.rate / out.bpm;

    // Phase: kick-led, because off-beat hats and basslines are often louder in the full
    // band than the kick (trance), which would put the grid half a beat off.
    double kickMean = 0.0, fullMean = 0.0;
    for (size_t i = 0; i < env.full.size(); ++i)
    {
        kickMean += env.low[i];
        fullMean += env.full[i];
    }
    const double kickWeight = kickMean > 1.0e-12 ? 1.5 * fullMean / kickMean : 0.0;

    double bestPhase = 0.0, bestScore = -1.0;
    for (double phase = 0.0; phase < period; phase += 0.25)
    {
        double s = 0.0;
        for (double pos = phase; pos < (double) env.full.size() - 1; pos += period)
            s += interpolate (env.full, pos) + kickWeight * interpolate (env.low, pos);
        if (s > bestScore)
        {
            bestScore = s;
            bestPhase = phase;
        }
    }

    // Downbeat: beat 1 of the bar is where the kick is strong, the snare/clap is absent
    // (it marks 2 & 4, or 3 in half-time) and chords / basslines change.
    // DJ beat grids are 4/4 unless the track is confidently in three (waltz-time).
    const int beatsPerBar = (m.valid && m.beatsPerBar == 3 && ! m.compound && m.confidence > 0.5f) ? 3 : 4;
    const auto bpb = (size_t) beatsPerBar;
    std::vector<double> kick (bpb, 0.0), snare (bpb, 0.0), change (bpb, 0.0);
    std::vector<int> counts (bpb, 0);
    const int win = std::max (1, (int) std::lround (0.03 * env.rate)); // +-30 ms around each beat
    auto peakAround = [&] (const std::vector<float>& v, double pos)
    {
        double best = 0.0;
        for (int d = -win; d <= win; ++d)
            best = std::max (best, interpolate (v, pos + d));
        return best;
    };
    auto meanSpectrum = [&] (double from, double to)
    {
        std::array<double, 24> acc {};
        int count = 0;
        for (int i = std::max (0, (int) from); i < std::min ((int) env.bands.size(), (int) to); ++i, ++count)
            for (size_t b = 0; b < 24; ++b)
                acc[b] += env.bands[(size_t) i][b];
        if (count > 0)
            for (auto& v : acc)
                v /= count;
        return acc;
    };

    int beatIndex = 0;
    for (double pos = bestPhase; pos < (double) env.full.size() - 1; pos += period, ++beatIndex)
    {
        const auto slot = (size_t) (beatIndex % beatsPerBar);
        kick[slot] += peakAround (env.low, pos);
        snare[slot] += peakAround (env.snare, pos);
        if (pos > period)
        {
            const auto before = meanSpectrum (pos - period, pos);
            const auto after = meanSpectrum (pos, pos + period);
            double d = 0.0;
            for (size_t b = 0; b < 24; ++b)
                d += std::abs (after[b] - before[b]);
            change[slot] += d;
        }
        ++counts[slot];
    }

    // Normalise each cue to mean 1 across the bar positions, then combine.
    auto normalised = [&] (std::vector<double> v)
    {
        double mean = 0.0;
        for (double x : v)
            mean += x;
        mean /= (double) v.size();
        for (auto& x : v)
            x = mean > 1.0e-12 ? x / mean : 1.0;
        return v;
    };
    const auto kN = normalised (kick), sN = normalised (snare), cN = normalised (change);
    std::vector<double> score (bpb);
    // The detected feel says where the snare sits relative to beat 1: one beat after it
    // (backbeat: 2 & 4) or two beats after it (half-time: 3).
    const size_t snareOffset = (out.feel == Feel::Halftime) ? 2 : ((out.feel == Feel::Backbeat || out.feel == Feel::Breakbeat) ? 1 : 0);
    for (size_t r = 0; r < bpb; ++r)
    {
        score[r] = kN[r] - 0.8 * sN[r] + 1.2 * cN[r];
        if (snareOffset > 0 && bpb == 4)
            score[r] += 1.0 * sN[(r + snareOffset) % bpb];
    }

#ifdef KT_GRID_DEBUG
    for (size_t r = 0; r < bpb; ++r)
        std::fprintf (stderr, "  beat %zu: kick %.2f snare %.2f change %.2f -> %.2f (feel %s)\n", r + 1, kN[r], sN[r], cN[r], score[r], feelName (out.feel));
#endif
    const auto bestIt = std::max_element (score.begin(), score.end());
    const int downbeat = (int) (bestIt - score.begin());
    double second = -1.0e9;
    for (size_t r = 0; r < bpb; ++r)
        if ((int) r != downbeat)
            second = std::max (second, score[r]);
    out.downbeatConfidence = (float) std::clamp ((*bestIt - second) / 1.0, 0.0, 1.0);

    // Envelope frame i covers samples [i*hop, i*hop+size); onsets register at the frame
    // whose window first contains them, so frame time ~= start + half the window.
    const double frameOffset = 0.5 * (double) Fft::nextPowerOfTwo ((int) std::lround (sampleRate * 0.023)) / sampleRate;
    const double beatSeconds = 60.0 / out.bpm;
    double first = (bestPhase / env.rate) + frameOffset + downbeat * beatSeconds;
    const double barSeconds = beatSeconds * beatsPerBar;
    first = std::fmod (first, barSeconds);
    if (first < 0.0)
        first += barSeconds;
    out.firstBeatSeconds = first;
    return out;
}
} // namespace kt
