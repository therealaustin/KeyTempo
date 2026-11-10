#include "TrackAnalyzer.h"

#include "AnalysisEngine.h"
#include "Fft.h"
#include "KeyDetector.h"

#include <algorithm>
#include <array>
#include <memory>
#include <cmath>
#include <vector>
#if defined (KT_GRID_DEBUG) || defined (KT_SEG_DEBUG)
#include <cstdio>
#endif

namespace kt
{
namespace
{
    /** Full-band, kick-band and snare-band onset envelopes plus a coarse spectrum per
        frame (~5.8 ms frames at 44.1 kHz), built incrementally as audio streams in. */
    struct Envelopes
    {
        std::vector<float> full, low, snare;
        std::vector<std::array<float, 24>> bands; // coarse log spectrum per frame (harmony / timbre)
        double rate = 0.0;
    };

    class EnvelopeBuilder
    {
    public:
        explicit EnvelopeBuilder (double sampleRate)
            : sr (sampleRate),
              size (Fft::nextPowerOfTwo ((int) std::lround (sampleRate * 0.023))),
              hop (size / 4),
              fft (size)
        {
            e.rate = sr / hop;
            maxBin = std::min (size / 2, (int) (11000.0 * size / sr));
            lowBin = std::max (2, (int) std::lround (150.0 * size / sr));
            snareLo = (int) std::lround (1500.0 * size / sr);
            snareHi = std::min (size / 2, (int) std::lround (8000.0 * size / sr));
            for (int b = 0; b <= 24; ++b)
                bandEdges[(size_t) b] = std::clamp ((int) std::lround (80.0 * std::pow (5000.0 / 80.0, b / 24.0) * size / sr), 1, size / 2);
            prevLog.assign ((size_t) size / 2 + 1, 0.0f);
            prevLow.assign ((size_t) size / 2 + 1, 0.0f);
            ring.assign ((size_t) size, 0.0f);
            frame.assign ((size_t) size, 0.0f);
            untilFrame = size; // first frame once a full window has arrived
        }

        int windowSize() const noexcept { return size; }

        void push (const float* x, int n)
        {
            for (int i = 0; i < n; ++i)
            {
                ring[(size_t) write] = x[i];
                write = (write + 1) % size;
                if (--untilFrame == 0)
                {
                    processFrame();
                    untilFrame = hop;
                }
            }
        }

        Envelopes finish()
        {
            detrend (e.full);
            detrend (e.low);
            detrend (e.snare);
            return std::move (e);
        }

    private:
        void processFrame()
        {
            for (int i = 0; i < size; ++i)
                frame[(size_t) i] = ring[(size_t) ((write + i) % size)];
            fft.magnitudeSpectrum (frame.data(), mags);

            const float scale = 2.0f / (float) size;
            float flux = 0.0f, low = 0.0f, snare = 0.0f;
            const bool first = e.full.empty();
            for (int k = 1; k <= maxBin; ++k)
            {
                const float m = mags[(size_t) k] * scale;
                const float l = std::log1p (1000.0f * m);
                if (! first)
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

        /** Remove the local mean (~0.25 s) so loudness changes don't bias the grid. */
        void detrend (std::vector<float>& v) const
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
        }

        double sr;
        int size, hop;
        Fft fft;
        int maxBin = 0, lowBin = 0, snareLo = 0, snareHi = 0;
        std::array<int, 25> bandEdges {};
        std::vector<float> prevLog, prevLow, ring, frame, mags;
        int write = 0, untilFrame = 0;
        Envelopes e;
    };

    double interpolate (const std::vector<float>& v, double pos)
    {
        const int i = (int) pos;
        if (i < 0 || i + 1 >= (int) v.size())
            return 0.0;
        const double f = pos - i;
        return v[(size_t) i] * (1.0 - f) + v[(size_t) i + 1] * f;
    }

    /** Where the grid lands for one tempo over frames [from, to): a beat position (frames)
        and which beat after it is the downbeat. */
    struct GridLock
    {
        double phase = 0.0; // frame of a beat
        int downbeat = 0;   // beats after `phase` to the first downbeat
        float downbeatConfidence = 0.0f;
        std::vector<double> scores; // per beat of the bar, starting at `phase`
    };

    GridLock lockGrid (const Envelopes& env, double bpm, Feel feel, int beatsPerBar, int from, int to)
    {
        GridLock lock;
        const double period = 60.0 * env.rate / bpm;

        // Phase: kick-led, because off-beat hats and basslines are often louder in the full
        // band than the kick (trance), which would put the grid half a beat off. Broken beats
        // (amen, neurofunk, half-time DnB) are the opposite case: most of their kicks fall on
        // off-beat 8ths while the snares and the full band mark the beat, so a strong kick
        // weight pulls the grid half a beat late there. Those feels keep only a light kick cue.
        const bool brokenBeat = feel == Feel::Breakbeat || feel == Feel::Halftime;
        double kickMean = 0.0, fullMean = 0.0;
        for (size_t i = (size_t) from; i < (size_t) to; ++i)
        {
            kickMean += env.low[i];
            fullMean += env.full[i];
        }
        const double kickWeight = kickMean > 1.0e-12 ? (brokenBeat ? 0.5 : 1.5) * fullMean / kickMean : 0.0;

        double bestPhase = 0.0, bestScore = -1.0;
        for (double phase = 0.0; phase < period; phase += 0.25)
        {
            double s = 0.0;
            for (double pos = from + phase; pos < (double) to - 1; pos += period)
                s += interpolate (env.full, pos) + kickWeight * interpolate (env.low, pos);
            if (s > bestScore)
            {
                bestScore = s;
                bestPhase = phase;
            }
        }

        // Downbeat: beat 1 of the bar is where the kick is strong, the snare/clap is absent
        // (it marks 2 & 4, or 3 in half-time) and chords / basslines change.
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
        auto meanSpectrum = [&] (double a, double b)
        {
            std::array<double, 24> acc {};
            int count = 0;
            for (int i = std::max (0, (int) a); i < std::min ((int) env.bands.size(), (int) b); ++i, ++count)
                for (size_t k = 0; k < 24; ++k)
                    acc[k] += env.bands[(size_t) i][k];
            if (count > 0)
                for (auto& v : acc)
                    v /= count;
            return acc;
        };

        int beatIndex = 0;
        for (double pos = from + bestPhase; pos < (double) to - 1; pos += period, ++beatIndex)
        {
            const auto slot = (size_t) (beatIndex % beatsPerBar);
            kick[slot] += peakAround (env.low, pos);
            snare[slot] += peakAround (env.snare, pos);
            if (pos > from + period)
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
        const size_t snareOffset = (feel == Feel::Halftime) ? 2 : ((feel == Feel::Backbeat || feel == Feel::Breakbeat) ? 1 : 0);
        for (size_t r = 0; r < bpb; ++r)
        {
            score[r] = kN[r] - 0.8 * sN[r] + 1.2 * cN[r];
            if (snareOffset > 0 && bpb == 4)
                score[r] += 1.0 * sN[(r + snareOffset) % bpb];
        }

#ifdef KT_GRID_DEBUG
        for (size_t r = 0; r < bpb; ++r)
            std::fprintf (stderr, "  beat %zu: kick %.2f snare %.2f change %.2f -> %.2f (feel %s)\n", r + 1, kN[r], sN[r], cN[r], score[r], feelName (feel));
#endif
        const auto bestIt = std::max_element (score.begin(), score.end());
        lock.downbeat = (int) (bestIt - score.begin());
        double second = -1.0e9;
        for (size_t r = 0; r < bpb; ++r)
            if ((int) r != lock.downbeat)
                second = std::max (second, score[r]);
        lock.downbeatConfidence = (float) std::clamp ((*bestIt - second) / 1.0, 0.0, 1.0);
        lock.phase = from + bestPhase;
        lock.scores = score;
        return lock;
    }

    /** Weighted median of (value, weight) pairs. */
    double weightedMedian (std::vector<std::pair<double, double>> v)
    {
        if (v.empty())
            return 0.0;
        std::sort (v.begin(), v.end());
        double half = 0.0;
        for (auto& x : v)
            half += x.second;
        half *= 0.5;
        for (auto& x : v)
            if ((half -= x.second) <= 0.0)
                return x.first;
        return v.back().first;
    }

    /** Relative major / minor pairs share a class: a track moving between them hasn't
        changed key for splitting purposes. */
    int keyClass (int key) { return key >= 12 ? (key % 12 + 3) % 12 : key; }

    /** Keys a detector easily mistakes for each other: the same Camelot number (relative),
        a neighbour on the wheel (a fifth away) and the parallel key. A move between them is
        not reported as a key change; modulations (up a semitone or a tone) are far apart. */
    bool closelyRelated (int a, int b)
    {
        auto number = [] (int key) { return ((key % 12) * 7 + (key >= 12 ? 5 : 8) - 1) % 12 + 1; };
        const int d = std::abs (number (a) - number (b)), wheel = std::min (d, 12 - d);
        if (wheel <= 1)
            return true;
        const bool parallel = (a % 12) == (b % 12);
        return parallel;
    }
} // namespace

struct TrackAnalyzer::Impl
{
    Impl (double sampleRate, const TrackAnalysisOptions& o)
        : sr (sampleRate), options (o), envelopes (sampleRate),
          interval ((size_t) std::max (1.0, o.updateIntervalSeconds * sampleRate))
    {
        engine.prepare (sampleRate);
        engine.setUpdateInterval (o.updateIntervalSeconds);
        engine.setGenre (o.genre);
        if (options.detectChanges)
        {
            // No memory decay: its evidence is a running sum, so the difference between two
            // snapshots is the key evidence of the audio between them.
            keyTrack.prepare (sampleRate);
            keyTrack.setMemorySeconds (0.0);
        }
    }

