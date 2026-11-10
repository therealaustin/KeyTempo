#pragma once

#include "Fft.h"

#include <array>
#include <memory>
#include <vector>

namespace kt
{
struct KeyResult
{
    int key = -1;            // 0..23, see MusicTheory.h
    int runnerUp = -1;       // second-best key
    float confidence = 0.0f; // 0..1
    float tuningCents = 0.0f;
    std::array<float, 12> chroma {}; // normalised 0..1, C = 0
    bool valid = false;
};

/** Key estimation:
      1. Long STFT (~370 ms) with spectral peak picking and parabolic interpolation.
      2. Peaks between ~65 Hz and ~2 kHz are folded into a 12-bin chromagram,
         corrected for the estimated tuning (so 432 Hz material still works).
      3. The time-averaged chroma is correlated against 24 rotated key profiles.
      4. A separate bass chroma (40-200 Hz) adds weight to keys whose tonic the bass
         line keeps returning to, which helps separate relative major/minor pairs. */
/** The detector's accumulated evidence. Without memory decay (setMemorySeconds (0)) every
    field is a plain running sum, so the difference of two snapshots is the evidence of the
    audio between them (TrackAnalyzer uses that to find key changes). */
struct KeyEvidence
{
    std::array<double, 12> chroma {}, bass {};
    double energy = 0.0, tuningCos = 0.0, tuningSin = 0.0;

    KeyEvidence operator- (const KeyEvidence& o) const noexcept
    {
        KeyEvidence d;
        for (size_t i = 0; i < 12; ++i)
        {
            d.chroma[i] = chroma[i] - o.chroma[i];
            d.bass[i] = bass[i] - o.bass[i];
        }
        d.energy = energy - o.energy;
        d.tuningCos = tuningCos - o.tuningCos;
        d.tuningSin = tuningSin - o.tuningSin;
        return d;
    }
};

class KeyDetector
{
public:
    void prepare (double sampleRate);
    void reset();

    /** How far back the key estimate "remembers". <= 0 means the whole session. */
    void setMemorySeconds (double seconds);

    void push (const float* mono, int numSamples);

    KeyResult analyse() const { return keyFor (evidence()); }

    KeyEvidence evidence() const;
    /** The key the given evidence points to (what analyse() does with the detector's own). */
    static KeyResult keyFor (const KeyEvidence& e);
    /** Profile score of each of the 24 keys for the given evidence (higher = more likely). */
    static std::array<double, 24> keyScores (const KeyEvidence& e);

private:
    void processFrame();

    double sampleRate = 44100.0;
    int frameSize = 16384, hop = 4096, minBin = 0, maxBin = 0;
    double decay = 1.0, memorySeconds = 45.0;
    std::unique_ptr<Fft> fft;

    std::vector<float> inputRing, frame, mags;
    int ringWrite = 0, samplesUntilFrame = 0;

    std::array<double, 12> chromaAcc {}, bassAcc {};
    int bassMinBin = 0, bassMaxBin = 0;
    double tuningCos = 0.0, tuningSin = 0.0, energyAcc = 0.0;
};
} // namespace kt
