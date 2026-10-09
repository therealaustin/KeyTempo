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
class KeyDetector
{
public:
    void prepare (double sampleRate);
    void reset();

    /** How far back the key estimate "remembers". <= 0 means the whole session. */
    void setMemorySeconds (double seconds);

    void push (const float* mono, int numSamples);

    KeyResult analyse() const;

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
