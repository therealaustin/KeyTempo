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

    inputRing.assign ((size_t) frameSize, 0.0f);
    frame.assign ((size_t) frameSize, 0.0f);
    prevLog.assign ((size_t) frameSize / 2 + 1, 0.0f);
    prevLow.assign ((size_t) frameSize / 2 + 1, 0.0f);
    prevAccent.assign ((size_t) frameSize / 2 + 1, 0.0f);

    const auto envCapacity = (size_t) std::ceil (windowSeconds * envRate);
    env.assign (envCapacity, 0.0f);
    lowEnv.assign (envCapacity, 0.0f);
    accentEnv.assign (envCapacity, 0.0f);
    active.assign (envCapacity, 0);

    reset();
}

void TempoDetector::reset()
{
    std::fill (inputRing.begin(), inputRing.end(), 0.0f);
    std::fill (prevLog.begin(), prevLog.end(), 0.0f);
    std::fill (prevLow.begin(), prevLow.end(), 0.0f);
    std::fill (prevAccent.begin(), prevAccent.end(), 0.0f);
    std::fill (env.begin(), env.end(), 0.0f);
    std::fill (lowEnv.begin(), lowEnv.end(), 0.0f);
    std::fill (accentEnv.begin(), accentEnv.end(), 0.0f);
    std::fill (active.begin(), active.end(), (uint8_t) 0);
    ringWrite = 0;
    samplesUntilFrame = frameSize; // wait for a full first window
    havePrev = false;
    envWrite = envCount = 0;
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
    havePrev = true;

    if (! hasSignal)
        flux = lowFlux = accentFlux = 0.0f;

    env[(size_t) envWrite] = flux;
    lowEnv[(size_t) envWrite] = lowFlux;
    accentEnv[(size_t) envWrite] = accentFlux;
    active[(size_t) envWrite] = hasSignal ? 1 : 0;
    envWrite = (envWrite + 1) % (int) env.size();
    envCount = std::min (envCount + 1, (int) env.size());
}

