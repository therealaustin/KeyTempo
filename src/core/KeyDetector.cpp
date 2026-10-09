#include "KeyDetector.h"
#include "MusicTheory.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#ifdef KT_KEY_DEBUG
#include <cstdio>
#endif

namespace kt
{
namespace
{
    constexpr double twoPi = 6.283185307179586476925286766559;

    // Key profiles from Ibrahim Sha'ath's KeyFinder, tuned for contemporary music
    // (they are close to Krumhansl–Kessler but less biased toward the dominant).
    constexpr double majorProfile[12] = { 6.6, 2.0, 3.5, 2.3, 4.6, 4.0, 2.5, 5.2, 2.4, 3.7, 2.3, 3.4 };
    constexpr double minorProfile[12] = { 6.5, 2.7, 3.5, 5.4, 2.6, 3.5, 2.5, 5.2, 4.0, 2.7, 4.3, 3.2 };

    double pearson (const double* a, const double* b)
    {
        double ma = 0, mb = 0;
        for (int i = 0; i < 12; ++i) { ma += a[i]; mb += b[i]; }
        ma /= 12.0; mb /= 12.0;
        double num = 0, da = 0, db = 0;
        for (int i = 0; i < 12; ++i)
        {
            num += (a[i] - ma) * (b[i] - mb);
            da += (a[i] - ma) * (a[i] - ma);
            db += (b[i] - mb) * (b[i] - mb);
        }
        return (da > 0 && db > 0) ? num / std::sqrt (da * db) : 0.0;
    }
} // namespace

void KeyDetector::prepare (double sr)
{
    sampleRate = sr;
    frameSize = Fft::nextPowerOfTwo ((int) std::lround (sr * 0.37));
    hop = frameSize / 4;
    fft = std::make_unique<Fft> (frameSize);

    minBin = std::max (2, (int) std::floor (63.0 * frameSize / sr));   // ~B1
    bassMinBin = std::max (2, (int) std::floor (40.0 * frameSize / sr));
    bassMaxBin = (int) std::ceil (200.0 * frameSize / sr);
    maxBin = std::min (frameSize / 2 - 2, (int) std::ceil (2100.0 * frameSize / sr)); // ~C7

    inputRing.assign ((size_t) frameSize, 0.0f);
    frame.assign ((size_t) frameSize, 0.0f);

    setMemorySeconds (memorySeconds);
    reset();
}

void KeyDetector::setMemorySeconds (double seconds)
{
    memorySeconds = seconds;
    const double hopSeconds = hop / sampleRate;
    decay = seconds > 0.0 ? std::exp (-hopSeconds / seconds) : 1.0;
}

void KeyDetector::reset()
{
    std::fill (inputRing.begin(), inputRing.end(), 0.0f);
    chromaAcc.fill (0.0);
    bassAcc.fill (0.0);
    tuningCos = tuningSin = energyAcc = 0.0;
    ringWrite = 0;
    samplesUntilFrame = frameSize;
}

void KeyDetector::push (const float* mono, int numSamples)
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

void KeyDetector::processFrame()
{
    double energy = 0.0;
    for (int i = 0; i < frameSize; ++i)
    {
        const float s = inputRing[(size_t) ((ringWrite + i) % frameSize)];
        frame[(size_t) i] = s;
        energy += (double) s * s;
    }

    // Decay old evidence even through silence, so the memory setting behaves as expected.
    for (auto& c : chromaAcc)
        c *= decay;
    for (auto& c : bassAcc)
        c *= decay;
    tuningCos *= decay;
    tuningSin *= decay;
    energyAcc *= decay;

    if (std::sqrt (energy / frameSize) < 1.0e-4)
        return;

    fft->magnitudeSpectrum (frame.data(), mags);

    float frameMax = 0.0f;
    for (int k = minBin; k <= maxBin; ++k)
        frameMax = std::max (frameMax, mags[(size_t) k]);
    if (frameMax <= 0.0f)
        return;

    const float floorLevel = frameMax * 0.01f; // ignore peaks 40 dB below the strongest
    const double tuningOffset = std::atan2 (tuningSin, tuningCos) / twoPi; // semitones, -0.5..0.5
    const double norm = 2.0 / frameSize;

    std::array<double, 12> frameChroma {}, frameBass {};

    for (int k = std::min (minBin, bassMinBin); k <= maxBin; ++k)
    {
        const float m = mags[(size_t) k];
        if (m < floorLevel || m <= mags[(size_t) k - 1] || m < mags[(size_t) k + 1])
            continue;

        // Parabolic interpolation on log magnitudes for a precise peak frequency.
        const double a = std::log (mags[(size_t) k - 1] + 1e-12);
        const double b = std::log (m + 1e-12);
        const double c = std::log (mags[(size_t) k + 1] + 1e-12);
        const double denom = a - 2.0 * b + c;
        const double p = std::abs (denom) > 1e-12 ? 0.5 * (a - c) / denom : 0.0;
        const double freq = (k + std::clamp (p, -0.5, 0.5)) * sampleRate / frameSize;

        const double midi = 69.0 + 12.0 * std::log2 (freq / 440.0);
        const double amp = std::sqrt (m * norm);

        // Tuning statistics use the raw deviation from equal temperament at A440.
        const double rawDev = midi - std::round (midi);
        tuningCos += amp * std::cos (twoPi * rawDev);
        tuningSin += amp * std::sin (twoPi * rawDev);

        const double corrected = midi - tuningOffset;
        const double nearest = std::round (corrected);
        const double dev = std::abs (corrected - nearest);
        const double w = std::max (0.0, 1.0 - dev / 0.4);
        if (w <= 0.0)
            continue;

        const int pc = (((int) nearest % 12) + 12) % 12;
        if (k <= bassMaxBin)
            frameBass[(size_t) pc] += w * amp;
        if (k >= minBin)
            frameChroma[(size_t) pc] += w * amp;
    }

    for (int i = 0; i < 12; ++i)
    {
        chromaAcc[(size_t) i] += frameChroma[(size_t) i];
        bassAcc[(size_t) i] += frameBass[(size_t) i];
    }
    energyAcc += std::accumulate (frameChroma.begin(), frameChroma.end(), 0.0);
}

KeyResult KeyDetector::analyse() const
{
    KeyResult r;
    const double total = std::accumulate (chromaAcc.begin(), chromaAcc.end(), 0.0);
    if (total <= 1.0e-6 || energyAcc <= 1.0e-6)
        return r;

    const double peak = *std::max_element (chromaAcc.begin(), chromaAcc.end());
    for (int i = 0; i < 12; ++i)
        r.chroma[(size_t) i] = (float) (chromaAcc[(size_t) i] / peak);

    const double bassTotal = std::accumulate (bassAcc.begin(), bassAcc.end(), 0.0);
    const bool hasBass = bassTotal > 0.05 * total;

#ifdef KT_KEY_DEBUG
    for (int i = 0; i < 12; ++i)
        std::fprintf (stderr, "%5.2f/%5.2f ", chromaAcc[(size_t) i] / total * 12, bassAcc[(size_t) i] / std::max (1e-9, bassTotal) * 12);
    std::fprintf (stderr, "\n");
#endif
    std::array<double, 24> scores {};
    for (int key = 0; key < 24; ++key)
    {
        const int t = theory::tonic (key);
        const double* profile = theory::isMinor (key) ? minorProfile : majorProfile;
        double rotated[12];
        for (int i = 0; i < 12; ++i)
            rotated[(i + t) % 12] = profile[i];
        scores[(size_t) key] = pearson (chromaAcc.data(), rotated);
        if (hasBass)
        {
            // Bass evidence: how much the bass sits on this key's tonic vs. the average note.
            const double tonicShare = bassAcc[(size_t) t] / bassTotal;
            scores[(size_t) key] += 0.6 * (tonicShare - 1.0 / 12.0);
        }
    }

    int best = 0;
    for (int k = 1; k < 24; ++k)
        if (scores[(size_t) k] > scores[(size_t) best])
            best = k;
    int second = best == 0 ? 1 : 0;
    for (int k = 0; k < 24; ++k)
        if (k != best && scores[(size_t) k] > scores[(size_t) second])
            second = k;

    const double s1 = scores[(size_t) best], s2 = scores[(size_t) second];
    r.key = best;
    r.runnerUp = second;
    r.confidence = (float) (std::clamp (s1, 0.0, 1.0) * std::clamp ((s1 - s2) / 0.25, 0.0, 1.0));
    r.tuningCents = (float) (100.0 * std::atan2 (tuningSin, tuningCos) / twoPi);
    r.valid = true;
    return r;
}
} // namespace kt