    void push (const float* mono, int n)
    {
        total += (size_t) n;
        envelopes.push (mono, n);
        // Feed the engine in update-sized pieces so it re-estimates on schedule.
        while (n > 0)
        {
            const int chunk = (int) std::min ((size_t) n, interval - since);
            engine.push (mono, chunk);
            if (options.detectChanges)
                pushKey (mono, chunk);
            mono += chunk;
            n -= chunk;
            fed += (size_t) chunk;
            if ((since += (size_t) chunk) >= interval)
            {
                engine.update();
                record();
                since = 0;
            }
        }
    }

    /** Keeps what the segment search needs from each update: the tempo reading (dated to
        the middle of the detector's 12 s window) and a key evidence snapshot. */
    void record()
    {
        if (! options.detectChanges)
            return;
        const double now = (double) fed / sr;
        const auto& r = engine.getLatestReading();
        if (r.valid && r.bpm > 0.0)
        {
            // The same weighting the engine's tempo memory gives each reading.
            const double w = (0.1 + r.confidence) * (0.02 + r.grooveFit * r.grooveFit) * (0.1 + 0.9 * r.drumPresence);
            readings.push_back ({ now - 0.5 * std::min (12.0, now), r.bpm, w, r.dupleScore, r.tripleScore, r.compoundScore });
        }
    }

    /** Key evidence snapshots every ~0.3 s, so a key change can be placed to the bar. */
    void pushKey (const float* mono, int n)
    {
        const auto hop = (size_t) std::max (1.0, 0.3 * sr);
        while (n > 0)
        {
            const int chunk = (int) std::min ((size_t) n, hop - keySince);
            keyTrack.push (mono, chunk);
            mono += chunk;
            n -= chunk;
            keyFed += (size_t) chunk;
            if ((keySince += (size_t) chunk) >= hop)
            {
                keySnaps.emplace_back ((double) keyFed / sr, keyTrack.evidence());
                keySince = 0;
            }
        }
    }

    TrackAnalysis finish();
    void findTempoSegments (TrackAnalysis& out, const Envelopes& env, Feel feel, int beatsPerBar, const GridLock& wholeTrack, double frameOffset);
    void findKeySegments (TrackAnalysis& out, int beatsPerBar);
    void findMeterSegments (TrackAnalysis& out, const Envelopes& env, double frameOffset);

    struct Reading
    {
        double time, bpm, weight;
        float duple, triple, compound; // the detector's bar grouping and subdivision evidence
    };