TempoResult TempoDetector::analyse (double minBpm, double maxBpm) const
{
    TempoResult result;
    const int n = envCount;
    const int capacity = (int) env.size();

    // Need at least ~4 seconds of audio and enough of it to be non-silent.
    if (n < (int) (envRate * 4.0))
        return result;

    int activeFrames = 0;
    std::vector<float> e ((size_t) n), el ((size_t) n), ea ((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        const int idx = (envWrite - n + i + capacity) % capacity;
        e[(size_t) i] = env[(size_t) idx];
        el[(size_t) i] = lowEnv[(size_t) idx];
        ea[(size_t) i] = accentEnv[(size_t) idx];
        activeFrames += active[(size_t) idx];
    }
    if (activeFrames < (int) (envRate * 3.0))
        return result;

    // Lags up to 4 beat periods of the slowest tempo (also covers 3-beat bars).
    const double slowestPeriod = 60.0 * envRate / minBpm;
    const int maxLag = std::min (n - 1, (int) std::ceil (slowestPeriod * 4.0) + 4);

    const auto cond = conditionEnvelope (e, envRate);
    const auto acf = autocorrelation (cond, maxLag);
    if (acf.empty())
        return result;

    const double centre = std::sqrt (minBpm * maxBpm);
    auto prior = [&] (double bpm)
    {
        const double octaves = std::log2 (bpm / centre);
        return std::exp (-0.5 * octaves * octaves / (0.9 * 0.9));
    };

    // Periodicity score: autocorrelation at 1-4 beat periods plus the beat's subdivision.
    auto score = [&] (double bpm) -> double
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
        return std::max (0.0, s / wsum) * prior (bpm);
    };

    // Beat salience: with the best phase, how strong are the onsets that land on the
    // beats? This separates true beats from 3:2 / 2:3 groupings that are equally
    // periodic but put the beat on weak hi-hats instead of kicks and snares.
    // Salience uses a less-compressed envelope so loud hits (kick, snare) stand
    // clearly above quiet ones (hats, ghost notes).
    const auto accent = conditionEnvelope (ea, envRate);
    double condSd = 0.0;
    for (auto v : accent)
        condSd += (double) v * v;
    condSd = std::sqrt (condSd / n) + 1.0e-12;

    auto salience = [&] (double bpm) -> double
    {
        const double period = 60.0 * envRate / bpm;
        auto onsetAt = [&] (double pos)
        {
            const int i = (int) std::lround (pos);
            float v = accent[(size_t) i];
            if (i > 0) v = std::max (v, accent[(size_t) i - 1]);
            if (i + 1 < n) v = std::max (v, accent[(size_t) i + 1]);
            return v;
        };

        // Best phase = the one whose beats carry the most onset energy.
        double bestPhase = 0.0, bestMean = -1.0e9;
        for (double phase = 0.0; phase < period; phase += 1.0)
        {
            double sum = 0.0;
            int count = 0;
            for (double pos = phase; pos < n - 1; pos += period, ++count)
                sum += onsetAt (pos);
            if (count > 0 && sum / count > bestMean)
            {
                bestMean = sum / count;
                bestPhase = phase;
            }
        }

        // Then judge the *weak* beats (25th percentile): a mis-grouped tempo puts some
        // of its beats on hi-hats or empty space, a real tempo doesn't.
        std::vector<float> beats;
        for (double pos = bestPhase; pos < n - 1; pos += period)
            beats.push_back (onsetAt (pos));
        if (beats.empty())
            return 0.0;
        std::sort (beats.begin(), beats.end());
        return beats[beats.size() / 4] / condSd;
    };

    // Coarse grid over the whole range.
    const double step = 0.1;
    std::vector<double> grid, scores;
    double best = -1.0;
    for (double bpm = minBpm; bpm <= maxBpm + 1e-9; bpm += step)
    {
        grid.push_back (bpm);
        scores.push_back (score (bpm));
        best = std::max (best, scores.back());
    }
    if (best <= 0.0)
        return result;

    // Shortlist the strongest local peaks, refine each, and re-rank with beat salience.
    std::vector<std::pair<double, double>> peaks; // (score, bpm)
    for (size_t i = 0; i < scores.size(); ++i)
    {
        const bool left = i == 0 || scores[i] >= scores[i - 1];
        const bool right = i + 1 == scores.size() || scores[i] >= scores[i + 1];
        if (left && right && scores[i] >= 0.4 * best)
            peaks.emplace_back (scores[i], grid[i]);
    }
    std::sort (peaks.begin(), peaks.end(), [] (auto& a, auto& b) { return a.first > b.first; });
    if (peaks.size() > 8)
        peaks.resize (8);

    struct Candidate { double bpm, score, salience; };
    std::vector<Candidate> candidates;
    double maxSalience = 1.0e-9;
    for (auto& [s0, bpm0] : peaks)
    {
        Candidate c { bpm0, s0, 0.0 };
        for (double b = bpm0 - step; b <= bpm0 + step; b += 0.01)
        {
            const double v = score (b);
            if (v > c.score)
            {
                c.score = v;
                c.bpm = b;
            }
        }
        c.salience = salience (c.bpm);
        maxSalience = std::max (maxSalience, c.salience);
        candidates.push_back (c);
    }

    double bestBpm = candidates.front().bpm, bestFinal = -1.0, bestScore = 0.0;
    for (auto& c : candidates)
    {
        // Only penalise candidates whose weak beats are clearly weaker than the best.
        const double factor = std::clamp (c.salience / (0.6 * maxSalience), 0.0, 1.0);
        const double finalScore = c.score * factor;
#ifdef KT_TEMPO_DEBUG
        std::fprintf (stderr, "    cand %7.2f  score %.3f  prior %.2f  sal %.2f  final %.3f\n", c.bpm, c.score, prior (c.bpm), c.salience, finalScore);
#endif
        if (finalScore > bestFinal)
        {
            bestFinal = finalScore;
            bestBpm = c.bpm;
            bestScore = c.score;
        }
    }
    best = bestScore;

    // Fine tempo: phase-lock a pulse train to the onsets across the whole window.
    // Over ~25 beats even a 0.05 BPM error smears the alignment, so this gets far
    // tighter than the autocorrelation peak alone.
    {
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

        const double coarse = bestBpm;
        double bestAlign = alignment (coarse);
        for (double bpm = coarse - 0.5; bpm <= coarse + 0.5; bpm += 0.02)
        {
            const double a = alignment (bpm);
            if (a > bestAlign)
            {
                bestAlign = a;
                bestBpm = bpm;
            }
        }
    }

    // Confidence: how far the winning peak stands above the rest of the curve.
    const double avg = std::accumulate (scores.begin(), scores.end(), 0.0) / (double) scores.size();
    double var = 0.0;
    for (auto s : scores)
        var += (s - avg) * (s - avg);
    const double sd = std::sqrt (var / (double) scores.size()) + 1.0e-9;
    const double z = (best - avg) / sd;

    result.bpm = bestBpm;
    result.confidence = (float) std::clamp ((z - 1.5) / 3.0, 0.0, 1.0) * (float) std::clamp (best / 0.25, 0.0, 1.0);
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
