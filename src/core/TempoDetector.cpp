#include "TempoDetector.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#ifdef KT_TEMPO_DEBUG
#include <cstdio>
#endif

namespace kt
{
namespace
{
    /** Local-mean removal + half-wave rectification + zero-mean, so the
        autocorrelation sees sharp onsets rather than loudness swells. */
    std::vector<float> conditionEnvelope (const std::vector<float>& e, double envRate)
    {
        const int n = (int) e.size();
        const int half = std::max (1, (int) (envRate * 0.125));
        std::vector<double> prefix ((size_t) n + 1, 0.0);
        for (int i = 0; i < n; ++i)
            prefix[(size_t) i + 1] = prefix[(size_t) i] + e[(size_t) i];

        std::vector<float> o ((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            const int a = std::max (0, i - half), b = std::min (n, i + half + 1);
            const double mean = (prefix[(size_t) b] - prefix[(size_t) a]) / (b - a);
            o[(size_t) i] = (float) std::max (0.0, e[(size_t) i] - mean);
        }

        // Light smoothing (3-tap) widens onset peaks so fractional-lag
        // interpolation of the autocorrelation isn't biased toward integer lags.
        std::vector<float> s ((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            const float l = o[(size_t) std::max (0, i - 1)], r = o[(size_t) std::min (n - 1, i + 1)];
            s[(size_t) i] = 0.25f * l + 0.5f * o[(size_t) i] + 0.25f * r;
        }

        const double mean = std::accumulate (s.begin(), s.end(), 0.0) / std::max (1, n);
        for (auto& v : s)
            v -= (float) mean;
        return s;
    }

    /** Local-mean removal + half-wave rectification only (keeps the envelope >= 0). */
    std::vector<float> rectifyEnvelope (const std::vector<float>& e, double envRate)
    {
        const int n = (int) e.size();
        const int half = std::max (1, (int) (envRate * 0.125));
        std::vector<double> prefix ((size_t) n + 1, 0.0);
        for (int i = 0; i < n; ++i)
            prefix[(size_t) i + 1] = prefix[(size_t) i] + e[(size_t) i];
        std::vector<float> o ((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            const int a = std::max (0, i - half), b = std::min (n, i + half + 1);
            o[(size_t) i] = (float) std::max (0.0, e[(size_t) i] - (prefix[(size_t) b] - prefix[(size_t) a]) / (b - a));
        }
        return o;
    }

    /** Normalised (unbiased) autocorrelation for lags 0..maxLag. Empty if the signal is flat. */
    std::vector<double> autocorrelation (const std::vector<float>& o, int maxLag)
    {
        const int n = (int) o.size();
        std::vector<double> acf ((size_t) maxLag + 1, 0.0);
        for (int lag = 0; lag <= maxLag; ++lag)
        {
            double sum = 0.0;
            for (int i = lag; i < n; ++i)
                sum += (double) o[(size_t) i] * o[(size_t) (i - lag)];
            acf[(size_t) lag] = sum / (n - lag);
        }
        if (acf[0] <= 1.0e-12)
            return {};
        const double zero = acf[0];
        for (auto& v : acf)
            v /= zero;
        return acf;
    }

    double interpolate (const std::vector<double>& acf, double lag)
    {
        const int maxLag = (int) acf.size() - 1;
        if (acf.empty() || lag < 0.0 || lag >= maxLag)
            return 0.0;
        const int i = (int) lag;
        const double f = lag - i;
        return acf[(size_t) i] * (1.0 - f) + acf[(size_t) i + 1] * f;
    }

    /** Peak value near a lag (±1.5 frames), tolerant of slight tempo drift. */
    double peakNear (const std::vector<double>& acf, double lag)
    {
        double best = -1.0;
        for (double d = -1.5; d <= 1.5; d += 0.5)
            best = std::max (best, interpolate (acf, lag + d));
        return best;
    }
} // namespace

void TempoDetector::prepare (double sr, double windowSeconds)
{
    sampleRate = sr;

    // ~23 ms analysis window and a quarter-window hop, scaled with sample rate.
    frameSize = Fft::nextPowerOfTwo ((int) std::lround (sr * 0.023));
    hop = frameSize / 4;
    envRate = sr / hop;
    fft = std::make_unique<Fft> (frameSize);

    maxBin = std::min (frameSize / 2, (int) (11000.0 * frameSize / sr));
    lowBin = std::max (2, (int) std::lround (110.0 * frameSize / sr));
    midLo = (int) std::lround (200.0 * frameSize / sr);
    midHi = (int) std::lround (4000.0 * frameSize / sr);
    highLo = (int) std::lround (6000.0 * frameSize / sr);
    highHi = std::min (frameSize / 2, (int) std::lround (16000.0 * frameSize / sr));

    inputRing.assign ((size_t) frameSize, 0.0f);
    frame.assign ((size_t) frameSize, 0.0f);
    prevLog.assign ((size_t) frameSize / 2 + 1, 0.0f);
    prevLow.assign ((size_t) frameSize / 2 + 1, 0.0f);
    prevAccent.assign ((size_t) frameSize / 2 + 1, 0.0f);
    prevMid.assign ((size_t) frameSize / 2 + 1, 0.0f);
    prevHigh.assign ((size_t) frameSize / 2 + 1, 0.0f);

    const auto envCapacity = (size_t) std::ceil (windowSeconds * envRate);
    env.assign (envCapacity, 0.0f);
    lowEnv.assign (envCapacity, 0.0f);
    accentEnv.assign (envCapacity, 0.0f);
    midEnv.assign (envCapacity, 0.0f);
    highEnv.assign (envCapacity, 0.0f);
    active.assign (envCapacity, 0);

    reset();
}

void TempoDetector::reset()
{
    std::fill (inputRing.begin(), inputRing.end(), 0.0f);
    std::fill (prevLog.begin(), prevLog.end(), 0.0f);
    std::fill (prevLow.begin(), prevLow.end(), 0.0f);
    std::fill (prevAccent.begin(), prevAccent.end(), 0.0f);
    std::fill (prevMid.begin(), prevMid.end(), 0.0f);
    std::fill (prevHigh.begin(), prevHigh.end(), 0.0f);
    std::fill (env.begin(), env.end(), 0.0f);
    std::fill (lowEnv.begin(), lowEnv.end(), 0.0f);
    std::fill (accentEnv.begin(), accentEnv.end(), 0.0f);
    std::fill (midEnv.begin(), midEnv.end(), 0.0f);
    std::fill (highEnv.begin(), highEnv.end(), 0.0f);
    std::fill (active.begin(), active.end(), (uint8_t) 0);
    ringWrite = 0;
    samplesUntilFrame = frameSize; // wait for a full first window
    havePrev = false;
    envWrite = envCount = 0;
    kickEma = kickPeak = 0.0;
}

void TempoDetector::push (const float* mono, int numSamples)
{
    for (int i = 0; i < numSamples; ++i)
    {
        inputRing[(size_t) ringWrite] = mono[i];
        ringWrite = (ringWrite + 1) % frameSize;

        if (--samplesUntilFrame <= 0)
        {
            processFrame();
            samplesUntilFrame = hop;
        }
    }
}

void TempoDetector::processFrame()
{
    double energy = 0.0;
    for (int i = 0; i < frameSize; ++i)
    {
        const float s = inputRing[(size_t) ((ringWrite + i) % frameSize)];
        frame[(size_t) i] = s;
        energy += (double) s * s;
    }

    const bool hasSignal = std::sqrt (energy / frameSize) > 1.0e-4; // about -80 dBFS
    float flux = 0.0f, lowFlux = 0.0f, accentFlux = 0.0f;

    fft->magnitudeSpectrum (frame.data(), mags);
    const float scale = 2.0f / (float) frameSize;

    for (int k = 1; k <= maxBin; ++k)
    {
        const float m = mags[(size_t) k] * scale;
        const float l = std::log1p (1000.0f * m);
        if (havePrev)
        {
            flux += std::max (0.0f, l - prevLog[(size_t) k]);
            accentFlux += std::max (0.0f, std::log1p (10.0f * m) - prevAccent[(size_t) k]);
            // The low band uses (nearly) linear magnitude so that genuine low-end hits
            // (kick, bass) dominate rather than low-level spill from broadband noise.
            if (k <= lowBin)
                lowFlux += std::max (0.0f, m - prevLow[(size_t) k]);
        }
        prevLog[(size_t) k] = l;
        prevAccent[(size_t) k] = std::log1p (10.0f * m);
        if (k <= lowBin)
            prevLow[(size_t) k] = m;
    }
    // Instrument bands for groove analysis. Snare/clap: compressed flux in the mids.
    // Hats/cymbals: strongly compressed flux up top, so quiet hats still register.
    float midFlux = 0.0f, highFlux = 0.0f;
    for (int k = midLo; k <= midHi; ++k)
    {
        const float l = std::log1p (20.0f * mags[(size_t) k] * scale);
        if (havePrev)
            midFlux += std::max (0.0f, l - prevMid[(size_t) k]);
        prevMid[(size_t) k] = l;
    }
    for (int k = highLo; k <= highHi; ++k)
    {
        const float l = std::log1p (300.0f * mags[(size_t) k] * scale);
        if (havePrev)
            highFlux += std::max (0.0f, l - prevHigh[(size_t) k]);
        prevHigh[(size_t) k] = l;
    }
    havePrev = true;

    if (! hasSignal)
        flux = lowFlux = accentFlux = midFlux = highFlux = 0.0f;
    midEnv[(size_t) envWrite] = midFlux;
    highEnv[(size_t) envWrite] = highFlux;

    env[(size_t) envWrite] = flux;
    lowEnv[(size_t) envWrite] = lowFlux;

    // Kick level over ~2 s, and its slowly decaying peak (half-life ~3 min): lets us
    // tell a drop from a breakdown or build-up whose snare rolls fake a groove.
    const double emaCoef = 1.0 / (2.0 * envRate);
    kickEma += (lowFlux - kickEma) * emaCoef;
    kickPeak = std::max (kickPeak * std::exp (-std::log (2.0) / (180.0 * envRate)), kickEma);
    accentEnv[(size_t) envWrite] = accentFlux;
    active[(size_t) envWrite] = hasSignal ? 1 : 0;
    envWrite = (envWrite + 1) % (int) env.size();
    envCount = std::min (envCount + 1, (int) env.size());
}

TempoResult TempoDetector::analyse (double minBpm, double maxBpm, Genre genre) const
{
    TempoResult result;
    const int n = envCount;
    const int capacity = (int) env.size();

    // Need at least ~4 seconds of audio and enough of it to be non-silent.
    if (n < (int) (envRate * 4.0))
        return result;

    int activeFrames = 0;
    std::vector<float> e ((size_t) n), el ((size_t) n), ea ((size_t) n), em ((size_t) n), eh ((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        const int idx = (envWrite - n + i + capacity) % capacity;
        e[(size_t) i] = env[(size_t) idx];
        el[(size_t) i] = lowEnv[(size_t) idx];
        ea[(size_t) i] = accentEnv[(size_t) idx];
        em[(size_t) i] = midEnv[(size_t) idx];
        eh[(size_t) i] = highEnv[(size_t) idx];
        activeFrames += active[(size_t) idx];
    }
    if (activeFrames < (int) (envRate * 3.0))
        return result;

    // Search an octave beyond the requested range on both sides: the groove decides what
    // the music's real tempo is, and the answer is then folded into the range by 2x / 0.5x
    // (e.g. DnB with a Hip-Hop range reads 87, never an unrelated 116).
    const double searchLo = std::max (40.0, minBpm / 2.0), searchHi = std::min (260.0, maxBpm * 2.0);
    const double slowestPeriod = 60.0 * envRate / searchLo;
    const int maxLag = std::min (n - 1, (int) std::ceil (slowestPeriod * 4.0) + 4);

    const auto cond = conditionEnvelope (e, envRate);
    const auto acf = autocorrelation (cond, maxLag);
    if (acf.empty())
        return result;

    // A weak, broad prior only for music with no recognisable groove; the octave
    // decision for rhythmic music comes from the groove templates below.
    auto weakPrior = [&] (double bpm)
    {
        const double octaves = std::log2 (bpm / 120.0);
        return std::exp (-0.5 * octaves * octaves / (1.2 * 1.2));
    };

    // Periodicity: autocorrelation at 1-4 beat periods plus the beat's subdivision.
    auto periodicity = [&] (double bpm) -> double
    {
        const double period = 60.0 * envRate / bpm;
        double s = 0.0, wsum = 0.0;
        for (int k = 1; k <= 4; ++k)
        {
            if (period * k >= maxLag)
                break;
            const double w = 1.0 / std::sqrt ((double) k);
            s += w * interpolate (acf, period * k);
            wsum += w;
        }
        const double sub = std::max (interpolate (acf, period / 2.0),
                                     0.5 * (interpolate (acf, period / 3.0) + interpolate (acf, 2.0 * period / 3.0)));
        s += 0.5 * sub;
        wsum += 0.5;
        return std::max (0.0, s / wsum);
    };

    // Beat salience (see header): judged on a lightly compressed envelope so loud hits
    // stand above quiet ones.
    const auto accent = conditionEnvelope (ea, envRate);
    double accentSd = 0.0;
    for (auto v : accent)
        accentSd += (double) v * v;
    accentSd = std::sqrt (accentSd / n) + 1.0e-12;

    auto accentAt = [&] (double pos)
    {
        const int i = std::clamp ((int) std::lround (pos), 0, n - 1);
        float v = accent[(size_t) i];
        if (i > 0) v = std::max (v, accent[(size_t) i - 1]);
        if (i + 1 < n) v = std::max (v, accent[(size_t) i + 1]);
        return v;
    };

    /** Beat phase (frames) whose beats carry the most accent energy. */
    auto bestPhase = [&] (double period)
    {
        double phase = 0.0, bestMean = -1.0e9;
        for (double p = 0.0; p < period; p += 1.0)
        {
            double sum = 0.0;
            int count = 0;
            for (double pos = p; pos < n - 1; pos += period, ++count)
                sum += accentAt (pos);
            if (count > 0 && sum / count > bestMean)
            {
                bestMean = sum / count;
                phase = p;
            }
        }
        return phase;
    };

    auto salience = [&] (double bpm, double phase)
    {
        const double period = 60.0 * envRate / bpm;
        std::vector<float> beats;
        for (double pos = phase; pos < n - 1; pos += period)
            beats.push_back (accentAt (pos));
        if (beats.empty())
            return 0.0;
        std::sort (beats.begin(), beats.end());
        // The weaker beats (40th percentile). Not the weakest: many breakbeats (DnB
        // two-step) leave beat 3 empty, which is a quarter of all beats.
        return beats[beats.size() * 2 / 5] / accentSd;
    };

    // Fine tempo: phase-lock a pulse train to the onsets across the whole window.
    auto onsetInterp = [&] (double pos)
    {
        const int i = (int) pos;
        if (i < 0 || i + 1 >= n)
            return 0.0;
        const double f = pos - i;
        return (double) cond[(size_t) i] * (1.0 - f) + (double) cond[(size_t) i + 1] * f;
    };
    auto alignment = [&] (double bpm)
    {
        const double period = 60.0 * envRate / bpm;
        double bestSum = -1.0e9;
        for (double phase = 0.0; phase < period; phase += 0.5)
        {
            double sum = 0.0;
            for (double pos = phase; pos < n - 1; pos += period)
                sum += onsetInterp (pos);
            bestSum = std::max (bestSum, sum / std::max (1.0, (n - phase) / period));
        }
        return bestSum;
    };
    auto phaseLock = [&] (double bpm, double range, double step)
    {
        double best = bpm, bestAlign = alignment (bpm);
        for (double b = bpm - range; b <= bpm + range; b += step)
        {
            const double a = alignment (b);
            if (a > bestAlign)
            {
                bestAlign = a;
                best = b;
            }
        }
        return best;
    };

    // Band envelopes for the groove model (rectified, local mean removed).
    BandEnvelopes bands { rectifyEnvelope (el, envRate), rectifyEnvelope (em, envRate), rectifyEnvelope (eh, envRate) };

    // ---- 1. Periodicity peaks over the range.
    const double gridStep = 0.1;
    std::vector<double> grid, scores;
    double bestScore = -1.0;
    for (double bpm = searchLo; bpm <= searchHi + 1e-9; bpm += gridStep)
    {
        grid.push_back (bpm);
        scores.push_back (periodicity (bpm) * weakPrior (bpm));
        bestScore = std::max (bestScore, scores.back());
    }
    if (bestScore <= 0.0)
        return result;

    std::vector<std::pair<double, double>> peaks; // (score, bpm)
    for (size_t i = 0; i < scores.size(); ++i)
    {
        const bool left = i == 0 || scores[i] >= scores[i - 1];
        const bool right = i + 1 == scores.size() || scores[i] >= scores[i + 1];
        if (left && right && scores[i] >= 0.35 * bestScore)
            peaks.emplace_back (scores[i], grid[i]);
    }
    std::sort (peaks.begin(), peaks.end(), [] (auto& a, auto& b) { return a.first > b.first; });
    if (peaks.size() > 5)
        peaks.resize (5);

    // ---- 2. Expand with metrically related tempos: the classic errors are 2x, 1/2x, 3:2, 2:3.
    std::vector<double> seeds;
    auto addSeed = [&] (double bpm)
    {
        if (bpm < searchLo || bpm > searchHi)
            return;
        for (double s : seeds)
            if (std::abs (s - bpm) / bpm < 0.01)
                return;
        seeds.push_back (bpm);
    };
    for (auto& p : peaks)
        addSeed (p.second);
    for (size_t i = 0; i < std::min<size_t> (2, peaks.size()); ++i)
        for (double ratio : { 2.0, 0.5, 1.5, 2.0 / 3.0, 4.0 / 3.0, 0.75 })
            addSeed (peaks[i].second * ratio);

    // ---- 3. Score each candidate on periodicity, beat salience and groove fit.
    struct Candidate
    {
        double bpm = 0, periodicity = 0, salience = 0;
        GrooveFit groove;
        double total = 0;
    };
    std::vector<Candidate> candidates;
    double maxSalience = 1.0e-9;
    for (double seed : seeds)
    {
        Candidate c;
        c.bpm = seed;
        c.periodicity = periodicity (seed);
        for (double b = seed - 0.15; b <= seed + 0.15; b += 0.01)
        {
            const double v = periodicity (b);
            if (v > c.periodicity)
            {
                c.periodicity = v;
                c.bpm = b;
            }
        }
        c.bpm = phaseLock (c.bpm, 0.12, 0.02);
        const double period = 60.0 * envRate / c.bpm;
        const double phase = bestPhase (period);
        c.salience = salience (c.bpm, phase);
        maxSalience = std::max (maxSalience, c.salience);
        const auto folded = foldBar (bands, period, phase);
        const bool inRange = c.bpm >= minBpm - 0.5 && c.bpm <= maxBpm + 0.5;
        c.groove = fitGroove (folded, c.bpm, inRange ? genre : Genre::Auto);
#ifdef KT_TEMPO_DEBUG
        std::fprintf (stderr, "  fold %7.2f K:", c.bpm);
        for (auto v : folded.kick) std::fprintf (stderr, "%4.1f", v);
        std::fprintf (stderr, "\n                S:");
        for (auto v : folded.snare) std::fprintf (stderr, "%4.1f", v);
        std::fprintf (stderr, "\n                H:");
        for (auto v : folded.hats) std::fprintf (stderr, "%4.1f", v);
        std::fprintf (stderr, "\n");
#endif
        candidates.push_back (c);
    }

    const Candidate* winner = nullptr;
    const Candidate* rival = nullptr;
    for (auto& c : candidates)
    {
        // Penalise candidates whose weak beats are clearly weaker than the best's
        // (3:2 groupings), then let the groove decide between octaves.
        const double salienceFactor = std::clamp (c.salience / (0.5 * maxSalience), 0.15, 1.0);
        // Square roots compress periodicity and salience, which both naturally favour the
        // slower of two octave-related readings, so the groove gets the final say.
        c.total = std::sqrt (c.periodicity * salienceFactor) * (0.15 + c.groove.plausibility) * (0.6 + 0.4 * weakPrior (c.bpm));
#ifdef KT_TEMPO_DEBUG
        std::fprintf (stderr, "    cand %7.2f  per %.3f  sal %.2f  fof %.2f bb %.2f ht %.2f ob %.2f  plaus %.2f %-18s total %.3f\n",
                      c.bpm, c.periodicity, c.salience, c.groove.fourOnFloor, c.groove.backbeat, c.groove.halftime,
                      c.groove.offbeatHats, c.groove.plausibility, feelName (c.groove.feel), c.total);
#endif
    }
    // Fold a tempo into [minBpm, maxBpm] by octaves; 0 if it can't be.
    auto foldIntoRange = [&] (double bpm)
    {
        for (double f : { 1.0, 0.5, 2.0, 0.25, 4.0 })
            if (bpm * f >= minBpm - 0.5 && bpm * f <= maxBpm + 0.5)
                return bpm * f;
        return 0.0;
    };

    // Rank by total; the winner is the best candidate that can be shown in the range.
    std::vector<Candidate*> ranked;
    for (auto& c : candidates)
        ranked.push_back (&c);
    std::sort (ranked.begin(), ranked.end(), [] (auto* a, auto* b) { return a->total > b->total; });
    double shownBpm = 0.0;
    for (auto* c : ranked)
        if ((shownBpm = foldIntoRange (c->bpm)) > 0.0)
        {
            winner = c;
            break;
        }
    for (auto* c : ranked)
    {
        const double f = foldIntoRange (c->bpm);
        if (c != winner && f > 0.0 && std::abs (f - shownBpm) / shownBpm > 0.03)
        {
            rival = c;
            break;
        }
    }
    if (winner == nullptr || winner->total <= 0.0)
        return result;

    const double octave = shownBpm / winner->bpm; // 1, 0.5, 2 ...
    const double bestBpm = phaseLock (winner->bpm, 0.3, 0.01) * octave;

    // Confidence: periodicity peak prominence x margin over the strongest rival reading.
    const double avg = std::accumulate (scores.begin(), scores.end(), 0.0) / (double) scores.size();
    double var = 0.0;
    for (auto s : scores)
        var += (s - avg) * (s - avg);
    const double sd = std::sqrt (var / (double) scores.size()) + 1.0e-9;
    const double z = (periodicity (bestBpm / octave) * weakPrior (bestBpm / octave) - avg) / sd;
    const double margin = rival != nullptr ? 1.0 - rival->total / winner->total : 1.0;

    double kickMean = 0.0;
    for (int i = std::max (0, n - (int) (4.0 * envRate)); i < n; ++i) // the most recent 4 s
        kickMean += el[(size_t) i];
    kickMean /= std::min (n, (int) (4.0 * envRate));
    result.drumPresence = kickPeak > 1.0e-9 ? (float) std::clamp (kickMean / (0.5 * kickPeak), 0.0, 1.0) : 0.0f;

    result.bpm = bestBpm;
    result.feel = winner->groove.feel;
    result.grooveFit = (float) winner->groove.plausibility;
    result.alternateBpm = rival != nullptr ? foldIntoRange (rival->bpm) : 0.0;
    result.confidence = (float) (std::clamp ((z - 1.0) / 3.0, 0.0, 1.0)
                                 * std::clamp (winner->periodicity / 0.25, 0.0, 1.0)
                                 * std::clamp (0.35 + margin / 0.4, 0.0, 1.0));
    result.valid = true;

    // ---------------- Meter evidence ----------------
    const double period = 60.0 * envRate / bestBpm;
    const int meterLag = std::min (n - 1, (int) std::ceil (period * 4.0) + 4);
    const auto acfLow = autocorrelation (conditionEnvelope (el, envRate), meterLag);
    const auto acfMeter = (meterLag <= maxLag) ? acf : autocorrelation (cond, meterLag);

    auto barEvidence = [&] (double beats)
    {
        const double low = acfLow.empty() ? 0.0 : peakNear (acfLow, period * beats);
        const double full = peakNear (acfMeter, period * beats);
        return acfLow.empty() ? full : 0.7 * low + 0.3 * full;
    };

    const double two = barEvidence (2.0), three = barEvidence (3.0), four = barEvidence (4.0);
    result.dupleScore = (float) (0.5 * (two + four));
    result.tripleScore = (float) three;

    const double straight = peakNear (acfMeter, period / 2.0);
    const double triplet = 0.5 * (peakNear (acfMeter, period / 3.0) + peakNear (acfMeter, 2.0 * period / 3.0));
    result.compoundScore = (float) (triplet - straight);
    return result;
}
} // namespace kt