    double sr;
    TrackAnalysisOptions options;
    AnalysisEngine engine;
    KeyDetector keyTrack;
    EnvelopeBuilder envelopes;
    size_t interval, since = 0, total = 0, fed = 0, keySince = 0, keyFed = 0;
    std::vector<Reading> readings;
    std::vector<std::pair<double, KeyEvidence>> keySnaps;
};

TrackAnalyzer::TrackAnalyzer (double sampleRate, const TrackAnalysisOptions& options)
    : impl (std::make_unique<Impl> (sampleRate > 0.0 ? sampleRate : 44100.0, options)) {}
TrackAnalyzer::~TrackAnalyzer() = default;

void TrackAnalyzer::push (const float* mono, int numSamples)
{
    if (mono != nullptr && numSamples > 0)
        impl->push (mono, numSamples);
}

TrackAnalysis TrackAnalyzer::finish() { return impl->finish(); }

TrackAnalysis analyseTrack (const float* mono, size_t numSamples, double sampleRate, const TrackAnalysisOptions& options)
{
    if (mono == nullptr || numSamples == 0 || sampleRate <= 0.0)
        return {};
    TrackAnalyzer analyzer (sampleRate, options);
    for (size_t p = 0; p < numSamples; p += 65536)
        analyzer.push (mono + p, (int) std::min<size_t> (65536, numSamples - p));
    return analyzer.finish();
}

TrackAnalysis TrackAnalyzer::Impl::finish()
{
    TrackAnalysis out;
    out.durationSeconds = (double) total / sr;
    if (total == 0)
        return out;
    engine.update();
    record();
    const double sampleRate = sr;

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
    if (out.key >= 0)
        out.keySegments = { { 0.0, out.key, out.keyConfidence } };

    // DJ beat grids are 4/4 unless the track is confidently in three (waltz-time).
    const int beatsPerBar = (m.valid && m.beatsPerBar == 3 && ! m.compound && m.confidence > 0.5f) ? 3 : 4;

    if (! t.valid || t.bpm <= 0.0)
    {
        findKeySegments (out, beatsPerBar);
        return out;
    }

    out.valid = true;
    out.bpm = t.bpm;
    out.bpmConfidence = t.confidence;
    out.alternateBpm = t.alternateBpm;
    out.feel = t.feel;

    // ---- Beat grid: phase-lock a pulse train at the final BPM to the whole track, then
    // pick which beat of the bar is "1" from where the kick hits hardest.
    const auto env = envelopes.finish();
    if (env.full.size() < 16)
        return out;

    const auto lock = lockGrid (env, out.bpm, out.feel, beatsPerBar, 0, (int) env.full.size());
    out.downbeatConfidence = lock.downbeatConfidence;

    // Envelope frame i covers samples [i*hop, i*hop+size); onsets register at the frame
    // whose window first contains them, so frame time ~= start + half the window.
    const double frameOffset = 0.5 * (double) envelopes.windowSize() / sampleRate;
    const double beatSeconds = 60.0 / out.bpm;
    double first = (lock.phase / env.rate) + frameOffset + lock.downbeat * beatSeconds;
    const double barSeconds = beatSeconds * beatsPerBar;
    first = std::fmod (first, barSeconds);
    if (first < 0.0)
        first += barSeconds;
    out.firstBeatSeconds = first;

    out.tempoSegments = { { out.firstBeatSeconds, out.bpm, out.bpmConfidence, 1, false } };
    out.bpmMin = out.bpmMax = out.bpm;
    findTempoSegments (out, env, out.feel, beatsPerBar, lock, frameOffset);
    for (auto& seg : out.tempoSegments)
    {
        seg.numerator = out.numerator;
        seg.denominator = out.denominator;
    }
    findMeterSegments (out, env, frameOffset);
    findKeySegments (out, beatsPerBar);
    return out;
}

// ---- Tempo changes ------------------------------------------------------------------
//
// Steady tracks keep the whole-track result above untouched. A track only splits when the
// per-update readings, folded into the track's tempo family (so half / double time and
// 3:2 readings never count), hold a clearly different tempo for minSegmentBars, and the
// onsets confirm it: each section must fit its own tempo clearly better than the other's.
void TrackAnalyzer::Impl::findTempoSegments (TrackAnalysis& out, const Envelopes& env, Feel feel, int beatsPerBar,
                                             const GridLock& wholeTrack, double frameOffset)
{
    (void) wholeTrack;
    if (! options.detectChanges || readings.size() < 8)
        return;
    const double global = out.bpm;
    const double rate = env.rate;
    const int frames = (int) env.full.size();
    auto toFrame = [&] (double seconds) { return (seconds - frameOffset) * rate; };
    auto toSeconds = [&] (double frame) { return frame / rate + frameOffset; };

    // 1. Confident readings, folded into the family of the whole-track tempo.
    std::vector<double> weights;
    for (auto& r : readings)
        weights.push_back (r.weight);
    std::sort (weights.begin(), weights.end());
    const double threshold = 0.2 * weights[weights.size() * 4 / 5];
    struct Point { double time, bpm, weight, smooth; };
    std::vector<Point> pts;
    for (auto& r : readings)
    {
        if (r.weight < threshold)
            continue;
        double ratio = r.bpm / global;
        while (ratio > 1.42)
            ratio *= 0.5;
        while (ratio < 0.707)
            ratio *= 2.0;
        if (std::abs (ratio - 4.0 / 3.0) < 0.02)
            ratio *= 0.75; // a 3:2 reading folded an octave
        else if (std::abs (ratio - 0.75) < 0.015)
            ratio /= 0.75;
        pts.push_back ({ r.time, ratio * global, r.weight, 0.0 });
    }
    if (pts.size() < 8)
        return;
    // Median over +-6 s, so a fill or a misread window can't start a section.
    for (auto& p : pts)
    {
        std::vector<std::pair<double, double>> near;
        for (auto& q : pts)
            if (std::abs (q.time - p.time) <= 6.0)
                near.emplace_back (q.bpm, q.weight);
        p.smooth = weightedMedian (near);
    }

    // 2. Steady runs: consecutive readings within 0.7 % of the run's median (a few
    // stray readings are skipped).
    struct Region
    {
        size_t first = 0, last = 0; // indices into pts
        double bpm = 0.0;           // median reading
        double fine = 0.0;    // precise tempo from the onsets
        GridLock lock;
        double fromFrame = 0.0, toFrame = 0.0; // what the precise tempo was measured over
        float fitMargin = 0.0f;
    };
    auto runMedian = [&] (size_t a, size_t b)
    {
        std::vector<std::pair<double, double>> v;
        for (size_t i = a; i <= b; ++i)
            if (std::abs (pts[i].smooth / pts[a].smooth - 1.0) < 0.05)
                v.emplace_back (pts[i].bpm, pts[i].weight);
        return weightedMedian (v);
    };
    std::vector<Region> regions;
    for (size_t i = 0; i < pts.size();)
    {
        double ref = pts[i].smooth;
        size_t last = i;
        std::vector<std::pair<double, double>> members { { pts[i].smooth, pts[i].weight } };
        int misses = 0;
        for (size_t j = i + 1; j < pts.size(); ++j)
        {
            if (std::abs (pts[j].smooth / ref - 1.0) <= 0.007)
            {
                members.emplace_back (pts[j].smooth, pts[j].weight);
                ref = weightedMedian (members);
                last = j;
                misses = 0;
            }
            else if (++misses > 3)
                break;
        }
        const double span = pts[last].time - pts[i].time + 12.0; // the readings' windows
        const double minSeconds = 0.85 * options.minSegmentBars * beatsPerBar * 60.0 / ref;
        if (span >= minSeconds)
        {
            Region region;
            region.first = i;
            region.last = last;
            region.bpm = ref;
            regions.push_back (region);
        }
        i = last + 1;
    }

    // Neighbours at (nearly) the same tempo are one section: a breakdown between them
    // or a slow drift isn't a change.
    auto mergeClose = [&]
    {
        for (size_t r = 0; r + 1 < regions.size();)
            if (std::abs (regions[r + 1].bpm / regions[r].bpm - 1.0) < 0.015)
            {
                regions[r].last = regions[r + 1].last;
                regions[r].bpm = runMedian (regions[r].first, regions[r].last);
                regions.erase (regions.begin() + (long) r + 1);
            }
            else
                ++r;
    };
    mergeClose();
    if (regions.size() < 2)
        return;

    // 3. Precise tempo of each section from the onsets (full band plus kick, as the grid
    // uses), over the middle of its readings.
    std::vector<float> onset ((size_t) frames);
    {
        double kickMean = 0.0, fullMean = 0.0;
        for (int i = 0; i < frames; ++i)
        {
            kickMean += env.low[(size_t) i];
            fullMean += env.full[(size_t) i];
        }
        const bool brokenBeat = feel == Feel::Breakbeat || feel == Feel::Halftime;
        const double kw = kickMean > 1.0e-12 ? (brokenBeat ? 0.5 : 1.5) * fullMean / kickMean : 0.0;
        for (int i = 0; i < frames; ++i)
            onset[(size_t) i] = (float) (env.full[(size_t) i] + kw * env.low[(size_t) i]);
    }
    auto alignment = [&] (double bpm, double from, double to)
    {
        const double period = 60.0 * rate / bpm;
        double best = 0.0;
        for (double phase = 0.0; phase < period; phase += 0.5)
        {
            double sum = 0.0;
            int count = 0;
            for (double pos = from + phase; pos < to - 1; pos += period, ++count)
                sum += interpolate (onset, pos);
            if (count > 0)
                best = std::max (best, sum / count);
        }
        return best;
    };
    auto fineTempo = [&] (double approx, double from, double to)
    {
        double best = approx, bestA = alignment (approx, from, to);
        for (double b = approx * 0.988; b <= approx * 1.012; b += 0.02)
            if (const double a = alignment (b, from, to); a > bestA)
            {
                bestA = a;
                best = b;
            }
        const double centre = best;
        for (double b = centre - 0.03; b <= centre + 0.03; b += 0.002)
            if (const double a = alignment (b, from, to); a > bestA)
            {
                bestA = a;
                best = b;
            }
        return best;
    };
    auto bestNear = [&] (double bpm, double from, double to)
    {
        double best = 0.0;
        for (double b = bpm * 0.997; b <= bpm * 1.003; b += 0.02)
            best = std::max (best, alignment (b, from, to));
        return best;
    };
    auto measure = [&] (Region& r)
    {
        r.fromFrame = std::max (0.0, toFrame (pts[r.first].time - 3.0));
        r.toFrame = std::min ((double) frames, toFrame (pts[r.last].time + 3.0));
        r.fine = fineTempo (r.bpm, r.fromFrame, r.toFrame);
    };
    for (auto& r : regions)
        measure (r);

    // 4. Each section must fit its own tempo clearly better than its neighbour's; if not,
    // the readings were wrong and the two are one section.
    for (bool changed = true; changed && regions.size() >= 2;)
    {
        changed = false;
        for (size_t r = 0; r + 1 < regions.size(); ++r)
        {
            auto& a = regions[r];
            auto& b = regions[r + 1];
            const double own_a = alignment (a.fine, a.fromFrame, a.toFrame), other_a = bestNear (b.fine, a.fromFrame, a.toFrame);
            const double own_b = alignment (b.fine, b.fromFrame, b.toFrame), other_b = bestNear (a.fine, b.fromFrame, b.toFrame);
            const bool distinct = std::abs (b.fine / a.fine - 1.0) >= 0.015;
            if (distinct && own_a > 1.1 * other_a && own_b > 1.1 * other_b)
            {
                a.fitMargin = std::max (a.fitMargin, (float) (own_a / other_a));
                b.fitMargin = (float) (own_b / other_b);
                continue;
            }
            a.last = b.last;
            a.bpm = runMedian (a.first, a.last);
            regions.erase (regions.begin() + (long) r + 1);
            measure (regions[r]);
            changed = true;
            break;
        }
    }
    if (regions.size() < 2)
        return;

    // 5. Grid of each section, and the dominant (longest) one's downbeat.
    size_t dominant = 0;
    for (size_t r = 0; r < regions.size(); ++r)
    {
        auto& g = regions[r];
        g.lock = lockGrid (env, g.fine, feel, beatsPerBar, (int) g.fromFrame, (int) g.toFrame);
        if (g.toFrame - g.fromFrame > regions[dominant].toFrame - regions[dominant].fromFrame)
            dominant = r;
    }

    // 6. The change between neighbours: a jump (the next section's grid takes over at a
    // beat) or a ramp (the beat period glides from one tempo to the other over N beats),
    // whichever lands more beats on onsets.
    const int win = std::max (1, (int) std::lround (0.011 * rate));
    auto onsetAt = [&] (double pos)
    {
        double best = 0.0;
        for (int d = -win; d <= win; ++d)
            best = std::max (best, interpolate (onset, pos + d));
        return best;
    };
    struct Marker
    {
        double frame, bpm;
        bool ramp;
        long long beat; // beats since the first marker
        float confidence;
    };
    std::vector<Marker> markers;
    {
        const auto& g = regions.front();
        const double period = 60.0 * rate / g.fine;
        const double start = g.lock.phase - std::floor ((g.lock.phase - toFrame (0.0)) / period) * period;
        markers.push_back ({ start, g.fine, false, 0, 0.0f });
    }
    std::vector<size_t> regionMarker { 0 }; // each section's own (steady) marker
    const int rampBeats = std::max (1, options.rampMarkerBars) * beatsPerBar;
    for (size_t r = 0; r + 1 < regions.size(); ++r)
    {
        const auto& a = regions[r];
        const auto& b = regions[r + 1];
        const double ta = 60.0 * rate / a.fine, tb = 60.0 * rate / b.fine;
        const double lo = std::max (a.fromFrame, toFrame (pts[a.last].time - 16.0));
        const double hi = std::min (b.toFrame, toFrame (pts[b.first].time + 16.0));
        if (hi - lo < 2.0 * std::max (ta, tb))
            return; // can't place the change; keep the steady result
        auto aBeat = [&] (double k) { return a.lock.phase + k * ta; };
        auto bBeatAfter = [&] (double pos) { return b.lock.phase + std::ceil ((pos - b.lock.phase) / tb) * tb; };
        const double k0 = std::ceil ((lo - a.lock.phase) / ta), k1 = std::floor ((hi - a.lock.phase) / ta);

        // Jump after A's beat k.
        double bestJump = -1.0, jumpK = k0, jumpStart = 0.0;
        for (double kk = k0; kk <= k1; kk += 1.0)
        {
            double sum = 0.0;
            int count = 0;
            for (double pos = aBeat (k0); pos <= aBeat (kk) + 1.0e-6; pos += ta, ++count)
                sum += onsetAt (pos);
            const double startB = bBeatAfter (aBeat (kk) + 0.5 * tb);
            for (double pos = startB; pos <= hi; pos += tb, ++count)
                sum += onsetAt (pos);
            if (count > 0 && sum / count > bestJump)
            {
                bestJump = sum / count;
                jumpK = kk;
                jumpStart = startB;
            }
        }

        // Ramp from A's beat k over n beats, ending on one of B's beats.
        double bestRamp = -1.0, rampK = 0.0, rampEnd = 0.0;
        int rampN = 0;
        std::vector<double> rampPos, bestRampPos;
        for (double kk = k0; kk <= k1; kk += 1.0)
            for (int n = 4 * beatsPerBar; aBeat (kk) + n * std::min (ta, tb) <= hi; ++n)
            {
                // Tempo rising (or falling) evenly per beat, as a DJ's pitch ramp does.
                const double bpmA = 60.0 * rate / ta, bpmB = 60.0 * rate / tb;
                auto periodAt = [&] (int i) { return 60.0 * rate / (bpmA + (bpmB - bpmA) * i / n); };
                double end = aBeat (kk);
                for (int i = 1; i <= n; ++i)
                    end += periodAt (i);
                const double target = bBeatAfter (end - 0.5 * tb);
                if (std::abs (target - end) > 0.12 * tb)
                    continue;
                const double scale = (target - aBeat (kk)) / (end - aBeat (kk));
                rampPos.clear();
                double pos = aBeat (kk);
                for (int i = 1; i <= n; ++i)
                    rampPos.push_back (pos += scale * periodAt (i));
                double sum = 0.0;
                int count = 0;
                for (double p = aBeat (k0); p <= aBeat (kk) + 1.0e-6; p += ta, ++count)
                    sum += onsetAt (p);
                for (double p : rampPos)
                {
                    sum += onsetAt (p);
                    ++count;
                }
                for (double p = target + tb; p <= hi; p += tb, ++count)
                    sum += onsetAt (p);
                if (sum / count > bestRamp)
                {
                    bestRamp = sum / count;
                    rampK = kk;
                    rampN = n;
                    rampEnd = target;
                    bestRampPos = rampPos;
                }
            }

        auto beatsSinceLast = [&] (double pos)
        {
            const auto& last = markers.back();
            return last.beat + (long long) std::llround ((pos - last.frame) / (60.0 * rate / last.bpm));
        };
        const float confA = (float) std::clamp ((a.fitMargin - 1.0) / 0.3, 0.0, 1.0);
        const float confB = (float) std::clamp ((b.fitMargin - 1.0) / 0.3, 0.0, 1.0);
        if (markers.size() == 1)
            markers.back().confidence = confA * out.bpmConfidence;
        if (bestRamp > 1.03 * bestJump && rampN > 0)
        {
            // Markers every rampMarkerBars bars through the ramp, each at the mean tempo
            // of its beats.
            std::vector<double> beats { aBeat (rampK) };
            beats.insert (beats.end(), bestRampPos.begin(), bestRampPos.end());
            beats.back() = rampEnd;
            const long long startBeat = beatsSinceLast (beats.front());
            for (size_t i = 0; i + 1 < beats.size(); i += (size_t) rampBeats)
            {
                const size_t j = std::min (beats.size() - 1, i + (size_t) rampBeats);
                const double bpm = 60.0 * rate * (double) (j - i) / (beats[j] - beats[i]);
                markers.push_back ({ beats[i], bpm, true, startBeat + (long long) i, 0.8f * std::min (confA, confB) * out.bpmConfidence });
            }
            markers.push_back ({ rampEnd, b.fine, false, startBeat + (long long) (beats.size() - 1), confB * out.bpmConfidence });
        }
        else
        {
            // Where A's last beat and B's first coincide, B starts on that beat.
            double startFrame = jumpStart, lastA = aBeat (jumpK);
            if (std::abs (jumpStart - tb - lastA) < 0.1 * std::min (ta, tb))
            {
                startFrame = jumpStart - tb;
                lastA = aBeat (jumpK - 1.0);
            }
            markers.push_back ({ startFrame, b.fine, false, beatsSinceLast (lastA) + 1, confB * out.bpmConfidence });
        }
        regionMarker.push_back (markers.size() - 1);
    }

    // 7. Bar numbers: every section votes for where beat 1 falls (its downbeat cues,
    // weighted by its length), counted in beats along the whole grid, so bars stay
    // continuous across the changes.
    std::vector<double> votes ((size_t) beatsPerBar, 0.0);
    for (size_t gi = 0; gi < regions.size(); ++gi)
        for (size_t r = 0; r < regions[gi].lock.scores.size(); ++r)
        {
            const auto& g = regions[gi];
            const auto& mk = markers[regionMarker[gi]];
            const long long c = mk.beat + (long long) std::llround ((g.lock.phase + (double) r * 60.0 * rate / g.fine - mk.frame) / (60.0 * rate / g.fine));
            votes[(size_t) ((c % beatsPerBar + beatsPerBar) % beatsPerBar)] += g.lock.scores[r] * (g.toFrame - g.fromFrame) / rate;
#ifdef KT_SEG_DEBUG
            std::fprintf (stderr, "region %.1f-%.1f s bpm %.3f slot %zu count %lld score %.2f\n", toSeconds (g.fromFrame), toSeconds (g.toFrame), g.fine, r, c, g.lock.scores[r]);
#endif
        }
    const long long downbeatCount = (long long) (std::max_element (votes.begin(), votes.end()) - votes.begin());
    const auto& dom = regions[dominant];
    const double domBpm = dom.fine;
    auto beatInBar = [&] (long long beat) { return (int) (((beat - downbeatCount) % beatsPerBar + beatsPerBar) % beatsPerBar) + 1; };

    // The first segment starts on its first downbeat.
    {
        auto& m0 = markers.front();
        const double period = 60.0 * rate / m0.bpm;
        const int shift = (beatsPerBar - (beatInBar (m0.beat) - 1)) % beatsPerBar;
        m0.frame += shift * period;
        m0.beat += shift;
    }

    out.tempoSegments.clear();
    for (auto& mk : markers)
        out.tempoSegments.push_back ({ toSeconds (mk.frame), mk.bpm, mk.confidence, beatInBar (mk.beat), mk.ramp });
    out.variableTempo = true;
    out.bpmMin = out.bpmMax = markers.front().bpm;
    for (auto& mk : markers)
    {
        out.bpmMin = std::min (out.bpmMin, mk.bpm);
        out.bpmMax = std::max (out.bpmMax, mk.bpm);
    }

    // The single-tempo fields describe the dominant section, extended over the track.
    out.bpm = domBpm;
    out.downbeatConfidence = dom.lock.downbeatConfidence;
    const auto& domMarker = markers[regionMarker[dominant]];
    double domDownbeat = domMarker.frame;
    long long domCount = domMarker.beat;
    while (((domCount - downbeatCount) % beatsPerBar + beatsPerBar) % beatsPerBar != 0)
    {
        domDownbeat += 60.0 * rate / domBpm;
        ++domCount;
    }
    const double bar = beatsPerBar * 60.0 / domBpm;
    out.firstBeatSeconds = std::fmod (toSeconds (domDownbeat), bar);
    if (out.firstBeatSeconds < 0.0)
        out.firstBeatSeconds += bar;
}

// ---- Meter changes ------------------------------------------------------------------
//
// Each reading also carries the detector's bar grouping (2 / 4 against 3 beats), its
// subdivision (straight against triplet) and, through its raw tempo, the felt pulse. A
// section that holds a different meter or pulse for minSegmentBars, and whose onsets
// confirm it, starts a new segment on the downbeat where it begins.
void TrackAnalyzer::Impl::findMeterSegments (TrackAnalysis& out, const Envelopes& env, double frameOffset)
{
    if (! options.detectChanges || out.tempoSegments.empty() || readings.size() < 8)
        return;
    const double rate = env.rate;
    const int frames = (int) env.full.size();
    auto toFrame = [&] (double seconds) { return (seconds - frameOffset) * rate; };
    auto toSeconds = [&] (double frame) { return frame / rate + frameOffset; };
    auto segmentAt = [&] (double t)
    {
        size_t s = 0;
        while (s + 1 < out.tempoSegments.size() && out.tempoSegments[s + 1].startSeconds <= t)
            ++s;
        return s;
    };

    // Reading classes: bar grouping (4 or 3), straight or triplet subdivision, and the
    // pulse against the grid (1, 4:3 or 3:4).
    struct Point { double time; int grouping; bool compound; int pulse; }; // pulse: 0 = grid, 1 = 4:3, 2 = 3:4
    auto code = [] (const Point& p) { return p.grouping * 10 + (p.compound ? 5 : 0) + p.pulse; };
    std::vector<double> weights;
    for (auto& r : readings)
        weights.push_back (r.weight);
    std::sort (weights.begin(), weights.end());
    const double threshold = 0.2 * weights[weights.size() * 4 / 5];
    std::vector<Point> pts;
    for (auto& r : readings)
    {
#ifdef KT_SEG_DEBUG
        std::fprintf (stderr, "meter raw %.1f bpm %.2f w %.3f (threshold %.3f)\n", r.time, r.bpm, r.weight, threshold);
#endif
        if (r.weight < threshold)
            continue;
        const auto& seg = out.tempoSegments[segmentAt (r.time)];
        if (seg.ramp)
            continue;
        double ratio = r.bpm / seg.bpm;
        int octave = 0;
        while (ratio > 1.42)
        {
            ratio *= 0.5;
            ++octave;
        }
        while (ratio < 0.707)
        {
            ratio *= 2.0;
            --octave;
        }
        Point p { r.time, 4, false, 0 };
        if (std::abs (ratio - 1.0) < 0.01)
        {
            // Bar grouping and subdivision only mean something at the grid's own beat.
            if (octave == 0)
            {
                p.grouping = r.triple > r.duple + 0.08f ? 3 : 4;
                p.compound = r.compound > 0.08f;
            }
        }
        else if (std::abs (ratio - 4.0 / 3.0) < 0.01)
            p.pulse = 1;
        else if (std::abs (ratio - 0.75) < 0.008)
            p.pulse = 2;
        else
            continue;
        pts.push_back (p);
#ifdef KT_SEG_DEBUG
        std::fprintf (stderr, "meter reading %.1f bpm %.2f duple %.2f triple %.2f compound %.2f -> %d\n", r.time, r.bpm, r.duple, r.triple, r.compound, code (p));
#endif
    }
    if (pts.size() < 8)
        return;

    struct Region { double from, to; Point kind; double bpm = 0.0; double fromFrame = 0.0, toFrame = 0.0; };
    std::vector<Region> regions;
    for (size_t i = 0; i < pts.size();)
    {
        size_t last = i;
        int misses = 0;
        for (size_t j = i + 1; j < pts.size(); ++j)
        {
            if (code (pts[j]) == code (pts[i]))
            {
                last = j;
                misses = 0;
            }
            else if (++misses > 3)
                break;
        }
        const double bpm = out.tempoSegments[segmentAt (pts[i].time)].bpm;
        const double minSeconds = 0.85 * options.minSegmentBars * 4 * 60.0 / bpm;
        if (pts[last].time - pts[i].time + 6.0 >= minSeconds)
            regions.push_back ({ pts[i].time - 6.0, pts[last].time + 6.0, pts[i], bpm });
        i = last + 1;
    }

    // Onset evidence: periodicity of the kick and full band at a lag, over a region.
    std::vector<float> onset ((size_t) frames);
    for (int i = 0; i < frames; ++i)
        onset[(size_t) i] = env.full[(size_t) i] + env.low[(size_t) i];
    auto acf = [&] (const std::vector<float>& v, double from, double to, double lag)
    {
        double sum = 0.0, norm = 0.0;
        for (int i = std::max (0, (int) from); i + lag + 1 < std::min ((double) frames, to); ++i)
        {
            sum += v[(size_t) i] * interpolate (v, i + lag);
            norm += v[(size_t) i] * v[(size_t) i];
        }
        return norm > 1.0e-12 ? sum / norm : 0.0;
    };
    auto alignment = [&] (double bpm, double from, double to)
    {
        const double period = 60.0 * rate / bpm;
        double best = 0.0;
        for (double phase = 0.0; phase < period; phase += 0.5)
        {
            double sum = 0.0;
            int count = 0;
            for (double pos = from + phase; pos < to - 1; pos += period, ++count)
                sum += interpolate (onset, pos);
            if (count > 0)
                best = std::max (best, sum / count);
        }
        return best;
    };
    auto pulseRatio = [] (int pulse) { return pulse == 1 ? 4.0 / 3.0 : (pulse == 2 ? 0.75 : 1.0); };
    // How strongly a region's onsets show its own kind against another kind (> 1 = its own).
    auto evidenceFor = [&] (const Region& r, const Point& own, const Point& other)
    {
        const double period = 60.0 * rate / r.bpm;
        double ratio = 10.0;
        if (own.grouping != other.grouping)
        {
            // From the kick band: snares and hats on every beat say nothing about the bar.
            const double g = acf (env.low, r.fromFrame, r.toFrame, own.grouping * period), h = acf (env.low, r.fromFrame, r.toFrame, other.grouping * period);
            ratio = std::min (ratio, h > 1.0e-9 ? g / h : 10.0);
        }
        if (own.compound != other.compound)
        {
            const double third = acf (onset, r.fromFrame, r.toFrame, period / 3.0), half = acf (onset, r.fromFrame, r.toFrame, period / 2.0);
            const double g = own.compound ? third : half, h = own.compound ? half : third;
            ratio = std::min (ratio, h > 1.0e-9 ? g / h : 10.0);
        }
        if (own.pulse != other.pulse)
        {
            const double g = alignment (r.bpm * pulseRatio (own.pulse), r.fromFrame, r.toFrame);
            const double h = alignment (r.bpm * pulseRatio (other.pulse), r.fromFrame, r.toFrame);
            ratio = std::min (ratio, h > 1.0e-9 ? g / h : 10.0);
        }
        return ratio;
    };

    for (bool changed = true; changed;)
    {
        changed = false;
        for (auto& r : regions)
        {
            r.fromFrame = std::max (0.0, toFrame (r.from));
            r.toFrame = std::min ((double) frames, toFrame (r.to));
        }
        for (size_t r = 0; r + 1 < regions.size(); ++r)
        {
            auto& a = regions[r];
            auto& b = regions[r + 1];
            const bool distinct = code (a.kind) != code (b.kind) && std::abs (a.bpm / b.bpm - 1.0) < 1.0e-9
                                  && evidenceFor (a, a.kind, b.kind) > 1.1 && evidenceFor (b, b.kind, a.kind) > 1.1;
#ifdef KT_SEG_DEBUG
            std::fprintf (stderr, "meter %d (%.0f-%.0f) -> %d (%.0f-%.0f): evidence %.2f %.2f\n", code (a.kind), a.from, a.to, code (b.kind), b.from, b.to,
                          evidenceFor (a, a.kind, b.kind), evidenceFor (b, b.kind, a.kind));
#endif
            if (! distinct)
            {
                // Keep the kind with more time behind it.
                if (b.to - b.from > a.to - a.from)
                    a.kind = b.kind;
                a.to = b.to;
                regions.erase (regions.begin() + (long) r + 1);
                changed = true;
                break;
            }
        }
    }
    if (regions.size() < 2)
        return;

    auto signature = [] (const Point& k)
    {
        if (k.compound)
            return k.grouping == 3 ? std::pair<int, int> { 9, 8 } : std::pair<int, int> { 6, 8 };
        return k.grouping == 3 ? std::pair<int, int> { 3, 4 } : std::pair<int, int> { 4, 4 };
    };
    auto barBeats = [] (const TempoSegment& s) { return s.numerator == 3 && s.denominator == 4 ? 3 : 4; };

    // Each change goes on the beat where bar lines before it (in the old meter) and after
    // it (in the new one) land most kicks.
    const int win = std::max (1, (int) std::lround (0.03 * rate));
    auto kickAt = [&] (double frame)
    {
        double best = 0.0;
        for (int d = -win; d <= win; ++d)
            best = std::max (best, interpolate (env.low, frame + d));
        return best;
    };
    std::vector<TempoSegment> segments = out.tempoSegments;
    auto applyKind = [&] (TempoSegment& s, const Point& k)
    {
        const auto sig = signature (k);
        s.numerator = sig.first;
        s.denominator = sig.second;
        s.pulseBpm = k.pulse != 0 ? s.bpm * pulseRatio (k.pulse) : 0.0;
    };
    for (auto& s : segments)
        if (s.startSeconds < regions[1].from + 6.0)
            applyKind (s, regions[0].kind);
    for (size_t r = 1; r < regions.size(); ++r)
    {
        const auto& a = regions[r - 1];
        const auto& b = regions[r];
        const double from = a.to - 12.0, to = b.from + 12.0;
        size_t si = 0;
        while (si + 1 < segments.size() && segments[si + 1].startSeconds <= 0.5 * (from + to))
            ++si;
        const auto base = segments[si];
        const double period = 60.0 / base.bpm;
        const int beatsA = a.kind.grouping == 3 && ! a.kind.compound ? 3 : 4, beatsB = b.kind.grouping == 3 && ! b.kind.compound ? 3 : 4;
        double best = -1.0, at = -1.0;
        for (double k = std::ceil ((from - base.startSeconds) / period); base.startSeconds + k * period <= to; k += 1.0)
        {
            const double c = base.startSeconds + k * period;
            double score = 0.0;
            if (beatsA != beatsB || a.kind.compound != b.kind.compound)
                for (int bar = 1; bar <= 8; ++bar)
                    score += kickAt (toFrame (c - bar * beatsA * period)) + kickAt (toFrame (c + (bar - 1) * beatsB * period));
            if (a.kind.pulse != b.kind.pulse)
            {
                // The old pulse up to the change, the new one from it.
                const double pa = period / pulseRatio (a.kind.pulse), pb = period / pulseRatio (b.kind.pulse);
                for (int k = 1; k <= 32; ++k)
                    score += interpolate (onset, toFrame (c - k * pa)) + interpolate (onset, toFrame (c + (k - 1) * pb));
            }
            if (score > best)
            {
                best = score;
                at = c;
            }
        }
        if (at < 0.0)
            continue;
        TempoSegment seg = base;
        seg.startSeconds = at;
        seg.beatInBar = 1;
        seg.ramp = false;
        applyKind (seg, b.kind);
        segments.insert (segments.begin() + (long) si + 1, seg);
        for (size_t s = si + 2; s < segments.size() && (r + 1 >= regions.size() || segments[s].startSeconds < regions[r + 1].from + 6.0); ++s)
            applyKind (segments[s], b.kind);
    }

    // Beat numbers carried through: a meter change starts on a downbeat, every other
    // segment continues the bar it falls in.
    for (size_t s = 1; s < segments.size(); ++s)
    {
        const auto& prev = segments[s - 1];
        auto& cur = segments[s];
        const bool meterChange = cur.numerator != prev.numerator || cur.denominator != prev.denominator || cur.pulseBpm != prev.pulseBpm;
        if (meterChange && cur.bpm == prev.bpm)
        {
            cur.beatInBar = 1;
            continue;
        }
        const auto beats = (long long) std::llround ((cur.startSeconds - prev.startSeconds) * prev.bpm / 60.0);
        const int bpb = barBeats (prev);
        cur.beatInBar = (int) (((prev.beatInBar - 1 + beats) % bpb + bpb) % bpb) + 1;
    }
    (void) toSeconds;
    out.tempoSegments = segments;
}

// ---- Key changes --------------------------------------------------------------------
//
// Same idea on the key evidence: the key over a 16 s window at every update, runs of the
// same key (relative major / minor counting as the same) lasting minSegmentBars, then each
// change placed where the evidence before and after fits the two keys best, on a downbeat.
void TrackAnalyzer::Impl::findKeySegments (TrackAnalysis& out, int beatsPerBar)
{
    if (! options.detectChanges || out.key < 0 || keySnaps.size() < 8)
        return;

    const size_t n = keySnaps.size();
    auto snapAt = [&] (double time)
    {
        // Last snapshot at or before `time` (the first one if none).
        size_t lo = 0, hi = n;
        while (hi - lo > 1)
        {
            const size_t mid = (lo + hi) / 2;
            (keySnaps[mid].first <= time ? lo : hi) = mid;
        }
        return lo;
    };
    auto evidence = [&] (double from, double to)
    {
        const size_t a = snapAt (from), b = snapAt (to);
        if (b <= a)
            return KeyEvidence {};
        return keySnaps[b].second - keySnaps[a].second;
    };

    struct Point { double time; int key, cls; float confidence; };
    std::vector<Point> pts;
    std::vector<double> energies;
    for (size_t i = 0; i < n; ++i)
        energies.push_back ((evidence (keySnaps[i].first - 8.0, keySnaps[i].first + 8.0)).energy);
    auto sorted = energies;
    std::sort (sorted.begin(), sorted.end());
    const double energyFloor = 0.6 * sorted[sorted.size() / 2]; // skip drum-only intros and breaks
    double lastReading = -1.0e9;
    for (size_t i = 0; i < n; ++i)
    {
        const double t = keySnaps[i].first;
        if (t - lastReading < 1.2)
            continue; // a reading every ~1.2 s, like the tempo
        lastReading = t;
        if (t < 8.0 || t + 8.0 > keySnaps.back().first || energies[i] <= energyFloor)
            continue;
        const auto k = KeyDetector::keyFor (evidence (t - 8.0, t + 8.0));
        if (k.valid && k.confidence >= 0.1f)
            pts.push_back ({ t, k.key, keyClass (k.key), k.confidence });
    }
#ifdef KT_SEG_DEBUG
    for (auto& p : pts)
        std::fprintf (stderr, "key reading %.1f key %d conf %.2f\n", p.time, p.key, p.confidence);
#endif
    if (pts.size() < 8)
        return;

    const double bpm = out.valid ? out.bpm : 120.0;
    const double minSeconds = 0.85 * options.minSegmentBars * beatsPerBar * 60.0 / bpm;
    struct Region { double from, to; int cls, key = -1; float confidence = 0.0f; };
    std::vector<Region> regions;
    for (size_t i = 0; i < pts.size();)
    {
        size_t last = i;
        int misses = 0;
        for (size_t j = i + 1; j < pts.size(); ++j)
        {
            if (closelyRelated (pts[j].key, pts[i].key))
            {
                last = j;
                misses = 0;
            }
            else if (++misses > 3)
                break;
        }
        if (pts[last].time - pts[i].time + 8.0 >= minSeconds) // the windows only half count: drum-only intros read as a key
            regions.push_back ({ pts[i].time - 8.0, pts[last].time + 8.0, pts[i].cls });
        i = last + 1;
    }
#ifdef KT_SEG_DEBUG
    for (auto& r : regions)
        std::fprintf (stderr, "key run %.1f-%.1f class %d\n", r.from, r.to, r.cls);
#endif
    for (bool changed = true; changed;)
    {
        changed = false;
        for (auto& r : regions)
        {
            const auto k = KeyDetector::keyFor (evidence (r.from, r.to));
            r.key = k.valid ? k.key : -1;
            r.cls = k.valid ? keyClass (k.key) : -1;
            r.confidence = k.confidence;
        }
        for (size_t r = 0; r + 1 < regions.size(); ++r)
        {
            auto& a = regions[r];
            auto& b = regions[r + 1];
            bool same = a.cls < 0 || b.cls < 0 || closelyRelated (a.key, b.key);
            if (! same)
            {
                // Comparable harmonic level on both sides: a drum-only intro or break reads
                // as some key from the drums' pitch, which is not a modulation.
                const double rateA = evidence (a.from, a.to).energy / std::max (1.0, a.to - a.from);
                const double rateB = evidence (b.from, b.to).energy / std::max (1.0, b.to - b.from);
                same = std::min (rateA, rateB) < 0.88 * std::max (rateA, rateB);
#ifdef KT_SEG_DEBUG
                std::fprintf (stderr, "key rates %.1f %.1f\n", rateA, rateB);
#endif
            }
            if (! same)
            {
                // Each side must clearly prefer its own key over the other's.
                const auto sa = KeyDetector::keyScores (evidence (a.from, a.to));
                const auto sb = KeyDetector::keyScores (evidence (b.from, b.to));
                same = sa[(size_t) a.key] - sa[(size_t) b.key] < 0.3 || sb[(size_t) b.key] - sb[(size_t) a.key] < 0.3;
            }
#ifdef KT_SEG_DEBUG
            if (! closelyRelated (a.key, b.key))
            {
                const auto sa = KeyDetector::keyScores (evidence (a.from, a.to));
                const auto sb = KeyDetector::keyScores (evidence (b.from, b.to));
                std::fprintf (stderr, "key %d -> %d: conf %.2f %.2f, margins %.3f %.3f -> %s\n", a.key, b.key, a.confidence, b.confidence,
                              sa[(size_t) a.key] - sa[(size_t) b.key], sb[(size_t) b.key] - sb[(size_t) a.key], same ? "merged" : "kept");
            }
#endif
            if (same)
            {
                a.to = b.to;
                regions.erase (regions.begin() + (long) r + 1);
                changed = true;
                break;
            }
        }
    }
    if (regions.size() < 2)
        return;
#ifdef KT_SEG_DEBUG
    for (size_t r = 0; r + 1 < regions.size(); ++r)
    {
        const auto sa = KeyDetector::keyScores (evidence (regions[r].from, regions[r].to));
        const auto sb = KeyDetector::keyScores (evidence (regions[r + 1].from, regions[r + 1].to));
        std::fprintf (stderr, "key change %d -> %d at %.0f: A %.3f vs %.3f, B %.3f vs %.3f, conf %.2f %.2f, energy %.3g %.3g\n", regions[r].key, regions[r + 1].key, regions[r].to,
                      sa[(size_t) regions[r].key], sa[(size_t) regions[r + 1].key], sb[(size_t) regions[r + 1].key], sb[(size_t) regions[r].key],
                      regions[r].confidence, regions[r + 1].confidence, evidence (regions[r].from, regions[r].to).energy, evidence (regions[r + 1].from, regions[r + 1].to).energy);
    }
#endif

    // Bar lines from the tempo grid, to snap each change to.
    auto nearestDownbeat = [&] (double time)
    {
        if (out.tempoSegments.empty())
            return time;
        size_t s = 0;
        while (s + 1 < out.tempoSegments.size() && out.tempoSegments[s + 1].startSeconds <= time)
            ++s;
        const auto& seg = out.tempoSegments[s];
        const double beat = 60.0 / seg.bpm;
        const double firstDownbeat = seg.startSeconds + ((beatsPerBar - (seg.beatInBar - 1)) % beatsPerBar) * beat;
        const double bar = beat * beatsPerBar;
        const double snapped = firstDownbeat + std::round ((time - firstDownbeat) / bar) * bar;
        return snapped >= 0.0 ? snapped : time;
    };

    std::vector<KeySegment> segments { { 0.0, regions.front().key, regions.front().confidence } };
    for (size_t r = 0; r + 1 < regions.size(); ++r)
    {
        const auto& a = regions[r];
        const auto& b = regions[r + 1];
        // The change lies between the last window that is all A and the first that is all B.
        const double from = a.to - 16.0, to = b.from + 16.0;
        const double lo = std::max (a.from, from - 30.0), hi = std::min (b.to, to + 30.0);
        // Each ~0.3 s slice votes for A or B by how much better its chroma fits that key,
        // weighted by its energy; the change goes where the running vote peaks.
        double run = 0.0, best = 0.0, at = 0.5 * (from + to);
        for (size_t i = snapAt (lo); i + 1 < n && keySnaps[i + 1].first <= hi; ++i)
        {
            const auto slice = keySnaps[i + 1].second - keySnaps[i].second;
            if (slice.energy <= 0.0)
                continue;
            const auto sc = KeyDetector::keyScores (slice);
            run += slice.energy * (sc[(size_t) a.key] - sc[(size_t) b.key]);
            const double c = keySnaps[i + 1].first;
            if (run > best && c >= from && c <= to)
            {
                best = run;
                at = c;
            }
        }
        segments.push_back ({ nearestDownbeat (at), b.key, b.confidence });
    }

    // The single key is the one held longest.
    double longest = -1.0;
    for (size_t s = 0; s < segments.size(); ++s)
    {
        const double end = s + 1 < segments.size() ? segments[s + 1].startSeconds : out.durationSeconds;
        if (end - segments[s].startSeconds > longest)
        {
            longest = end - segments[s].startSeconds;
            out.key = segments[s].key;
            out.keyConfidence = segments[s].confidence;
        }
    }
    out.keySegments = segments;
}
} // namespace kt
